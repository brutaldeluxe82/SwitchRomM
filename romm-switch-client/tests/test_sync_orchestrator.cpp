// Tests for the grout-parity sync additions in save_sync:
//   - sync-state persistence (loadSyncState/saveSyncState/upsert/find)
//   - orchestrator plan builder (buildOrchestratorPlan)
//   - upload response classifier (classifyUploadResponse)
//   - device-scoped fetch URL building
#include "catch.hpp"

#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "romm/save_sync.hpp"

using romm::LocalAsset;
using romm::NegotiateResponse;
using romm::SyncOperation;
using romm::SyncStateRow;
using romm::SyncStateStore;

namespace {

void wipeDir(const std::string& dir) {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

SyncOperation makeOp(const std::string& action, long long romId, const std::string& slot,
                     const std::string& fileName) {
    SyncOperation op;
    op.action = action;
    op.romId = romId;
    op.slot = slot;
    op.fileName = fileName;
    return op;
}

LocalAsset makeLocal(int romId, const std::string& slot, const std::string& fileName,
                     const std::string& hash) {
    LocalAsset a;
    a.romId = romId;
    a.slot = slot;
    a.fileName = fileName;
    a.contentHash = hash;
    return a;
}

NegotiateResponse negotiateOf(std::vector<SyncOperation> ops) {
    NegotiateResponse r;
    r.sessionId = 77;
    r.operations = std::move(ops);
    return r;
}

} // namespace

// ---------- SyncStateStore round-trip ----------

TEST_CASE("saveSyncState/loadSyncState round trip preserves rows") {
    const std::string root = "/tmp/ss_orch_state";
    wipeDir(root);
    const std::string path = root + "/nested/save_sync_state.json";

    SyncStateStore store;
    store.deviceId = "dev-42";
    SyncStateRow row;
    row.fileName = "Game.srm";
    row.romId = 11;
    row.slot = "autosave";
    row.saveId = 555;
    row.contentHash = "abc123";
    row.syncedAt = "2025-08-20T14:03:27Z";
    store.rows.push_back(row);

    std::string err;
    REQUIRE(romm::saveSyncState(path, store, err));
    REQUIRE(err.empty());

    SyncStateStore loaded;
    REQUIRE(romm::loadSyncState(path, loaded));
    REQUIRE(loaded.deviceId == "dev-42");
    REQUIRE(loaded.rows.size() == 1);
    REQUIRE(loaded.rows[0].fileName == "Game.srm");
    REQUIRE(loaded.rows[0].romId == 11);
    REQUIRE(loaded.rows[0].slot == "autosave");
    REQUIRE(loaded.rows[0].saveId == 555);
    REQUIRE(loaded.rows[0].contentHash == "abc123");
    REQUIRE(loaded.rows[0].syncedAt == "2025-08-20T14:03:27Z");

    wipeDir(root);
}

TEST_CASE("loadSyncState missing file -> empty store, true") {
    SyncStateStore out;
    out.deviceId = "stale"; // overwritten
    REQUIRE(romm::loadSyncState("/tmp/ss_orch_missing_dir/state.json", out));
    REQUIRE(out.deviceId.empty());
    REQUIRE(out.rows.empty());
}

TEST_CASE("loadSyncState corrupt JSON -> false with empty store") {
    const std::string path = "/tmp/ss_orch_corrupt.json";
    {
        FILE* f = fopen(path.c_str(), "wb");
        REQUIRE(f != nullptr);
        fputs("{\"device_id\": oops", f);
        fclose(f);
    }
    SyncStateStore out;
    REQUIRE_FALSE(romm::loadSyncState(path, out));
    REQUIRE(out.rows.empty());
    REQUIRE(out.deviceId.empty());
    std::remove(path.c_str());
}

TEST_CASE("loadSyncState empty rows array -> empty store, true") {
    const std::string path = "/tmp/ss_orch_emptyrows.json";
    {
        FILE* f = fopen(path.c_str(), "wb");
        REQUIRE(f != nullptr);
        fputs("{\"device_id\":\"d1\",\"rows\":[]}", f);
        fclose(f);
    }
    SyncStateStore out;
    REQUIRE(romm::loadSyncState(path, out));
    REQUIRE(out.deviceId == "d1");
    REQUIRE(out.rows.empty());
    std::remove(path.c_str());
}

TEST_CASE("loadSyncState tolerates null slot/content_hash and drops unusable rows") {
    const std::string path = "/tmp/ss_orch_nulls.json";
    {
        FILE* f = fopen(path.c_str(), "wb");
        REQUIRE(f != nullptr);
        fputs("{\"device_id\":\"d2\",\"rows\":["
              "{\"file_name\":\"a.srm\",\"rom_id\":3,\"slot\":null,\"save_id\":9,"
              "\"content_hash\":null,\"synced_at\":\"t1\"},"
              "{\"file_name\":\"\",\"rom_id\":4,\"slot\":null,\"save_id\":0,"
              "\"content_hash\":null,\"synced_at\":\"\"},"
              "{\"file_name\":\"b.srm\",\"rom_id\":0,\"slot\":null,\"save_id\":0,"
              "\"content_hash\":null,\"synced_at\":\"\"}]}",
              f);
        fclose(f);
    }
    SyncStateStore out;
    REQUIRE(romm::loadSyncState(path, out));
    REQUIRE(out.rows.size() == 1); // rows without file name or rom id are skipped
    REQUIRE(out.rows[0].fileName == "a.srm");
    REQUIRE(out.rows[0].slot.empty());
    REQUIRE(out.rows[0].contentHash.empty());
    std::remove(path.c_str());
}

TEST_CASE("upsertSyncStateRow replaces in place by (romId, lowercased file name)") {
    SyncStateStore store;
    store.deviceId = "d";

    SyncStateRow a;
    a.fileName = "Zelda (USA).srm";
    a.romId = 7;
    a.saveId = 1;
    a.contentHash = "h1";
    upsertSyncStateRow(store, a);
    REQUIRE(store.rows.size() == 1);

    SyncStateRow b;
    b.fileName = "zelda (usa).SRM"; // same key, different case
    b.romId = 7;
    b.saveId = 2;
    b.contentHash = "h2";
    b.syncedAt = "2025-01-01T00:00:00Z";
    romm::upsertSyncStateRow(store, b);
    REQUIRE(store.rows.size() == 1); // replaced, not appended
    REQUIRE(store.rows[0].fileName == "zelda (usa).srm"); // canonical lowercase
    REQUIRE(store.rows[0].saveId == 2);

    SyncStateRow c;
    c.fileName = "mario.srm";
    c.romId = 8; // different rom -> different key even with same file name
    c.saveId = 3;
    romm::upsertSyncStateRow(store, c);
    REQUIRE(store.rows.size() == 2);

    SyncStateRow d;
    d.fileName = "zelda (usa).srm";
    d.romId = 9; // same file name, different rom -> separate row
    romm::upsertSyncStateRow(store, d);
    REQUIRE(store.rows.size() == 3);
}

TEST_CASE("upsertSyncStateRow ignores unusable rows") {
    SyncStateStore store;
    SyncStateRow noName;
    noName.romId = 5;
    romm::upsertSyncStateRow(store, noName);
    SyncStateRow noRom;
    noRom.fileName = "x.srm";
    noRom.romId = 0;
    romm::upsertSyncStateRow(store, noRom);
    REQUIRE(store.rows.empty());
}

TEST_CASE("findSyncStateRow matches case-insensitively and misses others") {
    SyncStateStore store;
    SyncStateRow row;
    row.fileName = "game.state0";
    row.romId = 4;
    row.contentHash = "hh";
    upsertSyncStateRow(store, row);

    const romm::SyncStateRow* found = romm::findSyncStateRow(store, 4, "GAME.STATE0");
    REQUIRE(found != nullptr);
    REQUIRE(found->contentHash == "hh");

    REQUIRE(romm::findSyncStateRow(store, 4, "game.state1") == nullptr);
    REQUIRE(romm::findSyncStateRow(store, 5, "game.state0") == nullptr); // rom must match
    REQUIRE(romm::findSyncStateRow(store, 0, "game.state0") == nullptr);

    SyncStateStore empty;
    REQUIRE(romm::findSyncStateRow(empty, 4, "game.state0") == nullptr);
}

// ---------- buildOrchestratorPlan ----------

TEST_CASE("plan pairs uploads by (romId, slot), never by file name") {
    NegotiateResponse nego = negotiateOf({
        makeOp("upload", 1, "autosave", "Server-Named.srm"),
    });
    std::vector<LocalAsset> locals = {
        makeLocal(1, "autosave", "device-named-different.srm", "hashA"),
    };

    romm::OrchestratorPlan plan = romm::buildOrchestratorPlan(nego, locals, {}, {1});

    REQUIRE(plan.ops.size() == 1);
    REQUIRE(plan.ops[0].local != nullptr);
    REQUIRE(plan.ops[0].local->fileName == "device-named-different.srm");
    REQUIRE(plan.suppressedUploads == 0);
    REQUIRE(plan.skippedDownloads == 0);
}

TEST_CASE("plan pairs by slot within a rom and keeps locals distinct per rom") {
    NegotiateResponse nego = negotiateOf({
        makeOp("upload", 1, "autosave", "a.srm"),
        makeOp("upload", 2, "autosave", "b.srm"),
    });
    std::vector<LocalAsset> locals = {
        makeLocal(1, "autosave", "one.srm", "h1"),
        makeLocal(2, "autosave", "two.srm", "h2"),
    };

    romm::OrchestratorPlan plan = romm::buildOrchestratorPlan(nego, locals, {}, {1, 2});

    REQUIRE(plan.ops.size() == 2);
    REQUIRE(plan.ops[0].local != nullptr);
    REQUIRE(plan.ops[0].local->fileName == "one.srm");
    REQUIRE(plan.ops[1].local != nullptr);
    REQUIRE(plan.ops[1].local->fileName == "two.srm");
}

TEST_CASE("plan download op never pairs to a local") {
    NegotiateResponse nego = negotiateOf({
        makeOp("download", 3, "autosave", "remote.srm"),
    });
    std::vector<LocalAsset> locals = {
        makeLocal(3, "autosave", "local.srm", "h"),
    };

    romm::OrchestratorPlan plan = romm::buildOrchestratorPlan(nego, locals, {}, {3});

    REQUIRE(plan.ops.size() == 1);
    REQUIRE(plan.ops[0].local == nullptr); // downloads fetch remote content
    REQUIRE(plan.ops[0].op.action == "download");
}

TEST_CASE("plan suppresses upload when recorded hash equals local hash") {
    NegotiateResponse nego = negotiateOf({
        makeOp("upload", 1, "autosave", "game.srm"),
    });
    std::vector<LocalAsset> locals = {
        makeLocal(1, "autosave", "game.srm", "same-hash"),
    };
    SyncStateStore state;
    SyncStateRow row;
    row.fileName = "game.srm";
    row.romId = 1;
    row.slot = "autosave";
    row.saveId = 42;
    row.contentHash = "same-hash";
    upsertSyncStateRow(state, row);

    romm::OrchestratorPlan plan = romm::buildOrchestratorPlan(nego, locals, state, {1});

    REQUIRE(plan.ops.empty());
    REQUIRE(plan.suppressedUploads == 1); // content already on server
}

TEST_CASE("plan keeps upload when recorded hash differs (content changed locally)") {
    NegotiateResponse nego = negotiateOf({
        makeOp("upload", 1, "autosave", "game.srm"),
    });
    std::vector<LocalAsset> locals = {
        makeLocal(1, "autosave", "game.srm", "new-hash"),
    };
    SyncStateStore state;
    SyncStateRow row;
    row.fileName = "game.srm";
    row.romId = 1;
    row.contentHash = "old-hash";
    upsertSyncStateRow(state, row);

    romm::OrchestratorPlan plan = romm::buildOrchestratorPlan(nego, locals, state, {1});

    REQUIRE(plan.ops.size() == 1);
    REQUIRE(plan.suppressedUploads == 0);
}

TEST_CASE("plan suppression is keyed to the op's (romId, file name) row only") {
    NegotiateResponse nego = negotiateOf({
        makeOp("upload", 2, "autosave", "other.srm"),
    });
    std::vector<LocalAsset> locals = {
        makeLocal(2, "autosave", "other.srm", "hashX"),
    };
    SyncStateStore state;
    SyncStateRow row;
    row.fileName = "other.srm";
    row.romId = 9; // different rom: not the op's row
    row.contentHash = "hashX";
    upsertSyncStateRow(state, row);

    romm::OrchestratorPlan plan = romm::buildOrchestratorPlan(nego, locals, state, {2});

    REQUIRE(plan.ops.size() == 1);
    REQUIRE(plan.suppressedUploads == 0);
}

TEST_CASE("plan skips downloads for ROMs not present on device") {
    NegotiateResponse nego = negotiateOf({
        makeOp("download", 10, "", "present.state"),
        makeOp("download", 11, "", "absent.state"),
        makeOp("download", 12, "", "also-absent.state"),
    });

    romm::OrchestratorPlan plan =
        romm::buildOrchestratorPlan(nego, {}, {}, {10});

    REQUIRE(plan.ops.size() == 1);
    REQUIRE(plan.ops[0].op.romId == 10);
    REQUIRE(plan.skippedDownloads == 2);
    REQUIRE(plan.suppressedUploads == 0);
}

TEST_CASE("plan keeps conflict ops with paired local (or null) for policy layer") {
    NegotiateResponse nego = negotiateOf({
        makeOp("conflict", 1, "autosave", "paired.srm"),
        makeOp("conflict", 2, "autosave", "unpaired.srm"),
    });
    std::vector<LocalAsset> locals = {
        makeLocal(1, "autosave", "paired.srm", "h"),
    };

    romm::OrchestratorPlan plan = romm::buildOrchestratorPlan(nego, locals, {}, {1, 2});

    REQUIRE(plan.ops.size() == 2); // conflicts pass through even unpaired
    REQUIRE(plan.ops[0].op.action == "conflict");
    REQUIRE(plan.ops[0].local != nullptr);
    REQUIRE(plan.ops[0].local->fileName == "paired.srm");
    REQUIRE(plan.ops[1].op.action == "conflict");
    REQUIRE(plan.ops[1].local == nullptr);
}

TEST_CASE("plan drops no_op ops entirely") {
    NegotiateResponse nego = negotiateOf({
        makeOp("no_op", 1, "autosave", "same.srm"),
        makeOp("no_op", 2, "", "same.state"),
    });

    romm::OrchestratorPlan plan = romm::buildOrchestratorPlan(nego, {}, {}, {1, 2});

    REQUIRE(plan.ops.empty());
    REQUIRE(plan.suppressedUploads == 0);
    REQUIRE(plan.skippedDownloads == 0);
}

TEST_CASE("plan preserves server op order across mixed actions") {
    NegotiateResponse nego = negotiateOf({
        makeOp("download", 1, "", "a.state"),   // kept
        makeOp("upload", 2, "autosave", "b.srm"), // kept
        makeOp("no_op", 3, "", "c.state"),      // dropped
        makeOp("conflict", 4, "", "d.state"),   // kept
        makeOp("download", 5, "", "e.state"),   // skipped (rom absent)
        makeOp("upload", 6, "", "f.state"),     // kept
    });
    std::vector<LocalAsset> locals = {
        makeLocal(2, "autosave", "b.srm", "hb"),
        makeLocal(6, "", "f.state", "hf"),
    };

    romm::OrchestratorPlan plan = romm::buildOrchestratorPlan(nego, locals, {}, {1, 2, 4, 6});

    REQUIRE(plan.ops.size() == 4);
    REQUIRE(plan.ops[0].op.romId == 1);
    REQUIRE(plan.ops[0].op.action == "download");
    REQUIRE(plan.ops[1].op.romId == 2);
    REQUIRE(plan.ops[1].op.action == "upload");
    REQUIRE(plan.ops[2].op.romId == 4);
    REQUIRE(plan.ops[2].op.action == "conflict");
    REQUIRE(plan.ops[3].op.romId == 6);
    REQUIRE(plan.ops[3].op.action == "upload");
    REQUIRE(plan.skippedDownloads == 1);
    REQUIRE(plan.sessionId == 77);
}

TEST_CASE("plan empty negotiate yields empty plan with session id") {
    romm::OrchestratorPlan plan = romm::buildOrchestratorPlan(negotiateOf({}), {}, {}, {});
    REQUIRE(plan.sessionId == 77);
    REQUIRE(plan.ops.empty());
}

TEST_CASE("plan upload without matching local is kept unpaired (execution layer decides)") {
    NegotiateResponse nego = negotiateOf({
        makeOp("upload", 5, "", "unknown.state"),
    });
    std::vector<LocalAsset> locals = {
        makeLocal(5, "autosave", "wrong-slot.srm", "h"),
    };

    romm::OrchestratorPlan plan = romm::buildOrchestratorPlan(nego, locals, {}, {5});

    REQUIRE(plan.ops.size() == 1);
    REQUIRE(plan.ops[0].local == nullptr); // empty op slot != local "autosave"
    REQUIRE(plan.suppressedUploads == 0);  // no local -> no hash to compare
}

// ---------- classifyUploadResponse ----------

TEST_CASE("classifyUploadResponse 201 with id parses save id") {
    romm::UploadOutcome out = romm::classifyUploadResponse(201, R"({"id":123,"file_name":"a.srm"})");
    REQUIRE(out.ok);
    REQUIRE_FALSE(out.slotConflict);
    REQUIRE(out.saveId == 123);
}

TEST_CASE("classifyUploadResponse 200 with id parses save id") {
    romm::UploadOutcome out = romm::classifyUploadResponse(200, R"({"id":77})");
    REQUIRE(out.ok);
    REQUIRE(out.saveId == 77);
}

TEST_CASE("classifyUploadResponse 200 without id still ok, saveId 0") {
    romm::UploadOutcome out = romm::classifyUploadResponse(200, R"({"detail":"ok"})");
    REQUIRE(out.ok);
    REQUIRE(out.saveId == 0);
}

TEST_CASE("classifyUploadResponse 200 with non-JSON body still ok") {
    romm::UploadOutcome out = romm::classifyUploadResponse(200, "not json");
    REQUIRE(out.ok);
    REQUIRE(out.saveId == 0);
}

TEST_CASE("classifyUploadResponse 409 flags slotConflict only") {
    romm::UploadOutcome out = romm::classifyUploadResponse(
        409, R"({"detail":"Slot has a newer save since your last sync"})");
    REQUIRE_FALSE(out.ok);
    REQUIRE(out.slotConflict);
    REQUIRE(out.saveId == 0);
}

TEST_CASE("classifyUploadResponse 500 is neither ok nor conflict") {
    romm::UploadOutcome out = romm::classifyUploadResponse(500, R"({"detail":"boom"})");
    REQUIRE_FALSE(out.ok);
    REQUIRE_FALSE(out.slotConflict);
    REQUIRE(out.saveId == 0);
}

TEST_CASE("classifyUploadResponse 404 is neither ok nor conflict") {
    romm::UploadOutcome out = romm::classifyUploadResponse(404, "");
    REQUIRE_FALSE(out.ok);
    REQUIRE_FALSE(out.slotConflict);
}

// ---------- fetchRemoteAssets URL building (device-scoped filter) ----------

TEST_CASE("fetch URL: no filters -> plain endpoint") {
    REQUIRE(romm::buildFetchAssetsUrlForTest("https://romm.example", "saves", "", "") ==
            "https://romm.example/api/saves");
    REQUIRE(romm::buildFetchAssetsUrlForTest("https://romm.example/", "states", "", "") ==
            "https://romm.example/api/states");
}

TEST_CASE("fetch URL: rom_id only") {
    REQUIRE(romm::buildFetchAssetsUrlForTest("https://romm.example", "saves", "123", "") ==
            "https://romm.example/api/saves?rom_id=123");
}

TEST_CASE("fetch URL: device_id only") {
    REQUIRE(romm::buildFetchAssetsUrlForTest("https://romm.example", "states", "", "dev-9") ==
            "https://romm.example/api/states?device_id=dev-9");
}

TEST_CASE("fetch URL: rom_id before device_id, percent-encoded") {
    REQUIRE(romm::buildFetchAssetsUrlForTest("https://romm.example", "saves", "12", "a b/c") ==
            "https://romm.example/api/saves?rom_id=12&device_id=a%20b%2Fc");
}

// ---------- parseNegotiateResponse feeds the orchestrator (wiring check) ----------

TEST_CASE("negotiate JSON parses into ops the orchestrator can execute") {
    const std::string json = R"({
        "session_id": 5,
        "operations": [
            {"action":"upload","rom_id":1,"save_id":null,"file_name":"A.srm","slot":"autosave","emulator":"mgba","reason":"no local","server_updated_at":null,"server_content_hash":null},
            {"action":"download","rom_id":2,"save_id":55,"file_name":"B.state","slot":null,"emulator":"","reason":"remote newer","server_updated_at":"2024-01-01T00:00:00Z","server_content_hash":"xyz"},
            {"action":"no_op","rom_id":3,"save_id":null,"file_name":"C.srm","slot":"autosave","emulator":"","reason":"identical","server_updated_at":"2024-01-01T00:00:00Z","server_content_hash":"same"}
        ],
        "total_upload":1,"total_download":1,"total_conflict":0,"total_no_op":1
    })";
    NegotiateResponse out;
    REQUIRE(romm::parseNegotiateResponse(json, out));
    REQUIRE(out.sessionId == 5);
    REQUIRE(out.operations.size() == 3);

    romm::OrchestratorPlan plan = romm::buildOrchestratorPlan(out, {}, {}, {2});
    REQUIRE(plan.sessionId == 5);
    REQUIRE(plan.ops.size() == 2); // upload kept unpaired, download kept, no_op dropped
    REQUIRE(plan.ops[0].op.action == "upload");
    REQUIRE(plan.ops[0].local == nullptr);
    REQUIRE(plan.ops[1].op.action == "download");
    REQUIRE(plan.skippedDownloads == 0);
}
