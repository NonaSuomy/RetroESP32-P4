# PAPP Developer Guide — Writing PSRAM Apps for RetroESP32-P4

A **PAPP** ("PSRAM Application") is a standalone program that the RetroESP32-P4 launcher loads from
the SD card into PSRAM and runs — no flash partition, no reboot. This guide explains how the
mechanism works, how to build your own PAPP, and the full set of services the launcher exposes to it.

> This is the developer guide. The implementation history, per-app porting notes, and resolved-bug
> log live in `PSRAM_APP.md`. The ABI is defined in
> `components/psram_app_loader/include/psram_app.h` — that header is the source of truth.

---

## 1. Concept

The launcher (in flash) hands a running PAPP a table of **function pointers** (`app_services_t`) for
everything it needs — display, audio, input, files, memory, tasks. The PAPP calls back into the
launcher through that table and **never calls ESP-IDF or libc directly** (those live in flash and
aren't linked into the app). When the PAPP's `app_entry()` returns, control drops straight back to
the launcher.

Why this design:
- **No flash partition / no flash wear** — apps live on SD as `.papp` files.
- **Instant switching** — load-and-call, < 1 s, no reboot (contrast the OTA emulators, which reboot).
- **Unlimited apps** — drop a `.papp` in `/sd/roms/papp/` and it appears in the launcher.
- **Big** — the full 32 MB PSRAM is available for app code, data, and heap.

Shipping PAPPs today: OpenTyrian, Doom (prboom-go), Quake (WinQuake), Duke3D, and the sprite-demo
template.

---

## 2. How It Works

### `.papp` binary format

A `.papp` is a flat RISC-V binary with a 32-byte header (`papp_header_t`):

| Offset | Field | Meaning |
|--------|-------|---------|
| 0x00 | `magic` | `0x50415050` ("PAPP") |
| 0x04 | `version` | ABI version (currently **1**) |
| 0x08 | `entry_off` | Byte offset to the entry fn from the start of the image (0 — see linker script) |
| 0x0C | `text_size` | `.text` + `.rodata` bytes |
| 0x10 | `data_size` | `.data` (initialized) bytes |
| 0x14 | `bss_size` | `.bss` (zero-filled) bytes to reserve |
| 0x18 | `flags` / 0x1C `reserved` | 0 |

Payload follows the header: a flat `[.text][.rodata][.data]` image (`.bss` is *not* stored — it's
zeroed at load). Built by compiling with a custom linker script and packing with `pack_papp.py`.

### Load → run → unload

The launcher runs this when you select a `.papp` (`launcher/main/main.c`, `psram_app_*` in
`components/psram_app_loader/psram_app_loader.c`):

**`psram_app_load(path, &handle)`**
1. Open the file, read + validate the 32-byte header (magic, ABI version, non-zero `text_size`).
2. `heap_caps_aligned_alloc` a **single 64 KB-page-aligned PSRAM buffer** sized
   `text_size + data_size + bss_size` (one contiguous image).
3. `fread` the `text+rodata+data` image into it; `memset` the trailing `.bss` + page padding to zero.
4. **Cache writeback** (`esp_cache_msync … C2M | DATA`) — push the data cache to physical PSRAM.

**`psram_app_run(handle)`**
1. `esp_mmu_vaddr_to_paddr()` → physical address of the buffer.
2. `esp_mmu_map(paddr, size, EXEC|READ|32BIT, PADDR_SHARED)` → a second, **executable** virtual
   mapping of the same physical pages (XIP — execute in place).
3. **Cache invalidate** (`… M2C | INST`) — force the CPU to fetch fresh instructions.
4. Populate the `app_services_t` table with launcher function pointers.
5. Call `entry_fn(&svc)` at `exec_ptr + entry_off` — **blocks until the app returns**.
6. `esp_mmu_unmap()` the executable mapping.

**`psram_app_unload(handle)`** — unmap (if still mapped), free the PSRAM buffer, free the handle.

### Memory / MMU model (why the rules exist)

- The heap maps physical PSRAM at `0x48000000`; the executable mapping lands at **`0x4A000000`**,
  which is why the linker script bases the app there.
- On ESP32-P4 the **I-bus and D-bus share the same virtual range**, so the one executable mapping is
  also readable/writable as data — a single contiguous image makes both PC-relative (`medany`) code
  and absolute initialized pointers resolve correctly.
- ESP-IDF's MMU validation forbids combining `EXEC` with `WRITE` or `8BIT`, so the mapping is
  `EXEC | READ | 32BIT` only. Cache coherence is manual: **writeback D-cache before invalidating
  I-cache**, which the loader does for you.

---

## 3. Quickstart — Build Your First PAPP

The template at `ESP32_P4_PAPP_Template/` is a complete, working example (a sprite you move with the
d-pad). Start from it.

### 3.1 Minimal app

```c
#define PAPP_APP_SIDE 1          /* excludes the launcher-only loader API from the header */
#include "psram_app.h"

__attribute__((section(".text.entry")))   /* entry MUST be first in .text → offset 0 */
int app_entry(const app_services_t *svc)
{
    if (svc->abi_version != PAPP_ABI_VERSION) return -1;   /* always check first */

    svc->log_printf("hello from PSRAM\n");

    papp_gamepad_state_t pad;
    for (;;) {
        svc->input_gamepad_read(&pad);
        if (pad.values[PAPP_INPUT_MENU]) break;   /* MENU = exit convention */
        /* ... draw a frame, submit audio ... */
        svc->delay_ms(16);
    }
    return 0;   /* clean exit → back to launcher */
}
```

### 3.2 Build → pack → upload → run

```powershell
# Source the ESP-IDF toolchain (provides riscv32-esp-elf-gcc)
$env:IDF_PYTHON_ENV_PATH = "C:\Users\97254\.espressif\python_env\idf5.5_py3.12_env"
& "C:\Users\97254\esp\v5.5.2\esp-idf\export.ps1"

# Compile + link + objcopy + pack → firmware\<AppName>.papp
.\tools\build_psram_app.ps1 -AppName MyApp -Sources src\main.c

# Multiple sources / extra include dirs:
.\tools\build_psram_app.ps1 -AppName MyApp -Sources "src\main.c","src\game.c" -ExtraIncludes "src\include"

# Upload to the SD card over USB Serial JTAG (no card removal)
python tools\upload_papp.py firmware\MyApp.papp --port COM30
```

The file lands at **`/sd/roms/papp/MyApp.papp`** and shows up in the launcher's **PAPP** carousel.
Select it to run. (You can also just copy the `.papp` onto the SD card directly.) A `<AppName>.png`
next to the `.papp` is shown as the carousel preview.

**Also commit it to the repo's SD staging folder:** copy the built `.papp` to
`SDcard/roms/papp/<AppName>.papp` and commit it. `SDcard/` is the git-tracked mirror of the physical
SD card — it's how PAPPs ship with the repo (doom/quake/opentyrian/duke3d live there). Uploading to
the device alone doesn't distribute it.

> **SD path:** the launcher browses `/sd/roms/papp/`. Some older comments/scripts say `/sd/apps/` —
> that's stale; use `/sd/roms/papp/`.

### 3.3 The rules (enforced by the build, or you crash)

1. `#define PAPP_APP_SIDE 1` **before** `#include "psram_app.h"`.
2. Entry point is exactly `__attribute__((section(".text.entry"))) int app_entry(const app_services_t *svc)`.
3. **No direct ESP-IDF or libc calls** — go through `svc->`. (Simple apps build `-nostdlib`; for a
   real libc see §6.)
4. Check `svc->abi_version == PAPP_ABI_VERSION` first; bail if it differs.
5. Watch `pad.values[PAPP_INPUT_MENU]` so the user can exit; **clean up** (delete tasks, close files,
   free memory) before returning.
6. Return `0` for success, negative for error.
7. Compiled position-independent (`-mcmodel=medany`, `--no-relax`), linked at `0x4A000000` via
   `tools/psram_app.ld`, entry in `.text.entry`.

### 3.4 Upload arbitrary SD-card files in batches

The `PAPU` serial protocol is a generic file writer; it is not limited to `.papp` files. It creates
missing parent directories and accepts any non-empty file up to 68 MiB. The single
`upload_papp.py` script handles both individual files and batches. For one file, `--dest` is the
complete destination path:

```bash
python3 tools/upload_papp.py assets/tracklist.cfg \
    --port /dev/ttyUSB0 --dest /sd/roms/quake/id1/tracklist.cfg
```

For multiple files or a complete directory tree, pass multiple sources to the same script. It keeps
one serial connection open and preserves subdirectories:

```bash
python3 tools/upload_papp.py SDcard/roms --port /dev/ttyUSB0 --dest /sd
```

That maps `SDcard/roms/quake/id1/pak0.pak` to `/sd/roms/quake/id1/pak0.pak`. Multiple individual
files can also be supplied as positional arguments. Empty directories are skipped because the
device creates directories as each file is written.

---

## 4. API Reference — `app_services_t`

Every service is a function pointer on the `svc` table passed to `app_entry()`. `abi_version` is the
first field and must equal `PAPP_ABI_VERSION`.

### Display

| Function | Signature | Purpose |
|----------|-----------|---------|
| `display_get_framebuffer` | `uint16_t *(void)` | Pointer to the native **800×480** RGB565 framebuffer (640×480 on HDMI) — draw here directly, no rotation |
| `display_get_emu_buffer` | `uint16_t *(void)` | Pointer to the launcher's shared **320×240** emulator intermediate buffer (not your app's canvas — see §5) |
| `display_flush` | `void(void)` | Flush the native FB (with rotate+scale) |
| `display_emu_flush` | `void(void)` | Flush the emu buffer |
| `display_clear` | `void(uint16_t color)` | Fill the screen with a solid RGB565 color |
| `display_set_scale` | `void(float sx, float sy)` | Override PPA scale factors |
| `display_write_frame_rgb565` | `void(const uint16_t *buf)` | Submit a standard 320×240 frame (default rotate+scale) |
| `display_write_frame_custom` | `void(const uint16_t *buf, uint16_t w, uint16_t h, float scale, bool byte_swap)` | Submit an arbitrary-size frame; PPA rotates 270° + scales uniformly |
| `display_write_rect` | `void(int x, int y, int w, int h, const uint16_t *data)` | Blit a rectangle into the native FB |
| `display_lock` / `display_unlock` | `void(void)` | Display mutex (guard shared FB access) |

