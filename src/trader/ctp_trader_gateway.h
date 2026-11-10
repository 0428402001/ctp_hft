// SPDX-License-Identifier: MIT
// ctp_trader_gateway.h - CTP trading gateway (compiled only with HFT_ENABLE_CTP).
#ifndef HFT_CTP_TRADER_GATEWAY_H
#define HFT_CTP_TRADER_GATEWAY_H

#include "hft/config.h"
#include "hft/protocol.h"

#ifdef HFT_ENABLE_CTP

#include "ThostFtdcTraderApi.h"

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <unordered_set>

namespace hft {

// Thin wrapper around CThostFtdcTraderApi that translates OrderCommand ->
// ReqOrderInsert and CThostFtdcOrderField/TradeField -> ExecReport.
//
// The CTP callbacks arrive on the API thread; they are the only writer of the
// execution ring buffers, which keeps the SPSC contract intact.
class CtpTraderGateway : public CThostFtdcTraderSpi {
public:
    using ExecSink = std::function<void(const ExecReport&)>;
    using StatusSink = std::function<void(const char*)>;

    explicit CtpTraderGateway(const Config& cfg);
    ~CtpTraderGateway();

    bool start(std::string& err);
    void stop();
    bool loggedIn() const { return loggedIn_.load(std::memory_order_acquire); }
    bool waitLogin(int timeoutMs);
#if defined(HFT_CTP_STD)
    bool settlementConfirmed() const { return settleConfirmed_.load(std::memory_order_acquire); }
    // Blocks until the settlement statement is confirmed (or the timeout
    // hits). Returns false on timeout - the caller decides whether that is
    // fatal. No-op (always true) on the Mini flavour.
    bool waitSettlement(int timeoutMs);
#endif

    int sendOrder(const OrderCommand& cmd);
    int cancelOrder(const OrderCommand& cmd);

    void setExecSink(ExecSink sink) { execSink_ = std::move(sink); }
    void setStatusSink(StatusSink sink) { statusSink_ = std::move(sink); }
    const std::string& lastError() const { return lastError_; }

    int frontId() const { return frontId_; }
    int sessionId() const { return sessionId_; }

    // ---- CThostFtdcTraderSpi -----------------------------------------------
    void OnFrontConnected() override;
    void OnFrontDisconnected(int nReason) override;
    void OnHeartBeatWarning(int nTimeLapse) override;
    void OnRspAuthenticate(CThostFtdcRspAuthenticateField* pRspAuthenticateField,
                           CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) override;
    void OnRspUserLogin(CThostFtdcRspUserLoginField* pRspUserLogin,
                        CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) override;
    void OnRspOrderInsert(CThostFtdcInputOrderField* pInputOrder, CThostFtdcRspInfoField* pRspInfo,
                          int nRequestID, bool bIsLast) override;
    void OnRspOrderAction(CThostFtdcInputOrderActionField* pInputOrderAction,
                          CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) override;
    void OnErrRtnOrderInsert(CThostFtdcInputOrderField* pInputOrder,
                             CThostFtdcRspInfoField* pRspInfo) override;
    void OnErrRtnOrderAction(CThostFtdcOrderActionField* pOrderAction,
                             CThostFtdcRspInfoField* pRspInfo) override;
    void OnRtnOrder(CThostFtdcOrderField* pOrder) override;
    void OnRtnTrade(CThostFtdcTradeField* pTrade) override;
    void OnRspError(CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) override;
    void OnRtnInstrumentStatus(CThostFtdcInstrumentStatusField* pInstrumentStatus) override;
#if defined(HFT_CTP_STD)
    // Standard CTP only (the Mini SDK has no settlement confirmation). Most
    // counters - SimNow in particular - reject every order until the
    // settlement statement of the day has been confirmed.
    void OnRspSettlementInfoConfirm(CThostFtdcSettlementInfoConfirmField* pSettlementInfoConfirm,
                                    CThostFtdcRspInfoField* pRspInfo, int nRequestID,
                                    bool bIsLast) override;
#endif

private:
    void authenticate();
    void login();
    // Standard CTP replays the whole day's orders/trades to every new session
    // (SubscribePrivateTopic). Reports for orders that were NOT issued by this
    // session (other front/session id, unknown order ref) are replays of
    // earlier activity - forwarding them would double-count positions in the
    // strategy process, so they are dropped here.
    bool isReplay(int frontId, int sessionId, const char* orderRef) const;
    bool isKnownOrderRef(const char* orderRef) const;
    void rememberOrderRef(const std::string& ref);
#if defined(HFT_CTP_STD)
    void confirmSettlement();
#endif
    std::string nextOrderRef();
    void emit(const ExecReport& r) const;

    const Config& cfg_;
    CThostFtdcTraderApi* api_ = nullptr;
    std::atomic<bool> loggedIn_{false};
#if defined(HFT_CTP_STD)
    std::atomic<bool> settleConfirmed_{false};
#endif
    std::atomic<int> requestId_{0};
    // CTP worker-thread CPU pinning ([trader] ctp_cpu, -1 = off): applied
    // from inside the first order/trade callback, because the thread belongs
    // to the SDK.
    std::atomic<int> workerPin_{0};
    int workerCpu_ = -1;
    int orderRefBase_ = 1;
    int frontId_ = 0;
    int sessionId_ = 0;
    int maxOrderRef_ = 0;
    ExecSink execSink_;
    StatusSink statusSink_;
    // Written from the poll thread (sendOrder), read from the CTP callback
    // thread (OnRtn*): a plain mutex is fine, the critical section is tiny.
    mutable std::mutex refMutex_;
    std::unordered_set<std::string> myOrderRefs_;
    // OrderRef -> strategy cmdId. Without this every live ExecReport would
    // carry cmdId=0 and the strategy could never match a fill to the order
    // that produced it (the sim matcher fills cmdId directly, so the gap
    // only shows up on live CTP).
    std::map<std::string, uint32_t> orderRefCmd_;
    uint32_t cmdIdOf(const std::string& ref) const;
    std::string lastError_;
};

} // namespace hft

#endif // HFT_ENABLE_CTP
#endif // HFT_CTP_TRADER_GATEWAY_H
