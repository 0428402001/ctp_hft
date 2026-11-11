// SPDX-License-Identifier: MIT
// query_main.cpp - Query process.
//
// Owns the *shared truth* about money and positions:
//   * live mode  -> periodic ReqQryTradingAccount / ReqQryInvestorPosition /
//                   ReqQryInstrument, published into the shared snapshot;
//   * sim mode   -> consumes the execution mirror from the trading process and
//                   keeps a virtual book, published into the same snapshot.
//
// Because it is a separate process, a slow query can never delay an order.
#include "hft/process_app.h"
#include "hft/channel.h"
#include "hft/logger.h"
#include "hft/time_util.h"

#include "ctp_query_gateway.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

namespace hft {

class QueryProcess : public ProcessApp {
public:
    QueryProcess() : ProcessApp(Role::Query, "query") {}

protected:
    bool attachChannels(std::string& err) override {
        const bool create = true;
        return channels().openControl(create, err) && channels().openQryReq(create, err) &&
               channels().openQryRsp(create, err) && channels().openSnapshot(create, err) &&
               channels().openExecsToQuery(create, err) && channels().openMarket(create, err);
    }

    bool onStart(std::string& err) override {
        initInstruments();

        if (simulation()) {
            balance_ = config().getDouble("sim", "initial_balance", 1000000.0);
            feeRate_ = config().getDouble("sim", "fee_rate", 0.0001);
            marginRate_ = config().getDouble("sim", "margin_rate", 0.10);
            available_ = balance_;
            control()->queryLoggedIn.store(1, std::memory_order_release);
            publishSnapshot();
            HFT_LOGI("simulation book ready, balance=%.2f", balance_);
            return true;
        }

#ifdef HFT_ENABLE_CTP
        ctp_.reset(new CtpQueryGateway(config(), channels().snapshot()));
        ctp_->setStatusSink([this](const char* s) {
            HFT_LOGI("query status: %s", s);
            if (std::strcmp(s, "logged in") == 0) {
                control()->queryLoggedIn.store(1, std::memory_order_release);
            }
        });
        if (!ctp_->start(err)) return false;
        if (!ctp_->waitLogin(15000)) {
            err = "query login timeout: " + ctp_->lastError();
            return false;
        }
        ctp_->refreshAll();
        return true;
#else
        err = "live mode requested but this build has no CTP support (HFT_ENABLE_CTP=OFF)";
        return false;
#endif
    }

    bool onPoll() override {
        const int64_t now = nowNs();
        bool any = false;

#ifdef HFT_ENABLE_CTP
        // Advance the spaced-out query sequence of the current refresh round
        // (one submission per ~1 s - CTP rejects bursts).
        if (ctp_) ctp_->pump(now);
#endif

        // 1) execution mirror (used by the simulated book; harmless in live mode)
        ExecReport rep;
        while (channels().execsToQuery()->pop(rep)) {
            ++execSeen_;
            if (simulation() && rep.kind == ExecType::TradeRtn) {
                applyTrade(rep);
                any = true;
            }
        }

        // 2) explicit requests from the strategy
        QueryRequest req;
        while (channels().qryReq()->pop(req)) {
            handleRequest(req);
            any = true;
        }

        // 3) periodic refresh of the shared snapshot
        if (now - lastRefreshNs_ >= static_cast<int64_t>(config().queryIntervalMs()) * 1000000ll) {
            lastRefreshNs_ = now;
            if (simulation()) {
                markToMarket();
                publishSnapshot();
            } else {
#ifdef HFT_ENABLE_CTP
                if (ctp_) ctp_->refreshAll();
#endif
            }
            any = true;
        }

        // 4) periodic account summary
        if (now - lastStatNs_ >= 5000000000ll) {
            lastStatNs_ = now;
            // In live mode balance_/available_/book_ are never filled - the
            // broker's answer goes straight into the shared snapshot (see
            // CtpQueryGateway::publish). Printing the members reported a fake
            // "balance=1000000.00 pos=0" account for hours on 2026-11-11 while
            // the snapshot held the real 20 996 125. Read back what was
            // published, so the log and the snapshot can never disagree.
            double bal = balance_, avail = available_, margin = marginUsed_;
            double realized = realized_, fee = feePaid_;
            size_t pos = book_.size();
            if (!simulation()) {
                SnapshotReader rd(channels().snapshot());
                if (rd.read()) {
                    bal = rd.account().balance;
                    avail = rd.account().available;
                    margin = rd.account().currMargin;
                    realized = rd.account().closeProfit;
                    fee = rd.account().commission;
                    pos = rd.positionCount();
                }
            }
            HFT_LOGI("account balance=%.2f available=%.2f margin=%.2f realized=%.2f fee=%.2f pos=%zu execs=%llu",
                     bal, avail, margin, realized, fee, pos,
                     static_cast<unsigned long long>(execSeen_));
        }
        return any;
    }

