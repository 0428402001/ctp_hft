// SPDX-License-Identifier: MIT
// ring_queue.h - Wait-free single-producer / single-consumer ring buffer.
#ifndef HFT_RING_QUEUE_H
#define HFT_RING_QUEUE_H

#include "hft/platform.h"

// Design notes
// ------------
// * The object is POD-friendly: it is always placed inside shared memory, so it
//   must not contain virtual functions, pointers to heap memory, or any state
//   that is meaningful only in one address space.
// * `writeIndex_` / `readIndex_` are monotonically increasing 64 bit counters.
//   The slot is selected with `index & kMask`, therefore Capacity MUST be a
//   power of two. Monotonic counters make "full" and "empty" unambiguous
//   without sacrificing a slot.
// * Memory ordering: the producer publishes data with a release store on
//   writeIndex_ after writing the slot; the consumer acquires writeIndex_
//   before reading the slot. Same idea in the other direction for readIndex_.
//   That is the classic, provably-correct SPSC protocol - no CAS, no locks,
//   no ABA problem.
template <typename T, uint32_t Capacity>
class RingQueue {
    static_assert(Capacity >= 2u, "capacity must be >= 2");
    static_assert((Capacity & (Capacity - 1u)) == 0u, "capacity must be a power of two");
    static_assert(std::is_trivially_copyable<T>::value, "T must be trivially copyable");

public:
    using value_type = T;
    static constexpr uint32_t kCapacity = Capacity;
    static constexpr uint32_t kMask = Capacity - 1u;

    // ---- producer side -----------------------------------------------------

    // Returns false when the queue is full (the caller decides to drop/retry).
    bool push(const T& value) {
        const uint64_t w = writeIndex_.load(std::memory_order_relaxed);
        const uint64_t r = readIndex_.load(std::memory_order_acquire);
        if (HFT_UNLIKELY(w - r >= Capacity)) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        slots_[static_cast<uint32_t>(w) & kMask] = value;
        writeIndex_.store(w + 1u, std::memory_order_release);
        pushed_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // Producer side of the event-doorbell handshake. Only the first push after
    // the consumer arms its wait needs to ring the kernel event.
    bool claimNotification() {
        return notificationArmed_.exchange(1u, std::memory_order_acq_rel) == 0u;
    }

    // Consumer side: disarm, then recheck the queue before blocking. The
    // exchange pairs with a producer that already claimed the notification;
    // that producer's queue publication is then visible to the empty() check.
    // False means work arrived during the handoff and the caller should poll
    // again instead of sleeping.
    bool prepareToWait() {
        notificationArmed_.exchange(0u, std::memory_order_acq_rel);
        return empty();
    }

    // ---- consumer side -----------------------------------------------------

    bool pop(T& out) {
        const uint64_t r = readIndex_.load(std::memory_order_relaxed);
        const uint64_t w = writeIndex_.load(std::memory_order_acquire);
        if (HFT_UNLIKELY(w == r)) return false;
        out = slots_[static_cast<uint32_t>(r) & kMask];
        readIndex_.store(r + 1u, std::memory_order_release);
        popped_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // Drains up to `max` items. Returns the number of items copied to `out`.
    uint32_t popBulk(T* out, uint32_t max) {
        const uint64_t r = readIndex_.load(std::memory_order_relaxed);
        const uint64_t w = writeIndex_.load(std::memory_order_acquire);
        uint64_t n = w - r;
        if (n == 0) return 0;
        if (n > max) n = max;
        for (uint64_t i = 0; i < n; ++i) {
            out[i] = slots_[static_cast<uint32_t>(r + i) & kMask];
        }
        readIndex_.store(r + n, std::memory_order_release);
        popped_.fetch_add(n, std::memory_order_relaxed);
        return static_cast<uint32_t>(n);
    }

    // Peek without consuming (index 0 == oldest). Returns nullptr if empty.
    const T* peek(uint32_t offset = 0) const {
        const uint64_t r = readIndex_.load(std::memory_order_relaxed);
        const uint64_t w = writeIndex_.load(std::memory_order_acquire);
        if (offset >= (w - r)) return nullptr;
        return &slots_[static_cast<uint32_t>(r + offset) & kMask];
    }

    // ---- observers ---------------------------------------------------------

    uint64_t size() const {
        const uint64_t w = writeIndex_.load(std::memory_order_acquire);
        const uint64_t r = readIndex_.load(std::memory_order_acquire);
        return (w >= r) ? (w - r) : 0;
    }
    bool empty() const { return size() == 0; }
    bool full() const { return size() >= Capacity; }

    uint64_t pushed() const { return pushed_.load(std::memory_order_relaxed); }
    uint64_t popped() const { return popped_.load(std::memory_order_relaxed); }
    uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }
    uint64_t writeIndex() const { return writeIndex_.load(std::memory_order_relaxed); }
    uint64_t readIndex() const { return readIndex_.load(std::memory_order_relaxed); }

    // Only valid before any concurrent access (used by the creator process).
    void reset() {
        writeIndex_.store(0, std::memory_order_relaxed);
        readIndex_.store(0, std::memory_order_relaxed);
        pushed_.store(0, std::memory_order_relaxed);
        popped_.store(0, std::memory_order_relaxed);
        dropped_.store(0, std::memory_order_relaxed);
        notificationArmed_.store(0u, std::memory_order_relaxed);
    }

private:
    // Producer-owned cache line.
    alignas(HFT_CACHELINE) std::atomic<uint64_t> writeIndex_{0};
    alignas(HFT_CACHELINE) std::atomic<uint64_t> pushed_{0};
    alignas(HFT_CACHELINE) std::atomic<uint64_t> dropped_{0};

    // Consumer-owned cache line.
    alignas(HFT_CACHELINE) std::atomic<uint64_t> readIndex_{0};
    alignas(HFT_CACHELINE) std::atomic<uint64_t> popped_{0};
    alignas(HFT_CACHELINE) std::atomic<uint32_t> notificationArmed_{0};

    // The payload itself, page aligned: the header above is padded out to a
    // full 4 KiB, so slot 0 lands exactly on a page boundary in the mapped
    // file view (MapViewOfFile bases are 64 KiB aligned). Combined with the
    // 256/512 B slot sizes in protocol.h a linear walk of the ring touches a
    // new page only every 8-16 slots instead of straddling two pages per
    // slot - one TLB entry per page, no split-slot double faults.
    alignas(4096) T slots_[Capacity];
};

#endif // HFT_RING_QUEUE_H
