// SPDX-License-Identifier: MIT
// obi_voi_strategy.h - L1 order book imbalance fade strategy.
//
// Signal: extreme imbalance + decaying flow, both inside a short lookback
//   OBI  = (bidVolume1 - askVolume1) / (bidVolume1 + askVolume1)
//          |OBI| > obi_threshold at some point in the last window_ticks
//          -> the book was EXTREMELY one sided.
//   VOI  = EMA(agggressor signed volume) / EMA(|aggressor volume|)
//          the aggressive flow behind the extreme has just crossed the
//          zero axis against it (buy extreme but VOI turned negative)
//          -> the pressure is EXHAUSTED and starts to withdraw.
//
// When both are true the strategy FADES the extreme: it places a passive
// limit order on the WEAK side of the book (sell @ ask1 after a buy
// extreme, buy @ bid1 after a sell extreme) and gets filled at a favorable
// price while the imbalance decays and the price reverts to the middle.
//
// Order rules (hard):
//   * quotes only at bid1 / ask1 itself, never deeper levels.
//   * the entry order is pulled IMMEDIATELY when VOI accelerates back into
//     the original extreme direction (the decay call was wrong) - the
//     passive order must not be adversely picked off in the wrong direction.
//   * the entry order is pulled after entry_timeout_ticks without a fill.
//
// Inventory rules (the lifeline of a passive strategy):
//   * net position is capped at max_position (1); at the cap only closing
//     orders are allowed, never new opens.
//   * after the entry fill a passive close rests at the touch; once VOI has
//     clearly turned against the position and the close did not fill within
//     close_timeout_ticks, the close switches to an IOC limit at the
//     opponent price (cross the spread, be flat NOW).
//   * every state change is driven by the asynchronous order/trade reports;
//     onTick NEVER assumes the previous order was filled or canceled.
#ifndef HFT_OBI_VOI_STRATEGY_H
#define HFT_OBI_VOI_STRATEGY_H

#include "hft/strategy_base.h"

#include <string>
#include <vector>

namespace hft {

class ObiVoiStrategy : public Strategy {
public:
    const char* name() const override { return "obi_voi"; }

    bool init(const Config& cfg, StrategyContext& ctx, std::string& err) override;
    void onTick(const TickMsg& tick) override;
    void onExec(const ExecReport& report) override;
    void onTimer(int64_t nowNs) override;
    void onStop() override;

private:
    enum class Phase : uint8_t {
        Idle = 0,       // flat, no working order, hunting for a signal
        Opening = 1,    // passive entry limit is resting at the touch
        Closing = 2,    // holding, passive close limit is resting at the touch
        Escaping = 3,   // holding, IOC close at the opponent price is cycling
    };

    // What the pending order slot is used for right now.
    enum class OrderKind : uint8_t {
        None = 0,
        Entry,        // passive open (GFD)
        ClosePassive, // passive take profit close (GFD)
        CloseIoc,     // escape close, IOC / FAK at the opponent price
        Flatten,      // startup leftover flatten (GFD, at the touch)
    };

    struct Pending {
        bool active = false;         // true from insert until the final report
        bool cancelSent = false;     // cancel request on the wire, awaiting report
        int64_t cancelSentNs = 0;    // when the cancel went out (watchdog)
        int64_t firstCancelNs = 0;   // first cancel attempt (dead-order deadline)
        OrderKind kind = OrderKind::None;
        uint32_t cmdId = 0;
        char instrument[kInstrumentLen] = {0};
        char orderRef[kOrderRefLen] = {0};
        char orderSysID[kOrderSysIdLen] = {0};
        char direction = '0';
        char offsetFlag = '0';
        int volume = 0;
        int filledVolume = 0;
        int frontID = 0;
        int sessionID = 0;
        uint64_t tag = 0;
        int64_t sentTimeNs = 0;
        int ticksSinceSend = 0;      // filled by onTick (timeout bookkeeping)
    };

    // One leg of a flatten (a position can span today / yesterday buckets).
    struct CloseLeg {
        char direction = '0';
        char offsetFlag = '0';
        int volume = 0;
    };