See §5 for the display/orientation model.

### Audio

| Function | Signature | Purpose |
|----------|-----------|---------|
| `audio_init` | `void(int sample_rate)` | Init I2S/ES8311 at a sample rate (e.g. 11025, 22050, 32000, 44100) |
| `audio_submit` | `void(short *stereo_buf, int frame_count)` | Submit 16-bit signed **interleaved stereo** PCM (`frame_count` L/R pairs) |

Init once; then submit from your main loop or a dedicated audio task (see §6 for large-stack tasks).

### Input

| Function | Signature | Purpose |
|----------|-----------|---------|
| `input_gamepad_read` | `void(papp_gamepad_state_t *state)` | Fill `state.values[PAPP_INPUT_*]` (1 = pressed) |
| `touch_read` | `int(int *x, int *y)` | GT911 touch: returns 1 if touched (fills `*x,*y`), 0 if not. **Null-check first** (see below) |
| `paddle_read` | `int(void)` | Physical analog wheel (paddle pot): raw 12-bit `0..4095`, or `-1` if unavailable. **Null-check first** |

Buttons: `UP RIGHT DOWN LEFT SELECT START A B X Y L R MENU VOLUME` (prefix `PAPP_INPUT_`). By
convention **MENU exits** the app.

**Touch** is reported in **landscape native-framebuffer space** — `x` ∈ [0,799], `y` ∈ [0,479],
matching `display_get_framebuffer()`. If you draw into a smaller canvas, scale down by your own
factor. `touch_read` was appended after ABI v1, so it may be `NULL` on an older launcher — always
guard the call:

