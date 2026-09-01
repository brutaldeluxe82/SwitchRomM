// screens_sync.cpp — Save sync, BIOS download, and the screen dispatcher.
// Grout parity: sync_menu.go, save_sync.go, save_conflict.go,
// device_registration.go, save_mapping.go, synced_games.go, sync_history.go,
// bios_download.go.

#include "screens.hpp"
#include "services.hpp"

#include <switch.h>
#include "romm/logger.hpp"
#include "romm/firmware.hpp"

#include <cstdio>
#include <ctime>

namespace romm::ui {

namespace {

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

// ---- Sync Menu (sync_menu.go) ----

void BuildSyncMenuScreen(App& a) {
    BuildOptionsScreen(a, "Save Sync",
        {
            {"Sync Now", {}, 0, [&a]() {
                a.pushScreen(static_cast<int>(ScreenId::SaveSyncRun));
            }},
            {"Synced Games", {}, 0, [&a]() {
                a.pushScreen(static_cast<int>(ScreenId::SyncedGames));
            }},
            {"View History", {}, 0, [&a]() {
                a.pushScreen(static_cast<int>(ScreenId::SyncHistory));
            }},
        },
        [&a]() { a.popScreen(); },
        {"B Back", "A Select"});
}

// ---- Save Sync run (save_sync.go) ----

void BuildSaveSyncRunScreen(App& a) {
    auto* s = a.services;
    a.layout->Clear();
    a.title = "Sync Now";
    a.footerHints = {"A Sync", "B Back"};
    BuildStatusBar(a);
    BuildTitle(a, "Save Sync");
    BuildFooter(a);

    std::string statusText;
    bool busy = false;
    {
        std::lock_guard<std::mutex> lock(s->status.mutex);
        busy = s->status.saveSyncRunBusy.load();
        statusText = s->status.saveSyncStatusText;
    }
    if (statusText.empty()) statusText = busy ? "Syncing..." : "Press A to sync saves and states.";
    AddText(a.layout, statusText, kMargin, 210, busy ? kAccent : kHint, kFontM, 1740);

    a.layout->SetOnInput([s, &a](const u64 down, const u64, const u64, pu::ui::TouchPoint) {
        if (down & HidNpadButton_B) {
            a.popScreen();
        } else if (down & HidNpadButton_A) {
            s->SubmitServerSync("sync screen");
        }
    });
}

// ---- Save Conflict (save_conflict.go; policy is pre-configured on Switch) ----

void BuildSaveConflictScreen(App& a) {
    auto* s = a.services;
    std::string behavior;
    {
        std::lock_guard<std::mutex> lock(s->status.mutex);
        behavior = s->config.saveSyncBehavior;
    }
    BuildOptionsScreen(a, "Resolve Conflicts",
        {
            {"Conflict Policy",
             {"Ask every time", "Newest wins", "Server wins", "Client wins"},
             behavior == "newest" ? 1 : behavior == "server" ? 2 : behavior == "client" ? 3 : 0,
             nullptr,
             [s](int idx) {
                 static const char* policies[4] = {"ask", "newest", "server", "client"};
                 s->config.saveSyncBehavior = policies[idx];
                 s->PersistConfig("Save Sync: " + s->config.saveSyncBehavior);
             }},
        },
        [&a]() { a.popScreen(); },
        {"B Back", "A Select"});
}

// ---- Save Sync Settings / device registration (device_registration.go) ----

void BuildSaveSyncSettingsScreen(App& a) {
    auto* s = a.services;
    bool paired = !s->config.apiToken.empty();
    std::vector<OptionRow> rows;
    if (!paired) {
        rows.push_back({"Register Device", {}, 0,
            [s, &a]() {
                std::string name = s->config.deviceName;
                if (!PromptText("Device Name", "Name shown on the server", name)) return;
                s->config.deviceName = name.empty() ? "Switch" : name;
                s->PersistConfig("Device name saved");
                a.pushScreen(static_cast<int>(ScreenId::DevicePairing));
            }});
    } else {
        rows.push_back({"Device Name", {}, 0,
            [s]() {
                std::string name = s->config.deviceName;
                if (PromptText("Device Name", "Name shown on the server", name)) {
                    s->config.deviceName = name;
                    s->PersistConfig("Device name saved");
                }
            }});
        rows.push_back({"Save Backups",
                        {"No limit", "5", "10", "15"},
                        s->config.saveBackupLimit == 0 ? 0 :
                        s->config.saveBackupLimit == 5 ? 1 :
                        s->config.saveBackupLimit == 10 ? 2 : 3,
                        nullptr,
                        [s](int idx) {
                            static const int limits[4] = {0, 5, 10, 15};
                            s->config.saveBackupLimit = limits[idx];
                            s->PersistConfig("Save Backups: " +
                                std::string(s->config.saveBackupLimit <= 0 ? "No limit"
                                                                           : std::to_string(s->config.saveBackupLimit)));
                        }});
        rows.push_back({"Save Mapping", {}, 0,
            [&a]() { a.pushScreen(static_cast<int>(ScreenId::SaveMapping)); }});
        rows.push_back({"Sync Now", {}, 0,
            [s]() { s->SubmitServerSync("sync settings"); }});
    }
    BuildOptionsScreen(a, "Save Sync", rows,
        [&a]() { a.popScreen(); },
        {"B Back", "A Select"});
}

// ---- Save Mapping (save_mapping.go; tico layout is the mapping on Switch) ----

void BuildSaveMappingScreen(App& a) {
    auto* s = a.services;
    a.layout->Clear();
    a.title = "Save Sync Mappings";
    a.footerHints = {"B Back"};
    BuildStatusBar(a);
    BuildTitle(a, "Save Sync Mappings", 36);
    BuildFooter(a);
    AddText(a.layout,
            std::string("Saves map to the tico layout folders under ") + romm::effectiveSaveDir(s->config) + ".",
            kMargin, 210, kHint, kFontM, 1740);
    AddText(a.layout,
            std::string("RetroArch layouts use ") + (romm::parseOutputLayout(s->config.outputLayout) == romm::OutputLayout::RetroArch
                ? "retroarch/.retroarch/saves" : "the tico mapping") + ".",
            kMargin, 300, kHint, kFontS, 1740);
    a.layout->SetOnInput([&a](const u64 down, const u64, const u64, pu::ui::TouchPoint) {
        if (down & HidNpadButton_B) a.popScreen();
    });
    (void)s;
}

// ---- Synced Games (synced_games.go: platforms -> games -> file list) ----

void BuildSyncedGamesScreen(App& a) {
    auto* s = a.services;
    std::vector<SyncPlatformRow> plats;
    std::vector<SyncGameRow> games;
    std::string platformSlug;
    bool filesOpen = false;
    std::string gameName;
    {
        std::lock_guard<std::mutex> lock(s->status.mutex);
        plats = s->status.syncPlatforms;
        games = s->status.syncGames;
        platformSlug = s->status.syncPlatformSlug;
        filesOpen = s->status.syncGameFilesOpen;
        gameName = s->status.selectedSyncGameName;
    }

    if (platformSlug.empty()) {
        // Level 1: platform list.
        std::vector<std::string> items;
        for (const auto& p : plats) {
            items.push_back(p.name + " (" + std::to_string(p.saveCount + p.stateCount) + ")");
        }
        if (items.empty()) {
            a.layout->Clear();
            a.footerHints = {"X Rescan", "B Back"};
            BuildStatusBar(a);
            BuildTitle(a, "Synced Games", 36);
            BuildFooter(a);
            AddText(a.layout, "No synced games found.", kMargin, 210, kHint, kFontM);
            a.layout->SetOnInput([s, &a](const u64 down, const u64, const u64, pu::ui::TouchPoint) {
                if (down & HidNpadButton_B) a.popScreen();
                else if (down & HidNpadButton_X) s->SubmitDiscovery("synced games");
            });
            return;
        }
        auto menu = BuildListScreen(a, "Synced Games", items,
            nullptr,
            [&a]() { a.popScreen(); },
            {"A Select", "X Rescan", "B Back"});
        a.layout->SetOnInput([s, &a, plats, menu](const u64 down, const u64, const u64, pu::ui::TouchPoint) {
            if (down & HidNpadButton_B) {
                a.popScreen();
            } else if (down & HidNpadButton_X) {
                s->SubmitDiscovery("synced games");
            } else if (down & HidNpadButton_A) {
                s32 sel = menu->GetSelectedIndex();
                if (sel >= 0 && sel < static_cast<s32>(plats.size())) {
                    std::lock_guard<std::mutex> lock(s->status.mutex);
                    s->status.syncPlatformSlug = plats[static_cast<size_t>(sel)].slug;
                    s->status.syncPlatformName = plats[static_cast<size_t>(sel)].name;
                    s->status.selectedSyncRomIndex = 0;
                    s->status.selectedSaveGroupIndex = 0;
                    s->status.syncGameFilesOpen = false;
                    s->status.syncGameTypeStates = false;
                    s->status.selectedSyncGameKey.clear();
                    s->status.selectedSyncGameName.clear();
                    s->BumpUi();
                    // Rebuild in place: platform -> games level.
                    a.RebuildCurrent();
                }
            }
        });
        return;
    }

    if (games.empty() || !s->status.syncGames.empty()) {
        // Level 2: game list for the platform.
        std::vector<std::string> items;
        for (const auto& g : games) {
            items.push_back(g.name + " (" + std::to_string(g.fileCount) + ")");
        }
        auto menu = BuildListScreen(a, s->status.syncPlatformName.empty() ? platformSlug : s->status.syncPlatformName,
                                    items, nullptr,
                                    [&a]() { a.popScreen(); },
                                    {"A Select", "B Back"});
        a.layout->SetOnInput([s, &a, games, menu](const u64 down, const u64, const u64, pu::ui::TouchPoint) {
            if (down & HidNpadButton_B) {
                // Back: clear platform scope, stay on synced games root.
                {
                    std::lock_guard<std::mutex> lock(s->status.mutex);
                    s->status.syncPlatformSlug.clear();
                    s->status.syncPlatformName.clear();
                }
                a.RebuildCurrent();
            } else if (down & HidNpadButton_A) {
                s32 sel = menu->GetSelectedIndex();
                if (sel >= 0 && sel < static_cast<s32>(games.size())) {
                    {
                        std::lock_guard<std::mutex> lock(s->status.mutex);
                        s->status.selectedSyncGameKey = games[static_cast<size_t>(sel)].romId;
                        s->status.selectedSyncGameName = games[static_cast<size_t>(sel)].name;
                        s->status.syncGameFilesOpen = true;
                        s->status.syncGameTypeStates = false;
                        s->status.syncFilesForGame.clear();
                        for (size_t i = 0; i < s->status.syncFiles.size(); ++i) {
                            if (s->status.syncFiles[i].gameKey == games[static_cast<size_t>(sel)].romId &&
                                !s->status.syncFiles[i].isState) {
                                s->status.syncFilesForGame.push_back(static_cast<int>(i));
                            }
                        }
                        s->BumpUi();
                    }
                    a.RebuildCurrent();
                }
            }
        });
        return;
    }

    // Level 3: file list for the selected game.
    a.layout->Clear();
    a.footerHints = {"Y Toggle SAVES/STATES", "B Back"};
    BuildStatusBar(a);
    BuildTitle(a, gameName.empty() ? "Saves" : gameName, 36);
    BuildFooter(a);
    auto menu = MakeMenu(kMenuX, kMenuY, kMenuW, kMenuItemH, kMenuVisibleItems);
    {
        std::lock_guard<std::mutex> lock(s->status.mutex);
        for (int idx : s->status.syncFilesForGame) {
            if (idx < 0 || idx >= static_cast<int>(s->status.syncFiles.size())) continue;
            const auto& f = s->status.syncFiles[static_cast<size_t>(idx)];
            auto mi = pu::ui::elm::MenuItem::New(f.slotLabel + "  " + f.updatedAtIso);
            mi->SetColor(kHint);
            menu->AddItem(mi);
        }
    }
    a.layout->Add(menu);
    a.layout->SetOnInput([s, &a](const u64 down, const u64, const u64, pu::ui::TouchPoint) {
        if (down & HidNpadButton_B) {
            {
                std::lock_guard<std::mutex> lock(s->status.mutex);
                s->status.syncGameFilesOpen = false;
                s->status.selectedSyncGameKey.clear();
                s->status.selectedSyncGameName.clear();
            }
            a.RebuildCurrent();
        } else if (down & HidNpadButton_Y) {
            std::lock_guard<std::mutex> lock(s->status.mutex);
            s->status.syncGameTypeStates = !s->status.syncGameTypeStates;
            s->status.syncFilesForGame.clear();
            for (size_t i = 0; i < s->status.syncFiles.size(); ++i) {
                if (s->status.syncFiles[i].gameKey == s->status.selectedSyncGameKey &&
                    s->status.syncFiles[i].isState == s->status.syncGameTypeStates) {
                    s->status.syncFilesForGame.push_back(static_cast<int>(i));
                }
            }
            s->BumpUi();
        }
    });
    (void)filesOpen;
}

// ---- Sync History (sync_history.go) ----

void BuildSyncHistoryScreen(App& a) {
    auto* s = a.services;
    a.layout->Clear();
    a.footerHints = {"B Back"};
    BuildStatusBar(a);
    BuildTitle(a, "Sync History", 36);
    BuildFooter(a);
    {
        std::lock_guard<std::mutex> lock(s->status.mutex);
        AddText(a.layout,
                s->status.saveSyncStatusText.empty() ? "No sync has run yet."
                                                     : "Last sync: " + s->status.saveSyncStatusText,
                kMargin, 210, kHint, kFontM, 1740);
    }
    a.layout->SetOnInput([&a](const u64 down, const u64, const u64, pu::ui::TouchPoint) {
        if (down & HidNpadButton_B) a.popScreen();
    });
    (void)s;
}

// ---- BIOS Download (bios_download.go) ----

void BuildBiosDownloadScreen(App& a) {
    auto* s = a.services;
    Platform p;
    {
        std::lock_guard<std::mutex> lock(s->status.mutex);
        int sel = s->status.selectedPlatformIndex;
        if (sel >= 0 && sel < static_cast<int>(s->status.platforms.size())) {
            p = s->status.platforms[static_cast<size_t>(sel)];
        }
    }
    if (p.id.empty()) {
        a.popScreen();
        return;
    }

    a.layout->Clear();
    a.footerHints = {"A Download", "B Back"};
    BuildStatusBar(a);
    BuildTitle(a, p.name + " - BIOS", 36);
    BuildFooter(a);
    AddText(a.layout, "Checking BIOS files...", kMargin, 210, kAccent, kFontM);

    // Submit list request; the poll application enqueues the bundle.
    s->SubmitBiosList(p.id, p.slug, p.name);
    a.layout->SetOnInput([s, &a, p](const u64 down, const u64, const u64, pu::ui::TouchPoint) {
        if (down & HidNpadButton_B) {
            a.popScreen();
        } else if (down & HidNpadButton_A) {
            s->SubmitBiosList(p.id, p.slug, p.name);
            a.queueToast("Checking BIOS for " + p.name);
        }
    });
}

// ---- Dispatcher (Grout's router transitions.go) ----

void BuildCurrentScreen(App& a) {
    auto id = static_cast<ScreenId>(a.top().screenId);
    switch (id) {
        case ScreenId::ServerAuth: BuildServerAuthScreen(a); break;
        case ScreenId::DevicePairing: BuildDevicePairingScreen(a); break;
        case ScreenId::PlatformSelection: BuildPlatformSelectionScreen(a); break;
        case ScreenId::GameList: BuildGameListScreen(a); break;
        case ScreenId::GameFilters: BuildGameFiltersScreen(a); break;
        case ScreenId::GameDetails: BuildGameDetailsScreen(a); break;
        case ScreenId::GameOptions: BuildGameOptionsScreen(a); break;
        case ScreenId::DownloadManager: BuildDownloadManagerScreen(a); break;
        case ScreenId::BiosDownload: BuildBiosDownloadScreen(a); break;
        case ScreenId::Settings: BuildSettingsScreen(a); break;
        case ScreenId::GeneralSettings: BuildGeneralSettingsScreen(a); break;
        case ScreenId::CollectionsSettings: BuildCollectionsSettingsScreen(a); break;
        case ScreenId::PlatformMapping: BuildPlatformMappingScreen(a); break;
        case ScreenId::MappingFilters: BuildPlatformMappingScreen(a); break;
        case ScreenId::ToolsSettings: BuildToolsSettingsScreen(a); break;
        case ScreenId::AdvancedSettings: BuildAdvancedSettingsScreen(a); break;
        case ScreenId::Info: BuildInfoScreen(a); break;
        case ScreenId::LogoutConfirm: BuildInfoScreen(a); break;
        case ScreenId::UpdateCheck: BuildUpdateCheckScreen(a); break;
        case ScreenId::RebuildCache: BuildRebuildCacheScreen(a); break;
        case ScreenId::InputMapping: BuildSettingsScreen(a); break;
        case ScreenId::ArtworkSync: BuildRebuildCacheScreen(a); break;
        case ScreenId::SyncMenu: BuildSyncMenuScreen(a); break;
        case ScreenId::SaveSyncRun: BuildSaveSyncRunScreen(a); break;
        case ScreenId::SaveConflict: BuildSaveConflictScreen(a); break;
        case ScreenId::SaveSyncSettings: BuildSaveSyncSettingsScreen(a); break;
        case ScreenId::SaveMapping: BuildSaveMappingScreen(a); break;
        case ScreenId::SyncedGames: BuildSyncedGamesScreen(a); break;
        case ScreenId::SyncHistory: BuildSyncHistoryScreen(a); break;
    }
}

} // namespace romm::ui
