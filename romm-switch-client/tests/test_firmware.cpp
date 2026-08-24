#include "catch.hpp"
#include <string>
#include <vector>
#include "romm/api.hpp"
#include "romm/firmware.hpp"
#include "romm/config.hpp"

using romm::Config;
using romm::Firmware;

// ---------- parseFirmwareList ----------

TEST_CASE("parseFirmwareList bare array with numeric values") {
    const std::string body = R"([
        {"id": 11, "file_name": "psxonpsp660.bin", "file_size_bytes": 1234},
        {"id": "12", "file_name": "scph5501.bin", "file_size_bytes": 524288},
        {"id": "drive_1.0.0_playstation.bin", "file_name": "drive.bin"}
    ])";
    std::vector<Firmware> fw;
    std::string err;
    bool ok = romm::parseFirmwareListTest(body, fw, err);
    REQUIRE(ok);
    REQUIRE(fw.size() == 3);
    REQUIRE(fw[0].id == 11);
    REQUIRE(fw[0].fileName == "psxonpsp660.bin");
    REQUIRE(fw[0].fileSizeBytes == 1234);
    // string numeric id handled
    REQUIRE(fw[1].id == 12);
    REQUIRE(fw[1].fileSizeBytes == 524288);
    // missing file_size_bytes tolerated (0); non-numeric id becomes 0
    REQUIRE(fw[2].id == 0);
    REQUIRE(fw[2].fileName == "drive.bin");
    REQUIRE(fw[2].fileSizeBytes == 0);
}

TEST_CASE("parseFirmwareList envelope {items}") {
    const std::string body = R"({
        "items": [
            {"id": 1, "file_name": "a.bin", "file_size_bytes": 10}
        ]
    })";
    std::vector<Firmware> fw;
    std::string err;
    bool ok = romm::parseFirmwareListTest(body, fw, err);
    REQUIRE(ok);
    REQUIRE(fw.size() == 1);
    REQUIRE(fw[0].fileName == "a.bin");
    REQUIRE(fw[0].fileSizeBytes == 10);
}

TEST_CASE("parseFirmwareList empty array yields empty success") {
    std::vector<Firmware> fw;
    std::string err;
    bool ok = romm::parseFirmwareListTest("[]", fw, err);
    REQUIRE(ok);
    REQUIRE(fw.empty());
}

TEST_CASE("parseFirmwareList empty envelope yields empty success") {
    std::vector<Firmware> fw;
    std::string err;
    bool ok = romm::parseFirmwareListTest(R"({"items": []})", fw, err);
    REQUIRE(ok);
    REQUIRE(fw.empty());
}

TEST_CASE("parseFirmwareList malformed returns false") {
    std::vector<Firmware> fw;
    std::string err;
    bool ok = romm::parseFirmwareListTest("not json", fw, err);
    REQUIRE_FALSE(ok);
    REQUIRE(!err.empty());
}

TEST_CASE("parseFirmwareList envelope missing items returns false") {
    std::vector<Firmware> fw;
    std::string err;
    bool ok = romm::parseFirmwareListTest(R"({"foo": 1})", fw, err);
    REQUIRE_FALSE(ok);
    REQUIRE(!err.empty());
}

TEST_CASE("parseFirmwareList drops entries with empty file_name") {
    const std::string body = R"([
        {"id": 1, "file_name": "", "file_size_bytes": 1},
        {"id": 2, "file_name": "keep.bin", "file_size_bytes": 2}
    ])";
    std::vector<Firmware> fw;
    std::string err;
    bool ok = romm::parseFirmwareListTest(body, fw, err);
    REQUIRE(ok);
    REQUIRE(fw.size() == 1);
    REQUIRE(fw[0].fileName == "keep.bin");
}

// ---------- biosDestinationDir ----------

TEST_CASE("biosDestinationDir tico default: psx -> system/psx") {
    Config cfg;
    cfg.outputLayout = "tico";
    cfg.biosDir = ""; // derive from layout -> sdmc:/tico/system
    REQUIRE(romm::biosDestinationDir(cfg, "psx") == "sdmc:/tico/system/psx");
}

TEST_CASE("biosDestinationDir tico: segacd -> system/genesis (Tico wiki BIOS layout)") {
    Config cfg;
    cfg.outputLayout = "tico";
    cfg.biosDir = "";
    REQUIRE(romm::biosDestinationDir(cfg, "segacd") == "sdmc:/tico/system/genesis");
}

TEST_CASE("biosDestinationDir tico matches documented Tico BIOS folders") {
    // Contract per https://ticoverse.com/wiki/library-and-roms/bios-setup
    Config cfg;
    cfg.outputLayout = "tico";
    cfg.biosDir = "";
    REQUIRE(romm::biosDestinationDir(cfg, "psx") == "sdmc:/tico/system/psx");
    REQUIRE(romm::biosDestinationDir(cfg, "dreamcast") == "sdmc:/tico/system/dc");
    REQUIRE(romm::biosDestinationDir(cfg, "saturn") == "sdmc:/tico/system/saturn");
    REQUIRE(romm::biosDestinationDir(cfg, "segacd") == "sdmc:/tico/system/genesis");
    REQUIRE(romm::biosDestinationDir(cfg, "genesis") == "sdmc:/tico/system/genesis");
}

TEST_CASE("biosDestinationDir explicit biosDir respected on tico segacd override") {
    Config cfg;
    cfg.outputLayout = "tico";
    cfg.biosDir = "sdmc:/custom/bios";
    REQUIRE(romm::biosDestinationDir(cfg, "segacd") == "sdmc:/custom/bios/genesis");
}

TEST_CASE("biosDestinationDir tico unknown slug falls back to bios root") {
    Config cfg;
    cfg.outputLayout = "tico";
    cfg.biosDir = "sdmc:/custom/bios";
    REQUIRE(romm::biosDestinationDir(cfg, "totally_unknown_platform") == "sdmc:/custom/bios");
}

TEST_CASE("biosDestinationDir retroarch flat layout") {
    Config cfg;
    cfg.outputLayout = "retroarch";
    cfg.biosDir = ""; // derive -> sdmc:/retroarch/system
    REQUIRE(romm::biosDestinationDir(cfg, "psx") == "sdmc:/retroarch/system");
    REQUIRE(romm::biosDestinationDir(cfg, "segacd") == "sdmc:/retroarch/system");
}

TEST_CASE("biosDestinationDir explicit biosDir respected on tico") {
    Config cfg;
    cfg.outputLayout = "tico";
    cfg.biosDir = "sdmc:/custom/bios";
    REQUIRE(romm::biosDestinationDir(cfg, "psx") == "sdmc:/custom/bios/psx");
}

// ---------- firmwareNeedsDownload ----------

TEST_CASE("firmwareNeedsDownload skip when file exists with matching size") {
    REQUIRE_FALSE(romm::firmwareNeedsDownload(true, 100, 100));
}

TEST_CASE("firmwareNeedsDownload download when absent") {
    REQUIRE(romm::firmwareNeedsDownload(false, 0, 100));
    REQUIRE(romm::firmwareNeedsDownload(false, 0, 0));
}

TEST_CASE("firmwareNeedsDownload download when size differs") {
    REQUIRE(romm::firmwareNeedsDownload(true, 99, 100));
    REQUIRE(romm::firmwareNeedsDownload(true, 0, 100));
}

TEST_CASE("firmwareNeedsDownload remote size 0 forces download of present file") {
    // A present file with a recorded remote size of 0 (>0 local) is a mismatch -> download.
    REQUIRE(romm::firmwareNeedsDownload(true, 5, 0));
}
