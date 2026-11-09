// SPDX-License-Identifier: MIT
// ctp_loader.h - Runtime loader for the CTP SDK DLLs.
//
// The official SFIT CTP Mini SDK ships Windows import libraries in MSVC format
// only. Linking them from MinGW is impossible (the C++ symbols are MSVC
// name-mangled, MinGW emits GCC mangling), which used to mean "no live trading
// without Visual Studio".
//
// That restriction is artificial: the CTP API surface is deliberately ABI
// friendly -
//   * the API classes are pure-virtual, single-inheritance interfaces whose
//     vtable layout is identical under MSVC and GCC on x64 (and the very same
//     headers are compiled with GCC against the Linux .so build of CTP),
//   * every data structure is a POD with fixed-width char arrays,
//   * the only entry points a client needs are the two exported factories.
// So instead of linking an import library we resolve those factories at
// run time with LoadLibrary/GetProcAddress - MSVC builds use exactly the same
// path, so there is a single code path for both toolchains.
#ifndef HFT_CTP_LOADER_H
#define HFT_CTP_LOADER_H

#include <string>

class CThostFtdcTraderApi;
class CThostFtdcMdApi;

namespace hft {
namespace ctp {

// Loads one of the CTP SDK DLLs and resolves its factory function.
//
// Search order (first hit wins):
//   1. <explicitDir>\thosttraderapi.dll / thostmduserapi.dll
//      (explicitDir comes from [general] ctp_dll_dir, optional)
//   2. the plain name through the standard loader search path, whose first
//      entry is the directory of the running .exe - so simply dropping the
//      two DLLs next to the executables just works.
//
// All methods are safe to call from any process; each DLL is loaded at most
// once (the loader keeps the module alive until process exit on purpose -
// CTP keeps background threads and flow files inside the library, unloading
// it while trading is undefined behaviour).
class CtpLib {
public:
    bool loadTrader(const char* explicitDir, std::string& err);
    bool loadMd(const char* explicitDir, std::string& err);

    // May return null only if loadXxx() failed; a null return from the factory
    // itself is surfaced through the err of the wrapper below.
    CThostFtdcTraderApi* createTraderApi(const char* frontAddress, std::string& err) const;
    CThostFtdcMdApi* createMdApi(const char* flowPath, bool useUdp, bool multicast,
                                 std::string& err) const;

    // Static version strings from the DLLs ("unknown" when not loaded).
    const char* traderVersion() const;
    const char* mdVersion() const;

    bool traderLoaded() const { return traderDll_ != nullptr; }
    bool mdLoaded() const { return mdDll_ != nullptr; }

private:
    bool loadDll(const char* name, const char* fallbackName, const char* explicitDir, void*& module,
                 std::string& err);

    void* traderDll_ = nullptr;  // HMODULE, kept as void* to avoid windows.h here
    void* mdDll_ = nullptr;
    CThostFtdcTraderApi* (*createTrader_)(const char*) = nullptr;
    CThostFtdcMdApi* (*createMd_)(const char*, bool, bool) = nullptr;
    const char* (*traderVersion_)() = nullptr;
    const char* (*mdVersion_)() = nullptr;
};

// Process-wide instance; gateways share it.
CtpLib& ctpLib();

// ---- flow path helpers (shared by the md / trader / query gateways) ------
// CTP concatenates "TradingDay.con" etc. onto the configured flow path
// WITHOUT adding a separator, so "./flow/md" would scatter the session
// files into the parent directory ("./flow/mdTradingDay.con").
// normalizeFlowPath appends the missing trailing separator.
std::string normalizeFlowPath(const std::string& path);

// Makes sure the flow directory exists and deletes stale CTP session files
// (*.con) from it. A TradingDay.con left over from a previous trading day
// makes the front hang on the first login of the next day; wiping the flow
// directory on start forces a fresh session. Returns files removed.
int prepareFlowDir(const std::string& dir);

} // namespace ctp
} // namespace hft

#endif // HFT_CTP_LOADER_H
