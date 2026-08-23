#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200112L
#endif

#include "romm/save_sync.hpp"

#include "mini/json.hpp"
#include "romm/http_common.hpp"
#include "romm/md5.hpp"

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

    std::string complete;
    if (getStringField(obj, "verification_path_complete", complete)) {
        out.verificationPathComplete = !complete.empty();
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
    if (rest.size() >= 2 && rest[0] == '.') {
        bool allDigits = !rest.empty();
        for (size_t i = 1; i < rest.size(); ++i) {
            if (rest[i] < '0' || rest[i] > '9') { allDigits = false; break; }
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
                             emulatorOf, out, unmatched, err);
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
        asset.romId = romMatcher(baseLower, "");
        out.push_back(asset);
        if (asset.romId == 0) unmatched.push_back(fileName);
    }
}

bool scanRoot(const std::string& root, bool isStateRoot, const RomMatcherFn& romMatcher,
              const EmulatorOfFn& emulatorOf, std::vector<LocalAsset>& out,
              std::vector<std::string>& unmatched, std::string& err) {
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) return true; // missing root is fine
    scanDirRecursive(root, 0, isStateRoot, romMatcher, emulatorOf, out, unmatched, err);
    return true;
}

} // namespace

ScanResult scanAssets(const std::string& savesRoot, const std::string& statesRoot,
                      const RomMatcherFn& romMatcher, const EmulatorOfFn& emulatorOf,
                      std::string& err) {
    err.clear();
    ScanResult result;
    scanRoot(savesRoot, false, romMatcher, emulatorOf, result.assets, result.unmatched, err);
    scanRoot(statesRoot, true, romMatcher, emulatorOf, result.assets, result.unmatched, err);
    return result;
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

// ---------- High-level wire ops ----------

bool fetchRemoteAssets(const SyncAuthCtx& ctx, const char* kind, std::vector<RemoteAsset>& out,
                       std::string& err) {
    out.clear();
    std::string url = joinUrl(ctx.baseUrl, std::string("/api/") + kind);
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
                   std::string& err) {
    std::string url = joinUrl(ctx.baseUrl, "/api/saves") +
                      "?rom_id=" + std::to_string(romId);
    if (!slot.empty()) url += "&slot=" + percentEncode(slot);
    if (!emulator.empty()) url += "&emulator=" + percentEncode(emulator);
    url += "&device_id=" + percentEncode(deviceId);
    url += "&session_id=" + std::to_string(sessionId);
    if (overwrite) url += "&overwrite=true";
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
