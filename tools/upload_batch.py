#!/usr/bin/env python3
"""Batch-upload arbitrary files and directories to the ESP32 SD card.

Examples:
    python3 tools/upload_batch.py SDcard/roms --dest /sd
    python3 tools/upload_batch.py firmware/quake.papp firmware/touchtest.papp \
        --dest /sd/roms/papp

Directory contents are uploaded recursively.  A directory's basename is
kept below --dest, so ``SDcard/roms`` maps to ``/sd/roms`` by default.
The serial connection is opened once and reused for the whole batch.
"""

import argparse
import os
import sys
import time
from pathlib import Path

import serial


ACK = b"\x06"
MAGIC = b"PAPU"
CHUNK_SIZE = 4096
MAX_FILE_SIZE = 68 * 1024 * 1024


class Protocol:
    def __init__(self, ser):
        self.ser = ser
        self.buf = b""

    def _fill(self, timeout):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.ser.in_waiting:
                self.buf = (self.buf + self.ser.read(self.ser.in_waiting))[-16384:]
                return True
            time.sleep(0.01)
        return False

    def wait_response(self, prefixes, timeout=15):
        """Wait for an atomic ACK-prefixed response, ignoring ESP_LOG text."""
        deadline = time.time() + timeout
        patterns = [(ACK + prefix.encode("ascii"), prefix) for prefix in prefixes]
        while time.time() < deadline:
            for pattern, prefix in patterns:
                idx = self.buf.find(pattern)
                if idx >= 0:
                    nl = self.buf.find(b"\n", idx + len(pattern))
                    if nl >= 0:
                        line = self.buf[idx + 1:nl].decode("utf-8", "replace").strip()
                        self.buf = self.buf[nl + 1:]
                        return line
            self._fill(min(0.2, max(0, deadline - time.time())))
        return None

    def wait_ack(self, timeout=30):
        deadline = time.time() + timeout
        while time.time() < deadline:
            idx = self.buf.find(ACK)
            if idx >= 0:
                self.buf = self.buf[idx + 1:]
                return True
            self._fill(min(0.2, max(0, deadline - time.time())))
        return False


def wait_for_launcher(ser, timeout=20):
    deadline = time.time() + timeout
    buf = b""
    while time.time() < deadline:
        data = ser.read(4096)
        if data:
            buf = (buf + data)[-8192:]
            if b"Listening on UART0" in buf:
                return True
    return False


def collect_files(sources, dest_root):
    result = []
    for source_name in sources:
        source = Path(source_name).resolve()
        if source.is_file():
            result.append((source, f"{dest_root.rstrip('/')}/{source.name}"))
            continue
        if source.is_dir():
            for path in sorted(p for p in source.rglob("*") if p.is_file()):
                relative = path.relative_to(source)
                destination = "/".join((dest_root.rstrip("/"), source.name,
                                         relative.as_posix()))
                result.append((path, destination))
            continue
        raise SystemExit(f"source does not exist: {source_name}")
    return result


def upload_one(proto, ser, local_path, destination):
    size = local_path.stat().st_size
    if size == 0 or size > MAX_FILE_SIZE:
        raise SystemExit(f"invalid file size for {local_path}: {size}")
    if len(destination) >= 256:
        raise SystemExit(f"destination path is too long: {destination}")

    header = (MAGIC + destination.encode("utf-8") + b"\n" +
              str(size).encode("ascii") + b"\n")
    ser.write(header)
    ser.flush()
    response = proto.wait_response(("READY",), 10)
    if response != "READY":
        raise SystemExit(f"device rejected {local_path}: {response!r}")

    sent = 0
    with local_path.open("rb") as source:
        while sent < size:
            chunk = source.read(min(CHUNK_SIZE, size - sent))
            if not chunk:
                raise SystemExit(f"local file ended early: {local_path}")
            ser.write(chunk)
            ser.flush()
            if not proto.wait_ack():
                raise SystemExit(f"timeout uploading {local_path} at {sent}")
            sent += len(chunk)

    response = proto.wait_response(("OK ", "ERR:"), 30)
    if response is None or not response.startswith("OK "):
        raise SystemExit(f"upload failed for {local_path}: {response!r}")
    print(f"uploaded {local_path} -> {destination} ({sent:,} bytes)")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sources", nargs="+", help="files or directories to upload")
    parser.add_argument("--dest", default="/sd",
                        help="SD destination root (default: /sd)")
    parser.add_argument("--port", default="/dev/ttyUSB0")
    parser.add_argument("--baud", type=int, default=115200)
    args = parser.parse_args()

    files = collect_files(args.sources, args.dest)
    if not files:
        raise SystemExit("no files found")

    print(f"{len(files)} file(s) queued for {args.port}")
    ser = serial.Serial()
    ser.port = args.port
    ser.baudrate = args.baud
    ser.timeout = 0.2
    ser.dtr = False
    ser.rts = False
    ser.open()
    try:
        print("Waiting for launcher...", end=" ", flush=True)
        if not wait_for_launcher(ser):
            raise SystemExit("timeout waiting for launcher")
        print("ready")
        ser.reset_input_buffer()
        proto = Protocol(ser)
        for local_path, destination in files:
            upload_one(proto, ser, local_path, destination)
    finally:
        ser.close()

    print("batch upload complete")


if __name__ == "__main__":
    try:
        main()
    except serial.SerialException as exc:
        print(f"serial error: {exc}", file=sys.stderr)
        raise SystemExit(1)
