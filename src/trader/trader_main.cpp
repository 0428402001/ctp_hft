// SPDX-License-Identifier: MIT
// trader_main.cpp - Trading process.
//
//   strategy --order commands--> TRADER --> exchange (or simulator)
//   TRADER --execution reports--> strategy  and  --mirror--> query process
//
// It owns no position state: every fill is published both to the strategy (so
// it can update its own book) and to the query process (so the shared snapshot
// stays correct).
#include "hft/process_app.h"
#include "hft/logger.h"
#include "hft/sim_matcher.h"
#include "hft/time_util.h"

#include "ctp_trader_gateway.h"

#include <cstring>
#include <memory>
#include <vector>

namespace hft {

class TraderProcess : public ProcessApp {
public:
    TraderProcess() : ProcessApp(Role::Trader, "trader") {}

protected:
    bool attachChannels(std::string& err) override {
        const bool create = true;
        return channels().openControl(create, err) && channels().openCommands(create, err) &&
               channels().openExecs(create, err) && channels().openExecsToQuery(create, err) &&
               channels().openMarket(create, err);
    }

    bool onStart(std::string& err) override {
        (void)err;
        // The matcher report batch is reused every poll: reserve once so the
        // sim hot path never grows a vector while matching.
        reports_.reserve(32);
        if (simulation()) {
            matcher_.setLatencyMs(config().getInt("sim", "order_latency_ms", 0));
            matcher_.setFillPartial(config().getBool("sim", "partial_fills", true));
            control()->traderLoggedIn.store(1, std::memory_order_release);
            HFT_LOGI("simulation matcher ready");
            return true;
        }
#ifdef HFT_ENABLE_CTP
        ctp_.reset(new CtpTraderGateway(config()));
        ctp_->setExecSink([this](const ExecReport& r) { publishExec(r); });
        ctp_->setStatusSink([this](const char* s) {
            HFT_LOGI("trader status: %s", s);
            if (std::strcmp(s, "logged in") == 0) {
                control()->traderLoggedIn.store(1, std::memory_order_release);
            }
        });
        if (!ctp_->start(err)) return false;
        if (!ctp_->waitLogin(15000)) {
            err = "trader login timeout: " + ctp_->lastError();
            return false;
        }
#if defined(HFT_CTP_STD)
        // SimNow & friends refuse orders until the settlement statement of
        // the day is confirmed; wait briefly instead of letting every first
        // order bounce with error 42 ("settlement result not confirmed").
        if (!ctp_->waitSettlement(60000)) {
            HFT_LOGW("settlement not confirmed after 60 s - orders may be rejected");
        }
#endif
        return true;
#else
        err = "live mode requested but this build has no CTP support (HFT_ENABLE_CTP=OFF)";
        return false;
#endif
    }

    bool onPoll() override {
        bool any = false;

        // 1) incoming orders from the strategy
        OrderCommand cmd;
        while (channels().commands()->pop(cmd)) {
            handleCommand(cmd);
            any = true;
        }

        // 2) in simulation, resting orders may fill as prices move
        if (simulation()) {
            reports_.clear();
            matcher_.match(channels().market(), reports_);
            for (const auto& r : reports_) publishExec(r);
            if (!reports_.empty()) any = true;
        }

        printStats();
        return any;
    }

    void onStop() override {
#ifdef HFT_ENABLE_CTP
        if (ctp_) ctp_->stop();
#endif
        control()->traderLoggedIn.store(0, std::memory_order_release);
    }

    int idleSleepUs() const override { return 500; }

    // Live: only the order-command doorbell matters; the 5 s stats print is
    // the longest the loop may sleep. Sim: the local matcher must track the
    // simulated market, so cap the block at 2 ms (sim mode only).
    int64_t maxWaitNs() const override {
        return simulation() ? 2000000ll : 5000000000ll;
    }

    // Wake instantly when the strategy sends an order command.
    void collectWaitHandles(std::vector<void*>& handles) override {
        handles.push_back(channels().commandsEvt().handle());
    }

