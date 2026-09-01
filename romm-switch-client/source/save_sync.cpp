#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200112L
#endif

#include "romm/save_sync.hpp"

#include "mini/json.hpp"
#include "romm/http_common.hpp"
#include "romm/md5.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <functional>
#include <sstream>
#include <string>
#include <system_error>
#include <sys/stat.h>
#include <vector>

namespace romm {

namespace {

// Very small JSON string escaper: escapes backslash and quote; others pass through.
std::string escapeJson(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        if (c == '\\' || c == '"') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

// Read a whole file (loops fread) and normalize trailing whitespace.
bool readTextFile(const std::string& path, std::string& out) {
    out.clear();
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[4096];
    for (;;) {
        size_t n = std::fread(buf, 1, sizeof(buf), f);
        out.append(buf, n);
        if (n < sizeof(buf)) break;
    }
    std::fclose(f);

    // Trim trailing whitespace (including any trailing newline).
    while (!out.empty() &&
           (out.back() == ' ' || out.back() == '\t' || out.back() == '\n' ||
            out.back() == '\r')) {
        out.pop_back();
    }
    return true;
}

bool writeTextFileEnsureParent(const std::string& path, const std::string& text) {
    auto openFile = [&]() -> std::FILE* { return std::fopen(path.c_str(), "wb"); };
    std::FILE* f = openFile();
    if (!f) {
        std::error_code ec;
        std::filesystem::path pp(path);
        std::filesystem::create_directories(pp.parent_path(), ec);
        f = openFile();
    }
    if (!f) return false;
    std::fwrite(text.data(), 1, text.size(), f);
    std::fwrite("\n", 1, 1, f);
    std::fclose(f);
    return true;
}

// Fetch a string field from a parsed object; absent/non-string -> false.
bool getStringField(const mini::Object& o, const char* key, std::string& dst) {
    auto it = o.find(key);
    if (it == o.end() || it->second.type != mini::Value::Type::String) return false;
    dst = it->second.str;
    return true;
}

bool getLongField(const mini::Object& o, const char* key, long& dst) {
    auto it = o.find(key);
    if (it == o.end() || it->second.type != mini::Value::Type::Number) return false;
    dst = (long)it->second.number;
    return true;
}

// Lowercase an ASCII string in place.
std::string lowerASCII(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = (char)(c + ('a' - 'A'));
    return s;
}

// Join a base URL and a path, keeping at most one trailing '/' between them.
std::string joinUrl(const std::string& base, const std::string& path) {
    std::string u = base;
    if (!u.empty() && u.back() == '/') u.pop_back();
    if (!path.empty() && path.front() == '/') return u + path;
    return u + "/" + path;
}

// Percent-encode a string for use inside a URL query value.
std::string percentEncode(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (unsigned char c : in) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back((char)c);
        } else {
            static const char kHex[] = "0123456789ABCDEF";
            out.push_back('%');
            out.push_back(kHex[(c >> 4) & 0xF]);
            out.push_back(kHex[c & 0xF]);
        }
    }
    return out;
}

// Last path component of a '/' or '\' separated path.
std::string fileBasename(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    if (slash == std::string::npos) return path;
    return path.substr(slash + 1);
}

// Read a whole file (binary); returns false if it can't be opened.
bool readFileBytes(const std::string& path, std::string& out) {
    out.clear();
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[8192];
    for (;;) {
        size_t n = std::fread(buf, 1, sizeof(buf), f);
        out.append(buf, n);
        if (n < sizeof(buf)) break;
    }
    bool ok = (std::ferror(f) == 0);
    std::fclose(f);
    return ok;
}

// Build the Authorization/Content-Type/other headers for a request into `headers`.
void appendAuthHeader(const SyncAuthCtx& ctx, std::vector<std::pair<std::string, std::string>>& headers) {
    if (!ctx.bearerTokenOrEmpty.empty()) {
        headers.emplace_back("Authorization", "Bearer " + ctx.bearerTokenOrEmpty);
    } else if (!ctx.basicAuthRawOrEmpty.empty()) {
        headers.emplace_back("Authorization", "Basic " + ctx.basicAuthRawOrEmpty);
    }
}

// Perform an HTTP request; returns the response body if HTTP status is 2xx.
// Sets err with status on non-2xx. Not exercised on the host (network stubbed).
bool performRequest(const SyncAuthCtx& ctx, const std::string& method, const std::string& url,
                    const std::vector<std::pair<std::string, std::string>>& headers,
                    const void* body, size_t bodySize,
                    std::string& bodyOut, std::string& err) {
    bodyOut.clear();
    err.clear();
    HttpRequestOptions options;
    options.timeoutSec = ctx.timeoutSeconds;
    options.requestBody = body;
    options.requestBodySize = bodySize;
    HttpTransaction tx;
    if (!httpRequestBuffered(method, url, headers, options, tx, err)) return false;
    if (tx.parsed.statusCode < 200 || tx.parsed.statusCode >= 300) {
        err = "HTTP " + std::to_string(tx.parsed.statusCode);
        return false;
    }
    bodyOut = tx.body;
    return true;
}

} // namespace

// ---------- Device token persistence ----------

bool parseDeviceTokenJson(const std::string& json, DeviceToken& out) {
    out = DeviceToken{};
    mini::Object obj;
    if (!mini::parse(json, obj)) return false;
    getStringField(obj, "access_token", out.accessToken);
    getStringField(obj, "device_id", out.deviceId);
    getStringField(obj, "client_device_identifier", out.clientDeviceIdentifier);
    if (out.accessToken.empty()) return false;
    return true;
}

std::string serializeDeviceTokenJson(const DeviceToken& t) {
    std::ostringstream os;
    os << "{";
    os << "\"access_token\":\"" << escapeJson(t.accessToken) << "\",";
    os << "\"device_id\":\"" << escapeJson(t.deviceId) << "\",";
    os << "\"client_device_identifier\":\"" << escapeJson(t.clientDeviceIdentifier) << "\"";
    os << "}";
    return os.str();
}

