// SPDX-License-Identifier: MIT
#include "hft/process_app.h"

#include "hft/logger.h"
#include "hft/time_util.h"

#include <algorithm>
#include <cstring>
#include <thread>

#if defined(HFT_POSIX)
#  include <csignal>
#endif

namespace hft {

#if defined(HFT_WINDOWS)
// timeBeginPeriod(1) without linking winmm.lib (keeps every build script
// link-flag free). Loaded once, never released - the process wants the 1 ms
// timer resolution for its entire lifetime anyway.
static void setTimerResolution1ms() {
    HMODULE winmm = ::LoadLibraryA("winmm.dll");
    if (!winmm) return;
    using Fn = UINT(WINAPI*)(UINT);
    Fn begin = reinterpret_cast<Fn>(::GetProcAddress(winmm, "timeBeginPeriod"));
    if (begin) begin(1);
}
#endif

static const char* roleSection(Role r) {
    switch (r) {
    case Role::Md: return "md";
    case Role::Strategy: return "strategy";
    case Role::Trader: return "trader";
    case Role::Query: return "query";
    }
    return "general";
}

void ProcessApp::applyPerformanceSettings() {
    // spin_us = 0 (default): pure event driven. The idle path goes straight
    // to the kernel events; a producer's doorbell wakes the loop in
    // microseconds. A positive spin_us keeps the old hybrid: burn up to N us
    // in _mm_pause before blocking (shaves a fraction of a microsecond off
    // the pickup latency at the price of a fully busy core while idling).
    spinUs_ = cfg_.getInt("general", "spin_us", 0);
    idleUs_ = idleSleepUs();

#if defined(HFT_WINDOWS)
    // 1) Sleep granularity: default Windows timer ~15.6 ms; 1 ms from here on.
    setTimerResolution1ms();

    // 2) Process priority: keeps the poll loops ahead of background work.
    if (cfg_.getBool("general", "high_priority", true)) {
        ::SetPriorityClass(::GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    }

    // 3) Main-thread CPU pinning (the poll loop thread only - CTP worker
    //    threads stay free so they do not fight the hot loop for the core).
    //    Key lookup: [<role section>] cpu, fallback [general] cpu. 0 = off.
    int cpu = cfg_.getInt(roleSection(role_), "cpu", -1);
    if (cpu < 0) cpu = cfg_.getInt("general", "cpu", -1);
    if (cpu >= 0 && cpu < 64) {
        const DWORD_PTR old = ::SetThreadAffinityMask(::GetCurrentThread(),
                                                      (DWORD_PTR(1) << cpu));
        if (old) {
            HFT_LOGI("perf: spin=%dus timer=1ms priority=HIGH main thread pinned to cpu %d",
                     spinUs_, cpu);
        } else {
            HFT_LOGW("perf: SetThreadAffinityMask(cpu=%d) failed (err=%lu)", cpu,
                     ::GetLastError());
        }
    } else {
        HFT_LOGI("perf: spin=%dus timer=1ms priority=HIGH affinity=off", spinUs_);
    }
#else
    HFT_LOGI("perf: spin=%dus (POSIX: no timer/priority tuning applied)", spinUs_);
#endif
}

std::atomic<bool>& ProcessApp::globalStopFlag() {
    static std::atomic<bool> gFlag{false};
    return gFlag;
}

#if defined(HFT_WINDOWS)
static BOOL WINAPI consoleCtrlHandler(DWORD type) {
    switch (type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        ProcessApp::globalStopFlag().store(true, std::memory_order_release);
        return TRUE;
    default:
        return FALSE;
    }
}
#endif

void ProcessApp::installConsoleHandler() {
#if defined(HFT_WINDOWS)
    ::SetConsoleCtrlHandler(consoleCtrlHandler, TRUE);
#else
    std::signal(SIGINT, [](int) { ProcessApp::globalStopFlag().store(true); });
    std::signal(SIGTERM, [](int) { ProcessApp::globalStopFlag().store(true); });
#endif
}

ProcessApp::ProcessApp(Role role, const char* name) : role_(role), name_(name) {}

ProcessApp::~ProcessApp() { stopHeartbeat(); }

void ProcessApp::printUsage() const {
    std::fprintf(stderr,
                 "usage: %s [--config <path>] [--mode sim|live] [--log-dir <dir>] "
                 "[--no-console] [--help]\n",
                 name_.c_str());
}

bool ProcessApp::parseArgs(int argc, char** argv, std::string& err) {
    std::string configPath = "config/config.ini";
    std::string modeOverride;
    std::string logDir;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* what) -> bool {
            if (i + 1 >= argc) {
                err = std::string("missing value for ") + what;
                return false;
            }
            return true;
        };
        if (a == "--config") {
            if (!next("--config")) return false;
            configPath = argv[++i];
        } else if (a == "--mode") {
            if (!next("--mode")) return false;
            modeOverride = argv[++i];
        } else if (a == "--log-dir") {
            if (!next("--log-dir")) return false;
            logDir = argv[++i];
        } else if (a == "--no-console") {
            consoleLog_ = false;
        } else if (a == "--help" || a == "-h") {
            printUsage();
            err = "help";
            return false;
        } else {
            err = "unknown argument: " + a;
            return false;
        }
    }

