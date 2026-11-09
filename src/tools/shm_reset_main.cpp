// SPDX-License-Identifier: MIT
// shm_reset_main.cpp - cold-start utility: zeroes every shared-memory section
// of the ctp_hft bus.
//
// Run this BEFORE starting the four processes (the start_*.bat scripts do it
// automatically). It assumes the previous processes are already stopped:
// wiping a queue while its producer is alive would drop in-flight data.
#include "hft/channel.h"


#include <cstdio>
#include <cstring>
#include <string>
#include <type_traits>

using namespace hft;

namespace {

// Opens (or creates) one section and zeroes it. Returns its byte size.
template <typename Region>
uint64_t wipe(Region& region, const char* name) {
    if (!region.openOrCreate(name)) {
        std::fprintf(stderr, "shm_reset: cannot open '%s': %s\n", name,
                     region.lastError().c_str());
        return 0;
    }
    using T = typename std::remove_reference<decltype(*region.get())>::type;
    std::memset(static_cast<void*>(region.get()), 0, sizeof(T));
    region.close();
    return static_cast<uint64_t>(sizeof(T));
}

} // namespace

int main() {
    ShmRegion<ControlBlock> control;
    ShmRegion<TickChannel> ticks;
    ShmRegion<CmdChannel> commands;
    ShmRegion<ExecChannel> execs;
    ShmRegion<ExecToQueryChannel> execsToQuery;
    ShmRegion<MarketSnapshotBlock> market;
    ShmRegion<QryReqChannel> qryReq;
    ShmRegion<QryRspChannel> qryRsp;
    ShmRegion<SnapshotBlock> snapshot;

    uint64_t total = 0;
    total += wipe(control, kShmControl);
    total += wipe(ticks, kShmTicks);
    total += wipe(commands, kShmCommands);
    total += wipe(execs, kShmExecs);
    total += wipe(execsToQuery, kShmExecsToQuery);
    total += wipe(market, kShmMarket);
    total += wipe(qryReq, kShmQryReq);
    total += wipe(qryRsp, kShmQryRsp);
    total += wipe(snapshot, kShmSnapshot);

    if (total == 0) {
        std::fprintf(stderr, "shm_reset: no shared-memory section could be reset\n");
        return 1;
    }
    std::printf("shm_reset: 9 shared sections wiped (%.1f MB) - bus is clean\n",
                static_cast<double>(total) / 1024.0 / 1024.0);
    return 0;
}
