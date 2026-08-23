#pragma once

#include <string>

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

// BIOS destination dir for a platform slug.
//   tico: <biosRoot>/<layoutPlatformFolder>; retroarch: <biosRoot> (flat).
// Empty rommSlug or unknown folder on tico falls back to biosRoot itself.
std::string layoutBiosFolder(const std::string& rommSlug, const std::string& biosRoot, OutputLayout layout);


} // namespace romm
