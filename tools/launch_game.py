#!/usr/bin/env python3
"""
launch_game.py — Launch a ROM on the RetroESP32-P4 via serial.

Sends the RUNR magic + ROM path over USB Serial JTAG. The device's
serial_upload task writes the path to NVS, sets the OTA boot partition,
and reboots into the emulator.

Usage:
    python tools/launch_game.py /sd/roms/neogeo/mslug3.zip
    python tools/launch_game.py /sd/roms/nes/mario.nes
    python tools/launch_game.py /sd/roms/snes/zelda.smc

The ROM path must be the full SD card path as seen by the device.
"""

import os
import sys
import time
import serial
import serial.tools.list_ports

DEFAULT_PORT = os.environ.get("ESP_PORT", "/dev/ttyUSB0")
BAUD = 115200  # doesn't matter for USB Serial JTAG, but required by pyserial
MAGIC = b"RUNR"
ACK_PREFIX = b"\x06"


def find_port():
    """Try DEFAULT_PORT, fall back to first ESP32 USB JTAG port."""
    for p in serial.tools.list_ports.comports():
        if p.device == DEFAULT_PORT:
            return DEFAULT_PORT
    # Fallback: look for ESP32 JTAG VID/PID
    for p in serial.tools.list_ports.comports():
        if p.vid == 0x303A:  # Espressif VID
            return p.device
    return DEFAULT_PORT


def wait_for_boot(s, timeout=30):
    """Wait for the device to finish booting by watching for serial_upload ready line."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        line = s.readline()
        if not line:
            continue
        text = line.decode("utf-8", "replace")
        # serial_upload_task prints "Listening on USB Serial JTAG" when ready
        if "Listening" in text or "serial_upload" in text.lower():
            return True
        # Also accept any line from app_main — means boot is well underway
        if "app_main" in text or "Emulator App Init Complete" in text or "Safe-boot check passed" in text:
            time.sleep(0.3)  # small extra margin for task startup
            return True
        # FPS/perf lines mean the emulator is already running (game in progress)
        if "FPS:" in text or "68K:" in text:
            return True
    return False


def open_serial(port):
    """Open serial port without triggering a device reset (no DTR/RTS toggle)."""
    s = serial.Serial()
    s.port = port
    s.baudrate = BAUD
    s.timeout = 2
    s.dtr = False
    s.rts = False
    s.open()
    return s


def launch(port, rom_path):
    s = open_serial(port)
    # Flush any stale data
    s.reset_input_buffer()

    # Wait for device to finish booting before sending command
    print("Waiting for device boot...", end=" ", flush=True)
    if wait_for_boot(s):
        print("ready.")
    else:
        print("timeout (sending anyway).")

    s.reset_input_buffer()

    # Send magic + path
    s.write(MAGIC)
    s.write((rom_path + "\n").encode("utf-8"))
    s.flush()

    # Wait for response (prefixed with \x06)
    deadline = time.time() + 5
    while time.time() < deadline:
        line = s.readline()
        if not line:
            continue
        # The \x06 prefix can appear anywhere in a line due to ESP_LOG mixing
        raw = line.decode("utf-8", "replace")
        ack_pos = raw.find("\x06")
        if ack_pos >= 0:
            resp = raw[ack_pos + 1:].strip()
            if resp.startswith("LAUNCH:"):
                print(f"OK — launching: {resp[7:]}")
                print("Device is rebooting into emulator...")
                s.close()
                return 0
            elif resp.startswith("ERR:"):
                print(f"ERROR: {resp[4:]}", file=sys.stderr)
                s.close()
                return 1

    print("ERROR: timeout waiting for device response", file=sys.stderr)
    s.close()
    return 1


def main():
    if len(sys.argv) < 2:
        print("Usage: python tools/launch_game.py <rom_path>")
        print("  e.g. python tools/launch_game.py /sd/roms/neogeo/mslug3.zip")
        sys.exit(1)

    rom_path = sys.argv[1]
    port = sys.argv[2] if len(sys.argv) > 2 else find_port()

    print(f"Launching '{rom_path}' via {port}...")
    sys.exit(launch(port, rom_path))


if __name__ == "__main__":
    main()
