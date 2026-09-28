#!/usr/bin/env python3
"""Convert a 360x360 PNG into raw RGB565 for LVGL, stdlib only (no Pillow here).

Byte order matches the panel: sdkconfig sets LV_COLOR_16_SWAP=y, so lv_color_t holds
RGB565 high-byte-first — the same order the screenshot path decodes.

usage: png_to_rgb565.py in.png out.bin [expected_w] [expected_h]
"""
import struct
import sys
import zlib
from pathlib import Path

SRC = Path(sys.argv[1])
DST = Path(sys.argv[2])
EXP_W = int(sys.argv[3]) if len(sys.argv) > 3 else 360
EXP_H = int(sys.argv[4]) if len(sys.argv) > 4 else 360


def read_png(path: Path):
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit("not a PNG")
    pos, idat, w = 8, bytearray(), None
    h = bits = ctype = interlace = None
    while pos < len(data):
        (ln,) = struct.unpack(">I", data[pos:pos + 4])
        tag = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + ln]
        if tag == b"IHDR":
            w, h, bits, ctype, _, _, interlace = struct.unpack(">IIBBBBB", body)
        elif tag == b"IDAT":
            idat += body
        elif tag == b"IEND":
            break
        pos += 12 + ln
    if bits != 8 or interlace != 0 or ctype not in (2, 6):
        raise SystemExit(f"need 8-bit non-interlaced RGB/RGBA, got bits={bits} ctype={ctype}")
    return w, h, ctype, zlib.decompress(bytes(idat))


def unfilter(raw: bytes, w: int, h: int, nch: int):
    """Undo the per-scanline PNG filters (spec 9.2)."""
    stride = w * nch
    out = bytearray(stride * h)
    prev = bytearray(stride)
    pos = 0
    for y in range(h):
        ft = raw[pos]; pos += 1
        line = bytearray(raw[pos:pos + stride]); pos += stride
        if ft == 1:
            for i in range(nch, stride):
                line[i] = (line[i] + line[i - nch]) & 0xFF
        elif ft == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif ft == 3:
            for i in range(stride):
                left = line[i - nch] if i >= nch else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 0xFF
        elif ft == 4:
            for i in range(stride):
                a = line[i - nch] if i >= nch else 0
                b = prev[i]
                c = prev[i - nch] if i >= nch else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        elif ft != 0:
            raise SystemExit(f"bad filter {ft}")
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return out


def main() -> int:
    w, h, ctype, raw = read_png(SRC)
    if (w, h) != (EXP_W, EXP_H):
        raise SystemExit(f"expected {EXP_W}x{EXP_H}, got {w}x{h}")
    nch = 3 if ctype == 2 else 4
    px = unfilter(raw, w, h, nch)

    out = bytearray(w * h * 2)
    for i in range(w * h):
        r, g, b = px[i * nch], px[i * nch + 1], px[i * nch + 2]
        v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        out[i * 2] = (v >> 8) & 0xFF      # high byte first (LV_COLOR_16_SWAP)
        out[i * 2 + 1] = v & 0xFF
    DST.write_bytes(bytes(out))
    print(f"{SRC.name} {w}x{h} {nch}ch -> {DST} ({len(out)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