    struct State {
        // ---- market microstructure estimators --------------------------------
        // OBI ring: the last window_ticks imbalance values. Fixed 32-slot
        // ring buffer (power of two) - covers window_ticks <= 32 with ZERO
        // heap traffic; std::deque allocated a chunk every few pushes and
        // pointer-chased across the heap on every scan.
        static constexpr uint32_t kObiRing = 32;
        double obiRing[kObiRing] = {};
        uint32_t obiHead = 0;   // next write slot
        uint32_t obiCount = 0;  // valid entries, <= kObiRing
        void obiPush(double v) {
            obiRing[obiHead] = v;
            obiHead = (obiHead + 1) & (kObiRing - 1);
            if (obiCount < kObiRing) ++obiCount;
        }
        double obiBack() const { return obiRing[(obiHead + kObiRing - 1) & (kObiRing - 1)]; }
        double obiAt(uint32_t i) const {  // 0 = oldest valid entry
            return obiRing[(obiHead + kObiRing - obiCount + i) & (kObiRing - 1)];
        }

        // Short-horizon mid drift, kept as ONE CHECKPOINT PER SECOND in a
        // 32-slot ring: 32 s of history covers the 30 s trend window at any
        // tick rate (ag2612 ticks at 1-15/s; a per-tick deque needed up to
        // 450 entries for the same window). The window is measured in TIME,
        // not ticks, and the checkpoints keep the per-tick update O(1) with
        // no allocation.
        static constexpr uint32_t kMidCkpts = 32;
        struct MidCkpt { int64_t ns = 0; double mid = 0.0; };
        MidCkpt midCk[kMidCkpts] = {};
        uint32_t midHead = 0;
        uint32_t midCount = 0;
        void midUpdate(int64_t ts, double mid) {
            if (midCount > 0) {
                MidCkpt& last = midCk[(midHead + kMidCkpts - 1) % kMidCkpts];
                if (ts - last.ns < INT64_C(1000000000)) {
                    // refresh the current second: the right edge of the drift
                    // window always tracks the latest tick
                    last.ns = ts;
                    last.mid = mid;
                    return;
                }
            }
            midCk[midHead].ns = ts;
            midCk[midHead].mid = mid;
            midHead = (midHead + 1) % kMidCkpts;
            if (midCount < kMidCkpts) ++midCount;
        }
        const MidCkpt& midCkAt(uint32_t i) const {  // 0 = oldest
            return midCk[(midHead + kMidCkpts - midCount + i) % kMidCkpts];
        }
        double prevPrice = 0.0;
        double prevVolume = 0.0;    // CTP volume is a cumulative double
        double voiSum = 0.0;        // EMA of signed aggressor volume
        double voiDen = 0.0;        // EMA of |aggressor volume|
        double voi = 0.0;           // normalized to roughly [-1, 1]
        int tickCount = 0;

        // ---- touch history for the stall filter -------------------------------
        // Last 32 bid/ask touches in a power-of-two ring: min/max over the
        // last stall_window_ticks is a short linear scan, zero allocation.
        static constexpr uint32_t kTouchRing = 32;
        double bidRing[kTouchRing] = {};
        double askRing[kTouchRing] = {};
        uint32_t touchHead = 0;    // next write slot
        uint32_t touchCount = 0;
        void touchPush(double b, double a) {
            bidRing[touchHead] = b;
            askRing[touchHead] = a;
            touchHead = (touchHead + 1) & (kTouchRing - 1);
            if (touchCount < kTouchRing) ++touchCount;
        }
        // min bid / max ask over the newest k entries (k clamped to what we have)
        double bidMin(uint32_t k) const {
            if (touchCount == 0) return 0.0;
            if (k > touchCount) k = touchCount;
            double m = 1e18;
            for (uint32_t i = 0; i < k; ++i) {
                const double v = bidRing[(touchHead + kTouchRing - 1 - i) & (kTouchRing - 1)];
                if (v > 0 && v < m) m = v;
            }
            return m == 1e18 ? 0.0 : m;
        }
        double askMax(uint32_t k) const {
            if (touchCount == 0) return 0.0;
            if (k > touchCount) k = touchCount;
            double m = -1.0;
            for (uint32_t i = 0; i < k; ++i) {
                const double v = askRing[(touchHead + kTouchRing - 1 - i) & (kTouchRing - 1)];
                if (v > m) m = v;
            }
            return m < 0 ? 0.0 : m;
        }

