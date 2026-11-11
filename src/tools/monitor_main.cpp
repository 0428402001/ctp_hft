// SPDX-License-Identifier: MIT
// monitor_main.cpp - Read-only observer of the shared memory bus.
//
// Shows process heartbeats, ring buffer depths and the account snapshot that
// the query process publishes. Attach it at any time without disturbing the
// running system.
#include "hft/channel.h"
#include "hft/config.h"
#include "hft/process_app.h"
#include "hft/logger.h"
#include "hft/time_util.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

void hr(const char* title) {
    std::printf("\n== %s ", title);
    for (size_t i = 0; i + strlen(title) < 70; ++i) std::printf("=");
    std::printf("\n");
}

} // namespace

int main(int argc, char** argv) {
    std::string configPath = (argc > 1) ? argv[1] : "config/config.ini";
    int refreshMs = (argc > 2) ? std::atoi(argv[2]) : 1000;

    hft::Config cfg;
    std::string err;
    if (!cfg.load(configPath, err)) {
        std::fprintf(stderr, "config: %s (continuing with defaults)\n", err.c_str());
    }

    hft::Channels ch;
    const bool create = false;  // observer only, never creates anything
    if (!ch.openControl(create, err) || !ch.openTicks(create, err) ||
        !ch.openCommands(create, err) || !ch.openExecs(create, err) ||
        !ch.openExecsToQuery(create, err) || !ch.openQryReq(create, err) ||
        !ch.openQryRsp(create, err) || !ch.openSnapshot(create, err) ||
        !ch.openMarket(create, err)) {
        std::fprintf(stderr, "cannot attach to shared memory: %s\n", err.c_str());
        std::fprintf(stderr, "is the system running? start it with start_sim.bat\n");
        return 1;
    }

    std::printf("ctp_hft monitor - attached to the shared bus (config: %s)\n",
                configPath.c_str());
    std::printf("press Ctrl+C to quit\n");

    hft::ProcessApp::installConsoleHandler();

    while (!hft::ProcessApp::globalStopFlag().load(std::memory_order_acquire)) {
#if defined(HFT_WINDOWS)
        std::system("cls");
#else
        std::printf("\033[2J\033[H");
#endif
        hft::ControlBlock* ctl = ch.control();

        std::printf("ctp_hft monitor                       %s\n", hft::localTimeString().c_str());
        std::printf("mode: %s   shutdown=%u  trading=%s\n", cfg.mode().c_str(),
                    ctl->shutdown.load(), ctl->tradingEnabled.load() ? "ON" : "OFF");
        std::printf("data-integrity faults: 0x%X (latched until cold restart)\n",
                    ctl->dataFaults.load(std::memory_order_acquire));

        hr("processes");
        std::printf("%-9s %-8s %-14s %-10s\n", "ROLE", "PID", "LAST BEAT", "BEATS");
        for (uint32_t r = 0; r < 4; ++r) {
            const hft::Heartbeat& hb = ctl->hb[r];
            const int64_t age = hft::nowNs() - hb.lastBeatNs.load();
            const bool alive = hb.alive.load() != 0 && (hb.lastBeatNs.load() > 0) && age < 3000000000ll;
            std::printf("%-9s %-8u %-14s %-10llu %s\n", hft::roleName(static_cast<hft::Role>(r)),
                        hb.pid.load(), alive ? "alive" : "DOWN",
                        static_cast<unsigned long long>(hb.beatCount.load()),
                        alive ? "" : "<-- not running");
        }

        hr("ring buffers");
        std::printf("%-14s %10s %12s %12s %10s\n", "CHANNEL", "DEPTH", "PUSHED", "POPPED",
                    "DROPPED");
        std::printf("%-14s %10llu %12llu %12llu %10llu\n", "ticks",
                    static_cast<unsigned long long>(ch.ticks()->size()),
                    static_cast<unsigned long long>(ch.ticks()->pushed()),
                    static_cast<unsigned long long>(ch.ticks()->popped()),
                    static_cast<unsigned long long>(ch.ticks()->dropped()));
        std::printf("%-14s %10llu %12llu %12llu %10llu\n", "commands",
                    static_cast<unsigned long long>(ch.commands()->size()),
                    static_cast<unsigned long long>(ch.commands()->pushed()),
                    static_cast<unsigned long long>(ch.commands()->popped()),
                    static_cast<unsigned long long>(ch.commands()->dropped()));
        std::printf("%-14s %10llu %12llu %12llu %10llu\n", "execs",
                    static_cast<unsigned long long>(ch.execs()->size()),
                    static_cast<unsigned long long>(ch.execs()->pushed()),
                    static_cast<unsigned long long>(ch.execs()->popped()),
                    static_cast<unsigned long long>(ch.execs()->dropped()));
        std::printf("%-14s %10llu %12llu %12llu %10llu\n", "execs->query",
                    static_cast<unsigned long long>(ch.execsToQuery()->size()),
                    static_cast<unsigned long long>(ch.execsToQuery()->pushed()),
                    static_cast<unsigned long long>(ch.execsToQuery()->popped()),
                    static_cast<unsigned long long>(ch.execsToQuery()->dropped()));
        std::printf("%-14s %10llu %12llu %12llu %10llu\n", "qry req",
                    static_cast<unsigned long long>(ch.qryReq()->size()),
                    static_cast<unsigned long long>(ch.qryReq()->pushed()),
                    static_cast<unsigned long long>(ch.qryReq()->popped()),
                    static_cast<unsigned long long>(ch.qryReq()->dropped()));
        std::printf("%-14s %10llu %12llu %12llu %10llu\n", "qry rsp",
                    static_cast<unsigned long long>(ch.qryRsp()->size()),
                    static_cast<unsigned long long>(ch.qryRsp()->pushed()),
                    static_cast<unsigned long long>(ch.qryRsp()->popped()),
                    static_cast<unsigned long long>(ch.qryRsp()->dropped()));

        hr("account (from query process)");
        hft::SnapshotReader rd(ch.snapshot());
        if (rd.read()) {
            const hft::AccountSnap& a = rd.account();
            std::printf("balance=%12.2f  available=%12.2f  margin=%12.2f\n", a.balance, a.available,
                        a.currMargin);
            std::printf("realized=%11.2f  unrealized=%10.2f  fee=%14.2f\n", a.closeProfit,
                        a.positionProfit, a.commission);
        } else {
            std::printf("(snapshot not available yet)\n");
        }

        hr("positions");
        std::printf("%-14s %6s %8s %8s %14s\n", "INSTRUMENT", "DIR", "QTY", "TODAY", "AVG COST");
        for (uint32_t i = 0; i < rd.positionCount(); ++i) {
            const hft::PositionSnap& p = rd.positions()[i];
            if (p.position == 0) continue;
            std::printf("%-14s %6s %8d %8d %14.2f\n", p.instrument,
                        p.direction == '2' ? "LONG" : "SHORT", p.position, p.todayPosition,
                        p.position > 0 ? p.positionCost / p.position : 0.0);
        }

        hr("market");
        hft::MarketSnapshotBlock* mkt = ch.market();
        const uint32_t n = mkt->count.load(std::memory_order_acquire);
        std::printf("%-14s %12s %12s %12s\n", "INSTRUMENT", "LAST", "BID", "ASK");
        for (uint32_t i = 0; i < n && i < hft::kMaxMarketInstruments; ++i) {
            hft::MarketTickSnap s{};
            if (!mkt->read(i, s)) continue;
            std::printf("%-14s %12.2f %12.2f %12.2f\n", s.instrument, s.lastPrice, s.bidPrice1,
                        s.askPrice1);
        }

        std::printf("\nrefresh every %d ms - Ctrl+C to quit\n", refreshMs);
        std::fflush(stdout);
        hft::sleepMs(static_cast<uint32_t>(refreshMs));
    }
    return 0;
}
