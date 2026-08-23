#include "catch.hpp"
#include "romm/layout.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>


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

