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

TEST_CASE("grout settings parse from env, JSON, and legacy aliases") {
    // Env keys parse (uppercase and lowercase both lowercased before match).
    {
        romm::Config cfg;
        std::string err;
        REQUIRE(romm::parseEnvString(
            "server_url=http://x\nSAVE_BACKUP_LIMIT=10\ndevice_name=Deck\n", cfg, err));
        REQUIRE(cfg.saveBackupLimit == 10);
        REQUIRE(cfg.deviceName == "Deck");
    }
    // Canonical snake_case at schema_version 1.
    {
        romm::Config cfg;
        std::string err;
        REQUIRE(romm::parseJsonString(
            "{\"schema_version\":1,\"server_url\":\"http://x\",\"save_backup_limit\":5,\"device_name\":\"Handheld\"}",
            cfg, err));
        REQUIRE(cfg.saveBackupLimit == 5);
        REQUIRE(cfg.deviceName == "Handheld");
    }
    // Uppercase env-style keys map via the schema_version 0 migration path.
    {
        romm::Config cfg;
        std::string err;
        REQUIRE(romm::parseJsonString(
            "{\"server_url\":\"http://x\",\"SAVE_BACKUP_LIMIT\":15,\"DOWNLOAD_TIMEOUT_MINUTES\":30,\"DEVICE_NAME\":\"Dock\"}",
            cfg, err));
        REQUIRE(cfg.saveBackupLimit == 15);
        REQUIRE(cfg.downloadTimeoutMinutes == 30);
        REQUIRE(cfg.deviceName == "Dock");
    }
}

TEST_CASE("save_sync_behavior default is ask") {
    romm::Config cfg;
    std::string err;
    REQUIRE(romm::parseEnvString("server_url=http://x\n", cfg, err));
    REQUIRE(cfg.saveSyncBehavior == "ask");
}

TEST_CASE("save_sync_behavior parses from env (case-normalized)") {
    romm::Config cfg;
    std::string err;
    REQUIRE(romm::parseEnvString(
        "server_url=http://x\nSAVE_SYNC_BEHAVIOR=Newest\n", cfg, err));
    REQUIRE(cfg.saveSyncBehavior == "newest");
}

TEST_CASE("save_sync_behavior parses from JSON and legacy uppercase alias") {
    // Canonical snake_case at schema_version 1.
    {
        romm::Config cfg;
        std::string err;
        REQUIRE(romm::parseJsonString(
            "{\"schema_version\":1,\"server_url\":\"http://x\",\"download_dir\":\"\",\"save_sync_behavior\":\"server\"}",
            cfg, err));
        REQUIRE(cfg.saveSyncBehavior == "server");
    }
    // Uppercase env-style key maps via the schema_version 0 migration path.
    {
        romm::Config cfg;
        std::string err;
        REQUIRE(romm::parseJsonString(
            "{\"server_url\":\"http://x\",\"download_dir\":\"\",\"SAVE_SYNC_BEHAVIOR\":\"client\"}",
            cfg, err));
        REQUIRE(cfg.saveSyncBehavior == "client");
    }
}

TEST_CASE("serializeConfigJson emits save_sync_behavior") {
    romm::Config cfg;
    cfg.saveSyncBehavior = "newest";
    const std::string json = romm::serializeConfigJson(cfg);
    REQUIRE(json.find("\"save_sync_behavior\":\"newest\"") != std::string::npos);
    // Default round-trip keeps the key present.
    romm::Config def;
    REQUIRE(romm::serializeConfigJson(def).find("\"save_sync_behavior\":\"ask\"") !=
            std::string::npos);
}