    if (!cfg_.load(configPath, err)) return false;
    sim_ = cfg_.simulation();
    if (modeOverride == "live") sim_ = false;
    if (modeOverride == "sim") sim_ = true;
    if (logDir.empty()) logDir = cfg_.logDir();

    Logger::instance().init(logDir, name_, consoleLog_);
    Logger::instance().setLevel(LogLevel::Debug);
    return true;
}

bool ProcessApp::init(int argc, char** argv) {
    std::string err;
    if (!parseArgs(argc, argv, err)) {
        if (err != "help") std::fprintf(stderr, "%s: %s\n", name_.c_str(), err.c_str());
        return false;
    }

    installConsoleHandler();

    // Control block: whoever starts first creates it; nobody ever "owns" it.
    if (!attachChannels(err)) {
        HFT_LOGE("attachChannels failed: %s", err.c_str());
        return false;
    }
    if (!control()) {
        HFT_LOGE("control block not available");
        return false;
    }

    ControlBlock* ctl = control();
    ctl->hb[static_cast<uint32_t>(role_)].pid.store(
#if defined(HFT_WINDOWS)
        static_cast<uint32_t>(::GetCurrentProcessId()),
#else
        static_cast<uint32_t>(::getpid()),
#endif
        std::memory_order_release);

    HFT_LOGI("%s initialised (mode=%s, config=%s)", name_.c_str(), sim_ ? "sim" : "live",
             cfg_.path().c_str());
    return true;
}

void ProcessApp::beginHeartbeat() {
    if (heartbeatRunning_.load()) return;
    heartbeatRunning_.store(true);
    ControlBlock* ctl = control();
    int periodMs = cfg_.heartbeatMs();
    heartbeatThread_ = std::thread([this, ctl, periodMs]() {
        while (heartbeatRunning_.load(std::memory_order_acquire)) {
            Heartbeat& hb = ctl->hb[static_cast<uint32_t>(role_)];
            hb.lastBeatNs.store(nowNs(), std::memory_order_release);
            hb.beatCount.fetch_add(1, std::memory_order_relaxed);
            hb.alive.store(1, std::memory_order_release);
            for (int waited = 0; waited < periodMs && heartbeatRunning_.load(); waited += 20) {
                sleepMs(20);
            }
        }
    });
}

void ProcessApp::stopHeartbeat() {
    if (!heartbeatRunning_.exchange(false)) return;
    if (heartbeatThread_.joinable()) heartbeatThread_.join();
}

void ProcessApp::updateHeartbeat() {
    ControlBlock* ctl = control();
    if (!ctl) return;
    Heartbeat& hb = ctl->hb[static_cast<uint32_t>(role_)];
    hb.lastBeatNs.store(nowNs(), std::memory_order_release);
    hb.beatCount.fetch_add(1, std::memory_order_relaxed);
}

void ProcessApp::markReady() {
    ControlBlock* ctl = control();
    if (!ctl) return;
    ctl->readyMask.fetch_or(roleBit(role_), std::memory_order_release);
    beginHeartbeat();
}

