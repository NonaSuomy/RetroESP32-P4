#!/usr/bin/env python3
"""
upload_papp.py — Upload a file to the ESP32-P4 SD card over USB Serial JTAG.

Usage:
    python tools/upload_papp.py firmware/opentyrian.papp
    python tools/upload_papp.py firmware/opentyrian.papp --port COM30
    python tools/upload_papp.py firmware/opentyrian.papp --dest /sd/roms/papp/opentyrian.papp

If --dest is not given, the file is placed at /sd/roms/papp/<filename>.
"""

import argparse
import os
import sys
import time

try:
    import serial
except ImportError:
    print("Error: pyserial not installed. Run: pip install pyserial", file=sys.stderr)
    sys.exit(1)

MAGIC = b"PAPU"
ACK = b"\x06"
DEFAULT_PORT = "COM30"
DEFAULT_BAUD = 115200
DEFAULT_SD_DIR = "/sd/roms/papp"


def find_response(ser, timeout=10, initial_buf=b""):
    """Read serial output until we find a line starting with \\x06 (protocol response)."""
    deadline = time.time() + timeout
    buf = initial_buf
    while time.time() < deadline:
        if ser.in_waiting > 0:
            buf += ser.read(ser.in_waiting)
        else:
            time.sleep(0.05)

        # Look for ACK-prefixed lines
        while ACK in buf:
            idx = buf.index(ACK)
            # Find end of line after ACK
            nl = buf.find(b"\n", idx)
            if nl < 0:
                break  # Incomplete line, wait for more
            line = buf[idx + 1 : nl].decode("utf-8", errors="replace").strip()
            buf = buf[nl + 1 :]
            return line

    return None


def wait_for_launcher(ser, timeout=20):
    """Wait through a UART bridge reset until the native launcher is ready."""
    deadline = time.time() + timeout
    buf = b""
    while time.time() < deadline:
        chunk = ser.read(4096)
        if chunk:
            buf = (buf + chunk)[-8192:]
            if b"Listening on UART0" in buf or b"Listening on USB Serial JTAG" in buf:
                return True
    return False


def upload(port, baud, filepath, dest):
    file_size = os.path.getsize(filepath)
    filename = os.path.basename(filepath)

    print(f"File:  {filepath} ({file_size:,} bytes)")
    print(f"Dest:  {dest}")
    print(f"Port:  {port}")

    # Open without toggling DTR/RTS.  On ESP32 USB Serial JTAG those control
    # lines can reset the board, which races the launcher before its upload
    # task is listening and makes an otherwise valid upload time out.
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = baud
    ser.timeout = 1
    ser.dtr = False
    ser.rts = False
    ser.open()

    # Opening the CH340 console port toggles the board reset lines.  Wait for
    # the launcher task after that reset before sending the PAPU header.
    print("Waiting for launcher...", end=" ", flush=True)
    if wait_for_launcher(ser):
        print("ready.")
    else:
        print("timeout; sending anyway.")

    # Flush any pending data
    ser.reset_input_buffer()

    # --- Send protocol header ---
    header = MAGIC + dest.encode("utf-8") + b"\n" + str(file_size).encode("utf-8") + b"\n"
    ser.write(header)
    ser.flush()

    print("Waiting for READY...")
    resp = find_response(ser, timeout=5)
    if resp is None:
        print("ERROR: No response from device (timeout). Is the firmware updated?", file=sys.stderr)
        ser.close()
        sys.exit(1)
    if not resp.startswith("READY"):
        print(f"ERROR: Unexpected response: {resp}", file=sys.stderr)
        ser.close()
        sys.exit(1)

    print("Device ready. Uploading...")

    # --- Send file data with flow control ---
    # After each chunk, wait for a \x06 ACK byte from the device.
    # This prevents the USB Serial JTAG RX buffer from overflowing.
    chunk_size = 4096
    sent = 0
    t0 = time.time()
    leftover = b""  # bytes read but not yet consumed

    with open(filepath, "rb") as f:
        while sent < file_size:
            chunk = f.read(min(chunk_size, file_size - sent))
            ser.write(chunk)
            ser.flush()
            sent += len(chunk)

            # Wait for chunk ACK (\x06) from device
            ack_deadline = time.time() + 10
            got_ack = False
            while time.time() < ack_deadline:
                if ser.in_waiting > 0:
                    leftover += ser.read(ser.in_waiting)
                elif not leftover:
                    time.sleep(0.01)
                    continue

                idx = leftover.find(ACK)
                if idx >= 0:
                    leftover = leftover[idx + 1:]  # keep bytes after ACK
                    got_ack = True
                    break

            if not got_ack:
                print(f"\nERROR: No chunk ACK at byte {sent}", file=sys.stderr)
                ser.close()
                sys.exit(1)

            pct = sent * 100 // file_size
            bar = "#" * (pct // 2) + "-" * (50 - pct // 2)
            print(f"\r  [{bar}] {pct:3d}% ({sent:,}/{file_size:,})", end="", flush=True)

    ser.flush()
    elapsed = time.time() - t0
    speed = file_size / elapsed / 1024 if elapsed > 0 else 0
    print(f"\n  Sent {sent:,} bytes in {elapsed:.1f}s ({speed:.1f} KB/s)")

    # --- Wait for confirmation ---
    print("Waiting for confirmation...")
    resp = find_response(ser, timeout=30, initial_buf=leftover)
    ser.close()

    if resp is None:
        print("ERROR: No confirmation from device (timeout)", file=sys.stderr)
        sys.exit(1)
    if resp.startswith("OK"):
        print(f"SUCCESS: {resp}")
    else:
        print(f"ERROR: {resp}", file=sys.stderr)
        sys.exit(1)


def main():
    parser = argparse.ArgumentParser(description="Upload a file to ESP32-P4 SD card over serial")
    parser.add_argument("file", help="Local file to upload (e.g. firmware/opentyrian.papp)")
    parser.add_argument("--port", default=DEFAULT_PORT, help=f"Serial port (default: {DEFAULT_PORT})")
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUD, help=f"Baud rate (default: {DEFAULT_BAUD})")
    parser.add_argument("--dest", default=None,
                        help="Destination path on SD card (default: /sd/roms/papp/<filename>)")
    args = parser.parse_args()

    if not os.path.isfile(args.file):
        print(f"Error: file not found: {args.file}", file=sys.stderr)
        sys.exit(1)

    dest = args.dest
    if dest is None:
        dest = f"{DEFAULT_SD_DIR}/{os.path.basename(args.file)}"

    upload(args.port, args.baud, args.file, dest)


if __name__ == "__main__":
    main()
