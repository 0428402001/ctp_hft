# ctp_hft — CTP 期货多进程高频交易框架（SimNow 7×24 常驻）

面向 Windows 的工程级框架：**行情 / 策略 / 交易 / 查询** 四个独立进程，进程间全部通过
**共享内存 + 无锁循环队列** 通信，策略侧内置一个 **L1 盘口失衡反转策略（obi_voi + 括号单退出）**，
外带一套 **7×24 守护体系**（时间自动切前置、进程看活、限流保护、账户审计），
可以编译即跑。

支持三种柜台风味：仿真（无需账号）、SimNow（标准 CTP 6.7.7，第一套 + 7×24 两套前置）、
CTPII Mini 极速柜台。

---

## 0. 快速开始

```bat
:: 编译（本机用免安装 MinGW，见 4.2）
build_mingw.bat

:: 7×24 运行：交给守护体系，窗口关掉也不影响
start_simnow724.bat

:: 看实时日志（等价 tail -f）
watch_log.bat            :: 策略：信号/成交/PnL
watch_log.bat all        :: 策略 + 交易
watch_log.bat errors     :: 只看 WRN/ERR

:: 状态 / 账户 / 真实盈亏
python tools\guard.py --status
python tools\acct_log.py --print
python tools\pnl_fifo.py
python tools\rounds724.py --hours 2
```

四个进程都是 `--no-console` 无声运行，**日志是唯一观察窗口**；不要再靠控制台。

## 1. 架构

```
                        ┌──────────────── 共享内存（Windows FileMapping）─────────────────┐
                        │                                                                │
   CTP 行情前置 ──►  md_proc  ──[ticks 无锁环形队列]──►  strategy_proc                    │
   (或仿真行情)          │                                   │  │                        │
                        │                    [order commands] │  │ [exec reports]         │
                        │                                   ▼  ▲                        │
                        │                              trader_proc ──► CTP 交易前置       │
                        │                                   │        (或仿真撮合)        │
                        │                       [exec 镜像] │                            │
                        │                                   ▼                            │
   [market snapshot] ◄──┘                             query_proc ◄──► CTP 查询会话        │
   [account/position snapshot (seqlock)]  ◄───────────────┘                             │
                        └────────────────────────────────────────────────────────────────┘
```

| 进程 | 职责 |
|---|---|
| `md_proc` | 连接 CTP MdApi，订阅合约，把每个 tick 写入共享环形队列；同时维护共享行情快照（最新价/买卖一档） |
| `strategy_proc` | 消费 tick，运行策略，发出报单/撤单指令，接收成交回报，维护策略侧持仓 |
| `trader_proc` | 消费指令，调用 `ReqOrderInsert` / `ReqOrderAction`，把报单回报与成交回报写回环形队列，并镜像一份给查询进程 |
| `query_proc` | 独立 CTP 会话，周期性查询资金/持仓/合约，用 seqlock 发布到共享快照；响应策略的主动查询 |
| `monitor` | 只读观察者：进程心跳、队列深度、账户、持仓、最新价 |

**为什么拆成四个进程**：查询（资金/持仓）通常是秒级的慢请求，交易是微秒级的关键路径，
把两者放进同一进程会让一次慢查询推迟一笔报单。拆开之后，慢的一方只会让自己的队列变长，
不会阻塞别人。

## 2. 无锁循环队列

`include/hft/ring_queue.h` 是 SPSC（单生产者单消费者）环形队列：

* 容量必须是 2 的幂，用 `index & (capacity-1)` 取槽位；
* 读写索引是单调递增的 64 位计数，空/满无需额外腾出一个槽位；
* 生产者 `release` 发布槽位，消费者 `acquire` 读取索引 —— 无 CAS、无锁、无 ABA；
* 读写索引各自独占一个 cache line，避免 false sharing；
* 队列**满**时生产者丢弃并计数（不做阻塞等待，保证关键路径永不背压到行情线程）；
* 队列整体放在共享内存里，因此只能是 POD，不能有虚表或堆指针。

### 2.1 事件门铃 + 混合等待（延迟与 CPU 兼得）

每条队列通道额外挂一个**内核事件对象**（`ChannelEvent`，Windows auto-reset Event）：
生产者发布消息后争用共享通知标志，消费者准备休眠前解除标志并复查队列；突发消息只触发一次
`SetEvent`（忙时消费者零 syscall），且不会在“检查为空”和“进入等待”之间丢失唤醒。消费者空闲时
`WaitForMultipleObjects` 阻塞在入站事件上——新消息**微秒级唤醒**且空闲 CPU ≈ 0%。
主循环空闲路径为两阶段：先自旋 `spin_us`（吃突发序列），再阻塞等事件（超时上限
由每个进程的 `maxWaitNs()` 决定，兜底心跳/定时器任务）。`spin_us` 默认 **0**
——纯事件驱动，实测 sim 行情持续推 tick 时四个进程 CPU 合计 ≈ 0.1%。

```ini
[general]
spin_us      = 0      ; 空闲先自旋的时长（µs），默认 0 = 纯事件驱动
high_priority = true  ; 四进程 HIGH_PRIORITY_CLASS
; [md] cpu = 0  [trader] cpu = 3  ...  ; 可选：绑主循环线程到指定核心
; [md] ctp_cpu = 1  [trader] ctp_cpu = 3  ; 可选：绑 CTP 回调线程（见 2.2）
```

### 2.2 性能工程

1. **事件驱动**：见上。等待上限按角色区分——策略/查询 1 s、trader live 5 s / sim 2 ms、
   md sim 精确睡到下一个 tick（`msUntilNextTick()`）。
2. **CPU 亲和**：主循环线程按 `[role] cpu` 钉核；CTP SDK 自建回调线程不属于我们，
   用 `thread_pin.h` 的 `pinCurrentThreadOnce` 在**首次回调内**自钉到 `ctp_cpu`。
   4 核示例：md 主 + query 主 = 0、md CTP = 1、strategy = 2、trader 主 + CTP = 3。
3. **热路径零分配**：策略上下文的 4 个 `std::map<std::string,…>` 换成 16 槽固定数组
   + strncmp 线性扫描；`SnapshotReader` 用预分配 `PositionSnap[512]` 替代 vector；
   合约表改惰性缓存直读 SHM（不再每秒拷贝 4096 行）。tick 回调路径上没有 new。
4. **全限价单 + 括号价预计算**：所有订单限价（IOC 也是限价 IOC）；`armBracket()` 在
   入场成交时一次算好 `tpTrigger / slTrigger / tpPrice`，tick 回调只剩纯数值比较，
   语义与原乘除比较逐位等价。
5. **只留短历史**：策略三处 deque 换固定环形缓存——OBI 窗口 `obiRing[32]`（2 幂掩码）、
   30 s 中价漂移窗口改 32 槽**秒级检查点环**（每个检查点原地刷新，内存降一个数量级，
   窗口左边界误差 ±1 s 可接受）、平仓腿 `flattenLegs[8]`。
6. **页局部性**：消息体全部填充到 4 KiB 因子（TickMsg / OrderCommand 256 B、
   ExecReport / QueryResponse 512 B、QueryRequest 64 B，`static_assert` 锁死防回退）；
   环形队列槽数组 `alignas(4096)` 页对齐；SHM 映射后 `VirtualLock` 锁页防换出
   （先 `SetProcessWorkingSetSize` 抬配额，best-effort，失败只打 stderr）。
7. **状态一致性与故障闸门**：行情快照使用原子字段和版本校验；tick/成交回报队列溢出会锁住新开仓，
   并由 monitor 显示数据完整性故障，要求冷启动对账后恢复。
8. **低开销观测**：默认关闭逐 tick Debug 日志；设置 `[general] debug_logging=true` 才记录研究用 `tickdat`。
   策略日志按窗口报告 tick→策略延迟平均值、P50、P99 和最大值。
