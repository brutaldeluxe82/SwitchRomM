#include "catch.hpp"
#include <string>
#include "romm/save_sync.hpp"

using romm::DeviceAuthPollResult;
using romm::DeviceAuthPollState;
using romm::DeviceToken;

// ---------- Device token JSON round-trip ----------

TEST_CASE("device token serialize then parse round-trips") {
    romm::DeviceToken t;
    t.accessToken = "tok_abc";
    t.deviceId = "dev_123";
    t.clientDeviceIdentifier = "switch-01";
    std::string json = romm::serializeDeviceTokenJson(t);
    romm::DeviceToken out;
    REQUIRE(romm::parseDeviceTokenJson(json, out));
    REQUIRE(out.accessToken == "tok_abc");
    REQUIRE(out.deviceId == "dev_123");
    REQUIRE(out.clientDeviceIdentifier == "switch-01");
}

TEST_CASE("device token parse rejects empty object") {
    romm::DeviceToken out;
    REQUIRE_FALSE(romm::parseDeviceTokenJson("{}", out));
}

TEST_CASE("device token parse rejects garbage") {
    romm::DeviceToken out;
    REQUIRE_FALSE(romm::parseDeviceTokenJson("not json", out));
}

TEST_CASE("device token parse tolerates unknown extra fields") {
    romm::DeviceToken out;
    const std::string json = R"({"access_token":"tok","device_id":"d","client_device_identifier":"c","extra_field":123})";
    REQUIRE(romm::parseDeviceTokenJson(json, out));
    REQUIRE(out.accessToken == "tok");
    REQUIRE(out.deviceId == "d");
}

TEST_CASE("device token parse empty access_token rejected") {
    romm::DeviceToken out;
    REQUIRE_FALSE(romm::parseDeviceTokenJson(R"({"access_token":"","device_id":"d"})", out));
}

// ---------- classifyDeviceTokenResponse matrix ----------

TEST_CASE("classify 200 valid body -> Approved with tokens") {
    auto r = romm::classifyDeviceTokenResponse(200, R"({"access_token":"tok1","device_id":"dev1"})");
    REQUIRE(r.state == romm::DeviceAuthPollState::Approved);
    REQUIRE(r.accessToken == "tok1");
    REQUIRE(r.deviceId == "dev1");
}

TEST_CASE("classify 200 bad json -> Error") {
    auto r = romm::classifyDeviceTokenResponse(200, "not json");
    REQUIRE(r.state == romm::DeviceAuthPollState::Error);
}

TEST_CASE("classify 401 -> Error") {
    auto r = romm::classifyDeviceTokenResponse(401, R"({"detail":"unauthorized"})");
    REQUIRE(r.state == romm::DeviceAuthPollState::Error);
}

TEST_CASE("classify 400 authorization_pending -> Pending") {
    auto r = romm::classifyDeviceTokenResponse(400, R"({"detail":"authorization_pending"})");
    REQUIRE(r.state == romm::DeviceAuthPollState::Pending);
}

TEST_CASE("classify 400 slow_down -> SlowDown") {
    auto r = romm::classifyDeviceTokenResponse(400, R"({"detail":"slow_down"})");
    REQUIRE(r.state == romm::DeviceAuthPollState::SlowDown);
}

TEST_CASE("classify 400 access_denied -> AccessDenied") {
    auto r = romm::classifyDeviceTokenResponse(400, R"({"detail":"access_denied"})");
    REQUIRE(r.state == romm::DeviceAuthPollState::AccessDenied);
}

TEST_CASE("classify 400 expired_token -> ExpiredToken") {
    auto r = romm::classifyDeviceTokenResponse(400, R"({"detail":"expired_token"})");
    REQUIRE(r.state == romm::DeviceAuthPollState::ExpiredToken);
}

TEST_CASE("classify 400 unknown detail -> Error") {
    auto r = romm::classifyDeviceTokenResponse(400, R"({"detail":"something_else"})");
    REQUIRE(r.state == romm::DeviceAuthPollState::Error);
}