        // ---- signal bookkeeping ----------------------------------------------
        bool armedBuy = true;   // may fire BUY (sell extreme + VOI > 0); re-arms while VOI > 0
        bool armedSell = true;  // may fire SELL (buy extreme + VOI < 0); re-arms while VOI < 0
        int cooldownTicks = 0;  // ticks to wait after a round trip / canceled entry

        // ---- round trip bookkeeping ------------------------------------------
        Pending pending;
        Phase phase = Phase::Idle;
        double entryPrice = 0.0;
        char entryDir = '0';
        int64_t openTimeNs = 0;
        // Bracket prices, computed ONCE when the entry fill is booked (see
        // armBracket): the tick loop then only compares the touch against
        // these, instead of redoing tickSize math on every tick.
        double tpTrigger = 0.0;   // unrounded: entry +/- profit_ticks * tick
        double slTrigger = 0.0;   // unrounded: entry -/+ stop_loss_ticks * tick (0 = off)
        double tpPrice = 0.0;     // roundToTick(tpTrigger) - resting TP limit
        int adverseTicks = 0;        // consecutive ticks with VOI against the position
        int escapeRetries = 0;
        // An escape IOC is the last line of defence: it has to keep being
        // retried until the book is flat. lastEscapeNs is when the previous
        // attempt went out (rate limit), escapeSinceNs when the campaign began
        // (used for the log line and the give-up decision).
        int64_t lastEscapeNs = 0;
        int64_t escapeSinceNs = 0;
        int holdTicks = 0;               // ticks held since the entry fill (survives re-posts)
        int entryQuoteTicks = 0;         // entry quotes placed in the current campaign
        int  closeOffsetTries = 0;        // close reposted with the next offset flag N times
        int closeReposts = 0;             // dead closes re-posted this round; >5 -> IOC
        int tpPosts = 0;                  // take-profit limits posted this round trip
        int ticksSinceTpPost = 0;         // throttle: ticks since the last TP post
        // The order report said "filled" but the trade report has not arrived
        // yet (the 7x24 front reorders them). Until this deadline the state
        // machine neither re-sends the order nor fires a new entry.
        int64_t awaitTradeUntilNs = 0;
        // Two-stage entry: the first IOC is priced AT the opponent touch (no
        // extra tick). When it dies unfilled the level was momentarily too
        // thin, so one retry crosses a tick - cheaper than paying that extra
        // tick on every entry. Cleared once the retry is spent.
        char retryEntryDir = '0';
        int64_t retryEntryUntilNs = 0;
        // streak guard
        int consecStops = 0;
        int64_t pauseUntilNs = 0;
        // set once the breakeven stop has been armed this round trip (log once)
        bool beArmed = false;
        // The counter refused an order because the session is closed. Nothing
        // can be sent until this deadline: rotating offset flags only produces
        // more rejections (2026-11-10 15:21:04, after the 15:00 close).
        int64_t marketClosedUntilNs = 0;
        // sign of the extreme the entry faded: +1 buy extreme (we sold), -1 sell
        // extreme (we bought). VOI moving into this direction again = wrong.
        int fadeSign = 0;

        // ---- startup leftover handling ----------------------------------------
        bool tradingStarted = false;
        bool startupDone = false;
        uint32_t seenRound = 0;
        int roundsSeen = 0;
        int staleBrokerRounds = 0;
        int flatBrokerRounds = 0;   // broker reports flat while local holds
        // Flatten legs: a position splits into at most 4 buckets (long/short x
        // today/yesterday). Fixed ring, never allocates.
        static constexpr uint32_t kMaxFlattenLegs = 8;
        CloseLeg flattenLegs[kMaxFlattenLegs] = {};
        uint32_t flattenHead = 0;
        uint32_t flattenCount = 0;
        bool flattenEmpty() const { return flattenCount == 0; }
        uint32_t flattenSize() const { return flattenCount; }
        void flattenClear() { flattenHead = 0; flattenCount = 0; }
        void flattenPush(const CloseLeg& leg) {
            if (flattenCount >= kMaxFlattenLegs) return;  // cannot happen (4 max)
            flattenLegs[(flattenHead + flattenCount) % kMaxFlattenLegs] = leg;
            ++flattenCount;
        }
        const CloseLeg& flattenFront() const { return flattenLegs[flattenHead]; }
        void flattenPop() {
            if (flattenCount == 0) return;
            flattenHead = (flattenHead + 1) % kMaxFlattenLegs;
            --flattenCount;
        }
        // flatten backoff: a position the matcher keeps rejecting (50 AND 51)
        // is a 7x24 replay phantom - retrying every tick only churns orders
        int flattenFails = 0;
        int64_t flattenBackoffUntilNs = 0;

