#!/usr/bin/env python3
"""Capture every UI route from a connected screenshot-enabled Shema build.

Run as a finite job, keeping one serial connection for the entire gallery:
  python tools/capture_gallery.py PORT OUTDIR

Uses the card's real contents. Never clicks settings, clears history, or edits
favourites. Repeated data rows use top/middle/bottom samples; every settings
section and every option in the scrolling dialogs is captured.
"""
import argparse
import base64
import html
import json
import re
import time
from pathlib import Path

import serial

from screenshot_server import rgb565be_to_png, START_RE, END_RE

from capture_catalog import SCENES, ALL_SCROLLS

UI_RE = re.compile(rb"<<<UI (OK|UNAVAILABLE) (-?\d+) (-?\d+) (\d+)>>>")


class Device:
    def __init__(self, port, log):
        self.log = log
        self.ser = serial.Serial()
        self.ser.port = port
        self.ser.baudrate = 115200
        self.ser.timeout = 0.2
        self.ser.write_timeout = 5
        self.ser.dtr = self.ser.rts = False
        self.ser.open()
        self.buf = bytearray()

    def lines(self, seconds):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            self.buf.extend(self.ser.read(self.ser.in_waiting or 1))
            while b"\n" in self.buf:
                line, _, rest = self.buf.partition(b"\n")
                self.buf = bytearray(rest)
                line = bytes(line).rstrip(b"\r")
                if not line.startswith(b"S:"):
                    self.log.write(line.decode(errors="replace") + "\n")
                    self.log.flush()
                yield line

    def send(self, text):
        self.ser.write((text + "\n").encode("ascii"))
        self.ser.flush()

    def ready(self):
        self.send("PING")
        for line in self.lines(25):
            if line == b"<<<SHOT READY>>>":
                return
            if b"<<<SHOT READY>>>" in line:
                # The boot announcement means stdin has just been installed.
                # Confirm a round trip: the first pre-boot PING can be discarded.
                self.send("PING")
        raise RuntimeError("No screenshot READY response; flash with BIBLE_SCREENSHOTS=1 first")

    def ui(self, command):
        self.send(command)
        for line in self.lines(35):
            m = UI_RE.search(line)
            if m:
                return m[1] == b"OK", *(int(v) for v in m.groups()[1:])
        raise RuntimeError(f"No UI acknowledgement for {command}")

    def shot(self, name, dest):
        self.send("SHOT " + name)
        header = None
        raw = bytearray()
        for line in self.lines(40):
            if header is None:
                header = START_RE.search(line)
                if header and header[1].decode() != name:
                    raise RuntimeError("Screenshot response name mismatch")
            elif line.startswith(b"S:"):
                raw.extend(base64.b64decode(line[2:], validate=True))
            elif END_RE.search(line):
                if END_RE.search(line)[1].decode() != name:
                    raise RuntimeError("Screenshot end name mismatch")
                w, h, want = (int(v) for v in header.groups()[1:])
                if (w, h) != (360, 360) or len(raw) != want or want != w * h * 2:
                    raise RuntimeError(f"Incomplete frame: {len(raw)} bytes, expected {want}")
                dest.write_bytes(rgb565be_to_png(raw, w, h))
                return
        raise RuntimeError(f"Screenshot timed out: {name}")


def gallery(out, records):
    order = {scene: i for i, scene in enumerate(SCENES)}
    records.sort(key=lambda r: (order.get(r["scene"], len(order)), r.get("scroll_y", 0)))
    (out / "manifest.json").write_text(json.dumps(records, indent=2) + "\n")
    figures = []
    for r in records:
        if "file" in r:
            label = html.escape(r["label"])
            figures.append(f'<figure><a href="{r["file"]}"><img src="{r["file"]}" '
                           f'alt="{label}" loading="lazy"></a><figcaption>{label}</figcaption></figure>')
        else:
            figures.append(f'<p>{html.escape(r["scene"])}: {html.escape(r["status"])}</p>')
    (out / "index.html").write_text('''<!doctype html><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Shema · Device screenshots</title><style>
body{background:#080d13;color:#f7f1e6;font:16px system-ui;margin:32px}
h1{font-size:28px}p{color:#afbdc7;max-width:850px;line-height:1.6}
main{display:grid;grid-template-columns:repeat(auto-fill,minmax(300px,1fr));gap:28px}
figure{margin:0;text-align:center}img{width:100%;max-width:360px;border-radius:50%}
figcaption{padding:12px;color:#afbdc7}a{color:#e8be78}
</style><h1>Shema · Device screenshots</h1>
<p>Real 360 × 360 framebuffer captures from the connected ESP32, using its existing
SD card and saved data. Images are masked to the round display here; click to view
the full, unmodified PNG. Long content lists show representative scroll positions;
settings and dialogs include all sections. Screens labelled Preview use sample
sample state on the real device to show empty/no-card/playback variants without
changing saved data. Splash re-displays the actual boot asset. Unavailable routes
are listed explicitly.</p>
<main>''' + "\n".join(figures) + "</main>")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port")
    parser.add_argument("out", type=Path)
    parser.add_argument("--scenes", nargs="+", default=SCENES)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    manifest = args.out / "manifest.json"
    records = json.loads(manifest.read_text()) if manifest.exists() else []
    records = [r for r in records if r["scene"] not in args.scenes]
    with (args.out / "serial.log").open("a") as log:
        device = Device(args.port, log)
        try:
            device.ready()
            for scene in args.scenes:
                ok, y, maximum, height = device.ui("SCENE " + scene)
                if not ok:
                    records.append({"scene": scene, "status": "Unavailable with the current card contents"})
                    print("UNAVAILABLE", scene, flush=True)
                    continue
                if maximum > 0:
                    if scene in ALL_SCROLLS:
                        offsets = list(range(0, maximum, max(40, height - 55))) + [maximum]
                    else:
                        offsets = sorted({0, maximum // 2, maximum})
                else:
                    offsets = [0]
                for i, offset in enumerate(offsets):
                    if maximum > 0:
                        _, y, _, _ = device.ui(f"SCROLL {offset}")
                    name = f"{scene}-{i + 1:02d}" if len(offsets) > 1 else scene
                    dest = args.out / (name + ".png")
                    device.shot(name, dest)
                    records.append({"scene": scene, "file": dest.name, "scroll_y": y,
                                    "fixture": scene.startswith("preview-"),
                                    "label": name.replace("-", " ").title(), "status": "captured"})
                    gallery(args.out, records)
                    print(f"CAPTURED {dest.name} (scroll {y}/{maximum})", flush=True)
            device.ui("SCENE home")
        finally:
            gallery(args.out, records)
            device.ser.close()
    print(f"DONE: {sum('file' in r for r in records)} screenshots → {args.out / 'index.html'}", flush=True)


if __name__ == "__main__":
    main()
