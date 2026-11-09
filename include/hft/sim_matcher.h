// SPDX-License-Identifier: MIT
// sim_matcher.h - Deterministic matching engine for the "sim" mode.
//
// It consumes the same OrderCommand structures the real CTP gateway consumes and
// emits exactly the same ExecReport stream, so the strategy process cannot tell
// the difference.
#ifndef HFT_SIM_MATCHER_H
#define HFT_SIM_MATCHER_H

#include "hft/protocol.h"

#include <cstdint>
#include <string>
#include <vector>

namespace hft {

class SimMatcher {
public:
    struct PendingOrder {
        uint32_t cmdId = 0;
        char instrument[kInstrumentLen] = {0};
        char direction = '0';
        char offsetFlag = '0';
        char priceType = '2';
        char timeCondition = '1';
        double price = 0.0;
        int volume = 0;
        int filled = 0;
        char orderRef[kOrderRefLen] = {0};
        char orderSysID[kOrderSysIdLen] = {0};
        uint64_t strategyTag = 0;
        int64_t insertTimeNs = 0;
        int64_t readyTimeNs = 0;   // simulated latency
        bool active = false;
    };

    void setLatencyMs(int ms) { latencyMs_ = ms; }
    void setFillPartial(bool on) { partialFills_ = on; }

    // New order. Emits: accepted/rejected + (optionally) a trade.
    void submit(const OrderCommand& cmd, const MarketSnapshotBlock* market,
                std::vector<ExecReport>& out);

    // Cancel by orderRef (CTP style) or everything of one instrument.
    void cancel(const OrderCommand& cmd, std::vector<ExecReport>& out);

    // Called on every loop iteration: fills or expires resting orders.
    void match(const MarketSnapshotBlock* market, std::vector<ExecReport>& out);

    size_t pendingCount() const;

private:
    const MarketTickSnap* findPrice(const MarketSnapshotBlock* market, const char* instrument) const;
    void emitOrderRtn(const PendingOrder& o, char status, const char* msg,
                      std::vector<ExecReport>& out) const;
    void emitTrade(const PendingOrder& o, double price, int volume, std::vector<ExecReport>& out) const;

    std::vector<PendingOrder> orders_;
    int orderRefSeq_ = 1;
    int latencyMs_ = 0;
    bool partialFills_ = true;
};

} // namespace hft

#endif // HFT_SIM_MATCHER_H
