// services.cpp — RomServices implementation. Ported from old main.cpp
// (workers, polling, filters/sorts, pairing, discovery, sync, updater).

#include "services.hpp"

#include <switch.h>
#include <mini/json.hpp>
#include "romm/logger.hpp"
#include "romm/http_common.hpp"
#include "romm/version.hpp"
#include "romm/filesystem.hpp"
#include "romm/downloader.hpp"
#include "romm/queue_store.hpp"
#include "romm/planner.hpp"
#include "romm/queue_policy.hpp"
#include "romm/self_update.hpp"
#include "romm/md5.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <sys/stat.h>
#include <unordered_set>
#include <thread>


namespace romm::ui {

namespace {

constexpr size_t kPlatformRomsCacheTtlMs = 2 * 60 * 1000;
constexpr size_t kPlatformRomsCacheMaxEntries = 2;
constexpr size_t kRomsFirstPageLimit = 250;
constexpr size_t kRomsNextPageLimit = 500;
constexpr size_t kRemoteSearchThreshold = 1200;
constexpr size_t kRemoteSearchLimit = 250;
constexpr const char* kUpdatePendingPath = "sdmc:/switch/romm_switch_client/update_pending.txt";

// Fold common Latin-1/Latin-Extended codepoints to ASCII (ported).
char foldCodepointToAscii(uint32_t cp) {
    switch (cp) {
        case 0x00A0: return ' ';
        case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015: case 0x2212: return '-';
        case 0x2018: case 0x2019: case 0x201A: case 0x2032: return '\'';
        case 0x201C: case 0x201D: case 0x201E: case 0x2033: return '"';
        case 0x2026: return '.';
        case 0x00C6: case 0x01E2: case 0x01FC: return 'A';
        case 0x00E6: case 0x01E3: case 0x01FD: return 'a';
        case 0x0152: return 'O';
        case 0x0153: return 'o';
        case 0x00DF: return 's';
        case 0x00DE: return 'T';
        case 0x00FE: return 't';
        case 0x00D0: return 'D';
        case 0x00F0: return 'd';
        // A..Z folded ranges below mirror main.cpp's table (common letters only).
        case 0x00C0: case 0x00C1: case 0x00C2: case 0x00C3: case 0x00C4: case 0x00C5:
        case 0x0100: case 0x0102: case 0x0104: case 0x01CD: case 0x01DE: case 0x01E0: return 'A';
        case 0x00E0: case 0x00E1: case 0x00E2: case 0x00E3: case 0x00E4: case 0x00E5:
        case 0x0101: case 0x0103: case 0x0105: case 0x01CE: case 0x01DF: case 0x01E1: return 'a';
        case 0x00C7: case 0x0106: case 0x0108: case 0x010A: case 0x010C: return 'C';
        case 0x00E7: case 0x0107: case 0x0109: case 0x010B: case 0x010D: return 'c';
        case 0x010E: case 0x0110: return 'D';
        case 0x010F: case 0x0111: return 'd';
        case 0x00C8: case 0x00C9: case 0x00CA: case 0x00CB:
        case 0x0112: case 0x0114: case 0x0116: case 0x0118: case 0x011A: return 'E';
        case 0x00E8: case 0x00E9: case 0x00EA: case 0x00EB:
        case 0x0113: case 0x0115: case 0x0117: case 0x0119: case 0x011B: return 'e';
        case 0x011C: case 0x011E: case 0x0120: case 0x0122: return 'G';
        case 0x011D: case 0x011F: case 0x0121: case 0x0123: return 'g';
        case 0x00CC: case 0x00CD: case 0x00CE: case 0x00CF:
        case 0x0128: case 0x012A: case 0x012C: case 0x012E: case 0x0130: return 'I';
        case 0x00EC: case 0x00ED: case 0x00EE: case 0x00EF:
        case 0x0129: case 0x012B: case 0x012D: case 0x012F: case 0x0131: return 'i';
        case 0x00D1: case 0x0143: case 0x0145: case 0x0147: return 'N';
        case 0x00F1: case 0x0144: case 0x0146: case 0x0148: return 'n';
        case 0x00D2: case 0x00D3: case 0x00D4: case 0x00D5: case 0x00D6: case 0x00D8:
        case 0x014C: case 0x014E: case 0x0150: return 'O';
        case 0x00F2: case 0x00F3: case 0x00F4: case 0x00F5: case 0x00F6: case 0x00F8:
        case 0x014D: case 0x014F: case 0x0151: return 'o';
        case 0x0154: case 0x0156: case 0x0158: return 'R';
        case 0x0155: case 0x0157: case 0x0159: return 'r';
        case 0x015A: case 0x015C: case 0x015E: case 0x0160: return 'S';
        case 0x015B: case 0x015D: case 0x015F: case 0x0161: case 0x017F: return 's';
        case 0x0162: case 0x0164: case 0x0166: return 'T';
        case 0x0163: case 0x0165: case 0x0167: return 't';
        case 0x00D9: case 0x00DA: case 0x00DB: case 0x00DC:
        case 0x0168: case 0x016A: case 0x016C: case 0x016E: case 0x0170: case 0x0172: return 'U';
        case 0x00F9: case 0x00FA: case 0x00FB: case 0x00FC:
        case 0x0169: case 0x016B: case 0x016D: case 0x016F: case 0x0171: case 0x0173: return 'u';
        case 0x00DD: case 0x0178: return 'Y';
        case 0x00FD: case 0x00FF: return 'y';
        case 0x017D: case 0x017B: return 'Z';
        case 0x017E: case 0x017C: return 'z';
        default: return 0;
    }
}

std::string decodeAndFoldUtf8(const std::string& in, bool replaceUnknown) {
    std::string out;
    out.reserve(in.size());
    size_t i = 0;
    while (i < in.size()) {
        unsigned char b = static_cast<unsigned char>(in[i]);
        uint32_t cp = 0;
        if (b < 0x80) {
            cp = b;
            i += 1;
        } else if ((b & 0xE0) == 0xC0 && i + 1 < in.size()) {
            cp = ((b & 0x1F) << 6) | (static_cast<unsigned char>(in[i + 1]) & 0x3F);
            i += 2;
        } else if ((b & 0xF0) == 0xE0 && i + 2 < in.size()) {
            cp = ((b & 0x0F) << 12) | ((static_cast<unsigned char>(in[i + 1]) & 0x3F) << 6) |
                 (static_cast<unsigned char>(in[i + 2]) & 0x3F);
            i += 3;
        } else if ((b & 0xF8) == 0xF0 && i + 3 < in.size()) {
            cp = ((b & 0x07) << 18) | ((static_cast<unsigned char>(in[i + 1]) & 0x3F) << 12) |
                 ((static_cast<unsigned char>(in[i + 2]) & 0x3F) << 6) |
                 (static_cast<unsigned char>(in[i + 3]) & 0x3F);
            i += 4;
        } else {
            cp = '?';
            i += 1;
        }
        if (cp < 0x80) {
            char c = static_cast<char>(cp);
            out.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c);
        } else {
            char f = foldCodepointToAscii(cp);
            if (f) {
                out.push_back(f);
            } else if (replaceUnknown) {
                out.push_back('?');
            }
        }
    }
    return out;
}

std::string normalizeSearchText(const std::string& in) {
    // Lowercase + fold + strip non-alphanumerics (ported behavior).
    std::string folded = decodeAndFoldUtf8(in, false);
    std::string out;
    out.reserve(folded.size());
    for (char c : folded) {
        if (std::isalnum(static_cast<unsigned char>(c))) out.push_back(c);
    }
    return out;
}

} // namespace

uint32_t TicksMs() {
    // armGetSystemTick is 19.2 MHz on Switch; convert to ms.
    return static_cast<uint32_t>(armGetSystemTick() / 19200);
}

// ---- Start/Stop ----

