#!/usr/bin/env python3
"""Capture real 360x360 screenshots off the device, on demand, over the USB console.

Requires firmware built with screenshots enabled:

    BIBLE_SCREENSHOTS=1 idf.py build flash
    python tools/screenshot_server.py /dev/cu.usbmodemXXXX out ctl

Then drive the UI by hand and ask for a frame whenever the screen shows what you want:

    echo my-screen > ctl/req     ->  writes out/my-screen.png

Holds ONE serial connection open for the whole session. That matters: on this
USB-Serial-JTAG port an open/close cycle pulses EN, so reconnecting per capture reset
the board and snapped the UI back to Home, losing whatever you had navigated to.
Opening once costs exactly one reset, at startup, and it reconnects if the port
re-enumerates (which happens on every flash).

Pixels arrive as base64 on "S:"-prefixed lines, so interleaved ESP_LOG output is
ignored rather than corrupting a frame. They are RGB565 big-endian (LV_COLOR_16_SWAP).
PNG is written with stdlib zlib only - no Pillow dependency.
"""
import base64
import re
import struct
import sys
import time
import zlib
from pathlib import Path

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbmodem2101"
OUTDIR = Path(sys.argv[2] if len(sys.argv) > 2 else "shots")
CTLDIR = Path(sys.argv[3] if len(sys.argv) > 3 else "ctl")

START_RE = re.compile(rb"<<<SHOT (\S+) (\d+) (\d+) (\d+)>>>")
END_RE = re.compile(rb"<<<ENDSHOT (\S+)>>>")


def rgb565be_to_png(raw: bytes, w: int, h: int) -> bytes:
    rows = bytearray()
    for y in range(h):
        rows.append(0)
        off = y * w * 2
        for x in range(w):
            v = (raw[off + x * 2] << 8) | raw[off + x * 2 + 1]
            r, g, b = (v >> 11) & 0x1F, (v >> 5) & 0x3F, v & 0x1F
            rows += bytes(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)))

    def chunk(tag: bytes, data: bytes) -> bytes:
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(rows), 6))
            + chunk(b"IEND", b""))


def log(ctl_log: Path, msg: str) -> None:
    with ctl_log.open("a") as f:
        f.write(msg + "\n")
    print(msg, flush=True)


def open_port(settle: float = 2.0):
    """Open without asserting DTR/RTS, so the board is not reset."""
    ser = serial.Serial()
    ser.port = PORT
    ser.baudrate = 115200
    ser.timeout = 0.1
    ser.dtr = False
    ser.rts = False
    ser.open()
    time.sleep(settle)
    ser.reset_input_buffer()
    return ser


def main() -> int:
    OUTDIR.mkdir(parents=True, exist_ok=True)
    CTLDIR.mkdir(parents=True, exist_ok=True)
    req_file, ctl_log = CTLDIR / "req", CTLDIR / "log"

    ser = open_port()
    log(ctl_log, "READY - navigate the device, then drop a name in ctl/req")

    def reconnect():
        """The USB-Serial-JTAG port disappears and re-enumerates on every flash and
        reset; wait for it to come back instead of exiting."""
        nonlocal ser
        try:
            ser.close()
        except Exception:
            pass
        for _ in range(120):
            try:
                ser = open_port(1.0)
                log(ctl_log, "RECONNECTED")
                return True
            except Exception:
                time.sleep(1.0)
        log(ctl_log, "FAILED to reconnect")
        return False

    buf = b""
    while True:
        # Drain continuously between captures so the board's console output cannot
        # back up and stall its logging task.
        try:
            data = ser.read(65536)
        except Exception:
            if not reconnect():
                return 1
            continue
        if data:
            buf = (buf + data)[-4096:]

        if not req_file.exists():
            time.sleep(0.1)
            continue

        try:
            name = req_file.read_text().strip() or "manual"
        except OSError:
            continue
        req_file.unlink(missing_ok=True)

        try:
            ser.reset_input_buffer()
            ser.write(f"SHOT {name}\n".encode())
            ser.flush()
        except Exception:
            log(ctl_log, f"FAIL {name}: port lost")
            reconnect()
            continue

        cur, payload, rbuf = None, bytearray(), b""
        deadline = time.time() + 30
        while time.time() < deadline:
            try:
                d = ser.read(65536)
            except Exception:
                break
            if not d:
                continue
            rbuf += d
            done = False
            while b"\n" in rbuf:
                line, rbuf = rbuf.split(b"\n", 1)
                line = line.strip(b"\r")
                if cur is None:
                    m = START_RE.search(line)
                    if m:
                        cur = (m.group(1).decode(), int(m.group(2)),
                               int(m.group(3)), int(m.group(4)))
                    continue
                if line.startswith(b"S:"):
                    try:
                        payload += base64.b64decode(line[2:], validate=True)
                    except Exception:
                        pass
                    continue
                if END_RE.search(line):
                    done = True
                    break
            if done:
                break

        if cur is None:
            log(ctl_log, f"FAIL {name}: no response from device")
            continue
        _, w, h, want = cur
        if len(payload) != want:
            log(ctl_log, f"FAIL {name}: got {len(payload)} of {want} bytes")
            continue
        out = OUTDIR / f"{name}.png"
        out.write_bytes(rgb565be_to_png(bytes(payload), w, h))
        log(ctl_log, f"OK {name} -> {out}")


if __name__ == "__main__":
    sys.exit(main())
