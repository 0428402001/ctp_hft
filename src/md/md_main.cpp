// SPDX-License-Identifier: MIT
// md_main.cpp - Market data process.
//
//   CTP MdApi (or the simulator)  ->  shared tick ring buffer  ->  strategy
//
// It also maintains the shared market snapshot (last price / bid / ask per
// instrument) that the trading process uses for matching and that strategies can
// peek at without waiting for the next tick.
#include "hft/process_app.h"
#include "hft/logger.h"
#include "hft/sim_market.h"
#include "hft/time_util.h"

#include "ctp_md_gateway.h"

#include <cstring>
#include <memory>

namespace hft {

class MdProcess : public ProcessApp {
public:
    MdProcess() : ProcessApp(Role::Md, "md") {}

protected:
    bool attachChannels(std::string& err) override {
        // First process up creates the regions; openOrCreate is idempotent.
        const bool create = true;
        return channels().openControl(create, err) && channels().openTicks(create, err) &&
               channels().openMarket(create, err);
    }

    bool onStart(std::string& err) override {
        std::vector<std::string> insts = config().instruments();
        if (insts.empty()) {
            err = "no instruments configured ([general] instruments)";
            return false;
        }
        HFT_LOGI("instruments: %zu", insts.size());
        for (const auto& s : insts) HFT_LOGI("  %s", s.c_str());

        if (simulation()) {
            if (!sim_.init(config(), err)) return false;
            sim_.publishSnapshot(channels().market());
            control()->mdLoggedIn.store(1, std::memory_order_release);
            HFT_LOGI("simulation market started with %zu instruments", sim_.size());
            return true;
        }

#ifdef HFT_ENABLE_CTP
        ctp_.reset(new CtpMdGateway(config()));
        ctp_->setTickSink([this](const TickMsg& t) { publish(t); });
        ctp_->setStatusSink([this](const char* s) {
            HFT_LOGI("md status: %s", s);
            if (std::strcmp(s, "logged in") == 0) {
                control()->mdLoggedIn.store(1, std::memory_order_release);
            }
        });
        if (!ctp_->start(err)) return false;
        if (!ctp_->waitLogin(15000)) {
            err = "md login timeout: " + ctp_->lastError();
            return false;
        }
        return true;
#else
        err = "live mode requested but this build has no CTP support (HFT_ENABLE_CTP=OFF)";
        return false;
#endif
    }

    bool onPoll() override {
        const int64_t now = nowNs();

        if (simulation()) {
            TickMsg t;
            bool any = false;
            // Drain whatever is due this iteration (usually 0 or 1 tick).
            while (sim_.next(t)) {
                publish(t);
                any = true;
            }
            if (any) sim_.publishSnapshot(channels().market());
            printStats(now);
            return any;
        }

        // Live mode: the CTP thread pushes ticks by itself, the poll loop only
        // publishes statistics and watches the shutdown flag.
        printStats(now);
        return false;
    }

    void onStop() override {
#ifdef HFT_ENABLE_CTP
        if (ctp_) ctp_->stop();
#endif
        control()->mdLoggedIn.store(0, std::memory_order_release);
    }

    int idleSleepUs() const override { return simulation() ? 500 : 5000; }

private:
    void publish(const TickMsg& t) {
        if (!channels().pushTick(t)) {
            ++dropped_;
            if (dropped_ == 1 || (dropped_ % 1000) == 0) {
                HFT_LOGW("tick ring full, dropped %llu ticks in total",
                         static_cast<unsigned long long>(dropped_));
            }
        } else {
            ++tickCount_;
        }
        updateMarketSnapshot(t);
    }

    void updateMarketSnapshot(const TickMsg& t) {
        MarketSnapshotBlock* blk = channels().market();
        if (!blk) return;
        uint32_t n = blk->count.load(std::memory_order_acquire);
        for (uint32_t i = 0; i < n && i < kMaxMarketInstruments; ++i) {
            if (std::strncmp(blk->items[i].instrument, t.instrument, kInstrumentLen) == 0) {
                blk->items[i].lastPrice = t.lastPrice;
                blk->items[i].bidPrice1 = t.bidPrice1;
                blk->items[i].askPrice1 = t.askPrice1;
                blk->items[i].openPrice = t.openPrice;
                blk->items[i].highestPrice = t.highestPrice;
                blk->items[i].lowestPrice = t.lowestPrice;
                blk->items[i].bidVolume1 = t.bidVolume1;
                blk->items[i].askVolume1 = t.askVolume1;
                blk->items[i].tickTimeNs = t.publishTimeNs;
                blk->items[i].recvTimeNs = t.recvTimeNs;
                return;
            }
        }
        if (n < kMaxMarketInstruments) {
            MarketTickSnap& s = blk->items[n];
            std::memset(&s, 0, sizeof(s));
            copyStr(s.instrument, t.instrument);
            s.lastPrice = t.lastPrice;
            s.bidPrice1 = t.bidPrice1;
            s.askPrice1 = t.askPrice1;
            s.openPrice = t.openPrice;
            s.highestPrice = t.highestPrice;
            s.lowestPrice = t.lowestPrice;
            s.bidVolume1 = t.bidVolume1;
            s.askVolume1 = t.askVolume1;
            s.tickTimeNs = t.publishTimeNs;
            s.recvTimeNs = t.recvTimeNs;
            blk->count.store(n + 1, std::memory_order_release);
        }
    }

    void printStats(int64_t now) {
        if (lastStatNs_ == 0) {
            lastStatNs_ = now;
            return;
        }
        if (now - lastStatNs_ < 5000000000ll) return;  // every 5 s
        TickChannel* q = channels().ticks();
        double secs = static_cast<double>(now - lastStatNs_) / 1e9;
        HFT_LOGI("ticks=%llu dropped=%llu queue=%llu (%.1f ticks/s)",
                 static_cast<unsigned long long>(tickCount_),
                 static_cast<unsigned long long>(dropped_),
                 static_cast<unsigned long long>(q->size()),
                 static_cast<double>(tickCount_) / secs);
        lastStatNs_ = now;
        tickCount_ = 0;
    }

    SimMarket sim_;
#ifdef HFT_ENABLE_CTP
    std::unique_ptr<CtpMdGateway> ctp_;
#endif
    uint64_t tickCount_ = 0;
    uint64_t dropped_ = 0;
    int64_t lastStatNs_ = 0;
};

} // namespace hft

int main(int argc, char** argv) {
    hft::MdProcess app;
    if (!app.init(argc, argv)) return 1;
    return app.run();
}