bool loadDeviceToken(const std::string& path, DeviceToken& out) {
    std::string content;
    if (!readTextFile(path, content)) return false;
    return parseDeviceTokenJson(content, out);
}

bool saveDeviceToken(const std::string& path, const DeviceToken& t, std::string& err) {
    err.clear();
    if (!writeTextFileEnsureParent(path, serializeDeviceTokenJson(t))) {
        err = "failed to write device token to " + path;
        return false;
    }
    return true;
}

// ---------- Device-auth pairing ----------

DeviceAuthPollResult classifyDeviceTokenResponse(long httpStatus, const std::string& body) {
    DeviceAuthPollResult r;

    if (httpStatus >= 200 && httpStatus < 300) {
        mini::Object obj;
        if (!mini::parse(body, obj)) {
            r.state = DeviceAuthPollState::Error;
            return r;
        }
        r.state = DeviceAuthPollState::Approved;
        getStringField(obj, "access_token", r.accessToken);
        getStringField(obj, "device_id", r.deviceId);
        return r;
    }

    // Non-2xx: look for a { "detail": "..." } flow-state marker.
    mini::Object obj;
    std::string detail;
    if (mini::parse(body, obj)) {
        getStringField(obj, "detail", detail);
    }

    r.detail = detail;
    if (httpStatus == 400) {
        if (detail == "authorization_pending") {
            r.state = DeviceAuthPollState::Pending;
        } else if (detail == "slow_down") {
            r.state = DeviceAuthPollState::SlowDown;
        } else if (detail == "access_denied") {
            r.state = DeviceAuthPollState::AccessDenied;
        } else if (detail == "expired_token") {
            r.state = DeviceAuthPollState::ExpiredToken;
        } else {
            r.state = DeviceAuthPollState::Error;
        }
    } else {
        r.state = DeviceAuthPollState::Error;
    }
    return r;
}

std::string serializeDeviceAuthInitBody(const DeviceAuthInitRequest& r) {
    static const char* kScopes[] = {
        "me.read",        "platforms.read",   "roms.read",
        "collections.read", "firmware.read",  "assets.read",
        "assets.write",   "devices.read",     "devices.write",
    };
    std::ostringstream os;
    os << "{";
    os << "\"client_device_identifier\":\"" << escapeJson(r.clientDeviceIdentifier) << "\",";
    os << "\"name\":\"" << escapeJson(r.name) << "\",";
    os << "\"client\":\"" << escapeJson(r.client) << "\",";
    os << "\"client_version\":\"" << escapeJson(r.clientVersion) << "\",";
    os << "\"requested_scopes\":[";
    for (size_t i = 0; i < 9; ++i) {
        if (i) os << ",";
        os << "\"" << kScopes[i] << "\"";
    }
    os << "]";
    os << "}";
    return os.str();
}

bool parseDeviceAuthInitResponse(const std::string& json, DeviceAuthInitResponse& out) {
    out = DeviceAuthInitResponse{};
    mini::Object obj;
    if (!mini::parse(json, obj)) return false;

    if (!getStringField(obj, "device_code", out.deviceCode)) return false;

    getStringField(obj, "user_code", out.userCode);
    getStringField(obj, "verification_path", out.verificationPath);

    if (getStringField(obj, "verification_path_complete", out.verificationUrlComplete)) {
        out.verificationPathComplete = !out.verificationUrlComplete.empty();
    }

    getLongField(obj, "expires_in", out.expiresIn);
    getLongField(obj, "interval", out.interval);
    return true;
}

bool serverSupportsDeviceAuth(const std::string& systemVersion) {
    // Mirror grout HeartbeatResponse.SupportsDeviceAuth.
    std::string v = systemVersion;
    if (!v.empty() && v[0] == 'v') v.erase(0, 1);

    // Take text before first '.'.
    size_t dot = v.find('.');
    if (dot != std::string::npos) v = v.substr(0, dot);

    // Strip any '-suffix' prerelease marker.
    size_t dash = v.find('-');
    if (dash != std::string::npos) v = v.substr(0, dash);

    if (v.empty()) return false;
    for (char c : v) {
        if (c < '0' || c > '9') return false;
    }
    long n = std::strtol(v.c_str(), nullptr, 10);
    return n >= 5;
}

// ---------- Save / state sync engine (phase 2) ----------

bool isValidBatterySaveExtension(const std::string& lowerExt) {
    static const char* kExts[] = {".srm", ".sav", ".dsv", ".mcr", ".mcd", ".brm",
                                  ".eep", ".sra", ".fla", ".mpk", ".nv"};
    std::string ext = lowerASCII(lowerExt);
    for (const char* e : kExts) {
        if (ext == e) return true;
    }
    return false;
}

StateKind classifyStateFileName(const std::string& fileName) {
    std::string lower = lowerASCII(fileName);
    size_t pos = lower.find(".state");
    if (pos == std::string::npos) return StateKind::None;
    std::string rest = lower.substr(pos + 6); // everything after ".state"
    if (rest.empty()) return StateKind::Base;
    if (rest == ".auto") return StateKind::Auto;
    // Tico writes bare slot digits ("game.state0".."game.state3");
    // legacy layouts use a second dot ("game.state.7").
    std::string digits = (!rest.empty() && rest[0] == '.') ? rest.substr(1) : rest;
    if (!digits.empty()) {
        bool allDigits = true;
        for (char c : digits) {
            if (c < '0' || c > '9') { allDigits = false; break; }
        }
        if (allDigits) return StateKind::Numbered;
    }
    return StateKind::None;
}

std::string formatIso8601Utc(long long epochSeconds) {
    std::time_t sec = (std::time_t)epochSeconds;
    std::tm t;
    gmtime_r(&sec, &t);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &t);
    return std::string(buf);
}
// Extension including the dot, lowercased ("" when none).
std::string extOf(const std::string& lowerName) {
    size_t dot = lowerName.rfind('.');
    return dot == std::string::npos ? std::string() : lowerName.substr(dot);
}

