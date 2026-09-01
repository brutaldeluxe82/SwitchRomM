#pragma once

// services.hpp — RomServices: the data/async core shared by all screens.
// Ported from the old main.cpp worker/poll logic (LatestJobWorker instances,
// platform ROM cache, paged fetch, remote search, pairing, discovery,
// orchestrated sync, update check/download). UI reads Status snapshots and
// navigates via navHook callbacks.

#include "romm/api.hpp"
#include "romm/config.hpp"
#include "romm/status.hpp"
#include "romm/job_manager.hpp"
#include "romm/cover_loader.hpp"
#include "romm/update.hpp"
#include "romm/save_sync.hpp"
#include "romm/models.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace romm::ui {

enum class NavOp { Push, Pop, ReplaceAll };
enum class ScreenId {
    ServerAuth,          // Grout login.go: server + auth, device-code flow
    DevicePairing, PlatformSelection, GameList,
    GameFilters, GameDetails, GameOptions, DownloadManager, BiosDownload,
    Settings, GeneralSettings, CollectionsSettings, PlatformMapping,
    MappingFilters, ToolsSettings, AdvancedSettings, Info,
    LogoutConfirm, UpdateCheck, RebuildCache, ArtworkSync, InputMapping,
    SyncMenu, SaveSyncRun, SaveConflict, SaveSyncSettings, SaveMapping,
    SyncedGames, SyncHistory
};

struct RomServices {
    Config config;
    Status status;

    // UI integration
    std::function<void(ScreenId, NavOp)> navHook;       // navigation requests
    std::function<void(const std::string&)> toastHook;  // transient toasts
    std::atomic<uint64_t> uiRevision{0};                // bump when any screen-visible data changes
    std::atomic<bool> quitRequested{false};

    // --- Cover cache (decoded RGBA; UI converts to textures) ---
    CoverLoader coverLoader;
    CoverResult currentCover;
    std::string currentCoverKey; // url of currentCover
    std::string requestedCoverUrl;

    // --- Worker job/request types (ported from old main.cpp) ---
    struct PendingRomFetch {
        enum class Mode { Probe, Page } mode{Mode::Page};
        std::string pid;
        std::string slug;
        std::string name;
        std::string cachedIdentifierDigest;
        size_t offset{0};
        size_t limit{250};
        uint64_t generation{0};
    };
    struct RomFetchResult {
        PendingRomFetch req;
        bool ok{false};
        std::vector<Game> games;
        size_t offset{0};
        size_t limit{0};
        bool hasMore{false};
        size_t nextOffset{0};
        size_t total{0};
        bool totalKnown{false};
        bool probeOnly{false};
        bool probeUnchanged{false};
        bool probeFailed{false};
        std::string identifierDigest;
        std::string error;
        ErrorInfo errorInfo{};
    };
    struct PendingRemoteSearch {
        std::string pid;
        std::string query;
        size_t limit{250};
        uint64_t generation{0};
    };
    struct RemoteSearchResult {
        PendingRemoteSearch req;
        bool ok{false};
        std::vector<Game> games;
        std::string error;
        ErrorInfo errorInfo{};
    };
    struct DiagProbeReq { uint64_t generation{0}; };
    struct DiagProbeResult {
        uint64_t generation{0};
        bool ok{false};
        std::string detail;
        ErrorInfo errorInfo{};
    };
    struct UpdateCheckReq { uint64_t generation{0}; };
    struct UpdateCheckResult {
        uint64_t generation{0};
        bool ok{false};
        GitHubRelease release;
        GitHubAsset asset;
        bool updateAvailable{false};
        std::string error;
        ErrorInfo errorInfo{};
    };
    struct UpdateDownloadReq {
        uint64_t generation{0};
        std::string url;
        std::string outPath;
        uint64_t expectedSizeBytes{0};
    };
    struct UpdateDownloadResult {
        uint64_t generation{0};
        bool ok{false};
        std::string outPath;
        uint64_t bytes{0};
        std::string error;
        ErrorInfo errorInfo{};
    };
    struct BiosListReq {
        uint64_t generation{0};
        std::string platformId;
        std::string platformSlug;
        std::string platformName;
    };
    struct BiosListResult {
        uint64_t generation{0};
        std::string platformSlug;
        std::string platformName;
        std::vector<Firmware> files;
        std::string error;
    };
    struct PairingReq {
        uint64_t generation{0};
        std::string serverUrl;
        std::string basicAuthRaw;
        std::string clientDeviceId;
        int timeoutSeconds{0};
    };
    struct PairingResultMsg {
        uint64_t generation{0};
        bool paired{false};
        std::string statusText;
    };
    struct DiscFile {
        std::string path;
        bool isState{false};
        std::string baseLower;
        long long romId{0};
        long long mtime{0};
        std::string updatedAtIso;
        std::vector<std::string> folderSlugs;
        std::string gameKey;
        std::string gameName;
        std::string slotLabel;
    };
    struct DiscoveryReq {
        uint64_t generation{0};
        std::string savesRoot;
        std::string statesRoot;
        std::vector<RomMatchEntry> roms;
        std::map<long long, std::string> romNameById;
    };
    struct DiscoveryResult {
        uint64_t generation{0};
        int unmatchedCount{0};
        std::vector<DiscFile> files;
    };
    struct SyncRunReq {
        uint64_t generation{0};
        std::string deviceId;
    };
    struct SyncRunResult {
        uint64_t generation{0};
        bool ok{false};
        std::string error;
        int uploaded{0};
        int downloaded{0};
        int conflicts{0};
        int failed{0};
        int suppressed{0};
        int unmatched{0};
        int skippedAbsent{0};
    };

