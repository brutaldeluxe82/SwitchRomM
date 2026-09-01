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
    // Device bearer token (set from config/env or the paired device_token.json
    // at startup/login); preferred over Basic credentials when non-empty.
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
    // Extract .zip archives on download (tico layout only); arcade-class romsets
    // (FBNeo/MAME cores) must have this false to keep archives intact.
    bool extractArchive{true};
    // Hide RomM platforms without a tico emulator/core (defaults on). Toggles
    // in General settings; affects the platform list + platform count labels.
    bool hideUnsupportedPlatforms{true};
    // Save sync conflict policy: ask | newest | server | client
    std::string saveSyncBehavior{"ask"};
    // Save backup retention limit for synced saves (0 = no limit; Grout offers 5/10/15).
    int saveBackupLimit{0};
    // Per-download timeout in minutes; clamped to [15,120] when parsed from config.
    int downloadTimeoutMinutes{60};
    // Device registration name shown on the server (editable in settings).
    std::string deviceName{"Switch"};
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

// Effective battery-save root for a config (no per-config override today):
//   tico: "sdmc:/tico/saves"; retroarch: "sdmc:/retroarch/.retroarch/saves".
inline std::string effectiveSaveDir(const Config& c) {
    OutputLayout layout = parseOutputLayout(c.outputLayout);
    if (layout == OutputLayout::RetroArch) return "sdmc:/retroarch/.retroarch/saves";
    return "sdmc:/tico/saves";
}

// Effective save-state root for a config:
//   tico: "sdmc:/tico/states"; retroarch: "sdmc:/retroarch/.retroarch/states".
inline std::string effectiveStatesDir(const Config& c) {
    OutputLayout layout = parseOutputLayout(c.outputLayout);
    if (layout == OutputLayout::RetroArch) return "sdmc:/retroarch/.retroarch/states";
    return "sdmc:/tico/states";
}

bool loadConfig(Config& outCfg, std::string& outError, ErrorInfo* outInfo = nullptr);

// Persist the interface-editable config (server_url/username/password and the
// other parseJson keys) to sdmc:/switch/romm_switch_client/config.json.
// apiToken is deliberately not written (see implementation note).
bool saveConfigJson(const Config& cfg, std::string& err);

#ifdef UNIT_TEST
// Test helper: parse .env-style content from an in-memory string.
bool parseEnvString(const std::string& contents, Config& outCfg, std::string& outError, ErrorInfo* outInfo = nullptr);
// Test helper: parse config.json-style content from an in-memory string.
bool parseJsonString(const std::string& contents, Config& outCfg, std::string& outError, ErrorInfo* outInfo = nullptr);
// Test helper: serialize cfg to config.json text (what saveConfigJson writes).
std::string serializeConfigJson(const Config& cfg);
#endif

} // namespace romm
