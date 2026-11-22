#!/usr/bin/env python3
"""Turn a digicore-c3 serial log into PNGs of the guest framebuffer.

The firmware prints exactly one `SHOT_BEGIN W H` ... `SHOT_END` block right
after the first non-blank guest frame (see dump_frame_hex() in
firmware/digicore-c3/main/app_main.cpp).  Each pixel is four hex digits of
RGB565, one row per line.

Usage:
  python tools/c3_shot.py dbgtest/c3_serial3.log docs/c3_device_boot.png
"""
import sys

from PIL import Image


def parse(path):
    text = open(path, "rb").read().decode("utf-8", "replace")
    i = text.find("SHOT_BEGIN")
    if i < 0:
        raise SystemExit("no SHOT_BEGIN in %s" % path)
    head = text[i:].splitlines()[0].split()
    w, h = int(head[1]), int(head[2])
    rows = []
    for line in text[i:].splitlines()[1:]:
        line = line.strip()
        if line.startswith("SHOT_END"):
            break
        if len(line) < w * 4:
            continue
        rows.append([int(line[k * 4:k * 4 + 4], 16) for k in range(w)])
    if len(rows) < h:
        raise SystemExit("only %d of %d rows in the shot block" % (len(rows), h))
    return rows


def to_image(rows):
    h, w = len(rows), len(rows[0])
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        for x in range(w):
            c = rows[y][x]
            r = ((c >> 11) & 0x1F) * 255 // 31
            g = ((c >> 5) & 0x3F) * 255 // 63
            b = (c & 0x1F) * 255 // 31
            px[x, y] = (r, g, b)
    return img


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    img = to_image(parse(sys.argv[1]))
    img.save(sys.argv[2])
    print("saved %s (%dx%d)" % (sys.argv[2], img.width, img.height))
    return 0


if __name__ == "__main__":
    sys.exit(main())
