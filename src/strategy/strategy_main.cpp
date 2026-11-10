// SPDX-License-Identifier: MIT
// strategy_main.cpp - Strategy process.
//
//   ticks  -> strategy.onTick()  -> order commands -> trader
//   execs  -> strategy.onExec()  -> internal position book
//   shared snapshot (from the query process) -> available money / reconciliation
//
// The strategy keeps its own position book because it must react in microseconds,
// while the query process' snapshot is the slower "official" truth used for
// reconciliation and for risk limits.
#include "hft/process_app.h"
#include "hft/channel.h"
#include "hft/logger.h"
#include "hft/strategy_base.h"
#include "hft/time_util.h"

#include "obi_voi_strategy.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace hft {

// ---------------------------------------------------------------------------
// Context implementation: translates strategy requests into ring buffer writes
// and keeps the strategy side position book.
//
// Hot-path rule: NO dynamic allocation and NO std::string construction in
// anything reachable from a tick. All lookup tables are fixed-size arrays of
// 16 slots with a linear scan by instrument name - a strategy trades a
// handful of instruments, so 2-3 strncmp()s beat a std::map node chase and
// can never allocate.
// ---------------------------------------------------------------------------
class StrategyContextImpl : public StrategyContext {
public:
    // Max instruments the strategy can touch in one run (config lists 1-4).
    static constexpr uint32_t kHotMax = 16;

    struct TickSlot { char instrument[kInstrumentLen]; TickMsg tick; };
    struct PosBook {
        int net = 0;
        double avg = 0.0;
        double realized = 0.0;
    };
    struct PosSlot { char instrument[kInstrumentLen]; PosBook book; };
    struct InstSlot { char instrument[kInstrumentLen]; InstrumentSnap snap; bool valid; };
    struct BrokerSlot { char instrument[kInstrumentLen]; BrokerPosition bp; };

    StrategyContextImpl(CmdChannel* commands, ChannelEvent* commandsEvt, ControlBlock* control,
                        MarketSnapshotBlock* market, SnapshotBlock* snapshot)
        : commands_(commands), commandsEvt_(commandsEvt), control_(control), market_(market),
          reader_(snapshot) {}

    // Linear find-or-insert over a fixed slot table. insert=false never
    // grows the table (read-only lookups for unknown instruments).
    template <typename Slot, uint32_t N>
    static Slot* findSlot(Slot (&arr)[N], uint32_t& count, const char* instrument,
                          bool insert) {
        for (uint32_t i = 0; i < count; ++i) {
            if (std::strncmp(arr[i].instrument, instrument, kInstrumentLen) == 0) return &arr[i];
        }
        if (!insert || count >= N) return nullptr;
        Slot& s = arr[count++];
        s = Slot();  // value-init: zero the name, keep member initializers
        copyStr(s.instrument, instrument);
        return &s;
    }

    uint32_t insert(const char* instrument, Dir direction, Offset offset, PriceType priceType,
                    double price, int volume, uint64_t tag) override {
        return insert(instrument, direction, offset, priceType, price, volume, tag, '1');
    }

    uint32_t insert(const char* instrument, Dir direction, Offset offset, PriceType priceType,
                    double price, int volume, uint64_t tag, char timeCondition) override {
        if (!commands_) return 0;
        OrderCommand cmd;
        std::memset(&cmd, 0, sizeof(cmd));
        cmd.magic = kMagicCmd;
        cmd.cmdId = ++cmdId_;
        cmd.type = CmdType::Insert;
        copyStr(cmd.instrument, instrument);
        cmd.direction = static_cast<char>(direction);
        cmd.offsetFlag = static_cast<char>(offset);
        cmd.hedgeFlag = '1';
        cmd.priceType = static_cast<char>(priceType);
        cmd.price = price;
        cmd.volume = volume;
        cmd.minVolume = 1;
        cmd.timeCondition = timeCondition ? timeCondition : '1';  // '1' GFD, '3' IOC
        cmd.volumeCondition = '1'; // any volume
        cmd.strategyTag = tag;
        cmd.sendTimeNs = nowNs();
        const bool wasEmptyBeforePush = commands_->empty();
        if (!commands_->push(cmd)) {
            --cmdId_;
            return 0;
        }
        // doorbell: wake the trader process (only on empty -> non-empty)
        if (commandsEvt_ && wasEmptyBeforePush) commandsEvt_->notify();
        return cmd.cmdId;
    }

