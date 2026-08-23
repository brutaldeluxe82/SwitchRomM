#include "romm/layout.hpp"

#include <cctype>
#include <filesystem>
#include <vector>
#include <string>


namespace romm {

namespace {

std::string toLower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Canonical fs_slug -> platform folder under tico's roms root.
const std::vector<std::pair<const char*, const char*>>& ticoFolderMap() {
    static const std::vector<std::pair<const char*, const char*>> kMap = {
        {"nes", "nes"},
        {"snes", "snes"},
        {"gb", "gb"},
        {"gbc", "gba"},
        {"gba", "gba"},
        {"n64", "n64"},
        {"nds", "nds"},
        {"3ds", "3ds"},
        {"psx", "psx"},
        {"psp", "psp"},
        {"dreamcast", "dc"},
        {"saturn", "saturn"},
        {"segacd", "sega-cd"},
        {"sms", "master-system"},
        {"gamegear", "game-gear"},
        {"genesis", "genesis"},
        {"gc", "gc"},
        {"wii", "wii"},
        {"n64dd", "n64"},
        {"vb", "vb"},
        {"neogeo", "neogeo"},
        {"arcade", "arcade"},
        {"pcengine", "pc-engine"},
        {"tg16", "pc-engine"},
        {"sega32x", "32x"},
        {"sg1000", "sg1000"},
        {"msx", "msx"},
        {"atari2600", "atari2600"},
        {"atari5200", "atari5200"},
        {"atari7800", "atari7800"},
        {"lynx", "atarilynx"},
        {"vectrex", "vectrex"},
        {"zxspectrum", "zx-spectrum"},
        {"amiga", "amiga"},
        {"c64", "c64"},
        {"c128", "c128"},
        {"intellivision", "intellivision"},
        {"colecovision", "colecovision"},
        {"channelf", "fairchild-channel-f"},
        {"odyssey2", "odyssey-2"},
        {"pokemini", "pokemon-mini"},
        {"virtualboy", "virtualboy"},
        {"wswan", "wonderswan"},
        {"wswanc", "wonderswan-c"},
        {"ngp", "neogeo-pocket"},
        {"ngpc", "neogeo-pocket-color"},
        {"neogeocd", "neogeo-cd"},
        {"supergrafx", "supergrafx"},
        {"pcfx", "pc-fx"},
        {"tgcd", "tg16cd"},
        {"atarist", "atarist"},
        {"x68000", "x68000"},
        {"cavestory", "cave-story"},
        {"pico8", "pico8"},
        {"tic80", "tic-80"},
        {"gameandwatch", "game-and-watch"},
        {"arduboy", "arduboy"},
        {"megaduck", "mega-duck"},
        {"supervision", "supervision"},
        {"jaguar", "atari-jaguar"},
        {"3do", "3do"},
        {"amstradcpc", "amstradcpc"},
        {"dos", "dos"},
        {"ps2", "ps2"},
        {"openbor", "openbor"},
        {"fds", "fds"},
    };
    return kMap;
}

// RomM slug alias -> canonical fs_slug (used for both layouts).
const std::vector<std::pair<const char*, const char*>>& slugAliases() {
    static const std::vector<std::pair<const char*, const char*>> kAliases = {
        {"famicom", "nes"},
        {"megadrive", "genesis"},
        {"md", "genesis"},
        {"sega_cd", "segacd"},
        {"scd", "segacd"},
        {"mega-cd", "segacd"},
        {"mastersystem", "sms"},
        {"master-system", "sms"},
        {"dc", "dreamcast"},
        {"ps1", "psx"},
        {"ps", "psx"},
        {"playstation", "psx"},
        {"ngc", "gc"},
        {"gamecube", "gc"},
        {"nintendo-gamecube", "gc"},
        {"sfc", "snes"},
        {"sfam", "snes"},
        {"super-famicom", "snes"},
        {"gg", "gamegear"},
        {"sg-1000", "sg1000"},
        {"pce", "pcengine"},
        {"neogeo-aes", "neogeo"},
        {"neogeomvs", "neogeo"},
        {"virtualboy", "vb"},
        {"game-and-watch", "gameandwatch"},
        {"g-and-w", "gameandwatch"},
        {"atari-st", "atarist"},
        {"sharp-x68000", "x68000"},
        {"philips-cd-i", "cdi"},
        {"pokemon-mini", "pokemini"},
        {"neo-geo-pocket", "ngp"},
        {"neo-geo-pocket-color", "ngpc"},
        {"wonderswan", "wswan"},
        {"wonderswan-color", "wswanc"},
        {"turbografx-cd", "tgcd"},
        {"fairchild-channel-f", "channelf"},
    };
    return kAliases;
}


} // namespace

OutputLayout parseOutputLayout(const std::string& s) {
    if (toLower(s) == "retroarch") return OutputLayout::RetroArch;
    return OutputLayout::Tico;
}

const char* outputLayoutName(OutputLayout l) {
    return l == OutputLayout::RetroArch ? "retroarch" : "tico";
}

std::string normalizeSlug(const std::string& slug) {
    std::string lower = toLower(slug);
    for (const auto& kv : slugAliases()) {
        if (lower == kv.first) return kv.second;
    }
    return lower;
}

std::string layoutPlatformFolder(const std::string& rommSlug, OutputLayout layout) {
    std::string canon = normalizeSlug(rommSlug);
    if (layout == OutputLayout::RetroArch) {
        return canon;
    }
    for (const auto& kv : ticoFolderMap()) {
        if (canon == kv.first) return kv.second;
    }
    return "";
}

std::string defaultDownloadDir(OutputLayout layout) {
    return layout == OutputLayout::RetroArch ? "sdmc:/retroarch/downloads" : "sdmc:/tico/roms";
}

std::string defaultBiosDir(OutputLayout layout) {
    return layout == OutputLayout::RetroArch ? "sdmc:/retroarch/system" : "sdmc:/tico/system";
}

std::string layoutBiosFolder(const std::string& rommSlug, const std::string& biosRoot, OutputLayout layout) {
    if (layout == OutputLayout::RetroArch) {
        return biosRoot;
    }
    if (rommSlug.empty()) return biosRoot;
    std::string folder = layoutPlatformFolder(rommSlug, OutputLayout::Tico);
    if (folder.empty()) return biosRoot; // unknown platform -> bios root itself
    if (biosRoot.empty()) return biosRoot;
    return biosRoot + "/" + folder;
}


} // namespace romm
