// SPDX-License-Identifier: MIT
// process_app.h - Common skeleton shared by all four processes.
//
// Every process does the same boring things: parse arguments, load the INI,
// attach to (or create) the shared-memory channels, start a heartbeat, run a
// tight poll loop until the control block says "shutdown". Only the interesting
// parts are virtual.
#ifndef HFT_PROCESS_APP_H
#define HFT_PROCESS_APP_H

#include "hft/channel.h"
#include "hft/config.h"
#include "hft/protocol.h"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace hft {

class ProcessApp {
public:
    ProcessApp(Role role, const char* name);
    virtual ~ProcessApp();

    // Parses argv, loads the config, opens the channels, starts logging.
    bool init(int argc, char** argv);

    // onStart() -> loop { onPoll(); heartbeat(); } -> onStop()
    int run();

    // ---- accessors for subclasses -----------------------------------------
    Role role() const { return role_; }
    const std::string& processName() const { return name_; }
    Config& config() { return cfg_; }
    const Config& config() const { return cfg_; }
    Channels& channels() { return ch_; }
    ControlBlock* control() { return ch_.control(); }
    bool simulation() const { return sim_; }
    bool shouldStop() const { return stopFlag_.load(std::memory_order_acquire); }
    void requestStop() { stopFlag_.store(true, std::memory_order_release); }

    // Marks this role as ready in the control block and starts the heartbeat.
    void markReady();

    // ---- signal handling ---------------------------------------------------
    static void installConsoleHandler();
    static std::atomic<bool>& globalStopFlag();

    void updateHeartbeat();

protected:
    // ---- hooks -------------------------------------------------------------
    // Attach to the channels this process needs (create == first process up).
    virtual bool attachChannels(std::string& err) = 0;
    // Called once after everything is attached.
    virtual bool onStart(std::string& err) = 0;
    // Called repeatedly. Returns true when it did useful work; when it returns
    // false the loop enters the hybrid wait below instead of burning a core.
    virtual bool onPoll() = 0;
    // Called once before exit (also on error paths).
    virtual void onStop() = 0;

    virtual int idleSleepUs() const { return 1000; }

    // ---- event-driven wake-up ------------------------------------------------
    // Each process lists the kernel events of its INBOUND channels here; the
    // idle path of run() blocks on them (WaitForMultipleObjects) instead of
    // sleeping on a timer. Producers ring the doorbell on empty -> non-empty
    // (see ChannelEvent). Default: no events -> plain timed sleep.
    // Handles are collected once after onStart().
    virtual void collectWaitHandles(std::vector<void*>& handles) { (void)handles; }

    // How long the idle path may block on the inbound events before polling
    // again anyway - the ONLY source of periodic wake-ups. Override with the
    // cadence of the process' own timer work (e.g. the strategy's 1 s
    // onTimer), so the loop is purely event driven between timer ticks:
    // a message wakes it in microseconds, otherwise it sleeps the full span.
    virtual int64_t maxWaitNs() const { return 1000000000ll; }

    // ---- performance setup -------------------------------------------------
    // Called from run() before the poll loop starts:
    //   * timeBeginPeriod(1)            - Windows sleep granularity 15 ms -> ~1 ms
    //   * HIGH_PRIORITY_CLASS           - [general] high_priority (default on)
    //   * main thread CPU pinning       - [general] cpu / <role section> cpu
    //   * hybrid spin wait parameters   - [general] spin_us (default 200)
    void applyPerformanceSettings();

private:
    void printUsage() const;
    bool parseArgs(int argc, char** argv, std::string& err);
    void beginHeartbeat();
    void stopHeartbeat();

    Role role_;
    std::string name_;
    Config cfg_;
    Channels ch_;
    bool sim_ = true;
    bool consoleLog_ = true;
    std::atomic<bool> stopFlag_{false};
    std::atomic<bool> heartbeatRunning_{false};
    std::thread heartbeatThread_;

    // Hybrid wait parameters (see process_app.cpp for the phase machine).
    int spinUs_ = 200;      // tight-spin phase length (pause instruction)
    int idleUs_ = 1000;     // final sleep length once fully idle
    std::vector<void*> waitHandles_;  // inbound channel events (kernel objects)
};

} // namespace hft

#endif // HFT_PROCESS_APP_H
