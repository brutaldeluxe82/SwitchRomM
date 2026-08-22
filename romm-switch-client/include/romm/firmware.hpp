#pragma once

#include "romm/api.hpp"
#include "romm/config.hpp"
#include "romm/models.hpp"
#include <functional>
#include <string>

namespace romm {

struct FirmwareSyncResult {
    int downloaded{0};
    int skipped{0};
    int failed{0};
};

// Resolve the BIOS destination dir for a platform under the active layout.
std::string biosDestinationDir(const Config& cfg, const std::string& platformSlug);

// Pure skip-decision helper: should a remote firmware file (remoteSize) be downloaded?
// Returns false (skip) when the local file exists with a matching size. A size-query
// failure surfaces as a size mismatch and triggers re-download.
bool firmwareNeedsDownload(bool fileExists, unsigned long long existingSize, unsigned long long remoteSize);

// Sync one platform's firmware: list remote files, skip ones already present with a
// matching size, stream the rest to a temp file and atomically rename into place.
// progress is called per item ("name", "name (skipped)", ...) for UI/log purposes.
bool syncFirmwareForPlatform(const Config& cfg,
                             const std::string& platformSlug,
                             const std::string& platformId,
                             const std::function<void(const std::string&)>& progress,
                             FirmwareSyncResult& outResult,
                             std::string& outError,
                             ErrorInfo* outInfo = nullptr);

} // namespace romm
