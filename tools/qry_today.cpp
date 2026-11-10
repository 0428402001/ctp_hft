// SPDX-License-Identifier: MIT
// qry_today.cpp - one-shot CTP query: account, positions, today's trades, orders.
//
// Why a separate tool: query_proc only refreshes account/position into shared
// memory (that is all the strategy needs at run time). Post-mortem analysis
// needs the two things it never asks for - ReqQryTrade (成交明细) and
// ReqQryOrder (报单) - plus the instrument meta (PriceTick / VolumeMultiple)
// without which a PnL expressed in price points cannot be turned into ticks.
//
// It is a throw-away session: log in, fire the queries one at a time (CTP
// rejects more than ~1 query/second), print + dump CSV, exit. It never
// subscribes to anything nor sends an order.
//
// Usage:
//   qry_today.exe --config config\config_simnow.ini --out log\qry
//   qry_today.exe --config config\config_simnow724.ini --out log\qry --front tcp://182.254.243.31:40001
#include "hft/config.h"
#include "hft/platform.h"

#include "common/ctp_compat.h"
#include "common/ctp_loader.h"

#include "ThostFtdcTraderApi.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

template <std::size_t N>
void setStr(char (&dst)[N], const std::string& src) {
    std::memset(dst, 0, N);
    if (!src.empty()) std::strncpy(dst, src.c_str(), N - 1);
}

std::string str(const char* p) { return p ? std::string(p) : std::string(); }

const char* dirName(char d) { return d == THOST_FTDC_D_Buy ? "BUY" : (d == THOST_FTDC_D_Sell ? "SELL" : "?"); }

const char* offsetName(char o) {
    switch (o) {
        case THOST_FTDC_OF_Open: return "OPEN";
        case THOST_FTDC_OF_Close: return "CLOSE";
        case THOST_FTDC_OF_CloseToday: return "CLOSETODAY";
        case THOST_FTDC_OF_CloseYesterday: return "CLOSEYD";
        default: return "?";
    }
}

const char* statusName(char s) {
    switch (s) {
        case THOST_FTDC_OST_AllTraded: return "ALLTRADED";
        case THOST_FTDC_OST_PartTradedQueueing: return "PART_QUEUE";
        case THOST_FTDC_OST_PartTradedNotQueueing: return "PART_NOQUEUE";
        case THOST_FTDC_OST_NoTradeQueueing: return "QUEUEING";
        case THOST_FTDC_OST_NoTradeNotQueueing: return "NOQUEUE";
        case THOST_FTDC_OST_Canceled: return "CANCELED";
        case THOST_FTDC_OST_Touched: return "TOUCHED";
        default: return "OTHER";
    }
}

struct AccountRow {
    std::string tradingDay;
    double preBalance = 0, balance = 0, available = 0, currMargin = 0, commission = 0;
    double closeProfit = 0, positionProfit = 0, deposit = 0, withdraw = 0;
    bool valid = false;
};

struct PosRow {
    std::string inst;
    char dir = 0;
    int volume = 0, ydVolume = 0, tdVolume = 0;
    double avgPrice = 0, useMargin = 0, positionProfit = 0, closeProfit = 0, openCost = 0;
};

struct TradeRow {
    std::string inst, tradeId, tradingDay, tradeDate, tradeTime, orderRef, sysId;
    char dir = 0, offset = 0;
    double price = 0;
    int volume = 0;
};

struct OrderRow {
    std::string inst, orderRef, sysId, insertTime, statusMsg;
    char dir = 0, offset = 0, status = 0;
    double limitPrice = 0;
    int volOrig = 0, volTraded = 0;
};

struct InstMeta {
    std::string inst;
    double priceTick = 0;
    int volumeMultiple = 1;
    double marginRatio = 0;
};

class Qry : public CThostFtdcTraderSpi {
public:
    Qry(const hft::Config& cfg, std::string flow, std::vector<std::string> fronts)
        : cfg_(cfg), flow_(std::move(flow)), fronts_(std::move(fronts)) {}

    bool start(std::string& err) {
        if (!hft::ctp::ctpLib().loadTrader(cfg_.ctpDllDir().c_str(), err)) return false;
        api_ = hft::ctp::ctpLib().createTraderApi(hft::ctp::normalizeFlowPath(flow_).c_str(), err);
        if (!api_) return false;
        api_->RegisterSpi(this);
        api_->SubscribePrivateTopic(THOST_TERT_QUICK);
        api_->SubscribePublicTopic(THOST_TERT_QUICK);
        for (auto& f : fronts_) api_->RegisterFront(const_cast<char*>(f.c_str()));
        hft::ctp::apiInit(api_);
        return true;
    }

