// SPDX-License-Identifier: MIT
#include "hft/sim_market.h"

#include "hft/logger.h"
#include "hft/time_util.h"

#include <cmath>

namespace hft {

namespace {

// Cheap deterministic PRNG (splitmix64).
//
// splitmix64 is used instead of a plain xorshift because consecutive outputs of
// a raw xorshift are strongly correlated: the Box-Muller transform below then
// produced a slowly drifting "random" series, which made the synthetic price
// walk in one direction and eventually stick to a single tick for minutes.
struct Rng {
    uint64_t state;
    explicit Rng(uint64_t seed) : state(seed ? seed : 1) {}
    uint64_t next() {
        state += 0x9E3779B97F4A7C15ull;
        uint64_t z = state;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    double uniform01() { return static_cast<double>(next() >> 11) / 9007199254740992.0; }
};

} // namespace

double SimMarket::uniform() {
    Rng r(rngState_);
    const double u = r.uniform01();   // advances r.state
    rngState_ = r.state;              // ... and only now write it back
    return u;
}

double SimMarket::normal() {
    // Box-Muller, one sample.
    double u1 = uniform();
    double u2 = uniform();
    if (u1 < 1e-12) u1 = 1e-12;
    return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * 3.14159265358979323846 * u2);
}

double SimMarket::roundToTick(double price, double tick) const {
    if (tick <= 0) return price;
    return std::floor(price / tick + 0.5) * tick;
}

bool SimMarket::init(const Config& cfg, std::string& err) {
    instruments_.clear();

    std::vector<std::string> names = cfg.instruments();
    if (names.empty()) {
        err = "no instruments configured ([general] instruments)";
        return false;
    }

    // seed <= 0 => seed from the wall clock so every run looks different.
    int64_t seedCfg = static_cast<int64_t>(cfg.getInt("sim", "seed", 20260101));
    if (seedCfg <= 0) {
        rngState_ = static_cast<uint64_t>(nowNs()) | 1ull;
    } else {
        rngState_ = static_cast<uint64_t>(seedCfg) | 1ull;
    }

    const double defaultTick = cfg.getDouble("sim", "price_tick", 1.0);
    const double defaultPrice = cfg.getDouble("sim", "initial_price", 3500.0);
    const double defaultVol = cfg.getDouble("sim", "volatility", 0.00035);
    const double defaultSpread = cfg.getDouble("sim", "spread_ticks", 1.0);
    const int defaultEveryMs = cfg.getInt("sim", "tick_interval_ms", 500);
    const int defaultMultiple = cfg.getInt("sim", "volume_multiple", 10);

    int64_t now = nowNs();
    for (size_t i = 0; i < names.size(); ++i) {
        const std::string& n = names[i];
        Instrument inst;
        copyStr(inst.name, n.c_str());
        // Per instrument overrides live in a [sim.<name>] section.
        const std::string section = "sim." + n;
        inst.priceTick = cfg.getDouble(section, "price_tick", defaultTick);
        inst.initialPrice = cfg.getDouble(section, "initial_price", defaultPrice);
        inst.volatility = cfg.getDouble(section, "volatility", defaultVol);
        inst.spreadTicks = cfg.getDouble(section, "spread_ticks", defaultSpread);
        inst.tickEveryMs = cfg.getInt(section, "tick_interval_ms", defaultEveryMs);
        inst.volumeMultiple = cfg.getInt(section, "volume_multiple", defaultMultiple);
        inst.meanReversion = cfg.getDouble(section, "mean_reversion",
                                           cfg.getDouble("sim", "mean_reversion", 0.002));
        inst.openInterest = cfg.getDouble(section, "open_interest", 100000.0);

        inst.lastPrice = inst.initialPrice;
        inst.openPrice = inst.initialPrice;
        inst.highestPrice = inst.initialPrice;
        inst.lowestPrice = inst.initialPrice;
        // Stagger the instruments so ticks interleave realistically.
        inst.nextTickNs = now + static_cast<int64_t>(i + 1) * 50 * 1000000ll;
        instruments_.push_back(inst);
        HFT_LOGI("sim %s: tick=%.4f p0=%.2f sigma=%.6f revert=%.4f everyMs=%d mult=%d", inst.name,
                 inst.priceTick, inst.initialPrice, inst.volatility, inst.meanReversion,
                 inst.tickEveryMs, inst.volumeMultiple);
    }

    started_ = true;
    return true;
}

int64_t SimMarket::msUntilNextTick() const {
    if (instruments_.empty()) return 1000;
    int64_t best = INT64_MAX;
    for (const auto& i : instruments_) {
        if (i.nextTickNs < best) best = i.nextTickNs;
    }
    int64_t deltaNs = best - nowNs();
    if (deltaNs <= 0) return 0;
    return deltaNs / 1000000 + 1;
}

bool SimMarket::next(TickMsg& out) {
    if (!started_ || instruments_.empty()) return false;

    const int64_t now = nowNs();
    size_t pick = instruments_.size();
    for (size_t i = 0; i < instruments_.size(); ++i) {
        if (instruments_[i].nextTickNs <= now) {
            pick = i;
            break;
        }
    }
    if (pick == instruments_.size()) return false;

    Instrument& inst = instruments_[pick];

    // Random walk with a weak pull towards the initial price so the series does
    // not wander off over long sessions.
    const double shock = normal() * inst.volatility;
    const double pull = (inst.initialPrice - inst.lastPrice) / inst.initialPrice * inst.meanReversion;
    double price = inst.lastPrice * (1.0 + shock + pull);
    price = roundToTick(price, inst.priceTick);
    if (price <= inst.priceTick) price = inst.priceTick * 10.0;
    inst.lastPrice = price;
    if (price > inst.highestPrice) inst.highestPrice = price;
    if (price < inst.lowestPrice) inst.lowestPrice = price;

    const double halfSpread = inst.priceTick * inst.spreadTicks * 0.5;
    const double bid = roundToTick(price - halfSpread, inst.priceTick);
    const double ask = roundToTick(price + halfSpread, inst.priceTick);

    inst.bidVolume = 1 + static_cast<int>(uniform() * 40);
    inst.askVolume = 1 + static_cast<int>(uniform() * 40);

    const int traded = static_cast<int>(uniform() * 30);
    inst.volume += traded;
    inst.turnover += traded * price * inst.volumeMultiple;

    std::memset(&out, 0, sizeof(out));
    out.magic = kMagicTick;
    out.seq = ++seq_;
    copyStr(out.instrument, inst.name);
    copyStr(out.exchangeID, "SIM");
    copyStr(out.tradingDay, localDateString().c_str());
    copyStr(out.updateTime, localTimeString().c_str());
    out.updateMillisec = static_cast<uint32_t>(nowNs() / 1000000 % 1000);
    out.lastPrice = price;
    out.openPrice = inst.openPrice;
    out.highestPrice = inst.highestPrice;
    out.lowestPrice = inst.lowestPrice;
    out.preClosePrice = inst.initialPrice;
    out.preSettlementPrice = inst.initialPrice;
    out.upperLimitPrice = inst.initialPrice * 1.08;
    out.lowerLimitPrice = inst.initialPrice * 0.92;
    out.averagePrice = inst.volume > 0 ? inst.turnover / (inst.volume * inst.volumeMultiple) : price;
    out.bidPrice1 = bid;
    out.askPrice1 = ask;
    out.bidVolume1 = inst.bidVolume;
    out.askVolume1 = inst.askVolume;
    out.volume = inst.volume;
    out.turnover = inst.turnover;
    out.openInterest = inst.openInterest;
    out.recvTimeNs = monotonicNowNs();
    out.publishTimeNs = monotonicNowNs();

    inst.nextTickNs = now + static_cast<int64_t>(inst.tickEveryMs) * 1000000ll;
    return true;
}

void SimMarket::publishSnapshot(MarketSnapshotBlock* block) const {
    if (!block) return;
    uint32_t n = 0;
    for (const auto& inst : instruments_) {
        if (n >= kMaxMarketInstruments) break;
        MarketTickSnap snap{};
        copyStr(snap.instrument, inst.name);
        snap.lastPrice = inst.lastPrice;
        snap.bidPrice1 = roundToTick(inst.lastPrice - inst.priceTick * inst.spreadTicks * 0.5,
                                     inst.priceTick);
        snap.askPrice1 = roundToTick(inst.lastPrice + inst.priceTick * inst.spreadTicks * 0.5,
                                     inst.priceTick);
        snap.openPrice = inst.openPrice;
        snap.highestPrice = inst.highestPrice;
        snap.lowestPrice = inst.lowestPrice;
        snap.bidVolume1 = inst.bidVolume;
        snap.askVolume1 = inst.askVolume;
        snap.tickTimeNs = nowNs();
        snap.recvTimeNs = snap.tickTimeNs;
        block->items[n].setInstrument(inst.name);
        block->items[n].publish(snap);
        ++n;
    }
    block->count.store(n, std::memory_order_release);
}

} // namespace hft
