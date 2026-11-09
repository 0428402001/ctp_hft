// SPDX-License-Identifier: MIT
// ctp_query_gateway.h - Independent CTP session used only for queries.
//
// Keeping queries in their own process (and its own session) is what prevents a
// slow "query account" round trip from ever delaying an order.
#ifndef HFT_CTP_QUERY_GATEWAY_H
#define HFT_CTP_QUERY_GATEWAY_H

#include "hft/channel.h"
#include "hft/config.h"

#ifdef HFT_ENABLE_CTP

#include "ThostFtdcTraderApi.h"

#include <algorithm>
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace hft {

class CtpQueryGateway : public CThostFtdcTraderSpi {
public:
    using StatusSink = std::function<void(const char*)>;

    explicit CtpQueryGateway(const Config& cfg, SnapshotBlock* snapshot);
    ~CtpQueryGateway();

    bool start(std::string& err);
    void stop();
    bool loggedIn() const { return loggedIn_.load(std::memory_order_acquire); }
    bool waitLogin(int timeoutMs);

    // Kicks off one full refresh (account + positions + instruments).
    // The queries are NOT sent in a burst: CTP rejects more than about one
    // query submission per second (rc -2/-3), so a burst silently drops the
    // position query. refreshAll only queues the round; pump() sends the
    // queries one by one, spaced out.
    void refreshAll();
    // Sends the next queued query once enough time has passed since the last
    // one. Cheap; call it from the process' poll loop.
    void pump(int64_t nowNs);
    bool queryInstrument(const char* instrument);

    void setStatusSink(StatusSink sink) { statusSink_ = std::move(sink); }
    const std::string& lastError() const { return lastError_; }

    // ---- CThostFtdcTraderSpi -----------------------------------------------
    void OnFrontConnected() override;
    void OnFrontDisconnected(int nReason) override;
    void OnRspAuthenticate(CThostFtdcRspAuthenticateField* pField, CThostFtdcRspInfoField* pRspInfo,
                           int nRequestID, bool bIsLast) override;
    void OnRspUserLogin(CThostFtdcRspUserLoginField* pRspUserLogin,
                        CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) override;
    void OnRspQryTradingAccount(CThostFtdcTradingAccountField* pTradingAccount,
                                CThostFtdcRspInfoField* pRspInfo, int nRequestID,
                                bool bIsLast) override;
    void OnRspQryInvestorPosition(CThostFtdcInvestorPositionField* pInvestorPosition,
                                  CThostFtdcRspInfoField* pRspInfo, int nRequestID,
                                  bool bIsLast) override;
    void OnRspQryInstrument(CThostFtdcInstrumentField* pInstrument, CThostFtdcRspInfoField* pRspInfo,
                            int nRequestID, bool bIsLast) override;
    void OnRspError(CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) override;

private:
    // One refresh round, executed as a spaced-out sequence of queries.
    struct QryTask {
        int type = 0;              // 0 = account, 1 = position, 2 = instrument
        std::string instrument;
    };

    void authenticate();
    void login();
    // Publishes the staged results into the shared snapshot (seqlock write).
    void publish();
    // Called by every response handler on its final record: the snapshot is
    // published only when ALL requests of the current refresh round have
    // answered. Publishing on the first response (which used to happen) races
    // the position query - the account usually answers first, so the snapshot
    // was published with an empty position list and the broker side position
    // never reached the strategy.
    void noteResponse();
    // Issues one query request; false when the counter refused it (rate
    // limit) so pump() can retry later.
    bool sendTask(const QryTask& t);

    const Config& cfg_;
    SnapshotBlock* snapshot_;
    CThostFtdcTraderApi* api_ = nullptr;
    std::atomic<bool> loggedIn_{false};
    std::atomic<int> requestId_{0};

    // Staging area: results are accumulated here and published to the shared
    // snapshot in one seqlock-protected burst.
    AccountSnap stagingAccount_{};
    std::vector<PositionSnap> stagingPositions_;
    std::vector<InstrumentSnap> stagingInstruments_;
    std::atomic<bool> collecting_{false};
    std::vector<QryTask> tasks_;   // round work list (guarded by collectMtx_)
    size_t taskIdx_ = 0;           // next task to submit
    int noted_ = 0;                // responses completed so far this round
    int64_t lastSendNs_ = 0;       // last successful submission
    bool posStale_ = true;         // clear the position staging on first record
    bool instrStale_ = true;       // same for the instrument staging
    std::mutex collectMtx_;

    StatusSink statusSink_;
    std::string lastError_;
};

} // namespace hft

#endif // HFT_ENABLE_CTP
#endif // HFT_CTP_QUERY_GATEWAY_H
