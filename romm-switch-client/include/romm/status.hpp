#pragma once

#include "romm/models.hpp"
#include "romm/errors.hpp"
#include "romm/platform_prefs.hpp"
#include "romm/planner.hpp"
#include <atomic>
#include <string>
#include <vector>
#include <mutex>
#include <utility>

namespace romm {

enum class QueueState { Pending, Downloading, Finalizing, Completed, Resumable, Failed, Cancelled };
enum class RomFilter { All, Queued, Resumable, Failed, Completed, NotQueued };
enum class RomSort { TitleAsc, TitleDesc, SizeDesc, SizeAsc };
enum class SavePairState { Idle, Initiating, AwaitingApproval, Paired, Error };
enum class WorkerEventType { DownloadFailureState, DownloadCompletion };

struct QueueItem {
    Game game;
    DownloadBundle bundle;
    QueueState state{QueueState::Pending};
    std::string error;

    QueueItem() = default;
    QueueItem(const Game& g, QueueState s, const std::string& errStr = std::string())
        : game(g), state(s), error(errStr) {}
};

struct WorkerEvent {
    WorkerEventType type{WorkerEventType::DownloadFailureState};
    bool failed{false};
    std::string message;
};

struct SyncPlatformRow {
    std::string slug;      // canonical slug hint derived from the on-disk folder
    std::string name;      // display name (server platform name, else folder)
    int saveCount{0};      // battery saves found under this platform
    int stateCount{0};     // save states found under this platform
};

struct SyncGameRow {
    std::string romId;          // key: "rid:<id>" when matched to a server ROM, else "f:<basename>"
    long long displayRomId{0};  // 0 = no known server ROM (local-only)
    std::string name;           // game display name (server title, else basename)
    int fileCount{0};           // saves + states in this game's group
    int saveCount{0};           // files under the SAVES group
    int stateCount{0};          // files under the STATES group
};

struct SyncFileRow {
    std::string fileName;       // full on-disk path (kept for future passes)
    std::string gameKey;        // grouping key, matches SyncGameRow::romId
    bool isState{false};
    std::string slotLabel;      // "State 0" / "Auto" / "Base" / "Save"
    std::string updatedAtIso;   // local mtime, formatIso8601Utc shape
};

struct Status {
    // TODO(thread-safety): use this mutex (or an event queue) to guard shared state between UI and worker.
    mutable std::mutex mutex;

    bool validHost{false};
    bool validCredentials{false};

    // Current UI/view state
    enum class View { PLATFORMS, ROMS, DETAIL, QUEUE, ERROR, DOWNLOADING, DIAGNOSTICS, UPDATER, SETTINGS, LOGIN, SYNCROMS, GAMESAVES } currentView{View::PLATFORMS};

    // Data loaded from API
    std::vector<Platform> platforms;
    std::vector<Game> roms;      // active (filtered/sorted) list used by UI
    std::vector<Game> romsAll;   // master list fetched from server for indexing
    uint64_t romsRevision{0}; // bump when `roms` changes to let UI caches avoid O(N) per-frame rebuilds
    uint64_t romsAllRevision{0};
    std::string romSearchQuery;
    RomFilter romFilter{RomFilter::All};
    RomSort romSort{RomSort::TitleAsc};
    uint64_t romListOptionsRevision{0};
    PlatformPrefs platformPrefs;

    // PLATFORMS is a tri-mode index: ROM (default), BIOS, and SYNC. ZR/ZL cycle modes.
    enum class PlatformIndexMode { Rom, Bios, Sync };
    PlatformIndexMode platformIndexMode{PlatformIndexMode::Rom};
    int selectedPlatformIndex{0};
    int selectedBiosPlatformIndex{0}; // independent selection for the BIOS index mode
    int selectedSyncPlatformIndex{0}; // independent selection for the SYNC index mode
    int selectedRomIndex{0};
    int selectedQueueIndex{0};
    bool queueReorderActive{false}; // when true in QUEUE: D-pad reorders the selected item instead of moving cursor
    // Selected platform identity (used to keep UI and behavior correlated even if indices drift).
    std::string currentPlatformId;
    std::string currentPlatformSlug;
    std::string currentPlatformName;
    std::vector<View> navStack; // simple stack for PLATFORMS -> ROMS -> DETAIL navigation
    View prevQueueView{View::ROMS}; // where to return when leaving queue
    View prevDiagnosticsView{View::PLATFORMS}; // where to return when leaving diagnostics
    View prevUpdaterView{View::PLATFORMS}; // where to return when leaving updater
    View prevSyncRomsView{View::PLATFORMS}; // where SYNCROMS returns when leaving
    View prevSyncGamesView{View::PLATFORMS}; // where GAMESAVES returns when leaving
    View prevSettingsView{View::PLATFORMS}; // where to return when leaving settings
    View prevLoginView{View::PLATFORMS};    // where to return after pairing/login

