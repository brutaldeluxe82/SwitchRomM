// screens_core.cpp — Login, Settings, Server Address, Info, Update screens.
// Grout parity: login.go (server/auth/pairing), settings.go + general/collections/
// tools/advanced, server_address.go, info.go (with logout), update.go.

#include "screens.hpp"
#include "services.hpp"

#include "romm/version.hpp"
#include "romm/self_update.hpp"
#include "romm/logger.hpp"

#include <cstdio>
#include <ctime>

namespace romm::ui {

namespace {

// libnx software keyboard prompt (ported promptText semantics).
bool PromptText(const char* header, const char* guide, std::string& inout) {
    SwkbdConfig kbd;
    Result rc = swkbdCreate(&kbd, 0);
    if (R_FAILED(rc)) return false;
    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetHeaderText(&kbd, header);
    swkbdConfigSetSubText(&kbd, guide);
    swkbdConfigSetInitialText(&kbd, inout.c_str());
    swkbdConfigSetStringLenMax(&kbd, 250);
    char buf[256] = {};
    rc = swkbdShow(&kbd, buf, sizeof(buf));
    swkbdClose(&kbd);
    if (R_FAILED(rc)) return false;
    inout = buf;
    return true;
}

} // namespace

// ---- Login: Server info (login.go drawServerInfo) ----

void BuildServerAuthScreen(App& a) {
    auto* s = a.services;
    bool paired = !s->config.apiToken.empty();
    bool hasBasic = !s->config.username.empty() && !s->config.password.empty();

    BuildOptionsScreen(a, "Server",
        {
            {"Server URL", {},
             0,
             [s, &a]() {
                 std::string v = s->config.serverUrl;
                 if (PromptText("Server URL", "romm.example.com (https:// auto-added)", v)) {
                     s->config.serverUrl = RomServices::NormalizeServerUrl(v);
                     s->PersistConfig("Server saved: " + s->config.serverUrl);
                     s->RefetchPlatforms();
                     a.RebuildCurrent();
                 }
             },
             nullptr},
            {"Protocol",
             {"https", "http"},
             s->config.serverUrl.rfind("http://", 0) == 0 ? 1 : 0,
             nullptr,
             [s](int idx) {
                 std::string url = s->config.serverUrl;
                 size_t pos = url.find("://");
                 std::string rest = (pos == std::string::npos) ? url : url.substr(pos + 3);
                 s->config.serverUrl = (idx == 1 ? "http://" : "https://") + rest;
                 s->PersistConfig("Protocol: " + std::string(idx == 1 ? "http" : "https"));
                 s->RefetchPlatforms();
             }},
            {"Pair Device (device code)", {},
             0,
             [s, &a]() {
                 a.pushScreen(static_cast<int>(ScreenId::DevicePairing));
             },
             nullptr},
            {"Sign Out", {},
             0,
             [s, &a]() {
                 if (s->config.apiToken.empty()) {
                     a.queueToast("Not paired (no device token).");
                     return;
                 }
                 s32 choice = a.ShowDialog("Sign Out",
                     "Remove all stored credentials (device token + Basic auth)\nfrom this Switch?",
                     {"Cancel", "Confirm"}, true);
                 if (choice != 1) return;
                 // Clobber EVERYTHING: device token file, in-memory bearer
                 // token, and Basic Auth username/password (user requirement).
                 if (std::remove(romm::kDeviceTokenPath) == 0) {
                     romm::logLine("AUTH: signed out (device_token.json removed)");
                 }
                 s->config.apiToken.clear();
                 s->config.username.clear();
                 s->config.password.clear();
                 s->PersistConfig("Signed out - credentials cleared");
                 a.queueToast("Signed out");
                 a.RebuildCurrent();
             },
             nullptr},
            // Basic Auth kept for advanced fallback only; hidden from the
            // normal flow (device-code pairing is preferred, per Grout).
            {"Use Basic Auth (advanced)", {},
             0, nullptr, nullptr, /*visible=*/false},
        },
        [s, &a]() {
            if (a.stack.size() <= 1) {
                s->quitRequested.store(true); // Grout: B Quit from first screen
            } else {
                a.popScreen();
            }
        },
        {"B Quit", "A Select"});
    (void)paired; (void)hasBasic;
}

// ---- Device Pairing (device_pairing.go) ----

void BuildDevicePairingScreen(App& a) {
    auto* s = a.services;
    a.layout->Clear();
    a.title = "Pair with RomM";
    a.footerHints = {"A Start/Retry", "B Cancel"};
    BuildStatusBar(a);
    BuildTitle(a, "Pair this Switch with your RomM server", 36);
    BuildFooter(a);

    std::string userCode;
    std::string verifyUrl;
    std::string stateText;
    bool paired = false;
    {
        std::lock_guard<std::mutex> lock(s->status.mutex);
        userCode = s->status.savePairUserCode;
        verifyUrl = s->status.saveVerificationPath;
        paired = (s->status.savePairState == SavePairState::Paired);
        switch (s->status.savePairState) {
            case SavePairState::Idle: stateText = "Press A to start pairing."; break;
            case SavePairState::Initiating: stateText = "Initiating pairing..."; break;
            case SavePairState::AwaitingApproval: stateText = "1. Open the address below\n2. Enter the code\n3. Approve this device"; break;
            case SavePairState::Paired: stateText = "Successfully paired!"; break;
            case SavePairState::Error: stateText = s->status.savePairDetail; break;
        }
    }
    AddText(a.layout, stateText, kMargin, 180, kHint, kFontM, 1740);
    if (!userCode.empty()) {
        // Left column: code + URL. Right: QR of the full verification URL —
        // scanning it lands on RomM's approve page with the code prefilled.
        AddText(a.layout, "DEVICE CODE", kMargin, 375, kAccent, kFontS);
        AddText(a.layout, userCode, kMargin, 450, kAccent, kFont);
        AddText(a.layout, verifyUrl.empty() ? "(verification URL unavailable)" : verifyUrl,
                kMargin, 630, kHint, kFontM, 900);
        if (!verifyUrl.empty()) {
            AddText(a.layout, "Scan to open:", 1100, 250, kAccent, kFontS);
            AddQrCode(a.layout, verifyUrl, 1100, 310, 620);
        }
    }
    a.layout->SetOnInput([s, &a, paired](const u64 down, const u64, const u64, pu::ui::TouchPoint) {
        if (down & HidNpadButton_B) {
            s->saveCancel.store(true);
            a.popScreen();
        } else if (down & HidNpadButton_A) {
            if (paired) {
                a.popScreen();
            } else {
                s->SubmitLoginPairing();
            }
        }
    });
}

// ---- Platform Mapping (settings_platform_mapping.go simplified to Switch dirs) ----

void BuildPlatformMappingScreen(App& a) {
    auto* s = a.services;
    std::vector<OptionRow> rows;
    rows.push_back({"Output Layout",
                    {"tico", "retroarch"},
                    romm::parseOutputLayout(s->config.outputLayout) == romm::OutputLayout::RetroArch ? 1 : 0,
                    nullptr,
                    [s](int idx) {
                        s->config.outputLayout = idx == 1 ? "retroarch" : "tico";
                        s->PersistConfig(std::string("Output Layout: ") + s->config.outputLayout);
                    }});
    rows.push_back({"Download Directory", {},
                    0,
                    [s, &a]() {
                        std::string v = s->config.downloadDir;
                        if (PromptText("Download Directory", "Empty = layout default", v)) {
                            s->config.downloadDir = v;
                            s->PersistConfig("Download directory saved");
                        }
                    }});
    rows.push_back({"BIOS Directory", {},
                    0,
                    [s, &a]() {
                        std::string v = s->config.biosDir;
                        if (PromptText("BIOS Directory", "Empty = layout default", v)) {
                            s->config.biosDir = v;
                            s->PersistConfig("BIOS directory saved");
                        }
                    }});
    BuildOptionsScreen(a, "Rom Directory Mapping", rows,
        [&a]() { a.popScreen(); },
        {"B Cancel", "A Select", "Start Save"});
}

// ---- Settings root (settings.go) ----

void BuildSettingsScreen(App& a) {
    auto* s = a.services;
    BuildOptionsScreen(a, "Settings",
        {
            {"Server & Auth", {}, 0, [&a]() { a.pushScreen(static_cast<int>(ScreenId::ServerAuth)); }},
            {"General", {}, 0, [&a]() { a.pushScreen(static_cast<int>(ScreenId::GeneralSettings)); }},
            {"Collections Settings", {}, 0, [&a]() { a.pushScreen(static_cast<int>(ScreenId::CollectionsSettings)); }},
            {"Directory Mappings", {}, 0, [&a]() { a.pushScreen(static_cast<int>(ScreenId::PlatformMapping)); }},
            {"Save Sync", {}, 0, [&a]() { a.pushScreen(static_cast<int>(ScreenId::SaveSyncSettings)); }},
            {"Tools", {}, 0, [&a]() { a.pushScreen(static_cast<int>(ScreenId::ToolsSettings)); }},
            {"Advanced", {}, 0, [&a]() { a.pushScreen(static_cast<int>(ScreenId::AdvancedSettings)); }},
            {"Grout Info", {}, 0, [&a]() { a.pushScreen(static_cast<int>(ScreenId::Info)); }},
            {"Check for Updates", {}, 0, [&a]() { a.pushScreen(static_cast<int>(ScreenId::UpdateCheck)); }},
        },
        [&a]() { a.popScreen(); },
        {"B Back", "A Select"});
    (void)s;
}

// ---- General Settings (general_settings.go) ----

void BuildGeneralSettingsScreen(App& a) {
    auto* s = a.services;
    int layoutIdx = romm::parseOutputLayout(s->config.outputLayout) == romm::OutputLayout::RetroArch ? 1 : 0;
    int extractIdx = s->config.extractArchive ? 0 : 1; // Uncompress / Do Nothing
    int backupsIdx = 0;
    {
        static const int limits[4] = {0, 5, 10, 15};
        for (int i = 0; i < 4; ++i) if (s->config.saveBackupLimit == limits[i]) backupsIdx = i;
    }
    int timeoutIdx = std::max(0, (s->config.downloadTimeoutMinutes - 15) / 15);
    int policyIdx = 0;
    {
        const std::vector<const char*> policies{"ask", "newest", "server", "client"};
        for (size_t i = 0; i < policies.size(); ++i)
            if (s->config.saveSyncBehavior == policies[i]) policyIdx = static_cast<int>(i);
    }
    int fat32Idx = s->config.fat32Safe ? 1 : 0;

    BuildOptionsScreen(a, "General",
        {
            {"Output Layout", {"tico", "retroarch"}, layoutIdx, nullptr,
             [s](int idx) {
                 s->config.outputLayout = idx == 1 ? "retroarch" : "tico";
                 s->PersistConfig(std::string("Output Layout: ") + s->config.outputLayout);
             }},
            {"Archived Downloads", {"Uncompress", "Do Nothing"}, extractIdx, nullptr,
             [s](int idx) {
                 s->config.extractArchive = (idx == 0);
                 s->PersistConfig(std::string("Extract Archives: ") + (s->config.extractArchive ? "on" : "off"));
             }},
            {"Hide Non-Tico Platforms", {"Off", "On"},
             s->config.hideUnsupportedPlatforms ? 1 : 0, nullptr,
             [s](int idx) {
                 s->config.hideUnsupportedPlatforms = (idx == 1);
                 s->PersistConfig(std::string("Hide Non-Tico Platforms: ") +
                                  (s->config.hideUnsupportedPlatforms ? "on" : "off"));
                 s->RefetchPlatforms(); // re-filter the visible list immediately
             }},
            {"FAT32 Safe", {"No", "Yes"}, fat32Idx, nullptr,
             [s](int idx) {
                 s->config.fat32Safe = (idx == 1);
                 s->PersistConfig(std::string("FAT32 Safe: ") + (s->config.fat32Safe ? "on" : "off"));
             }},
            {"Download Timeout", {},
             timeoutIdx,
             [s]() {
                 s->config.downloadTimeoutMinutes =
                     s->config.downloadTimeoutMinutes >= 120 ? 15 : s->config.downloadTimeoutMinutes + 15;
                 s->PersistConfig("Download Timeout: " + std::to_string(s->config.downloadTimeoutMinutes) + " min");
             },
             nullptr},
            {"Save Backups", {"No limit", "5", "10", "15"}, backupsIdx, nullptr,
             [s](int idx) {
                 static const int limits[4] = {0, 5, 10, 15};
                 s->config.saveBackupLimit = limits[idx];
                 s->PersistConfig("Save Backups: " + std::string(s->config.saveBackupLimit <= 0 ? "No limit"
                                                                                               : std::to_string(s->config.saveBackupLimit)));
             }},
            {"Save Sync", {"Ask", "Newest wins", "Server wins", "Client wins"}, policyIdx, nullptr,
             [s](int idx) {
                 static const char* policies[4] = {"ask", "newest", "server", "client"};
                 s->config.saveSyncBehavior = policies[idx];
                 s->PersistConfig("Save Sync: " + s->config.saveSyncBehavior);
             }},
        },
        [&a]() { a.popScreen(); },
        {"B Cancel", "A Select", "Start Save"});
}

// ---- Collections Settings (collections_settings.go; Switch has no collections cache yet) ----

void BuildCollectionsSettingsScreen(App& a) {
    BuildOptionsScreen(a, "Collections",
        {
            {"Collections", {"Not available on Switch yet"}, 0},
        },
        [&a]() { a.popScreen(); },
        {"B Back", "A Select"});
}

// ---- Tools (tools_settings.go) ----

void BuildToolsSettingsScreen(App& a) {
    auto* s = a.services;
    BuildOptionsScreen(a, "Tools",
        {
            {"Kid Mode", {"Disabled", "Enabled"}, 0, nullptr, nullptr, false}, // hidden: no kid mode on Switch
            {"Rescan SD Saves", {},
             0,
             [s]() { s->SubmitDiscovery("tools"); }},
        },
        [&a]() { a.popScreen(); },
        {"B Back", "A Select"});
}

// ---- Advanced Settings (advanced_settings.go) ----

void BuildAdvancedSettingsScreen(App& a) {
    auto* s = a.services;
    int logIdx = 1;
    {
        const std::vector<const char*> levels{"debug", "info", "warn", "error"};
        for (size_t i = 0; i < levels.size(); ++i)
            if (s->config.logLevel == levels[i]) logIdx = static_cast<int>(i);
    }
    BuildOptionsScreen(a, "Advanced",
        {
            {"Server & Auth", {},
             0,
             [&a]() { a.pushScreen(static_cast<int>(ScreenId::ServerAuth)); }},
            {"API Timeout (15-300s)", {},
             0,
             [s]() {
                 s->config.httpTimeoutSeconds = s->config.httpTimeoutSeconds >= 300
                     ? 15 : s->config.httpTimeoutSeconds + 15;
                 s->PersistConfig("API Timeout: " + std::to_string(s->config.httpTimeoutSeconds) + "s");
             },
             nullptr},
            {"Log Level", {"Debug", "Info", "Warn", "Error"}, logIdx, nullptr,
             [s](int idx) {
                 static const char* levels[4] = {"debug", "info", "warn", "error"};
                 s->config.logLevel = levels[idx];
                 s->PersistConfig("Log Level: " + s->config.logLevel);
             }},
        },
        [&a]() { a.popScreen(); },
        {"B Back", "A Select", "Start Save"});
}


// ---- Grout Info (info.go) ----

void BuildInfoScreen(App& a) {
    auto* s = a.services;
    a.layout->Clear();
    a.title = "Info";
    a.footerHints = {"B Back", "X Logout"};
    BuildStatusBar(a);
    BuildTitle(a, "Info");
    BuildFooter(a);

    s32 y = 195;
    auto section = [&](const std::string& t) {
        AddText(a.layout, t, kMargin, y, kAccent, kFontS);
        y += 69;
    };
    auto row = [&](const std::string& k, const std::string& v) {
        AddText(a.layout, k + ":  " + v, kMargin + 30, y, kHint, kFontS, 1710);
        y += 60;
    };
    section("TicromM");
    row("Version", romm::appVersion());
    row("UI", "Plutonium (Grout parity)");
    section("RomM");
    {
        std::lock_guard<std::mutex> lock(s->status.mutex);
        row("Server", s->config.serverUrl);
        row("Device", s->config.deviceName);
        row("Paired", !s->config.apiToken.empty() ? "Yes" : "No");
        row("Server version", s->status.saveServerVersion.empty() ? "Unknown" : s->status.saveServerVersion);
    }
    a.layout->SetOnInput([s, &a](const u64 down, const u64, const u64, pu::ui::TouchPoint) {
        if (down & HidNpadButton_B) {
            a.popScreen();
        } else if (down & HidNpadButton_X) {
            s32 choice = a.ShowDialog("Logout",
                "Are you sure you want to logout?",
                {"Cancel", "Confirm"}, true);
            if (choice == 1) {
                // Clobber all credentials (device token + Basic auth).
                if (std::remove(romm::kDeviceTokenPath) == 0) {
                    romm::logLine("INFO: signed out (device_token.json removed)");
                }
                s->config.apiToken.clear();
                s->config.username.clear();
                s->config.password.clear();
                s->PersistConfig("Signed out - credentials cleared");
                a.queueToast("Signed out");
                a.popScreen();
            }
        }
    });
}

// ---- Check for Updates (update.go) ----

void BuildUpdateCheckScreen(App& a) {
    auto* s = a.services;
    a.layout->Clear();
    a.title = "Check for Updates";
    a.footerHints = {"A Check", "X Download", "B Back"};
    BuildStatusBar(a);
    BuildTitle(a, "Check for Updates");
    BuildFooter(a);

    std::string statusText;
    bool canDownload = false;
    bool downloadInFlight = false;
    {
        std::lock_guard<std::mutex> lock(s->status.mutex);
        statusText = s->status.updateStatus.empty() ? "Press A to check for updates." : s->status.updateStatus;
        canDownload = s->status.updateAvailable && !s->status.updateDownloaded;
        downloadInFlight = s->status.updateDownloadInFlight;
        if (!s->status.updateLatestTag.empty() && s->status.updateAvailable) {
            statusText += "\nLatest: " + s->status.updateLatestTag;
        }
        if (s->status.updateDownloaded) {
            statusText += " Restart app to apply.";
        }
        if (!s->status.updateError.empty()) {
            statusText += " " + s->status.updateError;
        }
    }
    AddText(a.layout, statusText, kMargin, 210, kHint, kFontM, 1740);
    if (downloadInFlight) {
        AddText(a.layout, "Downloading...", kMargin, 390, kAccent, kFontS);
    }
    a.layout->SetOnInput([s, &a](const u64 down, const u64, const u64, pu::ui::TouchPoint) {
        if (down & HidNpadButton_B) {
            a.popScreen();
        } else if (down & HidNpadButton_A) {
            s->SubmitUpdateCheck();
        } else if (down & HidNpadButton_X) {
            s->SubmitUpdateDownload();
        }
    });
}

// ---- Rebuild Cache (rebuild_cache.go -> Switch equivalent: rescan/reload) ----

void BuildRebuildCacheScreen(App& a) {
    auto* s = a.services;
    BuildOptionsScreen(a, "Rebuild Cache",
        {
            {"Rescan SD Saves", {}, 0, [s]() { s->SubmitDiscovery("rebuild"); }},
            {"Reload Platforms", {},
             0,
             [s]() {
                 std::lock_guard<std::mutex> lock(s->status.mutex);
                 s->status.platforms.clear();
                 s->status.romFetchGeneration++;
                 s->status.netBusy.store(true);
                 s->status.netBusyWhat = "Platforms";
                 s->BumpUi();
                 std::string err;
                 ErrorInfo info;
                 if (!romm::fetchPlatforms(s->config, s->status, err, &info)) {
                     std::lock_guard<std::mutex> lock2(s->status.mutex);
                     s->status.lastError = err;
                     s->status.lastErrorInfo = info;
                 }
                 s->status.netBusy.store(false);
                 s->BumpUi();
             }},
        },
        [&a]() { a.popScreen(); },
        {"B Cancel", "A Continue"});
}

// ---- Artwork Sync (artwork_sync.go; N/A on Switch — no artwork cache) ----

// ---- Input Mapping (input_mapping_screen.go; Plutonium handles Switch input) ----

} // namespace romm::ui
