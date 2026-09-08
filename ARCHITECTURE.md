# RetroESP32-P4 — Architecture

> Authoritative chronological record is `DEVELOPMENT_LOG.md` (currently **Phase 49, May 25 2026**).
> If this file disagrees with the code, the code wins — update this file.

## Contents

1. [Overview](#overview)
2. [Hardware](#hardware)
3. [Two Execution Models](#two-execution-models)
4. [Flash Partition Layout](#flash-partition-layout)
5. [Directory Structure](#directory-structure)
6. [Components](#components)
7. [Launcher](#launcher)
8. [Emulator Apps](#emulator-apps)
9. [Neo Geo Cache](#neo-geo-cache)
10. [NVS Protocol](#nvs-protocol)
11. [HDMI Target](#hdmi-target)
12. [SDK Configuration](#sdk-configuration)
13. [Build & Flash](#build--flash)
14. [Adding an Emulator](#adding-an-emulator)
15. [Binary Sizes](#binary-sizes)
16. [Troubleshooting](#troubleshooting)

---

## Overview

Multi-system retro emulator + native-app platform on the **ESP32-P4**, ESP-IDF v5.5.2. A launcher in
the `factory` partition switches between emulators (each a separate OTA firmware) and native ports
(loaded into PSRAM). Authoritative extension→slot map: `get_ota_slot()` in `launcher/main/main.c`.

| System | Core | Delivery | Slot | Extensions |
|--------|------|----------|------|-----------|
| NES | nofrendo | OTA | ota_0 | `.nes` |
| Game Boy / Color | gnuboy | OTA | ota_1 | `.gb` `.gbc` |
| SMS / Game Gear / ColecoVision | smsplus | OTA | ota_2 | `.sms` `.gg` `.col` |
| ZX Spectrum | spectrum | OTA | ota_3 | `.z80` `.sna` |
| Atari 2600 | stella | OTA | ota_4 | `.a26` `.bin` |
| Atari 7800 | prosystem | OTA | ota_5 | `.a78` |
| Atari Lynx | handy | OTA | ota_6 | `.lnx` |
| PC Engine | huexpress | OTA | ota_7 | `.pce` |
| Atari 800XL / 5200 | atari800 | OTA | ota_8 | `.xex` `.atr` `.a52` |
| *(free)* | — | OTA | ota_9 | — |
| SNES | snes9x | OTA | ota_10 | `.smc` `.sfc` |
| Genesis / Mega Drive | gwenesis | OTA | ota_11 | `.md` `.gen` |
| Neo Geo | gngeo | OTA | ota_12 | `.zip` (see [Neo Geo Cache](#neo-geo-cache)) |
| Doom / Quake / Duke3D / OpenTyrian | prboom / quake / duke3d / opentyrian | PSRAM | — | `.papp` on SD |

---

## Hardware

| Component | Spec |
|-----------|------|
| MCU | ESP32-P4, RISC-V dual-core @ 360 MHz |
| Flash | 16 MB, DIO, 80 MHz |
| PSRAM | 32 MB, HEX mode, 200 MHz |
| Internal SRAM | 768 KB |
| Display (LCD) | 4.3" 480×800 MIPI-DSI, ST7701S |
| Display (HDMI) | Olimex LT8912 DSI→HDMI bridge @ 640×480 (alternate target) |
| Touch | GT911 (I2C) |
| Audio | ES8311 codec via I2S |
| Input | Onboard GPIO pad (pins 28–35) OR'd with USB-HID gamepad (PS3 native) |
| SD | SDMMC, FAT32 + LFN |

`odroid_input_gamepad_read()` OR's the GPIO pad and USB controller. Some board revisions have pins
that float HIGH (phantom presses); `odroid_input.c` does per-pin stuck detection at init (Phase 49).

---

## Two Execution Models

| | OTA multi-binary | PSRAM `.papp` |
|---|---|---|
| Used by | 13 emulators | Native ports (Doom, Quake, Duke3D, OpenTyrian) |
| Code location | A dedicated flash partition (`ota_N`) | An `.papp` file on the SD card |
| Start | Set OTA boot partition + `esp_restart()` (reboot) | Load into PSRAM, MMU-map executable, call |
| Return | Set boot = factory + `esp_restart()` (reboot) | `app_entry()` returns — launcher resumes, no reboot |
| Why | Cores are large and conflict at link time | Ports are self-contained via a services vtable |

**OTA:** a monolithic all-in-one build exceeded IRAM/DRAM limits, so each emulator is an independent
firmware with its own memory layout — this also eliminates cross-core symbol conflicts and allows
large cores (smsplus, stella, gwenesis, gngeo ~1.2 MB each). Boot selection is via ESP-IDF `otadata`.

**PSRAM:** the loader (`components/psram_app_loader`) reads the `.papp` from SD into PSRAM, maps it
executable (XIP), and calls `app_entry(&services)`. The app makes no direct ESP-IDF calls — display,
audio, input, file I/O and memory all route through the `app_services_t` vtable (~60 fn pointers);
`malloc`/`free` are redirected via linker `--wrap`. On return the launcher unmaps and continues. No
flash partition, no flash wear. `.papp` files build from `ESP32_P4_PAPP_Template/` (starter) or
`apps/psram_*/` (ports). **Writing a PAPP: `PAPP_GUIDE.md`**; impl history in `PSRAM_APP.md`.

---

## Flash Partition Layout

16 MB flash, source of truth `partitions_ota.csv`. **Offsets must match `$flash_map` in `flash_all.ps1`.**
Bootloader is at **0x2000** (ESP32-P4 requirement, not 0x0); partition table at 0x8000.

| Partition | Offset | Size | Contents |
|-----------|--------|------|----------|
| nvs | 0x009000 | 16 KB | NVS store |
| otadata | 0x00D000 | 8 KB | OTA boot selection |
| factory | 0x010000 | 768 KB | Launcher |
| ota_0 | 0x0D0000 | 576 KB | NES |
| ota_1 | 0x160000 | 640 KB | GB/GBC |
| ota_2 | 0x200000 | 1.31 MB | SMS/GG/COL |
| ota_3 | 0x350000 | 768 KB | ZX Spectrum |
| ota_4 | 0x410000 | 1.25 MB | Atari 2600 |
| ota_5 | 0x550000 | 640 KB | Atari 7800 |
| ota_6 | 0x5F0000 | 640 KB | Atari Lynx |
| ota_7 | 0x690000 | 704 KB | PC Engine |
| ota_8 | 0x740000 | 768 KB | Atari 800 |
| ota_9 | 0x800000 | 768 KB | *(free)* |
| ota_10 | 0x8C0000 | 960 KB | SNES |
| ota_11 | 0x9B0000 | 1.31 MB | Genesis |
| ota_12 | 0xB00000 | 1.5 MB | Neo Geo |

PSRAM `.papp` apps use no partition — they live on the SD card.

---

## Directory Structure

```
partitions_ota.csv              # OTA table (shared by launcher + all apps)
build_all.ps1 / build_all_hdmi.bat   # Build all → firmware/ (LCD / HDMI)
flash_all.ps1                   # Flash all via esptool
generate_merged_bin[_hdmi].ps1  # Merge → RetroESP32_P4[_HDMI]_v1.bin

launcher/main/main.c            # Carousel UI, ROM browser, get_ota_slot(), OTA switch, PSRAM loader
apps/<emu>/main/{main.c,<emu>_run.c}  # OTA app skeleton + per-emulator glue
apps/sdkconfig_common.defaults  # Shared emulator sdkconfig
apps/psram_*/                   # PSRAM app harnesses (doom, quake, duke3d, opentyrian, test)

components/app_common/          # Emulator lifecycle (init/exit/safe-boot)
components/odroid/              # HAL: system, audio, display, input, sdcard, settings
components/psram_app_loader/    # .papp loader (MMU map + services vtable)
components/{st7701_lcd,gt911_touch,ppa_engine}/   # LCD + touch + 2D accel
components/{lt8912,hdmi_display}/                  # HDMI path
components/{gamepad,usb_host_hid}/                 # USB HID gamepad stack
components/{audio,pngaux,pngdec}/                  # Audio + PNG sprite decode
components/<core>/              # Emulator cores + native ports

firmware/ firmware_hdmi/        # Build outputs
main/                           # LEGACY monolithic main — unused
managed_components/             # ESP-IDF component-manager deps
```

---

## Components

**`app_common`** — standard OTA-app lifecycle:
```c
void app_init(void);          // NVS → odroid_system_init → audio → gamepad → SD mount → safe-boot check
int  app_get_rom_path(char *buf, int len);   // read RomFilePath from NVS; 0 ok, -1 unset
void app_return_to_launcher(void);            // set boot=factory, esp_restart(); noreturn
```

**`odroid`** (HAL) — `odroid_system` (I2C/PPA/USB/display/touch init), `odroid_audio` (I2S+ES8311),
`odroid_display` (DSI/HDMI framebuffer, PPA scale/rotate), `odroid_input` (GPIO pad + USB HID),
`odroid_sdcard` (SDMMC/FAT32), `odroid_settings` (NVS).

Other: `psram_app_loader`, `st7701_lcd`, `lt8912`/`hdmi_display`, `gt911_touch`, `ppa_engine`,
`gamepad`/`usb_host_hid`, `audio`, `pngaux`/`pngdec`. Cores: `nofrendo gnuboy smsplus spectrum stella
prosystem handy huexpress atari800 snes9x gwenesis gngeo`. Ports: `prboom quake duke3d opentyrian`.

---

## Launcher

Full-screen graphical menu in the `factory` partition: carousel system-selection, SD ROM browser
(LFN, per-system dirs, favorites/recents), OTA switching for emulators and PSRAM load-and-call for
`.papp`. Also reads `/sd/roms/neogeo/gamenames.txt` (`shortname=Display Name`) for friendly Neo Geo titles.

**Launch an emulator (`rom_run`):** write `RomFilePath` + `DataSlot` to NVS → `get_ota_slot(ext)` →
`esp_partition_find_first()` → `esp_ota_set_boot_partition()` → `esp_restart()`.

Links hardware/UI components + `psram_app_loader` — **no emulator cores**.

---

## Emulator Apps

Each OTA app is a minimal project with a standard `app_main()`:
```c
void app_main(void) {
    app_init();
    char rom_path[256];
    if (app_get_rom_path(rom_path, sizeof(rom_path)) != 0) app_return_to_launcher();
    <core>_run(rom_path);          // blocks until the user exits
    app_return_to_launcher();
}
```
The `main` component `REQUIRES app_common <core>` only — small binaries, no cross-core conflicts. The
per-emulator `<emu>_run.c` holds the glue (audio pipeline, input mapping, video/scaling) where most
per-system tuning lives.

**Safe boot:** `app_init()` polls the gamepad ~500 ms; holding **A** returns to the launcher without
running the emulator — the recovery hatch when a core hangs on load.

---

## Neo Geo Cache

Neo Geo (gngeo) is the heaviest subsystem and does **not** run from the `.zip` at runtime. A game is a
MAME `.zip` romset whose sprite (C) ROMs need de-interleaving, often **CMC42/CMC50 decryption**, and
tile conversion — too slow to do on-device, and too large to hold converted in PSRAM.

**Workaround:** the host script **`SDcard/roms/neogeo/gen_cache.py <game>`** precomputes this on the
PC (reads `<game>.zip` + `gngeo_data.zip`, applies decryption + tile conversion matching `roms.c`) and
writes flat cache files into a per-game folder `/sd/roms/neogeo/<game>/`:

| File | Contents | Runtime |
|------|----------|---------|
| `<game>.ctile` | Converted/decrypted sprite tiles | Streamed; 12 MB PSRAM bank cache |
| `<game>.cusage` | Tile usage map (magic `"CU09"`) | Skips invisible tiles |
| `<game>.vroma` / `.vromb` | ADPCM-A / -B audio | Streamed; 4 MB PSRAM page cache |
| `<game>.sfix` | Fix-layer for CMC-encrypted games | Loaded at boot |

**Runtime:** launcher passes `.../mslug.zip`; `neogeo_run.c` derives `game_name`/`rom_dir`; the core
reads small P/M/S/BIOS ROMs from the zip (plus `neogeo.zip` BIOS, `gngeo_data.zip`) and streams
sprite/audio from the cache. `get_cache_path()` resolves `ROOTPATH<game>/<game>.<ext>`
(`ROOTPATH = /sd/roms/neogeo/`). A missing cache triggers slow on-device generation.
`gen_ctile.py` / `gen_vrom.py` at the root are the standalone generators `gen_cache.py` is built from.
Details in `Neogeo.md`.

**Add a game:** drop `<game>.zip` in `/sd/roms/neogeo/`, run `python gen_cache.py <game>` there.

---

## NVS Protocol

Launcher writes, emulator reads; namespace `"Odroid"`, API in `odroid_settings.h`:

| Key | Purpose |
|-----|---------|
| `RomFilePath` | SD path to the ROM |
| `DataSlot` | Resume slot (0 = fresh, 1 = resume) |
| `StartAction` | New vs restart |
| `Volume`, `Brightness` | Persisted settings |

(The launcher also stores its UI cursor under namespace `"storage"`, key `STEP`.)

---

## HDMI Target

Two board targets, built/flashed separately:

| Target | Display | Build | Output |
|--------|---------|-------|--------|
| LCD | 480×800 ST7701S + GT911 | `build_all.ps1` | `RetroESP32_P4_v1.bin` |
| HDMI | LT8912 DSI→HDMI @ 640×480 | `build_all_hdmi.bat` | `RetroESP32_P4_HDMI_v1.bin` |

HDMI enables `CONFIG_HDMI_OUTPUT=y`, links `lt8912` + `hdmi_display`, and shares I2C between the touch
controller and LT8912 (Phase 46.23). A stale `CONFIG_HDMI_OUTPUT=y` in an LCD app's `sdkconfig` →
black screen + broken audio; `build_all.ps1` deletes each app's `sdkconfig` before building. See `HDMIport.md`.

---

## SDK Configuration

Emulator apps share `apps/sdkconfig_common.defaults`; launcher uses `launcher/sdkconfig.defaults`
(+ `sdkconfig.hdmi.defaults`). Key settings:

```ini
CONFIG_IDF_TARGET="esp32p4"
CONFIG_SPIRAM=y  CONFIG_SPIRAM_MODE_HEX=y  CONFIG_SPIRAM_SPEED_200M=y
CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_PARTITION_TABLE_CUSTOM=y  CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="<rel>/partitions_ota.csv"
CONFIG_FATFS_LFN_HEAP=y  CONFIG_FATFS_MAX_LFN=255      # required, else 8.3-truncated ROM names
CONFIG_USB_HOST_HUBS_SUPPORTED=y  CONFIG_USB_HOST_HUB_MULTI_LEVEL=n   # hubs unusable (no TT)
CONFIG_COMPILER_OPTIMIZATION_SIZE=y
# HDMI target only: CONFIG_HDMI_OUTPUT=y
```

---

## Build & Flash

**Prereqs:** ESP-IDF v5.5.2 installed locally; use the Python environment created by ESP-IDF.

**Build all (LCD):** `.\build_all.ps1` — clean-builds launcher + 12 OTA apps into `firmware/`, then
merges to `RetroESP32_P4_v1.bin`. Deletes each project's `build/` and `sdkconfig` first (avoids stale
config). **HDMI:** `.\build_all_hdmi.bat` → `firmware_hdmi/`.

**Single app:**
```powershell
& "$env:IDF_PATH\export.ps1"
cd apps\snes ; idf.py build      # delete sdkconfig + build/ first if last built for the other target
```

**Flash all:** `.\flash_all.ps1` — set `$PORT` to the target's serial port before running. Flash
map mirrors `partitions_ota.csv`: bootloader 0x2000, partition-table 0x8000,
ota_data_initial 0xD000, launcher 0x10000, then each `*_app.bin` at its ota offset. Or flash the
merged `RetroESP32_P4_v1.bin` at 0x0.

**Flash one app:**
```powershell
python -m esptool --chip esp32p4 -p $PORT -b 460800 --before default_reset --after hard_reset `
  write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m 0xD0000 firmware\nes_app.bin
```

---

## Adding an Emulator

1. **Core component** `components/<core>/` exposing `void <core>_run(const char *rom_path)`.
2. **App project** `apps/<core>/` — project CMake sets `EXTRA_COMPONENT_DIRS` to `../../components`;
   `main/` `REQUIRES app_common <core>` with the standard `app_main()`; copy `sdkconfig_common.defaults`.
3. **Register extensions** in `get_ota_slot()` (and the browser's accepted-extension logic) in `launcher/main/main.c`.
4. **Check size** against the target slot in `partitions_ota.csv`; if it won't fit, resize and shift
   all later offsets (+ update `flash_all.ps1`).
5. **Add to** `build_all.ps1` (+ `build_all_hdmi.bat`) and `flash_all.ps1`.

For a native port, build a `.papp` from `ESP32_P4_PAPP_Template/` instead — no partition, no script changes.

---

## Binary Sizes

Approximate (from `partitions_ota.csv`; re-measure after builds). Partitions that run **tight**:
launcher (~706 KB / 768 KB), nes (~567 KB / 576 KB), atari800 (~734 KB / 768 KB). Others carry
60–360 KB headroom. Largest cores: sms ~1231 KB, genesis ~1212 KB, stella ~1184 KB, neogeo ~1177 KB.
On overflow: shrink the app (`-Os`, strip) or grow the partition and shift later offsets.

---

## Troubleshooting

| Symptom | Cause / Fix |
|---------|-------------|
| Stuck on a broken emulator | Hold **A** during the first second of boot → safe-boot to launcher |
| Black screen + broken audio after build | Stale `CONFIG_HDMI_OUTPUT` in the app's `sdkconfig`; delete `sdkconfig` + `build/`, rebuild |
| Gamepad works direct but not via hub | ESP-IDF has no Transaction Translator; FS HID behind a HS hub can't enumerate (Phase 47). Connect directly |
| Audio crackles / distorts | Audio strategy is per-emulator (Phase 48); a fixed samples/frame at <60 FPS underruns the I2S DMA |
| Phantom / stuck button | A GPIO pad pin floating HIGH (Phase 49, e.g. GPIO 30 → R); `odroid_input.c` stuck detection, board-dependent |
| 8.3-truncated filenames | Missing FATFS LFN config; set `CONFIG_FATFS_LFN_HEAP=y` + `MAX_LFN=255`, clean rebuild |
| Binary too large | See [Binary Sizes](#binary-sizes) |
| "OTA partition ota_N not found" | Partition-table mismatch; reflash `esptool write_flash 0x8000 firmware/partition-table.bin` |
| Neo Geo won't boot / very slow first launch | Missing precomputed cache; run `gen_cache.py <game>` (see [Neo Geo Cache](#neo-geo-cache)) |
| Emulator returns to launcher immediately | NVS `RomFilePath` unset or SD not mounted; check serial + card (FAT32) |
