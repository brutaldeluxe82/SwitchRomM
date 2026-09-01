#include "catch.hpp"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>
#include "romm/save_sync.hpp"
#include "romm/layout.hpp"

using romm::LocalAsset;
using romm::MultipartPart;
using romm::NegotiateResponse;
using romm::RemoteAsset;
using romm::StateKind;
using romm::SyncOperation;

// ---------- isValidBatterySaveExtension matrix ----------

TEST_CASE("battery save extension matrix") {
    REQUIRE(romm::isValidBatterySaveExtension(".srm"));
    REQUIRE(romm::isValidBatterySaveExtension(".sav"));
    REQUIRE(romm::isValidBatterySaveExtension(".dsv"));
    REQUIRE(romm::isValidBatterySaveExtension(".mcr"));
    REQUIRE(romm::isValidBatterySaveExtension(".mcd"));
    REQUIRE(romm::isValidBatterySaveExtension(".brm"));
    REQUIRE(romm::isValidBatterySaveExtension(".eep"));
    REQUIRE(romm::isValidBatterySaveExtension(".sra"));
    REQUIRE(romm::isValidBatterySaveExtension(".fla"));
    REQUIRE(romm::isValidBatterySaveExtension(".mpk"));
    REQUIRE(romm::isValidBatterySaveExtension(".nv"));
}

TEST_CASE("battery save extension rejects non-save extensions") {
    REQUIRE_FALSE(romm::isValidBatterySaveExtension(".state"));
    REQUIRE_FALSE(romm::isValidBatterySaveExtension(".txt"));
    REQUIRE_FALSE(romm::isValidBatterySaveExtension(".zip"));
    REQUIRE_FALSE(romm::isValidBatterySaveExtension(""));
    REQUIRE_FALSE(romm::isValidBatterySaveExtension(".srmx"));
}

TEST_CASE("battery save extension is case-insensitive") {
    REQUIRE(romm::isValidBatterySaveExtension(".SRM"));
    REQUIRE(romm::isValidBatterySaveExtension(".Sav"));
}

// ---------- classifyStateFileName ----------

TEST_CASE("classifyStateFileName matrix") {
    REQUIRE(romm::classifyStateFileName("game.state") == StateKind::Base);
    REQUIRE(romm::classifyStateFileName("game.state.auto") == StateKind::Auto);
    REQUIRE(romm::classifyStateFileName("game.state.7") == StateKind::Numbered);
    REQUIRE(romm::classifyStateFileName("game.state7") == StateKind::Numbered);
    REQUIRE(romm::classifyStateFileName("game.state0") == StateKind::Numbered);
    REQUIRE(romm::classifyStateFileName("game.state.bak") == StateKind::None);
    REQUIRE(romm::classifyStateFileName("game.srm") == StateKind::None);
    REQUIRE(romm::classifyStateFileName("game") == StateKind::None);
}

TEST_CASE("classifyStateFileName is case-insensitive") {
    REQUIRE(romm::classifyStateFileName("GAME.STATE") == StateKind::Base);
    REQUIRE(romm::classifyStateFileName("Game.State.Auto") == StateKind::Auto);
    REQUIRE(romm::classifyStateFileName("GAME.STATE.3") == StateKind::Numbered);
}

TEST_CASE("classifyStateFileName multi-digit numbered") {
    REQUIRE(romm::classifyStateFileName("game.state.15") == StateKind::Numbered);
    REQUIRE(romm::classifyStateFileName("game.state.0") == StateKind::Numbered);
}

// ---------- saveLookupBase ----------

TEST_CASE("saveLookupBase double-extension strip") {
    REQUIRE(romm::saveLookupBase("Zelda (USA)") == "Zelda (USA)");
    REQUIRE(romm::saveLookupBase("game.gba") == "game");       // strips likely ROM ext
    REQUIRE(romm::saveLookupBase("game.v1)") == "game.v1)");   // not alnum -> not stripped
    REQUIRE(romm::saveLookupBase("game") == "game");           // no dot -> unchanged
    REQUIRE(romm::saveLookupBase("game.sfc") == "game");
    REQUIRE(romm::saveLookupBase("Zelda.sfc") == "Zelda");     // double-extension save base
}

// ---------- formatIso8601Utc ----------

TEST_CASE("formatIso8601Utc known epochs") {
    REQUIRE(romm::formatIso8601Utc(0) == "1970-01-01T00:00:00Z");
    REQUIRE(romm::formatIso8601Utc(1000000000LL) == "2001-09-09T01:46:40Z");
}

// ---------- saveSlotLabel / trimIsoToMinutes (local browser labels) ----------

TEST_CASE("saveSlotLabel matrix over state and battery-save names") {
    REQUIRE(romm::saveSlotLabel("game.state0") == "State 0");
    REQUIRE(romm::saveSlotLabel("Game.State.7") == "State 7");
    REQUIRE(romm::saveSlotLabel("game.state12") == "State 12");
    REQUIRE(romm::saveSlotLabel("game.state.auto") == "Auto");
    REQUIRE(romm::saveSlotLabel("game.state") == "Base");
    REQUIRE(romm::saveSlotLabel("game.srm") == "Save");
    REQUIRE(romm::saveSlotLabel("game.SAV") == "Save");
    REQUIRE(romm::saveSlotLabel("game.dsv") == "Save");
    REQUIRE(romm::saveSlotLabel("game.sfc") == "");
}

