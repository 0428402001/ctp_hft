// SPDX-License-Identifier: MIT
// channel.h - Named shared-memory channels between the four processes.
//
//   MD  --ticks-->   STRATEGY  --order commands-->  TRADER
//                       ^                              |
//                       |                         exec reports
//                       +----qry resp---- QUERY <--qry req--+
//
// Every arrow is a dedicated lock-free ring buffer in its own shared-memory
// region, so a slow consumer can never block a producer (it only drops when the
// buffer is really full, and that drop is counted).
#ifndef HFT_CHANNEL_H
#define HFT_CHANNEL_H

#include "hft/platform.h"
#include "hft/protocol.h"
#include "hft/ring_queue.h"
#include "hft/shm.h"
#include "hft/time_util.h"

#include <vector>

namespace hft {

// ---- capacities (power of two) --------------------------------------------
// 16k ticks ~= a few seconds of a busy future contract at 2 ticks/500ms; plenty
// of head room for a strategy that is stuck for a moment.
constexpr uint32_t kTickCapacity = 16384;
constexpr uint32_t kCmdCapacity = 2048;
constexpr uint32_t kExecCapacity = 8192;
constexpr uint32_t kQryReqCapacity = 512;
constexpr uint32_t kQryRspCapacity = 4096;

using TickChannel = RingQueue<TickMsg, kTickCapacity>;
using CmdChannel = RingQueue<OrderCommand, kCmdCapacity>;
using ExecChannel = RingQueue<ExecReport, kExecCapacity>;
// The trading process also mirrors every execution report to the query process
// so that it can keep the simulated account / position book up to date.
using ExecToQueryChannel = RingQueue<ExecReport, kExecCapacity>;
using QryReqChannel = RingQueue<QueryRequest, kQryReqCapacity>;
using QryRspChannel = RingQueue<QueryResponse, kQryRspCapacity>;

// ---- shared memory region names -------------------------------------------
constexpr const char* kShmControl = "HFT_SHM_CONTROL";
constexpr const char* kShmTicks = "HFT_SHM_TICKS";
constexpr const char* kShmCommands = "HFT_SHM_COMMANDS";
constexpr const char* kShmExecs = "HFT_SHM_EXECS";
constexpr const char* kShmExecsToQuery = "HFT_SHM_EXECS_Q";
constexpr const char* kShmQryReq = "HFT_SHM_QRY_REQ";
constexpr const char* kShmQryRsp = "HFT_SHM_QRY_RSP";
constexpr const char* kShmSnapshot = "HFT_SHM_SNAPSHOT";
constexpr const char* kShmMarket = "HFT_SHM_MARKET";

// ---------------------------------------------------------------------------
// Kernel wake-up event attached to a queue channel.
//
// The rings themselves are wait-free and never block anyone. This event is the
// optional "doorbell" on top: the producer claims a shared notification flag
// after publishing, and the consumer disarms it and rechecks the queue before
// WaitForMultipleObjects(). This closes the lost-wakeup window while
// coalescing a burst into one SetEvent. Result: prompt pickup
// of new messages while burning ~0% CPU while idle (a poll loop that sleeps
// in 1 ms ticks costs latency; one that spins costs a core; this costs
// neither). POSIX builds degrade to plain polling (no-op stubs).
//
// Auto-reset semantics: one SetEvent() permits exactly one wait to return;
// any leftover signal just causes one harmless spurious wakeup.
// ---------------------------------------------------------------------------
class ChannelEvent {
public:
    ChannelEvent() = default;
    ~ChannelEvent() { close(); }
    ChannelEvent(const ChannelEvent&) = delete;
    ChannelEvent& operator=(const ChannelEvent&) = delete;

    // Creates or opens the named auto-reset event (both sides use full access).
    bool open(const char* shmName, std::string& err) {
#if defined(HFT_WINDOWS)
        name_ = std::string("Local\\HFT_EVT_") + shmName;
        evt_ = ::CreateEventA(nullptr, FALSE, FALSE, name_.c_str());
        if (!evt_) {
            err = "CreateEvent(" + name_ + ") failed";
            return false;
        }
#else
        (void)shmName;
        (void)err;
#endif
        return true;
    }

    void close() {
#if defined(HFT_WINDOWS)
        if (evt_) {
            ::CloseHandle(evt_);
            evt_ = nullptr;
        }
#endif
    }

    // Producer side: wake the consumer.
    void notify() const {
#if defined(HFT_WINDOWS)
        if (evt_) ::SetEvent(evt_);
#endif
    }