    // Workers
    LatestJobWorker<PendingRomFetch, RomFetchResult> romFetchJobs;
    LatestJobWorker<PendingRemoteSearch, RemoteSearchResult> remoteSearchJobs;
    LatestJobWorker<DiagProbeReq, DiagProbeResult> diagProbeJobs;
    LatestJobWorker<UpdateCheckReq, UpdateCheckResult> updateCheckJobs;
    LatestJobWorker<UpdateDownloadReq, UpdateDownloadResult> updateDownloadJobs;
    LatestJobWorker<BiosListReq, BiosListResult> biosListJobs;
    LatestJobWorker<PairingReq, PairingResultMsg> saveSyncJobs;
    LatestJobWorker<DiscoveryReq, DiscoveryResult> discoveryJobs;
    LatestJobWorker<SyncRunReq, SyncRunResult> orchestratedSyncJobs;

    // Generation counters / flags
    uint64_t remoteSearchGeneration = 0;
    bool remoteSearchInFlight = false;
    std::vector<Game> remoteSearchGames;
    bool remoteSearchActive = false;
    std::string remoteSearchQuery;
    std::string remoteSearchPlatformId;
    uint64_t remoteSearchRevision = 0;
    uint64_t biosListGeneration = 0;
    uint64_t discoveryGeneration = 0;
    DiscoveryResult lastDiscovery;
    uint64_t saveSyncGeneration = 0;
    uint64_t loginPairGeneration = 0;   // increments per SubmitLoginPairing
    uint64_t adoptedPairGeneration = 0; // PollJobs one-shot latch (paired toast)
    std::atomic<bool> saveCancel{false};
    uint64_t updateGeneration = 0;
    uint64_t updateCheckGenSubmitted = 0;
    uint64_t updateDownloadGenSubmitted = 0;
    uint64_t syncRunGeneration = 0;

    // Platform ROM cache (ported)
    struct CachedPlatformRoms {
        std::vector<Game> games;
        std::string slug;
        std::string name;
        std::string identifierDigest;
        uint32_t fetchedAtMs{0};
    };
    std::unordered_map<std::string, CachedPlatformRoms> platformRomsCache;
    uint32_t currentPlatformFetchedAtMs = 0;
    std::string currentPlatformIdentifierDigest;
    size_t pagedFetchNextOffset = 0;
    size_t pagedFetchPageLimit = 500;

