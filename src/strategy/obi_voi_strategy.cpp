// SPDX-License-Identifier: MIT
// obi_voi_strategy.cpp - L1 order book imbalance fade strategy.
//
// Everything below is event driven: onTick only THINKS (indicators, cancel
// decisions, new orders); all state transitions that depend on an order
// outcome happen in onExec when the actual report arrives. onTick never
// assumes the previous order was filled or canceled.
#include "obi_voi_strategy.h"

#include "hft/logger.h"
#include "hft/time_util.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace hft {

namespace {
// A counter rejection that says "the market is not taking orders now" is NOT
// retryable: rotating the offset flag, crossing wider or re-sending 800 ms
// later all come back with the same answer. Recognise it so the state machine
// stops churning and carries the position to the next session instead.
bool isClosedMarketRejection(const char* msg) {
    if (!msg || !*msg) return false;
    static const char* kNeedles[] = {"禁止此项操作", "非交易时间", "不在交易",
                                     "已收盘", "市场已关闭", "not in trading",
                                     "market closed"};
    for (const char* n : kNeedles) {
        if (std::strstr(msg, n) != nullptr) return true;
    }
    return false;
}
}  // namespace

const char* ObiVoiStrategy::phaseName(Phase p) {
    switch (p) {
        case Phase::Idle: return "idle";
        case Phase::Opening: return "opening";
        case Phase::Closing: return "closing";
        case Phase::Escaping: return "escaping";
    }
    return "?";
}

