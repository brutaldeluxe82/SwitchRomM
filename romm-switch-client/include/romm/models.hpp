#pragma once

#include <string>
#include <vector>
#include <atomic>

namespace romm {

struct Platform {
    std::string id;
    std::string name;
    std::string slug;
    int romCount{0};
    int firmwareCount{0}; // >0 when RomM reports BIOS/firmware for this platform
};

struct Firmware {
    long long id{0};
    std::string fileName;
    unsigned long long fileSizeBytes{0};
};

struct RomFile {
    std::string id;
    std::string name;
    std::string path;    // optional relative path from API
    std::string url;
    uint64_t sizeBytes{0};
    std::string category; // e.g., "game", "dlc", "update"
};

struct Game {
    std::string id;
    std::string title;
    std::string platformId;
    std::string platformSlug;
    std::string fsName;
    std::string fileId; // preferred RomM file id (xci/nsp)
    std::string coverUrl;
    std::string description; // RomM summary (Grout shows it on game details)
    uint64_t sizeBytes{0};
    std::string downloadUrl;
    std::vector<RomFile> files; // full file list from API detail
    bool isLocal{false};
};

} // namespace romm
