#include "romm/firmware.hpp"
#include "romm/filesystem.hpp"
#include "romm/layout.hpp"
#include "romm/logger.hpp"

namespace romm {

std::string biosDestinationDir(const Config& cfg, const std::string& platformSlug) {
    return layoutBiosFolder(platformSlug, effectiveBiosDir(cfg), parseOutputLayout(cfg.outputLayout));
}

bool firmwareNeedsDownload(bool fileExists, unsigned long long existingSize, unsigned long long remoteSize) {
    // Treat a size-query failure (fileExists false path) as needing download.
    return !fileExists || existingSize != remoteSize;
}

} // namespace romm
