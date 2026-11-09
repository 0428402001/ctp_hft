// SPDX-License-Identifier: MIT
#include "hft/sim_matcher.h"

#include "hft/time_util.h"

#include <cstdio>
#include <cstring>

namespace hft {

const MarketTickSnap* SimMatcher::findPrice(const MarketSnapshotBlock* market,
                                            const char* instrument) const {
    if (!market) return nullptr;
    const uint32_t n = market->count.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < n && i < kMaxMarketInstruments; ++i) {
        if (std::strncmp(market->items[i].instrument, instrument, kInstrumentLen) == 0) {
            return &market->items[i];
        }
    }
    return nullptr;
}

void SimMatcher::emitOrderRtn(const PendingOrder& o, char status, const char* msg,
                              std::vector<ExecReport>& out) const {
    ExecReport r;
    std::memset(&r, 0, sizeof(r));
    r.magic = kMagicExec;
    r.kind = ExecType::OrderRtn;
    r.cmdId = o.cmdId;
    copyStr(r.instrument, o.instrument);
    copyStr(r.orderRef, o.orderRef);
    copyStr(r.orderSysID, o.orderSysID);
    r.direction = o.direction;
    r.offsetFlag = o.offsetFlag;
    r.status = status;
    r.priceType = o.priceType;
    r.price = o.price;
    r.volume = o.volume;
    r.volumeTraded = o.filled;
    r.volumeLeft = o.volume - o.filled;
    r.strategyTag = o.strategyTag;
    if (msg) copyStr(r.statusMsg, msg);
    r.rtnTimeNs = nowNs();
    out.push_back(r);
}

void SimMatcher::emitTrade(const PendingOrder& o, double price, int volume,
                           std::vector<ExecReport>& out) const {
    ExecReport r;
    std::memset(&r, 0, sizeof(r));
    r.magic = kMagicExec;
    r.kind = ExecType::TradeRtn;
    r.cmdId = o.cmdId;
    copyStr(r.instrument, o.instrument);
    copyStr(r.orderRef, o.orderRef);
    copyStr(r.orderSysID, o.orderSysID);
    r.direction = o.direction;
    r.offsetFlag = o.offsetFlag;
    r.status = (o.filled + volume >= o.volume) ? kAllTraded : kPartTradedQueueing;
    r.priceType = o.priceType;
    r.price = o.price;
    r.volume = o.volume;
    r.tradePrice = price;
    r.tradeVolume = volume;
    r.tradeAmount = price * volume;
    r.volumeTraded = o.filled + volume;
    r.volumeLeft = o.volume - o.filled - volume;
    r.strategyTag = o.strategyTag;
    r.rtnTimeNs = nowNs();
    out.push_back(r);
}

void SimMatcher::submit(const OrderCommand& cmd, const MarketSnapshotBlock* market,
                        std::vector<ExecReport>& out) {
    PendingOrder o;
    o.cmdId = cmd.cmdId;
    copyStr(o.instrument, cmd.instrument);
    o.direction = cmd.direction;
    o.offsetFlag = cmd.offsetFlag;
    o.priceType = cmd.priceType;
    o.timeCondition = cmd.timeCondition;
    o.price = cmd.price;
    o.volume = cmd.volume;
    o.strategyTag = cmd.strategyTag;
    o.insertTimeNs = nowNs();
    o.readyTimeNs = o.insertTimeNs + static_cast<int64_t>(latencyMs_) * 1000000ll;
    std::snprintf(o.orderRef, sizeof(o.orderRef), "%d", orderRefSeq_);
    std::snprintf(o.orderSysID, sizeof(o.orderSysID), "SIM%d", orderRefSeq_);
    ++orderRefSeq_;

    const MarketTickSnap* px = findPrice(market, o.instrument);
    if (!px || px->lastPrice <= 0) {
        // No price yet: reject, exactly like a gateway would.
        o.active = false;
        orders_.push_back(o);
        ExecReport r;
        std::memset(&r, 0, sizeof(r));
        r.magic = kMagicExec;
        r.kind = ExecType::InsertRsp;
        r.cmdId = o.cmdId;
        copyStr(r.instrument, o.instrument);
        copyStr(r.orderRef, o.orderRef);
        r.direction = o.direction;
        r.offsetFlag = o.offsetFlag;
        r.price = o.price;
        r.volume = o.volume;
        r.errorId = -1;
        copyStr(r.errorMsg, "no market price available yet");
        r.status = kUnknown;
        r.strategyTag = o.strategyTag;
        r.rtnTimeNs = nowNs();
        out.push_back(r);
        return;
    }

    const bool isBuy = (o.direction == static_cast<char>(Dir::Buy));
    double fillPrice = 0;
    bool canFill = false;
    if (o.priceType == static_cast<char>(PriceType::Any)) {
        fillPrice = isBuy ? px->askPrice1 : px->bidPrice1;
        canFill = true;
    } else {
        // Limit order: crosses the spread => immediate fill at the touch.
        if (isBuy && o.price >= px->askPrice1) {
            fillPrice = px->askPrice1;
            canFill = true;
        } else if (!isBuy && o.price <= px->bidPrice1) {
            fillPrice = px->bidPrice1;
            canFill = true;
        }
    }
    if (fillPrice <= 0) fillPrice = px->lastPrice;

    if (canFill && o.readyTimeNs <= nowNs()) {
        int qty = o.volume;
        if (partialFills_ && qty > 1) {
            // Realistic touch: part of the quantity is filled immediately.
            qty = qty / 2 + 1;
            if (qty > o.volume) qty = o.volume;
        }
        o.filled = qty;
        o.active = (qty < o.volume);
        orders_.push_back(o);
        emitTrade(o, fillPrice, qty, out);
        emitOrderRtn(o, o.active ? kPartTradedQueueing : kAllTraded, "filled", out);
        return;
    }

    if (o.timeCondition == '3') {  // IOC / FAK: kill what did not trade
        o.active = false;
        orders_.push_back(o);
        emitOrderRtn(o, kCanceled, "IOC not filled", out);
        return;
    }

    o.active = true;
    orders_.push_back(o);
    emitOrderRtn(o, kNoTradeQueueing, "accepted, resting", out);
}