void RomServices::Start() {
    using namespace std::placeholders;
    romFetchJobs.start([this](const PendingRomFetch& req) -> RomFetchResult {
        RomFetchResult out;
        out.req = req;
        std::string err;
        ErrorInfo errInfo;
        if (req.mode == PendingRomFetch::Mode::Probe) {
            out.probeOnly = true;
            std::string digest;
            if (!fetchRomsIdentifiersDigest(config, req.pid, digest, err, &errInfo)) {
                out.ok = true;
                out.probeFailed = true;
                out.error = err;
                out.errorInfo = errInfo;
                return out;
            }
            out.ok = true;
            out.identifierDigest = digest;
            out.probeUnchanged = !digest.empty() && digest == req.cachedIdentifierDigest;
            return out;
        }
        GamesPage page;
        if (!fetchGamesPageForPlatform(config, req.pid, req.offset, req.limit, page, err, &errInfo)) {
            out.ok = false;
            out.error = err;
            out.errorInfo = errInfo;
            return out;
        }
        std::vector<Game> games = std::move(page.games);
        if (!req.pid.empty()) {
            bool anyDifferentId = false;
            bool anyHasId = false;
            for (const auto& r : games) {
                if (!r.platformId.empty()) {
                    anyHasId = true;
                    if (r.platformId != req.pid) { anyDifferentId = true; break; }
                }
            }
            if (anyHasId && anyDifferentId) {
                games.erase(std::remove_if(games.begin(), games.end(),
                    [&](const Game& r) {
                        return !r.platformId.empty() && r.platformId != req.pid;
                    }),
                    games.end());
            }
        }
        if (!req.slug.empty()) {
            bool anyDifferentSlug = false;
            bool anyHasSlug = false;
            for (const auto& r : games) {
                if (!r.platformSlug.empty()) {
                    anyHasSlug = true;
                    if (r.platformSlug != req.slug) { anyDifferentSlug = true; break; }
                }
            }
            if (anyHasSlug && anyDifferentSlug) {
                games.erase(std::remove_if(games.begin(), games.end(),
                    [&](const Game& r) {
                        return !r.platformSlug.empty() && r.platformSlug != req.slug;
                    }),
                    games.end());
            }
            for (auto& r : games) {
                if (r.platformSlug.empty()) r.platformSlug = req.slug;
            }
        }
        if (req.offset == 0) {
            std::string digest;
            std::string digestErr;
            if (fetchRomsIdentifiersDigest(config, req.pid, digest, digestErr, nullptr)) {
                out.identifierDigest = digest;
            }
        }
        out.ok = true;
        out.offset = page.offset;
        out.limit = page.limit;
        out.hasMore = page.hasMore;
        out.nextOffset = page.offset + page.games.size();
        out.total = page.total;
        out.totalKnown = page.totalKnown;
        out.games = std::move(games);
        return out;
    });

    remoteSearchJobs.start([this](const PendingRemoteSearch& req) -> RemoteSearchResult {
        RemoteSearchResult out;
        out.req = req;
        std::string err;
        ErrorInfo info;
        if (!searchGamesRemote(config, req.pid, req.query, req.limit, out.games, err, &info)) {
            out.ok = false;
            out.error = err;
            out.errorInfo = info;
            return out;
        }
        out.ok = true;
        return out;
    }, 120);

    diagProbeJobs.start([this](const DiagProbeReq& req) -> DiagProbeResult {
        DiagProbeResult out;
        out.generation = req.generation;
        std::string body;
        std::string err;
        ErrorInfo info;
        const std::string url = config.serverUrl + "/api/platforms?limit=1";
        if (!fetchBinary(config, url, body, err, &info)) {
            out.ok = false;
            out.detail = err;
            out.errorInfo = info;
            return out;
        }
        out.ok = true;
        out.detail = "HTTP OK";
        return out;
    });

    biosListJobs.start([this](const BiosListReq& req) -> BiosListResult {
        BiosListResult out;
        out.generation = req.generation;
        out.platformSlug = req.platformSlug;
        out.platformName = req.platformName;
        std::string err;
        fetchFirmware(config, req.platformId, out.files, err);
        out.error = err;
        romm::logLine("BIOS: listed " + std::to_string(out.files.size()) +
                      " firmware file(s) for " + req.platformName);
        return out;
    });

    saveSyncJobs.start([this](const PairingReq& req) -> PairingResultMsg {
        PairingResultMsg out;
        out.generation = req.generation;
        const std::string authRaw = req.basicAuthRaw;
        std::string deviceId;
        std::string accessToken;

        auto doRequest = [&](const std::string& method, const std::string& url,
                             const std::vector<std::pair<std::string, std::string>>& headers,
                             const void* body, size_t bodySize,
                             std::string& bodyOut, std::string& err) -> long {
            err.clear();
            HttpRequestOptions opt;
            opt.timeoutSec = (req.timeoutSeconds > 0) ? req.timeoutSeconds : 20;
            opt.keepAlive = false;
            opt.requestBody = body;
            opt.requestBodySize = bodySize;
            HttpTransaction tx;
            if (!httpRequestBuffered(method, url, headers, opt, tx, err)) return -1;
            bodyOut = tx.body;
            return static_cast<long>(tx.parsed.statusCode);
        };
        auto sleepCancellable = [&](long ms) -> bool {
            while (ms > 0) {
                if (saveCancel.load()) return false;
                long step = ms > 250 ? 250 : ms;
                std::this_thread::sleep_for(std::chrono::milliseconds(step));
                ms -= step;
            }
            return true;
        };
        auto postPairError = [&](const std::string& detail) {
            out.paired = false;
            out.statusText = detail;
            std::lock_guard<std::mutex> lock(status.mutex);
            if (req.generation == saveSyncGeneration) {
                status.savePairState = SavePairState::Error;
                status.savePairDetail = detail;
                status.saveStatusText = detail;
                status.saveBusy.store(false);
            }
            uiRevision.fetch_add(1); // re-render pairing screen with the error
            romm::logLine("SAVE: " + detail);
        };
        // 1. Heartbeat version gate.
        {
            std::string url = req.serverUrl + "/api/heartbeat";
            std::vector<std::pair<std::string, std::string>> headers;
            if (!authRaw.empty()) headers.emplace_back("Authorization", "Basic " + authRaw);
            std::string body;
            std::string err;
            long code = doRequest("GET", url, headers, nullptr, 0, body, err);
            std::string version;
            if (code >= 200 && code < 300) {
                mini::Object obj;
                if (mini::parse(body, obj)) {
                    auto sys = obj.find("SYSTEM");
                    if (sys != obj.end() && sys->second.type == mini::Value::Type::Object) {
                        auto v = sys->second.object.find("VERSION");
                        if (v != sys->second.object.end() && v->second.type == mini::Value::Type::String)
                            version = v->second.str;
                    }
                }
            }
            const bool supported = serverSupportsDeviceAuth(version);
            {
                std::lock_guard<std::mutex> lock(status.mutex);
                if (req.generation == saveSyncGeneration) {
                    status.saveServerDeviceAuthKnown = !version.empty();
                    status.saveServerDeviceAuthSupported = supported;
                    status.saveServerVersion = version;
                }
            }
            if (!version.empty() && !supported) {
                postPairError("server " + version + " lacks device auth (needs RomM 5+)");
                return out;
            }
            if (code < 200 || code >= 300) {
                romm::logLine("SAVE: heartbeat failed (" + err + "), attempting init anyway");
            }
        }

        // 2. Init.
        if (saveCancel.load()) { postPairError("Pairing cancelled"); return out; }
        {
            DeviceAuthInitRequest initReq;
            initReq.name = "Switch";
            initReq.client = "SwitchRomM";
            initReq.clientVersion = romm::appVersion();
            initReq.clientDeviceIdentifier = req.clientDeviceId;
            std::string payload = serializeDeviceAuthInitBody(initReq);
            std::string url = req.serverUrl + "/api/auth/device/init";
            std::vector<std::pair<std::string, std::string>> headers;
            headers.emplace_back("Content-Type", "application/json");
            if (!authRaw.empty()) headers.emplace_back("Authorization", "Basic " + authRaw);
            std::string body;
            std::string err;
            long code = doRequest("POST", url, headers, payload.data(), payload.size(), body, err);
            if (code < 200 || code >= 300) {
                postPairError("init failed: " + (err.empty() ? std::to_string(code) : err));
                return out;
            }
            DeviceAuthInitResponse initResp;
            if (!parseDeviceAuthInitResponse(body, initResp)) {
                postPairError("init response unparsable");
                return out;
            }
            long interval = initResp.interval >= 3 ? initResp.interval : 3;
            std::string userCode = initResp.userCode;
            std::string verifyPath = initResp.verificationPathComplete
                ? initResp.verificationUrlComplete
                : (!initResp.verificationPath.empty() ? initResp.verificationPath
                                                      : std::string("/pair/device"));
            {
                std::string origin = req.serverUrl;
                while (!origin.empty() && origin.back() == '/') origin.pop_back();
                verifyPath = origin + verifyPath;
            }
            {
                std::lock_guard<std::mutex> lock(status.mutex);
                if (req.generation == saveSyncGeneration) {
                    status.savePairState = SavePairState::AwaitingApproval;
                    status.savePairUserCode = userCode;
                    status.saveVerificationPath = verifyPath;
                    status.saveStatusText = "Awaiting approval...";
                }
            }
            // Re-render the pairing screen with the code even though this is
            // a worker thread; uiRevision is atomic.
            uiRevision.fetch_add(1);
            auto deadline = std::chrono::steady_clock::now() +
                            std::chrono::seconds(initResp.expiresIn > 0 ? initResp.expiresIn : 600);
            for (;;) {
                if (saveCancel.load()) { postPairError("Pairing cancelled"); return out; }
                if (std::chrono::steady_clock::now() >= deadline) {
                    postPairError("Pairing expired (please retry)");
                    return out;
                }
                if (!sleepCancellable(interval * 1000)) {
                    postPairError("Pairing cancelled");
                    return out;
                }
                if (saveCancel.load()) { postPairError("Pairing cancelled"); return out; }
                std::string pollPayload = "{\"device_code\":\"" + initResp.deviceCode + "\"}";
                std::vector<std::pair<std::string, std::string>> pollHeaders;
                pollHeaders.emplace_back("Content-Type", "application/json");
                if (!authRaw.empty()) pollHeaders.emplace_back("Authorization", "Basic " + authRaw);
                std::string pollUrl = req.serverUrl + "/api/auth/device/token";
                std::string pollBody;
                std::string pollErr;
                long pollCode = doRequest("POST", pollUrl, pollHeaders,
                                          pollPayload.data(), pollPayload.size(), pollBody, pollErr);
                DeviceAuthPollResult pr = classifyDeviceTokenResponse(pollCode, pollBody);
                switch (pr.state) {
                    case DeviceAuthPollState::Approved:
                        accessToken = pr.accessToken;
                        deviceId = pr.deviceId;
                        {
                            DeviceToken tok;
                            tok.accessToken = pr.accessToken;
                            tok.deviceId = pr.deviceId;
                            tok.clientDeviceIdentifier = req.clientDeviceId;
                            std::string saveErr;
                            if (!saveDeviceToken(kDeviceTokenPath, tok, saveErr))
                                romm::logLine("SAVE: warning - could not persist token: " + saveErr);
                            std::lock_guard<std::mutex> lock(status.mutex);
                            if (req.generation == saveSyncGeneration) {
                                status.savePairState = SavePairState::Paired;
                                status.savePairUserCode.clear();
                                status.saveVerificationPath.clear();
                                status.saveStatusText = "Paired - syncing saves...";
                                status.pairedDeviceId = deviceId;
                                status.saveBusy.store(false);
                            }
                        }
                        romm::logLine("SAVE: paired (device id " + deviceId + ")");
                        break;
                    case DeviceAuthPollState::Pending:
                        break;
                    case DeviceAuthPollState::SlowDown:
                        interval += 5;
                        break;
                    case DeviceAuthPollState::AccessDenied:
                        postPairError("Pairing denied");
                        return out;
                    case DeviceAuthPollState::ExpiredToken:
                        postPairError("Pairing code expired");
                        return out;
                    case DeviceAuthPollState::Error:
                    default:
                        postPairError("Pairing error: " + (pollErr.empty() ? std::string("server error") : pollErr));
                        return out;
                }
                if (!accessToken.empty()) break;
            }
        }
        {
            std::lock_guard<std::mutex> lock(status.mutex);
            if (req.generation == saveSyncGeneration) {
                status.savePairState = SavePairState::Paired;
                status.saveStatusText.clear();
                status.lastError.clear();
                status.saveBusy.store(false); // allow future re-pair attempts
            }
        }
        out.paired = true;
        return out;
    });

    discoveryJobs.start([this](const DiscoveryReq& req) -> DiscoveryResult {
        DiscoveryResult out;
        out.generation = req.generation;
        auto scanRootLocal = [&](const std::string& rootPath, bool isStates) {
            std::error_code ec;
            std::filesystem::path root(rootPath);
            if (root.empty() || !std::filesystem::exists(root, ec)) return;
            for (auto it = std::filesystem::recursive_directory_iterator(
                     root, std::filesystem::directory_options::skip_permission_denied, ec);
                 it != end(it); it.increment(ec)) {
                if (ec) break;
                if (it->is_directory(ec)) continue;
                const std::string path = it->path().string();
                const std::string fileName = it->path().filename().string();
                DiscFile f;
                f.path = path;
                f.isState = isStates;
                std::string lowerName;
                for (char c : fileName) {
                    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 'a' - 'A');
                    lowerName.push_back(c);
                }
                if (isStates) {
                    if (classifyStateFileName(fileName) == StateKind::None) continue;
                    size_t pos = lowerName.find(".state");
                    f.baseLower = (pos == std::string::npos) ? lowerName : lowerName.substr(0, pos);
                } else {
                    size_t dot = lowerName.rfind('.');
                    std::string ext = (dot == std::string::npos) ? "" : lowerName.substr(dot);
                    if (!isValidBatterySaveExtension(ext)) continue;
                    std::string bare = (dot == std::string::npos) ? fileName : fileName.substr(0, dot);
                    f.baseLower = saveLookupBase(bare);
                }
                struct ::stat st{};
                if (::stat(path.c_str(), &st) != 0) continue;
                f.mtime = static_cast<long long>(st.st_mtime);
                f.updatedAtIso = formatIso8601Utc(f.mtime);
                std::vector<std::string> slugs;
                {
                    std::filesystem::path rel = std::filesystem::relative(it->path(), root, ec);
                    if (!ec && rel.parent_path() != rel.root_path()) {
                        std::string top = rel.begin()->string();
                        if (!top.empty() && top != ".") slugs = ticoFolderToCanonicalSlugs(top);
                    }
                }
                f.folderSlugs = slugs;
                std::string hint = slugs.empty() ? std::string() : slugs.front();
                f.romId = romIdForMatch(req.roms, f.baseLower, hint);
                if (f.romId != 0) {
                    f.gameKey = "rid:" + std::to_string(f.romId);
                    auto nit = req.romNameById.find(f.romId);
                    if (nit != req.romNameById.end()) f.gameName = nit->second;
                } else {
                    f.gameKey = "f:" + f.baseLower;
                    f.gameName = fileName;
                }
                f.slotLabel = saveSlotLabel(fileName);
                out.files.push_back(std::move(f));
            }
        };
        scanRootLocal(req.savesRoot, false);
        scanRootLocal(req.statesRoot, true);
        romm::logLine("SAVE: discovered " + std::to_string(out.files.size()) + " file(s)");
        return out;
    });

    orchestratedSyncJobs.start([this](const SyncRunReq& req) -> SyncRunResult {
        SyncRunResult out;
        out.generation = req.generation;
        auto postStatus = [&](std::function<void()> fn) {
            std::lock_guard<std::mutex> lock(status.mutex);
            if (req.generation == syncRunGeneration) fn();
        };
        const SyncAuthCtx ctx{config.serverUrl, config.apiToken, "", config.httpTimeoutSeconds};
        std::string err;

        // 1. Snapshot: disk ROMs + ROM match table.
        std::vector<RomMatchEntry> matchTable;
        std::vector<long long> locallyPresentRomIds;
        std::vector<DiskRom> diskRoms = scanDiskRoms(defaultDownloadDir(parseOutputLayout(config.outputLayout)));
        {
            std::lock_guard<std::mutex> lock(status.mutex);
            for (const auto& g : status.romsAll) {
                long long rid = 0;
                bool ok = !g.id.empty();
                for (char c : g.id) {
                    if (c < '0' || c > '9') { ok = false; break; }
                    rid = rid * 10 + (static_cast<long long>(c - '0'));
                }
                if (!ok || rid <= 0) continue;
                matchTable.push_back({rid, g.fsName, g.platformSlug});
            }
        }
        for (const auto& d : diskRoms) {
            for (const auto& e : matchTable) {
                if (romIdForMatch({e}, d.baseLower, "") == e.romId) {
                    locallyPresentRomIds.push_back(e.romId);
                    break;
                }
            }
        }
        auto hintOf = [&](const std::string& path, bool) -> std::string {
            std::string rootNorm = path;
            for (char& c : rootNorm) if (c == '\\') c = '/';
            size_t savesPos = rootNorm.find("/saves/");
            size_t statesPos = rootNorm.find("/states/");
            size_t base = savesPos != std::string::npos ? savesPos + 7
                        : statesPos != std::string::npos ? statesPos + 8
                        : 0;
            size_t slash = rootNorm.find('/', base);
            if (base == 0 || slash == std::string::npos) return "";
            std::string top = rootNorm.substr(base, slash - base);
            auto slugs = ticoFolderToCanonicalSlugs(top);
            return slugs.empty() ? "" : slugs.front();
        };
        ScanResult scan = scanAssets(effectiveSaveDir(config), effectiveStatesDir(config),
                                     [&](const std::string& baseLower, const std::string& slugHint) {
                                         return static_cast<int>(romIdForMatch(matchTable, baseLower, slugHint));
                                     },
                                     [](const std::string&) { return std::string(); },
                                     hintOf, err);
        if (!err.empty()) {
            out.error = "scan failed: " + err;
            postStatus([&] {
                status.saveSyncStatusText = out.error;
                status.saveSyncRunBusy.store(false);
            });
            return out;
        }
        out.unmatched = static_cast<int>(scan.unmatched.size());

        // 2. Split locals; negotiate for saves.
        std::vector<LocalAsset> savesForNegotiate;
        std::vector<LocalAsset> states;
        for (auto& a : scan.assets) (a.isState ? states : savesForNegotiate).push_back(a);

        SyncStateStore stateStore;
        loadSyncState(kSaveSyncStatePath, stateStore);

        NegotiateResponse neg;
        if (!negotiateSync(ctx, req.deviceId, savesForNegotiate, neg, err)) {
            out.error = "negotiate failed: " + err;
            postStatus([&] {
                status.saveSyncStatusText = out.error;
                status.saveSyncRunBusy.store(false);
            });
            return out;
        }
        OrchestratorPlan plan = buildOrchestratorPlan(neg, savesForNegotiate, stateStore,
                                                      locallyPresentRomIds);
        out.suppressed = plan.suppressedUploads;

        auto backupLocal = [&](const std::string& path) -> bool {
            namespace fs = std::filesystem;
            fs::path p(path);
            fs::path backupDir = p.parent_path() / ".backup";
            std::error_code ec;
            fs::create_directories(backupDir, ec);
            if (ec) return false;
            std::string stamp = "1970-01-01 00-00-00";
            {
                unsigned long long sz = 0; long long mt = 0; std::string h;
                if (computeFileMd5AndStat(path, sz, mt, h) && mt > 0)
                    stamp = formatIso8601Utc(mt).substr(0, 19);
                for (char& c : stamp) { if (c == 'T') c = ' '; if (c == ':') c = '-'; }
            }
            std::string dest = (backupDir / (p.stem().string() + " [" + stamp + "]" +
                                             p.extension().string())).string();
            fs::copy_file(path, dest, fs::copy_options::overwrite_existing, ec);
            return !ec;
        };
        auto setFileMtimeEpoch = [](const std::string& path, long long epoch) {
            if (epoch <= 0) return;
            namespace fs = std::filesystem;
            std::error_code ec;
            auto nowF = fs::last_write_time(path, ec);
            if (ec) return;
            auto nowSys = std::chrono::system_clock::now();
            auto nowFile = fs::file_time_type::clock::now();
            auto delta = std::chrono::seconds(epoch) - nowSys.time_since_epoch() +
                         nowF.time_since_epoch();
            fs::last_write_time(path, fs::file_time_type(fs::file_time_type::duration(delta)), ec);
        };
        auto parseIsoToEpoch = [](const std::string& iso) -> long long {
            if (iso.size() < 19) return 0;
            struct tm tmv{};
            if (sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%d", &tmv.tm_year, &tmv.tm_mon,
                       &tmv.tm_mday, &tmv.tm_hour, &tmv.tm_min, &tmv.tm_sec) != 6)
                return 0;
            tmv.tm_year -= 1900;
            tmv.tm_mon -= 1;
            long long days = 0;
            {
                int y = tmv.tm_year + 1900;
                int m = tmv.tm_mon + 1;
                int d = tmv.tm_mday;
                long long yy = y;
                if (m <= 2) yy -= 1;
                long long era = (yy >= 0 ? yy : yy - 399) / 400;
                long long yoe = yy - era * 400;
                long long mp = (m + 9) % 12;
                long long doy = (153 * mp + 2) / 5 + d - 1;
                long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
                days = era * 146097 + doe - 719468;
            }
            return days * 86400LL + tmv.tm_hour * 3600LL + tmv.tm_min * 60LL + tmv.tm_sec;
        };

        int stateUpdates = 0;
        auto recordState = [&](long long romId, const LocalAsset& local, long long saveId) {
            SyncStateRow row;
            row.romId = romId;
            row.fileName = local.fileName;
            row.slot = local.slot;
            row.saveId = saveId;
            row.contentHash = local.contentHash;
            row.syncedAt = formatIso8601Utc(static_cast<long long>(time(nullptr)));
            upsertSyncStateRow(stateStore, row);
            stateUpdates++;
        };

        // 3. Execute saves plan.
        for (const auto& orchestrated : plan.ops) {
            const SyncOperation& op = orchestrated.op;
            const bool isDownload = (op.action == "download");
            const bool isConflict = (op.action == "conflict");
            if (op.action != "upload" && !isDownload && !isConflict) continue;

            if (isDownload || (isConflict && orchestrated.local == nullptr)) {
                if (op.assetId <= 0) { out.failed++; continue; }
                std::string bytes;
                if (!downloadAssetContent(ctx, "saves", op.assetId, req.deviceId, bytes, err)) {
                    romm::logLine("SYNC: download failed: " + err);
                    out.failed++;
                    continue;
                }
                std::string ticoFolder = layoutPlatformFolder(
                    [&]() -> std::string {
                        for (const auto& e : matchTable)
                            if (e.romId == op.romId) return e.slugLower;
                        return "";
                    }(), parseOutputLayout(config.outputLayout));
                if (ticoFolder.empty()) ticoFolder = std::to_string(op.romId);
                std::string dir = effectiveSaveDir(config) + "/" + ticoFolder;
                std::filesystem::create_directories(dir);
                std::string romBase;
                {
                    const std::string* fsName = nullptr;
                    for (const auto& e : matchTable)
                        if (e.romId == op.romId) { fsName = &e.fsNameLower; break; }
                    std::string base = fsName ? *fsName : op.fileName;
                    size_t dot = base.rfind('.');
                    if (dot != std::string::npos) base = base.substr(0, dot);
                    std::string pretty;
                    {
                        std::lock_guard<std::mutex> lock(status.mutex);
                        for (const auto& g : status.romsAll) {
                            long long rid = 0;
                            bool ok = !g.id.empty();
                            for (char c : g.id) {
                                if (c < '0' || c > '9') { ok = false; break; }
                                rid = rid * 10 + (static_cast<long long>(c - '0'));
                            }
                            if (ok && rid == op.romId) { pretty = g.fsName; break; }
                        }
                    }
                    if (!pretty.empty()) {
                        size_t pd = pretty.rfind('.');
                        if (pd != std::string::npos) pretty = pretty.substr(0, pd);
                        romBase = pretty;
                    } else {
                        romBase = base;
                    }
                }
                std::string ext = op.fileName.substr(op.fileName.rfind('.'));
                std::string dest = dir + "/" + romBase + ext;
                {
                    std::error_code ec;
                    if (std::filesystem::exists(dest, ec)) {
                        if (!backupLocal(dest)) {
                            romm::logLine("SYNC: backup failed, aborting download to protect save");
                            out.failed++;
                            continue;
                        }
                    }
                }
                std::string tmp = dest + ".tmp";
                {
                    FILE* f = fopen(tmp.c_str(), "wb");
                    if (!f) { out.failed++; continue; }
                    fwrite(bytes.data(), 1, bytes.size(), f);
                    fclose(f);
                }
                std::error_code ec;
                std::filesystem::rename(tmp, dest, ec);
                if (ec) { std::remove(tmp.c_str()); out.failed++; continue; }
                setFileMtimeEpoch(dest, parseIsoToEpoch(op.serverUpdatedAt));
                std::string confErr;
                if (!confirmSaveDownloaded(ctx, op.assetId, req.deviceId, confErr))
                    romm::logLine("SYNC: confirm-downloaded failed (non-fatal): " + confErr);
                out.downloaded++;
                SyncStateRow row;
                row.romId = op.romId;
                row.fileName = op.fileName;
                row.slot = op.slot.empty() ? "autosave" : op.slot;
                row.saveId = op.assetId;
                {
                    std::string h;
                    romm::md5Hex(bytes, h);
                    row.contentHash = h;
                }
                row.syncedAt = formatIso8601Utc(static_cast<long long>(time(nullptr)));
                upsertSyncStateRow(stateStore, row);
                continue;
            }

            // Upload leg.
            const LocalAsset* local = orchestrated.local;
            if (!local || local->path.empty()) { out.failed++; continue; }
            bool forceOverwrite = isConflict &&
                (config.saveSyncBehavior == "client" || config.saveSyncBehavior == "newest");
            std::string upErr;
            bool ok;
            if (op.assetId > 0 && isConflict) {
                ok = updateExistingSave(ctx, op.assetId, req.deviceId, local->path, upErr);
            } else {
                ok = uploadNewSave(ctx, op.romId,
                                   local->slot.empty() ? "autosave" : local->slot,
                                   local->emulator, req.deviceId, plan.sessionId,
                                   local->path, forceOverwrite, config.saveBackupLimit,
                                   upErr);
            }
            if (!ok) {
                romm::logLine("SYNC: upload failed: " + upErr);
                out.failed++;
                continue;
            }
            out.uploaded++;
            recordState(op.romId, *local, op.assetId);
        }

        // 4. States: client-side plan.
        std::vector<RemoteAsset> remoteStates;
        if (!fetchRemoteAssets(ctx, "states", remoteStates, err)) {
            romm::logLine("SYNC: states fetch failed (non-fatal): " + err);
        }
        std::vector<SyncPlanItem> statePlan =
            buildSyncPlan(states, {}, remoteStates, locallyPresentRomIds);
        for (auto& item : statePlan) {
            SyncPlanAction action = resolveSyncAction(item, parseSyncPolicy(config.saveSyncBehavior));
            if (action == SyncPlanAction::NoOp) continue;
            if (action == SyncPlanAction::Upload && item.local) {
                std::string upErr;
                bool ok;
                if (item.hasRemote && item.remote.id > 0)
                    ok = updateExistingState(ctx, item.remote.id, item.local->path, upErr);
                else
                    ok = uploadNewState(ctx, item.local->romId, item.local->emulator,
                                        item.local->path, upErr);
                if (ok) out.uploaded++;
                else { romm::logLine("SYNC: state upload failed: " + upErr); out.failed++; }
            } else if (action == SyncPlanAction::Download && item.hasRemote && item.remote.id > 0) {
                bool romPresent = false;
                for (long long rid : locallyPresentRomIds)
                    if (rid == item.remote.romId) { romPresent = true; break; }
                if (!romPresent) { out.skippedAbsent++; continue; }
                std::string bytes;
                if (!downloadAssetContent(ctx, "states", item.remote.id, "", bytes, err)) {
                    out.failed++;
                    continue;
                }
                std::string ticoFolder = layoutPlatformFolder(
                    [&]() -> std::string {
                        for (const auto& e : matchTable)
                            if (e.romId == item.remote.romId) return e.slugLower;
                        return "";
                    }(), parseOutputLayout(config.outputLayout));
                if (ticoFolder.empty()) ticoFolder = std::to_string(item.remote.romId);
                std::string dir = effectiveStatesDir(config) + "/" + ticoFolder;
                std::filesystem::create_directories(dir);
                std::string dest = dir + "/" + item.remote.fileName;
                {
                    std::error_code ec;
                    if (std::filesystem::exists(dest, ec) && !backupLocal(dest)) {
                        out.failed++;
                        continue;
                    }
                }
                std::string tmp = dest + ".tmp";
                {
                    FILE* f = fopen(tmp.c_str(), "wb");
                    if (!f) { out.failed++; continue; }
                    fwrite(bytes.data(), 1, bytes.size(), f);
                    fclose(f);
                }
                std::error_code ec;
                std::filesystem::rename(tmp, dest, ec);
                if (ec) { std::remove(tmp.c_str()); out.failed++; continue; }
                setFileMtimeEpoch(dest, parseIsoToEpoch(item.remote.updatedAt));
                out.downloaded++;
            }
        }

        // 5. Persist state + complete session.
        if (stateUpdates > 0) {
            stateStore.deviceId = req.deviceId;
            saveSyncState(kSaveSyncStatePath, stateStore, err);
        }
        if (plan.sessionId > 0) {
            std::string compErr;
            completeSyncSession(ctx, plan.sessionId, out.uploaded + out.downloaded,
                                out.failed, compErr);
        }
        out.ok = out.failed == 0;
        postStatus([&] {
            status.saveSyncStatusText =
                "U:" + std::to_string(out.uploaded) + " D:" + std::to_string(out.downloaded) +
                " S:" + std::to_string(out.suppressed) + " F:" + std::to_string(out.failed) +
                (out.unmatched > 0 ? " (" + std::to_string(out.unmatched) + " unmatched)" : "");
            status.saveSyncRunBusy.store(false);
        });
        return out;
    });

    updateCheckJobs.start([this](const UpdateCheckReq& req) -> UpdateCheckResult {
        UpdateCheckResult out;
        out.generation = req.generation;
        std::string err;
        HttpTransaction tx;
        HttpRequestOptions opt;
        opt.timeoutSec = (config.httpTimeoutSeconds > 0) ? config.httpTimeoutSeconds : 20;
        opt.keepAlive = true;
        opt.maxBodyBytes = 2 * 1024 * 1024;
        std::vector<std::pair<std::string, std::string>> headers;
        headers.emplace_back("User-Agent", "romm-switch-client");
        headers.emplace_back("Accept", "application/vnd.github+json");
        const std::string kUpdateLatestUrl =
            "https://api.github.com/repos/Shalasere/SwitchRomM/releases/latest";
        if (!httpRequestBuffered("GET", kUpdateLatestUrl, headers, opt, tx, err)) {
            out.ok = false;
            out.error = err;
            out.errorInfo = classifyError(err, ErrorCategory::Network);
            return out;
        }
        if (tx.parsed.statusCode != 200) {
            out.ok = false;
            out.error = "GitHub latest release request failed (HTTP " + std::to_string(tx.parsed.statusCode) + ")";
            out.errorInfo.category = ErrorCategory::Network;
            out.errorInfo.code = ErrorCode::HttpStatus;
            out.errorInfo.httpStatus = tx.parsed.statusCode;
            return out;
        }
        GitHubRelease rel;
        if (!parseGitHubLatestReleaseJson(tx.body, rel, err)) {
            out.ok = false;
            out.error = err;
            out.errorInfo = classifyError(err, ErrorCategory::Data);
            return out;
        }
        GitHubAsset asset;
        if (!pickReleaseNroAsset(rel, asset, err, "romm-switch-client.nro")) {
            out.ok = false;
            out.release = rel;
            out.error = err;
            out.errorInfo = classifyError(err, ErrorCategory::Data);
            return out;
        }
        out.ok = true;
        out.release = std::move(rel);
        out.asset = std::move(asset);
        out.updateAvailable = (compareVersions(out.release.tagName, romm::appVersion()) > 0);
        return out;
    });

    updateDownloadJobs.start([this](const UpdateDownloadReq& req) -> UpdateDownloadResult {
        UpdateDownloadResult out;
        out.generation = req.generation;
        out.outPath = req.outPath;
        std::string err;
        const std::string tmp = req.outPath + ".part";
        std::FILE* f = std::fopen(tmp.c_str(), "wb");
        if (!f) {
            out.ok = false;
            out.error = "Failed to open update temp file for write: " + tmp;
            out.errorInfo = classifyError(out.error, ErrorCategory::Filesystem);
            return out;
        }
        HttpRequestOptions opt;
        opt.timeoutSec = (config.httpTimeoutSeconds > 0) ? config.httpTimeoutSeconds : 20;
        opt.followRedirects = true;
        std::vector<std::pair<std::string, std::string>> headers;
        headers.emplace_back("User-Agent", "romm-switch-client");
        headers.emplace_back("Accept", "application/octet-stream");
        ParsedHttpResponse parsed;
        uint64_t bytes = 0;
        bool ok = httpRequestStreamed("GET", req.url, headers, opt, parsed,
                                      [&](const char* data, size_t n) -> bool {
                                          if (!data || n == 0) return true;
                                          size_t w = std::fwrite(data, 1, n, f);
                                          if (w != n) return false;
                                          bytes += static_cast<uint64_t>(n);
                                          return true;
                                      },
                                      err);
        std::fclose(f);
        out.bytes = bytes;
        if (!ok) {
            out.ok = false;
            out.error = err.empty() ? "Update download failed." : err;
            out.errorInfo = classifyError(out.error, ErrorCategory::Network);
            std::error_code ec;
            std::filesystem::remove(tmp, ec);
            return out;
        }
        if (parsed.statusCode != 200) {
            out.ok = false;
            out.error = "Update download failed (HTTP " + std::to_string(parsed.statusCode) + ")";
            out.errorInfo.category = ErrorCategory::Network;
            out.errorInfo.code = ErrorCode::HttpStatus;
            out.errorInfo.httpStatus = parsed.statusCode;
            std::error_code ec;
            std::filesystem::remove(tmp, ec);
            return out;
        }
        bool sizeMismatch = (req.expectedSizeBytes > 0 && bytes != req.expectedSizeBytes);
        if (!fileLooksLikeNro(tmp) || sizeMismatch) {
            const std::string bad = req.outPath + ".bad";
            std::error_code ec;
            std::filesystem::remove(bad, ec);
            std::filesystem::rename(tmp, bad, ec);
            out.ok = false;
            out.error = sizeMismatch
                ? "Downloaded update size mismatch (got " + std::to_string(bytes) +
                  ", expected " + std::to_string(req.expectedSizeBytes) + ")."
                : "Downloaded file does not look like a valid NRO.";
            out.error += " Saved as: " + bad;
            out.errorInfo = classifyError(out.error, ErrorCategory::Data);
            return out;
        }
        std::error_code ec;
        std::filesystem::remove(req.outPath, ec);
        std::filesystem::rename(tmp, req.outPath, ec);
        if (ec) {
            out.ok = false;
            out.error = "Failed to finalize staged update: " + ec.message();
            out.errorInfo = classifyError(out.error, ErrorCategory::Filesystem);
            return out;
        }
        if (!writeTextFileEnsureParent(kUpdatePendingPath, req.outPath)) {
            out.ok = false;
            out.error = std::string("Failed to record pending update (") + kUpdatePendingPath + ")";
            out.errorInfo = classifyError(out.error, ErrorCategory::Filesystem);
            return out;
        }
        out.ok = true;
        return out;
    });

    coverLoader.start([](const std::string& url, const Config& cfg,
                         std::vector<unsigned char>& outData, std::string& err) -> bool {
        std::string body;
        if (!fetchBinary(cfg, url, body, err)) return false;
        outData.assign(body.begin(), body.end());
        return true;
    });
}

