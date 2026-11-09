// SPDX-License-Identifier: MIT
// platform.h - Cross-platform primitives for the HFT framework (Windows first).
#ifndef HFT_PLATFORM_H
#define HFT_PLATFORM_H

#if defined(_WIN32) || defined(_WIN64)
#  define HFT_WINDOWS 1
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <intrin.h>
#else
#  define HFT_POSIX 1
#  include <unistd.h>
#  include <sys/mman.h>
#  include <fcntl.h>
#endif

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <type_traits>

// Cache line size used for padding shared-memory hot fields.
#define HFT_CACHELINE 64

// Branch prediction hints.
#if defined(__GNUC__) || defined(__clang__)
#  define HFT_LIKELY(x)   __builtin_expect(!!(x), 1)
#  define HFT_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#  define HFT_LIKELY(x)   (x)
#  define HFT_UNLIKELY(x) (x)
#endif

// CPU relax / pause instruction - used by spin loops.
inline void hftCpuRelax() {
#if defined(HFT_WINDOWS)
    _mm_pause();
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#else
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

// Yield current thread to the OS scheduler.
inline void hftThreadYield() {
#if defined(HFT_WINDOWS)
    SwitchToThread();
#else
    sched_yield();
#endif
}

namespace hft {

// Full memory fence (used rarely, e.g. at shutdown).
inline void fence() { std::atomic_thread_fence(std::memory_order_seq_cst); }

} // namespace hft

#endif // HFT_PLATFORM_H