    uint32_t cancel(const ExecReport& order, uint64_t tag) override {
        if (!commands_) return 0;
        OrderCommand cmd;
        std::memset(&cmd, 0, sizeof(cmd));
        cmd.magic = kMagicCmd;
        cmd.cmdId = ++cmdId_;
        cmd.type = CmdType::Cancel;
        copyStr(cmd.instrument, order.instrument);
        copyStr(cmd.orderRef, order.orderRef);
        copyStr(cmd.orderSysID, order.orderSysID);
        cmd.frontID = order.frontID;
        cmd.sessionID = order.sessionID;
        cmd.strategyTag = tag;
        cmd.sendTimeNs = nowNs();
        const bool wasEmptyBeforePush = commands_->empty();
        if (!commands_->push(cmd)) {
            --cmdId_;
            return 0;
        }
        // doorbell: wake the trader process (only on empty -> non-empty)
        if (commandsEvt_ && wasEmptyBeforePush) commandsEvt_->notify();
        return cmd.cmdId;
    }

    uint32_t cancelAll(const char* instrument, uint64_t tag) override {
        if (!commands_) return 0;
        OrderCommand cmd;
        std::memset(&cmd, 0, sizeof(cmd));
        cmd.magic = kMagicCmd;
        cmd.cmdId = ++cmdId_;
        cmd.type = CmdType::CancelAll;
        copyStr(cmd.instrument, instrument);
        cmd.strategyTag = tag;
        cmd.sendTimeNs = nowNs();
        const bool wasEmptyBeforePush = commands_->empty();
        if (!commands_->push(cmd)) {
            --cmdId_;
            return 0;
        }
        // doorbell: wake the trader process (only on empty -> non-empty)
        if (commandsEvt_ && wasEmptyBeforePush) commandsEvt_->notify();
        return cmd.cmdId;
    }

    const TickMsg* lastTick(const char* instrument) const override {
        const TickSlot* s = findSlot(tickSlots_, tickSlotsN_, instrument, false);
        return s ? &s->tick : nullptr;
    }

    double lastPrice(const char* instrument) const override {
        const TickMsg* t = lastTick(instrument);
        if (t) return t->lastPrice;
        const MarketTickSnap* s = marketSnap(instrument);
        return s ? s->lastPrice : 0.0;
    }

    double bidPrice(const char* instrument) const override {
        const TickMsg* t = lastTick(instrument);
        if (t && t->bidPrice1 > 0) return t->bidPrice1;
        const MarketTickSnap* s = marketSnap(instrument);
        return s ? s->bidPrice1 : 0.0;
    }

    double askPrice(const char* instrument) const override {
        const TickMsg* t = lastTick(instrument);
        if (t && t->askPrice1 > 0) return t->askPrice1;
        const MarketTickSnap* s = marketSnap(instrument);
        return s ? s->askPrice1 : 0.0;
    }

    double priceTick(const char* instrument) const override {
        const InstrumentSnap* s = instrumentOf(instrument);
        return (s && s->priceTick > 0) ? s->priceTick : fallbackTick_;
    }

    int volumeMultiple(const char* instrument) const override {
        const InstrumentSnap* s = instrumentOf(instrument);
        return (s && s->volumeMultiple > 0) ? s->volumeMultiple : fallbackMultiple_;
    }

    int position(const char* instrument) const override {
        const PosSlot* s = findSlot(posSlots_, posSlotsN_, instrument, false);
        return s ? s->book.net : 0;
    }

    double openPrice(const char* instrument) const override {
        const PosSlot* s = findSlot(posSlots_, posSlotsN_, instrument, false);
        return s ? s->book.avg : 0.0;
    }

    double realizedPnl(const char* instrument) const override {
        const PosSlot* s = findSlot(posSlots_, posSlotsN_, instrument, false);
        return s ? s->book.realized : 0.0;
    }

    bool snapshotReady() const override { return snapshotReady_; }

    uint32_t snapshotQueryRound() const override { return snapshotRound_; }

    bool brokerPosition(const char* instrument, BrokerPosition& out) const override {
        const BrokerSlot* s = findSlot(brokerSlots_, brokerSlotsN_, instrument, false);
        if (!s) return false;
        out = s->bp;
        return true;
    }

    void adoptPosition(const char* instrument, int net, double price) override {
        PosSlot* s = findSlot(posSlots_, posSlotsN_, instrument, true);
        if (!s) return;  // table full (16 instruments) - logged upstream
        s->book.net = net;
        s->book.avg = price;
    }

