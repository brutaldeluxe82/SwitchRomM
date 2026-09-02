#include "catch.hpp"
#include "api_test_hooks.hpp"
#include "romm/api.hpp"
#include "romm/downloader.hpp"
#include "romm/models.hpp"

namespace {

// Minimal helper to run the mock stream and count bytes.
size_t streamBytes(const std::string& raw, int& statusCode, std::string& err) {
    romm::HttpResponse resp;
    size_t total = 0;
    bool ok = romm::httpRequestStreamMock(raw, resp,
        [&](const char* /*data*/, size_t len) {
            total += len;
            return true;
        },
        err);
    statusCode = resp.statusCode;
    return ok ? total : 0;
}

} // namespace

TEST_CASE("preflight-style header parsing: content-length and accept-ranges") {
    const std::string raw =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 12345\r\n"
        "Accept-Ranges: bytes\r\n"
        "\r\n";

    int code = 0;
    std::string err;
    size_t total = streamBytes(raw, code, err);

    REQUIRE(total == 0); // no body in preflight mock
    REQUIRE(err.empty());
    REQUIRE(code == 200);
}

TEST_CASE("preflight fallback 206 with content-range") {
    const std::string raw =
        "HTTP/1.1 206 Partial Content\r\n"
        "Content-Range: bytes 0-0/9999\r\n"
        "\r\n";

    int code = 0;
    std::string err;
    size_t total = streamBytes(raw, code, err);

    REQUIRE(total == 0);
    REQUIRE(err.empty());
    REQUIRE(code == 206);
}

TEST_CASE("httpRequestStreamMock short-read vs content-length") {
    // Body is shorter than declared Content-Length => should error.
    const std::string raw =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 10\r\n"
        "\r\n"
        "short";

    romm::HttpResponse resp;
    size_t total = 0;
    std::string err;
    bool ok = romm::httpRequestStreamMock(raw, resp,
        [&](const char* /*data*/, size_t len) {
            total += len;
            return true;
        },
        err);

    REQUIRE_FALSE(ok);
    REQUIRE(err == "Short read");
    REQUIRE(total == 5);
}

TEST_CASE("httpRequestStreamMock exact content-length passes") {
    const std::string raw =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "hello";

    romm::HttpResponse resp;
    size_t total = 0;
    std::string err;
    bool ok = romm::httpRequestStreamMock(raw, resp,
        [&](const char* /*data*/, size_t len) {
            total += len;
            return true;
        },
        err);

    REQUIRE(ok);
    REQUIRE(err.empty());
    REQUIRE(resp.statusCode == 200);
    REQUIRE(total == 5);
}

TEST_CASE("part planning sanity: number of parts vs size") {
    constexpr uint64_t kPartSize = 0xFFFF0000ULL; // 4 GiB-ish

    auto partsFor = [&](uint64_t size) {
        return (size + kPartSize - 1) / kPartSize;
    };

    REQUIRE(partsFor(0) == 0);
    REQUIRE(partsFor(1) == 1);
    REQUIRE(partsFor(kPartSize) == 1);
    REQUIRE(partsFor(kPartSize + 1) == 2);
    REQUIRE(partsFor(kPartSize * 2) == 2);
    REQUIRE(partsFor(kPartSize * 2 + 1234) == 3);
}

TEST_CASE("parseLengthAndRangesForTest extracts length and ranges") {
    std::string hdrs =
        "Content-Length: 12345\r\n"
        "Accept-Ranges: bytes\r\n";
    bool ranges = false;
    uint64_t len = 0;
    REQUIRE(romm::parseLengthAndRangesForTest(hdrs, ranges, len));
    REQUIRE(ranges);
    REQUIRE(len == 12345);

    hdrs = "Content-Range: bytes 0-0/999\r\n";
    ranges = false; len = 0;
    REQUIRE(romm::parseLengthAndRangesForTest(hdrs, ranges, len));
    REQUIRE(len == 999);
}

TEST_CASE("parseLengthAndRangesForTest fails without length info") {
    std::string hdrs = "Accept-Ranges: bytes\r\n";
    bool ranges = false;
    uint64_t len = 0;
    REQUIRE_FALSE(romm::parseLengthAndRangesForTest(hdrs, ranges, len));
    REQUIRE(len == 0);
}

