// SPDX-License-Identifier: MIT
// time_util.h - Monotonic clock, sleeping and timestamp helpers.
#ifndef HFT_TIME_UTIL_H
#define HFT_TIME_UTIL_H

#include "hft/platform.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>
#include <thread>

namespace hft {

// Monotonic nanoseconds since the first call (stable inside one process).
inline int64_t nowNs() {
    static const auto gStart = std::chrono::steady_clock::now();
    auto d = std::chrono::steady_clock::now() - gStart;
    return std::chrono::duration_cast<std::chrono::nanoseconds>(d).count();
}

// Monotonic timestamp with a host-wide steady-clock epoch. Use this for fields
// exchanged between processes; nowNs() deliberately has a process-local origin.
inline int64_t monotonicNowNs() {
    auto d = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(d).count();
}

// Absolute wall clock nanoseconds (epoch based). Comparable *across*
// processes - this is what must be used for cross-process latency accounting,
// while nowNs() (per process monotonic) is for local timeouts.
inline int64_t wallNowNs() {
    auto d = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(d).count();
}

inline int64_t nowUs() { return nowNs() / 1000; }
inline int64_t nowMs() { return nowNs() / 1000000; }

// Wall clock timestamp "HH:MM:SS.mmm" for logs.
inline std::string localTimeString() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(HFT_WINDOWS)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d", tm.tm_hour, tm.tm_min, tm.tm_sec,
                  static_cast<int>(ms.count()));
    return std::string(buf);
}

inline std::string localDateString() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(HFT_WINDOWS)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return std::string(buf);
}

inline void sleepMs(uint32_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
inline void sleepUs(uint32_t us) { std::this_thread::sleep_for(std::chrono::microseconds(us)); }

// "HH:MM:SS" -> seconds since midnight; -1 when unparsable.
inline int timeToSeconds(const char* hhmmss) {
    if (!hhmmss || std::strlen(hhmmss) < 8) return -1;
    int h = (hhmmss[0] - '0') * 10 + (hhmmss[1] - '0');
    int m = (hhmmss[3] - '0') * 10 + (hhmmss[4] - '0');
    int s = (hhmmss[6] - '0') * 10 + (hhmmss[7] - '0');
    if (h < 0 || h > 23 || m < 0 || m > 59 || s < 0 || s > 59) return -1;
    return h * 3600 + m * 60 + s;
}

} // namespace hft

#endif // HFT_TIME_UTIL_H