    double available() const override { return available_; }
    bool tradingEnabled() const override {
        return control_ && control_->tradingEnabled.load(std::memory_order_acquire) != 0;
    }
    void haltTrading() override {
        if (control_) control_->tradingEnabled.store(0, std::memory_order_release);
    }

    // ---- bookkeeping driven by the process loop ----------------------------
    void setTick(const TickMsg& t) {
        TickSlot* s = findSlot(tickSlots_, tickSlotsN_, t.instrument, true);
        if (s) s->tick = t;
    }

    void applyExec(const ExecReport& r) {
        if (r.kind != ExecType::TradeRtn || r.tradeVolume <= 0) return;
        PosSlot* s = findSlot(posSlots_, posSlotsN_, r.instrument, true);
        if (!s) return;
        PosBook& p = s->book;
        const bool buy = (r.direction == static_cast<char>(Dir::Buy));
        const bool open = (r.offsetFlag == static_cast<char>(Offset::Open));
        const double px = r.tradePrice > 0 ? r.tradePrice : r.price;
        const int vol = r.tradeVolume;
        const double mult = static_cast<double>(volumeMultiple(r.instrument));

        if (open) {
            const int signedVol = buy ? vol : -vol;
            const int newNet = p.net + signedVol;
            if ((p.net >= 0 && signedVol > 0) || (p.net <= 0 && signedVol < 0)) {
                // adding in the same direction: moving average of the entry price
                p.avg = (p.avg * std::abs(p.net) + px * vol) / static_cast<double>(std::abs(newNet));
            } else {
                // reversing through an opposite open: keep it simple, reset avg
                p.avg = px;
            }
            p.net = newNet;
        } else {
            const int signedVol = buy ? vol : -vol;
            if ((p.net > 0 && signedVol < 0) || (p.net < 0 && signedVol > 0)) {
                const double pnl = (p.net > 0) ? (px - p.avg) : (p.avg - px);
                p.realized += pnl * vol * mult;
            }
            p.net += signedVol;
            if (p.net == 0) p.avg = 0.0;
        }
    }

    // Refresh account / instrument data from the query process' snapshot.
    void syncSnapshot() {
        if (!reader_.read()) return;
        available_ = reader_.account().available;
        // Refresh instrument metadata (tick size, contract multiplier).
        SnapshotBlock* blk = reader_.block();
        if (!blk) return;
        if (blk->updateTimeNs.load(std::memory_order_acquire) != 0) snapshotReady_ = true;
        snapshotRound_ = blk->queryRound.load(std::memory_order_acquire);

        // Rebuild the broker side position view (long/short x today/yesterday).
        // The per-record TodayPosition/YdPosition fields are the reliable way
        // to split the buckets: some counters return one aggregated row where
        // PositionDate says "today" but the row actually spans both days.
        brokerSlotsN_ = 0;
        const PositionSnap* snaps = reader_.positions();
        for (uint32_t si = 0; si < reader_.positionCount(); ++si) {
            const PositionSnap& p = snaps[si];
            if (p.instrument[0] == '\0') continue;
            if (p.position <= 0) continue;
            HFT_LOGD("pos record %s dir=%c date=%c pos=%d today=%d yd=%d", p.instrument,
                     p.direction, p.positionDate, p.position, p.todayPosition, p.ydPosition);
            BrokerSlot* slot = findSlot(brokerSlots_, brokerSlotsN_, p.instrument, true);
            if (!slot) break;  // table full; more instruments than a strategy can trade
            BrokerPosition& b = slot->bp;
            int today = 0;
            int yd = 0;
            if (p.todayPosition >= 0 && p.ydPosition >= 0 &&
                p.todayPosition + p.ydPosition >= p.position) {
                // Trust the explicit split (clamp to the reported total).
                today = std::min(p.todayPosition, p.position);
                yd = std::min(p.ydPosition, p.position - today);
            } else {
                // Fallback: the row's date bucket.
                today = (p.positionDate == '1') ? p.position : 0;
                yd = p.position - today;
            }
            if (p.direction == '2') {          // long
                b.longToday += today;
                b.longYd += yd;
            } else if (p.direction == '3') {   // short
                b.shortToday += today;
                b.shortYd += yd;
            }
        }
        for (uint32_t i = 0; i < brokerSlotsN_; ++i) {
            BrokerSlot& slot = brokerSlots_[i];
            slot.bp.net = slot.bp.longToday + slot.bp.longYd - slot.bp.shortToday -
                          slot.bp.shortYd;
            HFT_LOGD("broker position %s: long %d(today)+%d(yd), short %d(today)+%d(yd) -> net=%d",
                     slot.instrument, slot.bp.longToday, slot.bp.longYd,
                     slot.bp.shortToday, slot.bp.shortYd, slot.bp.net);
        }

        // Refresh the instrument metadata cache (tick size / multiplier) for
        // the instruments the strategy actually looks up - the full 4000-row
        // snapshot table is never copied, entries fill lazily in instrumentOf().
        for (uint32_t i = 0; i < instSlotsN_; ++i) {
            InstrumentSnap s;
            if (reader_.findInstrument(instSlots_[i].instrument, s)) instSlots_[i].snap = s;
        }
    }