TEST_CASE("trimIsoToMinutes trims formatIso8601Utc output to minutes") {
    std::string iso = romm::formatIso8601Utc(1000000000LL);
    REQUIRE(iso.size() >= 16);
    REQUIRE(romm::trimIsoToMinutes(iso) == "2001-09-09 01:46");
    // Shorter/other strings pass through unchanged.
    REQUIRE(romm::trimIsoToMinutes("") == "");
    REQUIRE(romm::trimIsoToMinutes("2025-08-20") == "2025-08-20");
}

// ---------- decideStateOperation decision table ----------

TEST_CASE("decideStateOperation no local + remote -> download") {
    REQUIRE(romm::decideStateOperation(nullptr, "2024-01-01T00:00:00Z", "abc123", "") == "download");
}

TEST_CASE("decideStateOperation no local + no remote -> skip") {
    REQUIRE(romm::decideStateOperation(nullptr, "", "", "") == "skip");
}

TEST_CASE("decideStateOperation local + no remote -> upload") {
    LocalAsset local;
    local.updatedAtIso = "2024-01-01T00:00:00Z";
    REQUIRE(romm::decideStateOperation(&local, "", "", local.contentHash) == "upload");
}

TEST_CASE("decideStateOperation local newer -> upload") {
    LocalAsset local;
    local.updatedAtIso = "2024-02-01T00:00:00Z";
    REQUIRE(romm::decideStateOperation(&local, "2024-01-01T00:00:00Z", "remoteH", local.contentHash) == "upload");
}

TEST_CASE("decideStateOperation remote newer -> download") {
    LocalAsset local;
    local.updatedAtIso = "2024-01-01T00:00:00Z";
    REQUIRE(romm::decideStateOperation(&local, "2024-02-01T00:00:00Z", "remoteH", "localH") == "download");
}

TEST_CASE("decideStateOperation equal + same hash -> no_op") {
    LocalAsset local;
    local.updatedAtIso = "2024-01-01T00:00:00Z";
    REQUIRE(romm::decideStateOperation(&local, "2024-01-01T00:00:00Z", "same", "same") == "no_op");
}

TEST_CASE("decideStateOperation equal + diff hash -> skip") {
    LocalAsset local;
    local.updatedAtIso = "2024-01-01T00:00:00Z";
    REQUIRE(romm::decideStateOperation(&local, "2024-01-01T00:00:00Z", "remoteH", "localH") == "skip");
}


// ---------- SyncPolicy ----------

TEST_CASE("parseSyncPolicy round-trips the four literals") {
    REQUIRE(romm::parseSyncPolicy("ask") == romm::SyncPolicy::AskEveryTime);
    REQUIRE(romm::parseSyncPolicy("newest") == romm::SyncPolicy::NewestWins);
    REQUIRE(romm::parseSyncPolicy("server") == romm::SyncPolicy::ServerWins);
    REQUIRE(romm::parseSyncPolicy("client") == romm::SyncPolicy::ClientWins);
}

TEST_CASE("parseSyncPolicy is case-insensitive and tolerant of spaces") {
    REQUIRE(romm::parseSyncPolicy("NEWEST") == romm::SyncPolicy::NewestWins);
    REQUIRE(romm::parseSyncPolicy("  Server ") == romm::SyncPolicy::ServerWins);
    REQUIRE(romm::parseSyncPolicy("Ask") == romm::SyncPolicy::AskEveryTime);
}

TEST_CASE("parseSyncPolicy garbage -> AskEveryTime") {
    REQUIRE(romm::parseSyncPolicy("") == romm::SyncPolicy::AskEveryTime);
    REQUIRE(romm::parseSyncPolicy("bogus") == romm::SyncPolicy::AskEveryTime);
}

TEST_CASE("syncPolicyName round-trips parseSyncPolicy") {
    for (const char* name : {"ask", "newest", "server", "client"}) {
        REQUIRE(std::string(romm::syncPolicyName(romm::parseSyncPolicy(name))) == name);
    }
}

// ---------- buildSyncPlan ----------

namespace {

LocalAsset makeLocal(long long romId, const std::string& fileName,
                     const std::string& updatedAtIso, const std::string& hash,
                     bool isState = false) {
    LocalAsset a;
    a.romId = (int)romId;
    a.fileName = fileName;
    a.updatedAtIso = updatedAtIso;
    a.contentHash = hash;
    a.isState = isState;
    return a;
}

RemoteAsset makeRemote(long long id, long long romId, const std::string& fileName,
                       const std::string& updatedAt, const std::string& hash = "") {
    RemoteAsset r;
    r.id = id;
    r.romId = romId;
    r.fileName = fileName;
    r.updatedAt = updatedAt;
    r.contentHash = hash;
    return r;
}

} // namespace

TEST_CASE("buildSyncPlan local-only -> upload baseline") {
    std::vector<LocalAsset> locals{makeLocal(10, "A.srm", "2024-01-01T00:00:00Z", "h1")};
    auto plan = romm::buildSyncPlan(locals, {}, {}, {});
    REQUIRE(plan.size() == 1);
    REQUIRE(plan[0].local == &locals[0]);
    REQUIRE_FALSE(plan[0].hasRemote);
    REQUIRE_FALSE(plan[0].isState);
    REQUIRE(plan[0].newestAction == romm::SyncPlanAction::Upload);
    REQUIRE_FALSE(plan[0].conflict);
}