        // ---- broker reconciliation -------------------------------------------
        uint32_t lastReconcileRound = 0;
        int mismatchRounds = 0;

        int signalCount = 0;
        int orderCount = 0;
    };

    State& state(const char* instrument);
    State* findState(const char* instrument);

    // ---- indicator update (called first in onTick) --------------------------
    void updateIndicators(State& st, const TickMsg& tick);

    // ---- state machine steps (onTick) ---------------------------------------
    void evaluate(State& st, const TickMsg& tick);
    void manageWorkingOrder(State& st, const TickMsg& tick);
    void reconcileBook(State& st, const TickMsg& tick);
    void startupGate(State& st, const TickMsg& tick);
    void startFlatten(State& st, const char* instrument);
    void pumpFlatten(State& st, const TickMsg& tick);

    // ---- order helpers -------------------------------------------------------
    void sendEntry(State& st, const TickMsg& tick, Dir dir, int extraTicks = 0);
    void sendClosePassive(State& st, const TickMsg& tick);
    // escape/taker close: IOC at the opponent price crossed by extraSlipTicks
    // (stop orders pass stopSlippageTicks_ so the fill is virtually certain)
    // offsetFlag defaults to CloseToday ('3'); SHFE products need the explicit
    // bucket, so the escape retry ring walks 3 -> 4 -> 1.
    // limitPrice > 0 pins the IOC limit to that price instead of the opponent
    // touch. Used by the take-profit path: a sell priced at the TP target still
    // fills at the (better) bid when the bid has run past the target, but it
    // can never print below the target if the book falls back in the 100-200 ms
    // the order spends on the wire.
    void sendCloseIoc(State& st, const TickMsg& tick, int extraSlipTicks = 0,
                      char offsetFlag = '3', double limitPrice = 0.0);
    void sendFlattenLeg(State& st, const TickMsg& tick, const CloseLeg& leg);
    void requestCancel(State& st, const char* instrument);
    void resetRoundTrip(State& st);

    // ---- risk bookkeeping -----------------------------------------------------
    // Rolls the daily loss budget when tick.tradingDay changes, books a closed
    // round trip into it, and audits the loaded config at start-up (a silently
    // stale config is what produced the 2026-11-10 loss).
    void rollTradingDay(const char* tradingDay);
    void bookRoundTrip(double pricePnl, int volume, const char* instrument);
    bool auditConfig(std::string& err) const;

    // ---- pricing --------------------------------------------------------------
    double weakSidePrice(const TickMsg& tick, Dir dir) const;  // buy @ bid1, sell @ ask1
    double opponentPrice(const TickMsg& tick, Dir dir) const;  // buy @ ask1, sell @ bid1
    double roundToTick(double price, double tickSize) const;
    // Computes the bracket (tp/sl trigger + resting TP limit) from the entry
    // price. Called ONCE per round trip, the moment the entry fill is booked.
    void armBracket(State& st, double tickSize);
    // true when the exit touch is stop_loss_ticks or further against entry
    bool stopLossHit(const State& st, const TickMsg& tick, int net) const;
    // Mid drift over trend_window_ms, expressed in ticks. Only meaningful once
    // the window holds a usable amount of history - check midDriftReady() first,
    // otherwise a cold/stalled window returns 0 and every entry would be judged
    // against a drift that was never measured.
    double midDriftTicks(const State& st, double tickSize) const;
    bool midDriftReady(const State& st) const;
    static const char* phaseName(Phase p);

    StrategyContext* ctx_ = nullptr;

