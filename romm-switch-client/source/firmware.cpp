#include "romm/firmware.hpp"
#include "romm/filesystem.hpp"
#include "romm/layout.hpp"
#include "romm/logger.hpp"
#include <fstream>
#include <system_error>
#include <cstdio>
#include <filesystem>

namespace romm {

std::string biosDestinationDir(const Config& cfg, const std::string& platformSlug) {
    return layoutBiosFolder(platformSlug, effectiveBiosDir(cfg), parseOutputLayout(cfg.outputLayout));
}

bool firmwareNeedsDownload(bool fileExists, unsigned long long existingSize, unsigned long long remoteSize) {
    // Treat a size-query failure (fileExists false path) as needing download.
    return !fileExists || existingSize != remoteSize;
}

bool syncFirmwareForPlatform(const Config& cfg,
                             const std::string& platformSlug,
                             const std::string& platformId,
                             const std::function<void(const std::string&)>& progress,
                             FirmwareSyncResult& outResult,
                             std::string& outError,
                             ErrorInfo* outInfo) {
    if (outInfo) *outInfo = ErrorInfo{};
    outResult = FirmwareSyncResult{};
    outError.clear();

    std::vector<Firmware> list;
    std::string listErr;
    if (!fetchFirmware(cfg, platformId, list, listErr, outInfo)) {
        outError = "Failed to list firmware: " + listErr;
        return false;
    }
    if (list.empty()) {
        logLine("FW: no firmware entries for platform '" + platformSlug + "'");
        return true;
    }

    const std::string destDir = biosDestinationDir(cfg, platformSlug);
    if (!ensureDirectory(destDir)) {
        outError = "Failed to create BIOS directory: " + destDir;
        if (outInfo) {
            outInfo->category = ErrorCategory::Filesystem;
            outInfo->code = ErrorCode::Unknown;
            outInfo->detail = outError;
        }
        return false;
    }

    for (const auto& fw : list) {
        if (fw.fileName.empty()) continue;

        // Resolve target with the layout-derived dir, but also accept files already present
        // directly in the layout root (e.g. retroarch flat layout).
        const std::string target = destDir + "/" + fw.fileName;
        const bool exists = fileExists(target);
        unsigned long long existingSize = 0;
        if (exists) {
            std::error_code ec;
            uintmax_t sz = std::filesystem::file_size(target, ec);
            if (!ec) existingSize = static_cast<unsigned long long>(sz);
        }
        if (exists && !firmwareNeedsDownload(true, existingSize, fw.fileSizeBytes)) {
            if (progress) progress(fw.fileName + " (skipped)");
            logLine("FW: skip " + fw.fileName + " (already present, matching size)");
            outResult.skipped++;
            continue;
        }

        if (progress) progress(fw.fileName);
        logLine("FW: downloading " + fw.fileName + " -> " + target);

        const std::string part = target + ".part";
        std::error_code ec;
        std::filesystem::remove(part, ec);

        std::ofstream ofs(part, std::ios::binary | std::ios::trunc);
        std::string url = cfg.serverUrl + "/api/firmware/" + std::to_string(fw.id) +
                          "/content/" + fw.fileName;

        std::vector<std::pair<std::string, std::string>> headers;
        headers.emplace_back("Accept", "application/octet-stream");
        std::string auth = basicAuthHeader(cfg);
        if (!auth.empty()) headers.emplace_back("Authorization", "Basic " + auth);

        HttpResponse resp;
        std::string err;
        bool streamOk = false;
        if (ofs.is_open()) {
            streamOk = httpRequestStream("GET", url, headers, cfg.httpTimeoutSeconds, resp,
                                         [&ofs](const char* data, size_t n) -> bool {
                                             if (!data || n == 0) return true;
                                             ofs.write(data, static_cast<std::streamsize>(n));
                                             return ofs.good();
                                         },
                                         err);
            ofs.close();
        } else {
            err = "Failed to open temp file for write: " + part;
        }

        bool ok = false;
        if (streamOk && resp.statusCode == 200) {
            ec.clear();
            std::filesystem::rename(part, target, ec);
            if (ec) {
                err = "Failed to finalize " + target + ": " + ec.message();
            } else {
                ok = true;
            }
        } else {
            if (err.empty()) {
                err = "HTTP " + std::to_string(resp.statusCode) + " during download of " + fw.fileName;
            }
        }

        if (!ok) {
            std::error_code ec2;
            std::filesystem::remove(part, ec2);
            logLine("FW: FAILED " + fw.fileName + " - " + err);
            outResult.failed++;
            continue;
        }

        logLine("FW: done " + fw.fileName);
        outResult.downloaded++;
    }

    if (outResult.failed > 0) {
        outError = std::to_string(outResult.failed) + " firmware file(s) failed";
        if (outInfo) {
            outInfo->category = ErrorCategory::Network;
            outInfo->code = ErrorCode::HttpStatus;
            outInfo->detail = outError;
        }
        return false;
    }
    return true;
}

} // namespace romm
