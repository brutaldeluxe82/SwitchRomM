# Config

 Put `.env` at `sdmc:/switch/TicromM/.env`. `config.json` in the same directory is also read; current load order is `.env` then `config.json`, so JSON overrides `.env` on the same key. JSON supports schema migration (`schema_version`), and missing `schema_version` is treated as legacy schema and migrated to current keys.

## Keys (with defaults)
- `SERVER_URL` (required): Base RomM URL. **Supports `http://` and `https://`.** Example: `https://192.168.1.10:8080`.
- `USERNAME`, `PASSWORD`: Basic auth credentials. Leave empty if your server does not require them.
- `API_TOKEN`: Reserved for future token support (unused today).
- `OUTPUT_LAYOUT` (`tico`): `tico` or `retroarch` (case-insensitive). Selects the SD
  folder convention for everything the client writes, plus per-layout behavior:

  | Path / behavior | `tico` | `retroarch` |
  |---|---|---|
  | ROMs root | `sdmc:/tico/roms` | `sdmc:/retroarch/downloads` |
  | BIOS root | `sdmc:/tico/system/<platform>/` | `sdmc:/retroarch/system` (flat) |
  | Battery saves | `sdmc:/tico/saves` | `sdmc:/retroarch/.retroarch/saves` |
  | Save states | `sdmc:/tico/states` | `sdmc:/retroarch/.retroarch/states` |
  | Zip extraction on download | when `EXTRACT_ARCHIVE=true` (default) | no |

- `DOWNLOAD_DIR` (empty): Override the ROMs root from the table above. Empty = derive
  from `OUTPUT_LAYOUT`. Platform subfolders are created automatically; temps under
  `<DOWNLOAD_DIR>/temp/`.
- `BIOS_DIR` (empty): Override the BIOS root from the table above. Empty = derive
  from `OUTPUT_LAYOUT`. Firmware downloads go through the standard download
  queue/worker (PLATFORMS - BIOS index, ZR/ZL to switch, A to enqueue) and land in
  the platform's BIOS dir.
- `HTTP_TIMEOUT_SECONDS` (`30`): HTTP send/recv timeout.
- `FAT32_SAFE` (`true`): If true, split into FAT32/DBI-sized parts (`0xFFFF0000`). If false, keep as a single file (no splitting). Multi-part handling still uses DBI archive bit when enabled.
- `EXTRACT_ARCHIVE` (`true`): If true, unpack `.zip` ROM downloads after fetch and delete the archive (tico layout only; ignored for retroarch). Keep `true` for consoles whose cores take loose ROMs. Set `false` wherever an arcade-class core (FBNeo/MAME) loads the zip romset directly, so the archive must stay packed — this is the `block_extract` contract from `ticohq/tico-fbneo`.
- `LOG_LEVEL` (`info`): `debug|info|warn|error`.
- `SPEED_TEST_URL` (blank): Optional URL to fetch ~40MB (Range) for a quick throughput estimate. If set, runs once at startup; if blank, only in-download speeds are shown.

## `config.json` schema
- `schema_version` (optional, JSON only): Current supported version is `1`.
- If omitted, config is treated as legacy schema (`0`) and migrated in-memory.
- Legacy aliases accepted by migration include env-style uppercase keys (for example `SERVER_URL`) plus older aliases like `platform_id`, `download_path`, `timeout_seconds`, and `fat32_split`.

## Files created by the client
- Downloads: `<DOWNLOAD_DIR>/<Title or fsName>_<id>.<ext>` (single file, ID-suffixed for collisions) or `<DOWNLOAD_DIR>/<Title or fsName>_<id>.<ext>/00 01 ...` (multi-part DBI layout).
- Device token: `sdmc:/switch/TicromM/device_token.json` (written after
  successful device pairing from the DIAGNOSTICS view; delete it to un-pair).
- Temps: `<DOWNLOAD_DIR>/temp/<safe-12>.tmp/*.part` and `manifest.json` (safe to delete after failures/stops).
- Log: `sdmc:/switch/TicromM/log.txt`.
- Queue snapshot: `sdmc:/switch/TicromM/queue_state.json` (pending/running queue items restored on restart; completed items are skipped).