bool ObiVoiStrategy::init(const Config& cfg, StrategyContext& ctx, std::string& err) {
    ctx_ = &ctx;

    instruments_ = cfg.instruments();
    windowTicks_ = cfg.getInt("obi_voi", "window_ticks", 10);
    obiThreshold_ = cfg.getDouble("obi_voi", "obi_threshold", 0.6);
    voiEmaTicks_ = cfg.getInt("obi_voi", "voi_ema_ticks", 5);
    voiFlipThreshold_ = cfg.getDouble("obi_voi", "voi_flip_threshold", 0.3);
    entryTimeoutTicks_ = cfg.getInt("obi_voi", "entry_timeout_ticks", 4);
    entrySlippageTicks_ = cfg.getInt("obi_voi", "entry_slippage_ticks", 1);
    // The SimNow 7x24 front auto-cancels a resting quote within 100-500 ms, so
    // a passive entry there fills almost never (measured 17:00-17:45 on
    // 2026-11-05: 0 fills out of 14 signals). entry_taker=1 crosses to the
    // opponent price and sends IOC so the signal actually lands.
    entryTaker_ = cfg.getBool("obi_voi", "entry_taker", false);
    profitTicks_ = cfg.getInt("obi_voi", "profit_ticks", 2);
    const std::string entryMode = cfg.getString("obi_voi", "entry_mode", "fade");
    makerMode_ = (entryMode == "maker");
    followMode_ = (entryMode == "follow");
    followPatienceMs_ = cfg.getInt("obi_voi", "follow_patience_ms", 3000);
    makerThinThreshold_ = cfg.getInt("obi_voi", "maker_thin_threshold", 0);
    closeTimeoutTicks_ = cfg.getInt("obi_voi", "close_timeout_ticks", 3);
    stopLossTicks_ = cfg.getInt("obi_voi", "stop_loss_ticks", 3);
    maxHoldTicks_ = cfg.getInt("obi_voi", "max_hold_ticks", 30);
    maxHoldSeconds_ = cfg.getInt("obi_voi", "max_hold_seconds", 600);
    escapeRetryMs_ = cfg.getInt("obi_voi", "escape_retry_ms", 800);
    voiEntryThreshold_ = cfg.getDouble("obi_voi", "voi_entry_threshold", 0.0);
    tpRepostIntervalTicks_ = cfg.getInt("obi_voi", "tp_repost_interval_ticks", 10);
    tpRepostMax_ = cfg.getInt("obi_voi", "tp_repost_max", 40);
    stopSlippageTicks_ = cfg.getInt("obi_voi", "stop_slippage_ticks", 1);
    maxSpreadTicks_ = cfg.getInt("obi_voi", "max_spread_ticks", 0);
    trendWindowMs_ = cfg.getInt("obi_voi", "trend_window_ms", 0);
    trendMinTicks_ = cfg.getInt("obi_voi", "trend_min_ticks", 1);
    entryStallTicks_ = cfg.getInt("obi_voi", "entry_stall_ticks", 2);
    stallWindowTicks_ = cfg.getInt("obi_voi", "stall_window_ticks", 6);
    breakevenTicks_ = cfg.getInt("obi_voi", "breakeven_ticks", 8);
    entryBlackoutSec_ = cfg.getInt("obi_voi", "entry_blackout_sec", 1800);
    consecStopPause_ = cfg.getInt("obi_voi", "consec_stop_pause", 0);
    pauseSeconds_ = cfg.getInt("obi_voi", "pause_seconds", 300);

    // Session gate: "21:00-02:30" or a comma separated list. A negative window
    // (to < from) means it crosses midnight, which is exactly what the night
    // session of a commodity future does.
    sessions_.clear();
    const std::string sess = cfg.getString("obi_voi", "trade_sessions", "");
    if (!sess.empty()) {
        size_t pos = 0;
        while (pos <= sess.size()) {
            const size_t comma = sess.find(',', pos);
            const std::string one =
                sess.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
            int h1 = 0, m1 = 0, h2 = 0, m2 = 0;
            if (std::sscanf(one.c_str(), " %d:%d - %d:%d", &h1, &m1, &h2, &m2) == 4 &&
                h1 >= 0 && h1 < 24 && h2 >= 0 && h2 < 24 &&
                m1 >= 0 && m1 < 60 && m2 >= 0 && m2 < 60) {
                sessions_.emplace_back(h1 * 3600 + m1 * 60, h2 * 3600 + m2 * 60);
            } else {
                HFT_LOGW("obi_voi: ignoring malformed trade_sessions entry '%s'", one.c_str());
            }
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
        if (sessions_.empty()) {
            HFT_LOGW("obi_voi: trade_sessions='%s' parsed to nothing; no window set, "
                     "entries are allowed 24 h", sess.c_str());
        }
    }
    if (sessions_.empty()) {
        HFT_LOGI("obi_voi: no trade_sessions configured -> entries allowed 24 h "
                 "(set e.g. 21:00-02:30 to restrict)");
    }
    tradeVolume_ = cfg.getInt("obi_voi", "trade_volume", 1);
    maxPosition_ = cfg.getInt("obi_voi", "max_position", 1);
    cooldownTicks_ = cfg.getInt("obi_voi", "cooldown_ticks", 5);
    allowShort_ = cfg.getBool("obi_voi", "allow_short", true);
    flattenOnStart_ = cfg.getBool("obi_voi", "flatten_on_start", true);
    closeOnStop_ = cfg.getBool("obi_voi", "close_on_stop", true);

    if (instruments_.empty()) {
        err = "obi_voi: no instruments configured ([general] instruments)";
        return false;
    }
    if (windowTicks_ < 2) windowTicks_ = 2;
    if (windowTicks_ > static_cast<int>(State::kObiRing)) {
        HFT_LOGW("obi_voi: window_ticks=%d exceeds the %u-slot OBI ring - clamped",
                 windowTicks_, State::kObiRing);
        windowTicks_ = static_cast<int>(State::kObiRing);
    }
    if (obiThreshold_ <= 0.0 || obiThreshold_ >= 1.0) obiThreshold_ = 0.6;
    if (voiEmaTicks_ < 1) voiEmaTicks_ = 1;
    if (voiFlipThreshold_ < 0.0) voiFlipThreshold_ = 0.0;
    if (entryTimeoutTicks_ < 1) entryTimeoutTicks_ = 1;
    if (entrySlippageTicks_ < 0) entrySlippageTicks_ = 0;
    if (profitTicks_ < 1) profitTicks_ = 1;
    if (closeTimeoutTicks_ < 1) closeTimeoutTicks_ = 1;
    if (stopLossTicks_ < 0) stopLossTicks_ = 0;
    if (voiEntryThreshold_ < 0.0) voiEntryThreshold_ = 0.0;
    if (tpRepostIntervalTicks_ < 1) tpRepostIntervalTicks_ = 1;
    if (tpRepostMax_ < 0) tpRepostMax_ = 0;
    if (stopSlippageTicks_ < 0) stopSlippageTicks_ = 0;
    if (maxSpreadTicks_ < 0) maxSpreadTicks_ = 0;
    if (trendWindowMs_ < 0) trendWindowMs_ = 0;
    if (trendMinTicks_ < 0) trendMinTicks_ = 0;
    if (entryStallTicks_ < 0) entryStallTicks_ = 0;
    if (stallWindowTicks_ < 1) stallWindowTicks_ = 1;
    if (stallWindowTicks_ > static_cast<int>(State::kTouchRing)) {
        stallWindowTicks_ = static_cast<int>(State::kTouchRing);
    }
    if (breakevenTicks_ < 0) breakevenTicks_ = 0;
    if (entryBlackoutSec_ < 0) entryBlackoutSec_ = 0;
    if (consecStopPause_ < 0) consecStopPause_ = 0;
    if (pauseSeconds_ < 0) pauseSeconds_ = 0;
    dailyLossLimitTicks_ = cfg.getInt("obi_voi", "daily_loss_limit_ticks", 35);
    if (dailyLossLimitTicks_ < 0) dailyLossLimitTicks_ = 0;
    if (tradeVolume_ < 1 || maxPosition_ < 1) {
        err = "obi_voi: trade_volume and max_position must be >= 1";
        return false;
    }

    HFT_LOGI("obi_voi: mode=%s window=%d obiTh=%.2f voiEma=%d voiFlip=%.2f voiEntry=%.2f | entryTimeout=%dt "
             "entrySlip=%dt taker=%s profit=%dt closeTimeout=%dt stopLoss=%dt stopSlip=%dt maxHold=%dt "
             "tpRepost=%d/%dt maxSpread=%dt trend=%dms/%dt vol=%d maxPos=%d short=%s | maxHoldSec=%d "
             "escapeRetry=%dms consecStop=%d/%ds stall=%dt/%dw be=%dt blackout=%ds dayLoss=%dt",
             makerMode_ ? "maker" : (followMode_ ? "follow" : "fade"), windowTicks_, obiThreshold_, voiEmaTicks_,
             voiFlipThreshold_, voiEntryThreshold_, entryTimeoutTicks_, entrySlippageTicks_,
             entryTaker_ ? "ioc" : "gfd",
             profitTicks_, closeTimeoutTicks_, stopLossTicks_, stopSlippageTicks_,
             maxHoldTicks_, tpRepostMax_, tpRepostIntervalTicks_, maxSpreadTicks_,
             trendWindowMs_, trendMinTicks_, tradeVolume_,
             maxPosition_, allowShort_ ? "yes" : "no",
             maxHoldSeconds_, escapeRetryMs_, consecStopPause_, pauseSeconds_,
             entryStallTicks_, stallWindowTicks_, breakevenTicks_, entryBlackoutSec_,
             dailyLossLimitTicks_);
    if (!sessions_.empty()) {
        std::string txt;
        char one[32];
        for (const auto& w : sessions_) {
            std::snprintf(one, sizeof(one), "%s%02d:%02d-%02d:%02d", txt.empty() ? "" : ",",
                          w.first / 3600, (w.first % 3600) / 60, w.second / 3600,
                          (w.second % 3600) / 60);
            txt += one;
        }
        HFT_LOGI("obi_voi: entry windows = %s (everything else: exits only)", txt.c_str());
    }
    return auditConfig(err);
}

// Report parameter deviations from the narrow historical sample. These are
// operator warnings, not universal invariants that should kill startup.
bool ObiVoiStrategy::auditConfig(std::string& err) const {
    (void)err;
    // These empirical values came from a narrow instrument/sample and are
    // diagnostics, not universal safety invariants. In particular, do not
    // terminate the process because a sim or another instrument uses different
    // settings. A missing trade_sessions window is not an error either: it
    // means "trade the whole day", which is the default configuration.
    if (sessions_.empty()) {
        HFT_LOGI("obi_voi: session gate disabled (no trade_sessions) - entries all day");
    }
    if (dailyLossLimitTicks_ <= 0) {
        HFT_LOGW("obi_voi: daily_loss_limit_ticks=0 - a runaway day is not bounded");
    }
    if (maxSpreadTicks_ == 0)
        HFT_LOGW("obi_voi: max_spread_ticks=0 disables the spread filter");
    if (profitTicks_ < 12)
        HFT_LOGW("obi_voi: profit_ticks=%d differs from the narrow historical sample; validate out of sample",
                 profitTicks_);
    return true;
}

ObiVoiStrategy::State& ObiVoiStrategy::state(const char* instrument) {
    if (State* s = findState(instrument)) return *s;
    if (statesN_ < kMaxStates) {
        StateEntry& e = states_[statesN_++];
        e.st = State();  // default-init (keeps the '0' sentinels / armed flags)
        copyStr(e.instrument, instrument);
        return e.st;
    }
    HFT_LOGE("obi_voi: more than %u instruments - %s shares the overflow slot",
             kMaxStates, instrument);
    return overflowState_;
}

ObiVoiStrategy::State* ObiVoiStrategy::findState(const char* instrument) {
    for (uint32_t i = 0; i < statesN_; ++i) {
        if (std::strncmp(states_[i].instrument, instrument, kInstrumentLen) == 0) {
            return &states_[i].st;
        }
    }
    return nullptr;
}

double ObiVoiStrategy::roundToTick(double price, double tickSize) const {
    if (tickSize <= 0) return price;
    return std::floor(price / tickSize + 0.5) * tickSize;
}

// Computes the exit bracket from the entry price, ONCE per round trip.
// After this the per-tick exit logic is a pair of plain comparisons - no
// multiplication, no division, no roundToTick inside the tick callback.
void ObiVoiStrategy::armBracket(State& st, double tickSize) {
    st.tpTrigger = 0.0;
    st.slTrigger = 0.0;
    st.tpPrice = 0.0;
    st.beArmed = false;
    if (st.entryPrice <= 0 || tickSize <= 0) return;
    const bool isLong = (st.entryDir == static_cast<char>(Dir::Buy));
    st.tpTrigger = isLong ? st.entryPrice + profitTicks_ * tickSize
                          : st.entryPrice - profitTicks_ * tickSize;
    st.tpPrice = roundToTick(st.tpTrigger, tickSize);
    if (stopLossTicks_ > 0) {
        st.slTrigger = isLong ? st.entryPrice - stopLossTicks_ * tickSize
                              : st.entryPrice + stopLossTicks_ * tickSize;
    }
}

// Price-based stop loss. The ONLY justification for an IOC escape (paying
// the spread a second time) is the market actually moving stop_loss_ticks
// against the entry. Measured on the exit touch (long -> bid1, short ->
// ask1): that is the price the IOC would fill at, so this is the true
// realized loss, not a mid-price illusion. VOI flips are noise and never
// reach this function.
bool ObiVoiStrategy::stopLossHit(const State& st, const TickMsg& tick, int net) const {
    if (st.slTrigger <= 0 || net == 0) return false;  // bracket armed at the fill
    const Dir closeDir = net > 0 ? Dir::Sell : Dir::Buy;
    const double touch = opponentPrice(tick, closeDir);
    if (touch <= 0) return false;
    return net > 0 ? (touch <= st.slTrigger + 1e-9) : (touch >= st.slTrigger - 1e-9);
}

// True once the drift window actually spans enough time to mean anything.
// Without this the "0 drift" of a cold window would read as "price has not
// overshot" and block every entry - a silent total trading stop.
bool ObiVoiStrategy::midDriftReady(const State& st) const {
    if (trendWindowMs_ <= 0) return false;
    if (st.midCount < 8) return false;
    const int64_t spanNs = st.midCkAt(st.midCount - 1).ns - st.midCkAt(0).ns;
    return spanNs >= static_cast<int64_t>(trendWindowMs_) * INT64_C(400000);
}

double ObiVoiStrategy::midDriftTicks(const State& st, double tickSize) const {
    if (!midDriftReady(st) || tickSize <= 0) return 0.0;
    const State::MidCkpt& newest = st.midCkAt(st.midCount - 1);
    const int64_t target = newest.ns - static_cast<int64_t>(trendWindowMs_) * INT64_C(1000000);
    // First checkpoint at or inside the window edge - the same rule the old
    // deque prune applied (front = first sample still within the window).
    uint32_t idx = 0;
    while (idx + 1 < st.midCount && st.midCkAt(idx).ns < target) ++idx;
    return (newest.mid - st.midCkAt(idx).mid) / tickSize;
}

// The weak side of the book: our quote joins the side with less pressure.
// buy -> rest on the bid, sell -> rest on the ask. Never deeper levels.
double ObiVoiStrategy::weakSidePrice(const TickMsg& tick, Dir dir) const {
    if (dir == Dir::Buy) {
        if (tick.bidPrice1 > 0) return tick.bidPrice1;
        if (tick.lowerLimitPrice > 0) return tick.lowerLimitPrice;
        return tick.lastPrice;
    }
    if (tick.askPrice1 > 0) return tick.askPrice1;
    if (tick.upperLimitPrice > 0) return tick.upperLimitPrice;
    return tick.lastPrice;
}

// The opponent price: crosses the spread, fills immediately.
double ObiVoiStrategy::opponentPrice(const TickMsg& tick, Dir dir) const {
    if (dir == Dir::Buy) {
        if (tick.askPrice1 > 0) return tick.askPrice1;
        if (tick.upperLimitPrice > 0) return tick.upperLimitPrice;
        return tick.lastPrice;
    }
    if (tick.bidPrice1 > 0) return tick.bidPrice1;
    if (tick.lowerLimitPrice > 0) return tick.lowerLimitPrice;
    return tick.lastPrice;
}

// ---------------------------------------------------------------------------
//  indicators
// ---------------------------------------------------------------------------
void ObiVoiStrategy::updateIndicators(State& st, const TickMsg& tick) {
    st.tickCount++;

    // ---- OBI: normalized best level book imbalance -------------------------
    const double bv = static_cast<double>(tick.bidVolume1);
    const double av = static_cast<double>(tick.askVolume1);
    if (bv + av > 0) st.obiPush((bv - av) / (bv + av));

    // ---- touch history for the stall filter --------------------------------
    if (tick.bidPrice1 > 0 && tick.askPrice1 > 0) {
        st.touchPush(tick.bidPrice1, tick.askPrice1);
    }

    // ---- short-horizon mid drift (trend filter input) -----------------------
    // One checkpoint per second in a 32-slot ring (see State::midCk) - the
    // right edge always tracks the latest tick, the window lookup happens at
    // read time in midDriftTicks(). O(1) per tick, zero allocation.
    if (trendWindowMs_ > 0 && tick.bidPrice1 > 0 && tick.askPrice1 > 0) {
        st.midUpdate(nowNs(), (tick.bidPrice1 + tick.askPrice1) * 0.5);
    }

    // ---- VOI: smoothed, normalized aggressor flow ---------------------------
    // Aggressor volume for this tick = delta of the cumulative traded volume,
    // signed by the direction of the last price move (up = buyers lifting the
    // ask, down = sellers hitting the bid). Unchanged price contributes 0.
    double dv = 0.0;
    if (st.prevVolume > 0 && tick.volume > st.prevVolume) {
        dv = tick.volume - st.prevVolume;
    }
    st.prevVolume = tick.volume > 0 ? tick.volume : st.prevVolume;

    double sgn = 0.0;
    if (st.prevPrice > 0) {
        if (tick.lastPrice > st.prevPrice) sgn = 1.0;
        else if (tick.lastPrice < st.prevPrice) sgn = -1.0;
    }
    st.prevPrice = tick.lastPrice;

    const double alpha = 2.0 / (static_cast<double>(voiEmaTicks_) + 1.0);
    st.voiSum += alpha * (sgn * dv - st.voiSum);
    st.voiDen += alpha * (dv - st.voiDen);
    st.voi = (st.voiDen > 1e-9) ? (st.voiSum / st.voiDen) : 0.0;

    // Re-arm the signal flags: each zero crossing of VOI allows exactly one
    // shot, so a permanently negative VOI cannot machine-gun sell signals.
    if (st.voi > 0) st.armedBuy = true;
    if (st.voi < 0) st.armedSell = true;

    if (st.cooldownTicks > 0) --st.cooldownTicks;

    // per-tick research line: forward-return analysis of the OBI/VOI signal
    // is done offline from the log, so the signal design stops being guesswork
    HFT_LOGD("tickdat %s last=%.2f bid=%.2f ask=%.2f bv=%u av=%u obi=%+.2f voi=%+.2f",
             tick.instrument, tick.lastPrice, tick.bidPrice1, tick.askPrice1,
             tick.bidVolume1, tick.askVolume1,
             st.obiCount ? st.obiBack() : 0.0, st.voi);
}

// ---------------------------------------------------------------------------
//  onTick
// ---------------------------------------------------------------------------
void ObiVoiStrategy::onTick(const TickMsg& tick) {
    if (tick.magic != kMagicTick) return;
    if (tick.lastPrice <= 0) return;

    // Exchange trading day first: the loss budget belongs to the day, and a
    // fresh session must not inherit yesterday's halt.
    rollTradingDay(tick.tradingDay);

    const int64_t tickNowNs = nowNs();
    if (!firstTickLogged_) {
        firstTickLogged_ = true;
        logFirstTick(tick);
    }
    logGateReport(tickNowNs, tick);

    State& st = state(tick.instrument);
    updateIndicators(st, tick);

    // ---- 1) leftover positions have absolute priority -----------------------
    if (flattenOnStart_) startupGate(st, tick);

    // ---- 2) a working order: manage it, no new decisions --------------------
    if (st.pending.active || !st.flattenEmpty()) {
        manageWorkingOrder(st, tick);
        return;
    }

    // ---- 2.4) order report said "filled", trade report still in flight ------
    // A short grace window: while it is open we neither re-send nor open a new
    // position. Once it expires the fill counts as a phantom and the round trip
    // is reset (the position book, if it really did move, is rebuilt in step 3).
    if (st.awaitTradeUntilNs) {
        if (nowNs() < st.awaitTradeUntilNs) return;
        st.awaitTradeUntilNs = 0;
        if (ctx_->position(tick.instrument) == 0) {
            // Two-stage entry: the entry IOC was priced exactly at the opponent
            // touch. When it died unfilled the level was too thin at that
            // instant - retry ONCE crossing a tick, instead of paying that tick
            // on every single entry.
            if (st.retryEntryDir != '0' && nowNs() <= st.retryEntryUntilNs) {
                const Dir dir = static_cast<Dir>(st.retryEntryDir);
                st.retryEntryDir = '0';
                st.retryEntryUntilNs = 0;
                HFT_LOGI("%s: entry IOC unfilled -> retry crossing 1 tick", tick.instrument);
                sendEntry(st, tick, dir, 1, followMode_);
                return;
            }
            st.retryEntryDir = '0';
            HFT_LOGW("%s: fill never confirmed by a trade report -> reset round trip",
                     tick.instrument);
            resetRoundTrip(st);
        }
    }

    // ---- 2.5) reconcile the local book against the broker snapshot ----------
    // SimNow's replay counter is known to deliver phantom fills and to hold
    // positions its own query store does not know about. When no order is in
    // flight and the discrepancy survives 3 query rounds, the broker wins.
    reconcileBook(st, tick);

    // ---- 3) holding without a working close (report lost / IOC died) --------
    // Bracket policy (fade mode): a take-profit LIMIT rests at entry +/-
    // profit_ticks. While it is alive manageWorkingOrder guards it - it is
    // NEVER pulled for anything but the stop trigger or the time limit.
    // When it is not alive (fill report lost, counter auto-killed it) this
    // branch re-arms it, throttled to tp_repost_interval_ticks so a counter
    // that auto-cancels resting limits can not turn us into a re-quote
    // storm. If the touch has already reached the target there is no point
    // in re-arming anything - an IOC captures the profit immediately. Once
    // tp_repost_max is exhausted the exit is purely price-triggered IOC.
    const int net = ctx_->position(tick.instrument);

    // ---- 3.0) wall-clock hard stop ------------------------------------------
    // Independent of phase: a position that has been open too long leaves at
    // any price. Covers both the book hovering between TP and SL (where the
    // bracket never triggers) and an exit that died unfilled.
    const bool holdExpired =
        net != 0 && maxHoldSeconds_ > 0 && st.openTimeNs > 0 &&
        (nowNs() - st.openTimeNs) >=
            static_cast<int64_t>(maxHoldSeconds_) * INT64_C(1000000000);

    // ---- 3a) ESCAPING: an escape IOC that dies unfilled MUST be retried ------
    // Phase::Escaping is entered when the escape IOC goes out and, before this
    // fix, the only exit was a fill - the whole block below is skipped while
    // escaping. The 7x24 front cancels IOCs with filled=0 all the time, so on
    // 2026-11-05 18:40:28 one ag2612 short (entry 12382) was left open with
    // nothing on the wire: 51 753 dead ticks / 14 h 23 m, straight through the
    // whole night session. An escape is a promise - keep hitting the book until
    // it is flat, widening the cross every few attempts so a fast market cannot
    // keep missing the fill.
    if (net != 0 && st.phase == Phase::Escaping) {
        ++st.holdTicks;
        const int64_t now = nowNs();
        if (st.escapeSinceNs == 0) st.escapeSinceNs = now;
        const int64_t gapNs = static_cast<int64_t>(escapeRetryMs_) * 1000000LL;
        if (st.lastEscapeNs == 0 || holdExpired || now - st.lastEscapeNs >= gapNs) {
            if (st.escapeRetries >= kMaxEscapeRetries) {
                HFT_LOGE("%s: ESCAPE EXHAUSTED - %d attempts over %lld s and still net=%d; "
                         "halt new entries, the exit needs a human",
                         tick.instrument, st.escapeRetries,
                         static_cast<long long>((now - st.escapeSinceNs) / INT64_C(1000000000)),
                         net);
                ctx_->haltTrading();
                return;
            }
            // Widen fast. A limit IOC prints at whatever the book offers, so
            // the cross width costs nothing on a liquid level - it only decides
            // whether the order reaches a level that still has volume. The old
            // "one tick every 3 attempts" cadence could not keep up with a
            // running market: 2026-11-06 11:08 a long stop needed THREE IOC
            // attempts (12433 -> 12431 -> 12425) and filled 6 ticks worse than
            // the trigger, turning a -6 into a -13.
            const int widen = (st.escapeRetries / 2) > 6 ? 6 : (st.escapeRetries / 2);
            // walk the SHFE buckets so a yesterday-bucket position (a fill from
            // before the night session rolled the trading day) is not refused
            // with ErrorID 50 forever
            static const char kRing[3] = {'3', '4', '1'};
            const char off = kRing[st.escapeRetries % 3];
            HFT_LOGW("ESCAPE %s: retry #%d (net=%d, held %lld s) -> IOC cross +%dt offset=%c",
                     tick.instrument, st.escapeRetries + 1, net,
                     static_cast<long long>((now - st.escapeSinceNs) / INT64_C(1000000000)),
                     stopSlippageTicks_ + widen, off);
            sendCloseIoc(st, tick, stopSlippageTicks_ + widen, off);
        }
        return;
    }

    if (net != 0) {
        if (st.entryPrice <= 0) st.entryPrice = ctx_->openPrice(tick.instrument);
        if (st.entryPrice <= 0) st.entryPrice = tick.lastPrice;
        if (st.entryDir == '0') st.entryDir = net > 0 ? static_cast<char>(Dir::Buy)
                                                      : static_cast<char>(Dir::Sell);
        ++st.holdTicks;
        const int posSign = net > 0 ? 1 : -1;
        if (st.voi * posSign < -voiFlipThreshold_) ++st.adverseTicks;
        else st.adverseTicks = 0;
        // ---- breakeven ratchet ---------------------------------------------
        // Once the trip has printed breakeven_ticks of profit, the stop moves
        // to the entry price (it never moves back). On 2026-11-10, 13 of 31
        // round trips reached >= +7 ticks and 5 still closed at <= -6: the
        // bracket kept waiting for +8 while the market gave it all back.
        // The exit on the ratcheted stop still pays stop slippage, so the
        // worst case after arming is roughly -1 tick minus fees, not -8.
        if (breakevenTicks_ > 0 && !st.beArmed && st.entryPrice > 0 && st.slTrigger > 0) {
            const double ts = ctx_->priceTick(tick.instrument);
            const double touchNow = posSign > 0 ? tick.bidPrice1 : tick.askPrice1;
            if (ts > 0 && touchNow > 0) {
                const double fav = (touchNow - st.entryPrice) * posSign / ts;
                if (fav >= breakevenTicks_ - 1e-9) {
                    st.beArmed = true;
                    st.slTrigger = st.entryPrice;
                    HFT_LOGI("BREAKEVEN %s: %+.0ft reached -> stop moves to entry %.2f",
                             tick.instrument, fav, st.entryPrice);
                }
            }
        }
        const bool stopped = stopLossHit(st, tick, net);
        const bool expired = (maxHoldTicks_ > 0 && st.holdTicks >= maxHoldTicks_);
        if (stopped || expired || holdExpired) {
            HFT_LOGW("ESCAPE %s: %s -> IOC stop order (entry=%.2f voi=%+.2f net=%d hold=%dt)",
                     tick.instrument,
                     stopped   ? "price stop loss hit"
                     : expired ? "hold expired"
                               : "wall-clock hold limit",
                     st.entryPrice, st.voi, net, st.holdTicks);
            sendCloseIoc(st, tick, stopSlippageTicks_);
        } else if (makerMode_) {
            sendClosePassive(st, tick);
        } else if (st.entryPrice > 0) {
            // Bracket prices come from armBracket (computed once at the fill);
            // the lazy arm covers a fill that happened before the tick size
            // was known. Per tick this branch is now: one lazy check + two
            // comparisons.
            const double tickSize = ctx_->priceTick(tick.instrument);
            if (st.tpTrigger <= 0 && tickSize > 0) armBracket(st, tickSize);
            const double touch = posSign > 0 ? tick.bidPrice1 : tick.askPrice1;
            const double trigger = st.tpTrigger > 0
                ? st.tpTrigger
                : (tickSize > 0 ? (posSign > 0 ? st.entryPrice + profitTicks_ * tickSize
                                              : st.entryPrice - profitTicks_ * tickSize)
                                : 0.0);
            if (trigger > 0 && touch > 0 &&
                (posSign > 0 ? touch >= trigger - 1e-9 : touch <= trigger + 1e-9)) {
                // Limit the IOC at the target, not at the touch: if the book
                // runs past the target the fill still prints at the better bid
                // (ask), and if it snaps back within the few hundred ms the
                // order spends on the wire we are protected from a worse print.
                const double target = st.tpPrice > 0 ? st.tpPrice
                                                     : roundToTick(trigger, tickSize);
                HFT_LOGI("TARGET %s: touch %.2f reached entry%+dt with no TP resting "
                         "-> take it now (IOC @ %.2f, never worse)",
                         tick.instrument, touch, profitTicks_, target);
                sendCloseIoc(st, tick, 0, '3', target);
            } else {
                ++st.ticksSinceTpPost;
                if (st.ticksSinceTpPost >= tpRepostIntervalTicks_ &&
                    st.tpPosts < tpRepostMax_) {
                    sendClosePassive(st, tick);
                }
            }
        }
        return;
    }

    // ---- 4) flat and idle: hunt for the entry signal ------------------------
    evaluate(st, tick);
}

// ---------------------------------------------------------------------------
//  Local book vs broker snapshot reconciliation. Only trusted when nothing is
//  in flight and the mismatch persists across 3 query rounds (~9 s), so a
//  lagging snapshot right after a fill can never trigger it.
// ---------------------------------------------------------------------------
void ObiVoiStrategy::reconcileBook(State& st, const TickMsg& tick) {
    if (!st.tradingStarted || !ctx_->snapshotReady()) return;
    const uint32_t round = ctx_->snapshotQueryRound();
    if (round == st.lastReconcileRound) return;  // same snapshot, already seen
    st.lastReconcileRound = round;

    const int local = ctx_->position(tick.instrument);
    BrokerPosition bp;
    const int broker = ctx_->brokerPosition(tick.instrument, bp) ? bp.net : 0;
    if (local == broker) {
        st.mismatchRounds = 0;
        return;
    }
    if (++st.mismatchRounds < 3) return;

    HFT_LOGW("RECONCILE %s: local net=%d disagrees with broker net=%d for %d rounds "
             "-> adopting the broker book",
             tick.instrument, local, broker, st.mismatchRounds);
    ctx_->adoptPosition(tick.instrument, broker, tick.lastPrice);
    st.mismatchRounds = 0;
    if (broker == 0) {
        // a phantom local position evaporated: reset the round trip state
        resetRoundTrip(st);
    } else if (st.entryPrice <= 0) {
        // the broker has a position we did not know about: remember the entry
        st.entryPrice = ctx_->openPrice(tick.instrument);
        if (st.entryPrice <= 0) st.entryPrice = tick.lastPrice;
        st.entryDir = broker > 0 ? static_cast<char>(Dir::Buy) : static_cast<char>(Dir::Sell);
        armBracket(st, ctx_->priceTick(tick.instrument));
    }
}

// The entry signal: extreme imbalance (condition A) + decaying flow that has
// crossed the zero axis against the extreme (condition B).
// ---------------------------------------------------------------------------
//  Session gate: is this tick inside one of the configured entry windows?
//  Uses the EXCHANGE timestamp carried by the tick, not the wall clock, so a
//  replayed or delayed tick cannot open a position in the wrong session.
//  Fails open on an unparseable timestamp: standing aside forever because of
//  a malformed field would be worse than trading the full day.
// ---------------------------------------------------------------------------
bool ObiVoiStrategy::inSession(const TickMsg& tick) const {
    if (sessions_.empty()) return true;   // no window configured = trade 24 h
    int h = 0, m = 0, s = 0;
    if (std::sscanf(tick.updateTime, "%d:%d:%d", &h, &m, &s) != 3 ||
        h < 0 || h > 23 || m < 0 || m > 59 || s < 0 || s > 59) return false;
    const int t = h * 3600 + m * 60 + s;
    for (const auto& w : sessions_) {
        if (w.first <= w.second) {
            if (t >= w.first && t < w.second) return true;
        } else if (t >= w.first || t < w.second) {
            return true;  // window crosses midnight (night session)
        }
    }
    return false;
}

// A position opened seconds before the close cannot be managed: on 2026-11-10
// the run bought ag2702 at 14:59:15 and the close order at 15:21:04 was refused
// by SHFE ("当前状态禁止此项操作"). No new entries in the last blackout of a
// window; exits are unaffected (a position must always be closable).
bool ObiVoiStrategy::nearSessionEnd(const TickMsg& tick) const {
    if (sessions_.empty() || entryBlackoutSec_ <= 0) return false;
    int h = 0, m = 0, s = 0;
    if (std::sscanf(tick.updateTime, "%d:%d:%d", &h, &m, &s) != 3 ||
        h < 0 || h > 23 || m < 0 || m > 59 || s < 0 || s > 59) return true;
    const int t = h * 3600 + m * 60 + s;
    const int day = 24 * 3600;
    for (const auto& w : sessions_) {
        const bool inWin = (w.first <= w.second)
                               ? (t >= w.first && t < w.second)
                               : (t >= w.first || t < w.second);
        if (!inWin) continue;
        int until = w.second - t;
        if (until < 0) until += day;          // window ends after midnight
        if (until > 0 && until <= entryBlackoutSec_) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
//  Operator visibility: an idle strategy must never be silent.
//  Historical failure this fixes (2026-11-11): the day session produced ZERO
//  orders for six hours; every heartbeat read "ticks=2/s execs=0" so the run
//  looked healthy. The real cause - trade_sessions = 21:00-02:30 - returned
//  from evaluate() three lines in and never logged anything.
// ---------------------------------------------------------------------------
void ObiVoiStrategy::logFirstTick(const TickMsg& tick) {
    int h = 0, m = 0, s = 0;
    const bool parsed = std::sscanf(tick.updateTime, "%d:%d:%d", &h, &m, &s) == 3;
    char clock[16] = "??:??:??";
    if (parsed) std::snprintf(clock, sizeof(clock), "%02d:%02d:%02d", h, m, s);
    if (sessions_.empty()) {
        HFT_LOGI("obi_voi: first tick %s at %s - no entry window configured, gate open all day",
                 tick.instrument, clock);
        return;
    }
    if (inSession(tick)) {
        HFT_LOGI("obi_voi: first tick %s at %s - INSIDE an entry window", tick.instrument, clock);
        return;
    }
    int best = -1;
    if (parsed) {
        const int t = h * 3600 + m * 60 + s;
        for (const auto& w : sessions_) {
            int d = w.first - t;
            if (d <= 0) d += 24 * 3600;
            if (best < 0 || d < best) best = d;
        }
    }
    if (best >= 0) {
        HFT_LOGW("obi_voi: first tick %s at %s - OUTSIDE every entry window; "
                 "no order for the next %02d:%02d (exits are never gated)",
                 tick.instrument, clock, best / 3600, (best % 3600) / 60);
    } else {
        HFT_LOGW("obi_voi: first tick %s at %s - OUTSIDE every entry window",
                 tick.instrument, clock);
    }
}

void ObiVoiStrategy::logGateReport(int64_t now, const TickMsg& tick) {
    if (lastGateLogNs_ == 0) {
        lastGateLogNs_ = now;
        return;
    }
    if (now - lastGateLogNs_ < INT64_C(60000000000)) return;  // 60 s
    lastGateLogNs_ = now;
    const uint64_t seen = gateTicks_;
    const uint64_t blocked = gateSession_ + gateBlackout_ + gateHalt_ + gateSpread_;
    const uint64_t sSession = gateSession_, sCutoff = gateBlackout_;
    const uint64_t sHalt = gateHalt_, sSpread = gateSpread_;
    gateTicks_ = gateSession_ = gateBlackout_ = gateHalt_ = gateSpread_ = 0;
    if (seen == 0 || blocked == 0) return;
    HFT_LOGW("%s: no entry order in the last 60 s - %llu/%llu ticks blocked "
             "| outside_session=%llu entry_cutoff=%llu daily_halt=%llu wide_spread=%llu",
             tick.instrument, static_cast<unsigned long long>(blocked),
             static_cast<unsigned long long>(seen),
             static_cast<unsigned long long>(sSession),
             static_cast<unsigned long long>(sCutoff),
             static_cast<unsigned long long>(sHalt),
             static_cast<unsigned long long>(sSpread));
}

void ObiVoiStrategy::evaluate(State& st, const TickMsg& tick) {
    ++gateTicks_;
    if (st.obiCount < static_cast<uint32_t>(windowTicks_)) return;  // warm-up
    if (st.cooldownTicks > 0) return;

    // ---- session gate: open when no window is configured (default), else only
    // inside the configured windows ------------------------------------------
    if (!inSession(tick)) { ++gateSession_; return; }
    // ... and not in the last minutes of one: a position opened there cannot
    // be closed before the exchange shuts (2026-11-10 14:59:15 entry).
    // No window -> no cutoff, because there is no close to protect against.
    if (nearSessionEnd(tick)) { ++gateBlackout_; return; }

    // ---- daily loss circuit breaker -----------------------------------------
    // Checked before anything else that could open a position: once the day's
    // realised loss reaches the limit, a fade strategy in a trending tape would
    // otherwise keep buying the same falling knife until the session ends.
    if (dailyHalted_) { ++gateHalt_; return; }

    // ---- streak guard: stand down while the market is trending against us ---
    if (st.pauseUntilNs) {
        if (nowNs() < st.pauseUntilNs) return;
        st.pauseUntilNs = 0;
        HFT_LOGI("%s: stand-down finished, hunting signals again", tick.instrument);
    }

    // ---- spread guard: do not pay an unusual spread ------------------------
    // The bracket target is measured from the FILL, not from the mid, so every
    // extra tick of spread is a tick the mid must travel before the TP can
    // print, while the stop sits that much closer. Measured on ag2612 the book
    // is 2 ticks wide at the median but 3+ in the first minutes of a session
    // and around news. Standing aside while it is wider than max_spread_ticks
    // removes exactly the entries with the worst cost/edge ratio.
    if (maxSpreadTicks_ > 0 && tick.bidPrice1 > 0 && tick.askPrice1 > 0) {
        const double ts = ctx_->priceTick(tick.instrument);
        if (ts > 0 &&
            (tick.askPrice1 - tick.bidPrice1) > maxSpreadTicks_ * ts + 1e-9) {
            ++gateSpread_;
            return;
        }
    }

    if (!ctx_->tradingEnabled()) return;
    if (ctx_->position(tick.instrument) != 0) return;  // inventory cap: only closes
    BrokerPosition bp;
    if (ctx_->brokerPosition(tick.instrument, bp) && bp.net != 0) return;
    if (!st.startupDone) return;

    // ---- follow mode: ride the imbalance extreme, do NOT fade it -----------
    // The whole fade apparatus below (drift gate, stall filter, VOI decay
    // confirmation) exists to answer "has the move exhausted itself". The
    // 2026-11-12 measurement said the answer does not pay on this tape: at
    // minute horizons the extreme CONTINUES (day session +2.15t over 480
    // ticks, t=1.95) and every confirmation filter we tried (VOI sign, VOI
    // magnitude, high-vol regime) made the edge worse. So follow mode is
    // deliberately dumb: the current OBI is extreme -> go with it. No
    // fadeSign is set, which also keeps the VOI pull-guard inert.
    if (followMode_) {
        const double obiNow = st.obiBack();
        if (obiNow >= obiThreshold_) {
            st.signalCount++;
            HFT_LOGI("SIGNAL %s FOLLOW-BID-EXTREME obi=%.2f -> BUY @ bid %.2f",
                     tick.instrument, obiNow, tick.bidPrice1);
            sendEntry(st, tick, Dir::Buy);
        } else if (obiNow <= -obiThreshold_ && allowShort_) {
            st.signalCount++;
            HFT_LOGI("SIGNAL %s FOLLOW-ASK-EXTREME obi=%.2f -> SELL @ ask %.2f",
                     tick.instrument, obiNow, tick.askPrice1);
            sendEntry(st, tick, Dir::Sell);
        }
        return;
    }

    // ---- maker mode: quote the touch on the side of the last print ---------
    // In a 1-tick oscillating replay the only positive-expectancy structure
    // is passive/passive spread capture: join the bid right after a seller
    // printed there, then offer one tick higher once filled.
    if (makerMode_) {
        if (st.entryQuoteTicks >= entryTimeoutTicks_) return;  // campaign expired
        const bool sellPrint = (tick.bidPrice1 > 0 && tick.lastPrice <= tick.bidPrice1);
        const bool buyPrint = (tick.askPrice1 > 0 && tick.lastPrice >= tick.askPrice1);
        const bool bidThin = (makerThinThreshold_ <= 0 ||
                              tick.bidVolume1 <= makerThinThreshold_);
        const bool askThin = (makerThinThreshold_ <= 0 ||
                              tick.askVolume1 <= makerThinThreshold_);
        if (sellPrint && bidThin && st.armedBuy) {
            st.fadeSign = -1;   // flow direction guard sign (see manageWorkingOrder)
            st.armedBuy = false;
            st.signalCount++;
            ++st.entryQuoteTicks;
            HFT_LOGI("SIGNAL %s MAKER-BUY after sell print -> quote bid %.2f",
                     tick.instrument, tick.bidPrice1);
            sendEntry(st, tick, Dir::Buy);
        } else if (buyPrint && askThin && st.armedSell && allowShort_) {
            st.fadeSign = +1;
            st.armedSell = false;
            st.signalCount++;
            ++st.entryQuoteTicks;
            HFT_LOGI("SIGNAL %s MAKER-SELL after buy print -> quote ask %.2f",
                     tick.instrument, tick.askPrice1);
            sendEntry(st, tick, Dir::Sell);
        }
        return;
    }

    // ---- condition A: one side reached the extreme in the window ------------
    double maxO = -2.0, minO = 2.0;
    for (uint32_t i = 0; i < st.obiCount; ++i) {
        const double o = st.obiAt(i);
        if (o > maxO) maxO = o;
        if (o < minO) minO = o;
    }
    const bool buyExtreme = (maxO > obiThreshold_);   // bids dominated the book
    const bool sellExtreme = (minO < -obiThreshold_); // asks dominated the book
    if (buyExtreme && sellExtreme) return;            // unstable window, stand aside

    // ---- condition D: the price must have ALREADY overshot ---------------
    // This is the core of what "fade" means, and it is the one thing that
    // separated winners from losers in 69 measured round trips (11-05 724
    // session + 11-06 day session, incl. 4 phantom trips removed):
    //
    //   entry AGAINST a 30 s drift of 0..5 ticks : 9 trades, +40 ticks, 67 % win
    //   entry AGAINST a drift > 5 ticks         : 16 trades, +16 ticks, 50 % win
    //   entry WITH a drift of 0..5 ticks        : 14 trades, -81 ticks, 14 % win
    //   entry WITH a drift > 5 ticks            : 30 trades, -72 ticks, 23 % win
    //
    // So the edge is NOT "price is moving my way" (that is momentum, and the
    // OBI extreme has already priced it in) - it is "price has run away from
    // the book and the dominant side is starting to pull". Require the drift
    // to point AGAINST the entry by >= trend_min_ticks.
    // Fail open ONLY when the window has no coverage yet (a fresh process, or a
    // data gap): there the drift was simply never measured, so it must not veto
    // anything. Once the window is live the filter is strict.
    const bool trendActive = midDriftReady(st) && trendMinTicks_ > 0;
    const double drift = midDriftTicks(st, ctx_->priceTick(tick.instrument));
    const bool driftUpOk = (!trendActive || drift >= trendMinTicks_ - 1e-9);
    const bool driftDownOk = (!trendActive || drift <= -trendMinTicks_ + 1e-9);

    // ---- condition E: the impulse must have STOPPED printing extremes ------
    // Drift is a 30 s average: it cannot tell an overshoot that is 20 s old
    // (the good fade) from one that started 2 s ago (the one that runs us
    // over). Require the touch to have come entry_stall_ticks off the extreme
    // of the last stall_window_ticks ticks. Every <=10 s stop-out in the
    // 2026-11-10 day session (8 trips, -40 ticks) fails this test.
    if (entryStallTicks_ > 0 && tick.bidPrice1 > 0 && tick.askPrice1 > 0) {
        const double tsz = ctx_->priceTick(tick.instrument);
        if (tsz > 0) {
            const double need = entryStallTicks_ * tsz;
            const double k = static_cast<uint32_t>(stallWindowTicks_);
            const double lowOff = tick.bidPrice1 - st.bidMin(static_cast<uint32_t>(k));
            const double highOff = st.askMax(static_cast<uint32_t>(k)) - tick.askPrice1;
            const bool buyStalled = (lowOff >= need - 1e-9);   // came off the low
            const bool sellStalled = (highOff >= need - 1e-9); // came off the high
            if (!buyStalled && !sellStalled) {
                HFT_LOGD("SKIP %s: impulse still extending (off low %.0ft, off high %.0ft, "
                         "need %dt) - not a fade yet", tick.instrument,
                         lowOff / tsz, highOff / tsz, entryStallTicks_);
                return;
            }
            if (buyExtreme && !sellStalled) return;
            if (sellExtreme && !buyStalled) return;
        }
    }

    // ---- condition B: the flow behind the extreme just crossed zero --------
    // Tightened (2026-11-05 backtest, 21.5k ticks): the sign-only flip fired
    // on chop noise. Now require (1) a minimum reversal magnitude
    // (voi_entry_threshold) and (2) decay confirmation: the current OBI must
    // have RECEDED from the extreme - the dominant side is actually pulling
    // its quotes, not just pausing. Fewer, better signals.
    constexpr double kDecayMargin = 0.05;  // current obi must be inside
                                           // threshold - margin (receded)
    const double obiNow = st.obiBack();
    const bool decayedB = obiNow < obiThreshold_ - kDecayMargin;  // buy extreme receded
    const bool decayedS = obiNow > -(obiThreshold_ - kDecayMargin);  // sell extreme receded
    if (buyExtreme && st.voi < -voiEntryThreshold_ && decayedB &&
        st.armedSell && allowShort_) {
        if (!driftUpOk) {
            HFT_LOGD("SKIP %s SELL: obiMax=%.2f confirms but drift=%+.1ft "
                     "(need >= +%dt, price must have run up first) - not overshot yet",
                     tick.instrument, maxO, drift, trendMinTicks_);
            return;
        }
        // Buy pressure exhausted and withdrawing -> fade it: passive SELL on
        // the weak (ask) side. If VOI turns positive again the order is
        // pulled before it can be adversely filled.
        st.fadeSign = +1;
        st.armedSell = false;
        st.signalCount++;
        HFT_LOGI("SIGNAL %s FADE-BUY-EXTREME obiMax=%.2f obiNow=%.2f voi=%+.2f drift=%+.1ft -> SELL @ ask %.2f",
                 tick.instrument, maxO, obiNow, st.voi, drift, tick.askPrice1);
        sendEntry(st, tick, Dir::Sell);
    } else if (sellExtreme && st.voi > voiEntryThreshold_ && decayedS &&
               st.armedBuy) {
        if (!driftDownOk) {
            HFT_LOGD("SKIP %s BUY: obiMin=%.2f confirms but drift=%+.1ft "
                     "(need <= -%dt, price must have run down first) - not overshot yet",
                     tick.instrument, minO, drift, trendMinTicks_);
            return;
        }
        st.fadeSign = -1;
        st.armedBuy = false;
        st.signalCount++;
        HFT_LOGI("SIGNAL %s FADE-SELL-EXTREME obiMin=%.2f obiNow=%.2f voi=%+.2f drift=%+.1ft -> BUY @ bid %.2f",
                 tick.instrument, minO, obiNow, st.voi, drift, tick.bidPrice1);
        sendEntry(st, tick, Dir::Buy);
    }
}

// Manages the working order (entry / close / IOC escape) on every tick.
// Cancels are REQUESTED here; the pending slot only clears when the
// final report arrives in onExec - "一切以回报为准".
void ObiVoiStrategy::manageWorkingOrder(State& st, const TickMsg& tick) {
    // Nothing can be sent while the counter says the session is closed.
    if (st.marketClosedUntilNs != 0 && nowNs() < st.marketClosedUntilNs) return;
    if (!st.flattenEmpty()) {
        pumpFlatten(st, tick);
        return;
    }
    if (!st.pending.active) return;
    // Cancel watchdog: a front drop can silently swallow a cancel request and
    // the order keeps resting at the counter (we saw it fill 2.5 minutes later).
    // If no terminal report arrives within 3 s, clear the flag so the pull
    // paths below re-issue the cancel on this tick.
    if (st.pending.cancelSent &&
        nowNs() - st.pending.cancelSentNs > INT64_C(3000000000)) {
        // Hard deadline: the terminal report never confirms at all (the 7x24
        // replay environment re-tags notifications so they are dropped by the
        // gateway). Declare the order dead and free the slot: if the position
        // is still open the "holding without a working close" branch escapes
        // it with an IOC next tick; if it actually filled, the reconcile step
        // adopts the broker book within a few query rounds.
        if (st.pending.firstCancelNs != 0 &&
            nowNs() - st.pending.firstCancelNs > INT64_C(10000000000)) {
            HFT_LOGW("PULL %s: cancel unconfirmed for 10 s -> declaring order dead "
                     "(slot released, reconcile/escape will resolve)",
                     tick.instrument);
            st.pending.active = false;
            st.pending.cancelSent = false;
            st.pending.firstCancelNs = 0;
            return;
        }
        HFT_LOGW("PULL %s: cancel not confirmed within 3 s -> resending",
                 tick.instrument);
        st.pending.cancelSent = false;
    }
    if (st.pending.cancelSent) return;  // cancel on the wire, await the report

    Pending& p = st.pending;
    ++p.ticksSinceSend;

    if (p.kind == OrderKind::Entry) {
        // (a) the decay call was wrong: VOI is accelerating back into the
        //     original extreme direction -> pull the order NOW, do not let
        //     it be adversely picked off. (Fade mode only: in maker mode a
        //     fill against strong flow IS the business - the fill is the
        //     spread capture, adverse runs are bounded by the hold stop.)
        if (!makerMode_ && st.fadeSign != 0 && st.voi * st.fadeSign > voiFlipThreshold_) {
            HFT_LOGW("PULL %s: entry wrong side - voi=%+.2f moved back into the %.2f "
                     "extreme -> cancel before an adverse fill",
                     tick.instrument, st.voi, obiThreshold_);
            requestCancel(st, tick.instrument);
            return;
        }
        // (b) plain timeout: an unfilled passive order accumulates adverse
        //     selection risk with every tick.  The tick counter alone is not
        //     enough: on the 7x24 front ticks arrive at 1-3/s, so 4 "ticks"
        //     can mean 2-4 seconds of exposure.  An absolute 800 ms ceiling
        //     makes the pull prompt regardless of the tick rate (the counter
        //     usually auto-cancels first - this is the belt to that braces).
        //     Follow mode is deliberately more patient: its edge lives for
        //     minutes, so the passive quote gets follow_patience_ms on the
        //     wall clock before the taker fallback; the tick count is ignored
        //     there because 2 tick/s tape would otherwise pull it in 2 s.
        const int64_t ageNs = p.sentTimeNs ? (nowNs() - p.sentTimeNs) : 0;
        const int64_t patienceNs =
            followMode_ ? static_cast<int64_t>(followPatienceMs_) * 1000000ll
                        : INT64_C(800000000);
        if ((!followMode_ && p.ticksSinceSend >= entryTimeoutTicks_) ||
            ageNs >= patienceNs) {
            HFT_LOGI("PULL %s: entry not filled within %d ticks / %lld ms -> cancel",
                     tick.instrument, entryTimeoutTicks_,
                     static_cast<long long>(ageNs / 1000000));
            requestCancel(st, tick.instrument);
        }
        return;
    }

    if (p.kind == OrderKind::ClosePassive) {
        const int net = ctx_->position(tick.instrument);
        // VOI bookkeeping is kept for reporting/diagnostics only. It used to
        // fire the IOC escape after close_timeout_ticks of adverse VOI, which
        // liquidated on chop noise (VOI flips every few ticks in a range) and
        // produced the -1/-2 tick loss streak. The escape now requires the
        // price itself to breach stop_loss_ticks against the entry.
        const int posSign = net > 0 ? 1 : -1;
        const bool adverse = (net != 0 && st.voi * posSign < -voiFlipThreshold_);
        if (adverse) ++st.adverseTicks; else st.adverseTicks = 0;
        ++st.holdTicks;  // counted from the entry fill, survives re-posts

        if (stopLossHit(st, tick, net)) {
            HFT_LOGW("ESCAPE %s: price stop hit (entry=%.2f net=%d adverse=%dt) -> "
                     "cancel and cross the spread (IOC)",
                     tick.instrument, st.entryPrice, net, st.adverseTicks);
            requestCancel(st, tick.instrument);
            return;
        }
        // Safety net: never hold longer than max_hold_ticks (time stop).
        if (maxHoldTicks_ > 0 && st.holdTicks >= maxHoldTicks_ && net != 0) {
            HFT_LOGW("ESCAPE %s: held %d ticks without a close -> cancel and cross the "
                     "spread (IOC)", tick.instrument, st.holdTicks);
            requestCancel(st, tick.instrument);
        }
        return;
    }

    if (p.kind == OrderKind::CloseIoc) {
        // An IOC that came back unfilled is re-quoted at the CURRENT opponent
        // price on the next tick (onExec clears the slot, this is a safety
        // net for a lost response).
        if (p.ticksSinceSend >= 2) {
            requestCancel(st, tick.instrument);
        }
    }
}

// ---------------------------------------------------------------------------
//  order helpers
// ---------------------------------------------------------------------------
void ObiVoiStrategy::sendEntry(State& st, const TickMsg& tick, Dir dir, int extraTicks,
                               bool forceTaker) {
    const int net = ctx_->position(tick.instrument);
    if (net != 0) return;  // inventory cap: at the limit only closes are allowed

    // Entry with a touch of slippage: start from the weak side of the book,
    // then cross entry_slippage_ticks toward the opponent (buy: bid1+N capped
    // at ask1, sell: ask1-N capped at bid1). A fully passive quote misses the
    // move and dies on the entry timeout; one tick through the book fills
    // promptly while never paying more than the touch.
    const double tickSize = ctx_->priceTick(tick.instrument);
    double px;
    char timeCond = '1';              // GFD - rests on the book
    const bool taker = entryTaker_ || forceTaker;
    if (taker) {
        // Cross the spread (+ slip) and demand an immediate fill.  A resting
        // quote on the SimNow 7x24 front is auto-cancelled 100-500 ms after it
        // lands, so a passive entry only burns the signal.
        //
        // entry_slippage_ticks is deliberately 0 in the live configs: an IOC
        // limited at ask1 already takes whatever the level offers, so paying an
        // extra tick only makes the fill price worse. extraTicks is the retry
        // path - the first attempt died unfilled, so this one goes one tick
        // through to guarantee it lands.
        px = opponentPrice(tick, dir);
        const int slipTicks = entrySlippageTicks_ + extraTicks;
        if (slipTicks > 0 && tickSize > 0) {
            const double slip = slipTicks * tickSize;
            px = (dir == Dir::Buy) ? px + slip : px - slip;
        }
        timeCond = '3';               // IOC / FAK: fill now or die
    } else {
        // Start from the weak side of the book, then cross entry_slippage_ticks
        // toward the opponent (buy: bid1+N capped at ask1, sell: ask1-N capped
        // at bid1). Never pays more than the touch.
        px = weakSidePrice(tick, dir);
        if (entrySlippageTicks_ > 0 && tickSize > 0) {
            const double slip = entrySlippageTicks_ * tickSize;
            const double opp = opponentPrice(tick, dir);
            px = (dir == Dir::Buy) ? std::min(px + slip, opp) : std::max(px - slip, opp);
        }
    }
    px = roundToTick(px, tickSize);
    const uint64_t tag = static_cast<uint64_t>(st.tickCount);
    const uint32_t cmdId = ctx_->insert(tick.instrument, dir, Offset::Open, PriceType::Limit, px,
                                        tradeVolume_, tag, timeCond);
    if (cmdId == 0) {
        HFT_LOGW("order rejected locally (command ring full) %s", tick.instrument);
        return;
    }

    st.pending.active = true;
    st.pending.cancelSent = false;
    st.pending.firstCancelNs = 0;
    st.pending.kind = OrderKind::Entry;
    st.pending.cmdId = cmdId;
    st.pending.instrument[0] = '\0';
    copyStr(st.pending.instrument, tick.instrument);
    st.pending.direction = static_cast<char>(dir);
    st.pending.offsetFlag = static_cast<char>(Offset::Open);
    st.pending.volume = tradeVolume_;
    st.pending.filledVolume = 0;
    st.pending.tag = tag;
    st.pending.sentTimeNs = nowNs();
    st.pending.ticksSinceSend = 0;
    st.pending.orderRef[0] = '\0';
    st.pending.orderSysID[0] = '\0';
    st.pending.frontID = 0;
    st.pending.sessionID = 0;
    st.phase = Phase::Opening;
    st.tradingStarted = true;
    st.startupDone = true;
    st.orderCount++;

    char why[48];
    if (taker) {
        const int slipTicks = entrySlippageTicks_ + extraTicks;
        if (slipTicks > 0) {
            std::snprintf(why, sizeof(why), "taker: %s+%dt",
                          dir == Dir::Buy ? "ask1" : "bid1", slipTicks);
        } else {
            std::snprintf(why, sizeof(why), "taker: %s",
                          dir == Dir::Buy ? "ask1" : "bid1");
        }
    } else if (entrySlippageTicks_ > 0) {
        std::snprintf(why, sizeof(why), dir == Dir::Buy ? "bid1+%dt" : "ask1-%dt",
                      entrySlippageTicks_);
    } else {
        std::snprintf(why, sizeof(why), "%s", dir == Dir::Buy ? "bid1" : "ask1");
    }
    HFT_LOGI("ORDER %s %s OPEN vol=%d LIMIT @ %.2f (%s, %s, cmdId=%u)", tick.instrument,
             dir == Dir::Buy ? "BUY" : "SELL", tradeVolume_, px, why,
             taker ? "IOC" : "GFD", cmdId);
}

// Passive take profit close: resting on the weak side of the CLOSE direction
// (closing a long -> sell @ ask1, closing a short -> buy @ bid1).
void ObiVoiStrategy::sendClosePassive(State& st, const TickMsg& tick) {
    const int net = ctx_->position(tick.instrument);
    if (net == 0) return;
    const Dir dir = net > 0 ? Dir::Sell : Dir::Buy;
    const int volume = net > 0 ? net : -net;

    const double tickSize = ctx_->priceTick(tick.instrument);
    // Profit-target close: rest at entry +/- profit_ticks so the fade round
    // trip actually captures the mean reversion. Closing at the touch gives
    // up the whole edge (entry already paid one tick of slippage).
    //
    // Maker mode instead FLIPS the spread: the entry filled passively at the
    // bid (ask), so offering at the CURRENT opposite touch captures the
    // spread itself - a fixed target away from the touch just queues behind
    // nothing and starves.
    double px;
    if (makerMode_) {
        // flip: bought the bid -> offer the ASK (weakSide of the sell), so a
        // buyer lifting the thin ask level fills us at +spread
        px = roundToTick(weakSidePrice(tick, dir), tickSize);
    } else if (st.tpPrice > 0) {
        px = st.tpPrice;  // precomputed at the entry fill (armBracket)
    } else if (st.entryPrice > 0) {
        const double target = (dir == Dir::Sell)
                                  ? st.entryPrice + profitTicks_ * tickSize
                                  : st.entryPrice - profitTicks_ * tickSize;
        px = roundToTick(target, tickSize);  // tick size was unknown at the fill
    } else {
        px = roundToTick(weakSidePrice(tick, dir), tickSize);  // unknown entry
    }
    const uint64_t tag = static_cast<uint64_t>(st.tickCount);
    const uint32_t cmdId = ctx_->insert(tick.instrument, dir, Offset::CloseToday, PriceType::Limit,
                                        px, volume, tag);
    if (cmdId == 0) {
        HFT_LOGW("order rejected locally (command ring full) %s", tick.instrument);
        return;
    }

    st.pending.active = true;
    st.pending.cancelSent = false;
    st.pending.firstCancelNs = 0;
    st.pending.kind = OrderKind::ClosePassive;
    st.pending.cmdId = cmdId;
    copyStr(st.pending.instrument, tick.instrument);
    st.pending.direction = static_cast<char>(dir);
    st.pending.offsetFlag = static_cast<char>(Offset::CloseToday);
    st.pending.volume = volume;
    st.pending.filledVolume = 0;
    st.pending.tag = tag;
    st.pending.sentTimeNs = nowNs();
    st.pending.ticksSinceSend = 0;
    st.pending.orderRef[0] = '\0';
    st.pending.orderSysID[0] = '\0';
    st.pending.frontID = 0;
    st.pending.sessionID = 0;
    st.phase = Phase::Closing;
    st.adverseTicks = 0;
    st.orderCount++;
    // bracket bookkeeping (fade mode): count TP posts and reset the throttle
    // so the counter auto-killing a resting limit re-arms at most once per
    // tp_repost_interval_ticks
    if (!makerMode_) {
        ++st.tpPosts;
        st.ticksSinceTpPost = 0;
    }

    HFT_LOGI("ORDER %s %s CLOSE vol=%d LIMIT @ %.2f (TP bracket, post %d/%d, cmdId=%u)",
             tick.instrument, dir == Dir::Buy ? "BUY" : "SELL", volume, px,
             st.tpPosts, tpRepostMax_, cmdId);
}

// Escape close: IOC / FAK limit at the opponent price, optionally crossed by
// extraSlipTicks so the fill is virtually immediate (stop orders use this -
// by the time the stop fires the book is already running away, an IOC
// exactly at the touch can miss and leave the loss unbounded). Fills now or
// dies; the dead-IOC path re-arms on the next tick.
void ObiVoiStrategy::sendCloseIoc(State& st, const TickMsg& tick, int extraSlipTicks,
                                  char offsetFlag, double limitPrice) {
    const int net = ctx_->position(tick.instrument);
    if (net == 0) return;
    // The counter told us the session is closed: every order sent before the
    // next open is refused and only burns the rate budget (2026-11-10, the
    // 15:21:04 close after the 15:00 SHFE close). Hold the position and retry
    // once the deadline expires - by then the market is usually open again
    // and the escape flattens it.
    if (st.marketClosedUntilNs != 0 && nowNs() < st.marketClosedUntilNs) return;
    const Dir dir = net > 0 ? Dir::Sell : Dir::Buy;
    const int volume = net > 0 ? net : -net;

    const double tickSize = ctx_->priceTick(tick.instrument);
    // limitPrice pins the fill price (take-profit path); otherwise cross to the
    // opponent touch and let extraSlipTicks widen it (stop / escape path).
    double px = (limitPrice > 0.0) ? limitPrice : opponentPrice(tick, dir);
    if (extraSlipTicks > 0 && tickSize > 0) {
        const double slip = extraSlipTicks * tickSize;
        px = (dir == Dir::Buy) ? px + slip : px - slip;
    }
    px = roundToTick(px, tickSize);
    const uint64_t tag = static_cast<uint64_t>(st.tickCount);
    const uint32_t cmdId = ctx_->insert(tick.instrument, dir, static_cast<Offset>(offsetFlag),
                                        PriceType::Limit, px, volume, tag, '3' /* IOC / FAK */);
    if (cmdId == 0) {
        HFT_LOGW("order rejected locally (command ring full) %s", tick.instrument);
        return;
    }

    st.pending.active = true;
    st.pending.cancelSent = false;
    st.pending.firstCancelNs = 0;
    st.pending.kind = OrderKind::CloseIoc;
    st.pending.cmdId = cmdId;
    copyStr(st.pending.instrument, tick.instrument);
    st.pending.direction = static_cast<char>(dir);
    st.pending.offsetFlag = offsetFlag;
    st.pending.volume = volume;
    st.pending.filledVolume = 0;
    st.pending.tag = tag;
    st.pending.sentTimeNs = nowNs();
    st.pending.ticksSinceSend = 0;
    st.pending.orderRef[0] = '\0';
    st.pending.orderSysID[0] = '\0';
    st.pending.frontID = 0;
    st.pending.sessionID = 0;
    st.phase = Phase::Escaping;
    st.escapeRetries++;
    st.lastEscapeNs = st.pending.sentTimeNs;
    if (st.escapeSinceNs == 0) st.escapeSinceNs = st.lastEscapeNs;
    st.orderCount++;

    HFT_LOGW("ORDER %s %s CLOSE vol=%d IOC LIMIT @ %.2f (%s, offset=%c, cmdId=%u)",
             tick.instrument, dir == Dir::Buy ? "BUY" : "SELL", volume, px,
             limitPrice > 0.0 ? "target: at the TP price, never worse"
             : extraSlipTicks > 0 ? "stop: cross the spread +slip"
                                  : "cross the spread",
             offsetFlag, cmdId);
}

void ObiVoiStrategy::sendFlattenLeg(State& st, const TickMsg& tick, const CloseLeg& leg) {
    const Dir dir = static_cast<Dir>(leg.direction);
    // Startup flattening always crosses the spread: getting flat comes first.
    const double tickSize = ctx_->priceTick(tick.instrument);
    const double px = roundToTick(opponentPrice(tick, dir), tickSize);
    const uint64_t tag = static_cast<uint64_t>(st.tickCount);
    const uint32_t cmdId = ctx_->insert(tick.instrument, dir, static_cast<Offset>(leg.offsetFlag),
                                        PriceType::Limit, px, leg.volume, tag);
    if (cmdId == 0) {
        HFT_LOGW("order rejected locally (command ring full) %s", tick.instrument);
        return;
    }

    st.pending.active = true;
    st.pending.cancelSent = false;
    st.pending.firstCancelNs = 0;
    st.pending.kind = OrderKind::Flatten;
    st.pending.cmdId = cmdId;
    copyStr(st.pending.instrument, tick.instrument);
    st.pending.direction = leg.direction;
    st.pending.offsetFlag = leg.offsetFlag;
    st.pending.volume = leg.volume;
    st.pending.filledVolume = 0;
    st.pending.tag = tag;
    st.pending.sentTimeNs = nowNs();
    st.pending.ticksSinceSend = 0;
    st.pending.orderRef[0] = '\0';
    st.pending.orderSysID[0] = '\0';
    st.pending.frontID = 0;
    st.pending.sessionID = 0;
    st.orderCount++;

    const char legOff = leg.offsetFlag;
    HFT_LOGW("FLATTEN %s %s %s vol=%d LIMIT @ %.2f", tick.instrument,
             dir == Dir::Buy ? "BUY" : "SELL",
             legOff == static_cast<char>(Offset::CloseToday)       ? "closetoday"
             : legOff == static_cast<char>(Offset::CloseYesterday) ? "closeyd"
                                                                   : "close",
             leg.volume, px);
}

// Request the cancel; the pending slot stays alive until the report lands.
void ObiVoiStrategy::requestCancel(State& st, const char* instrument) {
    if (!st.pending.active || st.pending.cancelSent) return;
    ExecReport rep;
    std::memset(&rep, 0, sizeof(rep));
    copyStr(rep.instrument, instrument);
    copyStr(rep.orderRef, st.pending.orderRef);
    copyStr(rep.orderSysID, st.pending.orderSysID);
    rep.frontID = st.pending.frontID;
    rep.sessionID = st.pending.sessionID;
    rep.direction = st.pending.direction;
    rep.offsetFlag = st.pending.offsetFlag;
    rep.volume = st.pending.volume;
    rep.strategyTag = st.pending.tag;
    ctx_->cancel(rep, st.pending.tag);
    st.pending.cancelSent = true;
    st.pending.cancelSentNs = nowNs();
    if (st.pending.firstCancelNs == 0) st.pending.firstCancelNs = st.pending.cancelSentNs;
}

void ObiVoiStrategy::resetRoundTrip(State& st) {
    st.phase = Phase::Idle;
    st.entryPrice = 0.0;
    st.entryDir = '0';
    st.openTimeNs = 0;
    st.adverseTicks = 0;
    st.escapeRetries = 0;
    st.lastEscapeNs = 0;
    st.escapeSinceNs = 0;
    st.holdTicks = 0;
    st.entryQuoteTicks = 0;
    st.closeOffsetTries = 0;
    st.fadeSign = 0;
    st.cooldownTicks = cooldownTicks_;
    st.closeReposts = 0;
    st.tpPosts = 0;
    st.ticksSinceTpPost = 0;
    st.tpTrigger = 0.0;
    st.slTrigger = 0.0;
    st.tpPrice = 0.0;
    st.beArmed = false;
    st.retryEntryDir = '0';
    st.retryEntryUntilNs = 0;
    st.awaitTradeUntilNs = 0;
    st.flattenClear();
    st.flattenFails = 0;
    st.flattenBackoffUntilNs = 0;
}

// ---------------------------------------------------------------------------
//  Daily loss budget
// ---------------------------------------------------------------------------
// The exchange trading day, not the wall clock: an SHFE night session that starts
// at 21:00 already belongs to the next trading day, so the budget rolls between
// the night and the day session instead of in the middle of one of them.
void ObiVoiStrategy::rollTradingDay(const char* tradingDay) {
    if (!tradingDay || tradingDay[0] == '\0') return;
    if (std::strncmp(dailyDay_, tradingDay, kDateLen) == 0) return;
    const bool hadBudget = dailyDay_[0] != '\0';
    std::memcpy(dailyDay_, tradingDay, kDateLen);
    dailyDay_[kDateLen - 1] = '\0';
    if (hadBudget) {
        HFT_LOGI("trading day %s: daily loss budget reset (previous day closed at %+.1f ticks, %s)",
                 dailyDay_, dailyPnlTicks_, dailyHalted_ ? "halted" : "active");
    }
    dailyPnlTicks_ = 0.0;
    dailyHalted_ = false;
}

// pricePnl is per unit (price difference); volume is in lots.
void ObiVoiStrategy::bookRoundTrip(double pricePnl, int volume, const char* instrument) {
    const double ts = ctx_ ? ctx_->priceTick(instrument) : 0.0;
    if (ts <= 0.0) return;
    const int lots = volume > 0 ? volume : 1;
    dailyPnlTicks_ += pricePnl / ts * static_cast<double>(lots);
    if (dailyLossLimitTicks_ > 0 && !dailyHalted_ &&
        dailyPnlTicks_ <= -static_cast<double>(dailyLossLimitTicks_)) {
        dailyHalted_ = true;
        HFT_LOGW("%s: DAILY LOSS LIMIT reached (%+.1f ticks <= -%d) -> no new entries "
                 "for the rest of trading day %s; open positions still exit",
                 instrument, dailyPnlTicks_, dailyLossLimitTicks_, dailyDay_);
    }
}

// ---------------------------------------------------------------------------
//  onExec - everything that depends on an order outcome
// ---------------------------------------------------------------------------
void ObiVoiStrategy::onExec(const ExecReport& report) {
    State* sp = findState(report.instrument);
    if (!sp) return;
    State& st = *sp;

    // ---- a fill -------------------------------------------------------------
    if (report.kind == ExecType::TradeRtn) {
        const bool open = (report.offsetFlag == static_cast<char>(Offset::Open));
        const double px = report.tradePrice > 0 ? report.tradePrice : report.price;
        HFT_LOGI("TRADE %s %s %s vol=%d @ %.2f", report.instrument,
                 report.direction == static_cast<char>(Dir::Buy) ? "BUY" : "SELL",
                 open ? "OPEN" : "CLOSE", report.tradeVolume, px);

        // Fills of an order we are no longer tracking are still booked by the
        // shared position book (ctx_), they just do not drive the state machine.
        const bool tracked = (st.pending.cmdId != 0 && report.cmdId != 0 &&
                              report.cmdId == st.pending.cmdId);
        if (tracked) {
            st.pending.filledVolume += report.tradeVolume;
        }

        if (open) {
            if (tracked) {
                st.entryPrice = px;
                st.entryDir = report.direction;
                st.openTimeNs = nowNs();
                armBracket(st, ctx_->priceTick(report.instrument));  // TP/SL precomputed
                st.pending.active = false;  // entry leg done (full or partial)
                // Any entry remainder that is still resting gets pulled: the
                // inventory cap forbids growing the position further.
                if (st.pending.filledVolume < st.pending.volume && st.pending.orderRef[0]) {
                    requestCancel(st, report.instrument);
                    // keep the slot so the final cancel report has a home
                    st.pending.active = true;
                }
            }
            // Even when this fill could not be matched to the working slot (the
            // counter interleaves the finished report, or the slot was already
            // released), the fill IS the entry: record its price. Without this
            // the take profit below fell back to the TOUCH price, i.e. it was
            // armed at the market instead of entry +/- profit_ticks - measured
            // 2026-11-06 11:02:06: TP went out at 12453 against a short entry of
            // 12455 (target 12447), giving away the whole fade edge to the
            // spread (and it only survived because the counter cancelled it).
            if (st.entryPrice <= 0 && px > 0) {
                st.entryPrice = px;
                st.entryDir = report.direction;
                if (st.openTimeNs == 0) st.openTimeNs = nowNs();
                armBracket(st, ctx_->priceTick(report.instrument));
            }
            // Fresh fill -> rest the take profit immediately (bracket order,
            // user spec 2026-11-05: TP = entry +/- profit_ticks, submitted the
            // moment the entry is booked, pulled ONLY by the stop trigger or
            // the time limit - never by VOI or by re-quoting whims).
            //
            // 2026-11-06: SimNow kills EVERY resting order in 80-300 ms
            // (measured: 8 of 8 TP posts per round trip came back status=5,
            // "已撤单报单已提交"). The bracket therefore never actually rested -
            // every exit came either from the price-trigger IOC or from the
            // stop, and each round trip still fired up to 8 dead orders at the
            // counter. tp_repost_max = 0 now means "do not post at all": the
            // exit is purely the price-triggered IOC, which is what was really
            // executing anyway, at a third of the order traffic.
            const int net = ctx_->position(report.instrument);
            const TickMsg* tick = ctx_->lastTick(report.instrument);
            if (tpRepostMax_ > 0 && net != 0 && tick && st.flattenEmpty() &&
                !st.pending.active) {
                sendClosePassive(st, *tick);
            }
        } else {
            // a close fill: flat?
            const int net = ctx_->position(report.instrument);
            if (net == 0) {
                const double pnl = (st.entryDir == static_cast<char>(Dir::Sell))
                                       ? (st.entryPrice - px)
                                       : (px - st.entryPrice);
                // Book it into the trading day before logging, so the log line
                // carries the running total (and the halt, if this trip tripped it).
                bookRoundTrip(pnl, report.tradeVolume, report.instrument);
                HFT_LOGI("ROUND TRIP %s closed @ %.2f (entry=%.2f %s, pnl=%+.2f/unit, %s) "
                         "| dayPnL=%+.1ft",
                         report.instrument, px, st.entryPrice,
                         st.entryDir == static_cast<char>(Dir::Sell) ? "short" : "long", pnl,
                         st.pending.kind == OrderKind::CloseIoc ? "IOC escape" : "passive",
                         dailyPnlTicks_);
                // Streak guard. A fade strategy bleeds exactly when the market
                // trends: every reversal signal fires, every one gets stopped.
                // Measured 2026-11-06 11:02-11:04: three shorts stopped at -5,
                // -6, -6 inside 100 s while the mid walked up 15 ticks - the
                // 20-tick cooldown (~10 s) is far too short to notice a trend.
                // After N losing round trips in a row, stand down for a while
                // instead of feeding the same move a fourth time.
                if (consecStopPause_ > 0) {
                    if (pnl < 0) {
                        ++st.consecStops;
                        if (st.consecStops >= consecStopPause_) {
                            st.pauseUntilNs = nowNs() + static_cast<int64_t>(pauseSeconds_) * INT64_C(1000000000);
                            HFT_LOGW("%s: %d losing round trips in a row -> stand down for %d s",
                                     report.instrument, st.consecStops, pauseSeconds_);
                            st.consecStops = 0;
                        }
                    } else {
                        st.consecStops = 0;
                    }
                }
                st.pending.active = false;
                resetRoundTrip(st);
            } else if (tracked && st.pending.filledVolume >= st.pending.volume) {
                st.pending.active = false;  // partial close done, remainder follows
            }
            // still holding: the next tick re-arms the appropriate close.
        }
        return;
    }

    const bool isOrder = (report.kind == ExecType::OrderRtn || report.kind == ExecType::InsertRsp ||
                          report.kind == ExecType::ActionRsp);
    if (!isOrder) return;

    // ---- errors ---------------------------------------------------------------
    if (report.errorId != 0) {
        HFT_LOGW("ORDER ERROR %s (%d) %s", report.instrument, report.errorId, report.errorMsg);
        // Session closed: stand down instead of re-sending. See the header of
        // isClosedMarketRejection - the retry paths below would each fire once
        // and be refused again.
        if (isClosedMarketRejection(report.errorMsg)) {
            const int64_t until = nowNs() + INT64_C(600000000000);  // 10 min
            st.marketClosedUntilNs = until;
            st.pauseUntilNs = until;
            if (st.pending.active) {
                st.pending.active = false;
                st.pending.cancelSent = false;
                st.pending.firstCancelNs = 0;
            }
            HFT_LOGE("%s: order refused because the session is closed (%s) -> "
                     "no orders for 10 min; net=%d is carried until the market "
                     "reopens (a stop it should have taken is logged above)",
                     report.instrument, report.errorMsg, ctx_->position(report.instrument));
            return;
        }
        // A cancel that the counter rejects ("order not found" and friends)
        // means the order is already terminal and no further report will come.
        // Waiting forever would wedge the slot; release it and let the tick
        // loop re-arm whatever the position requires.
        if (report.kind == ExecType::ActionRsp) {
            if (st.pending.active && (report.cmdId == 0 ||
                                      st.pending.cmdId == 0 ||
                                      report.cmdId == st.pending.cmdId)) {
                HFT_LOGW("        cancel rejected (order gone) -> release the pending slot");
                st.pending.active = false;
                st.pending.cancelSent = false;
                st.pending.firstCancelNs = 0;
                if (ctx_->position(report.instrument) == 0) resetRoundTrip(st);
            }
            return;  // action errors are otherwise informational
        }
        if (!st.pending.active) return;
        if (st.pending.cmdId != 0 && report.cmdId != 0 && report.cmdId != st.pending.cmdId) return;

        const TickMsg* tick = ctx_->lastTick(report.instrument);
        if (!tick) {
            st.pending.active = false;
            return;
        }
        // SHFE 50/51: the close bucket is smaller than the order -> 1 lot retry.
        if ((report.errorId == 50 || report.errorId == 51) && st.pending.volume > 1) {
            const Dir dir = static_cast<Dir>(st.pending.direction);
            const Offset off = static_cast<Offset>(st.pending.offsetFlag);
            const double px = roundToTick(opponentPrice(*tick, dir),
                                          ctx_->priceTick(tick->instrument));
            HFT_LOGW("        close volume %d exceeds the bucket -> retry 1 lot @ %.2f",
                     st.pending.volume, px);
            // reuse the slot for the retry
            st.pending.cancelSent = false;
            st.pending.firstCancelNs = 0;
            const uint32_t cmdId = ctx_->insert(report.instrument, dir, off, PriceType::Limit, px,
                                                1, st.pending.tag);
            if (cmdId != 0) {
                st.pending.cmdId = cmdId;
                st.pending.volume = 1;
                st.pending.filledVolume = 0;
                st.pending.ticksSinceSend = 0;
                st.pending.orderRef[0] = '\0';
                st.orderCount++;
            }
            return;
        }
        // Close offset flag refused -> walk the ring CloseToday -> CloseYesterday
        // -> Close. SHFE products (rb, ag, cu ...) require an explicit
        // today/yesterday flag and reject a plain close on a yesterday
        // position; most other exchanges are the other way round.
        const bool isCloseOrder = (st.pending.offsetFlag != static_cast<char>(Offset::Open));
        if (isCloseOrder && st.closeOffsetTries < 2) {
            ++st.closeOffsetTries;
            const Dir dir = static_cast<Dir>(st.pending.direction);
            const Offset cur = static_cast<Offset>(st.pending.offsetFlag);
            const Offset off = (cur == Offset::CloseToday)         ? Offset::CloseYesterday
                               : (cur == Offset::CloseYesterday)   ? Offset::Close
                                                                   : Offset::CloseToday;
            // A rejected close never rests: cross the spread so it fills now.
            const double px = roundToTick(opponentPrice(*tick, dir),
                                          ctx_->priceTick(tick->instrument));
            HFT_LOGW("        close rejected -> retry as LIMIT @ %.2f with offset %c", px,
                     static_cast<char>(off));
            st.pending.cancelSent = false;
            st.pending.firstCancelNs = 0;
            const uint32_t cmdId = ctx_->insert(report.instrument, dir, off, PriceType::Limit, px,
                                                st.pending.volume, st.pending.tag);
            if (cmdId != 0) {
                st.pending.cmdId = cmdId;
                st.pending.filledVolume = 0;
                st.pending.ticksSinceSend = 0;
                st.pending.orderRef[0] = '\0';
                st.orderCount++;
            }
            return;
        }
        // IOC rejected (e.g. no FAK support): fall back to a plain crossing limit.
        if (st.pending.kind == OrderKind::CloseIoc) {
            const Dir dir = static_cast<Dir>(st.pending.direction);
            const double px = roundToTick(opponentPrice(*tick, dir),
                                          ctx_->priceTick(tick->instrument));
            HFT_LOGW("        IOC close rejected -> plain LIMIT @ %.2f (crossing)", px);
            st.pending.cancelSent = false;
            st.pending.firstCancelNs = 0;
            const uint32_t cmdId = ctx_->insert(report.instrument, dir, Offset::CloseToday,
                                                PriceType::Limit, px, st.pending.volume,
                                                st.pending.tag);
            if (cmdId != 0) {
                st.pending.cmdId = cmdId;
                st.pending.kind = OrderKind::Flatten;  // taker close, managed by tick timeout
                st.pending.filledVolume = 0;
                st.pending.ticksSinceSend = 0;
                st.pending.orderRef[0] = '\0';
                st.orderCount++;
            }
            return;
        }
        st.pending.active = false;  // give this leg up; the tick loop re-arms
        if (st.pending.kind == OrderKind::Flatten) {
            ++st.flattenFails;
            // The counter walked every close bucket (CloseToday -> CloseYesterday
            // -> Close) and refused them all with 50/51, i.e. "no closable
            // position".  That is the matching engine telling us the position
            // only exists in the query snapshot - the 7x24 phantom.  Churning
            // more rejected closes parks the strategy in Escaping and silently
            // blocks every new signal (that is why trading stopped after the
            // first few fills), so drop the phantom and resume.
            if ((report.errorId == 50 || report.errorId == 51) &&
                st.closeOffsetTries >= 2) {
                HFT_LOGW("        all close buckets refused (err=%d) -> %s holds no "
                         "closable position, dropping the phantom and resuming",
                         report.errorId, report.instrument);
                ctx_->adoptPosition(report.instrument, 0, tick->lastPrice);
                resetRoundTrip(st);
                return;
            }
            const int backoffS = std::min(15 * st.flattenFails, 120);
            st.flattenBackoffUntilNs = nowNs() + INT64_C(1000000000) * backoffS;
            HFT_LOGW("        flatten rejected in every bucket (%d round(s)) -> back off "
                     "%d s (likely a 7x24 phantom: query reports it, matching denies it)",
                     st.flattenFails, backoffS);
        }
        return;
    }

    if (!st.pending.active) {
        if (report.statusMsg[0] != '\0') {
            HFT_LOGD("order %s status=%c %s", report.orderRef, report.status, report.statusMsg);
        }
        return;
    }

    // Late reports of a *previous* order must not touch the current one.
    if (st.pending.cmdId != 0 && report.cmdId != 0 && report.cmdId != st.pending.cmdId) {
        HFT_LOGD("ignore late report cmdId=%u (working on cmdId=%u)", report.cmdId,
                 st.pending.cmdId);
        return;
    }

    if (report.orderRef[0] != '\0') copyStr(st.pending.orderRef, report.orderRef);
    if (report.orderSysID[0] != '\0') copyStr(st.pending.orderSysID, report.orderSysID);
    if (report.frontID) st.pending.frontID = report.frontID;
    if (report.sessionID) st.pending.sessionID = report.sessionID;

    if (isOrderFinished(report.status)) {
        // ---- did this order actually fill? ---------------------------------
        // CTP order status '0' means AllTraded and volumeTraded is the
        // authoritative field.  The SimNow 7x24 front (看穿式前置) - and
        // occasionally the day front - delivers this completion report BEFORE
        // the matching trade report, so ctx_->position() still holds the
        // pre-fill value at this instant.  Judging by position alone made the
        // state machine believe that a filled entry never happened, and made it
        // re-send closes that had already filled; the duplicate then came back
        // as ErrorID 50 (平仓仓位不足) and the run degenerated into a cascade of
        // rejected orders.  Trust volumeTraded here, position later.
        if (report.volumeTraded > st.pending.filledVolume)
            st.pending.filledVolume = report.volumeTraded;
        if ((report.status == '0' || report.volumeTraded >= st.pending.volume) &&
            st.pending.filledVolume < st.pending.volume)
            st.pending.filledVolume = st.pending.volume;
        const int tradedVol = st.pending.filledVolume;

        HFT_LOGI("order %s finished, status=%c (%s, filled=%d/%d) msg=%s err=%d %s",
                 report.orderRef, report.status,
                 st.pending.kind == OrderKind::Entry       ? "entry"
                 : st.pending.kind == OrderKind::ClosePassive ? "passive close"
                 : st.pending.kind == OrderKind::CloseIoc ? "IOC close"
                                                                          : "close",
                 st.pending.filledVolume, st.pending.volume, report.statusMsg,
                 report.errorId, report.errorMsg);
        const int net = ctx_->position(report.instrument);
        const bool wasEntry = (st.pending.kind == OrderKind::Entry);
        st.pending.active = false;
        st.pending.cancelSent = false;
        st.pending.firstCancelNs = 0;

        if (wasEntry) {
            if (net == 0 && tradedVol == 0) {
                if (makerMode_) {
                    // Maker mode: the replay counter auto-cancels resting
                    // quotes every few hundred ms; that is not a signal
                    // failure. Just re-quote on the next tick (free), the
                    // entry patience is bounded by entry_timeout_ticks.
                    HFT_LOGI("         %s: entry quote canceled by the counter -> re-quote",
                             report.instrument);
                    st.phase = Phase::Idle;
                    st.startupDone = true;   // keep trading enabled
                } else {
                    // Entry died unfilled.  The 7x24 front routinely sends the
                    // trade report AFTER this cancel report (or after the
                    // counter's auto-cancel of a resting quote), so wiping the
                    // round trip instantly produced phantom positions with
                    // entry=0 and absurd PnL.  Hold a short grace window; onTick
                    // finalises it (reset + cooldown) once the deadline passes
                    // with no position booked.
                    HFT_LOGI("         %s: entry canceled unfilled -> grace window "
                             "before cooldown", report.instrument);
                    st.phase = Phase::Idle;
                    st.awaitTradeUntilNs = nowNs() + INT64_C(200000000);
                    // Only relevant when the IOC was sent AT the touch
                    // (entry_slippage_ticks = 0). With the shipped configs the
                    // IOC already crosses one tick, and measured end-to-end the
                    // retry lands ~2 s later (cancel report + grace window +
                    // next tick) by which time the book has moved - paying one
                    // tick up front is cheaper than that delay.
                    //
                    // Follow mode reuses the same slot as its maker->taker
                    // fallback: the passive quote timed out unfilled, so the
                    // retry (sent with forceTaker) crosses a tick as an IOC.
                    if ((followMode_ ||
                         (entryTaker_ && entrySlippageTicks_ == 0)) &&
                        report.direction != '0') {
                        st.retryEntryDir = report.direction;
                        st.retryEntryUntilNs = nowNs() + INT64_C(1500000000);
                    }
                }
            } else if (net == 0) {
                // Filled according to the order report, but the trade report is
                // still in flight. Stay out of Idle for a moment so that a
                // second entry is not fired on top of this fill.
                st.phase = Phase::Opening;
                st.awaitTradeUntilNs = nowNs() + INT64_C(300000000);
                HFT_LOGI("         %s: entry fill confirmed by the order report -> "
                         "waiting for the trade report", report.instrument);
            }
            // net != 0: partial fill happened; onTick step (3) puts the close on.
        } else if (net != 0) {
            if (tradedVol > 0) {
                // This close did fill - only the position snapshot lags behind
                // the trade report. Re-arming or re-sending here is precisely
                // what produced ErrorID 50 (平仓仓位不足) on the 7x24 front.
                st.awaitTradeUntilNs = nowNs() + INT64_C(300000000);
                HFT_LOGI("         %s: close fill confirmed by the order report -> "
                         "waiting for the trade report", report.instrument);
                return;
            }
            // A close order died without flattening. If the escape conditions
            // are ALREADY met (consecutive adverse ticks / hold expiry - the
            // same bar the state machine uses), go straight to the IOC;
            // otherwise the next tick re-arms the passive close. A single
            // adverse VOI tick must NOT fire an IOC: the replay counter
            // auto-cancels closes every few hundred ms, so a one-shot check
            // here would liquidate on noise.
            const TickMsg* tick = ctx_->lastTick(report.instrument);
            if (tick && (stopLossHit(st, *tick, net) ||
                         (maxHoldTicks_ > 0 && st.holdTicks >= maxHoldTicks_))) {
                HFT_LOGW("ESCAPE %s: close died with conditions met "
                         "(entry=%.2f adverse=%dt hold=%dt) -> IOC stop order",
                         report.instrument, st.entryPrice, st.adverseTicks, st.holdTicks);
                sendCloseIoc(st, *tick, stopSlippageTicks_);
            } else if (tick && st.escapeRetries >= kMaxEscapeRetries) {
                HFT_LOGE("%s: escape retries exhausted (net=%d) - halting new entries",
                         report.instrument, net);
                ctx_->haltTrading();
            }
            // otherwise: next tick re-arms the passive close (onTick step 3)
        } else {
            // Flat and the close order finished: round trip complete (the
            // ROUND TRIP log line comes from the trade report path).
            resetRoundTrip(st);
        }
    }
}

// ---------------------------------------------------------------------------
//  startup leftover handling (same proven pattern as ma_tick)
// ---------------------------------------------------------------------------
void ObiVoiStrategy::startupGate(State& st, const TickMsg& tick) {
    if (st.tradingStarted || st.startupDone) return;
    if (st.pending.active || !st.flattenEmpty()) return;

    const int net = ctx_->position(tick.instrument);
    if (net != 0) {
        startFlatten(st, tick.instrument);
        return;
    }

    if (!ctx_->snapshotReady()) return;
    BrokerPosition bp;
    const bool brokerHas = ctx_->brokerPosition(tick.instrument, bp) && bp.net != 0;

    const uint32_t round = ctx_->snapshotQueryRound();
    if (round != st.seenRound) {
        st.seenRound = round;
        ++st.roundsSeen;
        st.staleBrokerRounds = brokerHas ? (st.staleBrokerRounds + 1) : 0;
    }

    if (brokerHas && st.staleBrokerRounds >= kStaleRounds) {
        startFlatten(st, tick.instrument);
        return;
    }

    // Symmetric case: our local book still holds a position the broker no
    // longer reports (a 7x24 phantom that evaporated on its own). Adopt the
    // broker truth so the flatten loop stops churning rejected orders.
    if (!brokerHas && net != 0 && ctx_->snapshotReady()) {
        if (++st.flatBrokerRounds >= kStaleRounds) {
            HFT_LOGW("%s: local net=%d but broker reports flat for %d rounds -> "
                     "adopting broker book (phantom evaporated)",
                     tick.instrument, net, st.flatBrokerRounds);
            ctx_->adoptPosition(tick.instrument, 0, tick.lastPrice);
            resetRoundTrip(st);
        }
    } else {
        st.flatBrokerRounds = 0;
    }

    if (st.roundsSeen >= kStartupRounds) {
        st.startupDone = true;
        HFT_LOGI("%s: startup check done - no leftover position, trading enabled",
                 tick.instrument);
    }
}

void ObiVoiStrategy::startFlatten(State& st, const char* instrument) {
    // Two complete rounds of "every close bucket refused" means the position is
    // a 7x24 phantom: the query snapshot shows it, the matching engine has
    // nothing to close.  Drop it and go back to trading instead of looping.
    if (st.flattenFails >= 2) {
        HFT_LOGW("%s: %d flatten rounds refused in every bucket -> dropping the "
                 "phantom position, book is flat again",
                 instrument, st.flattenFails);
        ctx_->adoptPosition(instrument, 0, ctx_->lastPrice(instrument));
        resetRoundTrip(st);
        return;
    }
    if (nowNs() < st.flattenBackoffUntilNs) return;  // phantom-position backoff
    BrokerPosition bp;
    const bool hasBroker = ctx_->brokerPosition(instrument, bp) && bp.net != 0;
    const int localNet = ctx_->position(instrument);

    const char kCloseToday = static_cast<char>(Offset::CloseToday);
    const char kCloseYd = static_cast<char>(Offset::CloseYesterday);
    int net;
    if (hasBroker) {
        net = bp.net;
        if (bp.longToday > 0)
            st.flattenPush({static_cast<char>(Dir::Sell), kCloseToday, bp.longToday});
        if (bp.longYd > 0)
            st.flattenPush({static_cast<char>(Dir::Sell), kCloseYd, bp.longYd});
        if (bp.shortToday > 0)
            st.flattenPush({static_cast<char>(Dir::Buy), kCloseToday, bp.shortToday});
        if (bp.shortYd > 0)
            st.flattenPush({static_cast<char>(Dir::Buy), kCloseYd, bp.shortYd});
    } else if (localNet != 0) {
        net = localNet;
        const char dir = localNet > 0 ? static_cast<char>(Dir::Sell) : static_cast<char>(Dir::Buy);
        st.flattenPush({dir, kCloseToday, localNet > 0 ? localNet : -localNet});
    } else {
        return;
    }

    ctx_->cancelAll(instrument, static_cast<uint64_t>(st.tickCount));

    double ref = ctx_->lastPrice(instrument);
    if (ref <= 0) ref = ctx_->openPrice(instrument);
    if (ref <= 0) ref = 1.0;
    ctx_->adoptPosition(instrument, net, ref);
    st.entryPrice = ref;
    st.entryDir = net > 0 ? static_cast<char>(Dir::Buy) : static_cast<char>(Dir::Sell);
    armBracket(st, ctx_->priceTick(instrument));
    st.openTimeNs = nowNs();
    st.phase = Phase::Escaping;
    st.startupDone = false;

    HFT_LOGW("STARTUP FLATTEN %s: %s net=%d -> %u close leg(s) at the touch", instrument,
             hasBroker ? "broker reports" : "book holds", net, st.flattenSize());
}

void ObiVoiStrategy::pumpFlatten(State& st, const TickMsg& tick) {
    if (st.pending.active) return;  // a flatten leg is on the wire
    if (st.flattenEmpty()) return;
    const CloseLeg leg = st.flattenFront();
    st.flattenPop();
    sendFlattenLeg(st, tick, leg);
}

// ---------------------------------------------------------------------------
//  timer / stop
// ---------------------------------------------------------------------------
void ObiVoiStrategy::onTimer(int64_t nowNs) {
    if (flattenOnStart_) {
        for (const auto& inst : instruments_) {
            State& st = state(inst.c_str());
            if (st.tradingStarted || st.startupDone) continue;
            if (st.pending.active || !st.flattenEmpty()) continue;
            const TickMsg* tick = ctx_->lastTick(inst.c_str());
            if (!tick) continue;
            startupGate(st, *tick);
        }
    }

    for (uint32_t i = 0; i < statesN_; ++i) {
        State& st = states_[i].st;
        if (st.tickCount == 0) continue;
        const int net = ctx_->position(states_[i].instrument);
        HFT_LOGD("%s voi=%+.2f obiWin=%u net=%d phase=%s signals=%d orders=%d",
                 states_[i].instrument, st.voi, st.obiCount, net, phaseName(st.phase),
                 st.signalCount, st.orderCount);
        (void)nowNs;
    }
}

void ObiVoiStrategy::onStop() {
    if (!closeOnStop_ || !ctx_) return;
    for (uint32_t i = 0; i < statesN_; ++i) {
        State& st = states_[i].st;
        const int net = ctx_->position(states_[i].instrument);
        if (net == 0) continue;
        const TickMsg* tick = ctx_->lastTick(states_[i].instrument);
        if (!tick) continue;
        HFT_LOGI("shutdown: flatten %s net=%d at the touch", states_[i].instrument, net);
        const Dir dir = net > 0 ? Dir::Sell : Dir::Buy;
        const int volume = net > 0 ? net : -net;
        const double px = roundToTick(opponentPrice(*tick, dir),
                                      ctx_->priceTick(states_[i].instrument));
        const uint32_t cmdId = ctx_->insert(states_[i].instrument, dir, Offset::CloseToday,
                                            PriceType::Limit, px, volume,
                                            static_cast<uint64_t>(st.tickCount));
        if (cmdId != 0) st.orderCount++;
    }
}

} // namespace hft