void SimMatcher::cancel(const OrderCommand& cmd, std::vector<ExecReport>& out) {
    bool any = false;
    const bool cancelAll = (cmd.type == CmdType::CancelAll);
    for (auto& o : orders_) {
        if (!o.active) continue;
        if (cancelAll) {
            if (std::strncmp(o.instrument, cmd.instrument, kInstrumentLen) != 0) continue;
        } else if (std::strncmp(o.orderRef, cmd.orderRef, kOrderRefLen) != 0) {
            continue;
        }
        o.active = false;
        emitOrderRtn(o, kCanceled, "canceled by strategy", out);
        any = true;
        if (!cancelAll) break;
    }
    if (!any) {
        ExecReport r;
        std::memset(&r, 0, sizeof(r));
        r.magic = kMagicExec;
        r.kind = ExecType::ActionRsp;
        r.cmdId = cmd.cmdId;
        copyStr(r.instrument, cmd.instrument);
        copyStr(r.orderRef, cmd.orderRef);
        r.errorId = -2;
        copyStr(r.errorMsg, "order not found or already finished");
        r.status = kCanceled;
        r.strategyTag = cmd.strategyTag;
        r.rtnTimeNs = nowNs();
        out.push_back(r);
    }
}

void SimMatcher::match(const MarketSnapshotBlock* market, std::vector<ExecReport>& out) {
    if (orders_.empty() || !market) return;
    const int64_t now = nowNs();

    for (auto& o : orders_) {
        if (!o.active) continue;
        if (now < o.readyTimeNs) continue;

        const MarketTickSnap* px = findPrice(market, o.instrument);
        if (!px) continue;

        const bool isBuy = (o.direction == static_cast<char>(Dir::Buy));
        double fillPrice = 0;
        if (o.priceType == static_cast<char>(PriceType::Any)) {
            fillPrice = isBuy ? px->askPrice1 : px->bidPrice1;
        } else if (isBuy && o.price >= px->askPrice1) {
            fillPrice = px->askPrice1;
        } else if (!isBuy && o.price <= px->bidPrice1) {
            fillPrice = px->bidPrice1;
        }
        if (fillPrice <= 0) continue;

        int remaining = o.volume - o.filled;
        int qty = partialFills_ ? (remaining / 2 + 1) : remaining;
        if (qty > remaining) qty = remaining;

        o.filled += qty;
        emitTrade(o, fillPrice, qty, out);
        o.active = (o.filled < o.volume);
        emitOrderRtn(o, o.active ? kPartTradedQueueing : kAllTraded, "resting order filled", out);
    }

    // Drop finished orders so the book does not grow without bound.
    size_t write = 0;
    for (size_t i = 0; i < orders_.size(); ++i) {
        if (orders_[i].active) orders_[write++] = orders_[i];
    }
    orders_.resize(write);
}

size_t SimMatcher::pendingCount() const {
    size_t n = 0;
    for (const auto& o : orders_) {
        if (o.active) ++n;
    }
    return n;
}

} // namespace hft