std::string saveSlotLabel(const std::string& fileName) {
    std::string lower = lowerASCII(fileName);
    size_t pos = lower.find(".state");
    if (pos != std::string::npos) {
        std::string rest = lower.substr(pos + 6); // everything after ".state"
        if (rest.empty()) return "Base";
        if (rest == ".auto") return "Auto";
        // Same shape as classifyStateFileName: bare digits or ".<digits>".
        std::string digits = (!rest.empty() && rest[0] == '.') ? rest.substr(1) : rest;
        bool allDigits = !digits.empty();
        for (char c : digits) {
            if (c < '0' || c > '9') { allDigits = false; break; }
        }
        if (allDigits) return "State " + digits;
    }
    if (isValidBatterySaveExtension(extOf(lower))) return "Save";
    return "";
}

std::string trimIsoToMinutes(const std::string& iso) {
    if (iso.size() < 16 || iso[10] != 'T') return iso;
    return iso.substr(0, 10) + " " + iso.substr(11, 5);
}


std::string saveLookupBase(const std::string& fileNameNoExt) {
    size_t dot = fileNameNoExt.rfind('.');
    if (dot == std::string::npos || dot + 1 >= fileNameNoExt.size()) return fileNameNoExt;
    std::string tail = fileNameNoExt.substr(dot + 1);
    if (tail.size() >= 2 && tail.size() <= 5) {
        bool romLike = true;
        for (char c : tail) {
            if (!std::isalnum((unsigned char)c)) { romLike = false; break; }
        }
        if (romLike) return fileNameNoExt.substr(0, dot);
    }
    return fileNameNoExt;
}

long long romIdForMatch(const std::vector<RomMatchEntry>& roms,
                        const std::string& baseLower, const std::string& slugHint) {
    const std::string hint = lowerASCII(slugHint);
    long long fallback = 0;
    for (const auto& e : roms) {
        std::string romBase = lowerASCII(saveLookupBase(e.fsNameLower));
        if (romBase != baseLower) continue;
        if (fallback == 0) fallback = e.romId;
        if (!hint.empty() && hint == e.slugLower) return e.romId; // preferred same-slug match
    }
    return fallback;
}

std::string decideStateOperation(const LocalAsset* local, const std::string& remoteUpdatedAt,
                                 const std::string& remoteContentHash,
                                 const std::string& computedLocalHash) {
    if (local == nullptr) {
        return remoteUpdatedAt.empty() ? "skip" : "download";
    }
    if (remoteUpdatedAt.empty()) return "upload";

    if (local->updatedAtIso > remoteUpdatedAt) return "upload";
    if (local->updatedAtIso < remoteUpdatedAt) return "download";
    // Equal timestamps.
    if (computedLocalHash == remoteContentHash) return "no_op";
    return "skip";
}

// ---------- Client-side reconcile (plan builder + policy) ----------

SyncPolicy parseSyncPolicy(const std::string& s) {
    std::string v;
    v.reserve(s.size());
    for (char c : s) {
        if (std::isspace((unsigned char)c)) continue;
        v.push_back((char)std::tolower((unsigned char)c));
    }
    if (v == "newest") return SyncPolicy::NewestWins;
    if (v == "server") return SyncPolicy::ServerWins;
    if (v == "client") return SyncPolicy::ClientWins;
    return SyncPolicy::AskEveryTime; // "ask" and anything unknown
}

const char* syncPolicyName(SyncPolicy p) {
    switch (p) {
        case SyncPolicy::NewestWins: return "newest";
        case SyncPolicy::ServerWins: return "server";
        case SyncPolicy::ClientWins: return "client";
        case SyncPolicy::AskEveryTime: break;
    }
    return "ask";
}

namespace {

std::string syncPlanKey(long long romId, const std::string& fileName) {
    std::ostringstream os;
    os << romId << '\n' << fileName;
    return os.str();
}

// Decide the policy-independent baseline for a paired item. Mirrors
// decideStateOperation: newest timestamp wins; equal timestamps compare
// content hashes (no_op when identical, conflict flag when they differ).
void decideInto(const LocalAsset& local, bool hasRemote, const RemoteAsset& remote,
                SyncPlanItem& out) {
    if (!hasRemote) {
        out.newestAction = SyncPlanAction::Upload;
        return;
    }
    const std::string decision = decideStateOperation(&local, remote.updatedAt,
                                                      remote.contentHash,
                                                      local.contentHash);
    if (decision == "upload") {
        out.newestAction = SyncPlanAction::Upload;
    } else if (decision == "download") {
        out.newestAction = SyncPlanAction::Download;
    } else {
        // "no_op" and the genuine-conflict "skip" both stay NoOp; the conflict
        // flag keeps "skip" visible to ask-policy flows.
        out.newestAction = SyncPlanAction::NoOp;
        out.conflict = (decision == "skip");
    }
}

} // namespace

std::vector<SyncPlanItem> buildSyncPlan(const std::vector<LocalAsset>& locals,
                                        const std::vector<RemoteAsset>& remoteSaves,
                                        const std::vector<RemoteAsset>& remoteStates,
                                        const std::vector<long long>& knownRomIds) {
    std::vector<SyncPlanItem> plan;

    std::unordered_map<std::string, size_t> localIdx;
    localIdx.reserve(locals.size());
    for (size_t i = 0; i < locals.size(); ++i) {
        if (locals[i].romId <= 0) continue; // unmatched scan entries are invisible to sync
        const std::string key = syncPlanKey(locals[i].romId, locals[i].fileName);
        if (localIdx.find(key) == localIdx.end()) localIdx.emplace(key, plan.size());
        SyncPlanItem it;
        it.local = &locals[i];
        it.isState = locals[i].isState;
        decideInto(*it.local, false, RemoteAsset{}, it);
        plan.push_back(it);
    }

    auto applyRemote = [&](const RemoteAsset& r) {
        if (r.romId <= 0) return;
        const std::string key = syncPlanKey(r.romId, r.fileName);
        auto found = localIdx.find(key);
        if (found != localIdx.end()) {
            SyncPlanItem& it = plan[found->second];
            it.hasRemote = true;
            it.remote = r;
            decideInto(*it.local, true, r, it);
        } else if (std::find(knownRomIds.begin(), knownRomIds.end(), r.romId) != knownRomIds.end()) {
            SyncPlanItem it;
            it.hasRemote = true;
            it.remote = r;
            it.isState = classifyStateFileName(r.fileName) != StateKind::None;
            it.newestAction = SyncPlanAction::Download;
            plan.push_back(it);
        }
    };
    for (const auto& r : remoteSaves) applyRemote(r);
    for (const auto& r : remoteStates) applyRemote(r);
    return plan;
}