    void onStop() override {
#ifdef HFT_ENABLE_CTP
        if (ctp_) ctp_->stop();
#endif
        control()->queryLoggedIn.store(0, std::memory_order_release);
    }

    int idleSleepUs() const override { return 1000; }

    // Snapshot refresh runs every query interval_ms (default 3 s); waking
    // once a second keeps it punctual while the exec-mirror / request
    // doorbells handle the urgent traffic in between.
    int64_t maxWaitNs() const override { return 1000000000ll; }

    // Wake instantly on exec-mirror items or explicit query requests; the
    // periodic refresh continues via the wait timeout.
    void collectWaitHandles(std::vector<void*>& handles) override {
        handles.push_back(channels().execsToQueryEvt().handle());
        handles.push_back(channels().qryReqEvt().handle());
    }

    bool prepareWait() override {
        bool ready = channels().prepareExecsToQueryWait();
        ready = channels().prepareQryReqWait() && ready;
        return ready;
    }

private:
    // ---- instrument table --------------------------------------------------
    void initInstruments() {
        instruments_.clear();
        const double tick = config().getDouble("sim", "price_tick", 1.0);
        const int mult = config().getInt("sim", "volume_multiple", 10);
        for (const auto& name : config().instruments()) {
            InstrumentSnap s;
            std::memset(&s, 0, sizeof(s));
            copyStr(s.instrument, name.c_str());
            copyStr(s.exchangeID, "SIM");
            copyStr(s.productID, name.substr(0, std::min<size_t>(2, name.size())).c_str());
            s.volumeMultiple = config().getInt("sim." + name, "volume_multiple", mult);
            s.priceTick = config().getDouble("sim." + name, "price_tick", tick);
            s.longMarginRatio = config().getDouble("sim", "margin_rate", 0.10);
            s.shortMarginRatio = s.longMarginRatio;
            s.isTrading = 1;
            instruments_.push_back(s);
        }
    }

    const InstrumentSnap* findInstrument(const char* name) const {
        for (const auto& s : instruments_) {
            if (std::strncmp(s.instrument, name, kInstrumentLen) == 0) return &s;
        }
        return nullptr;
    }

    // ---- simulated bookkeeping --------------------------------------------
    struct BookPos {
        char instrument[kInstrumentLen] = {0};
        char direction = '2';
        int position = 0;
        int todayPosition = 0;
        double avgCost = 0.0;
        double useMargin = 0.0;
        double profit = 0.0;
    };

    BookPos& getPos(const char* instrument, char direction) {
        for (auto& p : book_) {
            if (p.direction == direction &&
                std::strncmp(p.instrument, instrument, kInstrumentLen) == 0) {
                return p;
            }
        }
        BookPos p;
        copyStr(p.instrument, instrument);
        p.direction = direction;
        book_.push_back(p);
        return book_.back();
    }