void RomServices::Stop() {
    romFetchJobs.stop();
    remoteSearchJobs.stop();
    diagProbeJobs.stop();
    biosListJobs.stop();
    saveCancel.store(true);
    saveSyncJobs.stop();
    updateCheckJobs.stop();
    updateDownloadJobs.stop();
    orchestratedSyncJobs.stop();
    discoveryJobs.stop();
    coverLoader.stop();
    stopDownloadWorker();
}

// ---- RebuildVisibleRoms (ported) ----

// Core rebuild; caller must hold status.mutex. Static caches are only touched
// from the UI thread (PollJobs/SetRomSearchQuery), matching the old main.cpp.
void RomServices::RebuildVisibleRomsLocked(bool resetSelection) {
    static uint64_t sIndexBuiltFor = 0;
    static std::vector<std::string> sNormalizedTitles;
    static bool sIndexBuiltForRemote = false;
    static uint64_t sCompletionCacheBuiltFor = 0;
    static std::unordered_map<std::string, bool> sCompletionById;
    static bool sCompletionBuiltForRemote = false;

    const bool useRemoteSource = remoteSearchActive &&
                                 status.currentPlatformId == remoteSearchPlatformId &&
                                 status.romSearchQuery == remoteSearchQuery;
    const auto& sourceRoms = useRemoteSource ? remoteSearchGames : status.romsAll;
    const uint64_t sourceRev = useRemoteSource ? remoteSearchRevision : status.romsAllRevision;

    if (sIndexBuiltFor != sourceRev || sIndexBuiltForRemote != useRemoteSource ||
        sNormalizedTitles.size() != sourceRoms.size()) {
        sNormalizedTitles.clear();
        sNormalizedTitles.reserve(sourceRoms.size());
        for (const auto& g : sourceRoms) {
            sNormalizedTitles.push_back(NormalizeSearchText(g.title));
        }
        sIndexBuiltFor = sourceRev;
        sIndexBuiltForRemote = useRemoteSource;
    }
    if (sCompletionCacheBuiltFor != sourceRev || sCompletionBuiltForRemote != useRemoteSource) {
        sCompletionById.clear();
        sCompletionCacheBuiltFor = sourceRev;
        sCompletionBuiltForRemote = useRemoteSource;
    }

    std::unordered_map<std::string, QueueState> stateById;
    stateById.reserve(status.downloadQueue.size() + status.downloadHistory.size());
    for (const auto& qi : status.downloadHistory) {
        if (!qi.game.id.empty()) stateById[qi.game.id] = qi.state;
    }
    for (const auto& qi : status.downloadQueue) {
        if (!qi.game.id.empty()) stateById[qi.game.id] = qi.state;
    }
    auto isCompletedCached = [&](const Game& g) -> bool {
        if (g.id.empty()) return false;
        auto it = sCompletionById.find(g.id);
        if (it != sCompletionById.end()) return it->second;
        bool v = isGameCompletedOnDisk(g, config);
        sCompletionById[g.id] = v;
        return v;
    };
    auto matchesFilter = [&](const Game& g) -> bool {
        auto it = g.id.empty() ? stateById.end() : stateById.find(g.id);
        std::optional<QueueState> st;
        if (it != stateById.end()) st = it->second;
        switch (status.romFilter) {
            case RomFilter::All: return true;
            case RomFilter::Queued:
                return st.has_value() &&
                       (*st == QueueState::Pending || *st == QueueState::Downloading ||
                        *st == QueueState::Finalizing);
            case RomFilter::Resumable:
                return st.has_value() && *st == QueueState::Resumable;
            case RomFilter::Failed:
                return st.has_value() && *st == QueueState::Failed;
            case RomFilter::Completed:
                return (st.has_value() && *st == QueueState::Completed) || isCompletedCached(g);
            case RomFilter::NotQueued:
                return !st.has_value() && !isCompletedCached(g);
            default: return true;
        }
    };

    std::vector<size_t> indices;
    indices.reserve(sourceRoms.size());
    std::string searchNorm = NormalizeSearchText(status.romSearchQuery);
    for (size_t i = 0; i < sourceRoms.size(); ++i) {
        if (!searchNorm.empty()) {
            if (i >= sNormalizedTitles.size() ||
                sNormalizedTitles[i].find(searchNorm) == std::string::npos) {
                continue;
            }
        }
        if (!matchesFilter(sourceRoms[i])) continue;
        indices.push_back(i);
    }
    auto cmpTitleAsc = [&](size_t a, size_t b) {
        const std::string& ta = (a < sNormalizedTitles.size()) ? sNormalizedTitles[a] : sourceRoms[a].title;
        const std::string& tb = (b < sNormalizedTitles.size()) ? sNormalizedTitles[b] : sourceRoms[b].title;
        if (ta != tb) return ta < tb;
        return sourceRoms[a].id < sourceRoms[b].id;
    };
    switch (status.romSort) {
        case RomSort::TitleAsc:
            std::sort(indices.begin(), indices.end(), cmpTitleAsc);
            break;
        case RomSort::TitleDesc:
            std::sort(indices.begin(), indices.end(), [&](size_t a, size_t b) { return cmpTitleAsc(b, a); });
            break;
        case RomSort::SizeDesc:
            std::sort(indices.begin(), indices.end(), [&](size_t a, size_t b) {
                if (sourceRoms[a].sizeBytes != sourceRoms[b].sizeBytes)
                    return sourceRoms[a].sizeBytes > sourceRoms[b].sizeBytes;
                return cmpTitleAsc(a, b);
            });
            break;
        case RomSort::SizeAsc:
            std::sort(indices.begin(), indices.end(), [&](size_t a, size_t b) {
                if (sourceRoms[a].sizeBytes != sourceRoms[b].sizeBytes)
                    return sourceRoms[a].sizeBytes < sourceRoms[b].sizeBytes;
                return cmpTitleAsc(a, b);
            });
            break;
    }
    std::vector<Game> rebuilt;
    rebuilt.reserve(indices.size());
    for (size_t idx : indices) rebuilt.push_back(sourceRoms[idx]);
    status.roms = std::move(rebuilt);
    status.romsRevision++;
    if (resetSelection) {
        status.selectedRomIndex = 0;
    } else if (status.selectedRomIndex >= static_cast<int>(status.roms.size())) {
        status.selectedRomIndex = status.roms.empty() ? 0 : static_cast<int>(status.roms.size()) - 1;
    } else if (status.selectedRomIndex < 0) {
        status.selectedRomIndex = 0;
    }
    visibleRomsSnapshot = status.roms;
}