```c
int tx, ty;
if (svc->touch_read && svc->touch_read(&tx, &ty)) {
    /* touched at (tx, ty) in 800x480 landscape space */
}
```

This null-check is the rule for **every service appended after v1** — see §7.

> **Touch vs. MENU/VOLUME:** on the LCD board, `input_gamepad_read()` also synthesizes **MENU from
> the top touch strip and VOLUME from the bottom strip** (`odroid_input.c`). So a touch app that quits
> on `PAPP_INPUT_MENU` will exit the moment the user touches the top of the screen. **Touch apps
> should exit on a physical button** (e.g. `PAPP_INPUT_X`) instead.

**The analog wheel** (`paddle_read`) is the potentiometer on ADC2_CH2 / GPIO 51 — the same one the
Atari paddle games use (`components/stella/stella_run.cpp`, `components/atari800/atari800_run.cpp`
have long driven it directly; `paddle_read` simply exposes it to PAPPs, which previously had no
analog input at all).

Two things to know:

- **The value only refreshes inside `input_gamepad_read()`.** Calling `paddle_read()` in a tight
  loop without polling input returns a stale reading. Poll input each frame.
- **The ADC is brought up lazily on the first call.** `odroid_paddle_adc_init()` is idempotent and
  *reuses* an existing ADC unit handle, so this never conflicts with the emulator cores.

