// SPDX-License-Identifier: MIT
#include "ctp_query_gateway.h"

#include "common/ctp_compat.h"
#include "common/ctp_loader.h"

#ifdef HFT_ENABLE_CTP

#include "hft/logger.h"
#include "hft/time_util.h"

#include <cstring>

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

CtpQueryGateway::CtpQueryGateway(const Config& cfg, SnapshotBlock* snapshot)
    : cfg_(cfg), snapshot_(snapshot) {}

CtpQueryGateway::~CtpQueryGateway() { stop(); }

bool CtpQueryGateway::start(std::string& err) {
    // Runtime-loaded SDK DLL (see ctp_loader.h) - keeps MinGW builds live-capable.
    if (!ctp::ctpLib().loadTrader(cfg_.ctpDllDir().c_str(), err)) return false;
    // Flow path: normalized (trailing separator - CTP concatenates the file
    // names without one) and wiped of stale session files from an earlier
    // trading day, which hang the first login of a new day.
    // Switch off with [general] clean_flow_on_start = false.
    const std::string flow = ctp::normalizeFlowPath(cfg_.queryFlowPath());
    if (cfg_.getBool("general", "clean_flow_on_start", true)) ctp::prepareFlowDir(flow);
    api_ = ctp::ctpLib().createTraderApi(flow.c_str(), err);
    if (!api_) return false;
    api_->RegisterSpi(this);
    api_->SubscribePrivateTopic(THOST_TERT_QUICK);
    api_->SubscribePublicTopic(THOST_TERT_QUICK);

    std::vector<std::string> fronts = cfg_.queryFronts();
    if (fronts.empty()) {
        err = "no query front configured ([query] front / fronts)";
        return false;
    }
    for (auto& f : fronts) api_->RegisterFront(const_cast<char*>(f.c_str()));
    ctp::apiInit(api_);
    return true;
}

void CtpQueryGateway::stop() {
    if (!api_) return;
    api_->Release();
    api_ = nullptr;
    loggedIn_.store(false);
}

bool CtpQueryGateway::waitLogin(int timeoutMs) {
    for (int waited = 0; waited < timeoutMs; waited += 20) {
        if (loggedIn_.load(std::memory_order_acquire)) return true;
        sleepMs(20);
    }
    return loggedIn_.load(std::memory_order_acquire);
}

void CtpQueryGateway::OnFrontConnected() {
    HFT_LOGI("query front connected");
    if (statusSink_) statusSink_("connected");
    if (!cfg_.appId().empty() && !cfg_.authCode().empty()) authenticate();
    else login();
}

void CtpQueryGateway::OnFrontDisconnected(int nReason) {
    loggedIn_.store(false);
    HFT_LOGW("query front disconnected, reason=%d", nReason);
    // Any in-flight queries are gone with the connection: drop the round.
    {
        std::lock_guard<std::mutex> lk(collectMtx_);
        tasks_.clear();
        taskIdx_ = 0;
        noted_ = 0;
    }
    collecting_.store(false, std::memory_order_release);
}

void CtpQueryGateway::authenticate() {
    CThostFtdcReqAuthenticateField req;
    std::memset(&req, 0, sizeof(req));
    copyStr(req.BrokerID, cfg_.brokerId().c_str());
    copyStr(req.UserID, cfg_.userId().c_str());
    copyStr(req.AppID, cfg_.appId().c_str());
    copyStr(req.AuthCode, cfg_.authCode().c_str());
    api_->ReqAuthenticate(&req, ++requestId_);
}

void CtpQueryGateway::OnRspAuthenticate(CThostFtdcRspAuthenticateField*,
                                        CThostFtdcRspInfoField* pRspInfo, int, bool) {
    if (isError(pRspInfo)) {
        lastError_ = "authenticate failed: " + describe(pRspInfo);
        HFT_LOGE("%s", lastError_.c_str());
        return;
    }
    login();
}

void CtpQueryGateway::login() {
    CThostFtdcReqUserLoginField req;
    std::memset(&req, 0, sizeof(req));
    copyStr(req.BrokerID, cfg_.brokerId().c_str());
    copyStr(req.UserID, cfg_.userId().c_str());
    copyStr(req.Password, cfg_.password().c_str());
    api_->ReqUserLogin(&req, ++requestId_);
}

