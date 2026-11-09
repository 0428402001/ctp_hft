// SPDX-License-Identifier: MIT
// config.h - INI configuration (login info, fronts, instruments, strategy knobs).
#ifndef HFT_CONFIG_H
#define HFT_CONFIG_H

#include "hft/platform.h"

#include <map>
#include <string>
#include <vector>

namespace hft {

// Everything the four processes need is described by one INI file so that no
// credentials are ever hard coded.
class Config {
public:
    // ---- raw access --------------------------------------------------------
    std::string getString(const std::string& section, const std::string& key,
                          const std::string& def = "") const;
    int getInt(const std::string& section, const std::string& key, int def = 0) const;
    double getDouble(const std::string& section, const std::string& key, double def = 0.0) const;
    bool getBool(const std::string& section, const std::string& key, bool def = false) const;
    std::vector<std::string> getList(const std::string& section, const std::string& key,
                                     const std::string& def = "") const;

    bool has(const std::string& section, const std::string& key) const;

    // ---- loading -----------------------------------------------------------
    bool load(const std::string& path, std::string& err);
    const std::string& path() const { return path_; }
    bool loaded() const { return loaded_; }

    // ---- commonly used groups ---------------------------------------------
    std::string brokerId() const { return getString("account", "broker_id", "9999"); }
    std::string userId() const { return getString("account", "user_id", ""); }
    std::string password() const { return getString("account", "password", ""); }
    std::string investorId() const;
    std::string appId() const { return getString("account", "app_id", ""); }
    std::string authCode() const { return getString("account", "auth_code", ""); }
    std::string productInfo() const { return getString("account", "product_info", "ctp_hft"); }

    std::vector<std::string> mdFronts() const;
    std::vector<std::string> tradeFronts() const;
    std::vector<std::string> queryFronts() const;

    std::string mdFlowPath() const { return getString("md", "flow_path", "./flow/md"); }
    std::string tradeFlowPath() const { return getString("trader", "flow_path", "./flow/trader"); }
    std::string queryFlowPath() const { return getString("query", "flow_path", "./flow/query"); }
    // Optional directory that holds thosttraderapi.dll / thostmduserapi.dll
    // for live mode. Empty = standard DLL search (exe directory first).
    std::string ctpDllDir() const { return getString("general", "ctp_dll_dir", ""); }

    std::vector<std::string> instruments() const { return getList("general", "instruments", ""); }
    std::string mode() const { return getString("general", "mode", "sim"); }
    bool simulation() const { return mode() != "live"; }
    std::string logDir() const { return getString("general", "log_dir", "./log"); }
    std::string strategyName() const { return getString("strategy", "name", "ma_tick"); }
    int queryIntervalMs() const { return getInt("query", "interval_ms", 3000); }
    int heartbeatMs() const { return getInt("general", "heartbeat_ms", 500); }

private:
    static std::string normalizeKey(const std::string& s);

    std::string path_;
    bool loaded_ = false;
    // key is "section.key" (lower cased)
    std::map<std::string, std::string> values_;
};

} // namespace hft

#endif // HFT_CONFIG_H