```c
if (svc->paddle_read) {
    papp_gamepad_state_t pad;
    svc->input_gamepad_read(&pad);        /* refreshes the reading */
    int raw = svc->paddle_read();         /* 0..4095, or -1 */
    if (raw >= 0) { int pct = (raw * 100) / 4095; /* ... */ }
}
```

Returns `-1` on the **HDMI build** (no paddle is wired) and on older launchers the pointer itself is
`NULL` — so always provide a fallback path. To tell a real pot from a floating pin, sample a few
times and check the spread, as the Atari cores do.

### File I/O

Standard C-style wrappers over the launcher's VFS. Handles are opaque `void*` (a `FILE*` underneath).
Paths are absolute from SD root, e.g. `/sd/roms/papp/data/level1.dat`.

| Function | Signature |
|----------|-----------|
| `file_open` | `void *(const char *path, const char *mode)` |
| `file_close` | `int (void *stream)` |
| `file_read` | `size_t (void *ptr, size_t size, size_t nmemb, void *stream)` |
| `file_write` | `size_t (const void *ptr, size_t size, size_t nmemb, void *stream)` |
| `file_seek` | `int (void *stream, long offset, int whence)` |
| `file_tell` | `long (void *stream)` |

### Memory

| Function | Signature | Notes |
|----------|-----------|-------|
| `mem_alloc` / `mem_calloc` / `mem_realloc` / `mem_free` | like `malloc`/`calloc`/`realloc`/`free` | default heap (PSRAM) |
| `mem_caps_alloc` | `void *(size_t size, uint32_t caps)` | choose the memory type |

Caps flags (OR together): `PAPP_MEM_CAP_SPIRAM` (PSRAM), `PAPP_MEM_CAP_INTERNAL` (internal SRAM),
`PAPP_MEM_CAP_DMA` (DMA-capable). DMA framebuffers: `mem_caps_alloc(n, PAPP_MEM_CAP_SPIRAM | PAPP_MEM_CAP_DMA)`.

### System