9. **共享内存 ABI v2**：本次调整了队列、控制块和行情快照布局。更新后必须停止全部旧进程，执行
   `shm_reset.exe` 并用同一版本二进制冷启动；进程会拒绝不匹配的共享内存版本。

README 历史基准（sim 100 ms 行情、四进程全开）记录 tick→策略平均处理约 33–50 µs、队列回压 0。
这只是仿真环境的阶段耗时，不代表实盘端到端或尾部延迟；当前策略日志另输出窗口 P50/P99/最大值。

> Windows 陷阱：`sleep_for(<1ms)` 会退化成 `Sleep(0)`——只让出时间片不阻塞，
> 等于全速自旋烧满一个核。本框架空闲等待一律走内核事件或整毫秒睡眠
> （并调用 `timeBeginPeriod(1)` 提高睡眠精度）。

## 3. 目录结构

```
ctp_hft/
├── CMakeLists.txt          主构建脚本（MSVC 完整版 / MinGW 双风味）
├── build.bat               MSVC 一键编译（自动定位 VS + vcvarsall）
├── build_mingw.bat         MinGW 一键编译（mini + std 双风味全量构建）
├── build_strategy.bat      只重编策略进程（改策略后最快上线路径）
├── start_sim.bat           四进程仿真（无需账号）
├── verify_sim.bat          离线链路自测（sim 模式，5 秒内出回合后自动退出）
├── start_simnow.bat        SimNow 白天启动器（guard.py --env day 的包装，见第 6 节）
├── start_simnow724.bat     SimNow 7×24 启动器（guard.py --env 724 的包装）
├── start_simnow_raw.bat    旧的直启逻辑（仅存档，运行中勿碰：会 shm_reset 清总线）
├── start_simnow724_raw.bat 同上，724 版本
├── watch_log.bat           实时跟日志（strategy/trader/md/query/all/errors）
├── monitor.bat             实时状态面板
├── stop_all.bat            彻底停止（挂 flow\.halt，守护不再自启；--daemon 连守护也停）
├── config/
│   ├── config.ini          离线仿真（mode=sim，无柜台无网络，自测用）
│   ├── config_simnow.ini   SimNow 第一套环境（30001/30011）
│   └── config_simnow724.ini   SimNow 7×24 环境（40001/40011）
├── tools/                  运维与诊断（Python / PowerShell，不进主构建）
│   ├── guard.py            环境判定 + 看活 + 限流闸门（交互/自动化入口）
│   ├── guard_daemon.py     常驻守护（计划任务 ctp_hft_daemon 拉起的唯一进程）
│   ├── launch_env.ps1      冷启动脚本（杀进程 → 清总线 → md/trader/query/strategy）
│   ├── account_peek.cpp    只读共享内存账户快照（不登录、不下单）
│   ├── acct_log.py         把快照追加到 log/account.csv
│   ├── pnl_fifo.py         从 TRADE 明细 FIFO 重算真实盈亏
│   ├── rounds724.py        跨零点的回合统计（--hours N）
│   ├── bt_bracket.py       括号单参数回测
│   ├── sim_round.py        仿真回合统计
│   ├── diag_1105_pm.py     2026-11-05 下午事故的日志诊断脚本
│   └── fetch_toolchain.py  下载免安装 MinGW（本机无 VS 时用）
├── include/hft/
│   ├── platform.h          平台宏、pause/yield
│   ├── ring_queue.h        无锁 SPSC 循环队列
│   ├── shm.h               共享内存封装（Windows / POSIX 双实现）
│   ├── protocol.h          跨进程消息定义（Tick/指令/回报/快照）
│   ├── channel.h           通道布局、seqlock 快照读写器
│   ├── config.h/.cpp       INI 配置解析
│   ├── logger.h/.cpp       分进程日志（fmtlog 后端，见 6.6）
│   ├── time_util.h         单调时钟 / 墙钟
│   ├── process_app.h       四个进程共用的启动骨架（参数、心跳、主循环）
│   ├── strategy_base.h     策略接口
│   └── sim_market.h/.cpp   仿真行情发生器（均值回归随机游走）
├── src/
│   ├── common/             共享内存、配置、日志、进程骨架、CTP DLL 运行时加载
│   ├── sim/                仿真行情发生器 + 仿真撮合器
│   ├── md/                 行情进程 + CTP MdApi 适配
│   ├── strategy/           策略进程 + obi_voi 策略
│   ├── trader/             交易进程 + CTP TraderApi 适配
│   ├── query/              查询进程 + CTP 查询适配
│   └── tools/              monitor（共享内存观测器）、shm_reset（冷启动清总线）
├── sdk/stdctp/             标准 CTP 6.7.7 SDK（include/lib/dll）
├── third_party/fmtlog/     fmtlog 日志库（MIT，含 fmt 头文件，commit 固定）
├── flow/                   CTP 流文件（按环境分目录：md724/trader724/query724…）
└── log/                    运行日志 + 审计数据（每个进程一个文件）
```

## 4. 编译

### 4.1 完整版（需要 Visual Studio 2017+）

```
build.bat
```

脚本会用 `vswhere` 找 VS、调用 `vcvarsall x64`，用 `cl.exe` 编译四个进程和 monitor，
链接 `../win64/mdapi/thostmduserapi.lib`、`../win64/traderapi/thosttraderapi.lib`，
并把两个 DLL 拷到 `build\bin`。也可以用 CMake：

```
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

### 4.2 MinGW 版（推荐，本机在用的就是这个）

两个工具链产出的二进制**功能完全一致**（sim 与 live 都可用）。CTP SDK 的导入库虽然是
MSVC 专用格式，但本框架在运行时用 `LoadLibrary` 直接加载 `thosttraderapi.dll` /
`thostmduserapi.dll` 并解析导出的工厂函数（CTP 接口类是纯虚 + POD 设计，跨编译器 ABI 安全），
所以 MinGW 无需链接任何 `.lib` 也能跑实盘。

```
build_mingw.bat                          :: 自动找 g++（含 .toolchain\mgw 免安装版）
build_mingw.bat C:\mingw64\bin\g++.exe   :: 或手动指定
```

改了策略代码后不需要全量重编：

```
build_strategy.bat                       :: 只编 strategy_proc.exe（自带 halt 窗口，见下）
```

> **编译窗口为什么要 halt**：守护进程每 20 s 巡检一次，发现 strategy 缺失就会立刻用
> **旧 exe** 拉起它；此时链接器无法覆盖正在运行的镜像，`copy` 静默失败，于是"新版本"
> 其实还在跑旧代码。2026-11-06 11:31 就踩了这个坑（kill 之后守护抢跑了）。
> 现在 `build_strategy.bat` 会先写 `flow\.halt`、**等满一个巡检周期（25 s）**再杀进程，
> 编完（或失败）自动删除；`guard_daemon.py` 也在动手前二次检查 halt。
> 编完重启策略：`python tools\guard.py --strategy-only`（不重启 CTP 会话），
> 或者等守护进程下一拍自动补起。

> 注意：改了 `src/sim/sim_market.cpp` 时 `md_proc` 也链接了它，需要单独重编 md_proc；
> 其余三个进程与策略代码无关。

只读账户工具单独编（一行即可，产物在 `build\bin_simnow`）：

```
g++ -std=c++17 -O2 -Iinclude -Isrc -static -o build\bin_simnow\account_peek.exe ^
    tools\account_peek.cpp src\common\shm.cpp src\common\logger.cpp
