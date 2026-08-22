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
| Save states | `sdmc:/tico/states/` | Per-emulator subfolders. |
| Battery saves | `sdmc:/tico/saves/` | Relative `./saves` in binary; under tico root. `.srm`/sram, PSX memcards. |
| BIOS | `sdmc:/tico/system/<platform>/` | Flat per platform, no subfolders. |
| Cores | `sdmc:/tico/cores/` | Bundled/downloadable by tico itself; not our concern. |
| Config/assets | `sdmc:/tico/config/`, `sdmc:/tico/data/` | tico-managed; don't touch. |

### ROM file constraints (wiki, library-and-roms)
- **No `.zip` / `.7z`** — ROMs must be uncompressed. SwitchRomM must extract on download.
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

## Current state of SwitchRomM (gap analysis for the bridge)

As of `a322359`, SwitchRomM only downloads ROMs:
- Endpoints used: `/api/platforms`, `/api/roms?platform_ids=`, `/api/roms/identifiers` (+ content download). 
- **Not yet used** (needed for full bridge): `/api/firmware` (BIOS into `system/`), `/api/saves` + `/api/states` (sync), multipart upload with `device_id`, `/api/sync/negotiate`.
- Default download dir is `sdmc:/romm_cache` — must become **`sdmc:/tico/roms`** with platform-slug subfolders for the bridge to work. `Filesystem.cpp`/`downloader.cpp` already group by `<downloadDir>/<platformSlug>/<title_id>/`, so this is a config + slug-map + extraction change, not a rewrite.
- `.zip` downloads must be extracted to satisfy tico (tico rejects archives). Downloader already stages zips (RomStaging) — needs an unwind step.
- Auth: currently username/password; RomM 4.7+ device pairing (`/api/auth/device/*`, bearer + `device_id` tag on saves upload) is the path that enables correct per-device save sync.

## Alignment verdict

**Feasible now, no tico cooperation required.** tico's ROM/save/state/BIOS layout is fully
documented and matches RetroArch conventions closely enough that SwitchRomM can populate it.
The integration is config-level for download location, plus a platform-slug mapping table and
zip-extraction on the SwitchRomM side. Save/state sync (iteration 2) needs SwitchRomM to add
the RomM saves/states + negotiate endpoints and read tico's `saves/` and `states/` trees —
the folder mapping above is the only contract that matters.
