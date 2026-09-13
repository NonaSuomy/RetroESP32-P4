# ESPHomeBrew

Cleaned PSRAM application binaries (`.papp`), emulator harnesses, and SD card distribution for the **ESP32-P4** (RetroESP32 / ESPHome PAPP loader ecosystem).

## Contents

- `apps/`: Native PSRAM application sources, emulator PAPP harnesses, and build configuration.
- `SDcard/`: Ready-to-deploy SD card filesystem tree:
  - `SDcard/roms/papp/`: Packaged `.papp` executables and sidecars for games (Doom, Duke3D, Quake, Red Alert, OpenTyrian, Another World, Wolf4SDL, ScummVM, etc.) and standalone emulator cores.
  - `SDcard/roms/`: Game data and ROM directories for individual engines and systems.
  - `SDcard/system_art/`: UI artwork and icons for the launcher carousel.

## Integration

Compatible with:
- **[RetroESP32-P4](https://github.com/NonaSuomy/RetroESP32-P4)** multi-system launcher firmware.
- **[ESPHome PAPP Loader](https://github.com/NonaSuomy/papp-conversions)** for on-device app streaming and store browsing.
