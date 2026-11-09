// SPDX-License-Identifier: MIT
// strategy_base.h - Strategy plugin interface.
//
// A strategy never touches CTP and never touches shared memory directly: it
// receives ticks, orders and positions through this small interface, which keeps
// strategies unit testable and reusable across live / simulation.
#ifndef HFT_STRATEGY_BASE_H
#define HFT_STRATEGY_BASE_H

#include "hft/config.h"
#include "hft/protocol.h"

#include <string>

namespace hft {

// Position as reported by the broker (mirrored by the query process from the
// shared snapshot). The strategy keeps its own faster book; this is the
// "official" view used at startup to detect leftovers from an earlier run.
struct BrokerPosition {
    int net = 0;          // long - short
    int longToday = 0;
    int longYd = 0;
    int shortToday = 0;
    int shortYd = 0;
};

// Everything a strategy is allowed to do to the outside world.
class StrategyContext {
public:
    virtual ~StrategyContext() = default;

    // Returns the command id (>0) or 0 when the command ring is full.
    virtual uint32_t insert(const char* instrument, Dir direction, Offset offset, PriceType priceType,
                            double price, int volume, uint64_t tag) = 0;

    // Same as insert() but with an explicit CTP time condition:
    //   '1' GFD (rests until canceled)   '3' IOC / FAK (fill now or die)
    // The default implementation falls back to a GFD insert, so existing
    // StrategyContext implementations keep working unchanged.
    virtual uint32_t insert(const char* instrument, Dir direction, Offset offset, PriceType priceType,
                            double price, int volume, uint64_t tag, char timeCondition) {
        (void)timeCondition;
        return insert(instrument, direction, offset, priceType, price, volume, tag);
    }

    // Cancel using the identifiers carried by an ExecReport.
    virtual uint32_t cancel(const ExecReport& order, uint64_t tag) = 0;
    virtual uint32_t cancelAll(const char* instrument, uint64_t tag) = 0;

    // ---- read-only views ---------------------------------------------------
    virtual const TickMsg* lastTick(const char* instrument) const = 0;
    virtual double lastPrice(const char* instrument) const = 0;
    virtual double bidPrice(const char* instrument) const = 0;
    virtual double askPrice(const char* instrument) const = 0;
    virtual double priceTick(const char* instrument) const = 0;
    virtual int volumeMultiple(const char* instrument) const = 0;

    // net position: >0 long, <0 short
    virtual int position(const char* instrument) const = 0;
    virtual double openPrice(const char* instrument) const = 0;
    virtual double realizedPnl(const char* instrument) const = 0;

    // ---- broker side view (query process snapshot) -------------------------
    // False until the query process has published its first snapshot.
    virtual bool snapshotReady() const = 0;
    // Increments every time the query process republishes the snapshot; used
    // to tell "no position yet" from "a fresh round confirmed no position".
    virtual uint32_t snapshotQueryRound() const = 0;
    // False when the snapshot holds no record for this instrument.
    virtual bool brokerPosition(const char* instrument, BrokerPosition& out) const = 0;
    // Seeds the strategy book with a position the broker already reports (used
    // at startup, before flattening a leftover position).
    virtual void adoptPosition(const char* instrument, int net, double price) = 0;

    virtual double available() const = 0;
    virtual bool tradingEnabled() const = 0;
    virtual void haltTrading() = 0;
};

class Strategy {
public:
    virtual ~Strategy() = default;

    virtual const char* name() const = 0;

    // Called once with the shared configuration.
    virtual bool init(const Config& cfg, StrategyContext& ctx, std::string& err) = 0;

    // Hot path: one tick. Must be allocation free and fast.
    virtual void onTick(const TickMsg& tick) = 0;

    // Order / trade feedback.
    virtual void onExec(const ExecReport& report) = 0;

    // Called roughly once per second (good enough for timeouts and reporting).
    virtual void onTimer(int64_t nowNs) = 0;

    // Called on shutdown so the strategy can flatten if configured to.
    virtual void onStop() {}
};

} // namespace hft

#endif // HFT_STRATEGY_BASE_H
