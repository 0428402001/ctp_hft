// SPDX-License-Identifier: MIT
#include "hft/config.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace hft {

namespace {

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (std::isspace(static_cast<unsigned char>(s[b])))) ++b;
    while (e > b && (std::isspace(static_cast<unsigned char>(s[e - 1])))) --e;
    return s.substr(b, e - b);
}

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    return s;
}

// Strips an optional inline comment (";" or "#" outside of quotes).
std::string stripComment(const std::string& line) {
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (c == ';' || c == '#') return line.substr(0, i);
    }
    return line;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, sep)) {
        std::string t = trim(item);
        if (!t.empty()) out.push_back(t);
    }
    return out;
}

} // namespace

std::string Config::normalizeKey(const std::string& s) { return toLower(trim(s)); }

bool Config::load(const std::string& path, std::string& err) {
    path_ = path;
    values_.clear();
    loaded_ = false;

    std::ifstream in(path);
    if (!in.is_open()) {
        err = "cannot open config file: " + path;
        return false;
    }

    std::string section;
    std::string line;
    int lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string raw = stripComment(line);
        std::string t = trim(raw);
        if (t.empty()) continue;

        if (t.front() == '[') {
            if (t.back() != ']') {
                err = "malformed section header at line " + std::to_string(lineNo);
                return false;
            }
            section = normalizeKey(t.substr(1, t.size() - 2));
            continue;
        }

        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;  // tolerate stray lines
        std::string key = normalizeKey(t.substr(0, eq));
        std::string val = trim(t.substr(eq + 1));
        // strip matching quotes
        if (val.size() >= 2 && ((val.front() == '"' && val.back() == '"') ||
                                (val.front() == '\'' && val.back() == '\''))) {
            val = val.substr(1, val.size() - 2);
        }
        if (section.empty()) section = "general";
        values_[section + "." + key] = val;
    }

    loaded_ = true;
    return true;
}

bool Config::has(const std::string& section, const std::string& key) const {
    return values_.find(normalizeKey(section) + "." + normalizeKey(key)) != values_.end();
}

std::string Config::getString(const std::string& section, const std::string& key,
                              const std::string& def) const {
    auto it = values_.find(normalizeKey(section) + "." + normalizeKey(key));
    return (it == values_.end()) ? def : it->second;
}

int Config::getInt(const std::string& section, const std::string& key, int def) const {
    auto it = values_.find(normalizeKey(section) + "." + normalizeKey(key));
    if (it == values_.end()) return def;
    try {
        return std::stoi(it->second);
    } catch (...) {
        return def;
    }
}

double Config::getDouble(const std::string& section, const std::string& key, double def) const {
    auto it = values_.find(normalizeKey(section) + "." + normalizeKey(key));
    if (it == values_.end()) return def;
    try {
        return std::stod(it->second);
    } catch (...) {
        return def;
    }
}

bool Config::getBool(const std::string& section, const std::string& key, bool def) const {
    auto it = values_.find(normalizeKey(section) + "." + normalizeKey(key));
    if (it == values_.end()) return def;
    std::string v = toLower(it->second);
    if (v == "1" || v == "true" || v == "yes" || v == "on") return true;
    if (v == "0" || v == "false" || v == "no" || v == "off") return false;
    return def;
}

std::vector<std::string> Config::getList(const std::string& section, const std::string& key,
                                         const std::string& def) const {
    std::string raw = getString(section, key, def);
    return split(raw, ',');
}

std::string Config::investorId() const {
    std::string v = getString("account", "investor_id", "");
    return v.empty() ? userId() : v;
}

std::vector<std::string> Config::mdFronts() const {
    std::vector<std::string> out = getList("md", "fronts", "");
    std::string single = getString("md", "front", "");
    if (!single.empty()) out.push_back(single);
    return out;
}

std::vector<std::string> Config::tradeFronts() const {
    std::vector<std::string> out = getList("trader", "fronts", "");
    std::string single = getString("trader", "front", "");
    if (!single.empty()) out.push_back(single);
    return out;
}

std::vector<std::string> Config::queryFronts() const {
    std::vector<std::string> out = getList("query", "fronts", "");
    std::string single = getString("query", "front", "");
    if (!single.empty()) out.push_back(single);
    if (out.empty()) out = tradeFronts();  // query process shares the trade front by default
    return out;
}

} // namespace hft