    void stop() {
        if (api_) {
            api_->Release();
            api_ = nullptr;
        }
    }

    bool waitFlag(std::atomic<int>& flag, int want, int timeoutMs) {
        for (int i = 0; i < timeoutMs / 20; ++i) {
            if (flag.load(std::memory_order_acquire) == want) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return false;
    }

    // ---- queries -----------------------------------------------------------
    bool qryAccount(int timeoutMs) { return runStep(stepAccount_, timeoutMs, [&] { return sendAccount(); }); }
    bool qryPosition(int timeoutMs) { return runStep(stepPosition_, timeoutMs, [&] { return sendPosition(); }); }
    bool qryTrade(int timeoutMs) { return runStep(stepTrade_, timeoutMs, [&] { return sendTrade(); }); }
    bool qryOrder(int timeoutMs) { return runStep(stepOrder_, timeoutMs, [&] { return sendOrder(); }); }
    bool qryInstrument(const std::string& inst, int timeoutMs) {
        inst_ = inst;
        return runStep(stepInstrument_, timeoutMs, [&] { return sendInstrument(inst); });
    }

    // ---- results -----------------------------------------------------------
    AccountRow account;
    std::vector<PosRow> positions;
    std::vector<TradeRow> trades;
    std::vector<OrderRow> orders;
    std::map<std::string, InstMeta> instMeta;
    std::string loginTradingDay, lastError;

    // ---- CThostFtdcTraderSpi ----------------------------------------------
    void OnFrontConnected() override {
        connected_.store(true, std::memory_order_release);
        std::fprintf(stderr, "[qry] front connected\n");
        if (!cfg_.appId().empty() && !cfg_.authCode().empty()) {
            CThostFtdcReqAuthenticateField req;
            std::memset(&req, 0, sizeof(req));
            setStr(req.BrokerID, cfg_.brokerId());
            setStr(req.UserID, cfg_.userId());
            setStr(req.AppID, cfg_.appId());
            setStr(req.AuthCode, cfg_.authCode());
            api_->ReqAuthenticate(&req, ++reqId_);
        } else {
            doLogin();
        }
    }

    void OnFrontDisconnected(int reason) override {
        connected_.store(false, std::memory_order_release);
        std::fprintf(stderr, "[qry] front disconnected reason=%d\n", reason);
    }

    void OnRspAuthenticate(CThostFtdcRspAuthenticateField*, CThostFtdcRspInfoField* info, int,
                           bool) override {
        if (info && info->ErrorID) {
            lastError = std::string("authenticate: ") + info->ErrorMsg;
            authFailed_.store(true);
            return;
        }
        doLogin();
    }

    void OnRspUserLogin(CThostFtdcRspUserLoginField* f, CThostFtdcRspInfoField* info, int,
                        bool) override {
        if (info && info->ErrorID) {
            lastError = std::string("login: ") + info->ErrorMsg;
            loginFailed_.store(true);
            return;
        }
        loggedIn_.store(true, std::memory_order_release);
        if (f) loginTradingDay = str(f->TradingDay);
        std::fprintf(stderr, "[qry] logged in, trading day %s\n", loginTradingDay.c_str());
    }

    void OnRspQryTradingAccount(CThostFtdcTradingAccountField* f, CThostFtdcRspInfoField* info,
                                int, bool last) override {
        if (f) {
            account.tradingDay = str(f->TradingDay);
            account.balance = f->Balance;
            account.available = f->Available;
            account.preBalance = f->PreBalance;
            account.currMargin = f->CurrMargin;
            account.commission = f->Commission;
            account.closeProfit = f->CloseProfit;
            account.positionProfit = f->PositionProfit;
            account.deposit = f->Deposit;
            account.withdraw = f->Withdraw;
            account.valid = true;
        }
        if (last) stepAccount_.store(doneOk(info), std::memory_order_release);
    }

    void OnRspQryInvestorPosition(CThostFtdcInvestorPositionField* f, CThostFtdcRspInfoField* info,
                                  int, bool last) override {
        if (f) {
            PosRow p;
            p.inst = str(f->InstrumentID);
            p.dir = f->PosiDirection;
            p.volume = f->Position;
            p.ydVolume = f->YdPosition;
            p.tdVolume = f->TodayPosition;
            p.avgPrice = f->PositionCost / (f->Position ? f->Position : 1);
            p.useMargin = f->UseMargin;
            p.positionProfit = f->PositionProfit;
            p.closeProfit = f->CloseProfit;
            p.openCost = f->OpenCost;
            if (p.volume != 0) positions.push_back(p);
        }
        if (last) stepPosition_.store(doneOk(info), std::memory_order_release);
    }

    void OnRspQryTrade(CThostFtdcTradeField* f, CThostFtdcRspInfoField* info, int, bool last) override {
        if (f) {
            TradeRow t;
            t.inst = str(f->InstrumentID);
            t.tradeId = str(f->TradeID);
            t.dir = f->Direction;
            t.offset = f->OffsetFlag;
            t.price = f->Price;
            t.volume = f->Volume;
            t.tradingDay = str(f->TradingDay);
            t.tradeDate = str(f->TradeDate);
            t.tradeTime = str(f->TradeTime);
            t.orderRef = str(f->OrderRef);
            t.sysId = str(f->OrderSysID);
            trades.push_back(t);
        }
        if (last) stepTrade_.store(doneOk(info), std::memory_order_release);
    }

    void OnRspQryOrder(CThostFtdcOrderField* f, CThostFtdcRspInfoField* info, int, bool last) override {
        if (f) {
            OrderRow o;
            o.inst = str(f->InstrumentID);
            o.orderRef = str(f->OrderRef);
            o.sysId = str(f->OrderSysID);
            o.dir = f->Direction;
            o.offset = f->CombOffsetFlag[0];
            o.status = f->OrderStatus;
            o.limitPrice = f->LimitPrice;
            o.volOrig = f->VolumeTotalOriginal;
            o.volTraded = f->VolumeTraded;
            o.insertTime = str(f->InsertTime);
            o.statusMsg = str(f->StatusMsg);
            orders.push_back(o);
        }
        if (last) stepOrder_.store(doneOk(info), std::memory_order_release);
    }

    void OnRspQryInstrument(CThostFtdcInstrumentField* f, CThostFtdcRspInfoField* info, int,
                            bool last) override {
        if (f) {
            InstMeta m;
            m.inst = str(f->InstrumentID);
            m.priceTick = f->PriceTick;
            m.volumeMultiple = f->VolumeMultiple;
            m.marginRatio = f->LongMarginRatio;
            instMeta[m.inst] = m;
        }
        if (last) stepInstrument_.store(doneOk(info), std::memory_order_release);
    }

    void OnRspError(CThostFtdcRspInfoField* info, int, bool) override {
        if (info && info->ErrorID)
            std::fprintf(stderr, "[qry] rsp error (%d) %s\n", info->ErrorID, info->ErrorMsg);
    }

    std::atomic<int>& connectedFlag() { return connected_; }
    std::atomic<int>& loggedInFlag() { return loggedIn_; }
    std::atomic<int>& authFailedFlag() { return authFailed_; }
    std::atomic<int>& loginFailedFlag() { return loginFailed_; }

private:
    static int doneOk(CThostFtdcRspInfoField* info) { return (info && info->ErrorID) ? -1 : 1; }

    bool runStep(std::atomic<int>& flag, int timeoutMs, const std::function<bool()>& send) {
        for (int attempt = 0; attempt < 3; ++attempt) {
            flag.store(0);
            if (send()) {
                for (int i = 0; i < timeoutMs / 20; ++i) {
                    if (flag.load(std::memory_order_acquire) == 1) return true;
                    if (flag.load(std::memory_order_acquire) == -1) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1200));  // respect 1 qry/s
        }
        return false;
    }

    void doLogin() {
        CThostFtdcReqUserLoginField req;
        std::memset(&req, 0, sizeof(req));
        setStr(req.BrokerID, cfg_.brokerId());
        setStr(req.UserID, cfg_.userId());
        setStr(req.Password, cfg_.password());
        api_->ReqUserLogin(&req, ++reqId_);
    }

    bool sendAccount() {
        CThostFtdcQryTradingAccountField q;
        std::memset(&q, 0, sizeof(q));
        setStr(q.BrokerID, cfg_.brokerId());
        setStr(q.InvestorID, cfg_.investorId());
        return api_->ReqQryTradingAccount(&q, ++reqId_) == 0;
    }

    bool sendPosition() {
        CThostFtdcQryInvestorPositionField q;
        std::memset(&q, 0, sizeof(q));
        setStr(q.BrokerID, cfg_.brokerId());
        setStr(q.InvestorID, cfg_.investorId());
        return api_->ReqQryInvestorPosition(&q, ++reqId_) == 0;
    }

    bool sendTrade() {
        CThostFtdcQryTradeField q;
        std::memset(&q, 0, sizeof(q));
        setStr(q.BrokerID, cfg_.brokerId());
        setStr(q.InvestorID, cfg_.investorId());
        return api_->ReqQryTrade(&q, ++reqId_) == 0;
    }

    bool sendOrder() {
        CThostFtdcQryOrderField q;
        std::memset(&q, 0, sizeof(q));
        setStr(q.BrokerID, cfg_.brokerId());
        setStr(q.InvestorID, cfg_.investorId());
        return api_->ReqQryOrder(&q, ++reqId_) == 0;
    }

    bool sendInstrument(const std::string& inst) {
        CThostFtdcQryInstrumentField q;
        std::memset(&q, 0, sizeof(q));
        setStr(q.InstrumentID, inst);
        return api_->ReqQryInstrument(&q, ++reqId_) == 0;
    }

    const hft::Config& cfg_;
    std::string flow_, inst_;
    std::vector<std::string> fronts_;
    CThostFtdcTraderApi* api_ = nullptr;
    int reqId_ = 0;

    std::atomic<int> connected_{0}, loggedIn_{0}, authFailed_{0}, loginFailed_{0};
    std::atomic<int> stepAccount_{0}, stepPosition_{0}, stepTrade_{0}, stepOrder_{0},
        stepInstrument_{0};
};

std::string fmt(const char* f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    return buf;
}

struct Leg {
    double price;
    int volume;
    std::string time;
};

// FIFO round-trip reconstruction: a long opens with BUY+OPEN and is closed by
// SELL+CLOSE(±today), shorts mirror that. Trades arrive ordered by time from CTP.
struct RoundTrip {
    std::string inst, dir, openTime, closeTime;
    double openPrice = 0, closePrice = 0;
    int volume = 0;
    double pnlPoints = 0;
};

std::vector<RoundTrip> buildRounds(const std::vector<TradeRow>& trades) {
    std::vector<RoundTrip> out;
    std::map<std::string, std::deque<Leg>> longs, shorts;  // key = instrument
    for (const auto& t : trades) {
        const bool open = (t.offset == THOST_FTDC_OF_Open);
        const bool buy = (t.dir == THOST_FTDC_D_Buy);
        if (open) {
            (buy ? longs : shorts)[t.inst].push_back({t.price, t.volume, t.tradeTime});
            continue;
        }
        // closing: a SELL closes longs, a BUY closes shorts
        auto& book = (buy ? shorts : longs)[t.inst];
        int left = t.volume;
        while (left > 0 && !book.empty()) {
            Leg& leg = book.front();
            const int n = std::min(left, leg.volume);
            RoundTrip rt;
            rt.inst = t.inst;
            rt.dir = buy ? "LONG" : "SHORT";  // the position being closed
            rt.openTime = leg.time;
            rt.closeTime = t.tradeTime;
            rt.openPrice = leg.price;
            rt.closePrice = t.price;
            rt.volume = n;
            rt.pnlPoints = (buy ? leg.price - t.price : t.price - leg.price) * n;
            out.push_back(rt);
            leg.volume -= n;
            left -= n;
            if (leg.volume == 0) book.pop_front();
        }
    }
    return out;
}

} // namespace