| Function | Signature | Purpose |
|----------|-----------|---------|
| `log_printf` | `int(const char *fmt, ...)` | printf to USB Serial JTAG — use this, **never** `printf()` |
| `log_vprintf` | `int(const char *fmt, va_list)` | vprintf variant |
| `delay_ms` | `void(int ms)` | Sleep (FreeRTOS `vTaskDelay`) |
| `get_time_us` | `int64_t(void)` | Microsecond timestamp |

### Settings (NVS)

| Function | Signature |
|----------|-----------|
| `settings_rom_path_get` / `_set` | `char *(void)` / `void(const char *)` |
| `settings_volume_get` / `_set` | `int32_t(void)` / `void(int32_t)` (0–4) |
| `settings_brightness_get` / `_set` | `int32_t(void)` / `void(int32_t)` |

### FreeRTOS tasks

| Function | Signature |
|----------|-----------|
| `task_create` | `int(void (*fn)(void*), const char *name, uint32_t stack_depth, void *arg, int priority, void *out_handle, int core)` |
| `task_delete` | `void(void *handle)` |

`task_create` transparently handles **large stacks**: ≤ 32 KB → normal internal-SRAM stack; > 32 KB →
a static task with a **PSRAM-backed stack** (TCB stays in internal RAM, as FreeRTOS requires). This is
how Quake gets its 256 KB stack. A FreeRTOS task **must never return** — loop on an exit flag and
`delay_ms` until deleted.

### Graphics helpers (hardware-accelerated)

| Function | Signature | Purpose |
|----------|-----------|---------|
| `png_load_rgb565` | `uint16_t *(const char *path, uint16_t *out_w, uint16_t *out_h)` | Decode a PNG from SD to a freshly-allocated RGB565 buffer (free it with `mem_free`) |
| `sprite_blit` | `int(uint16_t *fb, uint32_t fb_w, uint32_t fb_h, uint32_t x, uint32_t y, const uint16_t *sprite, uint32_t sp_w, uint32_t sp_h, uint16_t colorkey)` | PPA color-keyed sprite blend onto a framebuffer |
| `fb_copy` | `int(const uint16_t *src, uint16_t *dst, uint32_t w, uint32_t h)` | Hardware DMA framebuffer copy (PPA SRM) |

---

## 5. Display Model

**Three buffers, don't confuse them:**
- **Native framebuffer** — `display_get_framebuffer()`, **800×480** (640×480 HDMI). The real LCD
  memory; draw here directly if you want no rotation.
- **Emu intermediate buffer** — `display_get_emu_buffer()`, **320×240**. Launcher-owned, used by the
  OTA emulator scaling pipeline. Available to apps, but usually not what you want.
- **Your own canvas** — a buffer you allocate at whatever size suits you, submitted via
  `display_write_frame_custom()`. This is the normal path for a PAPP. The template uses **400×240**
  because at ×2.0 it fills the screen exactly (see below).

The physical LCD is **480×800 portrait**. Apps normally draw into their own **landscape** canvas and
let the PPA hardware rotate + scale on flush:

```
W×H landscape FB  →  PPA rotate 270° CCW  →  H×W  →  scale ×N  →  fits 480×800 LCD
```

`display_write_frame_custom(fb, W, H, scale, byte_swap)` does the rotate+scale in one PPA op. Output
dimensions after rotation are `out_w = H × scale`, `out_h = W × scale`, and must fit within 480×800.
Examples in the shipping apps: a 400×240 FB at ×2.0 → 480×800 (perfect fit); Doom's 320×200 at ×2.4 →
480×768; Quake's 320×240 at ×2.0 → 480×640.

Alternatives: `display_get_framebuffer()` for the native 800×480 buffer (draw directly, no rotation),
or `display_write_frame_rgb565()` for a standard 320×240 frame with the default rotate+scale.
`byte_swap` should be **false** — the PPA expects native little-endian RGB565.