// Public wrapper: takes the lock, then runs the core.
void RomServices::RebuildVisibleRoms(bool resetSelection) {
    std::lock_guard<std::mutex> lock(status.mutex);
    RebuildVisibleRomsLocked(resetSelection);
}

// ---- Submissions ----

void RomServices::SubmitRomFetch(const std::string& platformId, const std::string& slug,
                                 const std::string& name, bool startNewGeneration) {
    PendingRomFetch req;
    {
        std::lock_guard<std::mutex> lock(status.mutex);
        if (startNewGeneration) status.romFetchGeneration++;
        req.mode = PendingRomFetch::Mode::Page;
        req.pid = platformId;
        req.slug = slug;
        req.name = name;
        req.offset = 0;
        req.limit = kRomsFirstPageLimit;
        req.generation = status.romFetchGeneration;
        status.netBusy.store(true);
        status.netBusySinceMs.store(TicksMs());
        status.netBusyWhat = "Fetching ROMs...";
    }
    romFetchJobs.submit(req);
}

void RomServices::SubmitRemoteSearch(const std::string& platformId, const std::string& query) {
    PendingRemoteSearch req;
    {
        std::lock_guard<std::mutex> lock(status.mutex);
        remoteSearchGeneration++;
        req.generation = remoteSearchGeneration;
        req.pid = platformId;
        req.query = query;
        req.limit = kRemoteSearchLimit;
        remoteSearchInFlight = true;
        status.netBusy.store(true);
        status.netBusySinceMs.store(TicksMs());
        status.netBusyWhat = "Remote search...";
    }
    remoteSearchJobs.submit(req);
}

