#pragma once

#include "romm/api.hpp"
#include "romm/config.hpp"
#include "romm/models.hpp"
#include <functional>
#include <string>

namespace romm {


// Resolve the BIOS destination dir for a platform under the active layout.
std::string biosDestinationDir(const Config& cfg, const std::string& platformSlug);

// Pure skip-decision helper: should a remote firmware file (remoteSize) be downloaded?
// Returns false (skip) when the local file exists with a matching size. A size-query
// failure surfaces as a size mismatch and triggers re-download.
bool firmwareNeedsDownload(bool fileExists, unsigned long long existingSize, unsigned long long remoteSize);


} // namespace romm
