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

#include <deque>
#include <map>
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
        std::deque<double> obiWin;  // last window_ticks OBI values
        // Short-horizon mid drift, kept as (timestamp, mid) pairs so the window
        // is measured in TIME, not ticks: ag2612 ticks at 1-15/s depending on
        // activity, and a 20-tick window can be either 2 s or 20 s.
        std::deque<int64_t> midTimesNs;
        std::deque<double> midWin;
        double prevPrice = 0.0;
        double prevVolume = 0.0;    // CTP volume is a cumulative double
        double voiSum = 0.0;        // EMA of signed aggressor volume
        double voiDen = 0.0;        // EMA of |aggressor volume|
        double voi = 0.0;           // normalized to roughly [-1, 1]
        int tickCount = 0;

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
        std::deque<CloseLeg> flattenLegs;
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

    // ---- pricing --------------------------------------------------------------
    double weakSidePrice(const TickMsg& tick, Dir dir) const;  // buy @ bid1, sell @ ask1
    double opponentPrice(const TickMsg& tick, Dir dir) const;  // buy @ ask1, sell @ bid1
    double roundToTick(double price, double tickSize) const;
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
    // Streak guard: after N losing round trips in a row, stop looking for
    // entries for pause_seconds. A fade bleeds in a trend and the per-round
    // cooldown (20 ticks ~ 10 s) is far too short to notice one.
    int consecStopPause_ = 0;     // 0 = off
    int pauseSeconds_ = 300;
    int tradeVolume_ = 1;
    int maxPosition_ = 1;
    int cooldownTicks_ = 5;
    bool allowShort_ = true;
    bool flattenOnStart_ = true;
    bool closeOnStop_ = true;

    static constexpr int kStartupRounds = 3;
    static constexpr int kStaleRounds = 2;
    static constexpr int kMaxEscapeRetries = 200;

    std::map<std::string, State> states_;
};

} // namespace hft

#endif // HFT_OBI_VOI_STRATEGY_H