bool syncPlanItemActionable(const SyncPlanItem& it) {
    return it.newestAction != SyncPlanAction::NoOp || it.conflict;
}

bool syncPlanNeedsChoice(const SyncPlanItem& it, SyncPolicy policy) {
    if (policy != SyncPolicy::AskEveryTime) return false;
    if (it.local == nullptr || !it.hasRemote) return false;
    return it.conflict ||
           it.newestAction == SyncPlanAction::Upload ||
           it.newestAction == SyncPlanAction::Download;
}

SyncPlanAction resolveSyncAction(const SyncPlanItem& it, SyncPolicy policy) {
    switch (policy) {
        case SyncPolicy::NewestWins:
            return it.conflict ? SyncPlanAction::NoOp : it.newestAction;
        case SyncPolicy::ServerWins:
            return it.hasRemote ? SyncPlanAction::Download : SyncPlanAction::NoOp;
        case SyncPolicy::ClientWins:
            return it.local != nullptr ? SyncPlanAction::Upload : SyncPlanAction::NoOp;
        case SyncPolicy::AskEveryTime:
            break;
    }
    // AskEveryTime: single-sided items resolve mechanically; two-sided items
    // wait for the interactive choice (worker collects them instead).
    if (it.local == nullptr || !it.hasRemote) return it.newestAction;
    return SyncPlanAction::NoOp;
}


bool computeFileMd5AndStat(const std::string& path, unsigned long long& sizeOut,
                           long long& mtimeOut, std::string& hashOut) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    struct stat st;
    if (fstat(fileno(f), &st) != 0) {
        std::fclose(f);
        return false;
    }
    sizeOut = (unsigned long long)st.st_size;
    mtimeOut = (long long)st.st_mtime;

    std::string data;
    char buf[8192];
    for (;;) {
        size_t n = std::fread(buf, 1, sizeof(buf), f);
        if (n) data.append(buf, n);
        if (n < sizeof(buf)) break;
    }
    bool ok = (std::ferror(f) == 0);
    std::fclose(f);
    if (!ok) return false;
    return md5Hex(data, hashOut);
}

namespace {

// Recursively scan a directory. isStateRoot selects state vs battery-save rules.
void scanDirRecursive(const std::string& dir, int depth, bool isStateRoot,
                      const RomMatcherFn& romMatcher, const EmulatorOfFn& emulatorOf,
                      const SlugHintFn& slugHintOf,
                      std::vector<LocalAsset>& out, std::vector<std::string>& unmatched,
                      std::string& err) {
    if (depth > 3) return;
    std::error_code ec;
    std::filesystem::directory_iterator it(dir, ec);
    if (ec) {
        if (err.empty()) err = "cannot read directory " + dir;
        return;
    }
    for (auto const& entry : it) {
        std::error_code ec2;
        bool isDir = entry.is_directory(ec2);
        if (ec2) continue;
        if (isDir) {
            scanDirRecursive(entry.path().string(), depth + 1, isStateRoot, romMatcher,
                             emulatorOf, slugHintOf, out, unmatched, err);
            continue;
        }

        const std::string path = entry.path().string();
        const std::string fileName = entry.path().filename().string();

        bool isState = false;
        std::string baseLower;
        if (isStateRoot) {
            if (classifyStateFileName(fileName) == StateKind::None) continue;
            isState = true;
            std::string lowerName = lowerASCII(fileName);
            size_t pos = lowerName.find(".state");
            baseLower = (pos == std::string::npos) ? lowerName : lowerName.substr(0, pos);
        } else {
            std::string lowerName = lowerASCII(fileName);
            size_t dot = lowerName.rfind('.');
            std::string ext = (dot == std::string::npos) ? "" : lowerName.substr(dot);
            if (!isValidBatterySaveExtension(ext)) continue;
            std::string bare = (dot == std::string::npos) ? fileName : fileName.substr(0, dot);
            baseLower = lowerASCII(saveLookupBase(bare));
        }

        LocalAsset asset;
        asset.fileName = fileName;
        asset.path = path;
        asset.isState = isState;
        asset.slot = isState ? "" : "autosave";
        asset.emulator = emulatorOf(path);
        if (!computeFileMd5AndStat(path, asset.sizeBytes, asset.mtimeEpoch, asset.contentHash)) {
            if (err.empty()) err = "failed to hash " + path;
            continue;
        }

        asset.updatedAtIso = formatIso8601Utc(asset.mtimeEpoch);
        asset.romId = romMatcher(baseLower,
                                 slugHintOf ? slugHintOf(path, isState) : std::string());
        out.push_back(asset);
        if (asset.romId == 0) unmatched.push_back(fileName);
    }
}

bool scanRoot(const std::string& root, bool isStateRoot, const RomMatcherFn& romMatcher,
              const EmulatorOfFn& emulatorOf, const SlugHintFn& slugHintOf,
              std::vector<LocalAsset>& out, std::vector<std::string>& unmatched,
              std::string& err) {
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) return true; // missing root is fine
    scanDirRecursive(root, 0, isStateRoot, romMatcher, emulatorOf, slugHintOf,
                     out, unmatched, err);
    return true;
}

} // namespace