void RomServices::SubmitDiagnosticsProbe() {
    DiagProbeReq req{};
    {
        std::lock_guard<std::mutex> lock(status.mutex);
        status.diagnosticsProbeGeneration++;
        req.generation = status.diagnosticsProbeGeneration;
        status.diagnosticsProbeInFlight = true;
        status.diagnosticsLastProbeMs = TicksMs();
        status.diagnosticsLastProbeDetail.clear();
    }
    diagProbeJobs.submit(req);
}

void RomServices::SubmitBiosList(const std::string& id, const std::string& slug, const std::string& name) {
    BiosListReq req{};
    {
        std::lock_guard<std::mutex> lock(status.mutex);
        biosListGeneration++;
        req.generation = biosListGeneration;
        req.platformId = id;
        req.platformSlug = slug;
        req.platformName = name;
        status.netBusy.store(true);
        status.netBusySinceMs.store(TicksMs());
        status.netBusyWhat = "Checking BIOS...";
    }
    biosListJobs.submit(req);
}

void RomServices::SubmitLoginPairing() {
    PairingReq req{};
    {
        std::lock_guard<std::mutex> lock(status.mutex);
        if (status.saveBusy.load()) return;
        saveCancel.store(false);
        saveSyncGeneration++;
        loginPairGeneration = saveSyncGeneration; // PollJobs latch key
        req.generation = saveSyncGeneration;
        req.serverUrl = config.serverUrl;
        req.basicAuthRaw = basicAuthHeader(config);
        req.timeoutSeconds = config.httpTimeoutSeconds;
        uint64_t a = std::chrono::steady_clock::now().time_since_epoch().count();
        uint64_t b = (static_cast<uint64_t>(TicksMs()) * 2654435761u) ^ 0x5f5f5f5fu;
        char buf[17];
        snprintf(buf, sizeof(buf), "%08x%08x", static_cast<uint32_t>(a ^ (b >> 32)), static_cast<uint32_t>(b));
        req.clientDeviceId = buf;
        status.saveBusy.store(true);
        status.savePairState = SavePairState::Initiating;
        status.savePairDetail.clear();
        status.savePairUserCode.clear();
        status.saveVerificationPath.clear();
    }
    saveSyncJobs.submit(req);
}