void CtpQueryGateway::OnRspUserLogin(CThostFtdcRspUserLoginField* pRspUserLogin,
                                     CThostFtdcRspInfoField* pRspInfo, int, bool) {
    if (isError(pRspInfo)) {
        lastError_ = "query login failed: " + describe(pRspInfo);
        HFT_LOGE("%s", lastError_.c_str());
        return;
    }
    if (pRspUserLogin) copyStr(stagingAccount_.tradingDay, pRspUserLogin->TradingDay);
    loggedIn_.store(true, std::memory_order_release);
    loginNs_ = nowNs();
    if (statusSink_) statusSink_("logged in");
    HFT_LOGI("query session logged in");
#if defined(HFT_CTP_STD)
    // Account / position queries return nothing until the settlement is
    // confirmed on THIS session. Order matters: confirm first, query later.
    confirmSettlement();
#endif
}

#if defined(HFT_CTP_STD)
void CtpQueryGateway::confirmSettlement() {
    CThostFtdcSettlementInfoConfirmField req;
    std::memset(&req, 0, sizeof(req));
    copyStr(req.BrokerID, cfg_.brokerId().c_str());
    copyStr(req.InvestorID, cfg_.investorId().c_str());
    const int rc = api_->ReqSettlementInfoConfirm(&req, ++requestId_);
    if (rc != 0) HFT_LOGW("query: settlement confirm submit failed (rc=%d)", rc);
    else HFT_LOGI("query: settlement confirmation requested");
}

void CtpQueryGateway::OnRspSettlementInfoConfirm(
    CThostFtdcSettlementInfoConfirmField* pSettlementInfoConfirm,
    CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) {
    (void)pSettlementInfoConfirm; (void)nRequestID; (void)bIsLast;
    if (isError(pRspInfo)) {
        // "Already confirmed" is reported as an error by some counters; unlock
        // anyway, otherwise the snapshot stays empty for the whole session.
        HFT_LOGW("query: settlement confirm rejected: %s", describe(pRspInfo).c_str());
    } else {
        HFT_LOGI("query: settlement confirmed - account/position queries enabled");
    }
    settleConfirmed_.store(true, std::memory_order_release);
}
#endif

void CtpQueryGateway::refreshAll() {
    if (!api_ || !loggedIn_.load(std::memory_order_acquire)) return;
    const int64_t now = nowNs();

    // A round that never completes (the counter dropped one reply) used to
    // block every later round forever: collecting_ stayed true and the
    // snapshot kept its default values. Recover instead of waiting.
    if (collecting_.load(std::memory_order_acquire)) {
        if (roundStartNs_ != 0 && now - roundStartNs_ > 15000000000ll) {
            HFT_LOGW("query: previous round stalled >15 s (missing reply) - restarting");
            std::lock_guard<std::mutex> lk(collectMtx_);
            tasks_.clear();
            taskIdx_ = 0;
            noted_ = 0;
            collecting_.store(false, std::memory_order_release);
        } else {
            return;   // previous round still running
        }
    }

    // Give the settlement confirmation a few seconds to land; querying before
    // it is answered silently returns an empty account on SimNow.
    if (!settleConfirmed_.load(std::memory_order_acquire) && now - loginNs_ < 5000000000ll) {
        return;
    }
    if (!settleConfirmed_.load(std::memory_order_acquire) && !settleWarned_) {
        settleWarned_ = true;
        HFT_LOGW("query: settlement not confirmed after 5 s - querying anyway "
                 "(account/position replies may be empty)");
    }
    if (collecting_.exchange(true)) return;  // previous round still running

    // Queue the round. The queries are submitted one at a time by pump():
    // CTP refuses more than about one query submission per second, so a
    // burst of account+position+instrument would silently drop all but the
    // first (the position query!), leaving the snapshot without positions.
    std::vector<QryTask> tasks;
    tasks.push_back({0, ""});
    tasks.push_back({1, ""});
    for (const auto& inst : cfg_.instruments()) tasks.push_back({2, inst});
    {
        std::lock_guard<std::mutex> lk(collectMtx_);
        tasks_ = std::move(tasks);
        taskIdx_ = 0;
        noted_ = 0;
        roundStartNs_ = now;
    }
    pump(nowNs());   // submits the first task right away (last send was >= 3 s ago)
}

