#!/usr/bin/env python3
"""Record real device display frames while replaying a JSON navigation plan.

Requires BIBLE_SCREENSHOTS=1 firmware, pyserial and ffmpeg. One serial connection
owns both input commands and video. Frames retain device timestamps; 30fps export
repeats existing frames, with no generated/interpolated animation. No audio capture.
Plan actions: tap [x,y], swipe [x1,y1,x2,y2], scroll y, scene name, wait seconds.
Optional expect_page checks firmware navigation logs. See RECORDING.md.
"""
import argparse
import base64
import json
import queue
import re
import struct
import subprocess
import threading
import time
import zlib
from pathlib import Path

from capture_gallery import Device
from screenshot_server import rgb565be_to_png

HEADER = re.compile(rb"<<<FRAME (\d+) (\d+) (\d+)>>>")
END = re.compile(rb"<<<ENDFRAME (\d+)>>>")
STOP = re.compile(rb"<<<VIDEO STOP (\d+) (\d+) (\d+)>>>")
PAGE = re.compile(rb"UIPERF page=(\d+) ctx=(-?\d+)")


def decode_frame(encoded):
    encoded = zlib.decompress(encoded)
    if len(encoded) % 4 or len(encoded) > 360 * 360 * 4:
        raise ValueError("Invalid RLE length")
    runs = list(struct.iter_unpack(">H2s", encoded))
    if any(n == 0 for n, _ in runs) or sum(n for n, _ in runs) != 360 * 360:
        raise ValueError("Invalid RLE pixel count")
    return b"".join(pixel * n for n, pixel in runs)


class Recorder:
    def __init__(self, port, out):
        self.out = out
        (out / "frames").mkdir(parents=True, exist_ok=False)
        self.log = (out / "serial.log").open("w")
        self.device = Device(port, self.log)
        self.device.ready()
        self.device.ser.timeout = 0.05
        self.responses = queue.Queue()
        self.frames, self.actions = [], []
        self.page = None
        self.error = None
        self.closed = False
        self.reader = threading.Thread(target=self.read, daemon=True)
        self.reader.start()

    def read(self):
        buf, payload, header = bytearray(), bytearray(), None
        try:
            while not self.closed:
                buf.extend(self.device.ser.read(self.device.ser.in_waiting or 1))
                while b"\n" in buf:
                    line, _, buf = buf.partition(b"\n")
                    line = bytes(line).rstrip(b"\r")
                    if line.startswith(b"V:"):
                        if header is None:
                            raise ValueError("Video payload without header")
                        payload.extend(base64.b64decode(line[2:], validate=True))
                        if len(payload) > header[2]:
                            raise ValueError("Oversized frame payload")
                        continue
                    self.log.write(line.decode(errors="replace") + "\n")
                    self.log.flush()
                    if any(marker in line for marker in (b"ESP-ROM:", b"stack overflow", b"Guru Meditation", b"Video compression failed")):
                        raise RuntimeError("Device reset or crashed during recording")
                    if m := HEADER.search(line):
                        if header is not None:
                            raise ValueError("Incomplete video frame")
                        header = tuple(map(int, m.groups()))
                        if not 0 < header[2] <= 360 * 360 * 4 + 4096:
                            raise ValueError("Invalid compressed frame size")
                        if header[0] != len(self.frames) or (self.frames and header[1] < self.frames[-1]["ms"]):
                            raise ValueError("Out-of-order frame or timestamp")
                        payload = bytearray()
                    elif m := END.search(line):
                        if header is None or int(m[1]) != header[0] or len(payload) != header[2]:
                            raise ValueError("Video frame boundary/length mismatch")
                        decode_frame(payload) # validate before accepting the frame
                        name = f"frames/{header[0]:06d}.rlez"
                        (self.out / name).write_bytes(payload)
                        self.frames.append({"index": header[0], "ms": header[1], "file": name})
                        header = None
                    elif PAGE.search(line):
                        self.page = int(PAGE.search(line)[1])
                    elif line.startswith(b"<<<"):
                        self.responses.put(line)
        except Exception as exc:
            self.error = exc

    def command(self, text, prefix, timeout=35):
        self.device.send(text)
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            if self.error:
                raise RuntimeError("Video reader failed") from self.error
            try:
                line = self.responses.get(timeout=0.1)
            except queue.Empty:
                continue
            if b"ERROR" in line or b"UNAVAILABLE" in line:
                raise RuntimeError(f"{text}: {line!r}")
            if line.startswith(prefix):
                return line
        raise TimeoutError(text)

    def touch(self, x, y, down):
        self.command(f"TOUCH {x} {y} {int(down)}", b"<<<INPUT")

    def action(self, action):
        label = action.get("label", "")
        if label:
            print(label, flush=True)
        self.actions.append({**action, "ms": round((time.monotonic() - self.started) * 1000)})
        if "tap" in action:
            x, y = action["tap"]
            self.touch(x, y, True)
            time.sleep(0.10)
            self.touch(x, y, False)
        elif "swipe" in action:
            x1, y1, x2, y2 = action["swipe"]
            self.touch(x1, y1, True)
            for step in range(1, 9):
                time.sleep(0.04)
                self.touch(round(x1 + (x2 - x1) * step / 8), round(y1 + (y2 - y1) * step / 8), True)
            self.touch(x2, y2, False)
        elif "scroll" in action:
            self.command(f'GLIDE {action["scroll"]}', b"<<<UI")
        elif "scene" in action:
            self.command(f'SCENE {action["scene"]}', b"<<<UI")
        time.sleep(action.get("wait", 1.5))
        if "expect_page" in action and self.page != action["expect_page"]:
            raise RuntimeError(f'{label}: expected page {action["expect_page"]}, got {self.page}')

    def run(self, plan):
        active = False
        try:
            self.command("SCENE home", b"<<<UI")
            self.command("VIDEO START", b"<<<VIDEO START")
            self.started = time.monotonic()
            active = True
            for action in plan:
                self.action(action)
            end = self.command("VIDEO STOP", b"<<<VIDEO STOP", timeout=60)
            active = False
            count, dropped, elapsed = map(int, STOP.search(end).groups())
            if count != len(self.frames) or not count:
                raise RuntimeError("Frame count mismatch")
            metadata = {"width": 360, "height": 360, "duration_ms": elapsed,
                        "source": "Actual ESP32 LCD flush frames with device timestamps",
                        "audio": False, "frames": self.frames, "actions": self.actions,
                        "captured_frames": count, "dropped_frames": dropped}
            (self.out / "recording.json").write_text(json.dumps(metadata, indent=2) + "\n")
            print(f"Captured {count} frames; {dropped} dropped; {elapsed/1000:.2f}s", flush=True)
            return metadata
        finally:
            if active:
                try:
                    self.command("VIDEO STOP", b"<<<VIDEO STOP", timeout=60)
                except Exception:
                    pass
            self.closed = True
            self.reader.join(timeout=2)
            self.device.ser.close()
            self.log.close()


