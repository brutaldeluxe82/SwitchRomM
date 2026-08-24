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

## Implementation status (2026-08-23)

The bridge is implemented in SwitchRomM as of `main`:

- **ROM downloads into tico layout**: `output_layout` config key (`tico` default,
  `retroarch` supported) routes ROMs to `sdmc:/tico/roms/<platform>/` (slug-mapped) or
  `sdmc:/retroarch/.retroarch/roms/`; console `.zip` downloads are extracted on
  arrival, arcade-class romsets stay packed.
- **BIOS**: `/api/firmware` listing + download into `sdmc:/tico/system/<platform>/`
  (`sdmc:/retroarch/.retroarch/system/` under retroarch layout); triggered from the
  PLATFORMS - BIOS index (ZR/ZL cycles ROM/BIOS index, A enqueues the selected
  platform's firmware through the standard download queue/worker).
- **Save/state sync** (saves AND states): device-auth pairing
  (`/api/auth/device/init`, `/api/auth/device/token`, token persisted at
  `sdmc:/switch/romm_switch_client/device_token.json`), then per run:
  scan local saves/states → `/api/sync/negotiate` → multipart uploads
  (`saveFile`/`stateFile`), binary downloads, `/api/saves/{id}/downloaded` confirm,
  `/api/sync/sessions/{id}/complete`. Battery saves report `slot:"autosave"` because
  the server pairs negotiate rows on `(rom_id, slot)`; states are synced client-side
- **UI**: PLATFORMS index cycles ROM/BIOS lists with ZR/ZL; the BIOS index shows
  per-platform firmware counts and A enqueues a firmware download. DIAGNOSTICS
  view — Down = sync saves (pairing first when unpaired, showing the user code
  while awaiting approval; leaving the view cancels an in-flight pair).


## Alignment verdict

**Implemented.** tico's ROM/save/state/BIOS layout matched RetroArch conventions closely
enough that SwitchRomM populates it directly (config-level for download location, a
platform-slug mapping table, and the RomM saves/states + negotiate + device-auth
endpoints). The folder mapping above remains the only contract that matters;
SwitchRomM reads back tico's `saves/` and `states/` trees to drive sync.
