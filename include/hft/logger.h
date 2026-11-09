// SPDX-License-Identifier: MIT
// logger.h - Process logger backed by fmtlog (https://github.com/MengRao/fmtlog).
//
// The public interface (HFT_LOGD/I/W/E, printf-style) is unchanged so every
// call site keeps working, but the transport is fmtlog's lock-free SPSC
// queue + background polling thread instead of a hand-rolled
// mutex/fprintf/fflush loop:
//
//   * write() only formats into a thread_local buffer and enqueues the line
//     (tens of ns, no mutex, no syscall on the hot path);
//   * a polling thread (10 ms) batches lines into a large memory buffer and
//     writes them to disk; Debug/Info hit disk within ~10 ms;
//   * Warn/Error trigger an immediate flush of the file buffer
//     (fmtlog::flushOn), so a crash right after an error still leaves the
//     reason on disk - the same guarantee the old inline-fflush gave;
//   * the on-disk line format is byte-identical to the old logger
//     ("[HH:MM:SS.mmm][INF][name] body") because guard.py, watch_log.bat,
//     rounds724.py and pnl_fifo.py all parse it. fmtlog's own header is
//     disabled via setHeaderPattern("").
//
// If the queue is full the message is dropped (FMTLOG_BLOCK=0 default)
// instead of blocking the trading path - the old logger could stall a tick
// on a slow disk, dropping is the lesser evil here.
#ifndef HFT_LOGGER_H
#define HFT_LOGGER_H

#include "hft/platform.h"

#include <string>

namespace hft {

enum class LogLevel { Debug = 0, Info = 1, Warn = 2, Error = 3, Silent = 4 };

class Logger {
public:
    static Logger& instance();

    // Opens <dir>/<name>.log (append) via fmtlog and starts its background
    // polling thread (10 ms cadence).
    bool init(const std::string& dir, const std::string& name, bool console = true);
    void setLevel(LogLevel level) { level_ = level; }
    void setConsole(bool on) { console_ = on; }
    LogLevel level() const { return level_; }
    bool consoleEnabled() const { return console_; }

    void write(LogLevel level, const char* fmt, ...);

    // Best-effort flush. While fmtlog's polling thread is running the caller
    // must not poll() itself, so this is a no-op in that case (messages land
    // within ~10 ms anyway, Warn/Error immediately). close() stops the
    // thread and force-flushes everything that remains.
    void flush();
    void close();

private:
    Logger();
    ~Logger();

    std::string name_;
    LogLevel level_ = LogLevel::Info;
    bool console_ = true;
    bool ready_ = false;      // fmtlog configured + polling thread running
};

const char* levelTag(LogLevel level);

} // namespace hft

#define HFT_LOG(level, ...) ::hft::Logger::instance().write(level, __VA_ARGS__)
#define HFT_LOGD(...) HFT_LOG(::hft::LogLevel::Debug, __VA_ARGS__)
#define HFT_LOGI(...) HFT_LOG(::hft::LogLevel::Info, __VA_ARGS__)
#define HFT_LOGW(...) HFT_LOG(::hft::LogLevel::Warn, __VA_ARGS__)
#define HFT_LOGE(...) HFT_LOG(::hft::LogLevel::Error, __VA_ARGS__)

#endif // HFT_LOGGER_H