    // Raw handle for WaitForMultipleObjects (nullptr on POSIX / unopened).
    void* handle() const {
#if defined(HFT_WINDOWS)
        return static_cast<void*>(evt_);
#else
        return nullptr;
#endif
    }

private:
#if defined(HFT_WINDOWS)
    HANDLE evt_ = nullptr;
    std::string name_;
#endif
};

// A bundle of all regions. Each process opens only what it needs via the
// dedicated helpers; opening everything is fine too (cheap).
class Channels {
public:
    bool openControl(bool create, std::string& err) {
        return openRegion(control_, kShmControl, create, err);
    }
    bool openTicks(bool create, std::string& err) {
        return openRegionAndEvent(ticks_, evtTicks_, kShmTicks, create, err);
    }
    bool openCommands(bool create, std::string& err) {
        return openRegionAndEvent(commands_, evtCommands_, kShmCommands, create, err);
    }
    bool openExecs(bool create, std::string& err) {
        return openRegionAndEvent(execs_, evtExecs_, kShmExecs, create, err);
    }
    bool openExecsToQuery(bool create, std::string& err) {
        return openRegionAndEvent(execsToQuery_, evtExecsToQuery_, kShmExecsToQuery, create, err);
    }
    bool openMarket(bool create, std::string& err) {
        return openRegion(market_, kShmMarket, create, err);
    }
    bool openQryReq(bool create, std::string& err) {
        return openRegionAndEvent(qryReq_, evtQryReq_, kShmQryReq, create, err);
    }
    bool openQryRsp(bool create, std::string& err) {
        return openRegionAndEvent(qryRsp_, evtQryRsp_, kShmQryRsp, create, err);
    }
    bool openSnapshot(bool create, std::string& err) {
        return openRegion(snapshot_, kShmSnapshot, create, err);
    }

    ControlBlock* control() const { return control_.get(); }
    TickChannel* ticks() const { return ticks_.get(); }
    CmdChannel* commands() const { return commands_.get(); }
    ExecChannel* execs() const { return execs_.get(); }
    ExecToQueryChannel* execsToQuery() const { return execsToQuery_.get(); }
    QryReqChannel* qryReq() const { return qryReq_.get(); }
    QryRspChannel* qryRsp() const { return qryRsp_.get(); }
    SnapshotBlock* snapshot() const { return snapshot_.get(); }
    MarketSnapshotBlock* market() const { return market_.get(); }

    // ---- event accessors (see ChannelEvent above) --------------------------
    ChannelEvent& ticksEvt() { return evtTicks_; }
    ChannelEvent& commandsEvt() { return evtCommands_; }
    ChannelEvent& execsEvt() { return evtExecs_; }
    ChannelEvent& execsToQueryEvt() { return evtExecsToQuery_; }
    ChannelEvent& qryReqEvt() { return evtQryReq_; }
    ChannelEvent& qryRspEvt() { return evtQryRsp_; }

    // ---- producer helpers: push + ring the doorbell -------------------------
    // Push and ring once until the consumer disarms the notification before
    // sleeping; a busy consumer pays no syscall per message.
    template <typename Q>
    static bool pushNotify(Q& queue, ChannelEvent& evt, const typename Q::value_type& v) {
        if (!queue.push(v)) return false;
        if (queue.claimNotification()) evt.notify();
        return true;
    }
    bool pushTick(const TickMsg& v) { return pushNotify(*ticks_, evtTicks_, v); }
    bool pushCommand(const OrderCommand& v) { return pushNotify(*commands_, evtCommands_, v); }
    bool pushExec(const ExecReport& v) { return pushNotify(*execs_, evtExecs_, v); }
    bool pushExecToQuery(const ExecReport& v) {
        return pushNotify(*execsToQuery_, evtExecsToQuery_, v);
    }
    bool pushQryReq(const QueryRequest& v) { return pushNotify(*qryReq_, evtQryReq_, v); }
    bool pushQryRsp(const QueryResponse& v) { return pushNotify(*qryRsp_, evtQryRsp_, v); }

    bool prepareTicksWait() { return ticks_->prepareToWait(); }
    bool prepareCommandsWait() { return commands_->prepareToWait(); }
    bool prepareExecsWait() { return execs_->prepareToWait(); }
    bool prepareExecsToQueryWait() { return execsToQuery_->prepareToWait(); }
    bool prepareQryReqWait() { return qryReq_->prepareToWait(); }
    bool prepareQryRspWait() { return qryRsp_->prepareToWait(); }

private:
    template <typename Region>
    static bool openRegion(Region& region, const char* name, bool create, std::string& err) {
        bool ok = create ? region.openOrCreate(name) : region.open(name);
        if (!ok) {
            err = std::string("cannot open shared memory '") + name + "': " + region.lastError();
            return false;
        }
        return true;
    }

    template <typename Region>
    static bool openRegionAndEvent(Region& region, ChannelEvent& evt, const char* name, bool create,
                                   std::string& err) {
        if (!openRegion(region, name, create, err)) return false;
        if (!evt.open(name, err)) return false;
        return true;
    }

