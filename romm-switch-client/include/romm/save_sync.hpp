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

// Walk savesRoot (battery saves) and statesRoot (save states) recursively
// (depth <= 3). Matched files are hashed (md5) and stat'd during the scan.
// Missing roots are fine (empty result). err is set only on real failures.
ScanResult scanAssets(const std::string& savesRoot, const std::string& statesRoot,
                      const RomMatcherFn& romMatcher, const EmulatorOfFn& emulatorOf,
                      std::string& err);

// stat + md5 hash of a file in one pass. Returns false (with *no* guarantee
// about outputs) if the file can't be opened/read.
bool computeFileMd5AndStat(const std::string& path, unsigned long long& sizeOut,
                           long long& mtimeOut, std::string& hashOut);

// ---------- DTO parse / serialize ----------

struct RemoteAsset {
    long long id{0};
    long long romId{0};
    std::string fileName;
    unsigned long long fileSizeBytes{0};
    std::string updatedAt;
    std::string emulator;
    std::string slot; // "" = null
};

std::vector<RemoteAsset> parseSavesArray(const std::string& json);
std::vector<RemoteAsset> parseStatesArray(const std::string& json);

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
                   std::string& err);
bool updateExistingSave(const SyncAuthCtx&, long long saveId,
                        const std::string& deviceId, const std::string& filePath,
                        std::string& err);
bool uploadNewState(const SyncAuthCtx&, long long romId, const std::string& emulator,
                    const std::string& filePath, std::string& err);
bool updateExistingState(const SyncAuthCtx&, long long stateId,
                         const std::string& filePath, std::string& err);

} // namespace romm
