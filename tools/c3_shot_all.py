"""Decode every SHOT_BEGIN..SHOT_END block in a C3 serial capture into PNGs.

The firmware prints "SHOT_BEGIN W H" followed by H lines of W four-digit hex
RGB565 words, then "SHOT_END".  UART line endings and a missing tail row are
tolerated: short blocks are padded with black and reported.
"""
import sys

from PIL import Image


def decode(text):
    out = []
    for idx, chunk in enumerate(text.split("SHOT_BEGIN")[1:]):
        body = chunk.split("SHOT_END")[0]
        lines = [l.strip() for l in body.splitlines()]
        rows = [l for l in lines if l and all(c in "0123456789abcdefABCDEF" for c in l) and len(l) >= 96 * 4]
        img = Image.new("RGB", (96, 96))
        px = img.load()
        for y in range(96):
            if y >= len(rows):
                break
            s = rows[y][: 96 * 4]
            for x in range(96):
                v = int(s[x * 4 : x * 4 + 4], 16)
                px[x, y] = (((v >> 11) & 0x1F) << 3, ((v >> 5) & 0x3F) << 2, (v & 0x1F) << 3)
        out.append((idx, len(rows), img))
    return out


def main():
    src = sys.argv[1]
    prefix = sys.argv[2] if len(sys.argv) > 2 else "dbgtest/c3_block"
    with open(src, "rb") as fp:
        text = fp.read().decode("utf-8", "replace")
    for idx, rows, img in decode(text):
        path = "%s%d.png" % (prefix, idx)
        img.save(path)
        print("block %d: %d rows -> %s%s" % (idx, rows, path, "" if rows == 96 else " (padded)"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
