# Tico ↔ RomM platform contract

How platform slugs flow between RomM (server) and tico (on-Switch frontend), and
how SwitchRomM decides which platforms to show. Read this before touching
`platformSupportedByTico` (layout.cpp), `slugAliases()` / `ticoFolderMap()`
(layout.cpp), or the platform fetch path (services.cpp `ApplyPlatformFilter`).

## The three layers

```
RomM server  ──platform slug──▶  SwitchRomM  ──folder map──▶  sdmc:/tico/roms/<folder>/
(what you scanned)               (filter + map)              (what tico scans)
```

1. **RomM** scans your library folders; each top-level folder name is a RomM
   platform slug (its "Supported Platforms" list, ~400 slugs). The platform
   slug arrives in our client via `/api/platforms` (`Platform.slug`).
2. **SwitchRomM** filters that list (`hide_unsupported_platforms`, default on)
   and maps each slug to a tico ROM folder via `layoutPlatformFolder()`.
3. **tico** scans `sdmc:/tico/roms/<folder>/` for known extensions. A platform
   only works end-to-end if all three agree.

## Authoritative sources (as of 2026-09)

- tico wiki ROM formats: https://ticoverse.com/wiki/library-and-roms/supported-rom-formats
  - Out of date in places — cross-check against the tico UI itself.
- tico emulator cores (defacto truth for "what can run"):
  https://github.com/orgs/ticohq/repositories — one fork per core:
  - `tico-duckstation`, `tico-swanstation` → psx (two PS1 cores)
  - `ico-tresdeesse` (Citra/Azahar lineage) → **3ds only**
  - `tico-melonds` (melonDS) → **nds** (separate core from 3DS!)
  - `tico-mupen64plus` → n64
  - `tico-dolphin` → gc + wii (shared folder pair per wiki)
  - `tico-yabasanshiro` → saturn; `tico-flycast` → dreamcast + naomi + atomiswave
  - `tico-fbneo` → arcade (fbneo); `tico-ppsspp` → psp
  - `tico-snes9x` → snes; `tico-mgba` → gba (+gb/gbc); `tico-gambatte` → gb/gbc;
    `tico-fceumm` → nes (+fds); `tico-genesisplusgx` → genesis (+sms/gamegear/segacd)
- RomM slug truth:
  - Docs table: https://docs.romm.app/platforms/supported-platforms/
  - Canonical slugs: `backend/handler/metadata/base_handler.py`
    `class UniversalPlatformSlug` (rommapp/romm)
  - Icon set: `frontend/assets/platforms/<slug>.ico` — presence ≈ first-class
    platform; absence = version/variant only.

## The Naomi/Atomiswave answer (verified in rommapp/romm @ master)

RomM does **not** have `naomi` as a platform. Arcade hardware is modeled as
**IGDB platform *versions* hanging off the single `arcade` platform**:

- `backend/adapters/services/igdb.py` → `IGDB_PLATFORM_VERSIONS`:
  - `"naomi"`   → id 637, `platform_slug: UPS.ARCADE`, "NAOMI"
  - `"naomi-2"` → id 651, `platform_slug: UPS.ARCADE`, "NAOMI 2"
  - `"atomiswave"` → id 652, `platform_slug: UPS.ARCADE`, "Atomiswave"
- `igdb_handler.get_platform()` resolves version slugs to their parent
  (`platform_slug`), so metadata for a `naomi`/`atomiswave` ROM rolls up to
  Arcade (igdb_id 79-ish family), while keeping the version name.
- ScreenScraper side (`ss_handler.py`): only `UPS.ARCADE` (ss id 75) plus
  `cps1/2/3` exist; no naomi/atomiswave entries. Arcade-family romsets share
  the MAME filename-format path (`ARCADES_SS_IDS`).

But: **`frontend/assets/platforms/atomiswave.ico` + `fbneo.ico` exist** (with
`systematic/` variants) while **no `naomi.ico` exists**. So:

| RomM slug | status |
|---|---|
| `arcade` | first-class platform (docs row, icon, ss id 75) |
| `fbneo`  | icon exists; **no docs row, no UPS enum member** — treat as icon-only variant of arcade; do not rely on it as a scanned folder slug |
| `atomiswave` | icon exists; **no docs row, no UPS enum member**; resolves as IGDB version → Arcade |
| `naomi`, `naomi-2` | **nothing** — no docs row, no icon, no enum member; IGDB version → Arcade only |
| `hikaru`, `stv` | docs rows exist; icons absent |
| `cps1` / `cps2` / `cps3` | docs rows + icons + ss ids (6/7/8) |

**Practical rule for SwitchRomM:** if a RomM scan produces a platform whose
slug is `naomi` or `naomi-2`, it means a folder named `naomi` exists in the
RomM library (RomM will happily adopt unknown folders as unmatched platforms).
That platform will have little/no metadata. tico still runs it fine
(Flycast), and our allowlist accepts it.