void CtpQueryGateway::pump(int64_t nowNs) {
    if (!api_ || !collecting_.load(std::memory_order_acquire)) return;
    QryTask t;
    {
        std::lock_guard<std::mutex> lk(collectMtx_);
        if (taskIdx_ >= tasks_.size()) return;
        // Spacing between query submissions. 1.05 s keeps us safely under the
        // "one query per second" counter rule, so a full round of three
        // queries takes ~2.2 s plus response time.
        if (lastSendNs_ != 0 && nowNs - lastSendNs_ < 1050000000ll) return;
        t = tasks_[taskIdx_];
        if (!sendTask(t)) {
            // Rejected locally (rate limit / queue full): retry this task on
            // the next pump, a full spacing interval from now.
            lastSendNs_ = nowNs;
            return;
        }
        ++taskIdx_;
        lastSendNs_ = nowNs;
    }
}

bool CtpQueryGateway::sendTask(const QryTask& t) {
    if (!api_ || !loggedIn_.load(std::memory_order_acquire)) return false;
    switch (t.type) {
        case 0: {
            CThostFtdcQryTradingAccountField q;
            std::memset(&q, 0, sizeof(q));
            copyStr(q.BrokerID, cfg_.brokerId().c_str());
            copyStr(q.InvestorID, cfg_.investorId().c_str());
            const int rc = api_->ReqQryTradingAccount(&q, ++requestId_);
            if (rc != 0) HFT_LOGW("account query submit rejected (rc=%d)", rc);
            return rc == 0;
        }
        case 1: {
            CThostFtdcQryInvestorPositionField q;
            std::memset(&q, 0, sizeof(q));
            copyStr(q.BrokerID, cfg_.brokerId().c_str());
            copyStr(q.InvestorID, cfg_.investorId().c_str());
            posStale_ = true;   // first record of this response clears the staging
            const int rc = api_->ReqQryInvestorPosition(&q, ++requestId_);
            if (rc != 0) HFT_LOGW("position query submit rejected (rc=%d)", rc);
            return rc == 0;
        }
        default: {
            CThostFtdcQryInstrumentField q;
            std::memset(&q, 0, sizeof(q));
            copyStr(q.InstrumentID, t.instrument.c_str());
            instrStale_ = true;
            const int rc = api_->ReqQryInstrument(&q, ++requestId_);
            if (rc != 0) HFT_LOGW("instrument query submit rejected (rc=%d)", rc);
            return rc == 0;
        }
    }
}

bool CtpQueryGateway::queryInstrument(const char* instrument) {
    // Ad-hoc single instrument query (explicit request from the strategy).
    QryTask t{2, instrument};
    return sendTask(t);
}

// One query of the current round has finished. The complete round publishes
// exactly once - with the position list included, which is the whole point.
void CtpQueryGateway::noteResponse() {
    bool doPublish = false;
    {
        std::lock_guard<std::mutex> lk(collectMtx_);
        if (collecting_.load(std::memory_order_acquire) &&
            ++noted_ >= static_cast<int>(tasks_.size())) {
            doPublish = true;
        }
    }
    if (doPublish) publish();
}

void CtpQueryGateway::OnRspQryTradingAccount(CThostFtdcTradingAccountField* p,
                                             CThostFtdcRspInfoField* pRspInfo, int, bool bIsLast) {
    if (isError(pRspInfo)) {
        HFT_LOGE("qry account error: %s", describe(pRspInfo).c_str());
        noteResponse();   // count the failed query so the round can complete
        return;
    }
    if (p) {
        stagingAccount_.preBalance = p->PreBalance;
        stagingAccount_.balance = p->Balance;
        stagingAccount_.available = p->Available;
        stagingAccount_.currMargin = p->CurrMargin;
        stagingAccount_.frozenMargin = p->FrozenMargin;
        stagingAccount_.commission = p->Commission;
        stagingAccount_.closeProfit = p->CloseProfit;
        stagingAccount_.positionProfit = p->PositionProfit;
        stagingAccount_.withdrawQuota = p->WithdrawQuota;
    }
    if (bIsLast) noteResponse();
}

