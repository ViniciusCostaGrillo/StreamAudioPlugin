#pragma once

#include <Protocol.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace StreamAudio::ConfigStore {

namespace detail {

inline std::string trimString(std::string_view sv) {
    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t' || sv.front() == '\r' || sv.front() == '\n')) {
        sv.remove_prefix(1);
    }
    while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\r' || sv.back() == '\n')) {
        sv.remove_suffix(1);
    }
    return std::string(sv);
}

} // namespace detail

/**
 * Returns the path to recent.ini.
 * If baseDirOverride is non-empty, returns <baseDirOverride>\StreamAudioPlugin\recent.ini.
 * Otherwise, returns %APPDATA%\StreamAudioPlugin\recent.ini.
 */
inline std::string getIniPath(const std::string& baseDirOverride = "") {
    std::filesystem::path p;
    if (!baseDirOverride.empty()) {
        p = std::filesystem::path(baseDirOverride);
    } else {
        const char* appData = std::getenv("APPDATA");
        if (appData != nullptr && *appData != '\0') {
            p = std::filesystem::path(appData);
        } else {
            p = std::filesystem::current_path();
        }
    }
    p /= "StreamAudioPlugin";
    p /= "recent.ini";
    return p.string();
}

/**
 * Loads up to 10 valid custom IPv4 strings from recent.ini in most-recent order.
 */
inline std::vector<std::string> loadRecentIps(const std::string& baseDirOverride = "") {
    const std::string path = getIniPath(baseDirOverride);
    std::ifstream file(path);
    if (!file.is_open()) {
        return {};
    }

    std::vector<std::string> result;
    std::string line;
    while (std::getline(file, line)) {
        std::string trimmed = detail::trimString(line);
        if (trimmed.empty() || trimmed.front() == '[' || trimmed.front() == ';' || trimmed.front() == '#') {
            continue;
        }

        const size_t eqPos = trimmed.find('=');
        if (eqPos != std::string::npos) {
            trimmed = detail::trimString(trimmed.substr(eqPos + 1));
        }

        Protocol::DestinationKind kind;
        uint32_t netIp = 0;
        if (Protocol::resolveDestination(trimmed, kind, netIp) && kind == Protocol::DestinationKind::CustomIp) {
            if (std::find(result.begin(), result.end(), trimmed) == result.end()) {
                result.push_back(trimmed);
                if (result.size() >= 10) {
                    break;
                }
            }
        }
    }

    return result;
}

/**
 * Saves a valid custom IPv4 string into recent.ini.
 * Validates strict custom IPv4, deduplicates (moves to front), retains max 10 entries,
 * creates parent directory if necessary.
 */
inline void saveRecentIp(const std::string& ip, const std::string& baseDirOverride = "") {
    const std::string trimmed = detail::trimString(ip);
    Protocol::DestinationKind kind;
    uint32_t netIp = 0;
    if (!Protocol::resolveDestination(trimmed, kind, netIp) || kind != Protocol::DestinationKind::CustomIp) {
        return;
    }

    auto existing = loadRecentIps(baseDirOverride);
    existing.erase(std::remove(existing.begin(), existing.end(), trimmed), existing.end());
    existing.insert(existing.begin(), trimmed);
    if (existing.size() > 10) {
        existing.resize(10);
    }

    const std::string path = getIniPath(baseDirOverride);
    std::filesystem::path fsPath(path);
    if (fsPath.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(fsPath.parent_path(), ec);
    }

    std::ofstream file(path, std::ios::trunc);
    if (!file.is_open()) {
        return;
    }

    file << "[RecentIPs]\n";
    for (size_t i = 0; i < existing.size(); ++i) {
        file << "ip" << i << "=" << existing[i] << "\n";
    }
    file.flush();
}

} // namespace StreamAudio::ConfigStore