TEST_CASE("buildFirmwareBundle URL-encodes file names and routes to BIOS") {
    std::vector<romm::Firmware> files;
    romm::Firmware fw;
    fw.id = 42;
    fw.fileName = "scph5501 (USA).bin";
    fw.fileSizeBytes = 5242880;
    files.push_back(fw);

    romm::DownloadBundle bundle =
        romm::buildFirmwareBundle("psx", "Sony Playstation", files, "http://x:8080");
    REQUIRE(bundle.files.size() == 1);
    REQUIRE(bundle.romId == "__bios__psx");
    REQUIRE(bundle.title == "Sony Playstation BIOS");
    REQUIRE(bundle.mode == "firmware");
    const auto& spec = bundle.files[0];
    REQUIRE(spec.isBios);
    REQUIRE(spec.sizeBytes == 5242880);
    // Spaces and parentheses must be percent-encoded.
    REQUIRE(spec.url.find("%20%28USA%29.bin") != std::string::npos);
    REQUIRE(spec.url == "http://x:8080/api/firmware/42/content/scph5501%20%28USA%29.bin");
}

TEST_CASE("buildFirmwareBundle skips empty file names") {
    std::vector<romm::Firmware> files;
    romm::Firmware empty1;
    empty1.id = 1;
    romm::Firmware good;
    good.id = 2;
    good.fileName = "bios.bin";
    good.fileSizeBytes = 1024;
    files.push_back(empty1);
    files.push_back(good);

    romm::DownloadBundle bundle =
        romm::buildFirmwareBundle("nes", "Nintendo", files, "http://x");
    REQUIRE(bundle.files.size() == 1);
    REQUIRE(bundle.files[0].fileId == "2");
}

// ---------- multi-disc handling (tico wiki: linked discs must share base
// name + (Disc N)/(Disk N)/(CD N); server downloads each disc separately) ----------

namespace {

romm::RomFile makeFile(const std::string& id, const std::string& name,
                       const std::string& path, uint64_t size) {
    romm::RomFile rf;
    rf.id = id;
    rf.name = name;
    rf.path = path;
    rf.url = "http://x/api/roms/1/content/" + name;
    rf.sizeBytes = size;
    rf.category = "game";
    return rf;
}

} // namespace

TEST_CASE("planner: multi-disc game downloads every disc (bundle_best)") {
    romm::Game g;
    g.id = "1350";
    g.title = "Chrono Cross";
    g.platformSlug = "psx";
    g.files.push_back(makeFile("1", "Chrono Cross (USA) (Disc 1).chd", "roms/psx/Chrono Cross (USA)", 403357279));
    g.files.push_back(makeFile("2", "Chrono Cross (USA) (Disc 2).chd", "roms/psx/Chrono Cross (USA)", 390327286));

    romm::PlatformPrefs prefs; // defaultMode = bundle_best
    romm::DownloadBundle bundle = romm::buildBundleFromGame(g, prefs);
    REQUIRE(bundle.files.size() == 2);
    REQUIRE(bundle.totalSize() == 403357279ULL + 390327286ULL);
    // Each disc keeps its own file name so tico's disc-linking finds them.
    REQUIRE(bundle.files[0].name.find("(Disc 1)") != std::string::npos);
    REQUIRE(bundle.files[1].name.find("(Disc 2)") != std::string::npos);
}

TEST_CASE("planner: single_best on a multi-disc set still picks one disc only") {
    romm::Game g;
    g.id = "1274";
    g.title = "Panzer Dragoon Saga";
    g.platformSlug = "saturn";
    g.files.push_back(makeFile("1", "Panzer Dragoon Saga (Europe) (Disc 1).chd", "roms/saturn/x", 397928216));
    g.files.push_back(makeFile("2", "Panzer Dragoon Saga (Europe) (Disc 2).chd", "roms/saturn/x", 340840805));

    romm::PlatformPrefs prefs;
    prefs.defaultMode = "single_best";
    romm::DownloadBundle bundle = romm::buildBundleFromGame(g, prefs);
    REQUIRE(bundle.files.size() == 1);
    REQUIRE(bundle.totalSize() == 397928216ULL);
}

TEST_CASE("planner: soundtrack/extra files excluded from multi-disc bundle") {
    romm::Game g;
    g.id = "7";
    g.title = "Game With Extras";
    g.platformSlug = "psx";
    g.files.push_back(makeFile("1", "Game (Disc 1).chd", "roms/psx/Game", 100));
    g.files.push_back(makeFile("2", "Game (Disc 2).chd", "roms/psx/Game", 200));
    romm::RomFile snd = makeFile("3", "Game Soundtrack.mp3", "roms/psx/Game", 50);
    snd.category = "soundtrack";
    g.files.push_back(snd);

    romm::PlatformPrefs prefs;
    romm::DownloadBundle bundle = romm::buildBundleFromGame(g, prefs);
    REQUIRE(bundle.files.size() == 2);
    REQUIRE(bundle.totalSize() == 300);
}