TEST_CASE("buildSyncPlan remote-only with known romId -> download; unknown romId omitted") {
    std::vector<RemoteAsset> saves{
        makeRemote(1, 10, "B.srm", "2024-01-01T00:00:00Z"),
        makeRemote(2, 99, "Ghost.srm", "2024-01-02T00:00:00Z"),
    };
    auto plan = romm::buildSyncPlan({}, saves, {}, {});
    // rom 99 not in knownRomIds {10, 11} -> dropped; two downloads remain
    // (remote save for rom 10 + remote state for rom 11).
    auto state = makeRemote(3, 11, "C.state", "2024-01-03T00:00:00Z");
    plan = romm::buildSyncPlan({}, saves, std::vector<RemoteAsset>{state}, {10, 11});
    REQUIRE(plan.size() == 2);
    for (const auto& it : plan) {
        REQUIRE(it.local == nullptr);
        REQUIRE(it.hasRemote);
        REQUIRE(it.newestAction == romm::SyncPlanAction::Download);
    }
    // State classification from the file name extension.
    REQUIRE(plan[0].isState == false);
    REQUIRE(plan[1].isState == true);
    REQUIRE(plan[0].remote.id == 1);
    REQUIRE(plan[1].remote.id == 3);
}

TEST_CASE("buildSyncPlan pairs on (romId, fileName)") {
    std::vector<LocalAsset> locals{
        makeLocal(10, "A.srm", "2024-02-01T00:00:00Z", "h1"),
        makeLocal(20, "A.srm", "2024-01-01T00:00:00Z", "h2"), // same name, other rom
    };
    std::vector<RemoteAsset> saves{makeRemote(5, 20, "A.srm", "2024-03-01T00:00:00Z", "hr")};
    auto plan = romm::buildSyncPlan(locals, saves, {}, {20});
    REQUIRE(plan.size() == 2);
    REQUIRE(plan[0].hasRemote == false); // rom 10 untouched by rom 20's save
    REQUIRE(plan[1].hasRemote == true);
    REQUIRE(plan[1].newestAction == romm::SyncPlanAction::Download); // remote newer
}

TEST_CASE("buildSyncPlan timestamp matrix") {
    const std::string t1 = "2024-01-01T00:00:00Z";
    const std::string t2 = "2024-02-01T00:00:00Z";
    struct Row {
        const char* localTs;
        const char* remoteTs;
        const char* remoteHash;
        romm::SyncPlanAction expected;
        bool conflict;
    };
    const Row rows[] = {
        // equal ts + same hash -> no_op
        {t1.c_str(), t1.c_str(), "sameH", romm::SyncPlanAction::NoOp, false},
        // equal ts + different hash -> no_op baseline but flagged as conflict
        {t1.c_str(), t1.c_str(), "otherH", romm::SyncPlanAction::NoOp, true},
        {t2.c_str(), t1.c_str(), "x", romm::SyncPlanAction::Upload, false},
        {t1.c_str(), t2.c_str(), "x", romm::SyncPlanAction::Download, false},
    };
    for (const auto& row : rows) {
        std::vector<LocalAsset> locals{makeLocal(10, "A.srm", row.localTs, "sameH")};
        std::vector<RemoteAsset> saves{
            makeRemote(1, 10, "A.srm", row.remoteTs, row.remoteHash)};
        auto plan = romm::buildSyncPlan(locals, saves, {}, {});
        REQUIRE(plan.size() == 1);
        INFO("local=" << row.localTs << " remote=" << row.remoteTs);
        REQUIRE(plan[0].newestAction == row.expected);
        REQUIRE(plan[0].conflict == row.conflict);
    }
}

TEST_CASE("buildSyncPlan unmatched locals and zero-rom remotes are skipped") {
    std::vector<LocalAsset> locals{makeLocal(0, "Orphan.srm", "2024-01-01T00:00:00Z", "h")};
    std::vector<RemoteAsset> saves{makeRemote(1, 0, "Bad.srm", "2024-01-01T00:00:00Z")};
    auto plan = romm::buildSyncPlan(locals, saves, {}, {});
    REQUIRE(plan.empty());
}

// ---------- resolveSyncAction / actionable / needsChoice ----------

TEST_CASE("resolveSyncAction server-wins downloads when remote present, never deletes local") {
    romm::SyncPlanItem bothSides;
    LocalAsset local = makeLocal(10, "A.srm", "2024-02-01T00:00:00Z", "h"); // local newer
    bothSides.local = &local;
    bothSides.hasRemote = true;
    bothSides.remote = makeRemote(1, 10, "A.srm", "2024-01-01T00:00:00Z");
    bothSides.newestAction = romm::SyncPlanAction::Upload;
    REQUIRE(romm::resolveSyncAction(bothSides, romm::SyncPolicy::ServerWins) ==
            romm::SyncPlanAction::Download);

    romm::SyncPlanItem localOnly;
    localOnly.local = &local;
    localOnly.newestAction = romm::SyncPlanAction::Upload;
    REQUIRE(romm::resolveSyncAction(localOnly, romm::SyncPolicy::ServerWins) ==
            romm::SyncPlanAction::NoOp);
}

TEST_CASE("resolveSyncAction client-wins uploads when local present") {
    romm::SyncPlanItem item;
    item.hasRemote = true;
    item.remote = makeRemote(1, 10, "A.srm", "2024-03-01T00:00:00Z");
    item.newestAction = romm::SyncPlanAction::Download; // remote newer
    REQUIRE(romm::resolveSyncAction(item, romm::SyncPolicy::ClientWins) ==
            romm::SyncPlanAction::NoOp); // no local -> nothing to upload

    LocalAsset local = makeLocal(10, "A.srm", "2024-01-01T00:00:00Z", "h");
    item.local = &local;
    REQUIRE(romm::resolveSyncAction(item, romm::SyncPolicy::ClientWins) ==
            romm::SyncPlanAction::Upload);
}