void CtpQueryGateway::OnRspQryInvestorPosition(CThostFtdcInvestorPositionField* p,
                                               CThostFtdcRspInfoField* pRspInfo, int, bool bIsLast) {
    if (isError(pRspInfo)) {
        HFT_LOGE("qry position error: %s", describe(pRspInfo).c_str());
        noteResponse();   // count the failed query so the round can complete
        return;
    }
    if (posStale_) {
        stagingPositions_.clear();   // first record of this round's response
        posStale_ = false;
    }
    if (p && p->Position > 0) {
        PositionSnap snap;
        std::memset(&snap, 0, sizeof(snap));
        copyStr(snap.instrument, p->InstrumentID);
        snap.direction = p->PosiDirection;
        snap.positionDate = p->PositionDate;
        snap.ydPosition = p->YdPosition;
        snap.position = p->Position;
        snap.todayPosition = p->TodayPosition;
        snap.longFrozen = p->LongFrozen;
        snap.shortFrozen = p->ShortFrozen;
        snap.openVolume = p->OpenVolume;
        snap.closeVolume = p->CloseVolume;
        snap.positionCost = p->PositionCost;
        snap.openCost = p->OpenCost;
        snap.useMargin = p->UseMargin;
        snap.positionProfit = p->PositionProfit;
        snap.closeProfit = p->CloseProfit;
        snap.settlementPrice = p->SettlementPrice;
        stagingPositions_.push_back(snap);
    }
    if (bIsLast) noteResponse();
}

void CtpQueryGateway::OnRspQryInstrument(CThostFtdcInstrumentField* p,
                                         CThostFtdcRspInfoField* pRspInfo, int, bool bIsLast) {
    if (isError(pRspInfo)) {
        HFT_LOGE("qry instrument error: %s", describe(pRspInfo).c_str());
        noteResponse();   // count the failed query so the round can complete
        return;
    }
    if (instrStale_) {
        stagingInstruments_.clear();
        instrStale_ = false;
    }
    if (p) {
        InstrumentSnap snap;
        std::memset(&snap, 0, sizeof(snap));
        copyStr(snap.instrument, p->InstrumentID);
        copyStr(snap.exchangeID, p->ExchangeID);
        copyStr(snap.productID, p->ProductID);
        snap.volumeMultiple = p->VolumeMultiple;
        snap.priceTick = p->PriceTick;
        snap.longMarginRatio = p->LongMarginRatio;
        snap.shortMarginRatio = p->ShortMarginRatio;
        snap.isTrading = p->IsTrading;
        stagingInstruments_.push_back(snap);
    }
    if (bIsLast) {
        static bool logged = false;
        if (!logged && !stagingInstruments_.empty()) {
            logged = true;
            std::string ids;
            for (const auto& s : stagingInstruments_) {
                if (!ids.empty()) ids += " ";
                ids += s.instrument;
            }
            HFT_LOGI("instrument table (%zu): %s", stagingInstruments_.size(), ids.c_str());
        }
        noteResponse();
    }
}

void CtpQueryGateway::publish() {
    if (!snapshot_ || !collecting_.load(std::memory_order_acquire)) return;
    SnapshotWriter w(snapshot_);
    w.begin();
    w.account() = stagingAccount_;
    uint32_t n = static_cast<uint32_t>(std::min<size_t>(stagingPositions_.size(), kMaxPositions));
    for (uint32_t i = 0; i < n; ++i) w.positions()[i] = stagingPositions_[i];
    uint32_t m = static_cast<uint32_t>(std::min<size_t>(stagingInstruments_.size(), kMaxInstruments));
    for (uint32_t i = 0; i < m; ++i) w.instruments()[i] = stagingInstruments_[i];
    w.setCounts(n, m);
    w.setTradingDay(stagingAccount_.tradingDay);
    w.end();
    collecting_.store(false, std::memory_order_release);
    HFT_LOGD("snapshot published: positions=%u instruments=%u balance=%.2f available=%.2f", n, m,
             stagingAccount_.balance, stagingAccount_.available);
}

void CtpQueryGateway::OnRspError(CThostFtdcRspInfoField* pRspInfo, int, bool) {
    HFT_LOGE("query rsp error: %s", describe(pRspInfo).c_str());
    // The round is broken: drop it so the next refresh starts clean instead
    // of waiting forever for responses that will never come.
    {
        std::lock_guard<std::mutex> lk(collectMtx_);
        tasks_.clear();
        taskIdx_ = 0;
        noted_ = 0;
    }
    collecting_.store(false, std::memory_order_release);
}

} // namespace hft

#endif // HFT_ENABLE_CTP
