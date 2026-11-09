// SPDX-License-Identifier: MIT
// logger.cpp - hft::Logger on top of fmtlog.
//
// All printf-style formatting still happens here in the caller thread (into
// a thread_local buffer, exactly as before); fmtlog then owns the disk
// writes. The complete line is handed to fmtlog as a single string argument
// with an empty header pattern, so the bytes on disk are identical to the
// old logger and every log-parsing tool keeps working.
#include "hft/logger.h"

#include "hft/time_util.h"

#include "fmtlog.h"

#include <cstdarg>
#include <cstdio>

#if defined(HFT_WINDOWS)
#  include <direct.h>
#else
#  include <sys/stat.h>
#  include <sys/types.h>
#endif

namespace hft {

const char* levelTag(LogLevel level) {
    switch (level) {
    case LogLevel::Debug: return "DBG";
    case LogLevel::Info: return "INF";
    case LogLevel::Warn: return "WRN";
    case LogLevel::Error: return "ERR";
    default: return "---";
    }
}

static fmtlog::LogLevel toFmtlogLevel(LogLevel level) {
    switch (level) {
    case LogLevel::Debug: return fmtlog::DBG;
    case LogLevel::Info: return fmtlog::INF;
    case LogLevel::Warn: return fmtlog::WRN;
    default: return fmtlog::ERR;
    }
}

Logger::Logger() = default;

Logger::~Logger() { close(); }

Logger& Logger::instance() {
    static Logger gLogger;
    return gLogger;
}

static void ensureDir(const std::string& dir) {
    if (dir.empty()) return;
#if defined(HFT_WINDOWS)
    ::_mkdir(dir.c_str());
#else
    ::mkdir(dir.c_str(), 0775);
#endif
}

bool Logger::init(const std::string& dir, const std::string& name, bool console) {
    // Re-init (only used by tests): stop the polling thread outside first.
    if (ready_) close();

    console_ = console;
    name_ = name;
    if (dir.empty()) {
        // No file: messages go to fmtlog's default stdout sink. Still useful
        // for tests; console flag is irrelevant then.
        fmtlog::setLogLevel(fmtlog::DBG);
        fmtlog::setHeaderPattern("");
        fmtlog::flushOn(fmtlog::WRN);
        fmtlog::startPollingThread(10 * 1000 * 1000);
        ready_ = true;
        return true;
    }

    ensureDir(dir);
    const std::string path = dir + "/" + name + ".log";

    // fmtlog accepts everything at runtime; hft-side level_ does the actual
    // filtering (setLevel is process-specific, not a fmtlog concern).
    fmtlog::setLogLevel(fmtlog::DBG);
    // The header (timestamp / level / thread) is built by write() itself so
    // the on-disk format stays "[HH:MM:SS.mmm][TAG][name] body".
    fmtlog::setHeaderPattern("");
    // Warn/Error flush the file buffer during the same poll cycle - the old
    // logger's "errors must survive a crash" guarantee, now asynchronous.
    fmtlog::flushOn(fmtlog::WRN);
    // Batch: flush at most 50 ms after the oldest buffered line, or once the
    // output buffer reaches 64 KB.
    fmtlog::setFlushDelay(50 * 1000 * 1000);
    fmtlog::setFlushBufSize(64 * 1024);
    // Append, never truncate: log files double as the audit trail (pnl_fifo,
    // rounds724 rebuild whole days from them).
    fmtlog::setLogFile(path.c_str(), false);
    fmtlog::startPollingThread(10 * 1000 * 1000);  // 10 ms cadence
    ready_ = true;
    return true;
}

void Logger::write(LogLevel level, const char* fmt, ...) {
    if (level < level_) return;

    static thread_local char body[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

    static thread_local char line[2304];
    // No trailing '\n': fmtlog terminates each record itself.
    const int n = std::snprintf(line, sizeof(line), "[%s][%s][%s] %s", localTimeString().c_str(),
                                levelTag(level), name_.c_str(), body);
    if (n <= 0) return;

    if (console_) {
        // stderr keeps stdout free for the process' own status output.
        const size_t len = static_cast<size_t>(
            n < static_cast<int>(sizeof(line)) ? n : static_cast<int>(sizeof(line)));
        std::fwrite(line, 1, len, stderr);
        std::fputc('\n', stderr);
        std::fflush(stderr);
    }
    if (!ready_) return;

    // Enqueue only; formatting of "{}" + the disk write happen on fmtlog's
    // polling thread. A plain const char* is recognized as a cstring and
    // copied synchronously into the SPSC queue, so the thread_local buffer
    // can be reused right after the call.
    const char* cstr = line;
    switch (toFmtlogLevel(level)) {
    case fmtlog::DBG: FMTLOG(fmtlog::DBG, "{}", cstr); break;
    case fmtlog::INF: FMTLOG(fmtlog::INF, "{}", cstr); break;
    case fmtlog::WRN: FMTLOG(fmtlog::WRN, "{}", cstr); break;
    default: FMTLOG(fmtlog::ERR, "{}", cstr); break;
    }
}

void Logger::flush() {
    // With the polling thread running, callers must not poll() themselves;
    // the thread covers this within ~10 ms (immediately for Warn/Error).
}

void Logger::close() {
    if (!ready_) return;
    // Stop the thread first; only then is it safe to force-flush the rest.
    fmtlog::stopPollingThread();
    fmtlog::poll(true);
    fmtlog::closeLogFile();
    ready_ = false;
}

} // namespace hft