TEST_CASE("resolveSyncAction newest-wins takes the baseline; conflicts stay NoOp") {
    romm::SyncPlanItem item;
    item.hasRemote = true;
    item.remote = makeRemote(1, 10, "A.srm", "2024-03-01T00:00:00Z");
    item.newestAction = romm::SyncPlanAction::Download;
    REQUIRE(romm::resolveSyncAction(item, romm::SyncPolicy::NewestWins) ==
            romm::SyncPlanAction::Download);

    item.newestAction = romm::SyncPlanAction::NoOp;
    item.conflict = true;
    REQUIRE(romm::resolveSyncAction(item, romm::SyncPolicy::NewestWins) ==
            romm::SyncPlanAction::NoOp);
    REQUIRE(romm::syncPlanItemActionable(item)); // conflict still surfaced
}

TEST_CASE("resolveSyncAction ask auto-resolves single-sided and holds two-sided") {
    romm::SyncPlanItem localOnly;
    LocalAsset local = makeLocal(10, "A.srm", "2024-01-01T00:00:00Z", "h");
    localOnly.local = &local;
    localOnly.newestAction = romm::SyncPlanAction::Upload;
    REQUIRE(romm::resolveSyncAction(localOnly, romm::SyncPolicy::AskEveryTime) ==
            romm::SyncPlanAction::Upload);

    romm::SyncPlanItem twoSided = localOnly;
    twoSided.hasRemote = true;
    twoSided.remote = makeRemote(1, 10, "A.srm", "2024-01-01T00:00:00Z", "different");
    twoSided.conflict = true;
    REQUIRE(romm::resolveSyncAction(twoSided, romm::SyncPolicy::AskEveryTime) ==
            romm::SyncPlanAction::NoOp);
    REQUIRE(romm::syncPlanNeedsChoice(twoSided, romm::SyncPolicy::AskEveryTime));
}

TEST_CASE("syncPlanNeedsChoice only fires for ask with differing two-sided items") {
    LocalAsset local = makeLocal(10, "A.srm", "2024-01-01T00:00:00Z", "h");
    romm::SyncPlanItem item;
    item.local = &local;
    item.hasRemote = true;
    item.remote = makeRemote(1, 10, "A.srm", "2024-01-01T00:00:00Z", "h");
    item.newestAction = romm::SyncPlanAction::NoOp; // identical content

    REQUIRE_FALSE(romm::syncPlanNeedsChoice(item, romm::SyncPolicy::AskEveryTime));
    REQUIRE_FALSE(romm::syncPlanNeedsChoice(item, romm::SyncPolicy::NewestWins));

    item.newestAction = romm::SyncPlanAction::Upload; // local newer
    REQUIRE(romm::syncPlanNeedsChoice(item, romm::SyncPolicy::AskEveryTime));
    REQUIRE_FALSE(romm::syncPlanNeedsChoice(item, romm::SyncPolicy::NewestWins));
    REQUIRE_FALSE(romm::syncPlanNeedsChoice(item, romm::SyncPolicy::ServerWins));
    REQUIRE_FALSE(romm::syncPlanNeedsChoice(item, romm::SyncPolicy::ClientWins));

    item.newestAction = romm::SyncPlanAction::NoOp;
    item.conflict = true;
    REQUIRE(romm::syncPlanNeedsChoice(item, romm::SyncPolicy::AskEveryTime));
}

TEST_CASE("syncPlanItemActionable flags baseline actions and conflicts only") {
    romm::SyncPlanItem inSync;
    inSync.newestAction = romm::SyncPlanAction::NoOp;
    REQUIRE_FALSE(romm::syncPlanItemActionable(inSync));

    romm::SyncPlanItem uploadable;
    uploadable.newestAction = romm::SyncPlanAction::Upload;
    REQUIRE(romm::syncPlanItemActionable(uploadable));
}

// ---------- parseSavesArray / parseStatesArray ----------

TEST_CASE("parseSavesArray happy path with null and string slot") {
    const std::string json = R"([
        {"id":1,"rom_id":10,"file_name":"A.srm","file_size_bytes":100,"updated_at":"2024-01-01T00:00:00Z","emulator":"mgba","slot":"autosave"},
        {"id":2,"rom_id":20,"file_name":"B.sav","file_size_bytes":200,"updated_at":"2024-01-02T00:00:00Z","emulator":"","slot":null}
    ])";
    auto v = romm::parseSavesArray(json);
    REQUIRE(v.size() == 2);
    REQUIRE(v[0].id == 1);
    REQUIRE(v[0].romId == 10);
    REQUIRE(v[0].fileName == "A.srm");
    REQUIRE(v[0].fileSizeBytes == 100);
    REQUIRE(v[0].updatedAt == "2024-01-01T00:00:00Z");
    REQUIRE(v[0].emulator == "mgba");
    REQUIRE(v[0].slot == "autosave");
    REQUIRE(v[1].id == 2);
    REQUIRE(v[1].romId == 20);
    REQUIRE(v[1].fileName == "B.sav");
    REQUIRE(v[1].fileSizeBytes == 200);
    REQUIRE(v[1].updatedAt == "2024-01-02T00:00:00Z");
    REQUIRE(v[1].emulator == "");
    REQUIRE(v[1].slot == ""); // null -> ""
}

TEST_CASE("parseSavesArray empty array") {
    REQUIRE(romm::parseSavesArray("[]").empty());
}

TEST_CASE("parseSavesArray garbage -> empty") {
    REQUIRE(romm::parseSavesArray("not json").empty());
}

