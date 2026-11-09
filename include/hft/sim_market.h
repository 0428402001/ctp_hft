// SPDX-License-Identifier: MIT
// sim_market.h - Synthetic tick generator used by the "sim" mode.
//
// The simulator lets the whole four-process pipeline run end to end without a
// broker connection: same shared memory, same lock-free queues, same strategy
// code - only the last mile (socket to the front) is replaced.
#ifndef HFT_SIM_MARKET_H
#define HFT_SIM_MARKET_H

#include "hft/config.h"
#include "hft/protocol.h"

#include <cstdint>
#include <string>
#include <vector>

namespace hft {

class SimMarket {
public:
    struct Instrument {
        char name[kInstrumentLen];
        double priceTick = 1.0;
        double initialPrice = 3500.0;
        double lastPrice = 3500.0;
        double openPrice = 3500.0;
        double highestPrice = 3500.0;
        double lowestPrice = 3500.0;
        double volatility = 0.00035;   // relative sigma per tick
        double meanReversion = 0.002;  // pull back to initialPrice
        double spreadTicks = 1.0;
        int volumeMultiple = 10;
        int tickEveryMs = 500;
        double volume = 0;
        double turnover = 0;
        double openInterest = 100000;
        int bidVolume = 10;
        int askVolume = 10;
        int64_t nextTickNs = 0;
    };

    bool init(const Config& cfg, std::string& err);

    // Produces the next tick if it is due. Returns false when the generator
    // still has time left (caller may sleep).
    bool next(TickMsg& out);

    // Publishes the current book into the shared market snapshot.
    void publishSnapshot(MarketSnapshotBlock* block) const;

    const std::vector<Instrument>& instruments() const { return instruments_; }
    size_t size() const { return instruments_.size(); }

    // Milliseconds until the next tick is due (0 == now).
    int64_t msUntilNextTick() const;

private:
    double normal();
    double uniform();
    double roundToTick(double price, double tick) const;

    std::vector<Instrument> instruments_;
    uint64_t rngState_ = 0x853c49e6748fea9bull;
    uint32_t seq_ = 0;
    bool started_ = false;
};

} // namespace hft

#endif // HFT_SIM_MARKET_H