void RomServices::SubmitDiscovery(const char* why) {
    DiscoveryReq req{};
    {
        std::lock_guard<std::mutex> lock(status.mutex);
        if (discoveryJobs.busy()) {
            romm::logLine(std::string("DISCOVERY: already running, ignoring ") + why);
            return;
        }
        discoveryGeneration++;
        req.generation = discoveryGeneration;
        req.savesRoot = effectiveSaveDir(config);
        req.statesRoot = effectiveStatesDir(config);
        for (const auto& g : status.romsAll) {
            const std::string& gid = g.id;
            long long rid = 0;
            bool ok = !gid.empty();
            for (char c : gid) {
                if (c < '0' || c > '9') { ok = false; break; }
                rid = rid * 10 + (static_cast<long long>(c - '0'));
            }
            if (!ok || rid <= 0) continue;
            req.roms.push_back({rid, g.fsName, g.platformSlug});
        }
    }
    discoveryJobs.submit(req);
}

void RomServices::SubmitServerSync(const char* why) {
    SyncRunReq req{};
    bool haveToken = false;
    {
        std::lock_guard<std::mutex> lock(status.mutex);
        if (status.saveSyncRunBusy.load()) return;
        if (config.apiToken.empty()) {
            status.settingsStatus = "Pair a device first (Sign in).";
            return;
        }
        req.deviceId = status.pairedDeviceId;
        haveToken = !req.deviceId.empty();
    }
    if (!haveToken) {
        DeviceToken tok;
        if (loadDeviceToken(kDeviceTokenPath, tok)) req.deviceId = tok.deviceId;
    }
    if (req.deviceId.empty()) {
        std::lock_guard<std::mutex> lock(status.mutex);
        status.settingsStatus = "No device id; re-pair to register a device.";
        return;
    }
    {
        std::lock_guard<std::mutex> lock(status.mutex);
        syncRunGeneration++;
        req.generation = syncRunGeneration;
        status.saveSyncRunBusy.store(true);
        status.saveSyncStatusText = "Syncing...";
    }
    (void)why;
    orchestratedSyncJobs.submit(req);
}

void RomServices::SubmitUpdateCheck() {
    UpdateCheckReq req{};
    {
        std::lock_guard<std::mutex> lock(status.mutex);
        updateGeneration++;
        req.generation = updateGeneration;
        updateCheckGenSubmitted = req.generation;
        status.updateCheckInFlight = true;
        status.updateChecked = false;
        status.updateAvailable = false;
        status.updateError.clear();
        status.updateStatus = "Checking GitHub releases...";
    }
    updateCheckJobs.submit(req);
}

void RomServices::SubmitUpdateDownload() {
    UpdateDownloadReq req{};
    const std::string updateDir = computeUpdateDirFromDownloadDir(config.downloadDir);
    ensureDirectory(updateDir);
    {
        std::lock_guard<std::mutex> lock(status.mutex);
        if (!status.updateAvailable || status.updateAssetUrl.empty()) {
            status.updateStatus = "No update available to download.";
            return;
        }
        if (status.updateDownloadInFlight) {
            status.updateStatus = "Update download already in progress.";
            return;
        }
        updateGeneration++;
        req.generation = updateGeneration;
        updateDownloadGenSubmitted = req.generation;
        req.url = status.updateAssetUrl;
        req.outPath = defaultStagedUpdatePath(updateDir);
        req.expectedSizeBytes = status.updateAssetSizeBytes;
        status.updateStagedPath = req.outPath;
        status.updateDownloadInFlight = true;
        status.updateDownloaded = false;
        status.updateError.clear();
        status.updateStatus = "Downloading update...";
    }
    updateDownloadJobs.submit(req);
}

// ---- Filter/search mutations ----

void RomServices::SetRomSearchQuery(const std::string& q) {
    std::string next = NormalizeSearchText(q);
    std::string platformId;
    size_t romCount = 0;
    bool submitRemote = false;
    std::string pid;
    {
        std::lock_guard<std::mutex> lock(status.mutex);
        std::string cur = status.romSearchQuery;
        status.romSearchQuery = next;
        status.romListOptionsRevision++;
        status.selectedRomIndex = 0;
        platformId = status.currentPlatformId;
        romCount = status.romsAll.size();
        if (next.empty()) {
            remoteSearchActive = false;
        } else if (!platformId.empty() && romCount >= kRemoteSearchThreshold) {
            pid = platformId;
            submitRemote = true;
        } else {
            remoteSearchActive = false;
            remoteSearchGames.clear();
            remoteSearchQuery.clear();
            remoteSearchPlatformId.clear();
            remoteSearchRevision++;
        }
        (void)cur;
    }
    if (submitRemote) SubmitRemoteSearch(pid, next);
    BumpUi();
}

void RomServices::CycleRomFilter(int dir) {
    std::lock_guard<std::mutex> lock(status.mutex);
    int f = static_cast<int>(status.romFilter);
    int count = 6; // All, Queued, Resumable, Failed, Completed, NotQueued
    f = ((f + dir) % count + count) % count;
    status.romFilter = static_cast<RomFilter>(f);
    status.romListOptionsRevision++;
    BumpUi();
}

void RomServices::CycleRomSort(int dir) {
    std::lock_guard<std::mutex> lock(status.mutex);
    int s = static_cast<int>(status.romSort);
    int count = 4;
    s = ((s + dir) % count + count) % count;
    status.romSort = static_cast<RomSort>(s);
    status.romListOptionsRevision++;
    BumpUi();
}

// ---- Settings persistence ----

std::string RomServices::NormalizeServerUrl(std::string url) {
    // Trim whitespace.
    while (!url.empty() && (url.front() == ' ' || url.front() == '\t')) url.erase(url.begin());
    while (!url.empty() && (url.back() == ' ' || url.back() == '\t' || url.back() == '/')) url.pop_back();
    if (url.empty()) return url;
    if (url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0) return url;
    return "https://" + url; // Grout/RomM default: TLS-first
}

void RomServices::RefetchPlatforms() {
    {
        std::lock_guard<std::mutex> lock(status.mutex);
        status.platforms.clear();
        status.roms.clear();
        status.romsAll.clear();
        status.romsRevision++;
        status.romsAllRevision++;
        status.romFetchGeneration++;
        status.selectedPlatformIndex = 0;
        status.selectedRomIndex = 0;
        status.netBusy.store(true);
        status.netBusyWhat = "Platforms";
        romFetchJobs.clearPending();
        remoteSearchJobs.clearPending();
    }
    // Blocking-but-bounded: runs on the UI thread like boot; the busy line
    // keeps the screen informative. On failure the error lands on Status and
    // the platform screen shows the hint.
    std::string err;
    ErrorInfo info;
    if (!fetchPlatforms(config, status, err, &info)) {
        std::lock_guard<std::mutex> lock(status.mutex);
        status.lastError = err;
        status.lastErrorInfo = info.code == ErrorCode::None
            ? classifyError(err, ErrorCategory::Network) : info;
    }
    ApplyPlatformFilter();
    BumpUi();
}

void RomServices::ApplyPlatformFilter() {
    // Drop platforms without a tico emulator/core when the toggle is on.
    // Runs after every platform fetch so both boot and settings changes land
    // on the same filtered view; caller holds no lock.
    if (!config.hideUnsupportedPlatforms) return;
    std::lock_guard<std::mutex> lock(status.mutex);
    auto& plats = status.platforms;
    size_t before = plats.size();
    plats.erase(std::remove_if(plats.begin(), plats.end(),
                               [](const Platform& p) {
                                   return !romm::platformSupportedByTico(p.slug);
                               }),
                plats.end());
    if (plats.size() != before) {
        romm::logLine("PLATFORMS: hid " + std::to_string(before - plats.size()) +
                      " platform(s) without a tico core");
    }
    if (status.selectedPlatformIndex >= static_cast<int>(plats.size())) {
        status.selectedPlatformIndex = 0;
    }
}
bool RomServices::PersistConfig(const std::string& okMsg) {
    std::string saveErr;
    if (!saveConfigJson(config, saveErr)) {
        romm::logLine("SETTINGS: config.json write failed: " + saveErr);
        std::lock_guard<std::mutex> lock(status.mutex);
        status.settingsStatus = "Save failed: " + saveErr;
        return false;
    }
    std::lock_guard<std::mutex> lock(status.mutex);
    status.settingsStatus = okMsg;
    BumpUi();
    return true;
}

void RomServices::PersistQueueState() {
    std::string qerr;
    if (!saveQueueState(status, qerr)) {
        romm::logLine("Queue state save warning: " + qerr);
    }
}

void RomServices::RecomputeTotals() {
    std::lock_guard<std::mutex> lock(status.mutex);
    uint64_t remaining = 0;
    if (status.downloadWorkerRunning.load()) {
        uint64_t curSize = status.currentDownloadSize.load();
        uint64_t curDone = status.currentDownloadedBytes.load();
        if (curSize > curDone) remaining += (curSize - curDone);
    }
    for (const auto& q : status.downloadQueue) {
        remaining += q.bundle.totalSize();
    }
    uint64_t already = status.totalDownloadedBytes.load();
    status.totalDownloadBytes.store(already + remaining);
}

// ---- Enqueue ----

bool RomServices::EnqueueGame(const Game& gIn) {
    Game enriched = gIn;
    std::string err;
    ErrorInfo errInfo;
    if (!enrichGameWithFiles(config, enriched, err, &errInfo)) {
        std::lock_guard<std::mutex> lock(status.mutex);
        status.lastError = err;
        status.lastErrorInfo = errInfo.code == ErrorCode::None
            ? classifyError(err, ErrorCategory::Data) : errInfo;
        return false;
    }
    DownloadBundle bundle = buildBundleFromGame(enriched, status.platformPrefs);
    if (!bundle.files.empty()) enriched.sizeBytes = bundle.totalSize();
    if (!canEnqueueGame(status, enriched)) {
        romm::logLine("ROM already queued this session: " + enriched.title);
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(status.mutex);
        for (auto& mg : status.romsAll) {
            if (mg.id == enriched.id) { mg = enriched; break; }
        }
        QueueItem qi;
        qi.game = enriched;
        qi.bundle = bundle;
        qi.state = QueueState::Pending;
        status.downloadQueue.push_back(std::move(qi));
        status.downloadQueueRevision++;
        status.romsRevision++;
        status.selectedQueueIndex = 0;
        status.downloadCompleted = false;
    }
    RecomputeTotals();
    PersistQueueState();
    romm::logLine("Queued ROM: " + enriched.title);
    return true;
}