int ProcessApp::run() {
    std::string err;
    // ---- startup with retries -----------------------------------------------
    // A live front refuses logins outside its service window (lunch break,
    // night maintenance, front restart) and a stale flow file from the
    // previous trading day can hang the very first login of a new day.
    // Instead of exiting, keep retrying: the process stays up and connects
    // as soon as the front accepts logins again. Nothing (not even "no
    // signal / no ticks") ever terminates the process on its own.
    //   [general] startup_retry      = false -> fail fast (old behavior)
    //   [general] startup_retry_ms   = interval between attempts
    // Default: retry forever in live mode, fail fast in sim mode (a sim
    // startup error is a local configuration problem, retrying is pointless).
    const bool retryStartup = cfg_.getBool("general", "startup_retry", !simulation());
    const int retryMs = cfg_.getInt("general", "startup_retry_ms", 10000);
    int attempt = 0;
    while (!onStart(err)) {
        ++attempt;
        HFT_LOGE("startup failed (attempt %d): %s", attempt, err.c_str());
        onStop();
        if (!retryStartup) return 1;
        if (shouldStop() || globalStopFlag().load(std::memory_order_acquire)) return 1;
        HFT_LOGW("retrying startup in %d ms - the front may be closed (lunch break / "
                 "maintenance); staying up and waiting ...",
                 retryMs);
        for (int w = 0; w < retryMs; w += 100) {
            if (shouldStop() || globalStopFlag().load(std::memory_order_acquire)) return 1;
            sleepMs(100);
        }
        err.clear();
    }
    markReady();
    HFT_LOGI("%s running (pid=%u)", name_.c_str(),
             control()->hb[static_cast<uint32_t>(role_)].pid.load());

    applyPerformanceSettings();

    // Inbound channel doorbell events (collected once; live for the run).
    collectWaitHandles(waitHandles_);
    waitHandles_.erase(std::remove(waitHandles_.begin(), waitHandles_.end(), nullptr),
                       waitHandles_.end());
    // The block length on the kernel events is the process' timer cadence
    // (maxWaitNs), NOT a fixed poll interval: with no traffic the loop sleeps
    // the whole span at 0% CPU, and any doorbell rings it awake immediately.
    int64_t maxWait = maxWaitNs();
    if (maxWait <= 0) maxWait = 1000000ll;  // at least 1 ms
    const uint32_t waitMs =
        static_cast<uint32_t>(std::min<int64_t>(maxWait / 1000000ll, 60000ll));

    // ---- hybrid wait --------------------------------------------------------
    // Phase machine for the idle path (reset to phase 0 whenever onPoll()
    // does useful work):
    //   phase 0  tight spin, _mm_pause          [0, spin_us)   - only when
    //            spin_us > 0; the default (0) skips this entirely
    //   phase 1  block on the inbound channel events (waitMs timeout); a
    //            producer's SetEvent wakes us in microseconds; without
    //            events this degrades to a single timed sleep of waitMs.
    const int64_t spinNs = static_cast<int64_t>(spinUs_) * 1000;
    bool idle = false;
    int64_t idleStartNs = 0;
    while (!shouldStop() && !globalStopFlag().load(std::memory_order_acquire)) {
        if (control()->shutdown.load(std::memory_order_acquire) != 0) {
            HFT_LOGI("shutdown requested via control block");
            break;
        }
        const bool active = onPoll();
        if (active) {
            idle = false;
            continue;
        }
        if (!idle) {
            idle = true;
            idleStartNs = nowNs();
        }
        const int64_t idleNs = nowNs() - idleStartNs;
        if (idleNs < spinNs) {
            // Phase 0: burn a few microseconds for follow-up events (a second
            // tick usually lands while we are still here).
            for (int i = 0; i < 16; ++i) hftCpuRelax();
            continue;
        }

        // Phase 1: block until an inbound event rings or the timeout expires.
        if (!waitHandles_.empty()) {
#if defined(HFT_WINDOWS)
            const DWORD n = static_cast<DWORD>(waitHandles_.size());
            ::WaitForMultipleObjects(n, reinterpret_cast<const HANDLE*>(waitHandles_.data()),
                                     FALSE, waitMs);
#else
            sleepUs(static_cast<uint32_t>(std::min<int64_t>(maxWait, INT64_C(60000000)) /
                                          1000ll));
#endif
        } else {
            // No inbound events (e.g. the md process: its producer thread
            // lives inside the CTP DLL): plain timed sleep, whole milliseconds.
            sleepMs(waitMs);
        }
        idle = false;  // re-evaluate immediately after a wake
    }

    stopHeartbeat();
    onStop();
    if (control()) {
        control()->hb[static_cast<uint32_t>(role_)].alive.store(0, std::memory_order_release);
        control()->readyMask.fetch_and(~roleBit(role_), std::memory_order_release);
    }
    HFT_LOGI("%s stopped", name_.c_str());
    Logger::instance().flush();
    Logger::instance().close();
    return 0;
}

} // namespace hft