    ShmRegion<ControlBlock> control_;
    ShmRegion<TickChannel> ticks_;
    ShmRegion<CmdChannel> commands_;
    ShmRegion<ExecChannel> execs_;
    ShmRegion<ExecToQueryChannel> execsToQuery_;
    ShmRegion<QryReqChannel> qryReq_;
    ShmRegion<QryRspChannel> qryRsp_;
    ShmRegion<SnapshotBlock> snapshot_;
    ShmRegion<MarketSnapshotBlock> market_;

    ChannelEvent evtTicks_;
    ChannelEvent evtCommands_;
    ChannelEvent evtExecs_;
    ChannelEvent evtExecsToQuery_;
    ChannelEvent evtQryReq_;
    ChannelEvent evtQryRsp_;
};

// ---------------------------------------------------------------------------
// Snapshot (seqlock) helpers
// ---------------------------------------------------------------------------

class SnapshotWriter {
public:
    explicit SnapshotWriter(SnapshotBlock* block) : block_(block) {}

    void begin() { block_->seq.fetch_add(1, std::memory_order_release); }  // odd = writing
    void end() {
        block_->updateTimeNs.store(nowNs(), std::memory_order_release);
        block_->queryRound.fetch_add(1, std::memory_order_relaxed);
        block_->seq.fetch_add(1, std::memory_order_release);  // even = stable
    }

    AccountSnap& account() { return block_->account; }
    PositionSnap* positions() { return block_->positions; }
    InstrumentSnap* instruments() { return block_->instruments; }
    void setCounts(uint32_t pos, uint32_t instr) {
        block_->posCount.store(pos, std::memory_order_relaxed);
        block_->instrCount.store(instr, std::memory_order_relaxed);
    }
    void setTradingDay(const char* day) { copyStr(block_->tradingDay, day); }

private:
    SnapshotBlock* block_;
};

// A consistent copy of the snapshot, obtained without taking any lock.
class SnapshotReader {
public:
    explicit SnapshotReader(SnapshotBlock* block) : block_(block) {}

    // Returns false if the writer was active and the copy had to be retried too
    // often (should not happen in practice).
    // Copies account + positions under the seqlock into a PRE-ALLOCATED array
    // (kMaxPositions entries, ~56 KiB): no vector, no per-read allocation on
    // the strategy hot path. Instruments are looked up on demand (see
    // findInstrument) so we never copy hundreds of kilobytes.
    bool read(int maxRetries = 8) {
        for (int i = 0; i < maxRetries; ++i) {
            uint32_t s1 = block_->seq.load(std::memory_order_acquire);
            if (s1 & 1u) continue;  // writer in progress
            account_ = block_->account;
            uint32_t n = block_->posCount.load(std::memory_order_relaxed);
            if (n > kMaxPositions) n = kMaxPositions;
            instrCount_ = block_->instrCount.load(std::memory_order_relaxed);
            if (instrCount_ > kMaxInstruments) instrCount_ = kMaxInstruments;
            std::memcpy(positions_, block_->positions, n * sizeof(PositionSnap));
            posCount_ = n;
            std::atomic_thread_fence(std::memory_order_acquire);
            uint32_t s2 = block_->seq.load(std::memory_order_acquire);
            if (s1 == s2) return true;
        }
        return false;
    }

    SnapshotBlock* block() const { return block_; }

    const AccountSnap& account() const { return account_; }
    uint32_t positionCount() const { return posCount_; }
    uint32_t instrumentCount() const { return instrCount_; }
    const PositionSnap* positions() const { return positions_; }

    int32_t positionOf(const char* instrument, char direction) const {
        int32_t total = 0;
        for (uint32_t i = 0; i < posCount_; ++i) {
            const PositionSnap& p = positions_[i];
            if (std::strncmp(p.instrument, instrument, kInstrumentLen) == 0 &&
                p.direction == direction) {
                total += p.position;
            }
        }
        return total;
    }

    // Reads a single instrument record under the seqlock (no full copy).
    bool findInstrument(const char* instrument, InstrumentSnap& out, int maxRetries = 8) const {
        for (int i = 0; i < maxRetries; ++i) {
            uint32_t s1 = block_->seq.load(std::memory_order_acquire);
            if (s1 & 1u) continue;
            uint32_t n = block_->instrCount.load(std::memory_order_relaxed);
            if (n > kMaxInstruments) n = kMaxInstruments;
            bool found = false;
            for (uint32_t k = 0; k < n; ++k) {
                if (std::strncmp(block_->instruments[k].instrument, instrument, kInstrumentLen) == 0) {
                    out = block_->instruments[k];
                    found = true;
                    break;
                }
            }
            std::atomic_thread_fence(std::memory_order_acquire);
            uint32_t s2 = block_->seq.load(std::memory_order_acquire);
            if (s1 == s2) return found;
        }
        return false;
    }

private:
    SnapshotBlock* block_;
    AccountSnap account_{};
    uint32_t instrCount_ = 0;
    uint32_t posCount_ = 0;
    PositionSnap positions_[kMaxPositions];  // pre-allocated, see read()
};

} // namespace hft

#endif // HFT_CHANNEL_H
