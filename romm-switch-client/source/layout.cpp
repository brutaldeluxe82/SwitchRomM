#include "romm/layout.hpp"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <vector>
#include <string>

#include <minizip/unzip.h>

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

// Sanitize a zip entry name into a safe relative path under destDir.
// Returns "" if the entry is unsafe (absolute, drive letter, or ".." component).
std::string sanitizeZipEntry(const std::string& raw) {
    std::string name = raw;
    // Normalize backslashes to forward slashes (zip-slip defense-in-depth).
    for (auto& c : name) {
        if (c == '\\') c = '/';
    }
    if (name.empty()) return "";
    // Reject absolute paths (unix) and drive-letter / UNC (windows) prefixes.
    if (name[0] == '/') return "";
    if (name.size() >= 2 && name[1] == ':') return "";
    if (name.rfind("//", 0) == 0) return "";
    // Split into components and reject any ".." (or ".") segments.
    std::vector<std::string> comps;
    size_t start = 0;
    while (start <= name.size()) {
        size_t slash = name.find('/', start);
        std::string comp = name.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (comp == "..") return "";
        if (!comp.empty() && comp != ".") comps.push_back(comp);
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    std::string out;
    for (size_t i = 0; i < comps.size(); ++i) {
        if (i) out.push_back('/');
        out += comps[i];
    }
    return out;
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
bool layoutRequiresExtraction(OutputLayout layout, const std::string& rommSlug) {
    if (layout == OutputLayout::RetroArch) return false;
    // Zip-based arcade romsets stay packed: tico's FBNeo build is libretro
    // block_extract and hands the archive to the core intact
    // (valid_extensions "zip|7z", ticohq/tico-fbneo src/burner/libretro).
    std::string slug = toLower(rommSlug);
    return slug != "arcade" && slug != "mame" && slug != "fba" && slug != "fbneo";
}

bool extractZipToDir(const std::string& zipPath, const std::string& destDir, std::string& err) {
    err.clear();
    unzFile zf = unzOpen64(zipPath.c_str());
    if (!zf) {
        err = "Failed to open zip: " + zipPath;
        return false;
    }

    bool ok = true;
    const size_t kChunk = 64 * 1024;
    std::vector<char> buf(kChunk);

    int rc = unzGoToFirstFile(zf);
    while (rc == UNZ_OK) {
        unz_file_info64 info;
        char entryName[1024];
        std::memset(entryName, 0, sizeof(entryName));
        if (unzGetCurrentFileInfo64(zf, &info, entryName, sizeof(entryName), nullptr, 0, nullptr, 0) != UNZ_OK) {
            err = "Failed to read zip entry info";
            ok = false;
            break;
        }

        bool skip = false;
        std::string rel;
        {
            std::string raw = entryName;
            bool isDir = !raw.empty() && raw.back() == '/';
            if (!isDir) {
                rel = sanitizeZipEntry(raw);
                if (rel.empty()) {
                    // Unsafe (zip-slip) entry: skip it entirely rather than write outside dest.
                    skip = true;
                }
            } else {
                skip = true; // directory records don't need writing
            }
        }

        if (!skip) {
            if (unzOpenCurrentFile(zf) != UNZ_OK) {
                err = "Failed to open zip entry: " + rel;
                ok = false;
                break;
            }
            std::filesystem::path outPath = std::filesystem::path(destDir) / rel;
            std::error_code ec;
            // Create parent directories (error_code overload: no exceptions).
            std::filesystem::create_directories(outPath.parent_path(), ec);
            if (ec) {
                err = "Failed to create directory for " + rel + ": " + ec.message();
                unzCloseCurrentFile(zf);
                ok = false;
                break;
            }
            std::ofstream ofs(outPath.string(), std::ios::binary | std::ios::trunc);
            if (!ofs) {
                err = "Failed to open output file: " + outPath.string();
                unzCloseCurrentFile(zf);
                ok = false;
                break;
            }
            int readBytes = 0;
            bool writeFailed = false;
            while ((readBytes = unzReadCurrentFile(zf, buf.data(), static_cast<unsigned>(buf.size()))) > 0) {
                ofs.write(buf.data(), readBytes);
                if (!ofs) {
                    writeFailed = true;
                    break;
                }
            }
            if (readBytes < 0) {
                err = "Failed reading zip entry: " + rel;
                writeFailed = true;
            }
            ofs.close();
            unzCloseCurrentFile(zf);
            if (writeFailed) {
                std::error_code fec;
                std::filesystem::remove(outPath, fec); // best-effort cleanup of partial entry
                err = err.empty() ? "Failed writing entry: " + rel : err;
                ok = false;
                break;
            }
        }

        rc = unzGoToNextFile(zf);
    }

    if (ok && rc != UNZ_END_OF_LIST_OF_FILE) {
        err = "Unexpected error iterating zip entries";
        ok = false;
    }

    unzClose(zf);
    if (!ok) {
        return false;
    }
    return true;
}

} // namespace romm
