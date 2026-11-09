// SPDX-License-Identifier: MIT
// protocol.h - Inter-process message definitions.
//
// Every struct below is laid out in shared memory and must therefore be:
//   * fixed size, no pointers, no heap, no vtable;
//   * self describing (magic + type tag) so a version mismatch is detectable.
#ifndef HFT_PROTOCOL_H
#define HFT_PROTOCOL_H

#include "hft/platform.h"

namespace hft {

// ---- fixed string capacities ----------------------------------------------
constexpr uint32_t kInstrumentLen = 32;
constexpr uint32_t kExchangeLen = 12;
constexpr uint32_t kDateLen = 12;
constexpr uint32_t kTimeLen = 16;
constexpr uint32_t kOrderRefLen = 24;
constexpr uint32_t kOrderSysIdLen = 32;
constexpr uint32_t kMsgLen = 128;
constexpr uint32_t kNameLen = 64;

// ---- magic numbers (cheap corruption / version guard) ---------------------
constexpr uint32_t kMagicTick = 0x5449'434Bu;      // "TICK"
constexpr uint32_t kMagicCmd = 0x434D'4444u;       // "CMDD"
constexpr uint32_t kMagicExec = 0x4558'4543u;      // "EXEC"
constexpr uint32_t kMagicQryReq = 0x5152'4551u;    // "QREQ"
constexpr uint32_t kMagicQryRsp = 0x5152'5350u;    // "QRSP"
constexpr uint32_t kMagicShm = 0x4846'5453u;       // "HFTS"

// ---- role ids -------------------------------------------------------------
enum class Role : uint32_t { Md = 0, Strategy = 1, Trader = 2, Query = 3 };

inline const char* roleName(Role r) {
    switch (r) {
    case Role::Md: return "MD";
    case Role::Strategy: return "STRATEGY";
    case Role::Trader: return "TRADER";
    case Role::Query: return "QUERY";
    }
    return "UNKNOWN";
}

// ---- helpers --------------------------------------------------------------
template <uint32_t N>
inline void copyStr(char (&dst)[N], const char* src) {
    if (!src) {
        dst[0] = '\0';
        return;
    }
    const uint32_t n = N - 1u;
    uint32_t i = 0;
    for (; i < n && src[i] != '\0'; ++i) dst[i] = src[i];
    dst[i] = '\0';
}

// ---------------------------------------------------------------------------
// 1) Market data process -> strategy process
// ---------------------------------------------------------------------------
struct TickMsg {
    uint32_t magic;
    uint32_t seq;                        // monotonic per instrument stream
    char instrument[kInstrumentLen];
    char exchangeID[kExchangeLen];
    char tradingDay[kDateLen];
    char updateTime[kTimeLen];           // exchange time "HH:MM:SS"
    uint32_t updateMillisec;
    double lastPrice;
    double openPrice;
    double highestPrice;
    double lowestPrice;
    double preClosePrice;
    double preSettlementPrice;
    double upperLimitPrice;
    double lowerLimitPrice;
    double averagePrice;
    double bidPrice1;
    double askPrice1;
    int32_t bidVolume1;
    int32_t askVolume1;
    double volume;                       // cumulative volume of the day
    double turnover;
    double openInterest;
    int64_t recvTimeNs;                  // when the gateway saw the tick
    int64_t publishTimeNs;               // when it hit the ring buffer
};

// ---------------------------------------------------------------------------
// 2) Strategy process -> trading process
// ---------------------------------------------------------------------------
enum class CmdType : uint32_t {
    Insert = 1,        // new order
    Cancel = 2,        // cancel by orderRef / orderSysID
    CancelAll = 3,     // cancel everything for one instrument
    RiskHalt = 4,      // strategy asks the gateway to stop accepting orders
    RiskResume = 5,
};

enum class Dir : char { Buy = '0', Sell = '1' };
enum class Offset : char {
    Open = '0',
    Close = '1',
    ForceClose = '2',
    CloseToday = '3',
    CloseYesterday = '4',
};
enum class PriceType : char {
    Any = '1',
    Limit = '2',
    Best = '3',
    Last = '4',
};

struct OrderCommand {
    uint32_t magic;
    uint32_t cmdId;                      // strategy side monotonic id
    CmdType type;
    char instrument[kInstrumentLen];
    char exchangeID[kExchangeLen];
    char direction;                      // Dir
    char offsetFlag;                     // Offset
    char hedgeFlag;                      // '1' speculation, '2' arbitrage, '3' hedge
    char priceType;                      // PriceType
    double price;
    int32_t volume;
    int32_t minVolume;
    char timeCondition;                  // '1' GFD, '3' IOC
    char volumeCondition;                // '1' any volume, '3' complete volume
    uint64_t strategyTag;                // echoed back on every report
    int64_t sendTimeNs;
    // filled in for Cancel / CancelAll:
    char orderRef[kOrderRefLen];
    char orderSysID[kOrderSysIdLen];
    int32_t frontID;
    int32_t sessionID;
};

// ---------------------------------------------------------------------------
// 3) Trading process -> strategy process (order/trade feedback)
// ---------------------------------------------------------------------------
enum class ExecType : uint32_t {
    OrderRtn = 1,       // OnRtnOrder
    TradeRtn = 2,       // OnRtnTrade
    InsertRsp = 3,      // OnRspOrderInsert / OnErrRtnOrderInsert
    ActionRsp = 4,      // OnRspOrderAction / OnErrRtnOrderAction
    StatusMsg = 5,      // connection / login status
};

// Mirrors CThostFtdcOrderStatusType values.
// NOTE: 'a' (kUnknown/kSubmitted) means "order has been submitted" - the order
// is ALIVE and working. It must never be treated as terminal. Terminal states
// are: fully traded, canceled, and the two "not in queue" states.
enum OrderStatus : char {
    kAllTraded = '0',
    kPartTradedQueueing = '1',
    kPartTradedNotQueueing = '2',
    kNoTradeQueueing = '3',
    kNoTradeNotQueueing = '4',
    kCanceled = '5',
    kUnknown = 'a',       // CTP: order submitted (still live and working)
    kNotTouched = 'b',
    kTouched = 'c',
};

inline bool isOrderFinished(char status) {
    // '0' fully traded, '5' canceled, '2'/'4' left the queue (terminal).
    // Deliberately NOT 'a' (submitted), 'b' (cancel submitted), 'c' (modify
    // submitted), '1'/'3' (queueing) - those are all still working.
    return status == kAllTraded || status == kCanceled ||
           status == kPartTradedNotQueueing || status == kNoTradeNotQueueing;
}

struct ExecReport {
    uint32_t magic;
    ExecType kind;
    uint32_t cmdId;                      // 0 when the report is unsolicited
    char instrument[kInstrumentLen];
    char orderRef[kOrderRefLen];
    char orderSysID[kOrderSysIdLen];
    char direction;
    char offsetFlag;
    char status;                         // OrderStatus (for order reports)
    char priceType;
    double price;                        // order limit price
    int32_t volume;                      // original volume
    int32_t volumeTraded;
    int32_t volumeLeft;
    double tradePrice;                   // valid when kind == TradeRtn
    int32_t tradeVolume;
    double tradeAmount;
    int32_t frontID;
    int32_t sessionID;
    uint64_t strategyTag;
    int32_t errorId;
    char errorMsg[kMsgLen];
    char statusMsg[kMsgLen];
    int64_t rtnTimeNs;
};

// ---------------------------------------------------------------------------
// 4) Strategy process <-> query process
// ---------------------------------------------------------------------------
enum class QueryType : uint32_t {
    Account = 1,
    Position = 2,
    Instrument = 3,
    Orders = 4,
    Trades = 5,
    CommissionRate = 6,
    MarginRate = 7,
    TradingDay = 8,
    All = 100,       // refresh the shared snapshot with everything
};

struct QueryRequest {
    uint32_t magic;
    uint32_t reqId;
    QueryType type;
    char instrument[kInstrumentLen];
    int64_t sendTimeNs;
};

struct AccountSnap {
    double preBalance;
    double balance;
    double available;
    double currMargin;
    double frozenMargin;
    double commission;
    double closeProfit;
    double positionProfit;
    double withdrawQuota;
    char tradingDay[kDateLen];
};

struct PositionSnap {
    char instrument[kInstrumentLen];
    char direction;                      // '2' long, '3' short
    char positionDate;                   // '1' today, '2' yesterday
    int32_t ydPosition;
    int32_t position;
    int32_t todayPosition;
    int32_t longFrozen;
    int32_t shortFrozen;
    int32_t openVolume;
    int32_t closeVolume;
    double positionCost;
    double openCost;
    double useMargin;
    double positionProfit;
    double closeProfit;
    double settlementPrice;
};

struct InstrumentSnap {
    char instrument[kInstrumentLen];
    char exchangeID[kExchangeLen];
    char productID[kInstrumentLen];
    int32_t volumeMultiple;
    double priceTick;
    double longMarginRatio;
    double shortMarginRatio;
    int32_t isTrading;
    // Market snapshot maintained by the market-data process (8 byte aligned =>
    // tear free on x64). Used by the simulator's matcher and by strategies that
    // want a "latest price" without waiting for the next tick.
    double lastPrice;
    double bidPrice1;
    double askPrice1;
    int64_t tickTimeNs;
};

struct QueryResponse {
    uint32_t magic;
    uint32_t reqId;
    QueryType type;
    uint32_t isLast;
    int32_t errorId;
    char errorMsg[kMsgLen];
    AccountSnap account;
    PositionSnap position;
    InstrumentSnap instrument;
    int64_t rtnTimeNs;
};

// ---------------------------------------------------------------------------
// Shared snapshot maintained by the query process (seqlock protected)
// ---------------------------------------------------------------------------
constexpr uint32_t kMaxPositions = 512;
constexpr uint32_t kMaxInstruments = 4096;

struct SnapshotBlock {
    // Writer: seq++ (odd) -> write payload -> seq++ (even).
    // Reader: sample seq, retry if odd or if it changed while copying.
    std::atomic<uint32_t> seq{0};
    std::atomic<uint64_t> updateTimeNs{0};
    std::atomic<uint32_t> posCount{0};
    std::atomic<uint32_t> instrCount{0};
    std::atomic<uint32_t> queryRound{0};
    char tradingDay[kDateLen];
    AccountSnap account;
    PositionSnap positions[kMaxPositions];
    InstrumentSnap instruments[kMaxInstruments];
};

// ---------------------------------------------------------------------------
// Market snapshot: written by the market-data process, read by everyone else.
// Plain doubles on naturally aligned offsets => tear free on x64, no locking.
// ---------------------------------------------------------------------------
constexpr uint32_t kMaxMarketInstruments = 256;

struct MarketTickSnap {
    char instrument[kInstrumentLen];
    double lastPrice;
    double bidPrice1;
    double askPrice1;
    double openPrice;
    double highestPrice;
    double lowestPrice;
    int32_t bidVolume1;
    int32_t askVolume1;
    int64_t tickTimeNs;
    int64_t recvTimeNs;
};

struct MarketSnapshotBlock {
    std::atomic<uint32_t> count{0};
    MarketTickSnap items[kMaxMarketInstruments];
};

// ---------------------------------------------------------------------------
// Control block shared by all four processes
// ---------------------------------------------------------------------------
struct Heartbeat {
    std::atomic<uint32_t> pid{0};
    std::atomic<uint64_t> lastBeatNs{0};
    std::atomic<uint64_t> beatCount{0};
    std::atomic<uint32_t> alive{0};
};

struct ControlBlock {
    std::atomic<uint32_t> magic{kMagicShm};
    std::atomic<uint32_t> shutdown{0};          // 1 => every process must exit
    std::atomic<uint32_t> tradingEnabled{1};    // master switch for new positions
    std::atomic<uint32_t> flatten{0};           // 1 => strategy must close everything
    std::atomic<uint32_t> readyMask{0};         // bit(Role)
    std::atomic<uint32_t> mdLoggedIn{0};
    std::atomic<uint32_t> traderLoggedIn{0};
    std::atomic<uint32_t> queryLoggedIn{0};
    Heartbeat hb[4];
};

inline uint32_t roleBit(Role r) { return 1u << static_cast<uint32_t>(r); }

} // namespace hft

#endif // HFT_PROTOCOL_H
