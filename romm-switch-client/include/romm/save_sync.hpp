#pragma once

#include <functional>
#include <string>
#include <vector>

namespace romm {

// ---------- Device token persistence ----------

struct DeviceToken {
    std::string accessToken;
    std::string deviceId;
    std::string clientDeviceIdentifier;
};

// Parse a device-token JSON object. Tolerant of missing fields; returns false
// when access_token is missing/empty (an unusable token).
bool parseDeviceTokenJson(const std::string& json, DeviceToken& out);

// Serialize a device token to a compact JSON object.
std::string serializeDeviceTokenJson(const DeviceToken& t);

constexpr const char* kDeviceTokenPath = "sdmc:/switch/romm_switch_client/device_token.json";

// Load/save a device token at an explicit path (fopen-based, like self_update).
bool loadDeviceToken(const std::string& path, DeviceToken& out);
bool saveDeviceToken(const std::string& path, const DeviceToken& t, std::string& err);

// ---------- Device-auth pairing (phase 1: init + poll classification) ----------

// Poll classification from POST /api/auth/device/token response.
enum class DeviceAuthPollState {
    Approved,
    Pending,
    SlowDown,
    AccessDenied,
    ExpiredToken,
    Error,
};

struct DeviceAuthPollResult {
    DeviceAuthPollState state{DeviceAuthPollState::Error};
    std::string accessToken;
    std::string deviceId;
    std::string detail;
};

// status = HTTP status code; body = raw response body.
// 200 + parse OK -> Approved (extract access_token, device_id).
// 400 with detail string: authorization_pending->Pending, slow_down->SlowDown,
// access_denied->AccessDenied, expired_token->ExpiredToken, anything else->Error.
DeviceAuthPollResult classifyDeviceTokenResponse(long httpStatus, const std::string& body);

struct DeviceAuthInitRequest {
    std::string name;
    std::string client;
    std::string clientVersion;
    std::string clientDeviceIdentifier;
};

// Serializes init request body incl. the requested_scopes array.
std::string serializeDeviceAuthInitBody(const DeviceAuthInitRequest& r);

struct DeviceAuthInitResponse {
    std::string deviceCode;
    std::string userCode;
    std::string verificationPath;
    std::string verificationUrlComplete; // "/pair/device?user_code=…" ("" when absent)
    bool verificationPathComplete{false};
    long expiresIn{0};
    long interval{5};
};

// Parses init response {device_code, user_code, verification_path,
// verification_path_complete, expires_in, interval}. Returns false unless
// device_code is present.
bool parseDeviceAuthInitResponse(const std::string& json, DeviceAuthInitResponse& out);

// Pure version gate mirroring grout HeartbeatResponse.SupportsDeviceAuth:
// strip leading 'v', take text before first '.', strip '-suffix', atoi >= 5.
// Unparsable => false.
bool serverSupportsDeviceAuth(const std::string& systemVersion);

// ---------- Save / state sync engine (phase 2) ----------

// True if lowerExt (includes the leading dot) is a recognised battery-save
// extension: [".srm" ".sav" ".dsv" ".mcr" ".mcd" ".brm" ".eep" ".sra"
//  ".fla" ".mpk" ".nv"].
bool isValidBatterySaveExtension(const std::string& lowerExt); // lowerExt includes dot

enum class StateKind { None, Base, Auto, Numbered };

// Classify a save-state file name (case-insensitive):
//   "game.state"        -> Base
//   "game.state.auto"   -> Auto
//   "game.state.<digits>"-> Numbered
//   anything else       -> None
StateKind classifyStateFileName(const std::string& fileName);

// Format a Unix epoch (seconds) as "YYYY-MM-DDTHH:MM:SSZ" (UTC) via gmtime+snprintf.
std::string formatIso8601Utc(long long epochSeconds);

struct LocalAsset {
    int romId;                    // 0 = unmatched
    std::string fileName;         // on-disk file name
    std::string path;
    unsigned long long sizeBytes;
    long long mtimeEpoch;
    std::string updatedAtIso;
    std::string contentHash;
    std::string emulator;         // core dir name when nested, else ""
    bool isState;                 // true=save state, false=battery save
    std::string slot;             // "autosave" for battery saves; "" for states
};

// Given a file name with its SAVE extension already stripped, apply the grout
// double-extension-strip rule: if the base itself ends in something that looks
// like a ROM extension (a dot followed by 2..5 alphanumerics), strip it again.
// e.g. "game.gba" -> "game"; "Zelda (USA)" -> "Zelda (USA)"; "game.v1)" unchanged.
std::string saveLookupBase(const std::string& fileNameNoExt);

// A single server ROM usable for matching scanned saves. fsNameLower is the
// lowercased ROM file name (including its extension); slugLower the lowercased
// platform slug ("" when unknown). romId is the numeric server id.
struct RomMatchEntry {
    long long romId{0};
    std::string fsNameLower;
    std::string slugLower;
};

// Look up the best romId for a scanned save given its lookup base (as produced
// by scanAssets: lowercased, double-extension-stripped) and an optional slug
// hint. Compares against each ROM's own lookup base (saveLookupBase applied to
// fsNameLower). When slugHint is non-empty a same-slug match is preferred.
// Returns 0 when nothing matches.
long long romIdForMatch(const std::vector<RomMatchEntry>& roms,
                        const std::string& baseLower, const std::string& slugHint);

// Decide a save-state sync action given whether a local asset exists and the
// remote's metadata. Returns "upload" | "download" | "no_op" | "skip".
//   no local + remote present            -> "download"
//   local present + remote absent        -> "upload"
//   local newer                          -> "upload"
//   remote newer                         -> "download"
//   equal timestamps + same content hash -> "no_op"
//   equal timestamps + different hash    -> "skip"
//   neither local nor remote             -> "skip"
std::string decideStateOperation(const LocalAsset* local /*null=no local*/,
                                 const std::string& remoteUpdatedAt,
                                 const std::string& remoteContentHash,
                                 const std::string& computedLocalHash);

// ---------- Scanning ----------

struct ScanResult {
    std::vector<LocalAsset> assets;
    std::vector<std::string> unmatched; // valid save file names with no matching ROM (romId 0)
};

using RomMatcherFn = std::function<int(const std::string& baseLower, const std::string& slugHint)>;
using EmulatorOfFn = std::function<std::string(const std::string& path)>;
// Resolves the ROM-matching slug hint for one scanned asset: (path, isState).
// Empty return = no hint. Used to scope matching like Grout scopes it to the
// platform folder: a save under saves/<folder>/ prefers ROMs of that platform.
using SlugHintFn = std::function<std::string(const std::string& path, bool isState)>;

// Walk savesRoot (battery saves) and statesRoot (save states) recursively
// (depth <= 3). Matched files are hashed (md5) and stat'd during the scan.
// Missing roots are fine (empty result). err is set only on real failures.
// slugHintOf scopes each file's match to a platform slug when non-empty
// (Grout-style); "" keeps the legacy global match.
ScanResult scanAssets(const std::string& savesRoot, const std::string& statesRoot,
                      const RomMatcherFn& romMatcher, const EmulatorOfFn& emulatorOf,
                      const SlugHintFn& slugHintOf, std::string& err);

// One ROM found on disk under the tico ROMs root.
struct DiskRom {
    std::string platformFolder;  // top-level folder under romsRoot (e.g. "snes")
    std::string subFolder;       // optional game subfolder ("" for root files)
    std::string baseLower;       // saveLookupBase(fileName), lowercased
};

// Walk romsRoot (e.g. sdmc:/tico/roms) one level of platform folders plus one
// optional game subfolder (depth <= 2). For each regular file, record its
// platform folder, game subfolder, and lookup base. Missing root is fine.
std::vector<DiskRom> scanDiskRoms(const std::string& romsRoot);

// stat + md5 hash of a file in one pass. Returns false (with *no* guarantee
// about outputs) if the file can't be opened/read.
bool computeFileMd5AndStat(const std::string& path, unsigned long long& sizeOut,
                           long long& mtimeOut, std::string& hashOut);
// Human-facing row labels for the local save/state browser:
//   "State 0".."State 9" (bare or dotted slots), "Auto" (.auto),
//   "Base" (plain .state), "Save" (battery-save extensions), "" otherwise.
std::string saveSlotLabel(const std::string& fileName);

// "2025-08-20T14:03:27Z" -> "2025-08-20 14:03" (formatIso8601Utc shape);
// anything shorter/other passes through unchanged.
std::string trimIsoToMinutes(const std::string& iso);


// ---------- DTO parse / serialize ----------

struct RemoteAsset {
    long long id{0};
    long long romId{0};
    std::string fileName;
    unsigned long long fileSizeBytes{0};
    std::string updatedAt;
    std::string contentHash; // "" unless the server list includes content_hash
    std::string emulator;
    std::string slot; // "" = null
};

std::vector<RemoteAsset> parseSavesArray(const std::string& json);
std::vector<RemoteAsset> parseStatesArray(const std::string& json);

// ---------- Client-side reconcile (plan builder + policy) ----------

enum class SyncPolicy { AskEveryTime, NewestWins, ServerWins, ClientWins };

// Parse "ask" | "newest" | "server" | "client" (case-insensitive, trimmed).
// Anything else -> AskEveryTime.
SyncPolicy parseSyncPolicy(const std::string& s);

// Canonical lowercase name: "ask" | "newest" | "server" | "client".
const char* syncPolicyName(SyncPolicy p);

enum class SyncPlanAction { Upload, Download, NoOp };

// One pairable save/state slot: a local file and/or its remote counterpart.
struct SyncPlanItem {
    const LocalAsset* local{nullptr}; // null = remote-only (download candidate)
    bool hasRemote{false};
    RemoteAsset remote;               // valid when hasRemote
    bool isState{false};
    SyncPlanAction newestAction{SyncPlanAction::NoOp}; // policy-independent baseline
    bool conflict{false};             // equal timestamps, different content hash
};

// Pair local assets with remote saves+states on (romId, fileName).
// Locals with romId==0 are skipped; remote-only items appear only when their
// romId is in knownRomIds (roms with >=1 matched local asset plus every rom
// id in the caller's rom snapshot). Order: locals in scan order, then
// remote-only items in fetch order.
std::vector<SyncPlanItem> buildSyncPlan(const std::vector<LocalAsset>& locals,
                                        const std::vector<RemoteAsset>& remoteSaves,
                                        const std::vector<RemoteAsset>& remoteStates,
                                        const std::vector<long long>& knownRomIds);

// True when the item may change either side: baseline action != NoOp or an
// unresolved conflict exists.
bool syncPlanItemActionable(const SyncPlanItem& it);

// True when a winner must be chosen interactively: policy is AskEveryTime,
// both sides exist, and they differ (newer on either side or conflict).
bool syncPlanNeedsChoice(const SyncPlanItem& it, SyncPolicy policy);

// Resolve the final action under a policy:
//   NewestWins   -> newestAction (conflict stays NoOp: winner unknowable)
//   ServerWins   -> Download when hasRemote, else NoOp (never deletes local)
//   ClientWins   -> Upload when local exists, else NoOp
//   AskEveryTime -> newestAction for single-sided items, NoOp for two-sided
//                   (worker surfaces those via syncPlanNeedsChoice)
SyncPlanAction resolveSyncAction(const SyncPlanItem& it, SyncPolicy policy);


// Serialize the negotiate request body:
// {"device_id":..., "saves":[{rom_id,file_name,slot,emulator,content_hash,updated_at,file_size_bytes},...]}
std::string serializeNegotiatePayload(const std::string& deviceId,
                                      const std::vector<LocalAsset>& saves);

struct SyncOperation {
    std::string action;        // "upload" | "download" | "conflict" | "no_op"
    long long romId{0};
    long long assetId{0};
    bool hasAssetId{false};
    std::string fileName;
    std::string slot;
    std::string emulator;
    std::string reason;
    std::string serverUpdatedAt;
    std::string serverContentHash;
};

struct NegotiateResponse {
    long long sessionId{0};
    std::vector<SyncOperation> operations;
    int totalUpload{0};
    int totalDownload{0};
    int totalConflict{0};
    int totalNoOp{0};
};

// Parse the negotiate response. Returns false on malformed JSON / missing session_id.
bool parseNegotiateResponse(const std::string& json, NegotiateResponse& out);

// Serialize the session-complete body: {"operations_completed":n,"operations_failed":n}
std::string serializeSyncCompleteBody(int completed, int failed);

// Format a sync completion summary line, e.g.
//   "U:3 D:1 C:2 N:0 F:1"  -> plus " (4 unmatched)" when unmatched > 0.
// Empty action counts are still shown so each run reports all categories.
std::string formatSaveSyncSummary(int uploaded, int downloaded, int conflicts,
                                  int noOp, int failed, int unmatched);


// ---------- Save-sync state persistence (grout save_sync_state) ----------

// One row of the device's save-sync state (mirrors grout's save_sync_state
// table): what was last synced for (romId, file name) and when.
struct SyncStateRow {
    std::string fileName;
    long long romId{0};
    std::string slot;
    long long saveId{0};
    std::string contentHash;
    std::string syncedAt;
};

struct SyncStateStore {
    std::string deviceId;
    std::vector<SyncStateRow> rows;
};

constexpr const char* kSaveSyncStatePath = "sdmc:/switch/romm_switch_client/save_sync_state.json";

// JSON shape:
//   {"device_id":"...","rows":[{"file_name":...,"rom_id":...,"slot":...,
//    "save_id":...,"content_hash":...,"synced_at":...}]}
// Missing file -> empty store + true. Unreadable/corrupt file -> false with
// an empty store (caller should not clobber it).
bool loadSyncState(const std::string& path, SyncStateStore& out);
bool saveSyncState(const std::string& path, const SyncStateStore& s, std::string& err);

// Row for (romId, fileNameLower); row file names compare case-insensitively.
// nullptr when absent.
const SyncStateRow* findSyncStateRow(const SyncStateStore& s, long long romId,
                                     const std::string& fileNameLower);

// Insert or replace in place, keyed by (romId, lowercased fileName). The
// stored file name is canonicalized to lowercase (grout parity).
void upsertSyncStateRow(SyncStateStore& s, SyncStateRow row);

// ---------- Server-orchestrated sync plan (negotiate -> executable ops) ----------

// One negotiated operation ready for execution. local points into the
// caller's locals vector; null for downloads and unpaired ops.
struct OrchestratedOp {
    SyncOperation op;
    const LocalAsset* local{nullptr};
};

struct OrchestratorPlan {
    long long sessionId{0};
    std::vector<OrchestratedOp> ops;
    int suppressedUploads{0}; // uploads dropped: content unchanged since last sync
    int skippedDownloads{0};  // downloads dropped: ROM not present on device
};

// Turn a negotiate response into an executable plan (pure; the worker runs it):
//   - upload/conflict ops pair to locals by (romId, slot == local.slot),
//     never by file name;
//   - uploads whose recorded state-row hash equals the paired local's hash
//     are suppressed (content already on server), counted in suppressedUploads;
//   - downloads whose romId is absent from locallyPresentRomIds are skipped
//     (grout: downloads only apply to games on device), counted in
//     skippedDownloads;
//   - conflict ops keep their paired local (may be null); execution decides
//     via policy;
//   - no_op ops are dropped; server op order is otherwise preserved.
OrchestratorPlan buildOrchestratorPlan(const NegotiateResponse& negotiate,
                                       const std::vector<LocalAsset>& locals,
                                       const SyncStateStore& state,
                                       const std::vector<long long>& locallyPresentRomIds);

// Result of one upload/overwrite HTTP round trip.
struct UploadOutcome {
    bool ok{false};
    bool slotConflict{false}; // 409: "Slot has a newer save since your last sync"
    long long saveId{0};      // parsed response "id" on success, else 0
};

// 200/201 -> ok (saveId parsed from response JSON "id" when present);
// 409 -> slotConflict only; anything else -> neither.
UploadOutcome classifyUploadResponse(long httpStatus, const std::string& body);

// ---------- Multipart + high-level wire ops (thin) ----------

struct MultipartPart {
    std::string name;
    std::string fileName;
    std::string contentType;
    std::string data;
};

// Build a multipart/form-data body with the given boundary. Each part:
//   --boundary\r\n
//   Content-Disposition: form-data; name="X"; filename="Y"\r\n
//   Content-Type: T\r\n
//   \r\n
//   <data>\r\n
// ...then the closing --boundary--\r\n.
std::string buildMultipartBody(const std::vector<MultipartPart>& parts,
                               const std::string& boundary);

struct SyncAuthCtx {
    std::string baseUrl;
    std::string bearerTokenOrEmpty;
    std::string basicAuthRawOrEmpty; // raw base64 (caller prepends "Basic ")
    int timeoutSeconds{0};
};

// Wire ops. kind is "saves" | "states". These read whole files into memory and
// build multipart bodies. Authorization: "Bearer "+token, else "Basic "+basic,
// else none. Not exercised by host tests (network is stubbed).
bool fetchRemoteAssets(const SyncAuthCtx&, const char* kind /*"saves"|"states"*/,
                       std::vector<RemoteAsset>& out, std::string& err);
// Device-scoped variant: appends ?rom_id= and ?device_id= query params for
// non-empty filters (rom_id first). The signature above delegates here.
bool fetchRemoteAssets(const SyncAuthCtx&, const char* kind /*"saves"|"states"*/,
                       const std::string& romIdOrEmpty, const std::string& deviceIdOrEmpty,
                       std::vector<RemoteAsset>& out, std::string& err);

#ifdef UNIT_TEST
// Test hook: the exact request URL fetchRemoteAssets builds.
std::string buildFetchAssetsUrlForTest(const std::string& baseUrl, const char* kind,
                                       const std::string& romIdOrEmpty,
                                       const std::string& deviceIdOrEmpty);
#endif

bool negotiateSync(const SyncAuthCtx&, const std::string& deviceId,
                   const std::vector<LocalAsset>& saves, NegotiateResponse& out,
                   std::string& err);
bool completeSyncSession(const SyncAuthCtx&, long long sessionId, int completed,
                         int failed, std::string& err);
bool downloadAssetContent(const SyncAuthCtx&, const char* kind, long long assetId,
                          const std::string& deviceId, std::string& bytesOut,
                          std::string& err);
bool confirmSaveDownloaded(const SyncAuthCtx&, long long saveId,
                           const std::string& deviceId, std::string& err);
bool uploadNewSave(const SyncAuthCtx&, long long romId, const std::string& slot,
                   const std::string& emulator, const std::string& deviceId,
                   long long sessionId, const std::string& filePath, bool overwrite,
                   int autocleanupLimit, // SAVE_BACKUP_LIMIT; <=0 -> server default trim (10)
                   std::string& err);
bool updateExistingSave(const SyncAuthCtx&, long long saveId,
                        const std::string& deviceId, const std::string& filePath,
                        std::string& err);
bool uploadNewState(const SyncAuthCtx&, long long romId, const std::string& emulator,
                    const std::string& filePath, std::string& err);
bool updateExistingState(const SyncAuthCtx&, long long stateId,
                         const std::string& filePath, std::string& err);

} // namespace romm