ScanResult scanAssets(const std::string& savesRoot, const std::string& statesRoot,
                      const RomMatcherFn& romMatcher, const EmulatorOfFn& emulatorOf,
                      const SlugHintFn& slugHintOf, std::string& err) {
    err.clear();
    ScanResult result;
    scanRoot(savesRoot, false, romMatcher, emulatorOf, slugHintOf, result.assets,
             result.unmatched, err);
    scanRoot(statesRoot, true, romMatcher, emulatorOf, slugHintOf, result.assets,
             result.unmatched, err);
    return result;
}

std::vector<DiskRom> scanDiskRoms(const std::string& romsRoot) {
    std::vector<DiskRom> out;
    std::error_code ec;
    if (!std::filesystem::is_directory(romsRoot, ec)) return out; // missing root is fine
    std::filesystem::directory_iterator platformIt(romsRoot, ec);
    if (ec) return out;
    for (auto const& plat : platformIt) {
        std::error_code ec2;
        if (!plat.is_directory(ec2) || ec2) continue;
        const std::string platformFolder = plat.path().filename().string();
        auto record = [&](const std::filesystem::directory_entry& f,
                          const std::string& subFolder) {
            DiskRom rom;
            rom.platformFolder = platformFolder;
            rom.subFolder = subFolder;
            rom.baseLower = lowerASCII(saveLookupBase(f.path().stem().string()));
            out.push_back(std::move(rom));
        };
        // Pass 1: ROM files directly in the platform folder.
        std::filesystem::directory_iterator topIt(plat.path(), ec2);
        if (ec2) continue;
        for (auto const& entry : topIt) {
            std::error_code ec3;
            if (!entry.is_regular_file(ec3) || ec3) continue;
            record(entry, "");
        }
        // Pass 2: ROM files inside one optional game subfolder.
        std::filesystem::directory_iterator subIt(plat.path(), ec2);
        if (ec2) continue;
        for (auto const& sub : subIt) {
            std::error_code ec3;
            if (!sub.is_directory(ec3) || ec3) continue;
            std::filesystem::directory_iterator fit(sub.path(), ec3);
            if (ec3) continue;
            for (auto const& f : fit) {
                std::error_code ec4;
                if (!f.is_regular_file(ec4) || ec4) continue;
                record(f, sub.path().filename().string());
            }
        }
    }
    return out;
}

// ---------- DTO parse / serialize ----------

namespace {

RemoteAsset parseRemoteAsset(const mini::Value& v) {
    RemoteAsset a;
    if (v.type != mini::Value::Type::Object) return a;
    if (auto it = v.object.find("id"); it != v.object.end() && it->second.type == mini::Value::Type::Number)
        a.id = (long long)it->second.number;
    if (auto it = v.object.find("rom_id"); it != v.object.end() && it->second.type == mini::Value::Type::Number)
        a.romId = (long long)it->second.number;
    if (auto it = v.object.find("file_name"); it != v.object.end() && it->second.type == mini::Value::Type::String)
        a.fileName = it->second.str;
    if (auto it = v.object.find("file_size_bytes"); it != v.object.end() && it->second.type == mini::Value::Type::Number)
        a.fileSizeBytes = (unsigned long long)it->second.number;
    if (auto it = v.object.find("updated_at"); it != v.object.end() && it->second.type == mini::Value::Type::String)
        a.updatedAt = it->second.str;
    if (auto it = v.object.find("content_hash"); it != v.object.end() && it->second.type == mini::Value::Type::String)
        a.contentHash = it->second.str;
    if (auto it = v.object.find("emulator"); it != v.object.end() && it->second.type == mini::Value::Type::String)
        a.emulator = it->second.str;
    if (auto it = v.object.find("slot"); it != v.object.end() && it->second.type == mini::Value::Type::String)
        a.slot = it->second.str;
    return a;
}

std::vector<RemoteAsset> parseAssetArray(const std::string& json) {
    std::vector<RemoteAsset> out;
    mini::Array arr;
    if (!mini::parse(json, arr)) return out;
    for (const auto& v : arr) {
        out.push_back(parseRemoteAsset(v));
    }
    return out;
}

} // namespace

std::vector<RemoteAsset> parseSavesArray(const std::string& json) {
    return parseAssetArray(json);
}

std::vector<RemoteAsset> parseStatesArray(const std::string& json) {
    return parseAssetArray(json);
}

std::string serializeNegotiatePayload(const std::string& deviceId,
                                      const std::vector<LocalAsset>& saves) {
    std::ostringstream os;
    os << "{";
    os << "\"device_id\":\"" << escapeJson(deviceId) << "\",";
    os << "\"saves\":[";
    for (size_t i = 0; i < saves.size(); ++i) {
        if (i) os << ",";
        const LocalAsset& s = saves[i];
        os << "{";
        os << "\"rom_id\":" << s.romId << ",";
        os << "\"file_name\":\"" << escapeJson(s.fileName) << "\",";
        os << "\"slot\":" << (s.slot.empty() ? "null" : "\"" + escapeJson(s.slot) + "\"") << ",";
        os << "\"emulator\":" << (s.emulator.empty() ? "null" : "\"" + escapeJson(s.emulator) + "\"") << ",";
        os << "\"content_hash\":" << (s.contentHash.empty() ? "null" : "\"" + escapeJson(s.contentHash) + "\"") << ",";
        os << "\"updated_at\":\"" << escapeJson(s.updatedAtIso) << "\",";
        os << "\"file_size_bytes\":" << s.sizeBytes;
        os << "}";
    }
    os << "]";
    os << "}";
    return os.str();
}