    // ---- parameters -----------------------------------------------------------
    std::vector<std::string> instruments_;
    int windowTicks_ = 10;        // OBI extreme lookback
    double obiThreshold_ = 0.6;   // |OBI| that counts as an extreme
    int voiEmaTicks_ = 5;         // VOI smoothing
    double voiFlipThreshold_ = 0.3;  // |VOI| beyond this = "clearly reversed"
    int entryTimeoutTicks_ = 4;   // pull an unfilled entry after N ticks
    int entrySlippageTicks_ = 1;  // entry crosses N ticks toward the opponent
                                  // (buy: bid1+N, capped at ask1; sell: ask1-N,
                                  // capped at bid1) so entries fill promptly
    bool entryTaker_ = false;     // entry_taker=1: cross to the opponent price
                                  // and send IOC.  Required on the SimNow 7x24
                                  // front, which cancels resting quotes within
                                  // 100-500 ms (passive entries never fill
                                  // there).  Costs 0-1 tick of slippage.
    int profitTicks_ = 2;         // passive close rests at entry +/- N ticks:
                                  // the fade thesis captures mean reversion,
                                  // the touch would give up all the edge
    bool makerMode_ = false;      // entry_mode=maker: quote the touch on the
                                  // side of the last print and capture the
                                  // spread (for 1-tick oscillating replays)
    int makerThinThreshold_ = 0;  // quote only when the level we join holds <=
                                  // N lots (0 = off); a thin level sweeps
                                  // through our queue position
    int closeTimeoutTicks_ = 3;   // passive close deadline once VOI is adverse
                                   // (reporting only; VOI noise never IOCs)
    int stopLossTicks_ = 3;        // price-based stop: IOC escape only after the
                                   // market has moved N ticks against the entry
                                   // (0 = off). VOI flips alone are NOT a stop -
                                   // they re-post the close, they never cross.
    int maxHoldTicks_ = 30;       // safety net: escape after N ticks holding (0 = off)
    // Wall-clock hard stop. Tick counts are useless as a safety net when the
    // feed is sparse or the book sits between TP and SL: on 2026-11-05 an ag
    // short was held for 14 h overnight because its escape IOC was cancelled
    // and nothing retried it. This is the last-resort "get out at any price".
    int maxHoldSeconds_ = 600;    // 0 = off
    int escapeRetryMs_ = 800;     // min gap between escape IOC attempts
    double voiEntryThreshold_ = 0.0;  // |VOI| that counts as a CONFIRMED reversal
                                  // (0 = sign only, the old loose behavior)
    int tpRepostIntervalTicks_ = 10;  // min ticks between TP re-posts (anti-storm:
                                  // the counter may auto-kill resting limits)
    int tpRepostMax_ = 40;        // TP re-posts per round trip; beyond this the
                                  // exit waits purely on the price trigger
    int stopSlippageTicks_ = 1;   // stop IOC crosses this many extra ticks past
                                  // the opponent touch so the fill is immediate
    // Spread guard. The taker entry pays half the spread on the way in and the
    // exit pays it again, so a wide book eats the fade edge before the mid has
    // moved at all. Measured on ag2612: 2 ticks is the median, 3+ is common in
    // the first minutes after the open. Entries are skipped while the book is
    // wider than this. 0 = off.
    int maxSpreadTicks_ = 0;
    // Overshoot filter ("has price run away yet?"). Measured over 69 round
    // trips (11-05 724 + 11-06 day, phantom trips removed): entering AGAINST a
    // 0-5 tick drift over the last 30 s made +40 ticks at 67 % win, and > 5
    // ticks against made +16 at 50 %; entering WITH the drift lost -153 ticks
    // at 18 % win. So the edge is the overshoot itself, not the direction.
    // A signal is skipped unless the mid moved >= trend_min_ticks AGAINST the
    // entry over the last trend_window_ms.
    int trendWindowMs_ = 0;       // 0 = filter off
    int trendMinTicks_ = 1;       // required overshoot, in ticks, against the entry
    // Stall filter ("has the impulse actually stopped?"). The 30 s drift filter
    // above only knows that price ran; it cannot tell a 20 s old overshoot from
    // an impulse that started 2 s ago.
    //
    // DEFAULT OFF (2026-11-10, second measurement). The +4.13 -> +4.53 figure
    // in the earlier note came from a replay WITHOUT the spread gate. On the
    // universe this strategy actually trades (spread <= 1 tick, tp16/sl8, night)
    // waiting for the pullback is destructive, because the signals with the best
    // forward edge are exactly the ones whose impulse is still extending:
    //   stall 0 -> 68 trades +4.13/trip   stall 2 -> 66 +3.83
    //   stall 4 -> 41 +3.95               stall 6 -> 17 +0.50
    //   stall 8 ->  9 -0.17               stall 10 ->  0 trades
    // A tight spread already proves the book is liquid, which removes the
    // "extending into a vacuum" entries this filter was meant to block.
    int entryStallTicks_ = 0;     // 0 = filter off
    int stallWindowTicks_ = 6;    // look-back for the extreme being faded
    // Breakeven ratchet: after N ticks of profit the stop moves to the entry
    // price, so a round trip that worked cannot turn into a full stop-out.
    // 2026-11-10: 13 trips printed >= +7 ticks and 5 of them closed <= -6.
    int breakevenTicks_ = 8;      // 0 = off
    // Do not open new positions in the last N seconds of an entry window: the
    // 2026-11-10 run bought at 14:59:15 (45 s before the SHFE close), then the
    // close order at 15:21 was refused because the market was shut.
    int entryBlackoutSec_ = 300;  // 0 = off
    // Streak guard: after N losing round trips in a row, stop looking for
    // entries for pause_seconds. A fade bleeds in a trend and the per-round
    // cooldown (20 ticks ~ 10 s) is far too short to notice one.
    int consecStopPause_ = 0;     // 0 = off
    int pauseSeconds_ = 300;
    // ---- daily loss circuit breaker -------------------------------------------
    // Per-trade stops bound one trade; nothing bounded a DAY. Replaying the
    // 2026-11-10 tape, 31 round trips averaged -1.65 tick and the session simply
    // kept trading into the same losing regime. Once realised losses for the
    // trading day reach this many ticks (per lot), no new entries are opened -
    // existing positions still exit normally through their bracket. Reset when
    // the exchange trading day rolls (tick.tradingDay), so the night session
    // and the day session after it are budgeted separately.
    // 0 = off. 40 ticks ~ 600 CNY on one ag lot, about 0.7 daily sigma.
    int dailyLossLimitTicks_ = 0;
    bool allowLooseRisk_ = false;  // boot audit bypass: [obi_voi] allow_loose_risk
    double dailyPnlTicks_ = 0.0;   // realised, per lot, current trading day
    char dailyDay_[kDateLen] = {0};
    bool dailyHalted_ = false;
    // Session gate. The fade edge is not constant over the day: replaying the
    // 23 h of recorded ag2612 ticks (11-05 night + 11-06 day) with cost, the
    // night session paid +259 ticks over 61 trades at 56 % win while the day
    // session lost -116 over 24 trades, and NO tp/sl pair in the grid made the
    // day session positive (-3.5 .. -6.2 tick/trade). Entry seeking is
    // therefore restricted to the windows listed here; exits are never gated
    // (a position must always be closable).
    // Format: "HH:MM-HH:MM[,HH:MM-HH:MM]", a window may cross midnight.
    // Empty = 24 h (old behaviour).
    std::vector<std::pair<int, int>> sessions_;   // seconds of day, [from, to)
    bool inSession(const TickMsg& tick) const;
    // true when the tick sits inside an entry window but within
    // entry_blackout_sec of that window's END (no new entries there).
    bool nearSessionEnd(const TickMsg& tick) const;
    int tradeVolume_ = 1;
    int maxPosition_ = 1;
    int cooldownTicks_ = 5;
    bool allowShort_ = true;
    bool flattenOnStart_ = true;
    bool closeOnStop_ = true;

    static constexpr int kStartupRounds = 3;
    static constexpr int kStaleRounds = 2;
    static constexpr int kMaxEscapeRetries = 200;

    // Per-instrument states in a FIXED table: a strategy trades a handful of
    // instruments, so a 16-slot array + linear scan replaces a std::map
    // (which allocated a node and built a std::string key on EVERY tick).
    static constexpr uint32_t kMaxStates = 16;
    struct StateEntry {
        char instrument[kInstrumentLen];
        State st;
    };
    StateEntry states_[kMaxStates];
    uint32_t statesN_ = 0;
    State overflowState_;  // returned if the table ever overruns (logged)
};

} // namespace hft

#endif // HFT_OBI_VOI_STRATEGY_H