    // Derived library helpers (ported)
    std::vector<Game> visibleRomsSnapshot; // mirror of status.roms for screens
    uint64_t appliedRomsAllRev = 0;
    uint64_t appliedRomsOptionsRev = 0;
    uint64_t appliedQueueRevForRoms = 0;
    uint64_t appliedHistRevForRoms = 0;

    // Live manager refresh: main sets this each frame to the visible
    // ScreenId (from the shell stack) so PollJobs can bump the UI at a
    // throttled rate while a download is running on the manager.
    std::atomic<int> uiVisibleScreen{-1};
    uint32_t lastManagerProgressBumpMs = 0;
    // One-shot: PollJobs sets this when a login pairing completes and the
    // token is adopted; the render callback consumes it (3s success banner,
    // then unwind to PlatformSelection + platform refetch).
    std::atomic<bool> pairSuccessPending{false};

    // ---- API for screens (thread-safe) ----
    // Start everything (workers, cover loader). Call once at boot.
    void Start();
    void Stop();

    // Poll all workers; apply results to Status; fire nav/toast hooks.
    // Call every frame from the UI thread.
    void PollJobs();

    // Rebuild status.roms from romsAll/remote search + filter/sort/search.
    void RebuildVisibleRoms(bool resetSelection);
    void RebuildVisibleRomsLocked(bool resetSelection); // caller holds status.mutex
    // Submissions (called from screens).
    void SubmitRomFetch(const std::string& platformId, const std::string& slug,
                        const std::string& name, bool startNewGeneration);
    void SubmitRemoteSearch(const std::string& platformId, const std::string& query);
    void SubmitDiagnosticsProbe();
    void SubmitBiosList(const std::string& id, const std::string& slug, const std::string& name);
    void SubmitLoginPairing();
    bool EnqueueGame(const Game& g);
    int EnqueueGamesBulk(const std::vector<Game>& games); // multi-select queueing
    void SubmitDiscovery(const char* why);
    void SubmitServerSync(const char* why);
    void SubmitUpdateCheck();
    void SubmitUpdateDownload();

    // Apply search/filter/sort changes and bump revisions.
    void SetRomSearchQuery(const std::string& q);
    void CycleRomFilter(int dir);
    void CycleRomSort(int dir);

    // Settings persistence (bumps uiRevision on save).
    bool PersistConfig(const std::string& okMsg);

    // "host[:port][/path]" -> "https://host[:port][/path]" (default scheme
    // https; Grout/RomM are TLS-first). Full URLs pass through unchanged.
    static std::string NormalizeServerUrl(std::string url);

    // Drop stale library state and re-fetch platforms in the background
    // (called after server/credential changes so the UI refreshes without
    // an app restart).
    void RefetchPlatforms();
    // Persist queue snapshot.
    void PersistQueueState();

    // Compute pending download total (ported recomputeTotals).
    void RecomputeTotals();

    // Queue an entire platform's ROM library (multi-select on platforms).
    void QueuePlatformBulk(const Platform& p);
    // Cover requests.
    void RequestCover(const std::string& url, const std::string& title);
    // Poll cover loader; stores currentCover when ready.
    void PollCover();

    // Nav helpers (call navHook).
    void Nav(ScreenId id, NavOp op);
    void BumpUi() { uiRevision.fetch_add(1); }


    // Drop platforms without a tico emulator/core (config.hideUnsupportedPlatforms).
    // Called after platform fetches; safe to call repeatedly.
    void ApplyPlatformFilter();
    static std::string NormalizeSearchText(const std::string& in);
};

// Ticks since start (ms) — SDL_GetTicks replacement for ported logic.
uint32_t TicksMs();

} // namespace romm::ui