bool parseNegotiateResponse(const std::string& json, NegotiateResponse& out) {
    out = NegotiateResponse{};
    mini::Object obj;
    if (!mini::parse(json, obj)) return false;

    auto sid = obj.find("session_id");
    if (sid == obj.end() || sid->second.type != mini::Value::Type::Number) return false;
    out.sessionId = (long long)sid->second.number;

    auto ops = obj.find("operations");
    if (ops != obj.end() && ops->second.type == mini::Value::Type::Array) {
        for (const auto& v : ops->second.array) {
            if (v.type != mini::Value::Type::Object) continue;
            SyncOperation op;
            if (auto it = v.object.find("action"); it != v.object.end() && it->second.type == mini::Value::Type::String)
                op.action = it->second.str;
            if (auto it = v.object.find("rom_id"); it != v.object.end() && it->second.type == mini::Value::Type::Number)
                op.romId = (long long)it->second.number;
            if (auto it = v.object.find("save_id"); it != v.object.end() && it->second.type == mini::Value::Type::Number) {
                op.assetId = (long long)it->second.number;
                op.hasAssetId = true;
            }
            if (auto it = v.object.find("file_name"); it != v.object.end() && it->second.type == mini::Value::Type::String)
                op.fileName = it->second.str;
            if (auto it = v.object.find("slot"); it != v.object.end() && it->second.type == mini::Value::Type::String)
                op.slot = it->second.str;
            if (auto it = v.object.find("emulator"); it != v.object.end() && it->second.type == mini::Value::Type::String)
                op.emulator = it->second.str;
            if (auto it = v.object.find("reason"); it != v.object.end() && it->second.type == mini::Value::Type::String)
                op.reason = it->second.str;
            if (auto it = v.object.find("server_updated_at"); it != v.object.end() && it->second.type == mini::Value::Type::String)
                op.serverUpdatedAt = it->second.str;
            if (auto it = v.object.find("server_content_hash"); it != v.object.end() && it->second.type == mini::Value::Type::String)
                op.serverContentHash = it->second.str;
            out.operations.push_back(op);
        }
    }

    auto g = [&](const char* key) -> int {
        auto it = obj.find(key);
        if (it != obj.end() && it->second.type == mini::Value::Type::Number) return (int)it->second.number;
        return 0;
    };
    out.totalUpload = g("total_upload");
    out.totalDownload = g("total_download");
    out.totalConflict = g("total_conflict");
    out.totalNoOp = g("total_no_op");
    return true;
}

std::string serializeSyncCompleteBody(int completed, int failed) {
    std::ostringstream os;
    os << "{\"operations_completed\":" << completed << ",\"operations_failed\":" << failed << "}";
    return os.str();
}

std::string formatSaveSyncSummary(int uploaded, int downloaded, int conflicts,
                                  int noOp, int failed, int unmatched) {
    std::ostringstream os;
    os << "U:" << uploaded << " D:" << downloaded << " C:" << conflicts
       << " N:" << noOp << " F:" << failed;
    if (unmatched > 0) os << " (" << unmatched << " unmatched)";
    return os.str();
}

// ---------- Save-sync state persistence (grout save_sync_state) ----------

namespace {

// Canonical key of a state row: rom id + lowercased file name.
std::string syncStateKey(long long romId, const std::string& fileName) {
    std::ostringstream os;
    os << romId << '\n' << lowerASCII(fileName);
    return os.str();
}

// A row is usable when it names a file and belongs to a ROM.
bool syncStateRowUsable(const SyncStateRow& r) {
    return r.romId > 0 && !r.fileName.empty();
}

bool parseSyncStateJson(const std::string& json, SyncStateStore& out) {
    out = SyncStateStore{};
    mini::Object obj;
    if (!mini::parse(json, obj)) return false;
    getStringField(obj, "device_id", out.deviceId);
    auto rows = obj.find("rows");
    if (rows == obj.end() || rows->second.type != mini::Value::Type::Array) return true;
    for (const auto& v : rows->second.array) {
        if (v.type != mini::Value::Type::Object) continue;
        SyncStateRow row;
        getStringField(v.object, "file_name", row.fileName);
        if (auto it = v.object.find("rom_id"); it != v.object.end() && it->second.type == mini::Value::Type::Number)
            row.romId = (long long)it->second.number;
        getStringField(v.object, "slot", row.slot);
        if (auto it = v.object.find("save_id"); it != v.object.end() && it->second.type == mini::Value::Type::Number)
            row.saveId = (long long)it->second.number;
        getStringField(v.object, "content_hash", row.contentHash);
        getStringField(v.object, "synced_at", row.syncedAt);
        if (syncStateRowUsable(row)) out.rows.push_back(std::move(row));
    }
    return true;
}

} // namespace

bool loadSyncState(const std::string& path, SyncStateStore& out) {
    out = SyncStateStore{};
    std::string content;
    // Missing file = nothing synced yet: empty store, success.
    if (!readTextFile(path, content)) return true;
    return parseSyncStateJson(content, out);
}

bool saveSyncState(const std::string& path, const SyncStateStore& s, std::string& err) {
    err.clear();
    std::ostringstream os;
    os << "{";
    os << "\"device_id\":\"" << escapeJson(s.deviceId) << "\",";
    os << "\"rows\":[";
    for (size_t i = 0; i < s.rows.size(); ++i) {
        if (i) os << ",";
        const SyncStateRow& r = s.rows[i];
        os << "{";
        os << "\"file_name\":\"" << escapeJson(r.fileName) << "\",";
        os << "\"rom_id\":" << r.romId << ",";
        os << "\"slot\":" << (r.slot.empty() ? "null" : "\"" + escapeJson(r.slot) + "\"") << ",";
        os << "\"save_id\":" << r.saveId << ",";
        os << "\"content_hash\":" << (r.contentHash.empty() ? "null" : "\"" + escapeJson(r.contentHash) + "\"") << ",";
        os << "\"synced_at\":\"" << escapeJson(r.syncedAt) << "\"";
        os << "}";
    }
    os << "]}";
    if (!writeTextFileEnsureParent(path, os.str())) {
        err = "failed to write save sync state to " + path;
        return false;
    }
    return true;
}

const SyncStateRow* findSyncStateRow(const SyncStateStore& s, long long romId,
                                     const std::string& fileNameLower) {
    const std::string key = syncStateKey(romId, fileNameLower);
    for (const SyncStateRow& r : s.rows) {
        if (syncStateKey(r.romId, r.fileName) == key) return &r;
    }
    return nullptr;
}