    bool prepareWait() override { return channels().prepareCommandsWait(); }

private:
    void handleCommand(const OrderCommand& cmd) {
        if (cmd.magic != kMagicCmd) {
            HFT_LOGW("dropping malformed command (magic=%08X)", cmd.magic);
            return;
        }

        switch (cmd.type) {
        case CmdType::RiskHalt:
            control()->tradingEnabled.store(0, std::memory_order_release);
            HFT_LOGW("trading disabled by strategy");
            return;
        case CmdType::RiskResume:
            control()->tradingEnabled.store(1, std::memory_order_release);
            HFT_LOGI("trading enabled by strategy");
            return;
        default:
            break;
        }

        // Master switch: never open new risk when trading is halted.
        if (cmd.type == CmdType::Insert && cmd.offsetFlag == static_cast<char>(Offset::Open) &&
            control()->tradingEnabled.load(std::memory_order_acquire) == 0) {
            ExecReport r;
            std::memset(&r, 0, sizeof(r));
            r.magic = kMagicExec;
            r.kind = ExecType::InsertRsp;
            r.cmdId = cmd.cmdId;
            copyStr(r.instrument, cmd.instrument);
            r.direction = cmd.direction;
            r.offsetFlag = cmd.offsetFlag;
            r.price = cmd.price;
            r.volume = cmd.volume;
            r.errorId = -9;
            copyStr(r.errorMsg, "trading disabled (risk halt)");
            r.status = kUnknown;
            r.strategyTag = cmd.strategyTag;
            r.rtnTimeNs = nowNs();
            publishExec(r);
            return;
        }

        ++cmdCount_;
        if (simulation()) {
            reports_.clear();
            if (cmd.type == CmdType::Insert) {
                matcher_.submit(cmd, channels().market(), reports_);
            } else if (cmd.type == CmdType::Cancel || cmd.type == CmdType::CancelAll) {
                matcher_.cancel(cmd, reports_);
            }
            for (const auto& r : reports_) publishExec(r);
            return;
        }

#ifdef HFT_ENABLE_CTP
        if (!ctp_) return;
        if (cmd.type == CmdType::Insert) {
            ctp_->sendOrder(cmd);
        } else if (cmd.type == CmdType::Cancel || cmd.type == CmdType::CancelAll) {
            ctp_->cancelOrder(cmd);
        }
#endif
    }

    void publishExec(const ExecReport& r) {
        if (!channels().pushExec(r)) {
            ++execDropped_;
            control()->dataFaults.fetch_or(kFaultExecQueueOverflow, std::memory_order_release);
            control()->tradingEnabled.store(0, std::memory_order_release);
            if (execDropped_ == 1 || (execDropped_ % 500) == 0) {
                HFT_LOGE("exec ring full, dropped %llu reports; new entries disabled",
                         static_cast<unsigned long long>(execDropped_));
            }
        }
        // Mirror to the query process (best effort - it is a bookkeeping feed).
        if (!channels().pushExecToQuery(r)) {
            control()->dataFaults.fetch_or(kFaultExecMirrorOverflow, std::memory_order_release);
            control()->tradingEnabled.store(0, std::memory_order_release);
            HFT_LOGE("execution mirror ring full; query bookkeeping is incomplete");
        }
        ++execCount_;
    }

    void printStats() {
        const int64_t now = nowNs();
        if (lastStatNs_ == 0) {
            lastStatNs_ = now;
            return;
        }
        if (now - lastStatNs_ < 5000000000ll) return;
        HFT_LOGI("commands=%llu execs=%llu dropped=%llu pending=%zu",
                 static_cast<unsigned long long>(cmdCount_),
                 static_cast<unsigned long long>(execCount_),
                 static_cast<unsigned long long>(execDropped_),
                 simulation() ? matcher_.pendingCount() : 0);
        lastStatNs_ = now;
    }

    SimMatcher matcher_;
    std::vector<ExecReport> reports_;
#ifdef HFT_ENABLE_CTP
    std::unique_ptr<CtpTraderGateway> ctp_;
#endif
    uint64_t cmdCount_ = 0;
    uint64_t execCount_ = 0;
    uint64_t execDropped_ = 0;
    int64_t lastStatNs_ = 0;
};

} // namespace hft

int main(int argc, char** argv) {
    hft::TraderProcess app;
    if (!app.init(argc, argv)) return 1;
    return app.run();
}
