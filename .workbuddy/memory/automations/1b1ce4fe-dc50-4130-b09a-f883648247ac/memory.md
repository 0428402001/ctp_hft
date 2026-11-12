# 夜盘盘后小结自动化 — 执行记录

## 2026-11-12 02:45 执行（night_20261111 场次）
- 值守脚本正常：log/session_night_20261111_summary.md 存在（20:50→02:31，无进程重启）。
- 结果：10 回合，-42 tick（≈-630 元，ag 15元/tick），胜1负8平1，胜率10%，全部 IOC escape 出场，TP/保本 0 触发。
- 21:37 起 dayPnL=-42t 触发 daily_loss_limit_ticks=35 熔断，之后无新开仓（拦截汇总 318 条，daily_halt 累计 34975 tick，wide_spread 930）。
- 无 [ERR]、无拒单。持仓归零（10开10平），无隔夜持仓。
- account_peek 失败：收盘后共享内存 HFT_SHM_SNAPSHOT 已释放，属预期；真实余额待日盘会话核对。
- 结论：样本 <30 笔，不调参；建议累计样本后再评估 stop_loss_ticks=8 在开盘高噪声段是否偏紧。
