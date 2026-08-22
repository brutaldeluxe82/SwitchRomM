#pragma once

#include "romm/errors.hpp"
#include "romm/layout.hpp"
#include <string>

namespace romm {

struct Config {
    // Parsed config schema version (JSON); .env paths default to current behavior.
    int schemaVersion{1};
    // Base RomM server URL (http only)
    std::string serverUrl;
    // Optional token (currently unused)
    std::string apiToken;
    // Optional Basic auth credentials
    std::string username;
    std::string password;
    // Platform slug (optional; UI drives selection when empty)
    std::string platform{""};
    // Output layout ("tico" | "retroarch"); drives default dirs and extraction.
    std::string outputLayout{"tico"};
    // Destination directory on SD for downloads (platform/rom subfolders created automatically).
    // Empty derives from the output layout (see effectiveDownloadDir).
    std::string downloadDir{""};
    // BIOS directory base. Empty derives from the output layout (see effectiveBiosDir).
    std::string biosDir{""};
    // HTTP timeout (seconds) for network calls
    int httpTimeoutSeconds{30};
    // FAT32-safe split flag
    bool fat32Safe{false};
    // Logging verbosity (debug, info, warn, error)
    std::string logLevel{"info"};
    // Optional URL to fetch ~10MB for a quick throughput estimate; blank to skip.
    std::string speedTestUrl;
    // Platform prefs source selection
    std::string platformPrefsMode{"auto"};      // auto | sd | romfs
    std::string platformPrefsPathSd{"sdmc:/switch/SwitchRomM/platform_prefs.json"};
    std::string platformPrefsPathRomfs{"romfs:/platform_prefs.json"};
};

// Effective download root for a config: explicit downloadDir if set, else the
// layout's default.
inline std::string effectiveDownloadDir(const Config& c) {
    OutputLayout layout = parseOutputLayout(c.outputLayout);
    return c.downloadDir.empty() ? defaultDownloadDir(layout) : c.downloadDir;
}

// Effective BIOS root for a config: explicit biosDir if set, else the layout's default.
inline std::string effectiveBiosDir(const Config& c) {
    OutputLayout layout = parseOutputLayout(c.outputLayout);
    return c.biosDir.empty() ? defaultBiosDir(layout) : c.biosDir;
}

bool loadConfig(Config& outCfg, std::string& outError, ErrorInfo* outInfo = nullptr);

#ifdef UNIT_TEST
// Test helper: parse .env-style content from an in-memory string.
bool parseEnvString(const std::string& contents, Config& outCfg, std::string& outError, ErrorInfo* outInfo = nullptr);
// Test helper: parse config.json-style content from an in-memory string.
bool parseJsonString(const std::string& contents, Config& outCfg, std::string& outError, ErrorInfo* outInfo = nullptr);
#endif

} // namespace romm
