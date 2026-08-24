#include "catch.hpp"
#include "romm/layout.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <minizip/zip.h>

namespace fs = std::filesystem;

// Write a small zip archive to disk using the classic minizip writer API.
static bool makeZip(const std::string& zipPath,
                    const std::vector<std::pair<std::string, std::string>>& files,
                    std::string& err) {
    zipFile zf = zipOpen64(zipPath.c_str(), 0);
    if (!zf) {
        err = "zipOpen64 failed";
        return false;
    }
    for (const auto& f : files) {
        // Allow nested dirs by creating parent dirs implicitly through the entry name.
        zip_fileinfo zfi = {};
        if (zipOpenNewFileInZip64(zf, f.first.c_str(), &zfi, nullptr, 0, nullptr, 0, nullptr,
                                  Z_DEFLATED, Z_DEFAULT_COMPRESSION, 0) != ZIP_OK) {
            zipClose(zf, nullptr);
            err = "zipOpenNewFileInZip64 failed for " + f.first;
            return false;
        }
        if (zipWriteInFileInZip(zf, f.second.data(), static_cast<unsigned>(f.second.size())) != ZIP_OK) {
            zipClose(zf, nullptr);
            err = "zipWriteInFileInZip failed for " + f.first;
            return false;
        }
        if (zipCloseFileInZip(zf) != ZIP_OK) {
            zipClose(zf, nullptr);
            err = "zipCloseFileInZip failed";
            return false;
        }
    }
    if (zipClose(zf, nullptr) != ZIP_OK) {
        err = "zipClose failed";
        return false;
    }
    return true;
}