    void setFallbackInstrumentInfo(double tick, int multiple) {
        fallbackTick_ = tick;
        fallbackMultiple_ = multiple;
    }

private:
    const MarketTickSnap* marketSnap(const char* instrument) const {
        if (!market_) return nullptr;
        const uint32_t n = market_->count.load(std::memory_order_acquire);
        for (uint32_t i = 0; i < n && i < kMaxMarketInstruments; ++i) {
            if (std::strncmp(market_->items[i].instrument, instrument, kInstrumentLen) == 0) {
                return &market_->items[i];
            }
        }
        return nullptr;
    }

    // Instrument metadata, cached lazily: a lookup miss reads ONE record
    // straight out of the shared snapshot under the seqlock (no bulk copy of
    // the 4000-instrument table, no std::string, no map nodes). The cache is
    // refreshed once a second by syncSnapshot().
    const InstrumentSnap* instrumentOf(const char* instrument) const {
        InstSlot* s = findSlot(instSlots_, instSlotsN_, instrument, false);
        if (s && s->valid) return &s->snap;
        if (!s) {
            s = findSlot(instSlots_, instSlotsN_, instrument, true);
            if (!s) return nullptr;  // cache full
        }
        SnapshotBlock* blk = reader_.block();
        if (blk && reader_.findInstrument(instrument, s->snap)) {
            s->valid = true;
            return &s->snap;
        }
        return nullptr;
    }

    CmdChannel* commands_;
    ChannelEvent* commandsEvt_ = nullptr;
    ControlBlock* control_;
    MarketSnapshotBlock* market_;
    SnapshotReader reader_;

    // Fixed hot tables (see class comment). Mutable: filled from the const
    // accessor instrumentOf() and from const setTick() call paths.
    mutable TickSlot tickSlots_[kHotMax]{};
    mutable uint32_t tickSlotsN_ = 0;
    mutable PosSlot posSlots_[kHotMax]{};
    mutable uint32_t posSlotsN_ = 0;
    mutable InstSlot instSlots_[kHotMax]{};
    mutable uint32_t instSlotsN_ = 0;
    mutable BrokerSlot brokerSlots_[kHotMax]{};
    mutable uint32_t brokerSlotsN_ = 0;
    bool snapshotReady_ = false;
    uint32_t snapshotRound_ = 0;
    double available_ = 0.0;
    double fallbackTick_ = 1.0;
    int fallbackMultiple_ = 10;
    uint32_t cmdId_ = 0;
};

// ---------------------------------------------------------------------------
// Process
// ---------------------------------------------------------------------------
class StrategyProcess : public ProcessApp {
public:
    StrategyProcess() : ProcessApp(Role::Strategy, "strategy") {}

protected:
    bool attachChannels(std::string& err) override {
        const bool create = true;
        return channels().openControl(create, err) && channels().openTicks(create, err) &&
               channels().openCommands(create, err) && channels().openExecs(create, err) &&
               channels().openQryReq(create, err) && channels().openQryRsp(create, err) &&
               channels().openSnapshot(create, err) && channels().openMarket(create, err);
    }

