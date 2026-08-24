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
    REQUIRE(out.fat32Safe == false);
    REQUIRE(out.httpTimeoutSeconds == cfg.httpTimeoutSeconds);
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
