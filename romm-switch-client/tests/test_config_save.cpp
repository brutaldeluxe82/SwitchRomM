#include "catch.hpp"
#include "romm/config.hpp"

// saveConfigJson writes what parseJsonString reads; round-trip must preserve
// values including characters the .env parser would corrupt (space, " #").
TEST_CASE("config json round-trip preserves fields") {
    romm::Config cfg;
    cfg.serverUrl = "http://192.168.1.50:8080";
    cfg.username = "olly";
    cfg.password = "plain-pass";
    cfg.outputLayout = "retroarch";
    cfg.downloadDir = "sdmc:/romDownloads";
    cfg.platform = "switch";

    const std::string json = romm::serializeConfigJson(cfg);
    romm::Config out;
    std::string err;
    REQUIRE(romm::parseJsonString(json, out, err));

    REQUIRE(out.schemaVersion == 1);
    REQUIRE(out.serverUrl == cfg.serverUrl);
    REQUIRE(out.username == cfg.username);
    REQUIRE(out.password == cfg.password);
    REQUIRE(out.outputLayout == cfg.outputLayout);
    REQUIRE(out.downloadDir == cfg.downloadDir);
    REQUIRE(out.platform == cfg.platform);
    REQUIRE(out.extractArchive == true);
    REQUIRE(out.hideUnsupportedPlatforms == true); // default-on
    REQUIRE(out.fat32Safe == false);
}

TEST_CASE("config json round-trip escapes hostile password values") {
    romm::Config cfg;
    cfg.serverUrl = "http://example.com";
    cfg.username = "u";
    // Space + inline-comment trigger, quotes, backslash.
    cfg.password = "p w #not-a-com\"ment\\ q;tail";

    const std::string json = romm::serializeConfigJson(cfg);
    romm::Config out;
    std::string err;
    REQUIRE(romm::parseJsonString(json, out, err));
    REQUIRE(out.username == "u");
    REQUIRE(out.password == cfg.password);
}

TEST_CASE("config json does not persist api token") {
    romm::Config cfg;
    cfg.serverUrl = "http://example.com";
    cfg.apiToken = "super-secret-bearer";

    const std::string json = romm::serializeConfigJson(cfg);
    REQUIRE(json.find("super-secret-bearer") == std::string::npos);
    romm::Config out;
    std::string err;
    REQUIRE(romm::parseJsonString(json, out, err));
    REQUIRE(out.apiToken.empty());
}

TEST_CASE("config json round-trips grout settings") {
    romm::Config cfg;
    cfg.serverUrl = "http://example.com";
    cfg.saveBackupLimit = 5;
    cfg.downloadTimeoutMinutes = 90;
    cfg.deviceName = "Bedroom Switch";

    const std::string json = romm::serializeConfigJson(cfg);
    REQUIRE(json.find("\"save_backup_limit\":5") != std::string::npos);
    REQUIRE(json.find("\"download_timeout_minutes\":90") != std::string::npos);
    REQUIRE(json.find("\"device_name\":\"Bedroom Switch\"") != std::string::npos);

    romm::Config out;
    std::string err;
    REQUIRE(romm::parseJsonString(json, out, err));
    REQUIRE(out.saveBackupLimit == 5);
    REQUIRE(out.deviceName == "Bedroom Switch");
    // Hide-unsupported toggle: off round-trips as off.
    cfg.hideUnsupportedPlatforms = false;
    const std::string json2 = romm::serializeConfigJson(cfg);
    REQUIRE(json2.find("\"hide_unsupported_platforms\":false") != std::string::npos);
    romm::Config out2;
    REQUIRE(romm::parseJsonString(json2, out2, err));
    REQUIRE(out2.hideUnsupportedPlatforms == false);
    // Back on.
    cfg.hideUnsupportedPlatforms = true;
    const std::string json3 = romm::serializeConfigJson(cfg);
    REQUIRE(json3.find("\"hide_unsupported_platforms\":true") != std::string::npos);
    romm::Config out3;
    REQUIRE(romm::parseJsonString(json3, out3, err));
    REQUIRE(out3.hideUnsupportedPlatforms == true);
}

TEST_CASE("config json clamps grout settings on parse") {
    romm::Config cfg;
    cfg.serverUrl = "http://example.com";
    cfg.saveBackupLimit = -1;
    cfg.downloadTimeoutMinutes = 999;

    const std::string json = romm::serializeConfigJson(cfg);
    romm::Config out;
    std::string err;
    REQUIRE(romm::parseJsonString(json, out, err));
    REQUIRE(out.saveBackupLimit == 0);
    REQUIRE(out.downloadTimeoutMinutes == 120);
}


TEST_CASE("config json defaults for legacy files without grout keys") {
    // Old config files lack the new keys entirely: parse must leave defaults.
    const std::string json =
        "{\"schema_version\":1,\"server_url\":\"http://example.com\"}";
    romm::Config cfg;
    std::string err;
    REQUIRE(romm::parseJsonString(json, cfg, err));
    REQUIRE(cfg.saveBackupLimit == 0);
    REQUIRE(cfg.downloadTimeoutMinutes == 60);
    REQUIRE(cfg.hideUnsupportedPlatforms == true); // new key defaults on
    REQUIRE(cfg.deviceName == "Switch");
    // Serialized defaults keep the keys present for future round-trips.
    const std::string defJson = romm::serializeConfigJson(cfg);
    REQUIRE(defJson.find("\"save_backup_limit\":0") != std::string::npos);
    REQUIRE(defJson.find("\"download_timeout_minutes\":60") != std::string::npos);
    REQUIRE(defJson.find("\"device_name\":\"Switch\"") != std::string::npos);
    REQUIRE(defJson.find("\"hide_unsupported_platforms\":true") != std::string::npos);
}
