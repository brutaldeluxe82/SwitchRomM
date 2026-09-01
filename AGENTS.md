# SwitchRomM

## Platform mapping contract

- Platform ↔ tico folder ↔ RomM slug contract (what runs where, and how to
  iterate when platforms change on either end):
  `romm-switch-client/docs/platform_contract.md`.

## Build & test
- Host tests: `cd romm-switch-client/tests && make -j$(nproc) && ./romm_tests`
  (`run_tests.ps1` is Windows-only; never use it on Linux.)
- Switch build: `cd romm-switch-client && DEVKITPRO=/opt/devkitpro make -j$(nproc)`
  -> produces `romm-switch-client.nro`. The Makefile fails with
  "Please set DEVKITPRO" if the variable is missing.
- Quick main.cpp syntax check without linking:
  `/opt/devkitpro/devkitA64/bin/aarch64-none-elf-g++ -fsyntax-only -std=c++17 -D__SWITCH__ -Iinclude -I/opt/devkitpro/libnx/include -I/opt/devkitpro/portlibs/switch/include -I/opt/devkitpro/portlibs/switch/include/SDL2 source/main.cpp`

## Deploy to QNAP share

After a successful Switch build, copy the NRO to the QNAP Public share so it can be
installed on the Switch easily:

```
cp romm-switch-client/romm-switch-client.nro /mnt/qnap-public/Public/SwitchRomM.nro
```

- `/mnt/qnap-public` is an NFS mount of `100.68.18.125:/Public` (QNAP).
- Destination file keeps the name `SwitchRomM.nro` (differs from the build output name).

## Run/debug on Switch via Sphaira netloader

1. On the Switch, open Sphaira -> Netloader (it listens on TCP 28232 only while
   that screen is open; "nxlink running" on the PC is not enough).
2. From the repo root:
   `/opt/devkitpro/tools/bin/nxlink -a <switch-ip> -s romm-switch-client/romm-switch-client.nro`
   (`-s` keeps the stdio server up; app stdout/stderr stream back on port 28233.)
3. The app also writes `sdmc:/switch/romm_switch_client/log.txt` (512 KB
4. If a transfer stalls or aborts mid-send and then the PC-side connection
   is refused, assume the NRO was launched by the netloader anyway (the
   transfer completing launched the app, which closed the netloader server).
   Re-enter Netloader and retry rather than assuming a crash.
5. argv[0] under the netloader is a temp path; self-update falls back to the
   canonical sdmc:/switch/romm_switch_client/ install path.
