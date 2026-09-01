# SwitchRomM ↔ tico bridge contract

Foundation verified 2026-08-22. SwitchRomM builds on devkitA64 (gcc 16.1.0,
libnx, switch-sdl2, switch-curl). This doc records the folder contract that lets
SwitchRomM act as the RomM download/sync engine and tico as the pure front-end/launcher.

## Workflow (target)

1. User opens **SwitchRomM** (NRO), browses their RomM server library, downloads a game.
2. SwitchRomM writes ROMs + BIOS into tico's folder layout on the SD card.
3. User exits, opens **tico**, sees the game with box art, launches it on a bundled core.
4. (Later iteration) SwitchRomM syncs save files / save states back to the RomM server,
   reading from tico's save/state directories.

Neither app contains the other's code; the shared SD layout is the whole integration.

## tico directory layout (from tico.nro 0.7.9 strings + ticoverse.com/wiki)

Root: `sdmc:/tico/`

| Purpose | Path | Notes |
|---|---|---|
| ROMs | `sdmc:/tico/roms/<platform>/` | One optional subfolder per game (`roms/psx/My Game/game.chd`). No deeper nesting. |
| Save states | `sdmc:/tico/states/<platform>/` | Platform folders (e.g. `states/snes/`). Slots named `<game>.state0`..`state3` (4 slots, 0-indexed; bare digit, no dot). |
| Battery saves | `sdmc:/tico/saves/<platform>/` | Platform folders. `.srm`/sram, PSX memcards. |
| BIOS | `sdmc:/tico/system/<platform>/` | Flat per platform, no subfolders. |
| Cores | `sdmc:/tico/cores/` | Bundled/downloadable by tico itself; not our concern. |
| Config/assets | `sdmc:/tico/config/`, `sdmc:/tico/data/` | tico-managed; don't touch. |

### ROM file constraints (wiki, library-and-roms)
- **No `.zip` / `.7z`** per the wiki — applies to console ROMs (the wiki's format
  list ends at GC/Wii; arcade is undocumented). SwitchRomM extracts console zips on
  download, but keeps arcade-class romsets (`arcade`/`mame`/`fbneo`/`fba` slugs)
  packed: tico's FBNeo build declares libretro `block_extract` + `need_fullpath`
  (`ticohq/tico-fbneo` `src/burner/libretro/libretro.cpp`) and loads the archive
  itself.
- Disc consoles: **`.chd` recommended**, single file per disc; `.cue/.bin`, `.gdi` also OK if all tracks in same folder.
- Multi-disc: same console root folder, identical base filename differing only by disc number. tico scans up to 10 discs.
- ROM may sit directly in the platform root or one subfolder.

### Platform folder map (tico slug → our source)
| tico folder (`roms/`) | Notes |
|---|---|
| `nes` `snes` `gb` `gbc` `gba` | Nintendo handhelds/8-16bit, uncompressed single ROMs |
| `master-system` `game-gear` `genesis` `sega-cd` `saturn` `dc` | Sega; `genesis` = Mega Drive |
| `psx` `psp` | Sony; psx/psp prefer `.chd` |
| `gc` `wii` | GameCube/Wii via Dolphin |
| `3ds` | via Azahar (BIOS `system/3ds/cheats` referenced) |

RomM platform slugs (IGDB-derived) must be **mapped** to these tico folder names;
they do not match 1:1 everywhere (e.g. RomM may use `megadrive` where tico wants `genesis`).
A SlugMap table belongs in SwitchRomM's platform_prefs, defaulting to the table above.

## Implementation status (2026-08-25)

The bridge is implemented in SwitchRomM as of `main`:

- **ROM downloads into tico layout**: `output_layout` config key (`tico` default,
  `retroarch` supported) routes ROMs to `sdmc:/tico/roms/<platform>/` (slug-mapped) or
  `sdmc:/retroarch/.retroarch/roms/`; console `.zip` downloads are extracted on
  arrival, arcade-class romsets stay packed.
