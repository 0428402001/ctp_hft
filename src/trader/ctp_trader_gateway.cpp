// SPDX-License-Identifier: MIT
#include "ctp_trader_gateway.h"

#include "common/ctp_compat.h"
#include "common/ctp_loader.h"

#ifdef HFT_ENABLE_CTP

#include "hft/logger.h"
#include "hft/thread_pin.h"
#include "hft/time_util.h"

#include <cstring>
#include <ctime>

namespace hft {

namespace {

bool isError(CThostFtdcRspInfoField* pRspInfo) { return pRspInfo && pRspInfo->ErrorID != 0; }

std::string describe(CThostFtdcRspInfoField* pRspInfo) {
    if (!pRspInfo) return "";
    char buf[256];
    std::snprintf(buf, sizeof(buf), "(%d) %s", pRspInfo->ErrorID, pRspInfo->ErrorMsg);
    return std::string(buf);
}

} // namespace

CtpTraderGateway::CtpTraderGateway(const Config& cfg) : cfg_(cfg) {
    workerCpu_ = cfg.getInt("trader", "ctp_cpu", -1);
}

CtpTraderGateway::~CtpTraderGateway() { stop(); }

bool CtpTraderGateway::start(std::string& err) {
    // Runtime-loaded SDK DLL (see ctp_loader.h) - keeps MinGW builds live-capable.
    if (!ctp::ctpLib().loadTrader(cfg_.ctpDllDir().c_str(), err)) return false;
    // Salt the order-ref base with the current second so a restarted process
    // cannot re-issue the same numeric OrderRef as an earlier session: CTP
    // replays the day's orders/trades and a ref collision would correlate a
    // replayed fill with a live order (phantom fills, double counts).
    orderRefBase_ = static_cast<int>(std::time(nullptr) % 100000) * 100;
    // Flow path: normalized (trailing separator - CTP concatenates the file
    // names without one) and wiped of stale session files from an earlier
    // trading day, which hang the first login of a new day.
    // Switch off with [general] clean_flow_on_start = false.
    const std::string flow = ctp::normalizeFlowPath(cfg_.tradeFlowPath());
    if (cfg_.getBool("general", "clean_flow_on_start", true)) ctp::prepareFlowDir(flow);
    api_ = ctp::ctpLib().createTraderApi(flow.c_str(), err);
    if (!api_) return false;
    api_->RegisterSpi(this);
    // Restart private/public topics from the beginning so the process always
    // sees the full order book state after a restart.
    api_->SubscribePrivateTopic(THOST_TERT_RESTART);
    api_->SubscribePublicTopic(THOST_TERT_RESTART);

    std::vector<std::string> fronts = cfg_.tradeFronts();
    if (fronts.empty()) {
        err = "no trading front configured ([trader] front / fronts)";
        return false;
    }
    for (auto& f : fronts) api_->RegisterFront(const_cast<char*>(f.c_str()));
    ctp::apiInit(api_);
    HFT_LOGI("trader api initialised (%s), version=%s", ctp::flavourName(),
             ctp::ctpLib().traderVersion());
    return true;
}

void CtpTraderGateway::stop() {
    if (!api_) return;
    api_->Release();
    api_ = nullptr;
    loggedIn_.store(false);
}

bool CtpTraderGateway::waitLogin(int timeoutMs) {
    for (int waited = 0; waited < timeoutMs; waited += 20) {
        if (loggedIn_.load(std::memory_order_acquire)) return true;
        sleepMs(20);
    }
    return loggedIn_.load(std::memory_order_acquire);
}

void CtpTraderGateway::emit(const ExecReport& r) const {
    if (execSink_) execSink_(r);
}

std::string CtpTraderGateway::nextOrderRef() {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%d", ++orderRefBase_);
    return std::string(buf);
}

void CtpTraderGateway::OnFrontConnected() {
    HFT_LOGI("trading front connected");
    if (statusSink_) statusSink_("connected");
    if (!cfg_.appId().empty() && !cfg_.authCode().empty()) {
        authenticate();
    } else {
        login();
    }
}

void CtpTraderGateway::OnFrontDisconnected(int nReason) {
    loggedIn_.store(false);
    HFT_LOGW("trading front disconnected, reason=%d", nReason);
    if (statusSink_) statusSink_("disconnected");
}

void CtpTraderGateway::OnHeartBeatWarning(int nTimeLapse) {
    HFT_LOGW("trading heartbeat warning: %ds", nTimeLapse);
}

void CtpTraderGateway::authenticate() {
    CThostFtdcReqAuthenticateField req;
    std::memset(&req, 0, sizeof(req));
    copyStr(req.BrokerID, cfg_.brokerId().c_str());
    copyStr(req.UserID, cfg_.userId().c_str());
    copyStr(req.AppID, cfg_.appId().c_str());
    copyStr(req.AuthCode, cfg_.authCode().c_str());
    copyStr(req.UserProductInfo, cfg_.productInfo().c_str());
    int rc = api_->ReqAuthenticate(&req, ++requestId_);
    if (rc != 0) HFT_LOGE("ReqAuthenticate returned %d", rc);
}

void CtpTraderGateway::OnRspAuthenticate(CThostFtdcRspAuthenticateField* pField,
                                         CThostFtdcRspInfoField* pRspInfo, int nRequestID,
                                         bool bIsLast) {
    if (isError(pRspInfo)) {
        lastError_ = "authenticate failed: " + describe(pRspInfo);
        HFT_LOGE("%s", lastError_.c_str());
        return;
    }
    HFT_LOGI("authenticated");
    login();
}

void CtpTraderGateway::login() {
    CThostFtdcReqUserLoginField req;
    std::memset(&req, 0, sizeof(req));
    copyStr(req.BrokerID, cfg_.brokerId().c_str());
    copyStr(req.UserID, cfg_.userId().c_str());
    copyStr(req.Password, cfg_.password().c_str());
    copyStr(req.UserProductInfo, cfg_.productInfo().c_str());
    int rc = api_->ReqUserLogin(&req, ++requestId_);
    if (rc != 0) {
        lastError_ = "ReqUserLogin returned " + std::to_string(rc);
        HFT_LOGE("%s", lastError_.c_str());
    }
}

void CtpTraderGateway::OnRspUserLogin(CThostFtdcRspUserLoginField* pRspUserLogin,
                                      CThostFtdcRspInfoField* pRspInfo, int nRequestID,
                                      bool bIsLast) {
    if (isError(pRspInfo)) {
        lastError_ = "trader login failed: " + describe(pRspInfo);
        HFT_LOGE("%s", lastError_.c_str());
        return;
    }
    if (pRspUserLogin) {
        frontId_ = pRspUserLogin->FrontID;
        sessionId_ = pRspUserLogin->SessionID;
        maxOrderRef_ = std::atoi(pRspUserLogin->MaxOrderRef);
        if (maxOrderRef_ > orderRefBase_) orderRefBase_ = maxOrderRef_;
        HFT_LOGI("trader login ok: front=%d session=%d maxOrderRef=%d tradingDay=%s", frontId_,
                 sessionId_, maxOrderRef_, pRspUserLogin->TradingDay);
    }
    loggedIn_.store(true, std::memory_order_release);
    if (statusSink_) statusSink_("logged in");
#if defined(HFT_CTP_STD)
    // Standard CTP counters (SimNow included) reject all orders until the
    // daily settlement statement has been confirmed, so do it immediately.
    confirmSettlement();
#endif
}

bool CtpTraderGateway::isReplay(int frontId, int sessionId, const char* orderRef) const {
    // An order reference we issued is ours, period - checked FIRST. The
    // SimNow 7x24 replay matcher re-tags notifications with a different
    // session id; if we trusted the session tag above the ref, the terminal
    // reports of our own orders would be dropped and the strategy would wait
    // forever on a dead pending slot.
    if (orderRef && *orderRef) {
        std::lock_guard<std::mutex> lock(refMutex_);
        if (myOrderRefs_.find(orderRef) != myOrderRefs_.end()) return false;
    }
    if (sessionId != sessionId_ || frontId != frontId_) {
        HFT_LOGD("replay dropped (order %s from session %d/%d, we are %d/%d)",
                 orderRef ? orderRef : "?", frontId, sessionId, frontId_, sessionId_);
        return true;
    }
    if (!orderRef || !*orderRef) return true;
    HFT_LOGD("replay dropped (order %s not issued by this session)", orderRef);
    return true;
}

bool CtpTraderGateway::isKnownOrderRef(const char* orderRef) const {
    if (!orderRef || !*orderRef) return false;
    std::lock_guard<std::mutex> lock(refMutex_);
    return myOrderRefs_.find(orderRef) != myOrderRefs_.end();
}

void CtpTraderGateway::rememberOrderRef(const std::string& ref) {
    std::lock_guard<std::mutex> lock(refMutex_);
    myOrderRefs_.insert(ref);
    if (myOrderRefs_.size() > 4096) {  // paranoia cap
        myOrderRefs_.clear();
        orderRefCmd_.clear();
    }
}

uint32_t CtpTraderGateway::cmdIdOf(const std::string& ref) const {
    std::lock_guard<std::mutex> lock(refMutex_);
    auto it = orderRefCmd_.find(ref);
    return (it == orderRefCmd_.end()) ? 0 : it->second;
}

#if defined(HFT_CTP_STD)
void CtpTraderGateway::confirmSettlement() {
    CThostFtdcSettlementInfoConfirmField req;
    std::memset(&req, 0, sizeof(req));
    copyStr(req.BrokerID, cfg_.brokerId().c_str());
    copyStr(req.InvestorID, cfg_.investorId().c_str());
    const int rc = api_->ReqSettlementInfoConfirm(&req, ++requestId_);
    if (rc != 0) {
        HFT_LOGW("settlement confirm request failed (rc=%d)", rc);
    } else {
        HFT_LOGI("settlement confirmation requested");
    }
}

void CtpTraderGateway::OnRspSettlementInfoConfirm(
    CThostFtdcSettlementInfoConfirmField* pSettlementInfoConfirm,
    CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) {
    (void)pSettlementInfoConfirm; (void)nRequestID; (void)bIsLast;
    if (isError(pRspInfo)) {
        HFT_LOGW("settlement confirm rejected: %s", describe(pRspInfo).c_str());
        return;
    }
    settleConfirmed_.store(true, std::memory_order_release);
    HFT_LOGI("settlement confirmed - orders are enabled");
}

bool CtpTraderGateway::waitSettlement(int timeoutMs) {
    for (int waited = 0; waited < timeoutMs; waited += 20) {
        if (settleConfirmed_.load(std::memory_order_acquire)) return true;
        sleepMs(20);
    }
    return settleConfirmed_.load(std::memory_order_acquire);
}
#endif

int CtpTraderGateway::sendOrder(const OrderCommand& cmd) {
    if (!api_ || !loggedIn_.load(std::memory_order_acquire)) return -1;

    CThostFtdcInputOrderField o;
    std::memset(&o, 0, sizeof(o));
    copyStr(o.BrokerID, cfg_.brokerId().c_str());
    copyStr(o.InvestorID, cfg_.investorId().c_str());
    copyStr(o.InstrumentID, cmd.instrument);
    copyStr(o.UserID, cfg_.userId().c_str());
    std::string ref = nextOrderRef();
    copyStr(o.OrderRef, ref.c_str());
    rememberOrderRef(ref);
    {
        // Live reports must carry the strategy's cmdId or the strategy cannot
        // match a fill to its order (the sim matcher does this natively).
        std::lock_guard<std::mutex> lock(refMutex_);
        orderRefCmd_[ref] = cmd.cmdId;
    }
    o.OrderPriceType = cmd.priceType;
    o.Direction = cmd.direction;
    o.CombOffsetFlag[0] = cmd.offsetFlag;
    o.CombHedgeFlag[0] = cmd.hedgeFlag ? cmd.hedgeFlag : '1';
    o.LimitPrice = cmd.price;
    o.VolumeTotalOriginal = cmd.volume;
    o.TimeCondition = cmd.timeCondition ? cmd.timeCondition : '1';
    o.VolumeCondition = cmd.volumeCondition ? cmd.volumeCondition : '1';
    o.MinVolume = cmd.minVolume;
    o.ContingentCondition = '1';  // immediately
    o.ForceCloseReason = '0';
    o.IsAutoSuspend = 0;
    o.UserForceClose = 0;

    int rc = api_->ReqOrderInsert(&o, ++requestId_);
    if (rc != 0) {
        ExecReport r;
        std::memset(&r, 0, sizeof(r));
        r.magic = kMagicExec;
        r.kind = ExecType::InsertRsp;
        r.cmdId = cmd.cmdId;
        copyStr(r.instrument, cmd.instrument);
        copyStr(r.orderRef, ref.c_str());
        r.direction = cmd.direction;
        r.offsetFlag = cmd.offsetFlag;
        r.price = cmd.price;
        r.volume = cmd.volume;
        r.errorId = rc;
        copyStr(r.errorMsg, "ReqOrderInsert rejected locally");
        r.status = kUnknown;
        r.strategyTag = cmd.strategyTag;
        r.rtnTimeNs = nowNs();
        emit(r);
    }
    return rc;
}

int CtpTraderGateway::cancelOrder(const OrderCommand& cmd) {
    if (!api_ || !loggedIn_.load(std::memory_order_acquire)) return -1;

    CThostFtdcInputOrderActionField a;
    std::memset(&a, 0, sizeof(a));
    copyStr(a.BrokerID, cfg_.brokerId().c_str());
    copyStr(a.InvestorID, cfg_.investorId().c_str());
    copyStr(a.InstrumentID, cmd.instrument);
    copyStr(a.ExchangeID, cmd.exchangeID);
    copyStr(a.OrderRef, cmd.orderRef);
    copyStr(a.OrderSysID, cmd.orderSysID);
    copyStr(a.UserID, cfg_.userId().c_str());
    a.FrontID = cmd.frontID ? cmd.frontID : frontId_;
    a.SessionID = cmd.sessionID ? cmd.sessionID : sessionId_;
    a.ActionFlag = '0';  // delete
    a.LimitPrice = cmd.price;
    a.VolumeChange = 0;

    int rc = api_->ReqOrderAction(&a, ++requestId_);
    if (rc != 0) {
        ExecReport r;
        std::memset(&r, 0, sizeof(r));
        r.magic = kMagicExec;
        r.kind = ExecType::ActionRsp;
        r.cmdId = cmd.cmdId;
        copyStr(r.instrument, cmd.instrument);
        copyStr(r.orderRef, cmd.orderRef);
        r.errorId = rc;
        copyStr(r.errorMsg, "ReqOrderAction rejected locally");
        r.status = kCanceled;
        r.strategyTag = cmd.strategyTag;
        r.rtnTimeNs = nowNs();
        emit(r);
    }
    return rc;
}

void CtpTraderGateway::OnRspOrderInsert(CThostFtdcInputOrderField* pInputOrder,
                                        CThostFtdcRspInfoField* pRspInfo, int nRequestID,
                                        bool bIsLast) {
    // ErrRtn variants are replayed by the counter to every new session, and
    // they carry no session id; the order-ref gate drops those replays.
    if (pInputOrder && !isKnownOrderRef(pInputOrder->OrderRef)) {
        HFT_LOGD("replay dropped (err-rtn insert, order %s not ours)", pInputOrder->OrderRef);
        return;
    }
    if (isError(pRspInfo)) {
        ExecReport r;
        std::memset(&r, 0, sizeof(r));
        r.magic = kMagicExec;
        r.kind = ExecType::InsertRsp;
        r.errorId = pRspInfo->ErrorID;
        copyStr(r.errorMsg, pRspInfo->ErrorMsg);
        if (pInputOrder) {
            copyStr(r.instrument, pInputOrder->InstrumentID);
            copyStr(r.orderRef, pInputOrder->OrderRef);
            r.cmdId = cmdIdOf(pInputOrder->OrderRef);
            r.direction = pInputOrder->Direction;
            r.price = pInputOrder->LimitPrice;
            r.volume = pInputOrder->VolumeTotalOriginal;
        }
        r.status = kUnknown;
        r.rtnTimeNs = nowNs();
        emit(r);
    }
}

void CtpTraderGateway::OnErrRtnOrderInsert(CThostFtdcInputOrderField* pInputOrder,
                                           CThostFtdcRspInfoField* pRspInfo) {
    OnRspOrderInsert(pInputOrder, pRspInfo, 0, true);
}

void CtpTraderGateway::OnRspOrderAction(CThostFtdcInputOrderActionField* pInputOrderAction,
                                        CThostFtdcRspInfoField* pRspInfo, int nRequestID,
                                        bool bIsLast) {
    if (pInputOrderAction && !isKnownOrderRef(pInputOrderAction->OrderRef)) {
        HFT_LOGD("replay dropped (err-rtn action, order %s not ours)",
                 pInputOrderAction->OrderRef);
        return;
    }
    if (isError(pRspInfo)) {
        ExecReport r;
        std::memset(&r, 0, sizeof(r));
        r.magic = kMagicExec;
        r.kind = ExecType::ActionRsp;
        r.errorId = pRspInfo->ErrorID;
        copyStr(r.errorMsg, pRspInfo->ErrorMsg);
        if (pInputOrderAction) {
            copyStr(r.instrument, pInputOrderAction->InstrumentID);
            copyStr(r.orderRef, pInputOrderAction->OrderRef);
            r.cmdId = cmdIdOf(pInputOrderAction->OrderRef);
        }
        r.status = kCanceled;
        r.rtnTimeNs = nowNs();
        emit(r);
    }
}

void CtpTraderGateway::OnErrRtnOrderAction(CThostFtdcOrderActionField* pOrderAction,
                                           CThostFtdcRspInfoField* pRspInfo) {
    if (pOrderAction && !isKnownOrderRef(pOrderAction->OrderRef)) {
        HFT_LOGD("replay dropped (err-rtn action, order %s not ours)", pOrderAction->OrderRef);
        return;
    }
    if (isError(pRspInfo)) {
        ExecReport r;
        std::memset(&r, 0, sizeof(r));
        r.magic = kMagicExec;
        r.kind = ExecType::ActionRsp;
        r.errorId = pRspInfo->ErrorID;
        copyStr(r.errorMsg, pRspInfo->ErrorMsg);
        if (pOrderAction) {
            copyStr(r.instrument, pOrderAction->InstrumentID);
            copyStr(r.orderRef, pOrderAction->OrderRef);
            r.cmdId = cmdIdOf(pOrderAction->OrderRef);
        }
        r.status = kCanceled;
        r.rtnTimeNs = nowNs();
        emit(r);
    }
}

void CtpTraderGateway::OnRtnOrder(CThostFtdcOrderField* pOrder) {
    if (!pOrder) return;
    pinCurrentThreadOnce(workerCpu_, workerPin_);  // first callback pins the SDK thread
    if (isReplay(pOrder->FrontID, pOrder->SessionID, pOrder->OrderRef)) return;
    ExecReport r;
    std::memset(&r, 0, sizeof(r));
    r.magic = kMagicExec;
    r.kind = ExecType::OrderRtn;
    copyStr(r.instrument, pOrder->InstrumentID);
    copyStr(r.orderRef, pOrder->OrderRef);
    copyStr(r.orderSysID, pOrder->OrderSysID);
    r.cmdId = cmdIdOf(pOrder->OrderRef);
    r.direction = pOrder->Direction;
    r.offsetFlag = pOrder->CombOffsetFlag[0];
    r.status = pOrder->OrderStatus;
    r.priceType = pOrder->OrderPriceType;
    r.price = pOrder->LimitPrice;
    r.volume = pOrder->VolumeTotalOriginal;
    r.volumeTraded = pOrder->VolumeTraded;
    r.volumeLeft = pOrder->VolumeTotal;
    r.frontID = pOrder->FrontID;
    r.sessionID = pOrder->SessionID;
    copyStr(r.statusMsg, pOrder->StatusMsg);
    r.rtnTimeNs = nowNs();
    // Terminal states deserve a permanent INF record: it is the only place
    // where counter-side/auto cancels (SimNow replay auto-cancel, other
    // sessions, exchange kills) become visible. StatusMsg carries the reason.
    if (pOrder->OrderStatus == kCanceled || pOrder->OrderStatus == '4' ||
        pOrder->OrderStatus == '2') {
        HFT_LOGI("RTN order %s sysid=%s sess=%d/%d status=%c traded=%d left=%d msg=%s",
                 pOrder->OrderRef, pOrder->OrderSysID, pOrder->FrontID, pOrder->SessionID,
                 pOrder->OrderStatus, pOrder->VolumeTraded, pOrder->VolumeTotal,
                 pOrder->StatusMsg);
    }
    if (isOrderFinished(r.status)) {
        // terminal: forget the ref so late/other-session reports with the same
        // numeric ref can never be correlated against a dead cmdId
        std::lock_guard<std::mutex> lock(refMutex_);
        orderRefCmd_.erase(pOrder->OrderRef);
    }
    emit(r);
}

void CtpTraderGateway::OnRtnTrade(CThostFtdcTradeField* pTrade) {
    if (!pTrade) return;
    if (isReplay(frontId_, sessionId_, pTrade->OrderRef)) return;
    ExecReport r;
    std::memset(&r, 0, sizeof(r));
    r.magic = kMagicExec;
    r.kind = ExecType::TradeRtn;
    copyStr(r.instrument, pTrade->InstrumentID);
    copyStr(r.orderRef, pTrade->OrderRef);
    copyStr(r.orderSysID, pTrade->OrderSysID);
    r.cmdId = cmdIdOf(pTrade->OrderRef);
    r.direction = pTrade->Direction;
    r.offsetFlag = pTrade->OffsetFlag;
    r.status = kAllTraded;
    r.price = pTrade->Price;
    r.tradePrice = pTrade->Price;
    r.tradeVolume = pTrade->Volume;
    r.tradeAmount = pTrade->Price * pTrade->Volume;
    r.volumeTraded = pTrade->Volume;
    r.frontID = frontId_;
    r.sessionID = sessionId_;
    r.rtnTimeNs = nowNs();
    emit(r);
}

void CtpTraderGateway::OnRspError(CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) {
    HFT_LOGE("trader rsp error: %s", describe(pRspInfo).c_str());
}

void CtpTraderGateway::OnRtnInstrumentStatus(CThostFtdcInstrumentStatusField* pInstrumentStatus) {
    if (!pInstrumentStatus) return;
    HFT_LOGD("instrument status %s -> %c", pInstrumentStatus->InstrumentID,
             pInstrumentStatus->InstrumentStatus);
}

} // namespace hft

#endif // HFT_ENABLE_CTP
