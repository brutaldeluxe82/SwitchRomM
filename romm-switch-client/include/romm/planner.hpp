#pragma once

#include "romm/models.hpp"
#include "romm/platform_prefs.hpp"
#include <vector>

namespace romm {

struct DownloadFileSpec {
    std::string fileId;
    std::string name;
    std::string url;
    uint64_t sizeBytes{0};
    std::string relativePath; // optional subpath within bundle output
    std::string category;      // e.g., game/dlc/update
    bool isBios{false};        // route to the BIOS dir instead of the ROM folder
};

struct DownloadBundle {
    std::string romId;
    std::string title;
    std::string platformSlug;
    std::string mode; // single_best | bundle_best | all_files (for future use)
    std::vector<DownloadFileSpec> files;

    uint64_t totalSize() const {
        uint64_t sum = 0;
        for (const auto& f : files) sum += f.sizeBytes;
        return sum;
    }
};

// Stage 1: trivial plan builder (single file), to be extended to multi-file bundles.
DownloadBundle buildBundleFromGame(const Game& g, const PlatformPrefs& prefs);

// Stage-1 BIOS plan builder: one DownloadFileSpec per firmware entry, each routed
// to the platform's BIOS directory. Filenames are URL-encoded in the download URL.
DownloadBundle buildFirmwareBundle(const std::string& platformSlug,
                                   const std::string& platformName,
                                   const std::vector<Firmware>& files,
                                   const std::string& serverUrl);

} // namespace romm
