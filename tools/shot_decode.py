import re
import struct
import sys
import zlib

src = sys.argv[1]
out = sys.argv[2]

width = height = None
pixels = None
seen_end = False

with open(src, "r", encoding="utf-8", errors="replace") as f:
    for line in f:
        line = line.strip()
        if line.startswith("SHOT_BEGIN"):
            parts = line.split()
            width, height = int(parts[1]), int(parts[2])
            pixels = [[0] * width for _ in range(height)]
        elif line.startswith("SHOT_ROW") and pixels is not None:
            parts = line.split()
            y, x, n = int(parts[1]), int(parts[2]), int(parts[3])
            color = int(parts[4], 16)
            r = ((color >> 11) & 0x1F) * 255 // 31
            g = ((color >> 5) & 0x3F) * 255 // 63
            b = (color & 0x1F) * 255 // 31
            rgb = (r << 16) | (g << 8) | b
            for i in range(n):
                if 0 <= y < height and 0 <= x + i < width:
                    pixels[y][x + i] = rgb
        elif line.startswith("SHOT_END"):
            seen_end = True

if pixels is None:
    raise SystemExit("no SHOT_BEGIN in " + src)
if not seen_end:
    print("warning: no SHOT_END; image may be partial")

def chunk(tag, data):
    return (struct.pack(">I", len(data)) + tag + data +
            struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

raw = bytearray()
for row in pixels:
    raw.append(0)
    for rgb in row:
        raw.append((rgb >> 16) & 0xFF)
        raw.append((rgb >> 8) & 0xFF)
        raw.append(rgb & 0xFF)

png = b"\x89PNG\r\n\x1a\n"
png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
png += chunk(b"IEND", b"")
with open(out, "wb") as f:
    f.write(png)
print("wrote", out, width, "x", height, "end=", seen_end)
