// SPDX-License-Identifier: MIT
// thread_pin.h - Pin a thread to one CPU core, once, from inside a callback
// whose thread we do not own (the CTP SDK spawns its own worker threads).
#ifndef HFT_THREAD_PIN_H
#define HFT_THREAD_PIN_H

#include "hft/platform.h"

#include <atomic>

namespace hft {

// Pins the CALLING thread to `cpu` exactly once. `state` is a shared atomic
// marker owned by the caller (one per thread to pin):
//   0 -> not attempted yet, 1 -> someone is inside, 2 -> done.
// cpu < 0 or cpu >= 64 disables pinning (counts as done).
// A failed SetThreadAffinityMask leaves state at 0 so the next callback
// retries (a transient failure must not silently give up).
inline bool pinCurrentThreadOnce(int cpu, std::atomic<int>& state) {
    int expected = 0;
    if (!state.compare_exchange_strong(expected, 1, std::memory_order_acquire)) {
        return true;  // already done (or another callback is pinning)
    }
    if (cpu < 0 || cpu >= 64) {
        state.store(2, std::memory_order_release);
        return true;
    }
#if defined(HFT_WINDOWS)
    if (::SetThreadAffinityMask(::GetCurrentThread(), (DWORD_PTR(1) << cpu)) != 0) {
        state.store(2, std::memory_order_release);
        return true;
    }
#else
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (::pthread_setaffinity_np(::pthread_self(), sizeof(set), &set) == 0) {
        state.store(2, std::memory_order_release);
        return true;
    }
#endif
    state.store(0, std::memory_order_release);  // retry on the next callback
    return false;
}

} // namespace hft

#endif // HFT_THREAD_PIN_H