// ---------- serializeDeviceAuthInitBody ----------

TEST_CASE("init body contains all nine scopes") {
    romm::DeviceAuthInitRequest r;
    r.name = "Switch";
    r.client = "romm-switch-client";
    r.clientVersion = "0.2.8";
    r.clientDeviceIdentifier = "switch-01";
    std::string body = romm::serializeDeviceAuthInitBody(r);
    const char* scopes[] = {
        "me.read", "platforms.read", "roms.read", "collections.read",
        "firmware.read", "assets.read", "assets.write", "devices.read",
        "devices.write"};
    for (const char* s : scopes) {
        REQUIRE(body.find(s) != std::string::npos);
    }
    // Name appears escaped when it contains a quote.
    REQUIRE(body.find("requested_scopes") != std::string::npos);
}

TEST_CASE("init body escapes quotes in name") {
    romm::DeviceAuthInitRequest r;
    r.name = "My \"Console\"";
    std::string body = romm::serializeDeviceAuthInitBody(r);
    REQUIRE(body.find("\"My \\\"Console\\\"\"") != std::string::npos);
}

// ---------- parseDeviceAuthInitResponse ----------

TEST_CASE("init response happy path") {
    const std::string json = R"({
        "device_code":"dc1",
        "user_code":"ABCD-EFGH",
        "verification_path":"/pair/device",
        "verification_path_complete":"/pair/device?user_code=ABCD-EFGH",
        "expires_in":600,
        "interval":5
    })";
    romm::DeviceAuthInitResponse out;
    REQUIRE(romm::parseDeviceAuthInitResponse(json, out));
    REQUIRE(out.deviceCode == "dc1");
    REQUIRE(out.userCode == "ABCD-EFGH");
    REQUIRE(out.verificationPath == "/pair/device");
    REQUIRE(out.verificationPathComplete == true);
    REQUIRE(out.expiresIn == 600);
    REQUIRE(out.interval == 5);
}

TEST_CASE("init response missing device_code -> false") {
    romm::DeviceAuthInitResponse out;
    REQUIRE_FALSE(romm::parseDeviceAuthInitResponse(R"({"user_code":"ABCD"})", out));
}

// ---------- serverSupportsDeviceAuth ----------

TEST_CASE("serverSupportsDeviceAuth gate") {
    REQUIRE(romm::serverSupportsDeviceAuth("5.0.1"));
    REQUIRE_FALSE(romm::serverSupportsDeviceAuth("v4.8.0"));
    REQUIRE(romm::serverSupportsDeviceAuth("5"));
    REQUIRE(romm::serverSupportsDeviceAuth("5-beta"));
    REQUIRE_FALSE(romm::serverSupportsDeviceAuth("4"));
    REQUIRE_FALSE(romm::serverSupportsDeviceAuth("garbage"));
    REQUIRE_FALSE(romm::serverSupportsDeviceAuth(""));
}

// ---------- load/save round-trip via temp file ----------

TEST_CASE("save then load device token round-trips via file") {
    const char* path = "/tmp/romm_switch_client_test_device_token.json";
    romm::DeviceToken t;
    t.accessToken = "tok_file";
    t.deviceId = "dev_file";
    t.clientDeviceIdentifier = "switch-file-1";
    std::string err;
    REQUIRE(romm::saveDeviceToken(path, t, err));
    romm::DeviceToken out;
    REQUIRE(romm::loadDeviceToken(path, out));
    REQUIRE(out.accessToken == "tok_file");
    REQUIRE(out.deviceId == "dev_file");
    REQUIRE(out.clientDeviceIdentifier == "switch-file-1");
}

TEST_CASE("load missing file -> false") {
    romm::DeviceToken out;
    REQUIRE_FALSE(romm::loadDeviceToken("/tmp/definitely_not_present_token.json", out));
}