    // Download queue and progress
    std::vector<QueueItem> downloadQueue;
    std::vector<QueueItem> downloadHistory; // completed/failed items for UI display
    uint64_t downloadQueueRevision{0};   // bump when queue contents or states change
    uint64_t downloadHistoryRevision{0}; // bump when history changes
    std::atomic<size_t> currentDownloadIndex{0};
    std::atomic<size_t> currentDownloadFileCount{0};
    std::atomic<uint64_t> currentDownloadSize{0};
    std::atomic<uint64_t> currentDownloadedBytes{0};
    std::atomic<uint64_t> totalDownloadBytes{0};
    std::atomic<uint64_t> totalDownloadedBytes{0};
    std::string currentDownloadTitle;
    double lastSpeedMBps{0.0}; // last measured throughput in MB/s, updated by worker
    std::atomic<bool> downloadWorkerRunning{false};
    std::atomic<bool> lastDownloadFailed{false};
    std::string lastDownloadError;
    bool downloadCompleted{false};
    bool burnInMode{false}; // burn-in prevention "black screen" overlay for DOWNLOADING

    // Async flags (unused/legacy)
    std::atomic<bool> platformsReady{false};
    std::atomic<bool> romsReady{false};
    std::atomic<bool> downloadInProgress{false};

    // Network/IO busy indicator for UI throbber.
    std::atomic<bool> netBusy{false};
    std::atomic<uint32_t> netBusySinceMs{0};
    std::string netBusyWhat;
    uint64_t romFetchGeneration{0}; // increments to cancel/ignore stale ROM fetches

    std::string lastError;
    ErrorInfo lastErrorInfo{};

    // Worker -> UI event channel for UI-facing status fields.
    std::vector<WorkerEvent> workerEvents;
    uint64_t workerEventsRevision{0};

    // Diagnostics probe state.
    bool diagnosticsServerReachableKnown{false};
    bool diagnosticsServerReachable{false};
    bool diagnosticsProbeInFlight{false};
    uint64_t diagnosticsProbeGeneration{0};
    uint32_t diagnosticsLastProbeMs{0};
    std::string diagnosticsLastProbeDetail;


    // Save-sync (device-auth pairing + sync job) state (DIAGNOSTICS view).
    // Non-atomic fields are guarded by Status::mutex.
    SavePairState savePairState{SavePairState::Idle};
    std::string savePairUserCode;   // shown while awaiting approval
    std::string savePairDetail;     // error detail / verification URL hint
    std::string saveVerificationPath; // verification_path from init (or "/pair")
    std::string saveStatusText;     // last sync result summary
    std::atomic<bool> saveBusy{false};
    bool saveServerDeviceAuthKnown{false};
    bool saveServerDeviceAuthSupported{false};
    std::string saveServerVersion;  // from heartbeat probe

    // ---- Local save/state discovery (SYNC index, SYNCROMS, GAMESAVES) ----
    std::vector<SyncFileRow> syncFiles;         // every discovered file (flat)
    std::vector<int> syncFilesForGame;          // indices into syncFiles for the open game+type filter
    std::vector<SyncPlatformRow> syncPlatforms; // one row per on-disk platform folder
    uint64_t syncPlanRevision{0};               // bump on each completed local scan
    std::string syncStatusText;                 // status line for the discovery UI
    int selectedSyncRomIndex{0};                // cursor inside SYNCROMS list
    int selectedSaveGroupIndex{0};              // cursor inside GAMESAVES SAVES/STATES rows
    std::string syncPlatformSlug;               // scope of the open SYNCROMS view
    std::string syncPlatformName;
    std::vector<SyncGameRow> syncGames;         // per-game rows under the open platform
    std::string selectedSyncGameKey;            // key of the drilled-into game
    std::string selectedSyncGameName;           // title for the GAMESAVES header
    bool syncGameFilesOpen{false};              // GAMESAVES: false=group rows, true=file rows under the chosen group
    bool syncGameTypeStates{false};             // GAMESAVES group selection: false=SAVES, true=STATES

    // Settings view (Plus): selected row in the settings menu.
    int selectedSettingsIndex{0};
    std::string settingsStatus; // inline feedback line (save errors, sign-out, rejections)

    std::string pairedDeviceId;   // set when pairing completes; persisted via device_token.json
    std::atomic<bool> saveSyncRunBusy{false};
    std::string saveSyncStatusText; // latest orchestrated-sync run summary or error

    // Updater state (GitHub latest release check + staged .nro download)
    bool updateCheckInFlight{false};
    bool updateChecked{false};
    bool updateAvailable{false};
    std::string updateLatestTag;
    std::string updateLatestName;
    std::string updateLatestPublishedAt;
    std::string updateReleaseHtmlUrl;
    std::string updateAssetName;
    std::string updateAssetUrl;
    uint64_t updateAssetSizeBytes{0};
    bool updateDownloadInFlight{false};
    bool updateDownloaded{false};
    std::string updateStagedPath;
    std::string updateStatus;
    std::string updateError;
};

// Helper to run a callable while holding the status mutex, returning its result.
// Keeps locking policy consistent across UI and worker paths.
template <typename F>
auto withStatusLock(Status& st, F&& fn) -> decltype(fn()) {
    std::lock_guard<std::mutex> lock(st.mutex);
    return fn();
}

inline void postWorkerEvent(Status& st, WorkerEvent ev) {
    std::lock_guard<std::mutex> lock(st.mutex);
    st.workerEvents.push_back(std::move(ev));
    st.workerEventsRevision++;
}

} // namespace romm
