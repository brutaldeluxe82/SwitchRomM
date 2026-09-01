#pragma once

// screens.hpp — Grout-parity screen registry.
// One builder per Grout view; all driven by App + Services (Status/Config).

#include "app_shell.hpp"

namespace romm::ui {

// Screen IDs (Grout's app/screens.go equivalent).
enum class Screen {
    LanguageSelection,   // (scaffold; no language data on Switch yet)
    DevicePairing,
    PlatformSelection,   // main menu: Collections + platforms
    CollectionList,
    CollectionPlatforms,
    GameList,
    GameFilters,
    GameDetails,
    GameOptions,
    Search,
    DownloadManager,
    BiosDownload,
    Settings,
    GeneralSettings,
    CollectionsSettings,
    PlatformMapping,
    MappingFilters,
    ToolsSettings,
    AdvancedSettings,
    Info,
    LogoutConfirm,
    UpdateCheck,
    RebuildCache,
    ArtworkSync,
    InputMapping,
    SyncMenu,
    SaveSyncRun,
    SaveConflict,
    SaveSyncSettings,
    SaveMapping,
    SyncedGames,
    SyncHistory,
    Count
};

// Build the screen for the current stack top into App's layout.
void BuildCurrentScreen(App& a);

// Individual builders (used by BuildCurrentScreen's dispatch).
void BuildServerAuthScreen(App& a);
void BuildDevicePairingScreen(App& a);
void BuildPlatformSelectionScreen(App& a);
void BuildGameListScreen(App& a);
void BuildGameFiltersScreen(App& a);
void BuildGameDetailsScreen(App& a);
void BuildGameOptionsScreen(App& a);
void BuildSettingsScreen(App& a);
void BuildGeneralSettingsScreen(App& a);
void BuildCollectionsSettingsScreen(App& a);
void BuildPlatformMappingScreen(App& a);
void BuildToolsSettingsScreen(App& a);
void BuildAdvancedSettingsScreen(App& a);
void BuildInfoScreen(App& a);
void BuildUpdateCheckScreen(App& a);
void BuildRebuildCacheScreen(App& a);
void BuildBiosDownloadScreen(App& a);
void BuildSyncMenuScreen(App& a);
void BuildSaveSyncSettingsScreen(App& a);
void BuildSaveMappingScreen(App& a);
void BuildSyncedGamesScreen(App& a);
void BuildSyncHistoryScreen(App& a);
void BuildSaveConflictScreen(App& a);
void BuildDownloadManagerScreen(App& a);

} // namespace romm::ui