    void applyTrade(const ExecReport& r) {
        if (r.tradeVolume <= 0) return;
        const InstrumentSnap* inst = findInstrument(r.instrument);
        const int mult = inst ? (inst->volumeMultiple > 0 ? inst->volumeMultiple : 1) : 10;
        const double rate = inst ? (inst->longMarginRatio > 0 ? inst->longMarginRatio : marginRate_)
                                 : marginRate_;
        const double price = r.tradePrice > 0 ? r.tradePrice : r.price;
        const int vol = r.tradeVolume;
        const bool isBuy = (r.direction == static_cast<char>(Dir::Buy));
        const bool isOpen = (r.offsetFlag == static_cast<char>(Offset::Open));

        const double fee = price * vol * mult * feeRate_;
        feePaid_ += fee;
        balance_ -= fee;

        if (isOpen) {
            BookPos& p = getPos(r.instrument, isBuy ? '2' : '3');
            const int newPos = p.position + vol;
            p.avgCost = (p.avgCost * p.position + price * vol) / static_cast<double>(newPos);
            p.position = newPos;
            p.todayPosition += vol;
            p.useMargin = p.position * p.avgCost * mult * rate;
        } else {
            // Closing: reduce the opposite side.
            BookPos& p = getPos(r.instrument, isBuy ? '3' : '2');
            const int closed = std::min(vol, p.position);
            if (closed > 0) {
                const double pnl = isBuy ? (p.avgCost - price) * closed * mult
                                         : (price - p.avgCost) * closed * mult;
                realized_ += pnl;
                balance_ += pnl;
                p.position -= closed;
                p.todayPosition = std::max(0, p.todayPosition - closed);
                if (p.position <= 0) {
                    p.position = 0;
                    p.todayPosition = 0;
                    p.avgCost = 0.0;
                    p.useMargin = 0.0;
                } else {
                    p.useMargin = p.position * p.avgCost * mult * rate;
                }
            }
        }

        recomputeMargin();
        HFT_LOGI("fill %s %c %s vol=%d @ %.2f (balance=%.2f realized=%.2f)",
                 r.instrument, r.direction, isOpen ? "OPEN" : "CLOSE", vol, price, balance_,
                 realized_);
    }

    void recomputeMargin() {
        marginUsed_ = 0.0;
        for (const auto& p : book_) marginUsed_ += p.useMargin;
        available_ = balance_ - marginUsed_;
        // Drop flat positions so the published snapshot stays tidy.
        book_.erase(std::remove_if(book_.begin(), book_.end(),
                                   [](const BookPos& p) { return p.position == 0; }),
                    book_.end());
    }

    void markToMarket() {
        MarketSnapshotBlock* mkt = channels().market();
        if (!mkt) return;
        const uint32_t n = mkt->count.load(std::memory_order_acquire);
        unrealized_ = 0.0;
        for (auto& p : book_) {
            double last = 0.0;
            for (uint32_t i = 0; i < n && i < kMaxMarketInstruments; ++i) {
                MarketTickSnap snap{};
                if (mkt->read(i, snap) &&
                    std::strncmp(snap.instrument, p.instrument, kInstrumentLen) == 0) {
                    last = snap.lastPrice;
                    break;
                }
            }
            if (last <= 0) continue;
            const InstrumentSnap* inst = findInstrument(p.instrument);
            const int mult = inst ? (inst->volumeMultiple > 0 ? inst->volumeMultiple : 1) : 10;
            p.profit = (p.direction == '2') ? (last - p.avgCost) * p.position * mult
                                            : (p.avgCost - last) * p.position * mult;
            unrealized_ += p.profit;
        }
    }