TEST_CASE("parseStatesArray happy path") {
    const std::string json = R"([
        {"id":5,"rom_id":30,"file_name":"C.state","file_size_bytes":50,"updated_at":"2024-03-01T00:00:00Z","emulator":"snes9x"},
        {"id":6,"rom_id":31,"file_name":"D.state.auto","file_size_bytes":60,"updated_at":"2024-03-02T00:00:00Z","emulator":""}
    ])";
    auto v = romm::parseStatesArray(json);
    REQUIRE(v.size() == 2);
    REQUIRE(v[0].id == 5);
    REQUIRE(v[0].romId == 30);
    REQUIRE(v[0].fileName == "C.state");
    REQUIRE(v[0].fileSizeBytes == 50);
    REQUIRE(v[0].updatedAt == "2024-03-01T00:00:00Z");
    REQUIRE(v[0].emulator == "snes9x");
    REQUIRE(v[1].id == 6);
    REQUIRE(v[1].emulator == "");
}

TEST_CASE("parseStatesArray garbage -> empty") {
    REQUIRE(romm::parseStatesArray("{}").empty());
}

// ---------- serializeNegotiatePayload ----------

TEST_CASE("serializeNegotiatePayload exact shape") {
    LocalAsset a;
    a.romId = 7;
    a.fileName = "Zelda.srm";
    a.slot = "autosave";
    a.emulator = "mgba";
    a.contentHash = "abc123";
    a.updatedAtIso = "2024-01-01T00:00:00Z";
    a.sizeBytes = 42;
    std::vector<LocalAsset> saves;
    saves.push_back(a);
    std::string body = romm::serializeNegotiatePayload("dev-1", saves);
    REQUIRE(body.find("\"device_id\":\"dev-1\"") != std::string::npos);
    REQUIRE(body.find("\"rom_id\":7") != std::string::npos);
    REQUIRE(body.find("\"file_name\":\"Zelda.srm\"") != std::string::npos);
    REQUIRE(body.find("\"slot\":\"autosave\"") != std::string::npos);
    REQUIRE(body.find("\"emulator\":\"mgba\"") != std::string::npos);
    REQUIRE(body.find("\"content_hash\":\"abc123\"") != std::string::npos);
    REQUIRE(body.find("\"updated_at\":\"2024-01-01T00:00:00Z\"") != std::string::npos);
    REQUIRE(body.find("\"file_size_bytes\":42") != std::string::npos);
}

TEST_CASE("serializeNegotiatePayload escapes quotes and nulls") {
    LocalAsset a;
    a.romId = 3;
    a.fileName = "A \"B\".srm";
    a.slot = "autosave";
    a.emulator = "";
    a.contentHash = "";
    std::vector<LocalAsset> saves;
    saves.push_back(a);
    std::string body = romm::serializeNegotiatePayload("dev", saves);
    REQUIRE(body.find("\"file_name\":\"A \\\"B\\\".srm\"") != std::string::npos);
    REQUIRE(body.find("\"emulator\":null") != std::string::npos);
    REQUIRE(body.find("\"content_hash\":null") != std::string::npos);
}

TEST_CASE("serializeNegotiatePayload empty saves") {
    std::string body = romm::serializeNegotiatePayload("dev", {});
    REQUIRE(body.find("\"saves\":[]") != std::string::npos);
}

// ---------- parseNegotiateResponse ----------

TEST_CASE("parseNegotiateResponse happy path incl null save_id/server fields") {
    const std::string json = R"({
        "session_id":99,
        "operations":[
            {"action":"upload","rom_id":1,"save_id":null,"file_name":"A.srm","slot":"autosave","emulator":"mgba","reason":"no local","server_updated_at":null,"server_content_hash":null},
            {"action":"download","rom_id":2,"save_id":55,"file_name":"B.sav","slot":null,"emulator":"","reason":"remote newer","server_updated_at":"2024-01-01T00:00:00Z","server_content_hash":"xyz"}
        ],
        "total_upload":1,"total_download":1,"total_conflict":0,"total_no_op":0
    })";
    NegotiateResponse out;
    REQUIRE(romm::parseNegotiateResponse(json, out));
    REQUIRE(out.sessionId == 99);
    REQUIRE(out.operations.size() == 2);
    REQUIRE(out.totalUpload == 1);
    REQUIRE(out.totalDownload == 1);
    REQUIRE(out.totalConflict == 0);
    REQUIRE(out.totalNoOp == 0);

    const SyncOperation& u = out.operations[0];
    REQUIRE(u.action == "upload");
    REQUIRE(u.romId == 1);
    REQUIRE_FALSE(u.hasAssetId);
    REQUIRE(u.fileName == "A.srm");
    REQUIRE(u.slot == "autosave");
    REQUIRE(u.emulator == "mgba");
    REQUIRE(u.reason == "no local");

    const SyncOperation& d = out.operations[1];
    REQUIRE(d.action == "download");
    REQUIRE(d.romId == 2);
    REQUIRE(d.hasAssetId);
    REQUIRE(d.assetId == 55);
    REQUIRE(d.slot == "");
    REQUIRE(d.serverUpdatedAt == "2024-01-01T00:00:00Z");
    REQUIRE(d.serverContentHash == "xyz");
}

TEST_CASE("parseNegotiateResponse garbage -> false") {
    NegotiateResponse out;
    REQUIRE_FALSE(romm::parseNegotiateResponse("not json", out));
}

TEST_CASE("parseNegotiateResponse empty operations") {
    const std::string json = R"({"session_id":1,"operations":[],"total_upload":0,"total_download":0,"total_conflict":0,"total_no_op":0})";
    NegotiateResponse out;
    REQUIRE(romm::parseNegotiateResponse(json, out));
    REQUIRE(out.sessionId == 1);
    REQUIRE(out.operations.empty());
}

