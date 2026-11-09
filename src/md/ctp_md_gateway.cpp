// SPDX-License-Identifier: MIT
#include "ctp_md_gateway.h"

#include "common/ctp_compat.h"
#include "common/ctp_loader.h"

#ifdef HFT_ENABLE_CTP

#include "hft/logger.h"
#include "hft/time_util.h"

#include <cstring>

namespace hft {

namespace {

bool isError(CThostFtdcRspInfoField* pRspInfo) {
    return pRspInfo && pRspInfo->ErrorID != 0;
}

std::string describe(CThostFtdcRspInfoField* pRspInfo) {
    if (!pRspInfo) return "";
    char buf[256];
    std::snprintf(buf, sizeof(buf), "(%d) %s", pRspInfo->ErrorID, pRspInfo->ErrorMsg);
    return std::string(buf);
}

} // namespace

CtpMdGateway::CtpMdGateway(const Config& cfg) : cfg_(cfg) {}

CtpMdGateway::~CtpMdGateway() { stop(); }

bool CtpMdGateway::start(std::string& err) {
    // The SDK DLL is loaded at run time (see ctp_loader.h): the CTP import
    // libraries are MSVC-only, so this is how MinGW builds get live support.
    if (!ctp::ctpLib().loadMd(cfg_.ctpDllDir().c_str(), err)) return false;
    // Flow path: normalized (trailing separator - CTP concatenates the file
    // names without one) and wiped of stale session files from an earlier
    // trading day, which hang the first login of a new day.
    // Switch off with [general] clean_flow_on_start = false.
    const std::string flow = ctp::normalizeFlowPath(cfg_.mdFlowPath());
    if (cfg_.getBool("general", "clean_flow_on_start", true)) ctp::prepareFlowDir(flow);
    api_ = ctp::ctpLib().createMdApi(flow.c_str(), false, false, err);
    if (!api_) return false;
    api_->RegisterSpi(this);

    std::vector<std::string> fronts = cfg_.mdFronts();
    if (fronts.empty()) {
        err = "no market data front configured ([md] front / fronts)";
        return false;
    }
    for (auto& f : fronts) {
        api_->RegisterFront(const_cast<char*>(f.c_str()));
    }
    ctp::apiInit(api_);
    HFT_LOGI("md api initialised (%s), version=%s", ctp::flavourName(),
             ctp::ctpLib().mdVersion());
    return true;
}

void CtpMdGateway::stop() {
    if (!api_) return;
    // Join() returns once the API thread has finished.
    api_->Release();
    api_ = nullptr;
    loggedIn_.store(false);
}

bool CtpMdGateway::waitLogin(int timeoutMs) {
    for (int waited = 0; waited < timeoutMs; waited += 20) {
        if (loggedIn_.load(std::memory_order_acquire)) return true;
        sleepMs(20);
    }
    return loggedIn_.load(std::memory_order_acquire);
}

void CtpMdGateway::OnFrontConnected() {
    connected_.store(true);
    HFT_LOGI("market data front connected");
    if (statusSink_) statusSink_("connected");
    login();
}

void CtpMdGateway::OnFrontDisconnected(int nReason) {
    connected_.store(false);
    loggedIn_.store(false);
    HFT_LOGW("market data front disconnected, reason=%d", nReason);
    if (statusSink_) statusSink_("disconnected");
}

void CtpMdGateway::OnHeartBeatWarning(int nTimeLapse) {
    HFT_LOGW("market data heartbeat warning: %ds since last packet", nTimeLapse);
}

void CtpMdGateway::login() {
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

void CtpMdGateway::OnRspUserLogin(CThostFtdcRspUserLoginField* pRspUserLogin,
                                  CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) {
    if (isError(pRspInfo)) {
        lastError_ = "login failed: " + describe(pRspInfo);
        HFT_LOGE("%s", lastError_.c_str());
        return;
    }
    if (pRspUserLogin) tradingDay_ = pRspUserLogin->TradingDay;
    HFT_LOGI("market data login ok, trading day=%s", tradingDay_.c_str());
    loggedIn_.store(true, std::memory_order_release);
    if (statusSink_) statusSink_("logged in");
    subscribe();
}

void CtpMdGateway::subscribe() {
    std::vector<std::string> insts = cfg_.instruments();
    if (insts.empty()) return;
    std::vector<char*> ptrs;
    std::vector<std::string> storage = insts;
    for (auto& s : storage) ptrs.push_back(const_cast<char*>(s.c_str()));
    int rc = api_->SubscribeMarketData(ptrs.data(), static_cast<int>(ptrs.size()));
    if (rc != 0) {
        HFT_LOGE("SubscribeMarketData returned %d", rc);
    } else {
        HFT_LOGI("subscribed %zu instruments", insts.size());
    }
}

void CtpMdGateway::OnRspSubMarketData(CThostFtdcSpecificInstrumentField* pSpecificInstrument,
                                      CThostFtdcRspInfoField* pRspInfo, int nRequestID,
                                      bool bIsLast) {
    if (isError(pRspInfo)) {
        HFT_LOGE("subscribe response error: %s", describe(pRspInfo).c_str());
        return;
    }
    HFT_LOGI("subscribed: %s", pSpecificInstrument ? pSpecificInstrument->InstrumentID : "");
}

void CtpMdGateway::OnRspError(CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) {
    HFT_LOGE("md rsp error: %s", describe(pRspInfo).c_str());
}

TickMsg CtpMdGateway::convert(const CThostFtdcDepthMarketDataField& f) const {
    TickMsg t;
    std::memset(&t, 0, sizeof(t));
    t.magic = kMagicTick;
    copyStr(t.instrument, f.InstrumentID);
    copyStr(t.exchangeID, f.ExchangeID);
    copyStr(t.tradingDay, f.TradingDay);
    copyStr(t.updateTime, f.UpdateTime);
    t.updateMillisec = static_cast<uint32_t>(f.UpdateMillisec);
    t.lastPrice = f.LastPrice;
    t.openPrice = f.OpenPrice;
    t.highestPrice = f.HighestPrice;
    t.lowestPrice = f.LowestPrice;
    t.preClosePrice = f.PreClosePrice;
    t.preSettlementPrice = f.PreSettlementPrice;
    t.upperLimitPrice = f.UpperLimitPrice;
    t.lowerLimitPrice = f.LowerLimitPrice;
    t.averagePrice = f.AveragePrice;
    t.bidPrice1 = f.BidPrice1;
    t.askPrice1 = f.AskPrice1;
    t.bidVolume1 = f.BidVolume1;
    t.askVolume1 = f.AskVolume1;
    t.volume = static_cast<double>(f.Volume);
    t.turnover = f.Turnover;
    t.openInterest = f.OpenInterest;
    t.recvTimeNs = wallNowNs();
    return t;
}

void CtpMdGateway::OnRtnDepthMarketData(CThostFtdcDepthMarketDataField* pDepthMarketData) {
    if (!pDepthMarketData || !tickSink_) return;
    TickMsg t = convert(*pDepthMarketData);
    t.publishTimeNs = wallNowNs();
    tickSink_(t);
}

} // namespace hft

#endif // HFT_ENABLE_CTP
