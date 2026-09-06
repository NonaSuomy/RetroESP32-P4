#!/usr/bin/env python3
"""Record or capture the optional PAPP diagnostic screen stream.

Examples:
  python tools/record_screen.py 10.20.30.180 --screenshot screen.png
  python tools/record_screen.py 10.20.30.180 --duration 10 --output papp.mp4
"""

from __future__ import annotations

import argparse
import socket
import struct
import subprocess
import sys
import time
import tempfile
import wave
from pathlib import Path

from PIL import Image


HEADER = struct.Struct("<8sHHII")
AUDIO_HEADER = struct.Struct("<8sIHHII")


def read_exact(sock: socket.socket, size: int) -> bytes:
    data = bytearray()
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise ConnectionError("ESP32 closed the screen stream")
        data.extend(chunk)
    return bytes(data)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("host")
    parser.add_argument("--port", type=int, default=3232)
    parser.add_argument("--duration", type=float, default=10.0)
    parser.add_argument("--output", type=Path, help="MP4 output file")
    parser.add_argument("--screenshot", type=Path, help="Save the first received frame as PNG")
    parser.add_argument("--fullscreen", action="store_true", help="Record video at the 1024x600 display size")
    parser.add_argument("--fullscreen-screenshot", type=Path, help="Save the first frame padded to 1024x600")
    parser.add_argument("--rotation", type=int, choices=(0, 90, 180, 270), default=180,
                        help="Rotate saved captures counter-clockwise; default: 180")
    args = parser.parse_args()

    if args.output is None and args.screenshot is None:
        args.screenshot = Path("papp-screenshot.png")

    # The ESPHome API is the control plane. The TCP stream stays unavailable
    # until this authenticated command enables it, and the firmware disables
    # it again when this recorder disconnects.
    launcher = Path(__file__).with_name("remote_papp.py")
    subprocess.run([sys.executable, str(launcher), args.host, "record", "--no-monitor"], check=True)

    started = time.monotonic()
    frames = 0
    video_frames: list[bytes] = []
    video_size = (0, 0)
    audio = bytearray()
    audio_rate = 0
    with socket.create_connection((args.host, args.port), timeout=10) as sock:
        sock.settimeout(15)
        while time.monotonic() - started < args.duration or frames == 0:
            magic = read_exact(sock, 8)
            if magic == b"PAPPFB01":
                _, width, height, frame_bytes, _sequence = HEADER.unpack(magic + read_exact(sock, HEADER.size - 8))
                if frame_bytes != width * height * 2:
                    raise RuntimeError("invalid PAPP screen stream header")
                raw = read_exact(sock, frame_bytes)
            elif magic == b"PAPPAU01":
                _, rate, channels, bits, audio_bytes, _sequence = AUDIO_HEADER.unpack(
                    magic + read_exact(sock, AUDIO_HEADER.size - 8))
                if channels != 2 or bits != 16:
                    raise RuntimeError("unsupported PAPP audio format")
                audio_rate = rate
                audio.extend(read_exact(sock, audio_bytes))
                continue
            else:
                raise RuntimeError(f"invalid PAPP stream packet: {magic!r}")
            # The panel path is rotated 180 degrees; normalize captures for
            # host-side inspection and video playback.
            image = Image.frombytes("RGB", (width, height), raw, "raw", "BGR;16").rotate(args.rotation)
            if frames == 0 and args.screenshot is not None:
                args.screenshot.parent.mkdir(parents=True, exist_ok=True)
                image.save(args.screenshot)
                print(f"Screenshot saved to {args.screenshot}")
            if frames == 0 and args.fullscreen_screenshot is not None:
                full = Image.new("RGB", (1024, 600), "black")
                full_papp = image.copy()
                full_papp.thumbnail((800, 480), Image.Resampling.NEAREST)
                full.paste(full_papp, ((1024 - full_papp.width) // 2, (600 - full_papp.height) // 2))
                args.fullscreen_screenshot.parent.mkdir(parents=True, exist_ok=True)
                full.save(args.fullscreen_screenshot)
                print(f"Fullscreen screenshot saved to {args.fullscreen_screenshot}")
            video_image = image
            if args.fullscreen:
                video_image = Image.new("RGB", (1024, 600), "black")
                full_papp = image.copy()
                full_papp.thumbnail((800, 480), Image.Resampling.NEAREST)
                video_image.paste(full_papp, ((1024 - full_papp.width) // 2, (600 - full_papp.height) // 2))
            video_size = video_image.size
            video_frames.append(video_image.tobytes())
            frames += 1
    if args.output is not None and video_frames:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="papp-record-") as temp_dir:
            video_raw = Path(temp_dir) / "video.rgb"
            video_raw.write_bytes(b"".join(video_frames))
            command = ["ffmpeg", "-y", "-f", "rawvideo", "-pix_fmt", "rgb24",
                       "-s", f"{video_size[0]}x{video_size[1]}", "-r", "5", "-i", str(video_raw)]
            if audio and audio_rate:
                audio_wav = Path(temp_dir) / "audio.wav"
                with wave.open(str(audio_wav), "wb") as wav:
                    wav.setnchannels(2)
                    wav.setsampwidth(2)
                    wav.setframerate(audio_rate)
                    wav.writeframes(audio)
                command += ["-i", str(audio_wav), "-c:v", "libx264", "-pix_fmt", "yuv420p",
                            "-c:a", "aac", "-map", "0:v:0", "-map", "1:a:0",
                            "-t", f"{max(frames / 5.0, 0.2):.3f}", str(args.output)]
            else:
                command += ["-an", "-c:v", "libx264", "-pix_fmt", "yuv420p", str(args.output)]
            result = subprocess.run(command)
            if result.returncode != 0:
                raise RuntimeError("ffmpeg failed to encode the recording")
        print(f"Recording saved to {args.output} ({frames} frames, audio={bool(audio)})")


if __name__ == "__main__":
    main()