int RomServices::EnqueueGamesBulk(const std::vector<Game>& games) {
    // Multi-select queueing: enrich + append each game, one persistence pass.
    // Returns how many were newly queued (duplicates skipped).
    int queued = 0;
    {
        std::lock_guard<std::mutex> lock(status.mutex);
        status.downloadCompleted = false;
    }
    for (const auto& gIn : games) {
        Game enriched = gIn;
        std::string err;
        ErrorInfo errInfo;
        if (!enrichGameWithFiles(config, enriched, err, &errInfo)) {
            std::lock_guard<std::mutex> lock(status.mutex);
            status.lastError = err;
            status.lastErrorInfo = errInfo.code == ErrorCode::None
                ? classifyError(err, ErrorCategory::Data) : errInfo;
            continue;
        }
        DownloadBundle bundle = buildBundleFromGame(enriched, status.platformPrefs);
        if (!bundle.files.empty()) enriched.sizeBytes = bundle.totalSize();
        if (!canEnqueueGame(status, enriched)) {
            romm::logLine("Bulk: already queued this session: " + enriched.title);
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(status.mutex);
            for (auto& mg : status.romsAll) {
                if (mg.id == enriched.id) { mg = enriched; break; }
            }
            QueueItem qi;
            qi.game = enriched;
            qi.bundle = bundle;
            qi.state = QueueState::Pending;
            status.downloadQueue.push_back(std::move(qi));
            status.downloadQueueRevision++;
            status.romsRevision++;
        }
        romm::logLine("Bulk queued ROM: " + enriched.title);
        queued++;
    }
    if (queued > 0) {
        {
            std::lock_guard<std::mutex> lock(status.mutex);
            status.selectedQueueIndex = 0;
        }
        RecomputeTotals();
        PersistQueueState();
    }
    return queued;
}

void RomServices::QueuePlatformBulk(const Platform& p) {
    // Queue every ROM of a platform: page through the library, enrich each
    // row, bulk-enqueue. Runs inline (UI thread) — same cost as opening the
    // platform's list; the toast + manager give immediate feedback.
    std::vector<Game> all;
    size_t offset = 0;
    const size_t page = 250;
    std::string lastErr;
    for (;;) {
        GamesPage pg;
        std::string err;
        if (!romm::fetchGamesPageForPlatform(config, p.id, offset, page, pg, err)) {
            lastErr = err;
            break;
        }
        for (auto& g : pg.games) {
            g.platformSlug = p.slug;
            all.push_back(std::move(g));
        }
        if (!pg.hasMore) break;
        offset = pg.offset + pg.games.size();
    }
    if (all.empty()) {
        std::lock_guard<std::mutex> lock(status.mutex);
        status.lastError = lastErr.empty() ? "Platform has no ROMs" : lastErr;
        BumpUi();
        return;
    }
    int n = EnqueueGamesBulk(all);
    std::lock_guard<std::mutex> lock(status.mutex);
    status.settingsStatus = "Queued " + std::to_string(n) + "/" +
                            std::to_string(all.size()) + " ROMs from " + p.name;
    BumpUi();
}

// ---- Covers ----

void RomServices::RequestCover(const std::string& url, const std::string& title) {
    if (url.empty()) return;
    if (url == requestedCoverUrl) return;
    requestedCoverUrl = url;
    CoverJob job;
    job.url = url;
    job.title = title;
    job.cfg = config;
    coverLoader.request(job, currentCoverKey);
}

void RomServices::PollCover() {
    auto res = coverLoader.poll();
    if (!res) return;
    if (res->ok && !res->pixels.empty()) {
        currentCover = std::move(*res);
        currentCoverKey = currentCover.url;
        romm::logLine("Loaded cover for " + currentCover.title);
        BumpUi(); // details screen rebuilds with the boxart in place
    } else {
        currentCoverKey = res->url; // failed; don't retry every frame
    }
}

// ---- PollJobs (ported main-loop poll application) ----