void upsertSyncStateRow(SyncStateStore& s, SyncStateRow row) {
    if (!syncStateRowUsable(row)) return;
    row.fileName = lowerASCII(row.fileName);
    const std::string key = syncStateKey(row.romId, row.fileName);
    for (SyncStateRow& r : s.rows) {
        if (syncStateKey(r.romId, r.fileName) == key) {
            r = std::move(row);
            return;
        }
    }
    s.rows.push_back(std::move(row));
}

// ---------- Server-orchestrated sync plan (negotiate -> executable ops) ----------

namespace {

// First local whose romId matches the op and whose slot matches exactly
// (both empty counts as equal). File name is deliberately ignored: the
// server keys slots, the device names files.
const LocalAsset* pairLocalByRomSlot(const SyncOperation& op,
                                     const std::vector<LocalAsset>& locals) {
    for (const LocalAsset& l : locals) {
        if ((long long)l.romId != op.romId) continue;
        if (l.slot != op.slot) continue;
        return &l;
    }
    return nullptr;
}

bool romIdPresent(const std::vector<long long>& romIds, long long romId) {
    return std::find(romIds.begin(), romIds.end(), romId) != romIds.end();
}

} // namespace

OrchestratorPlan buildOrchestratorPlan(const NegotiateResponse& negotiate,
                                       const std::vector<LocalAsset>& locals,
                                       const SyncStateStore& state,
                                       const std::vector<long long>& locallyPresentRomIds) {
    OrchestratorPlan plan;
    plan.sessionId = negotiate.sessionId;
    for (const SyncOperation& op : negotiate.operations) {
        const bool isUploadLike = (op.action == "upload" || op.action == "conflict");
        if (isUploadLike) {
            OrchestratedOp o;
            o.op = op;
            o.local = pairLocalByRomSlot(op, locals);
            if (op.action == "upload" && o.local != nullptr) {
                const SyncStateRow* row =
                    findSyncStateRow(state, op.romId, lowerASCII(op.fileName));
                if (row != nullptr && row->contentHash == o.local->contentHash) {
                    ++plan.suppressedUploads; // file unchanged since last sync
                    continue;
                }
            }
            plan.ops.push_back(std::move(o));
            continue;
        }
        if (op.action == "download") {
            if (!romIdPresent(locallyPresentRomIds, op.romId)) {
                ++plan.skippedDownloads; // grout: downloads only for games on device
                continue;
            }
        }
        if (op.action == "no_op") continue; // nothing to do
        OrchestratedOp o;
        o.op = op;
        // Only upload/conflict pair to a local; downloads fetch remote
        // content and never reference one (spec: local is null for them).
        o.local = isUploadLike ? pairLocalByRomSlot(op, locals) : nullptr;
        plan.ops.push_back(std::move(o));
    }
    return plan;
}

UploadOutcome classifyUploadResponse(long httpStatus, const std::string& body) {
    UploadOutcome out;
    if (httpStatus == 409) {
        out.slotConflict = true;
        return out;
    }
    if (httpStatus < 200 || httpStatus >= 300) return out;
    out.ok = true;
    mini::Object obj;
    if (mini::parse(body, obj)) {
        if (auto it = obj.find("id"); it != obj.end() && it->second.type == mini::Value::Type::Number)
            out.saveId = (long long)it->second.number;
    }
    return out;
}


// ---------- Multipart ----------

std::string buildMultipartBody(const std::vector<MultipartPart>& parts, const std::string& boundary) {
    std::ostringstream os;
    for (const auto& p : parts) {
        os << "--" << boundary << "\r\n";
        os << "Content-Disposition: form-data; name=\"" << p.name << "\"; filename=\"" << p.fileName << "\"\r\n";
        os << "Content-Type: " << p.contentType << "\r\n";
        os << "\r\n";
        os << p.data;
        os << "\r\n";
    }
    os << "--" << boundary << "--\r\n";
    return os.str();
}
// Mirrors exactly the URL fetchRemoteAssets requests. Declared in the header
// only under UNIT_TEST; defined unconditionally so fetchRemoteAssets shares it.
std::string buildFetchAssetsUrlForTest(const std::string& baseUrl, const char* kind,
                                       const std::string& romIdOrEmpty,
                                       const std::string& deviceIdOrEmpty) {
    std::string url = joinUrl(baseUrl, std::string("/api/") + kind);
    std::string query;
    if (!romIdOrEmpty.empty()) query += "rom_id=" + percentEncode(romIdOrEmpty);
    if (!deviceIdOrEmpty.empty()) {
        if (!query.empty()) query += "&";
        query += "device_id=" + percentEncode(deviceIdOrEmpty);
    }
    if (!query.empty()) url += "?" + query;
    return url;
}

// ---------- High-level wire ops ----------

bool fetchRemoteAssets(const SyncAuthCtx& ctx, const char* kind, std::vector<RemoteAsset>& out,
                       std::string& err) {
    return fetchRemoteAssets(ctx, kind, std::string(), std::string(), out, err);
}

bool fetchRemoteAssets(const SyncAuthCtx& ctx, const char* kind,
                       const std::string& romIdOrEmpty, const std::string& deviceIdOrEmpty,
                       std::vector<RemoteAsset>& out, std::string& err) {
    out.clear();
    std::string url = buildFetchAssetsUrlForTest(ctx.baseUrl, kind, romIdOrEmpty, deviceIdOrEmpty);
    std::vector<std::pair<std::string, std::string>> headers;
    appendAuthHeader(ctx, headers);
    std::string body;
    if (!performRequest(ctx, "GET", url, headers, nullptr, 0, body, err)) return false;
    out = (std::string(kind) == "states") ? parseStatesArray(body) : parseSavesArray(body);
    return true;
}