def export(out, metadata, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    frames = metadata["frames"]
    cmd = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "rawvideo",
           "-pixel_format", "rgb565be", "-video_size", "360x360", "-framerate", "30",
           "-i", "-", "-an", "-c:v", "libx264", "-preset", "slow", "-crf", "18",
           "-pix_fmt", "yuv420p", "-movflags", "+faststart", str(destination)]
    process = subprocess.Popen(cmd, stdin=subprocess.PIPE)
    idx, raw = -1, None
    try:
        for tick in range((metadata["duration_ms"] * 30 + 999) // 1000):
            ms = tick * 1000 / 30
            next_idx = max(idx, 0)
            while next_idx + 1 < len(frames) and frames[next_idx + 1]["ms"] <= ms:
                next_idx += 1
            if idx != next_idx:
                idx = next_idx
                raw = decode_frame((out / frames[idx]["file"]).read_bytes())
            process.stdin.write(raw)
        process.stdin.close()
        if process.wait() != 0:
            raise RuntimeError("ffmpeg export failed")
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
    first = decode_frame((out / frames[0]["file"]).read_bytes())
    destination.with_suffix(".png").write_bytes(rgb565be_to_png(first, 360, 360))
    destination.with_suffix(".json").write_text(json.dumps(
        {key: value for key, value in metadata.items() if key != "frames"}, indent=2) + "\n")
    print(f"Exported {destination}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port")
    parser.add_argument("plan", type=Path)
    parser.add_argument("out", type=Path)
    parser.add_argument("destination", type=Path)
    parser.add_argument("--export-only", action="store_true")
    args = parser.parse_args()
    if args.export_only:
        metadata = json.loads((args.out / "recording.json").read_text())
    else:
        args.out.mkdir(parents=True, exist_ok=True)
        metadata = Recorder(args.port, args.out).run(json.loads(args.plan.read_text()))
    export(args.out, metadata, args.destination)


if __name__ == "__main__":
    main()
