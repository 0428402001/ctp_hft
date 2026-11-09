// Read-only window into the shared-memory snapshot.
//
// Needed because query_proc only ever printed balance/available, so when an
// overnight position had to be explained there was no way to see PreBalance,
// CloseProfit (realised) or Commission (fees) - the three numbers that answer
// "how much did the account actually lose".  Opening the region read-only is
// completely safe: no login, no order, no reconnect, no risk of tripping
// SimNow's front throttle.
//
//   build:  g++ -std=c++17 -O2 -Iinclude -Isrc -static -o build/bin_simnow/account_peek.exe
//           tools/account_peek.cpp src/common/shm.cpp src/common/logger.cpp
//   run:    account_peek.exe [--positions]

#include <cstdio>
#include <cstring>
#include <string>

#include "hft/channel.h"

using namespace hft;

int main(int argc, char** argv) {
    const bool showPositions = (argc > 1 && std::strcmp(argv[1], "--positions") == 0);

    Channels ch;
    std::string err;
    if (!ch.openSnapshot(false, err)) {
        std::printf("snapshot region not available: %s\n", err.c_str());
        return 2;
    }

    SnapshotReader r(ch.snapshot());
    if (!r.read()) {
        std::printf("snapshot is being written - retry\n");
        return 3;
    }
    const AccountSnap& a = r.account();

    std::printf("trading_day   : %s\n", ch.snapshot()->tradingDay);
    std::printf("pre_balance   : %14.2f\n", a.preBalance);
    std::printf("balance       : %14.2f\n", a.balance);
    std::printf("available     : %14.2f\n", a.available);
    std::printf("curr_margin   : %14.2f\n", a.currMargin);
    std::printf("close_profit  : %14.2f   <- realised P&L today\n", a.closeProfit);
    std::printf("position_pnl  : %14.2f   <- floating P&L\n", a.positionProfit);
    std::printf("commission    : %14.2f   <- fees today\n", a.commission);
    std::printf("net_today     : %14.2f   (balance - pre_balance)\n",
                a.balance - a.preBalance);
    std::printf("positions     : %u\n", r.positionCount());

    if (showPositions) {
        for (const PositionSnap& p : r.positions()) {
            std::printf("  %-10s dir=%c date=%c pos=%d today=%d yd=%d profit=%.2f\n",
                        p.instrument, p.direction, p.positionDate, p.position,
                        p.todayPosition, p.ydPosition, p.positionProfit);
        }
    }
    return 0;
}