    bool onStart(std::string& err) override {
        // Drain leftovers of a previous strategy run from the inbound rings:
        // stale ticks would trade on outdated prices, and stale exec reports
        // (fills of the dead process) would corrupt the fresh state machine.
        // The broker side query snapshot is the position truth on restart.
        {
            TickMsg t;
            size_t drainedTicks = 0;
            while (channels().ticks()->pop(t)) ++drainedTicks;
            ExecReport e;
            size_t drainedExecs = 0;
            while (channels().execs()->pop(e)) ++drainedExecs;
            if (drainedTicks || drainedExecs) {
                HFT_LOGW("discarded %zu stale ticks and %zu stale exec reports from a previous run",
                         drainedTicks, drainedExecs);
            }
        }

        ctx_.reset(new StrategyContextImpl(channels().commands(), &channels().commandsEvt(), control(), channels().market(),
                                           channels().snapshot()));
        ctx_->setFallbackInstrumentInfo(config().getDouble("sim", "price_tick", 1.0),
                                       config().getInt("sim", "volume_multiple", 10));

        const std::string name = config().strategyName();
        if (name == "obi_voi") {
            strategy_.reset(new ObiVoiStrategy());
        } else {
            err = "unknown strategy: " + name;
            return false;
        }
        if (!strategy_->init(config(), *ctx_, err)) return false;
        ctx_->syncSnapshot();
        HFT_LOGI("strategy '%s' ready", strategy_->name());
        return true;
    }

    bool onPoll() override {
        bool any = false;

        // 1) ticks (bulk pop keeps the per-tick overhead tiny)
        TickMsg buf[256];
        const uint32_t n = channels().ticks()->popBulk(buf, 256);
        for (uint32_t i = 0; i < n; ++i) {
            ctx_->setTick(buf[i]);
            latencySumNs_ += wallNowNs() - buf[i].publishTimeNs;
            strategy_->onTick(buf[i]);
        }
        if (n > 0) {
            tickCount_ += n;
            any = true;
        }

        // 2) execution reports
        ExecReport rep;
        while (channels().execs()->pop(rep)) {
            ctx_->applyExec(rep);
            strategy_->onExec(rep);
            ++execCount_;
            any = true;
        }

        // 3) responses to explicit queries (informational)
        QueryResponse rsp;
        while (channels().qryRsp()->pop(rsp)) {
            if (rsp.type == QueryType::Account) {
                HFT_LOGI("qry account: balance=%.2f available=%.2f", rsp.account.balance,
                         rsp.account.available);
            }
            any = true;
        }

        // 4) once a second: reconcile with the query snapshot + timer + stats
        const int64_t now = nowNs();
        if (now - lastTimerNs_ >= 1000000000ll) {
            lastTimerNs_ = now;
            ctx_->syncSnapshot();
            strategy_->onTimer(now);
            printStats(now);
        }

        return any;
    }

    void onStop() override {
        if (strategy_) strategy_->onStop();
    }

    int idleSleepUs() const override { return 200; }

    // The process' own cadence is the 1 s strategy timer (onTimer + snapshot
    // reconcile + stats). Between timer ticks the loop blocks on the tick /
    // exec / query doorbells and burns no CPU.
    int64_t maxWaitNs() const override { return 1000000000ll; }

    // Wake instantly when md publishes a tick, the trader returns an exec
    // report or the query process answers a request.
    void collectWaitHandles(std::vector<void*>& handles) override {
        handles.push_back(channels().ticksEvt().handle());
        handles.push_back(channels().execsEvt().handle());
        handles.push_back(channels().qryRspEvt().handle());
    }

private:
    void printStats(int64_t now) {
        if (lastStatNs_ == 0) {
            lastStatNs_ = now;
            return;
        }
        const double secs = static_cast<double>(now - lastStatNs_) / 1e9;
        const double avgLatencyUs = tickCount_ > 0 ? (static_cast<double>(latencySumNs_) / static_cast<double>(tickCount_) / 1000.0) : 0.0;
        HFT_LOGI("ticks=%llu (%.1f/s) execs=%llu tick->strategy latency=%.1f us backlog=%llu",
                 static_cast<unsigned long long>(tickCount_),
                 static_cast<double>(tickCount_) / secs,
                 static_cast<unsigned long long>(execCount_), avgLatencyUs,
                 static_cast<unsigned long long>(channels().ticks()->size()));
        lastStatNs_ = now;
        tickCount_ = 0;
        execCount_ = 0;
        latencySumNs_ = 0;
    }

    std::unique_ptr<StrategyContextImpl> ctx_;
    std::unique_ptr<Strategy> strategy_;
    uint64_t tickCount_ = 0;
    uint64_t execCount_ = 0;
    int64_t latencySumNs_ = 0;
    int64_t lastTimerNs_ = 0;
    int64_t lastStatNs_ = 0;
};

} // namespace hft

int main(int argc, char** argv) {
    hft::StrategyProcess app;
    if (!app.init(argc, argv)) return 1;
    return app.run();
}