    void publishSnapshot() {
        SnapshotBlock* blk = channels().snapshot();
        if (!blk) return;
        SnapshotWriter w(blk);
        w.begin();
        AccountSnap& a = w.account();
        std::memset(&a, 0, sizeof(a));
        a.preBalance = balance_;
        a.balance = balance_;
        a.available = available_;
        a.currMargin = marginUsed_;
        a.commission = feePaid_;
        a.closeProfit = realized_;
        a.positionProfit = unrealized_;
        a.withdrawQuota = available_;
        copyStr(a.tradingDay, localDateString().c_str());

        uint32_t n = 0;
        for (const auto& p : book_) {
            if (n >= kMaxPositions) break;
            PositionSnap& s = w.positions()[n++];
            std::memset(&s, 0, sizeof(s));
            copyStr(s.instrument, p.instrument);
            s.direction = p.direction;
            s.positionDate = '1';
            s.position = p.position;
            s.todayPosition = p.todayPosition;
            s.positionCost = p.avgCost * p.position;
            s.openCost = p.avgCost * p.position;
            s.useMargin = p.useMargin;
            s.positionProfit = p.profit;
        }
        uint32_t m = 0;
        for (const auto& inst : instruments_) {
            if (m >= kMaxInstruments) break;
            w.instruments()[m++] = inst;
        }
        w.setCounts(n, m);
        w.setTradingDay(a.tradingDay);
        w.end();
    }

    // ---- explicit requests -------------------------------------------------
    void handleRequest(const QueryRequest& req) {
        if (req.magic != kMagicQryReq) return;
        switch (req.type) {
        case QueryType::Account: {
            QueryResponse r;
            std::memset(&r, 0, sizeof(r));
            r.magic = kMagicQryRsp;
            r.reqId = req.reqId;
            r.type = req.type;
            r.isLast = 1;
            SnapshotReader rd(channels().snapshot());
            if (rd.read()) r.account = rd.account();
            r.rtnTimeNs = nowNs();
            pushResponse(r);
            break;
        }
        case QueryType::Position: {
            SnapshotReader rd(channels().snapshot());
            if (!rd.read()) break;
            const PositionSnap* pos = rd.positions();
            for (size_t i = 0; i < rd.positionCount(); ++i) {
                QueryResponse r;
                std::memset(&r, 0, sizeof(r));
                r.magic = kMagicQryRsp;
                r.reqId = req.reqId;
                r.type = req.type;
                r.isLast = (i + 1 == rd.positionCount()) ? 1 : 0;
                r.position = pos[i];
                r.rtnTimeNs = nowNs();
                pushResponse(r);
            }
            break;
        }
        case QueryType::Instrument: {
            for (const auto& inst : instruments_) {
                if (req.instrument[0] != '\0' &&
                    std::strncmp(inst.instrument, req.instrument, kInstrumentLen) != 0) {
                    continue;
                }
                QueryResponse r;
                std::memset(&r, 0, sizeof(r));
                r.magic = kMagicQryRsp;
                r.reqId = req.reqId;
                r.type = req.type;
                r.isLast = 1;
                r.instrument = inst;
                r.rtnTimeNs = nowNs();
                pushResponse(r);
            }
            break;
        }
        case QueryType::All:
            if (simulation()) {
                markToMarket();
                publishSnapshot();
            }
            break;
        default:
            break;
        }
    }

    void pushResponse(const QueryResponse& r) {
        if (!channels().pushQryRsp(r)) HFT_LOGW("query response ring full");
    }

    std::vector<InstrumentSnap> instruments_;
    std::vector<BookPos> book_;
    double balance_ = 1000000.0;
    double available_ = 1000000.0;
    double marginUsed_ = 0.0;
    double realized_ = 0.0;
    double unrealized_ = 0.0;
    double feePaid_ = 0.0;
    double feeRate_ = 0.0001;
    double marginRate_ = 0.10;
    uint64_t execSeen_ = 0;
    int64_t lastRefreshNs_ = 0;
    int64_t lastStatNs_ = 0;
#ifdef HFT_ENABLE_CTP
    std::unique_ptr<CtpQueryGateway> ctp_;
#endif
};

} // namespace hft

int main(int argc, char** argv) {
    hft::QueryProcess app;
    if (!app.init(argc, argv)) return 1;
    return app.run();
}