- **BIOS**: `/api/firmware` listing + download into `sdmc:/tico/system/<platform>/`
  (`sdmc:/retroarch/.retroarch/system/` under retroarch layout); triggered from the
  PLATFORMS - BIOS index (ZR/ZL cycles ROM/BIOS/SYNC index, A enqueues the selected
  platform's firmware through the standard download queue/worker).
- **Save/state sync** (saves AND states): device-auth pairing
  (`/api/auth/device/init`, `/api/auth/device/token`, token persisted at
  `sdmc:/switch/TicromM/device_token.json`), then per run:
  disk-driven discovery → scan local saves/states → fetch remote saves + states →
  client-side reconcile (`buildSyncPlan`: newest timestamp wins; equal timestamps
  compare content hashes) → multipart uploads (`saveFile`/`stateFile`), binary
  downloads, `/api/saves/{id}/downloaded` confirm. The server negotiate endpoint is
  no longer called. `save_sync_behavior` config picks the policy for two-sided
  files: `ask` (default, surfaces conflicts for manual choice), `newest`,
  `server`, or `client`. Battery saves upload as `slot:"autosave"`; states are
  synced client-side.
- **Disk-driven discovery**: sync no longer requires opening a ROM list first. At
  submit time the job snapshots the browsed ROMs plus all server platforms; at run
  time it walks the tico ROMs root (`scanDiskRoms`, root files and one subfolder
  level, matching tico's layout rule), maps each platform folder back to canonical
  RomM slugs (`ticoFolderToCanonicalSlugs` reverse map; collisions like
  `gba`→{gbc,gba} preserved), and — only for folders with no coverage in the
  snapshot and a matching server platform — fetches that platform's ROM metadata
  page-by-page (`fetchPlatformRomsPageAuthed`, bearer > Basic > anonymous,
  paged on `hasMore`). Enrichment runs before the local scan so every candidate
  ROM id participates in base-name matching. Fetch failures log and degrade
  gracefully; there is no fuzzy name matching anywhere (wrong-id uploads stay
  impossible). Files that look like saves/states but match no ROM are logged
  (`SAVE: unmatched: <name>`) and counted in the summary ("(N unmatched)").
- **UI**: PLATFORMS index cycles ROM/BIOS/SYNC lists with ZR/ZL; BIOS shows
  per-platform firmware counts (A enqueues firmware download); SYNC shows
  per-platform save/state counts (A opens the platform's SYNCROMS file list
  with planned actions; ask-policy conflicts are chosen in-view). The SYNC
  index rows and file list are derived from one grouping pass over the plan,
  so counts can never disagree with the drill-in list. SYNC runs from the SYNC
  views (pairing first when unpaired, showing the user code while awaiting
  approval; leaving the view cancels an in-flight pair). A single periodic
  tick auto-kicks sync while paired; entering the views does not double-kick.
- **Grout-reference scoping** (2026-08-25; `github.com/rommapp/grout` studied as
  the first-party Linux-handheld client): adopted Grout's folder-scoped
  save matching — a battery save directly under `saves/<folder>/` now prefers
  ROMs whose platform maps to that folder (`SlugHintFn` hint from
  `ticoFolderToCanonicalSlugs`, first canonical slug wins collisions like
  `gba`→{gbc,gba}; exact base-name matching unchanged, so no fuzzy risk).
  Hints apply to saves only — states stay globally matched. `scanDiskRoms`
  additionally records each ROM's game subfolder (`DiskRom.subFolder`) for
  future per-game-dir scoping. Rejected Grout pieces: negotiate orchestrator
  (server-side deletion propagation conflicts with our explicit client-side
  plan UX), fuzzy/name-similarity matching (wrong-id upload risk is a hard
  rule), and the incremental ROM cache (our per-job disk-driven enrichment
  covers metadata discovery leaner). Save-extension table verified identical
  to Grout's `.srm .sav .dsv .mcr .mcd .brm .eep .sra .fla .mpk .nv`.

- **Grout-style SYNC UI** (2026-08-25): the SYNCROMS view now lists **games**
  (Grout `synced_games.go` model) instead of raw files — each row shows an
  out-of-sync pip plus the game name and file count, aggregated as the worst
  pip of its files. A drills into a new GAMESAVES view listing only that
  game's save/state rows with per-file pips, `[SAVE]`/`[STATE]` tags,
  planned action and last-modified timestamps; the conflict "Which save
  wins?" dialog lives here. Pips: green = in sync (both sides agree),
  yellow = exists on both sides but differs (conflict or content delta),
  red = exists on only one side. X = sync-all still executes every actionable
  item in the platform scope from either view; B backs out level by level.
  The SYNC index rows and both drill-in levels derive from one grouping pass
  over the plan, so index counts can never disagree with either list.

- **Server-orchestrated save sync** (2026-08-29): server phases are back,
  driven by RomM 5.2's sync orchestrator (verified live against
  romm.ainger.cloud, RomM 5.2.0). Y on any SYNC surface runs a full sync:
  disk scan + MD5 hashing → `POST /api/sync/negotiate` (device-scoped save
  list: rom_id, file_name, slot, emulator, content_hash, updated_at,
  file_size_bytes) → `buildOrchestratorPlan` (pairs uploads by (rom_id, slot)
  — never filename; suppresses uploads whose hash matches the recorded
  save_sync_state.json row; skips downloads for ROMs not on the device) →
  execute uploads (`POST /api/saves`, overwrite only on policy, 409 slot
  conflicts surface via `classifyUploadResponse`) and downloads (backup local
  file into `.backup/` first, atomic temp+rename write, mtime set to the
  server's updated_at, `POST /api/saves/{id}/downloaded` confirm) →
  `POST /api/sync/sessions/{id}/complete`. Applied operations update
  `sdmc:/switch/TicromM/save_sync_state.json` (keyed
  rom_id + file_name with slot/save_id/hash/synced_at) so unchanged saves are
  never re-uploaded. Save **states** remain client-side
  (`decideStateOperation` newest-wins/policy) because the orchestrator tracks
  saves only — Grout behaves the same way. The old local-only discovery UI is
  unchanged underneath; the sync run reuses its scan and re-plans the display.

## Alignment verdict

**Implemented.** tico's ROM/save/state/BIOS layout matched RetroArch conventions closely
enough that SwitchRomM populates it directly (config-level for download location, a
platform-slug mapping table, and the RomM saves/states + device-auth endpoints). The
folder mapping above remains the only contract that matters; SwitchRomM reads back
tico's `saves/` and `states/` trees to drive sync.

Save/state **downloads** from RomM are written to
`{saves|states}/<tico-platform-folder>/<fileName>` — the folder comes from the
ROM's platform slug via the same mapping as ROMs, never from the asset's
emulator/core field. After an executed sync the client re-scans and re-plans so
index pips and counts reflect the post-sync tree.