static std::string readWholeFile(const fs::path& p) {
    std::ifstream in(p.string(), std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

TEST_CASE("parseOutputLayout round-trips both values and defaults to Tico") {
    REQUIRE(romm::parseOutputLayout("tico") == romm::OutputLayout::Tico);
    REQUIRE(romm::parseOutputLayout("retroarch") == romm::OutputLayout::RetroArch);
    REQUIRE(romm::parseOutputLayout("RETROARCH") == romm::OutputLayout::RetroArch);
    REQUIRE(romm::parseOutputLayout("RetroArch") == romm::OutputLayout::RetroArch);
    REQUIRE(romm::parseOutputLayout("garbage") == romm::OutputLayout::Tico);
    REQUIRE(romm::parseOutputLayout("") == romm::OutputLayout::Tico);

    REQUIRE(std::string(romm::outputLayoutName(romm::OutputLayout::Tico)) == "tico");
    REQUIRE(std::string(romm::outputLayoutName(romm::OutputLayout::RetroArch)) == "retroarch");
}

TEST_CASE("normalizeSlug canonicalizes known variants and passes unknown through") {
    struct Case { const char* in; const char* out; };
    const Case cases[] = {
        {"famicom", "nes"}, {"megadrive", "genesis"}, {"md", "genesis"},
        {"sega_cd", "segacd"}, {"segacd", "segacd"}, {"scd", "segacd"}, {"mega-cd", "segacd"},
        {"mastersystem", "sms"}, {"master-system", "sms"},
        {"dc", "dreamcast"}, {"dreamcast", "dreamcast"},
        {"ps1", "psx"}, {"ps", "psx"}, {"playstation", "psx"},
        {"ngc", "gc"}, {"gamecube", "gc"}, {"nintendo-gamecube", "gc"},
        {"sfc", "snes"}, {"sfam", "snes"}, {"super-famicom", "snes"},
        {"gg", "gamegear"}, {"sg-1000", "sg1000"},
        {"pce", "pcengine"}, {"pcengine", "pcengine"},
        {"neogeo-aes", "neogeo"}, {"neogeomvs", "neogeo"},
        {"virtualboy", "vb"}, {"game-and-watch", "gameandwatch"}, {"g-and-w", "gameandwatch"},
        {"atari-st", "atarist"}, {"sharp-x68000", "x68000"}, {"philips-cd-i", "cdi"},
        {"pokemon-mini", "pokemini"},
        {"neo-geo-pocket", "ngp"}, {"neo-geo-pocket-color", "ngpc"},
        {"wonderswan", "wswan"}, {"wonderswan-color", "wswanc"},
        {"turbografx-cd", "tgcd"}, {"fairchild-channel-f", "channelf"},
        // Unknown slugs pass through lowercased.
        {"SuperWEIRD", "superweird"}, {"unknownslug", "unknownslug"},
    };
    for (const auto& c : cases) {
        INFO("normalizeSlug(" << c.in << ")");
        REQUIRE(romm::normalizeSlug(c.in) == c.out);
    }
}

TEST_CASE("layoutPlatformFolder maps tico spot checks") {
    REQUIRE(romm::layoutPlatformFolder("genesis", romm::OutputLayout::Tico) == "genesis");
    REQUIRE(romm::layoutPlatformFolder("sms", romm::OutputLayout::Tico) == "master-system");
    REQUIRE(romm::layoutPlatformFolder("segacd", romm::OutputLayout::Tico) == "sega-cd");
    REQUIRE(romm::layoutPlatformFolder("dc", romm::OutputLayout::Tico) == "dc");
    // Alias first normalized, then mapped.
    REQUIRE(romm::layoutPlatformFolder("famicom", romm::OutputLayout::Tico) == "nes");
    REQUIRE(romm::layoutPlatformFolder("megadrive", romm::OutputLayout::Tico) == "genesis");
    REQUIRE(romm::layoutPlatformFolder("gamegear", romm::OutputLayout::Tico) == "game-gear");
    // Unknown slug has no tico folder.
    REQUIRE(romm::layoutPlatformFolder("unknownslug", romm::OutputLayout::Tico).empty());
    REQUIRE(romm::layoutPlatformFolder("", romm::OutputLayout::Tico).empty());
}

TEST_CASE("layoutPlatformFolder retroarch returns normalized slug") {
    REQUIRE(romm::layoutPlatformFolder("megadrive", romm::OutputLayout::RetroArch) == "genesis");
    REQUIRE(romm::layoutPlatformFolder("famicom", romm::OutputLayout::RetroArch) == "nes");
    REQUIRE(romm::layoutPlatformFolder("snes", romm::OutputLayout::RetroArch) == "snes");
    REQUIRE(romm::layoutPlatformFolder("unknownslug", romm::OutputLayout::RetroArch) == "unknownslug");
}

TEST_CASE("defaultDownloadDir and defaultBiosDir per layout") {
    REQUIRE(romm::defaultDownloadDir(romm::OutputLayout::Tico) == "sdmc:/tico/roms");
    REQUIRE(romm::defaultDownloadDir(romm::OutputLayout::RetroArch) == "sdmc:/retroarch/downloads");
    REQUIRE(romm::defaultBiosDir(romm::OutputLayout::Tico) == "sdmc:/tico/system");
    REQUIRE(romm::defaultBiosDir(romm::OutputLayout::RetroArch) == "sdmc:/retroarch/system");
}

TEST_CASE("layoutBiosFolder tico nested, retroarch flat, empty fallback") {
    REQUIRE(romm::layoutBiosFolder("psx", "sdmc:/tico/system", romm::OutputLayout::Tico) == "sdmc:/tico/system/psx");
    REQUIRE(romm::layoutBiosFolder("psx", "sdmc:/retroarch/system", romm::OutputLayout::RetroArch) == "sdmc:/retroarch/system");
    REQUIRE(romm::layoutBiosFolder("", "sdmc:/tico/system", romm::OutputLayout::Tico) == "sdmc:/tico/system");
    // Unknown folder on tico falls back to biosRoot itself.
    REQUIRE(romm::layoutBiosFolder("unknownslug", "sdmc:/tico/system", romm::OutputLayout::Tico) == "sdmc:/tico/system");
}

TEST_CASE("layoutRequiresExtraction per layout and arcade-class slugs") {
    using romm::OutputLayout;
    // tico extracts console ROMs regardless of slug.
    REQUIRE(romm::layoutRequiresExtraction(OutputLayout::Tico, "snes"));
    REQUIRE(romm::layoutRequiresExtraction(OutputLayout::Tico, "nes"));
    REQUIRE(romm::layoutRequiresExtraction(OutputLayout::Tico, "psx"));
    // Arcade-class slugs keep zip romsets packed for FBNeo/mame cores.
    REQUIRE_FALSE(romm::layoutRequiresExtraction(OutputLayout::Tico, "arcade"));
    REQUIRE_FALSE(romm::layoutRequiresExtraction(OutputLayout::Tico, "mame"));
    REQUIRE_FALSE(romm::layoutRequiresExtraction(OutputLayout::Tico, "fbneo"));
    REQUIRE_FALSE(romm::layoutRequiresExtraction(OutputLayout::Tico, "fba"));
    REQUIRE_FALSE(romm::layoutRequiresExtraction(OutputLayout::Tico, "Arcade"));
    // retroarch never extracts.
    REQUIRE_FALSE(romm::layoutRequiresExtraction(OutputLayout::RetroArch, "snes"));
    REQUIRE_FALSE(romm::layoutRequiresExtraction(OutputLayout::RetroArch, "arcade"));
}

TEST_CASE("extractZipToDir extracts files and nested dirs byte-equal") {
    fs::path root = fs::temp_directory_path() / "romm_layout_test_extract";
    fs::create_directories(root);
    std::string err;
    std::vector<std::pair<std::string, std::string>> files = {
        {"a.txt", "hello a"},
        {"sub/b.txt", "nested content"},
        {"sub/deep/c.txt", "deeper"},
    };
    REQUIRE(makeZip((root / "in.zip").string(), files, err));

    fs::path outDir = root / "out";
    fs::create_directories(outDir);
    REQUIRE(romm::extractZipToDir((root / "in.zip").string(), outDir.string(), err));

    REQUIRE(readWholeFile(outDir / "a.txt") == "hello a");
    REQUIRE(readWholeFile(outDir / "sub/b.txt") == "nested content");
    REQUIRE(readWholeFile(outDir / "sub/deep/c.txt") == "deeper");
    REQUIRE(fs::is_directory(outDir / "sub/deep"));

    fs::remove_all(root);
}

TEST_CASE("extractZipToDir rejects zip-slip entries") {
    fs::path root = fs::temp_directory_path() / "romm_layout_test_slip";
    fs::create_directories(root);
    std::string err;
    // Craft an archive with a malicious "../evil.txt" entry plus a benign file.
    std::vector<std::pair<std::string, std::string>> files = {
        {"../evil.txt", "boom"},
        {"good.txt", "fine"},
    };
    REQUIRE(makeZip((root / "slip.zip").string(), files, err));

    fs::path outDir = root / "out";
    fs::create_directories(outDir);
    REQUIRE(romm::extractZipToDir((root / "slip.zip").string(), outDir.string(), err));

    // The safe entry was extracted; the malicious one must not escape outDir.
    REQUIRE(readWholeFile(outDir / "good.txt") == "fine");
    REQUIRE_FALSE(fs::exists(root / "evil.txt"));
    REQUIRE_FALSE(fs::exists(outDir / ".." / "evil.txt"));

    fs::remove_all(root);
}

TEST_CASE("extractZipToDir fails on missing archive") {
    std::string err;
    REQUIRE_FALSE(romm::extractZipToDir("/no/such/file.zip", "/no/such/out", err));
    REQUIRE_FALSE(err.empty());
}