void RomServices::PollJobs() {
    reapDownloadWorkerIfDone();

    if (auto done = romFetchJobs.pollResult()) {
        bool queueNextPage = false;
        PendingRomFetch nextReq;
        bool navToGameList = false; // deferred: nav rebuilds UI -> locks status.mutex
        {
            std::lock_guard<std::mutex> lock(status.mutex);
            const bool staleResult = (done->req.generation != status.romFetchGeneration);
            if (staleResult) {
                if (!romFetchJobs.busy() && !remoteSearchInFlight) {
                    status.netBusy.store(false);
                    status.netBusyWhat.clear();
                }
            } else if (done->probeOnly) {
                bool usedProbeCache = false;
                if (done->probeUnchanged) {
                    uint32_t nowMs = TicksMs();
                    if (status.currentPlatformId == done->req.pid && !status.romsAll.empty()) {
                        currentPlatformFetchedAtMs = nowMs;
                        if (!done->identifierDigest.empty())
                            currentPlatformIdentifierDigest = done->identifierDigest;
                        usedProbeCache = true;
                    } else {
                        auto hit = platformRomsCache.find(done->req.pid);
                        if (hit != platformRomsCache.end() && !hit->second.games.empty()) {
                            if (!status.currentPlatformId.empty() &&
                                status.currentPlatformId != done->req.pid &&
                                !status.romsAll.empty()) {
                                CachedPlatformRoms keep;
                                keep.games = std::move(status.romsAll);
                                keep.slug = status.currentPlatformSlug;
                                keep.name = status.currentPlatformName;
                                keep.identifierDigest = currentPlatformIdentifierDigest;
                                keep.fetchedAtMs = currentPlatformFetchedAtMs;
                                platformRomsCache[status.currentPlatformId] = std::move(keep);
                            }
                            status.romsAll = std::move(hit->second.games);
                            status.romsAllRevision++;
                            status.currentPlatformId = done->req.pid;
                            status.currentPlatformSlug = hit->second.slug.empty() ? done->req.slug : hit->second.slug;
                            status.currentPlatformName = hit->second.name.empty() ? done->req.name : hit->second.name;
                            currentPlatformFetchedAtMs = nowMs;
                            currentPlatformIdentifierDigest =
                                !done->identifierDigest.empty() ? done->identifierDigest : hit->second.identifierDigest;
                            usedProbeCache = true;
                        }
                    }
                    if (usedProbeCache) {
                        status.netBusy.store(false);
                        status.netBusyWhat.clear();
                    }
                }
                if (!usedProbeCache && done->req.generation == status.romFetchGeneration) {
                    // Fall back to a full page fetch.
                    nextReq.mode = PendingRomFetch::Mode::Page;
                    nextReq.pid = done->req.pid;
                    nextReq.slug = done->req.slug;
                    nextReq.name = done->req.name;
                    nextReq.offset = 0;
                    nextReq.limit = kRomsFirstPageLimit;
                    queueNextPage = true;
                    status.netBusyWhat = "Fetching ROMs...";
                }
            } else if (!done->ok) {
                status.netBusy.store(false);
                status.netBusyWhat.clear();
                if (done->offset == 0) {
                    status.lastError = done->error;
                    status.lastErrorInfo = done->errorInfo.code == ErrorCode::None
                        ? classifyError(done->error, ErrorCategory::Network)
                        : done->errorInfo;
                    BumpUi();
                    if (toastHook) toastHook("Failed to fetch ROMs: " + done->error);
                }
            } else {
                const uint32_t nowMs = TicksMs();
                if (done->offset == 0) {
                    if (!status.currentPlatformId.empty() &&
                        status.currentPlatformId != done->req.pid &&
                        !status.romsAll.empty()) {
                        CachedPlatformRoms keep;
                        keep.games = std::move(status.romsAll);
                        keep.slug = status.currentPlatformSlug;
                        keep.name = status.currentPlatformName;
                        keep.identifierDigest = currentPlatformIdentifierDigest;
                        keep.fetchedAtMs = currentPlatformFetchedAtMs;
                        platformRomsCache[status.currentPlatformId] = std::move(keep);
                    }
                    status.romsAll = std::move(done->games);
                    status.romsAllRevision++;
                    status.currentPlatformId = done->req.pid;
                    status.currentPlatformSlug = done->req.slug;
                    status.currentPlatformName = done->req.name;
                    currentPlatformFetchedAtMs = nowMs;
                    if (!done->identifierDigest.empty())
                        currentPlatformIdentifierDigest = done->identifierDigest;
                    remoteSearchActive = false;
                    remoteSearchGames.clear();
                    remoteSearchQuery.clear();
                    remoteSearchPlatformId.clear();
                    remoteSearchRevision++;
                    pagedFetchNextOffset = done->nextOffset;
                    pagedFetchPageLimit = kRomsNextPageLimit;
                    if (done->hasMore) {
                        status.netBusy.store(true);
                        status.netBusyWhat = "Loading remaining ROMs...";
                        nextReq.mode = PendingRomFetch::Mode::Page;
                        nextReq.pid = done->req.pid;
                        nextReq.slug = done->req.slug;
                        nextReq.name = done->req.name;
                        nextReq.offset = pagedFetchNextOffset;
                        nextReq.limit = pagedFetchPageLimit;
                        queueNextPage = true;
                    } else {
                        status.netBusy.store(false);
                        status.netBusyWhat.clear();
                    }
                    RebuildVisibleRomsLocked(true);
                    // Defer navigation: navHook rebuilds screens which lock
                    // status.mutex; calling it under the lock deadlocks.
                    navToGameList = true;
                } else {
                    size_t before = status.romsAll.size();
                    std::unordered_set<std::string> existing;
                    existing.reserve(status.romsAll.size() + done->games.size());
                    for (const auto& g : status.romsAll) existing.insert(g.id);
                    for (auto& g : done->games) {
                        if (existing.insert(g.id).second) status.romsAll.push_back(std::move(g));
                    }
                    (void)before;
                    status.romsAllRevision++;
                    if (done->hasMore) {
                        pagedFetchNextOffset = done->nextOffset;
                        nextReq.mode = PendingRomFetch::Mode::Page;
                        nextReq.pid = done->req.pid;
                        nextReq.slug = done->req.slug;
                        nextReq.name = done->req.name;
                        nextReq.offset = pagedFetchNextOffset;
                        nextReq.limit = pagedFetchPageLimit;
                        queueNextPage = true;
                        status.netBusy.store(true);
                        status.netBusyWhat = "Loading remaining ROMs...";
                    } else {
                        status.netBusy.store(false);
                        status.netBusyWhat.clear();
                    }
                }
                BumpUi();
            }
        }
        if (queueNextPage) {
            {
                std::lock_guard<std::mutex> lock(status.mutex);
                nextReq.generation = status.romFetchGeneration;
                status.netBusySinceMs.store(TicksMs());
            }
            romFetchJobs.submit(nextReq);
        }
        if (navToGameList && navHook) {
            navHook(ScreenId::GameList, NavOp::ReplaceAll); // outside status.mutex
        }
    }

    if (auto searchDone = remoteSearchJobs.pollResult()) {
        std::lock_guard<std::mutex> lock(status.mutex);
        if (searchDone->req.generation != remoteSearchGeneration) {
            if (!romFetchJobs.busy() && !remoteSearchInFlight) {
                status.netBusy.store(false);
                status.netBusyWhat.clear();
            }
        } else {
            remoteSearchInFlight = false;
            if (searchDone->ok && !searchDone->req.query.empty() &&
                searchDone->req.pid == status.currentPlatformId &&
                searchDone->req.query == status.romSearchQuery) {
                remoteSearchGames = std::move(searchDone->games);
                remoteSearchActive = true;
                remoteSearchQuery = searchDone->req.query;
                remoteSearchPlatformId = searchDone->req.pid;
                remoteSearchRevision++;
                status.romListOptionsRevision++;
            } else if (!searchDone->ok) {
                remoteSearchActive = false;
                remoteSearchGames.clear();
                remoteSearchQuery.clear();
                remoteSearchPlatformId.clear();
                remoteSearchRevision++;
                status.romListOptionsRevision++;
            }
            if (!romFetchJobs.busy()) {
                status.netBusy.store(false);
                status.netBusyWhat.clear();
            }
            BumpUi();
        }
    }

    if (auto probe = diagProbeJobs.pollResult()) {
        std::lock_guard<std::mutex> lock(status.mutex);
        if (probe->generation == status.diagnosticsProbeGeneration) {
            status.diagnosticsProbeInFlight = false;
            status.diagnosticsServerReachableKnown = true;
            status.diagnosticsServerReachable = probe->ok;
            status.diagnosticsLastProbeMs = TicksMs();
            status.diagnosticsLastProbeDetail = probe->ok
                ? probe->detail
                : probe->detail + " (" + errorCodeLabel(probe->errorInfo.code) + ")";
            BumpUi();
        }
    }

    if (auto discDone = discoveryJobs.pollResult()) {
        lastDiscovery = std::move(*discDone);
        struct GameAgg { int saves{0}; int states{0}; };
        std::map<std::string, GameAgg> games;
        std::map<std::string, SyncPlatformRow> plats;
        for (const auto& f : lastDiscovery.files) {
            auto& agg = games[f.gameKey];
            if (f.isState) agg.states++; else agg.saves++;
            std::string topSlug;
            if (!f.folderSlugs.empty()) topSlug = f.folderSlugs.front();
            if (!topSlug.empty()) {
                auto& prow = plats[topSlug];
                prow.slug = topSlug;
                if (f.isState) prow.stateCount++; else prow.saveCount++;
            }
        }
        {
            std::lock_guard<std::mutex> lock(status.mutex);
            status.syncPlanRevision++;
            status.syncFiles.clear();
            status.syncGames.clear();
            status.syncPlatforms.clear();
            const bool drillOpen = false; // screens own their drill state now
            (void)drillOpen;
            for (const auto& f : lastDiscovery.files)
                status.syncFiles.push_back(
                    SyncFileRow{f.path, f.gameKey, f.isState, f.slotLabel, f.updatedAtIso});
            for (auto& kv : plats) {
                SyncPlatformRow row;
                row.slug = kv.second.slug;
                row.name = kv.second.slug;
                row.saveCount = kv.second.saveCount;
                row.stateCount = kv.second.stateCount;
                bool named = false;
                for (const auto& p : status.platforms) {
                    std::string pslugLower;
                    for (char c : p.slug) {
                        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 'a' - 'A');
                        pslugLower.push_back(c);
                    }
                    if (pslugLower == row.slug) { row.name = p.name; named = true; break; }
                }
                if (!named && row.name.empty()) row.name = row.slug;
                status.syncPlatforms.push_back(row);
            }
            status.syncStatusText.clear();
            BumpUi();
        }
    }

    if (auto biosDone = biosListJobs.pollResult()) {
        std::unique_lock<std::mutex> lock(status.mutex);
        status.netBusy.store(false);
        status.netBusyWhat.clear();
        if (biosDone->generation == biosListGeneration) {
            if (!biosDone->error.empty()) {
                romm::logLine("BIOS: list failed for " + biosDone->platformName + ": " + biosDone->error);
                std::string msg = "BIOS check failed: " + biosDone->error;
                lock.unlock();
                if (toastHook) toastHook(msg);
            } else if (biosDone->files.empty()) {
                lock.unlock();
                if (toastHook) toastHook("This platform doesn't require any BIOS files.");
            } else {
                DownloadBundle bundle = buildFirmwareBundle(
                    biosDone->platformSlug, biosDone->platformName,
                    biosDone->files, config.serverUrl);
                Game biosGame;
                biosGame.id = std::string("__bios__") + biosDone->platformSlug;
                biosGame.title = bundle.title;
                biosGame.platformSlug = biosDone->platformSlug;
                biosGame.sizeBytes = bundle.totalSize();
                lock.unlock();
                const bool alreadyQueued = !canEnqueueGame(status, biosGame);
                if (alreadyQueued) {
                    romm::logLine("BIOS already queued this session: " + biosGame.title);
                } else {
                    QueueItem qi;
                    qi.game = biosGame;
                    qi.bundle = bundle;
                    qi.state = QueueState::Pending;
                    {
                        std::lock_guard<std::mutex> lock2(status.mutex);
                        status.downloadQueue.push_back(std::move(qi));
                        status.downloadQueueRevision++;
                        status.selectedQueueIndex = 0;
                        status.downloadCompleted = false;
                    }
                    RecomputeTotals();
                    PersistQueueState();
                    if (toastHook) toastHook("BIOS queued for " + biosDone->platformName);
                    if (navHook) navHook(ScreenId::DownloadManager, NavOp::Push);
                }
                BumpUi();
            }
        }
    }

    // LOGIN pairing completion -> adopt token, navigate back. One-shot: the
    // Paired state persists (the worker set it), so track the last adopted
    // generation; without this latch the toast+BumpUi re-fired every frame
    // and the toast/rebuild storm crashed the app.
    {
        bool loginPaired = false;
        uint64_t gen = 0;
        {
            std::lock_guard<std::mutex> lock(status.mutex);
            gen = loginPairGeneration;
            loginPaired = (status.savePairState == SavePairState::Paired &&
                           !status.saveBusy.load() &&
                           adoptedPairGeneration != gen);
        }
        if (loginPaired) {
            DeviceToken tok;
            bool haveTok = loadDeviceToken(kDeviceTokenPath, tok);
            {
                std::lock_guard<std::mutex> lock(status.mutex);
                if (haveTok) {
                    config.apiToken = tok.accessToken;
                    status.pairedDeviceId = tok.deviceId;
                }
                adoptedPairGeneration = gen; // consume regardless; avoid re-fire loops
            }
            if (haveTok) {
                if (toastHook) toastHook("Successfully paired!");
                pairSuccessPending.store(true); // render callback: banner + nav home
                BumpUi();
            }
        }
    }

    if (auto syncRun = orchestratedSyncJobs.pollResult()) {
        std::lock_guard<std::mutex> lock(status.mutex);
        if (syncRun->generation == syncRunGeneration) {
            status.saveSyncRunBusy.store(false);
            if (!syncRun->ok && !syncRun->error.empty()) {
                status.saveSyncStatusText = "Sync failed: " + syncRun->error;
            } else if (!syncRun->ok) {
                status.saveSyncStatusText = "Sync finished with errors.";
            } else {
                status.saveSyncStatusText = syncRun->error.empty()
                    ? "Sync complete." : syncRun->error;
            }
            BumpUi();
        }
    }

    if (auto upd = updateCheckJobs.pollResult()) {
        std::lock_guard<std::mutex> lock(status.mutex);
        if (upd->generation == updateCheckGenSubmitted) {
            status.updateCheckInFlight = false;
            status.updateChecked = upd->ok;
            status.updateError.clear();
            status.updateStatus.clear();
            if (!upd->ok) {
                status.updateError = upd->error.empty() ? "Update check failed." : upd->error;
                status.updateStatus = "Retry failed.";
            } else {
                status.updateLatestTag = upd->release.tagName;
                status.updateLatestName = upd->release.name;
                status.updateLatestPublishedAt = upd->release.publishedAt;
                status.updateReleaseHtmlUrl = upd->release.htmlUrl;
                status.updateAssetName = upd->asset.name;
                status.updateAssetUrl = upd->asset.downloadUrl;
                status.updateAssetSizeBytes = upd->asset.sizeBytes;
                status.updateAvailable = upd->updateAvailable;
                status.updateStatus = upd->updateAvailable ? "Update available." : "Up to date.";
            }
            BumpUi();
        }
    }

    if (auto dl = updateDownloadJobs.pollResult()) {
        std::lock_guard<std::mutex> lock(status.mutex);
        if (dl->generation == updateDownloadGenSubmitted) {
            status.updateDownloadInFlight = false;
            status.updateError.clear();
            if (!dl->ok) {
                status.updateDownloaded = false;
                status.updateError = dl->error.empty() ? "Update download failed." : dl->error;
                status.updateStatus = "Download failed.";
            } else {
                status.updateDownloaded = true;
                status.updateStatus = "Download complete. Restart app to apply.";
            }
            BumpUi();
        }
    }

    // Visible-rom rebuild on revision bumps (single lock; core runs locked).
    {
        std::lock_guard<std::mutex> lock(status.mutex);
        bool needRebuild = false;
        if (status.romsAllRevision != appliedRomsAllRev ||
            status.romListOptionsRevision != appliedRomsOptionsRev) {
            needRebuild = true;
        } else if (status.romFilter != RomFilter::All &&
                   (status.downloadQueueRevision != appliedQueueRevForRoms ||
                    status.downloadHistoryRevision != appliedHistRevForRoms)) {
            needRebuild = true;
        }
        if (needRebuild) {
            RebuildVisibleRomsLocked(false);
        }
        appliedRomsAllRev = status.romsAllRevision;
        appliedRomsOptionsRev = status.romListOptionsRevision;
        appliedQueueRevForRoms = status.downloadQueueRevision;
        appliedHistRevForRoms = status.downloadHistoryRevision;
    }

    // Worker events.
    {
        std::lock_guard<std::mutex> lock(status.mutex);
        if (!status.workerEvents.empty()) {
            for (const auto& ev : status.workerEvents) {
                if (ev.type == WorkerEventType::DownloadFailureState) {
                    status.lastDownloadFailed.store(ev.failed);
                    status.lastDownloadError = ev.message;
                } else if (ev.type == WorkerEventType::DownloadCompletion) {
                    status.downloadCompleted = true;
                }
            }
            status.workerEvents.clear();
            status.workerEventsRevision++;
            BumpUi();
        }
    }

    // Live Download Manager refresh: while a download is running and the
    // manager is the visible screen, rebuild at ~2 Hz so the inline percent,
    // speed, and state lines track progress without per-frame churn.
    if (status.downloadWorkerRunning.load() &&
        uiVisibleScreen.load() == static_cast<int>(ScreenId::DownloadManager)) {
        const uint32_t now = TicksMs();
        if (now - lastManagerProgressBumpMs >= 500) {
            lastManagerProgressBumpMs = now;
            BumpUi();
        }
    }

    PollCover();
}

std::string RomServices::NormalizeSearchText(const std::string& in) {
    return normalizeSearchText(in);
}

} // namespace romm::ui