int main(int argc, char** argv) {
    std::string cfgPath = "config/config_simnow.ini";
    std::string outDir = "log/qry";
    std::string frontOverride;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--config" && i + 1 < argc) cfgPath = argv[++i];
        else if (a == "--out" && i + 1 < argc) outDir = argv[++i];
        else if (a == "--front" && i + 1 < argc) frontOverride = argv[++i];
    }

    hft::Config cfg;
    std::string err;
    if (!cfg.load(cfgPath, err)) {
        std::fprintf(stderr, "config: %s\n", err.c_str());
        return 2;
    }
    std::vector<std::string> fronts;
    if (!frontOverride.empty()) fronts.push_back(frontOverride);
    else fronts = cfg.queryFronts();
    if (fronts.empty()) {
        std::fprintf(stderr, "no query front in %s\n", cfgPath.c_str());
        return 2;
    }

    Qry spi(cfg, cfg.queryFlowPath(), fronts);
    if (!spi.start(err)) {
        std::fprintf(stderr, "start: %s\n", err.c_str());
        return 2;
    }
    if (!spi.waitFlag(spi.connectedFlag(), 1, 15000)) {
        std::fprintf(stderr, "connect timeout\n");
        spi.stop();
        return 3;
    }
    if (!spi.waitFlag(spi.loggedInFlag(), 1, 20000)) {
        std::fprintf(stderr, "login failed %s %s\n", spi.lastError.c_str(),
                     spi.authFailedFlag().load() ? "(auth)" : "");
        spi.stop();
        return 3;
    }

    bool okAcct = spi.qryAccount(12000);
    bool okPos = spi.qryPosition(12000);
    bool okTrd = spi.qryTrade(20000);
    bool okOrd = spi.qryOrder(20000);

    // instrument meta for whatever actually traded / is held
    std::vector<std::string> insts;
    for (const auto& t : spi.trades) insts.push_back(t.inst);
    for (const auto& p : spi.positions) insts.push_back(p.inst);
    std::sort(insts.begin(), insts.end());
    insts.erase(std::unique(insts.begin(), insts.end()), insts.end());
    for (const auto& i : insts) spi.qryInstrument(i, 15000);

    std::sort(spi.trades.begin(), spi.trades.end(), [](const TradeRow& a, const TradeRow& b) {
        if (a.tradeTime != b.tradeTime) return a.tradeTime < b.tradeTime;
        return a.tradeId < b.tradeId;
    });
    spi.stop();

    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);
    const std::string stamp = spi.loginTradingDay.empty() ? "unknown" : spi.loginTradingDay;

    // ---- CSV dumps ---------------------------------------------------------
    {
        std::ofstream f(outDir + "/trades_" + stamp + ".csv");
        f << "trading_day,trade_date,trade_time,instrument,direction,offset,price,volume,trade_id,order_ref,sys_id\n";
        for (const auto& t : spi.trades)
            f << t.tradingDay << ',' << t.tradeDate << ',' << t.tradeTime << ',' << t.inst << ','
              << dirName(t.dir) << ',' << offsetName(t.offset) << ',' << t.price << ',' << t.volume
              << ',' << t.tradeId << ',' << t.orderRef << ',' << t.sysId << '\n';
    }
    {
        std::ofstream f(outDir + "/positions_" + stamp + ".csv");
        f << "instrument,posi_direction,volume,yd_volume,td_volume,avg_price,use_margin,position_profit,close_profit\n";
        for (const auto& p : spi.positions)
            f << p.inst << ',' << (p.dir == THOST_FTDC_PD_Long ? "LONG" : (p.dir == THOST_FTDC_PD_Short ? "SHORT" : "NET"))
              << ',' << p.volume << ',' << p.ydVolume << ',' << p.tdVolume << ',' << p.avgPrice << ','
              << p.useMargin << ',' << p.positionProfit << ',' << p.closeProfit << '\n';
    }
    {
        std::ofstream f(outDir + "/orders_" + stamp + ".csv");
        f << "instrument,order_ref,sys_id,direction,offset,status,limit_price,vol_orig,vol_traded,insert_time,status_msg\n";
        for (const auto& o : spi.orders)
            f << o.inst << ',' << o.orderRef << ',' << o.sysId << ',' << dirName(o.dir) << ','
              << offsetName(o.offset) << ',' << statusName(o.status) << ',' << o.limitPrice << ','
              << o.volOrig << ',' << o.volTraded << ',' << o.insertTime << ',' << o.statusMsg << '\n';
    }

    // ---- console + markdown summary ---------------------------------------
    std::ostringstream md;
    auto line = [&](const std::string& s) {
        std::printf("%s\n", s.c_str());
        md << s << '\n';
    };

    line(fmt("# 查询结果  交易日 %s  (%s)", stamp.c_str(), cfgPath.c_str()));
    line("");
    if (spi.account.valid) {
        const auto& a = spi.account;
        line("## 资金");
        line(fmt("- 上日结存 %.2f | 动态权益 %.2f | 可用 %.2f | 占用保证金 %.2f", a.preBalance,
                 a.balance, a.available, a.currMargin));
        line(fmt("- 平仓盈亏 %.2f | 持仓盈亏 %.2f | 手续费 %.2f | 出入金 +%.2f/-%.2f", a.closeProfit,
                 a.positionProfit, a.commission, a.deposit, a.withdraw));
        line(fmt("- 账户口径净变动 (balance-pre) = %.2f", a.balance - a.preBalance));
    } else {
        line("## 资金  [查询失败]");
    }

    line("");
    line(fmt("## 成交 %zu 笔 | 报单 %zu 笔 | 持仓 %zu 行", spi.trades.size(), spi.orders.size(),
             spi.positions.size()));

    if (!spi.trades.empty()) {
        line("");
        line("| 时间 | 合约 | 方向 | 开平 | 价格 | 手数 |");
        line("|---|---|---|---|---|---|");
        for (const auto& t : spi.trades)
            line(fmt("| %s | %s | %s | %s | %.0f | %d |", t.tradeTime.c_str(), t.inst.c_str(),
                     dirName(t.dir), offsetName(t.offset), t.price, t.volume));
    }

    // ---- FIFO round trips --------------------------------------------------
    const auto rounds = buildRounds(spi.trades);
    std::map<std::string, std::pair<double, int>> byInst;  // inst -> (points, volume)
    double totalPoints = 0;
    int winN = 0, lossN = 0, flatN = 0;
    int openVol = 0, closeVol = 0;
    for (const auto& t : spi.trades)
        (t.offset == THOST_FTDC_OF_Open ? openVol : closeVol) += t.volume;

    line("");
    line(fmt("## 回合 (FIFO)  开仓 %d 手 / 平仓 %d 手 / 完整回合 %zu", openVol, closeVol, rounds.size()));
    if (!rounds.empty()) {
        line("");
        line("| 开仓时间 | 平仓时间 | 合约 | 方向 | 手 | 开价 | 平价 | 点数 |");
        line("|---|---|---|---|---|---|---|---|");
        for (const auto& r : rounds) {
            const auto& m = spi.instMeta.count(r.inst) ? spi.instMeta[r.inst] : InstMeta{};
            const double ticks = m.priceTick > 0 ? r.pnlPoints / m.priceTick : 0;
            line(fmt("| %s | %s | %s | %s | %d | %.0f | %.0f | %+.1f (%.1f tick) |",
                     r.openTime.c_str(), r.closeTime.c_str(), r.inst.c_str(), r.dir.c_str(), r.volume,
                     r.openPrice, r.closePrice, r.pnlPoints, ticks));
            byInst[r.inst].first += ticks;
            byInst[r.inst].second += r.volume;
            totalPoints += ticks;
            if (ticks > 0.01) ++winN;
            else if (ticks < -0.01) ++lossN;
            else ++flatN;
        }
        line("");
        line(fmt("- 胜/负/平 = %d / %d / %d，胜率 %.0f%%", winN, lossN, flatN,
                 100.0 * winN / std::max(1, winN + lossN + flatN)));
        for (const auto& kv : byInst) {
            const auto& m = spi.instMeta.count(kv.first) ? spi.instMeta[kv.first] : InstMeta{};
            const double yen = kv.second.first * m.priceTick * m.volumeMultiple;
            line(fmt("- %s: %+.1f tick，%d 手，约 %+.0f 元 (tick=%.4g, mult=%d)", kv.first.c_str(),
                     kv.second.first, kv.second.second, yen, m.priceTick, m.volumeMultiple));
        }
        line(fmt("- 合计 %+.1f tick（未扣手续费）", totalPoints));
    }

    if (!spi.orders.empty()) {
        std::map<std::string, int> byStatus;
        for (const auto& o : spi.orders) byStatus[statusName(o.status)]++;
        line("");
        line("## 报单状态分布");
        for (const auto& kv : byStatus) line(fmt("- %s: %d", kv.first.c_str(), kv.second));
    }

    {
        std::ofstream f(outDir + "/summary_" + stamp + ".md");
        f << md.str();
    }
    std::printf("\nCSV/MD 已写入 %s\n", outDir.c_str());
    return (okAcct && okTrd) ? 0 : 1;
}
