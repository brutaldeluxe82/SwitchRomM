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

TEST_CASE("JSON extract_archive parses and aliasing maps KEY variants") {
    // Default absent -> stays true.
    {
        romm::Config cfg;
        std::string err;
        REQUIRE(romm::parseJsonString(
            "{\"schema_version\":1,\"server_url\":\"http://x\",\"download_dir\":\"\"}",
            cfg, err));
        REQUIRE(cfg.extractArchive);
    }
    // Canonical snake_case false clears it.
    {
        romm::Config cfg;
        std::string err;
        REQUIRE(romm::parseJsonString(
            "{\"schema_version\":1,\"server_url\":\"http://x\",\"download_dir\":\"\",\"extract_archive\":false}",
            cfg, err));
        REQUIRE_FALSE(cfg.extractArchive);
    }
    // ENV-style uppercase alias maps via the legacy (schema_version 0) migration path.
    {
        romm::Config cfg;
        std::string err;
        // No schema_version -> v0 migration -> uppercase alias applied then canon getBool.
        REQUIRE(romm::parseJsonString(
            "{\"server_url\":\"http://x\",\"download_dir\":\"\",\"EXTRACT_ARCHIVE\":false}",
            cfg, err));
        REQUIRE_FALSE(cfg.extractArchive);
    }
    // camelCase alias also fires only in migration; schema_version 1 stays strict snake_case.
    {
        romm::Config cfg;
        std::string err;
        REQUIRE(romm::parseJsonString(
            "{\"server_url\":\"http://x\",\"download_dir\":\"\",\"extractArchive\":false}",
            cfg, err));
        REQUIRE_FALSE(cfg.extractArchive);
    }
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

TEST_CASE("effectiveSaveDir per layout") {
    romm::Config cfg;
    cfg.outputLayout = "tico";
    REQUIRE(romm::effectiveSaveDir(cfg) == "sdmc:/tico/saves");
    cfg.outputLayout = "retroarch";
    REQUIRE(romm::effectiveSaveDir(cfg) == "sdmc:/retroarch/.retroarch/saves");
}

TEST_CASE("effectiveStatesDir per layout") {
    romm::Config cfg;
    cfg.outputLayout = "tico";
    REQUIRE(romm::effectiveStatesDir(cfg) == "sdmc:/tico/states");
    cfg.outputLayout = "retroarch";
    REQUIRE(romm::effectiveStatesDir(cfg) == "sdmc:/retroarch/.retroarch/states");
}
