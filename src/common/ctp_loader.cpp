// SPDX-License-Identifier: MIT
// ctp_loader.cpp - runtime loader for the CTP SDK DLLs (see ctp_loader.h).
//
// Kept free of any CTP header: the factories are declared through function
// pointer types with forward-declared return classes, so this translation
// unit stays pure ASCII and needs neither the GBK-encoded SDK headers nor
// the SDK include path.
#include "ctp_loader.h"

#include "hft/config.h"
#include "hft/logger.h"

#include <windows.h>

#include <cstring>

namespace hft {
namespace ctp {

// MSVC x64 mangled names of the exported factories (dumped from
// thosttraderapi.dll / thostmduserapi.dll export tables).
static const char kCreateTraderSymbol[] =
    "?CreateFtdcTraderApi@CThostFtdcTraderApi@@SAPEAV1@PEBD@Z";
static const char kTraderVersionSymbol[] =
    "?GetApiVersion@CThostFtdcTraderApi@@SAPEBDXZ";
static const char kCreateMdSymbol[] =
    "?CreateFtdcMdApi@CThostFtdcMdApi@@SAPEAV1@PEBD_N1@Z";
static const char kMdVersionSymbol[] = "?GetApiVersion@CThostFtdcMdApi@@SAPEBDXZ";

static const char kTraderDll[] = "thosttraderapi.dll";
static const char kMdDll[] = "thostmduserapi.dll";
// The standard (non-Mini) SFIT SDK since 6.3.13 ships "data collecting" DLLs
// with an _se suffix; try both names so either SDK drop works unchanged.
static const char kTraderDllSe[] = "thosttraderapi_se.dll";
static const char kMdDllSe[] = "thostmduserapi_se.dll";

static std::string lastWin32Error(const char* what, const char* name) {
    const DWORD e = GetLastError();
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s failed (GetLastError=%lu)", what,
                  static_cast<unsigned long>(e));
    std::string msg = buf;
    if (e == 126) {  // ERROR_MOD_NOT_FOUND
        msg += " - the module or one of its dependencies was not found; ";
        msg += "put ";
        msg += name;
        msg += " next to the executables (or set ctp_dll_dir in [general])";
    } else if (e == 127) {  // ERROR_PROC_NOT_FOUND
        msg += " - the DLL was found but does not export the expected CTP entry";
        msg += " point; is it really the 64-bit SFIT CTP SDK?";
    }
    return msg;
}

bool CtpLib::loadDll(const char* name, const char* fallbackName, const char* explicitDir,
                     void*& module, std::string& err) {
    if (module) return true;  // already loaded

    HMODULE h = nullptr;
    if (explicitDir && *explicitDir) {
        std::string path = std::string(explicitDir);
        // Trim trailing separators, then append the DLL name.
        while (!path.empty() && (path.back() == '\\' || path.back() == '/')) path.pop_back();
        if (!path.empty()) {
            path += '\\';
            path += name;
            h = static_cast<HMODULE>(
                LoadLibraryExA(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH));
            if (h) HFT_LOGI("ctp: loaded %s (from %s)", name, path.c_str());
        }
    }
    if (!h) {
        // Standard search: exe directory first - DLLs copied next to the
        // binaries are found without any configuration.
        h = static_cast<HMODULE>(LoadLibraryA(name));
        if (h) HFT_LOGI("ctp: loaded %s (standard search path)", name);
    }
    if (!h && fallbackName) {
        // _se suffix fallback (standard SDK flavour).
        h = static_cast<HMODULE>(LoadLibraryA(fallbackName));
        if (h) HFT_LOGI("ctp: loaded %s (standard search path)", fallbackName);
    }
    if (!h) {
        err = lastWin32Error("LoadLibrary", name);
        return false;
    }
    module = h;
    return true;
}

bool CtpLib::loadTrader(const char* explicitDir, std::string& err) {
    if (!loadDll(kTraderDll, kTraderDllSe, explicitDir, traderDll_, err)) return false;
    const HMODULE h = static_cast<HMODULE>(traderDll_);
    createTrader_ = reinterpret_cast<decltype(createTrader_)>(
        reinterpret_cast<void*>(GetProcAddress(h, kCreateTraderSymbol)));
    traderVersion_ = reinterpret_cast<decltype(traderVersion_)>(
        reinterpret_cast<void*>(GetProcAddress(h, kTraderVersionSymbol)));
    if (!createTrader_) {
        err = lastWin32Error("GetProcAddress", kTraderDll);
        return false;
    }
    if (traderVersion_) HFT_LOGI("ctp: trader api version %s", traderVersion());
    return true;
}

bool CtpLib::loadMd(const char* explicitDir, std::string& err) {
    if (!loadDll(kMdDll, kMdDllSe, explicitDir, mdDll_, err)) return false;
    const HMODULE h = static_cast<HMODULE>(mdDll_);
    createMd_ = reinterpret_cast<decltype(createMd_)>(
        reinterpret_cast<void*>(GetProcAddress(h, kCreateMdSymbol)));
    mdVersion_ = reinterpret_cast<decltype(mdVersion_)>(
        reinterpret_cast<void*>(GetProcAddress(h, kMdVersionSymbol)));
    if (!createMd_) {
        err = lastWin32Error("GetProcAddress", kMdDll);
        return false;
    }
    if (mdVersion_) HFT_LOGI("ctp: md api version %s", mdVersion());
    return true;
}

CThostFtdcTraderApi* CtpLib::createTraderApi(const char* frontAddress,
                                             std::string& err) const {
    if (!createTrader_) {
        err = "thosttraderapi.dll is not loaded";
        return nullptr;
    }
    CThostFtdcTraderApi* api = createTrader_(frontAddress);
    if (!api) err = "CreateFtdcTraderApi returned null (invalid front address?)";
    return api;
}

CThostFtdcMdApi* CtpLib::createMdApi(const char* flowPath, bool useUdp, bool multicast,
                                      std::string& err) const {
    if (!createMd_) {
        err = "thostmduserapi.dll is not loaded";
        return nullptr;
    }
    CThostFtdcMdApi* api = createMd_(flowPath, useUdp, multicast);
    if (!api) err = "CreateFtdcMdApi returned null (check the flow path)";
    return api;
}

const char* CtpLib::traderVersion() const {
    return traderVersion_ ? traderVersion_() : "unknown";
}

const char* CtpLib::mdVersion() const { return mdVersion_ ? mdVersion_() : "unknown"; }

CtpLib& ctpLib() {
    static CtpLib instance;
    return instance;
}

std::string normalizeFlowPath(const std::string& path) {
    std::string p = path;
    while (!p.empty() && (p.back() == '/' || p.back() == '\\')) p.pop_back();
    if (p.empty()) p = ".";
    p += '\\';
    return p;
}

int prepareFlowDir(const std::string& dir) {
    if (dir.empty()) return 0;
    // Ensure the directory exists (ignore "already exists").
    std::string bare = dir;
    if (!bare.empty() && (bare.back() == '/' || bare.back() == '\\')) bare.pop_back();
    CreateDirectoryA(bare.c_str(), nullptr);

    std::string pattern = dir;
    pattern += "*.con";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int removed = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::string full = dir + fd.cFileName;
        if (DeleteFileA(full.c_str())) ++removed;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    if (removed > 0) {
        HFT_LOGI("ctp: removed %d stale flow file(s) from %s (fresh session)", removed,
                 bare.c_str());
    }
    return removed;
}

} // namespace ctp
} // namespace hft
