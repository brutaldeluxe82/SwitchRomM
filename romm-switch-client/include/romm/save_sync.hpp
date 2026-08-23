#pragma once

#include <string>

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

} // namespace romm