```

### 4.3 CMake + MinGW

```
cmake -B build-cmake -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build-cmake
```

### 4.4 运行时依赖（MinGW 构建）

MinGW 默认动态链接自己的运行时（`libgcc_s_seh-1.dll` / `libstdc++-6.dll` /
`libwinpthread-1.dll`），普通 Windows 上没有这些文件就会报“找不到 libgcc_s_seh-1.dll”。
工程已在两条构建路径都加了 `-static -static-libgcc -static-libstdc++`
（`build_mingw.bat` 的 `LDFLAGS`、`CMakeLists.txt` 的 `HFT_STATIC_RUNTIME`），
产物只依赖 `KERNEL32.dll` 和系统 UCRT。自检：

```
objdump -p build\bin\md_proc.exe | findstr "DLL Name"
:: 只应看到 KERNEL32.dll 和 api-ms-win-crt-*.dll
```

## 5. 配置

所有进程读同一个 ini；`--config <path>` 可覆盖。除 `[account]` 外，SimNow 两套环境的
策略参数保持**逐字一致**，保证切换前置时交易逻辑不变。

> 仓库里只提交脱敏后的 `config/*.ini.example`（账号密码是占位符）。首次使用先复制：
> `copy config\config_simnow724.ini.example config\config_simnow724.ini`，再填入自己的
> `user_id / password / investor_id`。真实的 `config/config*.ini` 已被 `.gitignore` 排除，
> 不会误提交。

现成的三套配置：

| 文件 | 前置 | 用途 |
|---|---|---|
| `config.ini` | 无（`mode = sim`） | 离线仿真自测，`verify_sim.bat` 用 |
| `config_simnow.ini` | 30001/30011（交易时段） | SimNow 第一套环境 |
| `config_simnow724.ini` | 40001/40011（16:00-09:00） | SimNow 7×24 环境 |

**三套配置都不限制交易时段**：`trade_sessions` 留空表示全天可开仓（2026-11-11 起）。
需要限制时写 `21:00-02:30` 或 `21:00-02:30,09:00-10:15`，只挡开仓，平仓永不受限。

想验证"这套东西到底能不能下单"，不用另建配置：`verify_sim.bat` 用 `config.ini`
在纯仿真下 5 秒内跑出 `SIGNAL -> ORDER -> TRADE -> ROUND TRIP`，不需要柜台和网络。

```ini
[account]
broker_id  = 9999
user_id    = YOUR_USER_ID
password   = YOUR_PASSWORD
investor_id = YOUR_INVESTOR_ID
app_id     = simnow_client_test      ; SimNow 固定
auth_code  = 0000000000000000        ; SimNow 固定

[md]      front = tcp://182.254.243.31:40011   ; 行情前置
[trader]  front = tcp://182.254.243.31:40001   ; 交易前置
[query]   front = tcp://182.254.243.31:40001   ; 查询走交易前置
[query]   interval_ms = 3000

[general]
mode        = live          ; sim = 仿真，live = 接真实前置
instruments = ag2612        ; 逗号分隔
startup_retry       = true  ; 登录失败不退出，按 startup_retry_ms 重试
startup_retry_ms    = 10000
clean_flow_on_start = true  ; 清掉上一交易日的会话流文件
heartbeat_ms = 500
spin_us      = 0              ; 0 = 纯事件驱动（推荐）；>0 时空闲先自旋 N µs
high_priority = true

; CPU 亲和（可选）：主循环线程 cpu，CTP 回调线程 ctp_cpu（线程归 SDK 创建，首回调自钉）
; [md] cpu = 0  ctp_cpu = 1
; [strategy] cpu = 2
; [trader] cpu = 3  ctp_cpu = 3
; [query] cpu = 0

[strategy]
name = obi_voi

[obi_voi]
; ---- 时段闸门 ----
trade_sessions      =              ; 留空 = 全天可开仓（当前默认）
                                   ; 要限制就写 21:00-02:30 或 21:00-02:30,09:00-10:15，
                                   ; 只挡开仓，平仓永不受限

; ---- 信号 ----
window_ticks        = 10    ; OBI 极端判定回看窗口（tick，上限 32 = obiRing 容量）
obi_threshold       = 0.90  ; |OBI| 超过它算极端
voi_ema_ticks       = 5     ; VOI 平滑
voi_flip_threshold  = 0.35  ; VOI“明确反向”的幅度
voi_entry_threshold = 0.1   ; 入场要求的最小 |VOI|
cooldown_ticks      = 12    ; 回合结束后的冷却（20 -> 12：质量由过滤器负责，不再靠冷却）

; ---- 入场 ----
entry_timeout_ticks  = 4    ; 挂单 N tick 未成交 -> 撤
entry_slippage_ticks = 1    ; 主动入场时向对手价多跨 N tick（成交在对手价，跨价几乎免费）
entry_taker          = 1    ; 1 = 跨对手价 + IOC（前置会秒撤挂单，必须 taker）
max_spread_ticks     = 1    ; 价差超过 N tick 就不入场（目标价从成交价起算，价差直接吃边际）

; ---- 括号单退出 ----
profit_ticks      = 16      ; 止盈限价挂在 entry ± 16 tick，GFD，永不主动撤
stop_loss_ticks   = 8       ; 价格逆向触碰 8 tick -> 撤 TP + IOC 止损
stop_slippage_ticks = 2     ; 止损 IOC 再多跨 2 tick（限价单仍按对手价成交，只保证够到有量的档）
close_timeout_ticks = 6     ; 平仓被动单的 VOI 反向超时
tp_repost_interval_ticks = 20   ; TP 被外部撤掉后，最快每 20 tick 补挂一次
tp_repost_max     = 8       ; 每回合最多补挂 8 次，超过转为纯价格触发 IOC

; ---- 入场质量（2026-11-10 复盘后新增，11-10 晚二次测量修正）----
entry_stall_ticks  = 0      ; 默认关。要求价格已从近 stall_window_ticks 个 tick 的
                            ;   极值回撤 N tick 才允许入场。二次测量（spread<=1 的
                            ;   真实交易域）：stall 0 -> 68 笔 +4.13 / 2 -> 66 +3.83
                            ;   / 4 -> 41 +3.95 / 6 -> 17 +0.50 / 8 -> 9 -0.17。
                            ;   最好的信号恰好是脉冲仍在延展的那些，等回撤反而错杀
stall_window_ticks = 6      ; 上述极值的回看窗口
breakeven_ticks    = 8      ; 浮盈达到 N tick 后止损上移到开仓价（只朝有利方向）
entry_blackout_sec = 1800  ; 窗口结束前 30 分钟不再开新仓（15:00 收盘则 14:30 截止）

; ---- 风控 ----
consec_stop_pause = 3       ; 连续 N 笔亏损回合 -> 停手 pause_seconds
pause_seconds     = 300     ; 停手时长（趋势日保护）
daily_loss_limit_ticks = 35 ; 当日（交易所交易日）累计亏损达到 N tick/手后不再开新仓，
                            ;   持仓仍按括号单正常退出；换交易日自动重置。
                            ;   按成交价差计，不含手续费；ag 每 tick 约 15 元/手，35 tick 约 525 元。0 = 关
; 历史样本参数差异只产生告警，不阻止仿真启动。

; ---- 兜底 ----
max_hold_ticks    = 0       ; 0 = 关（正常退出由括号单负责）
max_hold_seconds  = 600     ; 硬墙钟：持仓超 10 分钟无条件 IOC 逃逸（任何价格）
escape_retry_ms   = 800     ; 逃逸 IOC 重发间隔；每 2 次跨价 +1 tick（最多 +6）
                            ; offset 环形走 3(平今) -> 4(平昨) -> 1(平仓)

; ---- 库存 ----
trade_volume      = 1
max_position      = 1
allow_short       = true
flatten_on_start  = true    ; 启动时清柜台遗留仓
close_on_stop     = true
```

命令行可覆盖：`--config <path>`、`--mode sim|live`、`--log-dir <dir>`、`--no-console`。

## 6. 运行与 7×24 守护

### 6.1 为什么要守护

SimNow 给同一个账号提供**两组互斥前置**，7×24 不是“选一个不换”，而是**在 09:00 / 16:00
两个边界切换**，并且每次切换都要连共享内存总线一起重建（总线按名字共享，两种环境不能同时活）：

| 环境 | 交易/行情前置 | 服务时段 |
|---|---|---|
| `day`（第一套，`config_simnow.ini`） | `182.254.243.31:30001` / `30011` | 09:00 – 16:00 |
| `724`（`config_simnow724.ini`） | `182.254.243.31:40001` / `40011` | 交易日 16:00 → 次日 09:00；非交易日 16:00 → 12:00 |

而且 SimNow 对**频繁重连**有硬惩罚：`reason=4097` 直接踢线，账号会被封几分钟到十几分钟，
越急着重连封得越久。所以“进程挂了就立刻重启”是错的，必须有冷却与闸门。

### 6.2 三层结构

```
计划任务 ctp_hft_daemon  (pythonw.exe，开机/登录自启，单实例锁 127.0.0.1:49517)
        └── guard_daemon.py   常驻，20 s 一次巡检，唯一持有四个子进程
                ├── md_proc.exe / trader_proc.exe / query_proc.exe / strategy_proc.exe
                └── launch_env.ps1  冷启动：杀旧进程 → shm_reset → 按序拉起

guard.py   交互/自动化入口，只做判定与写请求，**从不自己 spawn**
           └── 写 flow\.restart_request → 守护进程在下一个 tick（≤20 s）执行
```

**为什么必须这样分层**：Windows 会把“计划任务实例”的整棵进程树在实例回收时一起杀掉，
所以让任务每次拉起四个进程，必然出现“几个进程几分钟后无声消失”。现在任务只拉起一个
长期存活的守护进程，四进程的生命周期绑在守护进程上，不再绑在会来会走的任务实例上。
同理，守护进程必须用 `pythonw.exe` —— `python.exe`（console 子系统）会收到
`CTRL_CLOSE_EVENT`（0xC000013A）被连带杀死。

### 6.3 日常命令

```bat
start_simnow.bat                     :: 白天第一套环境（guard.py --env day）
start_simnow.bat --status            :: 只看状态
start_simnow724.bat                  :: 拉起/确认 7×24 环境（guard.py --env 724）
start_simnow724.bat --status         :: 只看状态，不改动任何东西
start_simnow724.bat --force          :: 无视冷却强制回收（谨慎：会重新登录）
stop_all.bat                        :: 彻底停止且不再自启（见下）
```

两个启动器是同一个包装器，只差环境；按钟点用错会被 guard 拒绝并提示换另一个。
它们都不再接受 `config path` 参数——参数只认 `config\config_simnow.ini` /
`config_simnow724.ini`，要换参数直接改文件。

```bat
python tools\guard.py --status               :: 人读状态
python tools\guard.py --json                 :: 机器读
python tools\guard.py --strategy-only        :: 只换 strategy_proc.exe（保留 CTP 会话，不触发 4097）
python tools\guard.py --env day              :: 强制按 day 环境判定
python tools\guard.py --force-env 724 --minutes 60   :: 把环境钉死在 724 一小时（写 log\.envforce）
python tools\guard.py --clear-force          :: 解除钉死，回到按钟点判定
```

`--strategy-only` 是改完策略上线的主力路径：md/trader/query 三个 CTP 会话不动，
不会重连、不消耗重连配额，只把策略进程换掉。

**停止（重要）**：单独 `taskkill` 是无效的——常驻守护 20~40 s 内就会把树拉回来。
`stop_all.bat` 才是真正的开关：先挂 `flow\.halt`（守护看到它就拒绝修复），再杀四进程，
停机前还会跑一次持仓检查（空仓才"safe to stop"）。恢复就跑 `start_simnow.bat` /
`start_simnow724.bat`，它们会自动清掉 `flow\.halt` 再拉起，不用手动删文件。
`stop_all.bat --daemon` 连守护计划任务本身也停掉并禁用（彻底手动模式，恢复要先
`schtasks /Change /TN ctp_hft_daemon /ENABLE`）。**持仓不为零时停止 = 裸仓**（没有
止损、没有逃逸逻辑、没人看盘），脚本会先警告。

### 6.4 守护进程的判定规则

* **看活**：`tasklist` 探四进程；探测**失败**返回 `None`（= 未知，这一轮什么都不做），
  绝不等于“全死了”。输出少于 10 行视为不可信，退回 `Get-Process` 二次确认。
  ——这条是拿事故换来的：早期探测异常被当成“进程全没了”，守护进程把自己的健康进程树杀掉，
  17:19 / 17:24 / 17:36 三次无声消失都是这么来的。
* **二次确认**：发现缺失先等 3 s 复查，两次一致才动手。
* **分级修复**：只缺 `strategy_proc.exe` 走 strategy-only（不碰前置）；否则整树冷启动。
* **冷却**：自动回收 180 s 内最多一次、15 min 内最多 3 次；超限只记日志不动作。
  这只约束**自动修复**——`guard.py` 写入的 `flow\.restart_request` 是人工指令，
  守护直接执行不再二次限流（guard 写请求前已经过自己的冷却检查；4097 限流闸门
  在它之前仍然生效）。
* **限流闸门**：180 s 内出现 ≥6 次 `login timeout`（reason=4097），立刻 kill_all 并静默 300 s。
* **停机开关**：存在 `flow\.halt` 时不做任何自动修复（风控触发时由上层写入，删文件即恢复）。
* **环境钉死**：`log\.envforce` 里一行 `<env> <过期时间戳>`，到点自动失效；
  放在 `flow/` 会被清理流程删掉，所以落在 `log/`。

### 6.5 日志在哪

| 文件 | 内容 |
|---|---|
| `log/strategy.log` | 信号、报单、成交、回合、ESCAPE —— 最重要 |
| `log/trader.log` | 报单回报、错误码、撤单 |
| `log/md.log` | 行情登录、tick 计数 |
| `log/query.log` | 资金/持仓周期快照 |
| `log/guard_daemon.log` | 守护决策（为什么重启/为什么不动） |
| `log/launcher.log` | 冷启动过程 |
| `log/account.csv` | 每次 `acct_log.py` 追加的账户快照 |
| `log/halt_history.log` | 风控停机记录 |

```bat
watch_log.bat              :: 跟 strategy.log（默认）
watch_log.bat trader       :: 跟 trader.log
watch_log.bat all          :: strategy + trader 并行
watch_log.bat errors       :: 只看 WRN / ERR / ORDER ERROR / ROUND TRIP
```

### 6.6 日志后端：fmtlog

四个进程的日志统一走 [fmtlog](https://github.com/MengRao/fmtlog)（vendored 在
`third_party/fmtlog/`，MIT）：

* **热路径零 syscall**：`HFT_LOGx` 只把整行格式化进 thread_local 缓冲区并拷贝进
  fmtlog 的无锁 SPSC 队列（几十纳秒），磁盘写由后台 polling 线程（10 ms 节拍）批量完成；
  替代了旧实现"调用线程 mutex + fwrite + 100 ms fflush"的模式。
* **崩溃不丢错**：`flushOn(WRN)` 让 Warn/Error 在同一个 poll 周期内落盘（≤10 ms），
  保留旧版"错误必须死在磁盘上"的保证。
* **格式不变**：行格式仍是 `[HH:MM:SS.mmm][INF][name] body`（fmtlog 自带 header 已关，
  前缀由 `logger.cpp` 自己拼），guard / watch_log / rounds724 / pnl_fifo 全部照常工作。
* **满队列丢消息而非阻塞**：队列满时丢弃该条（FMTLOG_BLOCK=0）——旧版可能因慢盘卡住
  tick 线程，丢一条日志是更小的代价；1 MB 队列 + 10 ms 落盘，正常使用远远到不了。
* 接口保持 printf 风格（149 个调用点零改动）；新代码若要用 fmt 风格可直接
  `#include "fmtlog.h"` 调 `logi("x={}", x)`。

> 脚本用 `tail.exe`（`C:\Users\admin\bin\tail.exe`，uutils coreutils 静态版），
> 找不到时退回 PATH 里的 `tail`。Ctrl+C 只停显示，不动交易进程。

### 6.7 仿真 / 实盘手动方式

```
start_sim.bat                                  :: 四进程仿真，无需账号
build\bin\monitor.exe config\config.ini 1000   :: 观察队列、心跳、账户
stop_all.bat                                   :: 停止
```

手动起单进程（顺序无所谓，谁先起来谁创建共享内存）：

```
build\bin\md_proc.exe       --config config\config_simnow.ini
build\bin\trader_proc.exe   --config config\config_simnow.ini
build\bin\query_proc.exe    --config config\config_simnow.ini
build\bin\strategy_proc.exe --config config\config_simnow.ini
```

> 手动起的进程会随 shell / 会话退出被 job 对象回收，**只用于调试**；常驻请走第 6 节。
> 标准 CTP 柜台每天首次交易前需要**确认结算单**，交易进程登录后自动完成并等待（最多 60 s）。

> **两种 live 风味不能混用 exe**：
>
> | 风味 | 目录 | SDK | 适用柜台 |
> |---|---|---|---|
> | CTP Mini | `build\bin` | `..\win64`（V1.7.x） | CTPII Mini 极速柜台 |
> | 标准 CTP | `build\bin_simnow` | `sdk\stdctp`（6.7.7） | SimNow、普通 CTP 实盘柜台 |
>
> 差异只在三个 CTP 进程的编译宏（`HFT_CTP_STD`，见 `src/common/ctp_compat.h`）：
> 标准版多结算单确认、`Init()` 签名不同；策略、共享内存、队列完全一致。

## 7. 策略 obi_voi（L1 失衡反转 + 括号单）

`src/strategy/obi_voi_strategy.cpp`：基于一档盘口的订单流失衡做**反转（fade）**。
**纯回报驱动**——`onTick` 只做决策，一切成交/撤单以 `OnRtnOrder` / `OnRtnTrade` 回报为准。

### 7.1 信号

* `OBI = (bidVol1 − askVol1) / (bidVol1 + askVol1)`，一档挂单失衡；
* `VOI = EMA(带符号主动成交量) / EMA(|主动成交量|)`，按最新价方向给增量定号，衡量主动流方向；
* **条件 A（极端）**：`window_ticks`（20）内 |OBI| 曾超过 `obi_threshold`（0.95）；
* **条件 B（衰竭）**：VOI 已反向且幅度 ≥ `voi_entry_threshold`（0.1）；
* **条件 C（衰减确认）**：**当前** OBI 已从极值回落（越过 `threshold − margin`）——
  只在“极端正在瓦解”时入场，而不是在极端最强时逆势硬顶；
* 三个条件同时成立 → 在**弱势侧** fade（买盘极端就卖、卖盘极端就买）；
* 每个方向有 `armed` 门控：一次触发后需 VOI 重新回到该方向才解锁，避免同一段行情连打。

### 7.2 交易规则

| 场景 | 行为 |
|---|---|
| A+B+C 成立 | 弱势侧入场。`entry_taker=1` 时跨 1 tick 对手价 + IOC，保证落到 |
| 盘口价差 > `max_spread_ticks`（1 tick） | **跳过入场**——目标价从成交价起算，多 1 tick 价差就多 1 tick 要走。实测：spread≤1 → +4.06 tick/笔，≤2 → −0.60 |
| 入场 `entry_timeout_ticks`（4 tick）未成交 | 撤单，回冷却 |
| **入场成交的瞬间** | 立刻挂止盈限价单 @ `entry ± profit_ticks`（16 tick），**GFD，永不主动撤** |
| 价格逆向触及 `entry ∓ stop_loss_ticks`（8 tick） | 撤掉止盈单 → IOC 止损，跨 `stop_slippage_ticks`（2 tick）确保成交 |
| 止盈单被柜台/对手撤掉 | 按 `tp_repost_interval_ticks`（20 tick）节奏补挂，最多 `tp_repost_max`（8）次 |
| 补挂耗尽仍未成交 | 退化为纯价格触发 IOC 退出，不再挂单 |
| 价格触及目标且无在途止盈单 | 以**目标价**为限价的 IOC 落袋——跑过目标时成交在更优买/卖价，回抽时不成交 |
| 持仓超 `max_hold_seconds`（600 s） | **硬墙钟兜底**：无条件 IOC 逃逸，任何价格 |
| 逃逸 IOC 被撤（status=5, filled=0） | 800 ms 后重发，每 **2** 次跨价 +1 tick（最多 +6） |
| 平仓 offset 被拒 | 环形回退 `平今(3) → 平昨(4) → 平仓(1)`，适配 SHFE 的今昨仓规则 |
| 逃逸累计 200 次仍不成交 | `haltTrading()` + ERROR 日志（宁停不裸奔） |
| **连续 3 笔亏损回合** | 停止找信号 `pause_seconds`（300 s）——fade 在趋势里会连亏，见 7.4 |
| 库存 | 净仓上限 `max_position`（1 手），到顶只允许平仓方向 |
| 策略启动 | 撤残留挂单 + 清柜台遗留持仓（今/昨拆分，50/51 拆量重试） |

**成本口径**（ag2612：tick = 1 元/千克 × 15 = **¥15/跳**，往返手续费约 **2 跳**）：

* TP 16 跳 / SL 8 跳，账面净 ≈ **+¥240 / −¥135**，含费平衡胜率 ≈ **45%**（实测 58–63%）；
* 但目标是从**成交价**起算的，taker 入场成交在对手价，所以**中价必须先走 17 跳**才够 TP，
  逆向只需走 7 跳就触发 SL（价差 2 跳时）。这个不对称是 taker 入场的真实代价，
  也是为什么止损必须放到 8 跳（6 跳落在 ag 的噪声带里，会被随机波动打掉）；
* 所以参数不是"越敏感越好"，而是**少打、打准**：`window=10`、`obi=0.90`、
  `cooldown=12`、`max_spread=1`、`profit=16`、`stop_loss=8` 是拿 11-05 / 11-06 /
  11-10 三段实测换来的，完整归因见 §7.6。

### 7.3 关键实现点

* pending 槽只在收到**终结回报**（全成/已撤/拒绝）才清空；迟到回报按 cmdId 丢弃；
* **成交证据只看 `volumeTraded`**，不看 `OrderStatus`：CTP 的 `OnRtnOrder(AllTraded)`
  可能早于 `OnRtnTrade`，早期版本据此记账会产出 `entry=0.00 pnl=+12434` 的幽灵回合；
  现在配 300 ms（成交）/ 200 ms（撤单）宽限窗口等成交回报补齐；
* SHFE / INE 不接受市价单，全部用限价（IOC 也是限价 + FAK），天然兼容；
* VOI 用 EMA 归一化，避免量纲影响穿零判定；
* SimNow 会秒撤不主动成交的挂单（实测 **80–300 ms**，白天第一套前置与 7×24 一样）：
  策略把外部撤单当“信号失败”进入冷却，`entry_taker=1` 让信号直接以 IOC 落地，
  不再依赖挂单排队；
* **限价 IOC 多跨几跳并不增加成本**：IOC 以“最优可成交价”撮合，跨价只决定它能否
  够到还有量的那一档。所以止损用 2 跳、逃逸逐次加宽，成交价仍是当时的对手价；
  反过来，入场只跨 1 跳即可（跨太多只会在盘口薄时吃到更差的一档）；
* **成交即记录 `entryPrice`**，即使回报没能匹配到在途单槽：早期版本只在 `tracked`
  时写 `entryPrice`，11-06 11:02 那笔空单因此把止盈挂在了**盘口价**（12453）而不是
  目标价（12447）——等于把整个 fade 边际送给了价差；
* **括号价在入场成交时一次算好**（`armBracket`）：`tpTrigger / slTrigger / tpPrice`
  落进 State，之后每个 tick 只做纯数值比较（`touch >= tpTrigger − ε`），
  热路径上没有乘除、没有取整、没有分支外的计算。

**真实日志样例（2026-11-06 11:14，taker 入场 + 修好的止盈价）：**

```
[11:14:21.640][INF] SIGNAL ag2612 FADE-BUY-EXTREME obiMax=0.95 obiNow=0.88 voi=-0.63 -> SELL @ ask 12416.00
[11:14:21.640][INF] ORDER  ag2612 SELL OPEN vol=1 LIMIT @ 12413.00 (taker: bid1+1t, IOC, cmdId=12)
[11:14:21.805][INF] TRADE  ag2612 SELL OPEN vol=1 @ 12415.00          <- 成交在 bid1，不是 12413
[11:14:21.805][INF] ORDER  ag2612 BUY CLOSE vol=1 LIMIT @ 12407.00 (TP bracket, post 1/8)   <- entry-8，正确
[11:14:21.872][INF] order 3341424 finished, status=5 (passive close, filled=0/1)  <- 柜台 67 ms 就撤了
[11:14:41.405][WRN] ESCAPE ag2612: price stop loss hit -> IOC stop order (entry=12415.00 voi=+0.32 net=-1 hold=37t)
[11:14:41.710][INF] ROUND TRIP ag2612 closed @ 12420.00 (entry=12415.00 short, pnl=-5.00/unit, IOC escape)
[11:14:41.710][WRN] ag2612: 3 losing round trips in a row -> stand down for 300 s
```

启动日志会打印生效参数，换版本后第一件事就是核对这一行：

```
obi_voi: mode=fade window=10 obiTh=0.90 voiEma=5 voiFlip=0.35 voiEntry=0.10 |
entryTimeout=4t entrySlip=1t taker=ioc profit=16t closeTimeout=6t stopLoss=8t stopSlip=2t
maxHold=0t tpRepost=0/20t maxSpread=1t trend=30000ms/1t vol=1 maxPos=1 short=yes |
maxHoldSec=600 escapeRetry=800ms consecStop=3/180s
obi_voi: no trade_sessions configured -> entries allowed 24 h (set e.g. 21:00-02:30 to restrict)
```

最后一行是时段闸门。现在默认没有窗口（全天可开仓）；一旦配了窗口（如 `21:00-02:30`），
窗口外收到信号只记日志不下单。

### 7.4 2026-11-06 的实测与结论

盘中把 09:00–15:00 的信号与成交逐条对齐后，改掉了四件事（都已在上面表里）：

1. **白天配置缺 `entry_taker`**：入场是 GFD 被动单，`bid1+1` 在 2 跳价差里根本没跨过去，
   19 笔报单只成 5 笔（26%），14 笔是**柜台**撤的（策略自己只撤 1 笔）。
   补上 `entry_taker = 1` 后入场成交率回到接近 100%。
2. **止盈价用了盘口价**（成交回报没匹配到在途单，`entryPrice` 没写）；
3. **卖单补挂 40 次是纯噪音**：静挂的 TP 只有 ~200 ms 寿命，补挂基本都在喂撤单，降到 8 次；
4. **趋势日会连亏**：11:02–11:14 三笔 fade 全被同一段下跌打掉（−13 / −7 / −5），
   20 tick 冷却（约 10 s）完全挡不住，因此加了 `consec_stop_pause`。

仍未解决、需要数据说话的：**fade 在单边日天然亏钱**，`profit_ticks=8 / stop_loss=6`
是按“中价走 9 跳 / 5 跳”的不对称结构，是否要按趋势状态（例如 20 tick 中价斜率）动态
停手，得先回测再改——不要凭盘中手感调参。

### 7.5 2026-11-10 实盘复盘（ag2702 日盘 71 笔报单）

用户导出的柜台委托单（09:30–15:21，含 8 笔撤单）做 FIFO 回合配对：**31 个回合，
毛 −51 tick（−765 元），手续费约 602 元，净约 −1367 元，胜率 32%**。三个结论：

1. **参数是旧的**：盈利单集中在 +7…+9 tick、亏损单集中在 −5…−9 tick，正是
   `profit=8 / stop_loss=6 + 2 tick 止损滑点` 的指纹。回放 131 783 条真实 tick：
   `tp8/sl6` 夜盘 −2.09 tick/笔、日盘 −8.39；`tp16/sl8` 夜盘 +4.13、日盘 −4.89。
   仓库配置里确实还是 8/6（调参没落盘），已统一改成 16/8 + `trade_sessions`。
2. **亏损全在"入场后 30 秒内"**：持仓 ≤30 s 的 18 笔合计 −75 tick（胜率 12%/20%），
   >30 s 的 13 笔 +24 tick。方向是**入场时脉冲还在延展**——30 s 漂移过滤只看
   "跑过没有"，分不清"20 秒前的超调"和"2 秒前刚启动"。当时加了 `entry_stall_ticks=2`
   （要求已从近 6 tick 极值回撤 2 tick）。**但当轮就被自己的回测否掉了**：
   在真实交易域（spread≤1、tp16/sl8、夜盘）复算，`stall 0→+4.13 / 2→+3.83 /
   4→+3.95 / 6→+0.50 / 8→−0.17`——等回撤确认恰好把最好的信号过滤掉。默认改回 0，
   完整推导见 §7.6。
3. **收盘前 45 秒开了仓**：14:59:15 买开，15:21:04 的平仓被 SHFE 拒绝
   （"当前状态禁止此项操作"），仓位扛过收盘。将 `entry_blackout_sec` 收紧到 1800 秒
   （15:00 收盘的日盘窗口于 14:30 停止开仓），订单被拒时识别"休市"类错误 → 10 分钟内不再发单，
   吃单逻辑照旧只记日志不重试。

另外，报表对应的 13 个回合曾摸到 ≥ +7 tick，其中 5 个最终收在 ≤ −6。把保本线
从 8 降到 7 看起来能更早保护利润，但旧合约 131 783 条 tick 的夜盘回放中，`BE=8`
净 +287 tick，`BE=7` 净 +255 tick，不启用保本净 +281 tick。降低到 7 虽能减少部分回吐，
也会提前截断更多后续盈利单，因此保留 8；需要用 `ag2702` 的逐笔行情再验证。

### 7.6 信号有效性归因：三个闸门决定盈亏符号（2026-11-10 晚）

前几轮优化都在 tp/sl/阈值上打转，始终没回答一个前置问题：**信号本身有没有预测力**。
`tools/signal_alpha.py` 把 tp/sl 全部剥掉，只测信号之后 h 个 tick 的中价漂移：

```
ag2612 夜盘 th=0.90，反转方向 edge（tick）
h=1    -0.66   t=-14.8   命中 23%    <- 信号后第一个 tick 继续往极端走
h=5    -0.24                          <- 仍在水下
h=8    +0.04                          <- 回零
h=25   +0.22   命中 59%
h=40   +2.53   命中 58%
h=100  +3.98   命中 55%               <- 方差大，不稳
```

两条硬结论：

1. **短周期是动量，不是反转。** 信号出现后 1 个 tick 平均还要再走 −0.66 tick，
   t = −14.8（去趋势后依然如此：夜盘 21:00–02:30 首尾几乎持平，区间却有 537 点，
   不是单边趋势伪造的）。现在"信号一出立刻 IOC 跨价追"正好吃这一口，等于每笔
   先天亏 0.66。这解释了 11-10 那批 ≤30 s 的亏损单。edge 在 h≈8 回零、h≥25 才转正。
2. **edge 量级 ≈ 1~3 tick，而 taker 往返成本是 3.28 tick**（跨价 1×2 + 手续费 0.64×2）。
   这是"怎么调参都亏"的算术根源。

真正把符号翻过来的不是参数，是**三个闸门**（口径：串行持仓、含费用、夜盘）：

| 闸门 | 不满足 | 满足 | 差 |
|---|---|---|---|
| 时段 | 日盘 −2.52 … −4.90 | 夜盘 **+4.06** | ~6.6 tick |
| 价差 | spread≤2 → −0.60 | spread≤1 → **+4.06** | ~4.7 tick |
| 止盈止损 | tp8/sl6 → −1.18 | tp16/sl8 → **+4.06** | ~5.2 tick |

三者是**与**关系，缺一个就回到水下。11-10 实盘三个全不满足（日盘 + spread 2 +
tp8/sl6），模拟给出 −4.90 tick/笔，实盘实测 −1.65 tick/笔（同向同量级）。

**统计现实**：+4.06 来自 146 笔、1.5 个交易日的同一份数据，t≈2.4，bootstrap 95%
置信区间约 [−0.4, +4.7]。只能算"边际为正、待样本外确认"，**不是已验证的盈利策略**。
单日 20 笔时期望 +81 tick 而标准差约 55 tick，日胜率只有 ~60%——每 5 天有 2 天亏钱
是正常的，不要用单日结果肯定或否定策略。

由这条推导出的运行建议：

- **配置审计**：`max_spread_ticks`、止盈参数偏离历史样本时记录告警，不再将单一合约的
  回测结果当作启动硬门槛。`trade_sessions` 留空即全天开仓，不再是隐性开关；
  平仓和风险管理始终运行。逐 tick 研究日志默认关闭，可用 `[general] debug_logging=true` 开启。
- **日亏损熔断**：`daily_loss_limit_ticks=35`。按交易所交易日累计，夜盘与其后的日盘共用
  一个预算；达到后不再开新仓，持仓继续按括号单退出。设成 0 时只告警、不拦截。
- **收盘前开仓保护**：`entry_blackout_sec=1800`。仅对已配置的 `trade_sessions` 生效，窗口结束前
  30 分钟禁止新开仓；没有配置窗口时这条自然不生效。

### 7.7 2026-11-11：整天零委托，不是策略不工作，是闸门太安静

**现象**：11-11 交易日 09:00-15:00 一个委托都没发出。四份日志看起来全都健康
（`ticks=2/s`、`execs=0`、`backlog=0`、无 ERR）。

**真实时间线**（同一份日志按段切开看）：

| 时段 | 委托 | 成交 | 说明 |
|---|---|---|---|
| 00:00-02:30（夜盘尾） | 96 | 92 | 46 个回合，净 −197 tick |
| 09:00-15:00（日盘） | **0** | 0 | 被 `trade_sessions` 挡住 |
| 09:03 | 1 | 1 | `STARTUP FLATTEN` 把凌晨遗留的 1 手多单平掉 |

**原因**：`config_simnow.ini` 的 `trade_sessions = 21:00-02:30`。`evaluate()` 在第三个
`if (!inSession(tick)) return;` 就返回了，**在此之前不打印任何东西**。于是"配置决定不开仓"
和"策略坏了"在日志上完全同形。

**修复（2026-11-11 收尾）**：`trade_sessions` 留空 = 全天可开仓，这是三套配置现在的默认值
（原来空值在 live 模式会 fail-closed，已删除该逻辑）。想限制时段仍可写窗口，只是不再是
隐性开关。

**修复（可观测性，不改交易逻辑）**：

- 第一笔 tick 打印当前墙钟是否在窗口内，以及距离下一个窗口多久：
  `first tick ag2702 at 09:00:12 - OUTSIDE every entry window; no order for the next 11:59`
- 每 60 秒一行汇总，只在真被挡住时打印，逐 tick 只做一次 `uint64_t++`：
  ```
  ag2702: no entry order in the last 60 s - 203/214 ticks blocked
          | outside_session=203 entry_cutoff=0 daily_halt=0 wide_spread=0
  ```
  四个计数器分别对应 `trade_sessions` / `entry_blackout_sec` / 日亏损熔断 / `max_spread_ticks`。
  看到这一行就知道该改哪个参数，不用再猜。

**同一天暴露的另一个问题**：02:24:11.580 最后一笔 tick 是 12632，02:24:22.478 直接跳到
12787——**11 秒无 tick、跳空 155 tick**，一手空单的止损从 −8 变成 −151 tick（−2265 元），
一笔就吃掉当日全部。这是 SimNow 行情源在夜盘收尾的跳变，不是止损逻辑的问题（跳空时任何
触发都追不回来），但说明**统计盈亏必须剔除单笔 >20 tick 的离群值**，否则一天的数字全是噪音。

**验证**：`verify_sim.bat` 用 `config.ini`（`mode = sim`，无柜台无网络）跑
`SIGNAL -> ORDER -> TRADE -> ROUND TRIP`，5 秒内出结果。要验证真实柜台链路，
直接用 `config_simnow724.ini` 起四个进程——它现在也是全天可开仓。

**没做、也不建议做的**：

- **maker 入场**：spread≤1 时反而更差（+2.13 vs taker +2.43），省下的跨价被逆向选择
  吃掉。`wait=5` 算出的 +4.75 是 12 组里挑最好的，典型过拟合。
- **继续在这 1.5 天数据上调参**：已经试过上百组，最好那格必然偏乐观。扩样本
  （多品种 + 多交易日）比继续调参重要一个数量级。

## 8. 运维与审计工具

```bat
:: 进程/环境/日志健康（人读 + 机读）
python tools\guard.py --status
python tools\guard.py --json

:: 账户快照：只开共享内存只读窗口，不登录、不下单、不重连
build\bin_simnow\account_peek.exe --positions
python tools\acct_log.py --print        :: 顺手追加一行到 log\account.csv

:: 真实盈亏：不信策略自己写的 ROUND TRIP，用 TRADE 明细做 FIFO
python tools\pnl_fifo.py
python tools\pnl_fifo.py --instr ag2612 --fee-ticks 2 --after 523450

:: 回合统计：日志只有 HH:MM:SS，跨零点靠反向重建时间线
python tools\rounds724.py --hours 2
python tools\rounds724.py --hours 6 --instr ag2612

:: 当日柜台实况（资金/持仓/成交/报单 + FIFO 回合 + 每合约跳数金额）
:: 注意：CTP 的 ReqQryTrade 只能查当前交易日，隔夜成交柜台取不到
build\bin_simnow\qry_today.exe --config config\config_simnow724.ini
tools\qry_poll.sh     :: 前置不可用时按间隔重试（SimNow 停摆时守开盘用）
tools\qry_watch.sh    :: 多前置轮询版

:: 策略回测：本机录的 tick 数据回放参数网格（含费用）
python tools\bt_sweep.py
python tools\bt_sweep.py --eval 0.90,0.10,1,16,8,300 --session night --stall 2 --be 8
::   --stall N  入场要求价格已从近 --stall-win 个 tick 的极值回撤 N tick
::   --be M     浮盈 M tick 后止损移到开仓价（保本）

:: 信号有效性归因：剥掉 tp/sl，只测信号之后价格怎么走（§7.6 的全部数据来源）
python tools\signal_alpha.py --instr ag2612 --cost
::   输出每个阈值下 h=1…120 tick 的 edge / 命中率 / se / t，
::   以及分时段、分波动率、反转 vs 动量两个方向的对照。
::   调参之前先跑这个：edge 小于往返成本（约 3.3 tick）时，任何 tp/sl 都救不回来。

:: 无人值守跑一个完整交易时段（等前置开盘 -> 起四进程 -> 挂了自动重启
:: -> 收市停 -> 落一份 log\session_<tag>_summary.md）
python tools\session_run.py --mode auto      :: 今夜 20:50-02:31 + 次日 08:52-15:06
python tools\session_run.py --mode now       :: 立刻起，02:31 停
python tools\session_run.py --mode summary --tag night_20261111   :: 重算小结
::   进度写在 log\session_run.log，逐条带时间戳

:: 推代码：本机到 github.com:443 不通，走 api.github.com 的 git-data 接口
set GITHUB_TOKEN=ghp_xxx                :: classic token，勾 repo
python tools\gh_push_api.py
```

`account_peek` 输出中三个数最关键：`close_profit`（今日已实现）、`commission`（手续费）、
`net_today = balance − pre_balance`（账户口径净变动）。**策略日志的盈亏和账户口径不一致时，
以账户为准**。

`gh_push_api.py` 是 `git push` 的替代品：它从 **git 索引**（不是工作区）取 blob，因此 GBK
源码不会被重新编码、可执行位也不会丢；上传后会校验每个 blob 的 sha 未变、重建的 tree
与本地一致，再提交并更新 ref。正常情况下远程 commit sha 与本地 `HEAD` **完全相同**。

## 9. 踩过的坑（都是不变量）

| 现象 | 根因 | 现在的做法 |
|---|---|---|
| 进程几分钟后无声消失 | 计划任务实例回收连带杀子进程树 | 常驻 `guard_daemon.py` 持有子进程；任务只拉守护 |
| `schtasks.exe` 直接“拒绝访问” | 本机安全策略把计划任务工具列入黑名单，不能绕过 | 改用 `session_run.py` 作为常驻进程值守（重启后需手工补位） |
| 守护进程被 0xC000013A 杀死 | `python.exe` 收 `CTRL_CLOSE_EVENT` | 计划任务改 `pythonw.exe` + 失败重启 |
| 幽灵回合 `entry=0.00 pnl=+12434` | `OnRtnOrder(AllTraded)` 早于 `OnRtnTrade` | 以 `volumeTraded` 为成交证据 + 宽限窗口 |
| 14 小时裸空单（11-05 夜盘） | 平仓 IOC 被撤后 `phase=escaping` 无重发，永久卡死 | 800 ms 重发 + 跨价递增 + `max_hold_seconds=600` + 200 次 halt |
| 14 个信号 0 成交 | 7×24 前置 100–500 ms 秒撤挂单 | `entry_taker=1`：跨价 + IOC |
| 反复 4097 限流越重连越久 | SimNow 前置 IP 级惩罚 | 180 s 冷却 / 15 min 3 次上限 / 6 次超时静默 300 s |
| 守护误判“全死”自杀进程树 | `tasklist` 异常被当成空集 | 探测失败返回 `None`（未知），<10 行不可信，PS 兜底 |
| `.bat` 报“此时不应有 .。” | `if (...)` 块里的裸括号未转义 | 括号写 `^(` `^)` |
| 脚本 LF 换行报 “unexpected at this time” | cmd 解析括号块要求 CRLF；LF 版还会把 `start /b` 拆成 `tart`、并凭空生成 `12` / `next` 空文件 | `.bat` 一律 CRLF，仓库加 `.gitattributes`（`*.bat text eol=crlf`）锁死；延时用 `ping -n N+1 127.0.0.1 >nul` |
| `query.log` 一直打印 `balance=1000000 pos=0`，而快照里是 2000 万（11-11） | 两处：查询会话没做结算确认；日志打印的是 `simulation` 才会赋值的成员 | `CtpQueryGateway` 登录后 `ReqSettlementInfoConfirm`；统计行在 live 模式改读共享快照（`SnapshotReader`） |
| 19 笔报单只成 5 笔（11-06 09:00） | 白天配置缺 `entry_taker`，`bid1+1` 在 2 跳价差里没跨过去，柜台 80–300 ms 就撤 | 白天也 `entry_taker=1`；`max_spread_ticks=2` 过滤宽价差 |
| 止盈挂在 12453 而不是 12447 | 成交回报未匹配到在途单，`entryPrice` 没写，TP 回退到盘口价 | 任何 open 成交都记 `entryPrice`（不再限于 `tracked`） |
| 止损一笔亏 13 跳（应为 6） | 止损 IOC 只跨 1 跳，急跌里连追 3 次（12433→12431→12425） | `stop_slippage_ticks=2` + 逃逸每 2 次加宽 1 跳（上限 +6） |
| 11:02–11:14 三笔 fade 连亏 | 单边下跌 45 跳，20 tick 冷却挡不住同向连打 | `consec_stop_pause=3`：连亏 3 笔停手 300 s |
| **一天亏 1367 元，而仓库里的参数是对的**（11-10） | 实盘机跑的是旧配置文件：日盘没闸门、`spread=2`、`tp8/sl6`。三项叠加在当时回测里是 −4.90 tick/笔 | 记录实际生效参数；用多日样本外回测验证，不把窄样本结论写成通用启动门槛 |
| 单日连亏 10 笔仍在开仓（11-10） | 只有"连续 N 笔"的停手，没有"当日累计"的预算 | `daily_loss_limit_ticks=35`，按交易所交易日累计，换日自动重置 |
| 行情或成交回报队列溢出 | 丢失消息后策略状态可能与市场/柜台不同步 | 锁住新开仓并记录完整性故障；冷启动后先核对柜台持仓再恢复 |

## 10. 接入自己的策略

1. 继承 `hft::Strategy`（`include/hft/strategy_base.h`），实现 `init/onTick/onExec/onTimer`；
2. 在 `src/strategy/strategy_main.cpp` 的 `onStart()` 里按 `config [strategy] name` 加分支；
3. 新参数放 `[your_strategy]` 段，用 `cfg.getInt/getDouble/getBool` 读。

策略只见 `StrategyContext`（下单、撤单、查持仓/资金、最新价、读柜台持仓快照
`snapshotReady()/brokerPosition()`、接管柜台仓位 `adoptPosition()`），不碰 CTP、不碰共享内存，
因此同一份策略在 sim 与 live 下代码完全一致。

上线流程：`build_strategy.bat` → `python tools\guard.py --strategy-only` → 核对启动参数日志。

## 11. 已知边界

* ~~实盘必须用 MSVC~~：已通过运行时加载 CTP DLL（`src/common/ctp_loader.*`）消除，
  MinGW 同样支持 live；去掉 SDK 目录会自动退化为纯 sim 构建。
* 标准 CTP 会向新会话重放当日委托/成交回报；交易进程按 FrontID/SessionID/OrderRef 过滤
  （`isReplay`），只有本会话的单子进入执行回报队列。
* 每个通道严格 SPSC：一个通道只能有一个写进程和一个读进程。
* 仿真撮合只判“是否穿过对手价”，不模拟排队与部分成交优先级；真实交易以柜台回报为准。
* 共享内存按 Windows 会话（`Local\`）隔离：远程桌面不同会话之间互不可见。
* `shm_reset.exe` 会清整条总线，**运行中调用等于让四个进程失联**，只在冷启动脚本里用。
* 策略的本地账本与柜台持仓若长期不一致（`staleBrokerRounds` 连续多轮），会强制重启策略
  做一次 `flatten_on_start` 对账；这是已知的唯一“自愈式丢状态”路径。
* **SimNow 7×24 环境的行情不能当业绩看**：2026-11-11 17:00 实测，tick 里的
  `updateTime` 比墙钟快约 7 分钟，且价格会跳段（17:23 一笔空单从 12727 直接跳到
  12828 成交，+101 tick）。它适合验证链路（下单/成交/止盈/止损/保活），
  不适合评估策略盈亏；统计时剔除单笔 >20 tick 的离群值。
