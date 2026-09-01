// Live integration test: exercises the production wire layer (SyncAuthCtx ops)
// against the real RomM server. NOT part of the UNIT_TEST suite (no -DUNIT_TEST;
// uses real curl). Run: ./romm_live_tests <base_url>
//
// Flow (mirrors the app's pairing + orchestrator sync):
//   1. Pair a device: init -> approve via Basic auth -> poll token.
//   2. GET /api/users/me, /api/platforms, /api/roms (items envelope) with the
//      device token -> sanity for platform/ROM read paths.
//   3. Upload a synthetic save + state for a live ROM id.
//   4. Negotiate -> complete session (server records the sync; a follow-up
//      negotiate with the same hash must NOT re-plan the upload).
//   5. List saves, download ours (round-trip), confirm-downloaded.
//   6. Cleanup: delete save + state + device. Non-destructive: touches only
//      its own data.
//
// Exit code 0 = all criteria exercised; non-zero with diagnostic on stderr.

#include "romm/save_sync.hpp"
#include "romm/http_common.hpp"
#include "mini/json.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace romm;

namespace {

std::string g_base = "https://romm.ainger.cloud";
std::string g_user = "test";
std::string g_pass = "password";

std::string b64(const std::string& in) {
    static const char* t =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, bits = -6;
    for (unsigned char c : in) {
        val = (val << 8) + c;
        bits += 8;
        while (bits >= 0) {
            out.push_back(t[(val >> bits) & 0x3F]);
            bits -= 6;
        }
    }
    if (bits > -6) out.push_back(t[((val << 8) >> (bits + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

bool httpJson(const std::string& method, const std::string& url,
              const std::string& auth, const std::string& contentType,
              const void* body, size_t bodySize, long& codeOut,
              std::string& bodyOut, std::string& err) {
    std::vector<std::pair<std::string, std::string>> headers;
    if (!auth.empty()) headers.emplace_back("Authorization", auth);
    if (!contentType.empty()) headers.emplace_back("Content-Type", contentType);
    HttpRequestOptions opt;
    opt.timeoutSec = 30;
    opt.requestBody = body;
    opt.requestBodySize = bodySize;
    HttpTransaction tx;
    if (!httpRequestBuffered(method, url, headers, opt, tx, err)) return false;
    codeOut = (long)tx.parsed.statusCode;
    bodyOut = tx.body;
    return true;
}

int fail(const char* stage, const std::string& detail) {
    std::fprintf(stderr, "LIVE FAIL [%s]: %s\n", stage, detail.c_str());
    return 1;
}

std::string findStr(const mini::Object& o, const char* key) {
    auto it = o.find(key);
    if (it == o.end()) return "";
    if (it->second.type == mini::Value::Type::String) return it->second.str;
    return "";
}

long long findInt(const mini::Object& o, const char* key) {
    auto it = o.find(key);
    if (it == o.end()) return 0;
    if (it->second.type == mini::Value::Type::Number) return (long long)it->second.number;
    return 0;
}

std::string esc(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') o.push_back('\\');
        o.push_back(c);
    }
    return o;
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1) g_base = argv[1];
    while (!g_base.empty() && g_base.back() == '/') g_base.pop_back();

    const std::string basic = "Basic " + b64(g_user + ":" + g_pass);
    std::string err;

    // ---- 1. Device pairing (init -> approve -> poll) ----
    std::string initBody =
        "{\"client_device_identifier\":\"romm-live-test-001\",\"client\":\"SwitchRomM\","
        "\"name\":\"SwitchRomM Live Test\",\"platform\":\"Switch\",\"client_version\":\"live-test\","
        "\"requested_scopes\":[\"me.read\",\"platforms.read\",\"roms.read\",\"collections.read\","
        "\"firmware.read\",\"assets.read\",\"assets.write\",\"devices.read\",\"devices.write\"]}";
    long code = 0;
    std::string body;
    if (!httpJson("POST", g_base + "/api/auth/device/init", "", "application/json",
                  initBody.data(), initBody.size(), code, body, err))
        return fail("pair-init", err);
    if (code != 200 && code != 201) return fail("pair-init", "HTTP " + std::to_string(code) + " " + body);
    mini::Object initObj;
    if (!mini::parse(body, initObj)) return fail("pair-init", "unparsable init body");
    const std::string deviceCode = findStr(initObj, "device_code");
    const std::string userCode = findStr(initObj, "user_code");
    if (deviceCode.empty() || userCode.empty())
        return fail("pair-init", "missing device_code/user_code");
    std::printf("pair init ok: user_code=%s\n", userCode.c_str());

    {
        std::string approveBody =
            "{\"user_code\":\"" + esc(userCode) + "\",\"approved_scopes\":["
            "\"me.read\",\"platforms.read\",\"roms.read\",\"collections.read\","
            "\"firmware.read\",\"assets.read\",\"assets.write\",\"devices.read\","
            "\"devices.write\"],\"device_name\":\"SwitchRomM Live Test\"}";
        if (!httpJson("POST", g_base + "/api/auth/device/approve", basic,
                      "application/json", approveBody.data(), approveBody.size(),
                      code, body, err))
            return fail("pair-approve", err);
        if (code != 200 && code != 201) return fail("pair-approve", "HTTP " + std::to_string(code) + " " + body);
    }
    std::printf("pair approve ok\n");

    std::string token;
    std::string deviceId;
    for (int i = 0; i < 10 && token.empty(); ++i) {
        std::string pollBody = "{\"device_code\":\"" + esc(deviceCode) + "\"}";
        if (!httpJson("POST", g_base + "/api/auth/device/token", "",
                      "application/json", pollBody.data(), pollBody.size(),
                      code, body, err))
            return fail("pair-token", err);
        if (code == 200) {
            mini::Object tok;
            if (!mini::parse(body, tok)) return fail("pair-token", "unparsable token body");
            token = findStr(tok, "access_token");
            deviceId = findStr(tok, "device_id");
        } else if (code == 400) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        } else {
            return fail("pair-token", "HTTP " + std::to_string(code) + " " + body);
        }
    }
    if (token.empty()) return fail("pair-token", "no token after polling");
    std::printf("device token ok: device_id=%s\n", deviceId.c_str());

    const SyncAuthCtx ctx{g_base, token, "", 30}; // raw token; appendAuthHeader adds the scheme

    // ---- 2. Read sanity: /users/me, /platforms, /roms items envelope ----
    if (!httpJson("GET", g_base + "/api/users/me", "Bearer " + token, "", nullptr, 0,
                  code, body, err))
        return fail("read-me", err);
    if (code != 200) return fail("read-me", "HTTP " + std::to_string(code));
    if (!httpJson("GET", g_base + "/api/platforms?limit=5", "Bearer " + token, "", nullptr, 0,
                  code, body, err))
        return fail("read-platforms", err);
    if (code != 200) return fail("read-platforms", "HTTP " + std::to_string(code));
    long long platformId = 0;
    {
        mini::Object obj;
        mini::Array arr;
        const mini::Array* list = nullptr;
        if (mini::parse(body, obj)) {
            auto it = obj.find("items");
            if (it != obj.end() && it->second.type == mini::Value::Type::Array)
                list = &it->second.array;
        } else if (mini::parse(body, arr)) {
            list = &arr;
        }
        if (list && !list->empty()) platformId = findInt((*list)[0].object, "id");
    }
    if (platformId <= 0) return fail("read-platforms", "no platform id parsed");
    if (!httpJson("GET", g_base + "/api/roms?platform_ids=" + std::to_string(platformId) +
                             "&limit=5",
                  "Bearer " + token, "", nullptr, 0, code, body, err))
        return fail("read-roms", err);
    if (code != 200) return fail("read-roms", "HTTP " + std::to_string(code));
    long long romId = 0;
    std::string romFsName;
    {
        mini::Object obj;
        mini::Array arr;
        const mini::Array* list = nullptr;
        if (mini::parse(body, obj)) {
            auto it = obj.find("items");
            if (it != obj.end() && it->second.type == mini::Value::Type::Array)
                list = &it->second.array;
        } else if (mini::parse(body, arr)) {
            list = &arr;
        }
        if (list && !list->empty()) {
            romId = findInt((*list)[0].object, "id");
            romFsName = findStr((*list)[0].object, "fs_name");
        }
    }
    if (romId <= 0) return fail("read-roms", "no rom id parsed");
    std::printf("read sanity ok: platform=%lld rom=%lld (%s)\n", platformId, romId,
                romFsName.c_str());

    // ---- 3. Upload save + state (upload leg) ----
    const std::string saveBytes = "live-test-save-payload-20260829";
    const std::string stateBytes = "live-test-state-payload-20260829";
    const std::string savePath = "/tmp/romm_live_save.srm";
    const std::string statePath = "/tmp/romm_live_state.state";
    {
        FILE* f = fopen(savePath.c_str(), "wb");
        if (!f) return fail("prep", "cannot write " + savePath);
        fwrite(saveBytes.data(), 1, saveBytes.size(), f);
        fclose(f);
        f = fopen(statePath.c_str(), "wb");
        if (!f) return fail("prep", "cannot write " + statePath);
        fwrite(stateBytes.data(), 1, stateBytes.size(), f);
        fclose(f);
    }

    std::string saveErr;
    if (!uploadNewSave(ctx, romId, "autosave", "", deviceId, /*sessionId*/ 0,
                       savePath, /*overwrite*/ false, saveErr))
        return fail("upload-save", saveErr);
    std::printf("save upload ok\n");

    std::string stateErr;
    if (!uploadNewState(ctx, romId, "", statePath, stateErr))
        return fail("upload-state", stateErr);
    std::printf("state upload ok\n");

    // ---- 4. Negotiate with our save hash -> complete session ----
    LocalAsset la;
    la.romId = romId;
    la.fileName = "live-test.srm";
    la.path = savePath;
    la.sizeBytes = saveBytes.size();
    la.mtimeEpoch = 0;
    la.updatedAtIso = "2026-08-29T18:00:00Z";
    la.isState = false;
    la.slot = "autosave";
    unsigned long long sizeOut = 0;
    long long mtimeOut = 0;
    if (!computeFileMd5AndStat(savePath, sizeOut, mtimeOut, la.contentHash))
        return fail("negotiate", "hash failed");
    std::vector<LocalAsset> locals{la};
    NegotiateResponse neg;
    if (!negotiateSync(ctx, deviceId, locals, neg, err)) return fail("negotiate", err);
    std::printf("negotiate ok: session=%lld up=%d down=%d conflict=%d noop=%d\n",
                neg.sessionId, neg.totalUpload, neg.totalDownload, neg.totalConflict,
                neg.totalNoOp);
    if (neg.sessionId <= 0) return fail("negotiate", "no session id");
    // The save was uploaded BEFORE negotiate, so the server already holds the
    // identical content: the orchestrator must answer no_op (no re-upload).
    if (neg.totalUpload != 0 || neg.totalNoOp < 1) {
        std::fprintf(stderr,
                     "LIVE FAIL: expected no_op after identical upload, got up=%d noop=%d\n",
                     neg.totalUpload, neg.totalNoOp);
        return 1;
    }

    std::string compErr;
    if (!completeSyncSession(ctx, neg.sessionId, 0, 0, compErr))
        return fail("complete", compErr);
    std::printf("session complete ok\n");

    // ---- 5. List saves, download ours (round trip), confirm ----
    std::vector<RemoteAsset> saves;
    std::string listErr;
    if (!fetchRemoteAssets(ctx, "saves", saves, listErr)) return fail("list-saves", listErr);
    long long saveId = 0;
    for (const auto& s : saves) {
        if (s.romId == romId && s.fileName.find("romm_live") != std::string::npos) {
            saveId = s.id;
            break;
        }
    }
    if (saveId <= 0) return fail("list-saves", "our save not found in /api/saves");
    std::printf("save listed ok: id=%lld\n", saveId);

    std::string dlBytes;
    std::string dlErr;
    if (!downloadAssetContent(ctx, "saves", saveId, deviceId, dlBytes, dlErr))
        return fail("download-save", dlErr);
    if (dlBytes != saveBytes)
        return fail("download-save", "content mismatch: got " + std::to_string(dlBytes.size()) +
                                         " bytes");
    std::printf("save download round-trip ok (%zu bytes)\n", dlBytes.size());

    std::string confErr;
    if (!confirmSaveDownloaded(ctx, saveId, deviceId, confErr))
        return fail("confirm", confErr);
    std::printf("confirm-downloaded ok\n");

    // ---- 6. Cleanup: delete save + state + device (non-destructive) ----
    {
        long c2 = 0;
        std::string b2;
        std::string delBody = "{\"saves\":[" + std::to_string(saveId) + "]}";
        if (!httpJson("POST", g_base + "/api/saves/delete", "Bearer " + token,
                      "application/json", delBody.data(), delBody.size(), c2, b2, err) ||
            c2 >= 300)
            std::fprintf(stderr, "LIVE WARN: save delete HTTP %ld %s\n", c2, b2.c_str());
        std::vector<RemoteAsset> states;
        std::string stErr;
        if (fetchRemoteAssets(ctx, "states", states, stErr)) {
            long long stateId = 0;
            for (const auto& s : states) {
                if (s.romId == romId && s.fileName.find("romm_live") != std::string::npos) {
                    stateId = s.id;
                    break;
                }
            }
            if (stateId > 0) {
                std::string b3;
                std::string del3 = "{\"states\":[" + std::to_string(stateId) + "]}";
                if (!httpJson("POST", g_base + "/api/states/delete", "Bearer " + token,
                              "application/json", del3.data(), del3.size(), c2, b3, stErr) ||
                    c2 >= 300)
                    std::fprintf(stderr, "LIVE WARN: state delete HTTP %ld %s\n", c2, b3.c_str());
            }
        }
        if (!httpJson("DELETE", g_base + "/api/devices/" + deviceId, basic, "", nullptr, 0,
                      c2, b2, err) || c2 >= 300)
            std::fprintf(stderr, "LIVE WARN: device delete HTTP %ld %s\n", c2, b2.c_str());
        std::remove(savePath.c_str());
        std::remove(statePath.c_str());
    }

    std::printf("LIVE PASS: pairing, reads, save/state upload, negotiate, complete, "
                "download round-trip, confirm, cleanup\n");
    return 0;
}
