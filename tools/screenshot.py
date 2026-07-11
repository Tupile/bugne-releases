#!/usr/bin/env python3
# Fetch GET /api/screenshot from a Bugne device and save it as a PNG.
#
# The device returns a 16bpp BI_BITFIELDS top-down BMP (RGB565, masks
# F800/07E0/001F). This converts it to RGB888 and writes a PNG with only the
# standard library (no PIL): a single zlib-compressed IDAT, filter 0 per row.
#
# Usage: python3 tools/screenshot.py <ip> <out.png> [--login <password>]
# The bench has no config password, so --login is optional.
import sys
import struct
import zlib
import json
import urllib.request


def fetch(ip, out_path, password=None):
    opener = urllib.request.build_opener(urllib.request.HTTPCookieProcessor())

    if password:
        body = json.dumps({"pass": password}).encode()
        req = urllib.request.Request("http://%s/login" % ip, data=body,
                                     headers={"Content-Type": "application/json"})
        opener.open(req, timeout=10).read()

    resp = opener.open("http://%s/api/screenshot" % ip, timeout=10)
    data = resp.read()
    if resp.headers.get("Content-Type", "").split(";")[0] != "image/bmp":
        raise SystemExit("not a BMP (status/body: %r)" % data[:120])

    png = bmp565_to_png(data)
    with open(out_path, "wb") as f:
        f.write(png)
    print("wrote %s" % out_path)


def bmp565_to_png(bmp):
    if bmp[:2] != b"BM":
        raise SystemExit("bad BMP magic")
    off_bits = struct.unpack_from("<I", bmp, 10)[0]
    width = struct.unpack_from("<i", bmp, 18)[0]
    height = struct.unpack_from("<i", bmp, 22)[0]
    bpp = struct.unpack_from("<H", bmp, 28)[0]
    if bpp != 16:
        raise SystemExit("expected 16bpp, got %d" % bpp)
    top_down = height < 0
    h = abs(height)
    w = width
    row_bytes = w * 2  # 16bpp; already 4-byte aligned for w=240 and 320

    # Expand each RGB565 pixel to RGB888 and prepend the PNG filter byte (0).
    raw = bytearray()
    for y in range(h):
        src_y = y if top_down else (h - 1 - y)
        base = off_bits + src_y * row_bytes
        raw.append(0)  # filter type 0 (None)
        for x in range(w):
            px = bmp[base + x * 2] | (bmp[base + x * 2 + 1] << 8)
            r = (px >> 11) & 0x1F
            g = (px >> 5) & 0x3F
            b = px & 0x1F
            raw.append((r << 3) | (r >> 2))
            raw.append((g << 2) | (g >> 4))
            raw.append((b << 3) | (b >> 2))

    return build_png(w, h, bytes(raw))


def png_chunk(tag, data):
    out = struct.pack(">I", len(data)) + tag + data
    return out + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)


def build_png(w, h, raw_rgb):
    sig = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)  # 8-bit, color type 2 (RGB)
    idat = zlib.compress(raw_rgb, 9)
    return sig + png_chunk(b"IHDR", ihdr) + png_chunk(b"IDAT", idat) + png_chunk(b"IEND", b"")


def main():
    args = sys.argv[1:]
    password = None
    if "--login" in args:
        i = args.index("--login")
        password = args[i + 1]
        del args[i:i + 2]
    if len(args) != 2:
        raise SystemExit("usage: screenshot.py <ip> <out.png> [--login <password>]")
    fetch(args[0], args[1], password)


if __name__ == "__main__":
    main()
