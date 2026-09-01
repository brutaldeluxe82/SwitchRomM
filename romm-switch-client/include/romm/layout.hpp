#pragma once

#include <string>
#include <vector>

namespace romm {

// Output layout describes how downloaded ROMs are organized on the SD card.
enum class OutputLayout { Tico, RetroArch };

// Case-insensitive; "retroarch" -> RetroArch; anything else -> Tico.
OutputLayout parseOutputLayout(const std::string& s);

// "tico" | "retroarch"
const char* outputLayoutName(OutputLayout l);

// Normalize any RomM slug variant to the canonical fs_slug used for folder names.
// Unknown slugs are returned lowercased and unchanged.
std::string normalizeSlug(const std::string& slug);

// Destination platform folder name under the layout's roms root.
// Returns "" when the layout has no folder for this slug.
std::string layoutPlatformFolder(const std::string& rommSlug, OutputLayout layout);

// Layout default download roots:
//   tico: "sdmc:/tico/roms"; retroarch: "sdmc:/retroarch/downloads"
std::string defaultDownloadDir(OutputLayout layout);
//   tico: "sdmc:/tico/system"; retroarch: "sdmc:/retroarch/system"
std::string defaultBiosDir(OutputLayout layout);

// Reverse of the tico folder map: given an on-disk ROM folder name (e.g. from
// sdmc:/tico/roms/<folder>/), return every canonical fs_slug whose folder is
// this one, in table order. Collisions are real ("gba" <- {gbc,gba},
// "pc-engine" <- {pcengine,tg16}, "n64" <- {n64,n64dd}); the caller must try
// each candidate against the server's platforms.
std::vector<std::string> ticoFolderToCanonicalSlugs(const std::string& folder);

// True when the given RomM platform slug maps to one of tico's supported
// consoles. Base list: official wiki (ticoverse.com, "Supported ROM
// Formats"): NES (incl. FDS), SNES, GB, GBC, GBA, Master System, Game Gear,
// Genesis/Mega Drive, Sega CD, Sega Saturn, Dreamcast, PSX, PSP, GameCube,
// Wii — extended with atomiswave, naomi, arcade (fbneo), Nintendo 3DS,
// Nintendo 64 and Nintendo DS (dedicated melonDS fork in the ticohq org),
// which the wiki omits but tico supports. Aliases normalize
// first ("megadrive" -> genesis, "mastersystem" -> sms, "famicom" -> nes).
bool platformSupportedByTico(const std::string& rommSlug);

// BIOS destination dir for a platform slug.
//   tico: <biosRoot>/<layoutPlatformFolder>, except segacd which uses the
//   genesis folder per Tico's documented BIOS layout; retroarch: flat.
// Empty rommSlug or unknown folder on tico falls back to biosRoot itself.
std::string layoutBiosFolder(const std::string& rommSlug, const std::string& biosRoot, OutputLayout layout);

// Extract a .zip archive into destDir. Returns false (with err set) on failure.
// Entry paths are sanitized against zip-slip (".." and absolute paths are never
// extracted outside destDir). Whether extraction happens at all is a config
// concern (Config::extractArchive), not a layout rule.
bool extractZipToDir(const std::string& zipPath, const std::string& destDir, std::string& err);

} // namespace romm
