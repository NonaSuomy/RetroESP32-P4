#!/usr/bin/env python3
"""Upload one or more arbitrary files/directories to the ESP32 SD card.

Single-file compatibility:
    python3 tools/upload_papp.py firmware/quake.papp --port /dev/ttyUSB0
    python3 tools/upload_papp.py assets/tracklist.cfg \
        --dest /sd/roms/quake/id1/tracklist.cfg

Batch uploads:
    python3 tools/upload_papp.py SDcard/roms --dest /sd --port /dev/ttyUSB0
    python3 tools/upload_papp.py file1 file2 directory --dest /sd

Directories are walked recursively and their basename is preserved below
--dest.  The serial connection stays open for the entire batch.  The device
creates missing parent directories and accepts any non-empty file up to 68 MiB.
"""

import argparse
import sys
import time
from pathlib import Path

try:
    import serial
except ImportError:
    print("Error: pyserial is not installed", file=sys.stderr)
    print("Install it with: python3 -m pip install pyserial", file=sys.stderr)
    raise SystemExit(1)


ACK = b"\x06"
MAGIC = b"PAPU"
CHUNK_SIZE = 4096
MAX_FILE_SIZE = 68 * 1024 * 1024
DEFAULT_PORT = "/dev/ttyUSB0"
DEFAULT_BAUD = 115200
DEFAULT_SINGLE_DEST = "/sd/roms/papp"


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
        """Wait for an ACK-prefixed response while ignoring ESP_LOG text."""
        deadline = time.time() + timeout
        patterns = [(ACK + prefix.encode("ascii"), prefix) for prefix in prefixes]
        while time.time() < deadline:
            for pattern, prefix in patterns:
                idx = self.buf.find(pattern)
                if idx >= 0:
                    newline = self.buf.find(b"\n", idx + len(pattern))
                    if newline >= 0:
                        line = self.buf[idx + 1:newline].decode(
                            "utf-8", "replace"
                        ).strip()
                        self.buf = self.buf[newline + 1:]
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
    """Wait through the USB/CH340 reset until the launcher is listening."""
    deadline = time.time() + timeout
    buf = b""
    while time.time() < deadline:
        data = ser.read(4096)
        if data:
            buf = (buf + data)[-8192:]
            if b"Listening on UART0" in buf:
                return True
    return False


def remote_join(root, *parts):
    root = root.rstrip("/") or "/"
    suffix = "/".join(part.strip("/") for part in parts)
    return (root if root != "/" else "") + "/" + suffix


def collect_files(sources, dest_root, exact_single_file_dest=None):
    result = []
    for source_name in sources:
        source = Path(source_name).resolve()
        if source.is_file():
            if exact_single_file_dest is not None:
                destination = exact_single_file_dest
            else:
                destination = remote_join(dest_root, source.name)
            result.append((source, destination))
            continue
        if source.is_dir():
            for path in sorted(p for p in source.rglob("*") if p.is_file()):
                relative = path.relative_to(source).as_posix()
                result.append(
                    (path, remote_join(dest_root, source.name, relative))
                )
            continue
        raise SystemExit(f"source does not exist: {source_name}")
    return result


def upload_one(proto, ser, local_path, destination):
    size = local_path.stat().st_size
    if size == 0 or size > MAX_FILE_SIZE:
        raise SystemExit(f"invalid file size for {local_path}: {size}")
    if len(destination.encode("utf-8")) >= 256:
        raise SystemExit(f"destination path is too long: {destination}")

    header = (
        MAGIC
        + destination.encode("utf-8")
        + b"\n"
        + str(size).encode("ascii")
        + b"\n"
    )
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
    parser.add_argument(
        "--dest",
        default=None,
        help=(
            "single file: complete destination path; multiple files/directories: "
            "SD destination root (default single-file path is /sd/roms/papp)"
        ),
    )
    parser.add_argument("--port", default=DEFAULT_PORT)
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    args = parser.parse_args()

    first_source = Path(args.sources[0])
    one_file = len(args.sources) == 1 and first_source.is_file()
    if one_file:
        source_name = first_source.resolve().name
        if args.dest is None:
            exact_dest = remote_join(DEFAULT_SINGLE_DEST, source_name)
            dest_root = DEFAULT_SINGLE_DEST
        else:
            exact_dest = args.dest
            dest_root = "/sd"
    else:
        exact_dest = None
        dest_root = args.dest or "/sd"

    files = collect_files(args.sources, dest_root, exact_dest)
    if not files:
        raise SystemExit("no files found")

    print(f"{len(files)} file(s) queued for {args.port}")
    ser = serial.Serial()
    ser.port = args.port
    ser.baudrate = args.baud
    ser.timeout = 0.2
    # Do not let opening the CH340 toggle DTR/RTS unexpectedly.
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

    print("upload complete")


if __name__ == "__main__":
    try:
        main()
    except serial.SerialException as exc:
        print(f"serial error: {exc}", file=sys.stderr)
        raise SystemExit(1)
