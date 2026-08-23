#include "romm/save_sync.hpp"

#include "mini/json.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>
#include <system_error>
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

} // namespace romm