---

## 6. Advanced — Complex Apps (libc, ports, big engines)

The simple template builds `-nostdlib`. Real ports (Doom/Quake/OpenTyrian/Duke3D) link newlib and
redirect its heap and file I/O into the service table. Use a dedicated build script per app
(`tools/build_<name>_papp.ps1`) that adds:

- **Newlib + runtime:** `-lc -lgcc -lm`, with `-nostartfiles` (custom `app_entry`, no C startup).
- **Malloc routing via `--wrap`:** `-Wl,--wrap=malloc,--wrap=free,--wrap=calloc,--wrap=realloc` (+ the
  `_r` reentrant variants). The wrappers forward to `svc->mem_*`; the common policy is **≥ 1 KB → PSRAM**,
  **< 1 KB → internal RAM**. Implemented in the app's `papp_syscalls.c`.
- **Newlib syscall stubs** (`papp_syscalls.c`): `_open/_read/_write/_lseek/_close` over an fd table
  that maps to `svc->file_*`; `_fstat` must return a real `st_size` (compute via seek — a garbage size
  makes newlib's `fseek(SEEK_END)` allocate gigabytes). Silence `_write(fd 1|2)` (stdout/stderr) to
  avoid USB-JTAG stalls.
- **Shim layer:** replace the engine's platform layer (SDL, retro-go `rg_*`, `I_*`/`Sys_*`) with thin
  files that call `svc->` — video (indexed→RGB565 + PPA scale), audio (→ `audio_submit`), input
  (gamepad → engine key events). See `apps/psram_opentyrian/`, `apps/psram_doom/`, `apps/psram_quake/`.
- **Exit path:** engines whose main loop never returns (Doom's `D_DoomMain`) exit via `setjmp`/`longjmp`
  back to `app_entry`, plus a watchdog task that polls MENU and requests exit.

### `.data` / `.bss` sizing for the packer

The generic `build_psram_app.ps1` packs with `data_size = bss_size = 0`, which is correct only if your
app has **no zero-initialized globals** (like the template). Apps that link newlib have `.bss`, so the
dedicated scripts read the ELF's `_bss_end` symbol (`riscv32-esp-elf-nm`), subtract the loaded image
end, and pass `--bss-size N` to `pack_papp.py`. If you hand-roll a build and your app has `.bss`, you
**must** pass `--bss-size` (and `--data-size` if separating `.data`) or the loader won't reserve/zero
that memory → corruption.

---

## 7. Constraints & Gotchas

- **No global constructors / no C++ static init** — there's no startup code to run them.
- **Position-independent only** — don't defeat `-mcmodel=medany` / `--no-relax`.
- **A FreeRTOS task returning = reboot** (`prvTaskExitError → abort`). Loop-and-delay until deleted.
- **Clean up every run** — audio task stopped, files closed, buffers freed. Leaked I2S mutexes cause
  the *next* app to deadlock in `audio_init`; the launcher calls `audio_reset_sample_rate()` after each
  run to help, but your app must exit its audio task gracefully.
- **USB Serial JTAG:** the launcher stops its serial-upload listener before running a PAPP and restarts
  it after; heavy `log_printf` with no host reading can still stall — keep logging light in hot loops.
- **ABI evolution (append-only):** new services are **appended to the end** of `app_services_t`,
  which keeps every existing field's offset — so existing `.papp` binaries run unchanged with **no
  rebuild** and `PAPP_ABI_VERSION` stays the same. The cost: a field appended after v1 may be `NULL`
  if a newer app runs on an older launcher, so **always null-check appended services before calling**
  (e.g. `touch_read`). Only bump `PAPP_ABI_VERSION` for an *incompatible* change — reordering,
  removing, or changing the signature of an existing field — which forces every app to be rebuilt
  (the loader rejects mismatched-version `.papp` headers, and each app checks `svc->abi_version`).

---

## 8. File & Tool Reference

| Path | Role |
|------|------|
| `components/psram_app_loader/include/psram_app.h` | **ABI** — header, `app_services_t`, enums, loader API |
| `components/psram_app_loader/psram_app_loader.c` | Loader: load/map/run/unload, service table, `task_create` |
| `ESP32_P4_PAPP_Template/` | Starter app (`main.c`) + `CONTEXT.md` |
| `tools/build_psram_app.ps1` | Generic build (compile → link → objcopy → pack) |
| `tools/build_<name>_papp.ps1` | Per-app builds (newlib, `--wrap`, bss sizing) |
| `tools/psram_app.ld` | Linker script — base `0x4A000000`, `.text.entry` first |
| `tools/pack_papp.py` | Wrap a flat `.bin` in the 32-byte `.papp` header |
| `tools/upload_papp.py` | Push a `.papp` to `/sd/roms/papp/` over USB Serial JTAG |
| `apps/psram_{opentyrian,doom,quake,duke3d}/` | Full port examples (shims, syscalls, compat headers) |
| `apps/psram_lvgl/` | **LVGL + touch reference app** (see §9) |
| `tools/build_lvgl_papp.ps1` | LVGL build (globs + caches ~276 LVGL objects, derives bss) |
| `PSRAM_APP.md` | Implementation history, porting notes, resolved-bug log |

### Build flags (reference)

Compile: `-march=rv32imafc_zicsr_zifencei -mabi=ilp32f -mcmodel=medany -Os -ffunction-sections
-fdata-sections -DPAPP_APP_SIDE=1 -I<psram_app.h dir>`.
Link: `-nostartfiles -nostdlib -T tools/psram_app.ld -Wl,--gc-sections -Wl,--entry=app_entry
-Wl,--no-relax` (complex apps add `-nodefaultlibs … -lc -lgcc -lm` and `--wrap=` for malloc family).

---

## 9. Case Study — LVGL v9 with touch (`apps/psram_lvgl`)

A complete, working reference for running a real GUI toolkit inside a PAPP. Build it with:

```powershell
.\tools\build_lvgl_papp.ps1            # incremental (LVGL objects are cached)
.\tools\build_lvgl_papp.ps1 -Clean     # full rebuild (~276 files, a few minutes)
```

LVGL itself is **not committed** (it is ~200 MB with history). Fetch it once:

```powershell
git clone --depth 1 -b release/v9.2 https://github.com/lvgl/lvgl.git third_party\lvgl
```

Result: `firmware\psram_lvgl.papp` — **310 KB code + 89 B data + 257 KB bss**, also auto-staged
into `SDcard\roms\papp\`.

### 9.1 The three things you must wire up

| LVGL needs | We provide | Where |
|------------|-----------|-------|
| A display | `disp_flush_cb` → `svc->display_write_frame_custom()` | `main.c` |
| An input device | `touch_read_cb` (pointer indev) → `svc->touch_read()` | `main.c` |
| A millisecond tick | `lv_tick_set_cb(tick_cb)` → `svc->get_time_us()/1000` | `main.c` |

Then it is just `lv_timer_handler()` in your main loop. Always call `lv_display_flush_ready()`
at the end of the flush callback or LVGL stalls after one frame.

### 9.2 Making LVGL survive `-nostdlib`

LVGL is configured in `apps/psram_lvgl/lv_conf.h` to bring its **own** C library, so it needs no libc:

```c
#define LV_USE_STDLIB_MALLOC   LV_STDLIB_BUILTIN   /* tlsf on a static pool */
#define LV_USE_STDLIB_STRING   LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_SPRINTF  LV_STDLIB_BUILTIN
#define LV_USE_OS              LV_OS_NONE
#define LV_USE_FLOAT           0                   /* avoids soft-double helpers */
#define LV_MEM_SIZE            (256 * 1024U)       /* lives in .bss — see 9.3!  */
```

Two traps that will bite you:

1. **GCC still emits `memset`/`memcpy` implicitly** (struct init, array copies) even though the
   source never calls them. `apps/psram_lvgl/papp_libc.c` defines them. But that file **must** be
   compiled with `-fno-tree-loop-distribute-patterns`, or GCC turns the byte loop inside your own
   `memset()` into a call to `memset()` — infinite recursion. The build script passes it globally.
2. **Link `-lgcc`.** It supplies helper routines (64-bit divide, etc.). It is safe here because the
   linker script binds the app at its real runtime address (`0x4A000000`), so absolute addressing
   inside libgcc resolves correctly.

`-Wl,--no-warn-rwx-segments` just silences an expected notice: our flat image is one LOAD segment
holding text+data, which `ld` flags as RWX.

### 9.3 ⚠ The `.bss` trap (read this before shipping any large PAPP)

`.bss` is `NOLOAD`, so it is **not** in the flat binary. The loader allocates `text + data + bss`
and zeroes the bss region — **but only if the `.papp` header says how big `.bss` is.** Pack with
`bss_size = 0` and the app writes past its own allocation and corrupts the heap. LVGL alone puts
~256 KB there, so this is not a corner case.

`build_lvgl_papp.ps1` derives it from the linker symbol (same method as `build_doom_papp.ps1`) and
**refuses to pack** if it cannot:

```powershell
$bssEnd    = <address of _bss_end from riscv32-esp-elf-nm>
$bssToZero = $bssEnd - (0x4A000000 + <flat binary size>)
python tools\pack_papp.py $bin $papp --bss-size $bssToZero
```

Verify in the boot log — `load + bss` must land exactly on `_bss_end`:

```
Header OK: text=310025 data=0 bss=262695 entry_off=0
App buffer: 0x48250000 (load=310025 bss=262695 total=572720)
Executable mapping at: 0x4a000000     <- matches the link base
```

> The generic `tools/build_psram_app.ps1` does **not** pass `--bss-size`. That is fine for tiny apps
> with no globals, but any app with meaningful static data must use a per-app script that does.

### 9.4 Resolution and touch mapping

The native framebuffer is **800×480 on LCD but 640×480 on HDMI**, and there is no "get framebuffer
size" service. So a portable PAPP renders a **fixed 400×240 canvas** and lets the launcher scale it:

```c
svc->display_write_frame_custom(px_map, 400, 240, 2.0f, /*byte_swap=*/false);
```

400×240 × 2.0 maps exactly onto 800×480, letterboxes sensibly on HDMI, and is 4× less pixel work
than native — which keeps LVGL's software renderer smooth. `svc->touch_read()` reports **native**
coordinates, so the canvas mapping is simply `canvas = native / 2`.

If colours ever look red/blue-swapped, flip the `byte_swap` flag.

### 9.5 Gotchas specific to touch apps

- **Exit on the physical `X` button, never `MENU`.** On the LCD board the launcher synthesizes
  `MENU` from the top touch strip, so a MENU-to-exit app quits the moment you touch near the top.
- **`touch_read` is in the ABI's append-only zone** — an older launcher may not have it. Null-check:
  `if (!svc->touch_read) { ...bail... }`.
- **Decorative overlays must not steal input.** The finger-follow dot calls
  `lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE)`.

### 9.6 Launching it

`RUNR` (the serial launch protocol) only understands ROMs — it answers `ERR:unknown ext 'papp'`.
Launch a PAPP from the launcher's **PAPP section** on the device. Give it a `300×300` PNG next to
the binary (`psram_lvgl.png`) or the browser logs a missing-icon error.

### 9.7 What the demo shows

A touch-driven dashboard at 400×240: an interactive arc gauge, a slider bound to it, a live
auto-scrolling chart, a bar, an on/off switch, a live FPS + free-heap readout, and a dot that
follows your finger (the most direct proof the indev works).
