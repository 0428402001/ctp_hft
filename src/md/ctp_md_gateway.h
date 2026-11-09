// SPDX-License-Identifier: MIT
// ctp_md_gateway.h - CTP market-data gateway (compiled only with HFT_ENABLE_CTP).
#ifndef HFT_CTP_MD_GATEWAY_H
#define HFT_CTP_MD_GATEWAY_H

#include "hft/config.h"
#include "hft/protocol.h"

#ifdef HFT_ENABLE_CTP

#include "ThostFtdcMdApi.h"

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace hft {

// Wraps CThostFtdcMdApi. The CTP callbacks run on the API's own thread, which
// is the *only* thread that ever touches the tick ring buffer - that is exactly
// what a SPSC queue needs.
class CtpMdGateway : public CThostFtdcMdSpi {
public:
    using TickSink = std::function<void(const TickMsg&)>;
    using StatusSink = std::function<void(const char*)>;

    explicit CtpMdGateway(const Config& cfg);
    ~CtpMdGateway();

    bool start(std::string& err);
    void stop();
    bool loggedIn() const { return loggedIn_.load(std::memory_order_acquire); }
    bool waitLogin(int timeoutMs);

    void setTickSink(TickSink sink) { tickSink_ = std::move(sink); }
    void setStatusSink(StatusSink sink) { statusSink_ = std::move(sink); }

    const std::string& lastError() const { return lastError_; }

    // ---- CThostFtdcMdSpi ---------------------------------------------------
    void OnFrontConnected() override;
    void OnFrontDisconnected(int nReason) override;
    void OnHeartBeatWarning(int nTimeLapse) override;
    void OnRspUserLogin(CThostFtdcRspUserLoginField* pRspUserLogin,
                        CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) override;
    void OnRspSubMarketData(CThostFtdcSpecificInstrumentField* pSpecificInstrument,
                            CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) override;
    void OnRspError(CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) override;
    void OnRtnDepthMarketData(CThostFtdcDepthMarketDataField* pDepthMarketData) override;

private:
    void login();
    void subscribe();
    TickMsg convert(const CThostFtdcDepthMarketDataField& f) const;

    const Config& cfg_;
    CThostFtdcMdApi* api_ = nullptr;
    std::atomic<bool> loggedIn_{false};
    std::atomic<bool> connected_{false};
    std::atomic<int> requestId_{0};
    TickSink tickSink_;
    StatusSink statusSink_;
    std::string lastError_;
    std::string tradingDay_;
};

} // namespace hft

#endif // HFT_ENABLE_CTP
#endif // HFT_CTP_MD_GATEWAY_H
