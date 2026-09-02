#include "catch.hpp"
#include "romm/layout.hpp"
#include "romm/config.hpp"

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
    REQUIRE(romm::layoutPlatformFolder("dreamcast", romm::OutputLayout::Tico) == "dc");
    // Alias first normalized, then mapped.
    REQUIRE(romm::layoutPlatformFolder("famicom", romm::OutputLayout::Tico) == "nes");
    REQUIRE(romm::layoutPlatformFolder("megadrive", romm::OutputLayout::Tico) == "genesis");
    REQUIRE(romm::layoutPlatformFolder("gamegear", romm::OutputLayout::Tico) == "game-gear");
    // Unknown slug has no tico folder.
    REQUIRE(romm::layoutPlatformFolder("unknownslug", romm::OutputLayout::Tico).empty());
    REQUIRE(romm::layoutPlatformFolder("", romm::OutputLayout::Tico).empty());
    // Stock tico on-device roms folders (user-verified, all lowercase):
    // 3ds, atomiswave, dc, fbneo, game-gear, gb, gba, gbc, gc, genesis,
    // master-system, n64, naomi, nes, psp, psx, saturn, sega-cd, snes, wii.
    REQUIRE(romm::layoutPlatformFolder("3ds", romm::OutputLayout::Tico) == "3ds");
    REQUIRE(romm::layoutPlatformFolder("atomiswave", romm::OutputLayout::Tico) == "atomiswave");
    REQUIRE(romm::layoutPlatformFolder("naomi", romm::OutputLayout::Tico) == "naomi");
    REQUIRE(romm::layoutPlatformFolder("nds", romm::OutputLayout::Tico) == "nds");
    // RomM's canonical arcade slug maps to tico's fbneo folder; fbneo itself
    // is accepted as a direct entry.
    REQUIRE(romm::layoutPlatformFolder("arcade", romm::OutputLayout::Tico) == "fbneo");
    REQUIRE(romm::layoutPlatformFolder("fbneo", romm::OutputLayout::Tico) == "fbneo");
    REQUIRE(romm::layoutPlatformFolder("gb", romm::OutputLayout::Tico) == "gb");
    REQUIRE(romm::layoutPlatformFolder("gbc", romm::OutputLayout::Tico) == "gbc");
    REQUIRE(romm::layoutPlatformFolder("gba", romm::OutputLayout::Tico) == "gba");
    REQUIRE(romm::layoutPlatformFolder("ngc", romm::OutputLayout::Tico) == "gc");
    REQUIRE(romm::layoutPlatformFolder("gamecube", romm::OutputLayout::Tico) == "gc");
    REQUIRE(romm::layoutPlatformFolder("n64", romm::OutputLayout::Tico) == "n64");
    REQUIRE(romm::layoutPlatformFolder("nes", romm::OutputLayout::Tico) == "nes");
    REQUIRE(romm::layoutPlatformFolder("psp", romm::OutputLayout::Tico) == "psp");
    REQUIRE(romm::layoutPlatformFolder("psx", romm::OutputLayout::Tico) == "psx");
    REQUIRE(romm::layoutPlatformFolder("saturn", romm::OutputLayout::Tico) == "saturn");
    REQUIRE(romm::layoutPlatformFolder("snes", romm::OutputLayout::Tico) == "snes");
    REQUIRE(romm::layoutPlatformFolder("wii", romm::OutputLayout::Tico) == "wii");
}

TEST_CASE("platformFolderName applies layout map from config") {
    romm::Config tico;
    tico.outputLayout = "tico";
    // Slugs that differ from the on-device tico folder name.
    REQUIRE(romm::platformFolderName("dreamcast", tico) == "dc");
    REQUIRE(romm::platformFolderName("arcade", tico) == "fbneo");
    REQUIRE(romm::platformFolderName("sms", tico) == "master-system");
    REQUIRE(romm::platformFolderName("gamegear", tico) == "game-gear");
    REQUIRE(romm::platformFolderName("segacd", tico) == "sega-cd");
    REQUIRE(romm::platformFolderName("ngc", tico) == "gc");
    // Identical-name slugs pass through unchanged.
    REQUIRE(romm::platformFolderName("gba", tico) == "gba");
    // Unknown tico slug falls back to the canonical slug (never empty).
    REQUIRE(romm::platformFolderName("notaplatform", tico) == "notaplatform");
    REQUIRE(romm::platformFolderName("", tico).empty());
    // RetroArch layout: canonical slug is the folder.
    romm::Config ra;
    ra.outputLayout = "retroarch";
    REQUIRE(romm::platformFolderName("dreamcast", ra) == "dreamcast");
    REQUIRE(romm::platformFolderName("megadrive", ra) == "genesis");
}

TEST_CASE("platformSupportedByTico matches tico list (wiki + UI extras)") {
    // Official wiki "Supported ROM Formats" consoles (canonical slugs).
    REQUIRE(romm::platformSupportedByTico("nes"));
    REQUIRE(romm::platformSupportedByTico("fds")); // FDS ROMs live in nes/
    REQUIRE(romm::platformSupportedByTico("snes"));
    REQUIRE(romm::platformSupportedByTico("gb"));
    REQUIRE(romm::platformSupportedByTico("gbc"));
    REQUIRE(romm::platformSupportedByTico("gba"));
    REQUIRE(romm::platformSupportedByTico("sms"));
    REQUIRE(romm::platformSupportedByTico("gamegear"));
    REQUIRE(romm::platformSupportedByTico("genesis"));
    REQUIRE(romm::platformSupportedByTico("segacd"));
    REQUIRE(romm::platformSupportedByTico("saturn"));
    REQUIRE(romm::platformSupportedByTico("dreamcast"));
    REQUIRE(romm::platformSupportedByTico("psx"));
    REQUIRE(romm::platformSupportedByTico("psp"));
    REQUIRE(romm::platformSupportedByTico("gc"));
    REQUIRE(romm::platformSupportedByTico("wii"));
    // Shown in the tico UI but missing from the (out-of-date) wiki page.
    REQUIRE(romm::platformSupportedByTico("atomiswave"));
    REQUIRE(romm::platformSupportedByTico("naomi"));
    REQUIRE(romm::platformSupportedByTico("arcade")); // fbneo
    REQUIRE(romm::platformSupportedByTico("fbneo"));  // direct tico folder name
    REQUIRE(romm::platformSupportedByTico("3ds"));
    REQUIRE(romm::platformSupportedByTico("n64"));
    REQUIRE(romm::platformSupportedByTico("nds")); // tico-melonds fork in ticohq org
    // Aliases normalize into supported slugs.
    REQUIRE(romm::platformSupportedByTico("megadrive"));
    REQUIRE(romm::platformSupportedByTico("mastersystem"));
    REQUIRE(romm::platformSupportedByTico("famicom"));
    REQUIRE(romm::platformSupportedByTico("ps1"));
    REQUIRE(romm::platformSupportedByTico("SegaCD")); // case-insensitive
    // Have tico folders but not listed as supported.
    REQUIRE_FALSE(romm::platformSupportedByTico("ps2"));
    REQUIRE_FALSE(romm::platformSupportedByTico("atari2600"));
    // Unknown/empty never supported.
    REQUIRE_FALSE(romm::platformSupportedByTico("unknownslug"));
    REQUIRE_FALSE(romm::platformSupportedByTico(""));
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
