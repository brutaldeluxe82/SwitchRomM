#include "catch.hpp"
#include "romm/config.hpp"

TEST_CASE("env output_layout=retroarch parses") {
    const std::string env =
        "server_url=http://example.com\n"
        "output_layout=retroarch\n";
    romm::Config cfg;
    std::string err;
    bool ok = romm::parseEnvString(env, cfg, err);
    REQUIRE(ok);
    REQUIRE(err.empty());
    REQUIRE(cfg.outputLayout == "retroarch");
}

TEST_CASE("JSON output_layout=retroarch parses") {
    const std::string json =
        "{"
        "\"schema_version\":1,"
        "\"server_url\":\"http://example.com\","
        "\"output_layout\":\"retroarch\","
        "\"download_dir\":\"\""
        "}";
    romm::Config cfg;
    std::string err;
    bool ok = romm::parseJsonString(json, cfg, err);
    REQUIRE(ok);
    REQUIRE(err.empty());
    REQUIRE(cfg.outputLayout == "retroarch");
}

TEST_CASE("empty download_dir + retroarch -> layout default download dir") {
    romm::Config cfg;
    cfg.serverUrl = "http://example.com";
    cfg.outputLayout = "retroarch";
    cfg.downloadDir.clear();
    REQUIRE(romm::effectiveDownloadDir(cfg) == "sdmc:/retroarch/downloads");
}

TEST_CASE("explicit download_dir overrides layout default") {
    romm::Config cfg;
    cfg.outputLayout = "tico";
    cfg.downloadDir = "sdmc:/custom/roms";
    REQUIRE(romm::effectiveDownloadDir(cfg) == "sdmc:/custom/roms");
}

TEST_CASE("bios_dir override respected by effectiveBiosDir") {
    romm::Config cfg;
    cfg.outputLayout = "retroarch";
    cfg.biosDir = "sdmc:/custom/bios";
    REQUIRE(romm::effectiveBiosDir(cfg) == "sdmc:/custom/bios");
}

TEST_CASE("empty bios_dir derives from layout") {
    romm::Config cfg;
    cfg.outputLayout = "retroarch";
    cfg.biosDir.clear();
    REQUIRE(romm::effectiveBiosDir(cfg) == "sdmc:/retroarch/system");
}