bool negotiateSync(const SyncAuthCtx& ctx, const std::string& deviceId,
                   const std::vector<LocalAsset>& saves, NegotiateResponse& out,
                   std::string& err) {
    std::string payload = serializeNegotiatePayload(deviceId, saves);
    std::vector<std::pair<std::string, std::string>> headers;
    headers.emplace_back("Content-Type", "application/json");
    appendAuthHeader(ctx, headers);
    std::string url = joinUrl(ctx.baseUrl, "/api/sync/negotiate");
    std::string body;
    if (!performRequest(ctx, "POST", url, headers, payload.data(), payload.size(), body, err)) return false;
    return parseNegotiateResponse(body, out);
}

bool completeSyncSession(const SyncAuthCtx& ctx, long long sessionId, int completed,
                         int failed, std::string& err) {
    std::string payload = serializeSyncCompleteBody(completed, failed);
    std::vector<std::pair<std::string, std::string>> headers;
    headers.emplace_back("Content-Type", "application/json");
    appendAuthHeader(ctx, headers);
    std::string url = joinUrl(ctx.baseUrl, "/api/sync/sessions/" + std::to_string(sessionId) + "/complete");
    std::string body;
    return performRequest(ctx, "POST", url, headers, payload.data(), payload.size(), body, err);
}

bool downloadAssetContent(const SyncAuthCtx& ctx, const char* kind, long long assetId,
                          const std::string& deviceId, std::string& bytesOut, std::string& err) {
    std::string url;
    if (std::string(kind) == "states") {
        url = joinUrl(ctx.baseUrl, "/api/states/" + std::to_string(assetId) + "/content");
        (void)deviceId;
    } else {
        url = joinUrl(ctx.baseUrl, "/api/saves/" + std::to_string(assetId) + "/content") +
              "?device_id=" + percentEncode(deviceId) + "&optimistic=false";
    }
    std::vector<std::pair<std::string, std::string>> headers;
    appendAuthHeader(ctx, headers);
    return performRequest(ctx, "GET", url, headers, nullptr, 0, bytesOut, err);
}

bool confirmSaveDownloaded(const SyncAuthCtx& ctx, long long saveId,
                           const std::string& deviceId, std::string& err) {
    std::string payload = "{\"device_id\":\"" + escapeJson(deviceId) + "\"}";
    std::vector<std::pair<std::string, std::string>> headers;
    headers.emplace_back("Content-Type", "application/json");
    appendAuthHeader(ctx, headers);
    std::string url = joinUrl(ctx.baseUrl, "/api/saves/" + std::to_string(saveId) + "/downloaded");
    std::string body;
    return performRequest(ctx, "POST", url, headers, payload.data(), payload.size(), body, err);
}

// Build and send a single-part multipart upload for the given field name.
static bool multipartUpload(const SyncAuthCtx& ctx, const std::string& method,
                            const std::string& url, const std::string& fieldName,
                            const std::string& filePath, std::string& err) {
    std::string data;
    if (!readFileBytes(filePath, data)) {
        err = "cannot read " + filePath;
        return false;
    }
    const std::string boundary = "romm-switch-" + std::to_string(data.size());
    std::vector<MultipartPart> parts;
    MultipartPart part;
    part.name = fieldName;
    part.fileName = fileBasename(filePath);
    part.contentType = "application/octet-stream";
    part.data = data;
    parts.push_back(part);
    std::string body = buildMultipartBody(parts, boundary);

    std::vector<std::pair<std::string, std::string>> headers;
    headers.emplace_back("Content-Type", "multipart/form-data; boundary=" + boundary);
    appendAuthHeader(ctx, headers);
    std::string response;
    return performRequest(ctx, method, url, headers, body.data(), body.size(), response, err);
}

bool uploadNewSave(const SyncAuthCtx& ctx, long long romId, const std::string& slot,
                   const std::string& emulator, const std::string& deviceId,
                   long long sessionId, const std::string& filePath, bool overwrite,
                   int autocleanupLimit, std::string& err) {
    std::string url = joinUrl(ctx.baseUrl, "/api/saves") +
                      "?rom_id=" + std::to_string(romId);
    if (!slot.empty()) url += "&slot=" + percentEncode(slot);
    if (!emulator.empty()) url += "&emulator=" + percentEncode(emulator);
    url += "&device_id=" + percentEncode(deviceId);
    if (sessionId > 0) url += "&session_id=" + std::to_string(sessionId);
    if (overwrite) url += "&overwrite=true";
    // Grout trims server-side slot history (autocleanup=true, limit=10 on the
    // 'autosave' slot); our SAVE_BACKUP_LIMIT setting (0 = no limit) maps onto it.
    url += "&autocleanup=true&autocleanup_limit=" + std::to_string(autocleanupLimit <= 0 ? 10 : autocleanupLimit);
    return multipartUpload(ctx, "POST", url, "saveFile", filePath, err);
}

bool updateExistingSave(const SyncAuthCtx& ctx, long long saveId,
                        const std::string& deviceId, const std::string& filePath, std::string& err) {
    std::string url = joinUrl(ctx.baseUrl, "/api/saves/" + std::to_string(saveId)) +
                      "?device_id=" + percentEncode(deviceId);
    return multipartUpload(ctx, "PUT", url, "saveFile", filePath, err);
}

bool uploadNewState(const SyncAuthCtx& ctx, long long romId, const std::string& emulator,
                    const std::string& filePath, std::string& err) {
    std::string url = joinUrl(ctx.baseUrl, "/api/states") +
                      "?rom_id=" + std::to_string(romId);
    if (!emulator.empty()) url += "&emulator=" + percentEncode(emulator);
    return multipartUpload(ctx, "POST", url, "stateFile", filePath, err);
}

bool updateExistingState(const SyncAuthCtx& ctx, long long stateId,
                         const std::string& filePath, std::string& err) {
    std::string url = joinUrl(ctx.baseUrl, "/api/states/" + std::to_string(stateId));
    return multipartUpload(ctx, "PUT", url, "stateFile", filePath, err);
}

} // namespace romm