// ---------- serializeSyncCompleteBody ----------

TEST_CASE("serializeSyncCompleteBody shape") {
    std::string body = romm::serializeSyncCompleteBody(10, 2);
    REQUIRE(body.find("\"operations_completed\":10") != std::string::npos);
    REQUIRE(body.find("\"operations_failed\":2") != std::string::npos);
}

// ---------- buildMultipartBody ----------

TEST_CASE("buildMultipartBody single part exact literal") {
    std::vector<MultipartPart> parts;
    MultipartPart p;
    p.name = "saveFile";
    p.fileName = "Zelda.srm";
    p.contentType = "application/octet-stream";
    p.data = "AB";
    parts.push_back(p);

    std::string body = romm::buildMultipartBody(parts, "XxBOUNDARYxX");
    // Independent hand-written literal (CRLF framed).
    const std::string expected =
        "--XxBOUNDARYxX\r\n"
        "Content-Disposition: form-data; name=\"saveFile\"; filename=\"Zelda.srm\"\r\n"
        "Content-Type: application/octet-stream\r\n"
        "\r\n"
        "AB\r\n"
        "--XxBOUNDARYxX--\r\n";
    REQUIRE(body == expected);
}

TEST_CASE("buildMultipartBody two parts ordering") {
    std::vector<MultipartPart> parts;
    MultipartPart p1;
    p1.name = "stateFile";
    p1.fileName = "G.state";
    p1.contentType = "application/octet-stream";
    p1.data = "x";
    parts.push_back(p1);
    MultipartPart p2;
    p2.name = "extra";
    p2.fileName = "";
    p2.contentType = "text/plain";
    p2.data = "hello";
    parts.push_back(p2);

    std::string body = romm::buildMultipartBody(parts, "B1");
    REQUIRE(body.find("--B1\r\n") == 0);
    // first part then second part then close
    size_t first = body.find("name=\"stateFile\"");
    size_t second = body.find("name=\"extra\"");
    size_t close = body.find("--B1--\r\n");
    REQUIRE(first != std::string::npos);
    REQUIRE(second != std::string::npos);
    REQUIRE(close != std::string::npos);
    REQUIRE(body.find("\"hello\"") == std::string::npos); // data is raw, not quoted
    REQUIRE(first < second);
    REQUIRE(second < close);
}

// ---------- computeFileMd5AndStat ----------

TEST_CASE("computeFileMd5AndStat matches md5Hex on RFC vector") {
    const char* path = "/tmp/ss_test_md5_abc.bin";
    {
        std::ofstream of(path, std::ios::binary);
        of.write("abc", 3);
    }
    unsigned long long size = 0;
    long long mtime = 0;
    std::string hash;
    REQUIRE(romm::computeFileMd5AndStat(path, size, mtime, hash));
    REQUIRE(size == 3);
    REQUIRE(hash == "900150983cd24fb0d6963f7d28e17f72"); // md5("abc")
    std::remove(path);
}

// ---------- scanAssets integration (host FS) ----------