## tico ROM folder map (verified against wiki + ticohq)

Folder names under `sdmc:/tico/roms/` (from the wiki page; GBC and FDS fixed
per wiki after older verbal info):

| tico folder | RomM slug(s) | formats (wiki) |
|---|---|---|
| `nes/` | nes, fds | .nes .unf/.unif .fds |
| `snes/` | snes | .sfc .smc .fig .swc .bs .st |
| `gb/` | gb | .gb |
| `gbc/` | gbc | .gbc |
| `gba/` | gba | .gba .sgb |
| `master-system/` | sms | .sms .bin |
| `game-gear/` | gamegear | .gg .bin |
| `genesis/` | genesis (aliases megadrive, md) | .md .gen .bin .smd |
| `sega-cd/` | segacd (aliases scd, mega-cd) | .cue+.bin .iso .chd |
| `saturn/` | saturn | .cue+.bin .iso .chd |
| `dc/` | dreamcast (alias dc) | .chd .cdi .gdi .cue+.bin |
| `psx/` | psx (aliases ps1, playstation) | .chd .cue+.bin .iso |
| `psp/` | psp | .iso .cso .pbp .chd |
| `gc/`, `wii/` | ngc, wii (alias gamecube) | .iso .gcm .chd .rvz .wbfs .wia .m3u |
| `n64/` | n64 | (not on wiki; core = tico-mupen64plus) |
| `3ds/` | 3ds | (not on wiki; core = tico-tresdeesse) |
| `nds/` | nds | (not on wiki; core = tico-melonds) |
| `naomi/`, `atomiswave/`, `arcade/` | naomi, atomiswave, arcade | (not on wiki; core = tico-flycast / tico-fbneo) |

Wiki notes that matter for downloads:
- Max nesting: `<folder>/<game or subfolder>/<file>` — one subfolder deep.
- `.zip`/`.7z` archives are NOT supported → keep `extract_archive=true` for
  cart platforms; arcade-class (fbneo/naomi/atomiswave) keep `false`.
- Multi-file formats (cue/bin, gdi) must land in the same folder.

## Current allowlist (`platformSupportedByTico`, layout.cpp)

nes, fds, snes, gb, gbc, gba, sms, gamegear, genesis, segacd, saturn,
dreamcast, psx, psp, gc, wii, atomiswave, naomi, arcade, 3ds, n64, nds.

Slug normalization (`slugAliases()` in layout.cpp) runs first, so variants
like megadrive → genesis, mastersystem → sms, famicom → nes, ps1 → psx,
sega_cd/scd → segacd all pass.

## How to iterate when either side changes

1. **New platform appears in RomM** (user adds a folder or RomM adds a slug):
   it shows up in `/api/platforms`. If tico can run it: add the canonical slug
   to `platformSupportedByTico` (layout.cpp) + a `ticoFolderMap()` entry if the
   folder is new, + tests in `test_layout.cpp` (positive + negative), then
   rebuild/deploy. If tico can't run it: do nothing — the filter already hides
   it.
2. **tico adds a core/folder**: check ticohq for the fork → confirm the folder
   name from the wiki (or tico UI if wiki lags) → add `ticoFolderMap()` entry
   (canonical fs_slug → folder) → add slug to `platformSupportedByTico` →
   update `test_layout.cpp` + reverse-map tests in `test_save_sync_engine.cpp`.
3. **RomM renames/merges slugs** (e.g. naomi folds into arcade properly):
   watch `UniversalPlatformSlug` (base_handler.py) and the docs table. If a
   slug disappears, add the old name to `slugAliases()` so existing libraries
   keep mapping.
4. **Bisecting a "platform vanished" report**: check (a) is `slug` in RomM's
   response at all (`GET /api/platforms`), (b) does `platformSupportedByTico`
   accept it, (c) does `layoutPlatformFolder` return a folder. The filter log
   line `PLATFORMS: hid N platform(s)...` tells you how many were dropped.
5. **Config**: `hide_unsupported_platforms` (default true) in config.json /
   .env; toggle lives in General settings ("Hide Non-Tico Platforms").
   Turning it off shows every RomM platform regardless of tico support.

## Known sharp edges

- `gbc` maps to its own `gbc/` folder (wiki); older info said `gba/`. If a
  user reports GBC games missing from tico, check the download folder.
- `fds` maps into `nes/` (wiki); there is no `fds/` folder in tico.
- naomi/atomiswave/naomi-2/hikaru/stv ROMs are IGDB "versions" of Arcade in
  RomM — expect sparse metadata vs dedicated platforms.
- `fbneo` as a RomM folder slug is possible but undocumented; `arcade` is the
  canonical FBNeo target.
- tico does not support `.zip`/`.7z`; SwitchRomM's extract-on-download is the
  workaround (except arcade-class romsets, which Flycast/FBNeo want as-is).