namespace {

void wipeDir(const std::string& dir) {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

void writeFile(const std::string& path, const char* content) {
    std::filesystem::path pp(path);
    std::error_code ec;
    std::filesystem::create_directories(pp.parent_path(), ec);
    std::ofstream of(path, std::ios::binary);
    of.write(content, std::strlen(content));
}

// Matcher: known save basenames map to fixed rom ids; everything else -> 0.
int knownRomMatcher(const std::string& baseLower, const std::string& /*slugHint*/) {
    if (baseLower == "zelda (usa)") return 10;
    if (baseLower == "mario") return 20;
    return 0;
}

std::string emulatorMatching(const std::string& path) {
    if (path.find("/cores/") != std::string::npos) return "mgba";
    return "";
}

} // namespace

TEST_CASE("scanAssets integration matched/unmatched/isState/emulator/hash") {
    const std::string root = "/tmp/ss_test";
    const std::string savesRoot = root + "/saves";
    const std::string statesRoot = root + "/states";
    wipeDir(root);

    // Battery save in saves root (flat) -> emulator "".
    writeFile(savesRoot + "/Zelda (USA).srm", "zelda-save");
    // Nested battery save under a core dir -> emulator mgba.
    writeFile(savesRoot + "/cores/gba/Mario.sav", "mario-save");
    // A valid save with no matching ROM -> unmatched.
    writeFile(savesRoot + "/Unknown Game.dsv", "unk");
    // A non-save file -> ignored entirely.
    writeFile(savesRoot + "/readme.txt", "notes");

    // Save state under platform dir.
    writeFile(statesRoot + "/platformA/Mario.state", "state1");
    writeFile(statesRoot + "/platformA/Mario.state.auto", "state2");

    std::string err;
    auto result = romm::scanAssets(savesRoot, statesRoot, knownRomMatcher, emulatorMatching, romm::SlugHintFn{}, err);

    REQUIRE(result.assets.size() == 5);
    std::string unmatchedJoined;
    for (const auto& u : result.unmatched) unmatchedJoined += u + "|";
    REQUIRE(unmatchedJoined.find("Unknown Game.dsv") != std::string::npos);

    bool foundZelda = false, foundMario = false, foundState = false, foundAuto = false, foundUnknown = false;
    for (const auto& a : result.assets) {
        if (a.fileName == "Zelda (USA).srm") {
            foundZelda = true;
            REQUIRE(a.romId == 10);
            REQUIRE_FALSE(a.isState);
            REQUIRE(a.slot == "autosave");
            REQUIRE(a.emulator == "");
            REQUIRE(a.contentHash == "0b6e8f040d98438cb6b3834ac493ef16"); // md5("zelda-save")
            REQUIRE(a.updatedAtIso.size() == 20);
        } else if (a.fileName == "Mario.sav") {
            foundMario = true;
            REQUIRE(a.romId == 20);
            REQUIRE_FALSE(a.isState);
            REQUIRE(a.emulator == "mgba"); // nested under /cores/gba/
        } else if (a.fileName == "Mario.state") {
            foundState = true;
            REQUIRE(a.romId == 20);
            REQUIRE(a.isState);
            REQUIRE(a.slot == "");
        } else if (a.fileName == "Mario.state.auto") {
            foundAuto = true;
            REQUIRE(a.isState);
        } else if (a.fileName == "Unknown Game.dsv") {
            foundUnknown = true;
            REQUIRE(a.romId == 0);
        }
    }
    REQUIRE(foundZelda);
    REQUIRE(foundMario);
    REQUIRE(foundState);
    REQUIRE(foundAuto);
    REQUIRE(foundUnknown);
    // readme.txt must not appear.
    for (const auto& a : result.assets) REQUIRE(a.fileName != "readme.txt");

    wipeDir(root);
}

TEST_CASE("scanAssets missing roots -> empty") {
    const std::string root = "/tmp/ss_test_missing";
    wipeDir(root);
    std::string err;
    auto result = romm::scanAssets(root + "/nope-saves", root + "/nope-states",
                                   knownRomMatcher, emulatorMatching, romm::SlugHintFn{}, err);
    REQUIRE(result.assets.empty());
    REQUIRE(result.unmatched.empty());
}

TEST_CASE("scanAssets skips directories") {
    const std::string root = "/tmp/ss_test_skipdir";
    const std::string savesRoot = root + "/saves";
    const std::string statesRoot = root + "/states";
    wipeDir(root);

    writeFile(savesRoot + "/subdir/nested.srm", "nested");
    writeFile(savesRoot + "/top.srm", "top");

    // emulatorOf returns "" everywhere; a subdir must not be treated as an asset.
    std::string err;
    auto result = romm::scanAssets(savesRoot, statesRoot, knownRomMatcher, emulatorMatching, romm::SlugHintFn{}, err);
    bool sawTop = false, sawNested = false;
    for (const auto& a : result.assets) {
        if (a.fileName == "top.srm") sawTop = true;
        if (a.fileName == "nested.srm") sawNested = true;
    }
    REQUIRE(sawTop);
    REQUIRE(sawNested); // nested file is still scanned (recursive), but as a file not a dir
    // Directory "subdir" is not itself an asset; no asset should have a name equal to a dir.
    for (const auto& a : result.assets) {
        REQUIRE(a.fileName.find('/') == std::string::npos);
    }
    wipeDir(root);
}

// ---------- ticoFolderToCanonicalSlugs (reverse folder map) ----------

TEST_CASE("ticoFolderToCanonicalSlugs maps single-canonical folders") {
    // nes now maps {nes, fds} (wiki: FDS lives in the NES folder) — checked
    // in the collisions case below.
    REQUIRE(romm::ticoFolderToCanonicalSlugs("nes").size() == 2);
    REQUIRE(romm::ticoFolderToCanonicalSlugs("snes") == std::vector<std::string>{"snes"});
    // tico's dashed folders map back to RomM's compact canonical slugs.
    REQUIRE(romm::ticoFolderToCanonicalSlugs("sega-cd") == std::vector<std::string>{"segacd"});
    REQUIRE(romm::ticoFolderToCanonicalSlugs("dc") == std::vector<std::string>{"dreamcast"});
    REQUIRE(romm::ticoFolderToCanonicalSlugs("master-system") == std::vector<std::string>{"sms"});
}

TEST_CASE("ticoFolderToCanonicalSlugs preserves collisions in table order") {
    // pc-engine maps to both pcengine and tg16 canonical slugs (table order).
    auto pcengine = romm::ticoFolderToCanonicalSlugs("pc-engine");
    REQUIRE(pcengine.size() == 2);
    REQUIRE(pcengine[0] == "pcengine");
    REQUIRE(pcengine[1] == "tg16");

    auto n64 = romm::ticoFolderToCanonicalSlugs("n64");
    REQUIRE(n64.size() == 2);
    REQUIRE(n64[0] == "n64");
    REQUIRE(n64[1] == "n64dd");

    // Wiki defacto: GBC has its own folder; FDS ROMs live under nes/.
    REQUIRE(romm::ticoFolderToCanonicalSlugs("gbc") == std::vector<std::string>{"gbc"});
    REQUIRE(romm::ticoFolderToCanonicalSlugs("gba") == std::vector<std::string>{"gba"});
    REQUIRE(romm::ticoFolderToCanonicalSlugs("nes") ==
            std::vector<std::string>({"nes", "fds"}));
}

TEST_CASE("ticoFolderToCanonicalSlugs is case-insensitive") {
    auto up = romm::ticoFolderToCanonicalSlugs("GBA");
    REQUIRE(up.size() == 1);
    REQUIRE(up[0] == "gba");
    REQUIRE(romm::ticoFolderToCanonicalSlugs("Sega-CD").size() == 1);
}

TEST_CASE("ticoFolderToCanonicalSlugs unknown folder -> empty") {
    REQUIRE(romm::ticoFolderToCanonicalSlugs("not-a-platform").empty());
    REQUIRE(romm::ticoFolderToCanonicalSlugs("").empty());
}

// ---------- scanDiskRoms (host FS) ----------

namespace {
bool diskRomsContain(const std::vector<romm::DiskRom>& v,
                     const std::string& folder, const std::string& base) {
    for (const auto& d : v)
        if (d.platformFolder == folder && d.baseLower == base) return true;
    return false;
}
} // namespace

TEST_CASE("scanDiskRoms finds root files and one subfolder level") {
    const std::string root = "/tmp/ss_test_diskroms";
    wipeDir(root);
    writeFile(root + "/snes/Super Mario World.sfc", "x");
    writeFile(root + "/snes/Zelda/ALttP (USA).smc", "y");
    // Non-ROM extension still recorded; matching is extension-agnostic here.
    writeFile(root + "/gba/pokemon.gba", "z");

    auto roms = romm::scanDiskRoms(root);
    REQUIRE(roms.size() == 3);
    REQUIRE(diskRomsContain(roms, "snes", "super mario world"));
    // saveLookupBase strips one trailing <=5-char alnum token: "(usa)" region tag.
    bool zelda = false, pokemon = false;
    for (const auto& d : roms) {
        if (d.platformFolder == "snes" && d.baseLower.find("alttp") != std::string::npos) zelda = true;
        if (d.platformFolder == "gba" && d.baseLower == "pokemon") pokemon = true;
    }
    REQUIRE(zelda);
    REQUIRE(pokemon);
    wipeDir(root);
}

TEST_CASE("scanDiskRoms missing root and non-dir entries are tolerated") {
    wipeDir("/tmp/ss_test_disk_missing");
    REQUIRE(romm::scanDiskRoms("/tmp/ss_test_disk_missing").empty());

    const std::string root = "/tmp/ss_test_disk_nondir";
    wipeDir(root);
    writeFile(root + "/loose.txt", "junk");      // file at root: not a platform
    writeFile(root + "/nes/game.nes", "rom");    // real platform
    auto roms = romm::scanDiskRoms(root);
    REQUIRE(roms.size() == 1);
    REQUIRE(diskRomsContain(roms, "nes", "game"));
    wipeDir(root);
}

TEST_CASE("scanAssets slug hint scopes battery-save matching (Grout-style)") {
    const std::string root = "/tmp/ss_test_hint";
    const std::string statesRoot = root + "/states";
    const std::string savesRoot = root + "/saves";
    wipeDir(root);
    // Same base name in two folders: only the folder-scoped one may match.
    writeFile(savesRoot + "/nes/Mario.sav", "nes-mario");
    writeFile(savesRoot + "/gba/Mario.sav", "gba-mario");
    // Flat save at saves root: no top-level dir -> no hint -> global match.
    writeFile(savesRoot + "/Flat.sav", "flat");
    // State under a platform dir in the STATES root: hints never apply to states.
    writeFile(statesRoot + "/nes/Mario.state.0", "state");

    romm::SlugHintFn hints = [](const std::string& path, bool isState) -> std::string {
        size_t sPos = path.find("/saves/");
        if (sPos == std::string::npos || isState) return "";
        const std::string rest = path.substr(sPos + 7);
        const size_t slash = rest.find('/');
        if (slash == std::string::npos) return "";
        return romm::ticoFolderToCanonicalSlugs(rest.substr(0, slash)).front();
    };
    auto matcher = [](const std::string& baseLower, const std::string& slugHint) {
        if (baseLower == "flat") return slugHint.empty() ? 100 : 0; // flat: global only
        if (baseLower != "mario") return 0;
        if (slugHint.empty()) return 100;   // global fallback id
        if (slugHint == "nes") return 10;
        if (slugHint == "gbc") return 20;   // gbc folder now its own slug
        if (slugHint == "gba") return 30;   // gba folder maps to gba
        return 0;
    };
    auto noopEmu = [](const std::string&) { return std::string(); };
    std::string err;
    romm::ScanResult scan = romm::scanAssets(savesRoot, statesRoot,
                                             matcher, noopEmu, hints, err);
    REQUIRE(err.empty());
    REQUIRE(scan.assets.size() == 4);
    int nesMario = 0, gbaMario = 0, flat = 0;
    for (const auto& a : scan.assets) {
        if (a.path.find("/nes/Mario.sav") != std::string::npos)
            nesMario = a.romId;
        else if (a.path.find("/gba/Mario.sav") != std::string::npos)
            gbaMario = a.romId;
        else if (a.fileName == "Flat.sav")
            flat = a.romId;
    }
    REQUIRE(nesMario == 10);
    REQUIRE(gbaMario == 30);   // gba folder now maps solely to gba
    REQUIRE(flat == 100);      // no hint -> global match
    wipeDir(root);
}

TEST_CASE("scanDiskRoms records game subfolder per ROM") {
    const std::string root = "/tmp/ss_test_disk_sub";
    wipeDir(root);
    writeFile(root + "/snes/Super Mario World.sfc", "x");     // root file
    writeFile(root + "/snes/Zelda/ALttP (USA).smc", "y");     // subfolder file

    auto roms = romm::scanDiskRoms(root);
    REQUIRE(roms.size() == 2);
    bool sawRoot = false, sawSub = false;
    for (const auto& d : roms) {
        if (d.platformFolder == "snes" && d.baseLower == "super mario world" &&
            d.subFolder.empty())
            sawRoot = true;
        if (d.platformFolder == "snes" && d.baseLower.find("alttp") == 0 &&
            d.subFolder == "Zelda")
            sawSub = true;
    }
    REQUIRE(sawRoot);
    REQUIRE(sawSub);
    wipeDir(root);
}
