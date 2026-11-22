#!/usr/bin/env python3
"""Build the color-preview frame blob from a MAME raw AVI.

This is a display bring-up asset, not an emulator.  Each 96x96 frame is
converted to big-endian RGB565 and zlib-compressed.  No rotation is applied:
the MAME digicolr capture is already upright (verified by rasterizing the
SET/0:00 clock UI and checking glyph orientation against the raw AVI frame).

Blob layout (little-endian):
    magic      4s   b"DCPV"
    version    u16  1
    width      u16  96
    height     u16  96
    fps        u16  playback fps
    count      u32  frame count
    data_off   u32  byte offset of frame 0
    offsets    u32  count+1 absolute offsets (last == blob length)
    payload         zlib streams
"""
import argparse
import hashlib
import json
import struct
import zlib
from pathlib import Path

from PIL import Image, ImageDraw

FRAME_SIZE = 96 * 96 * 3
HEADER = struct.Struct("<4sHHHHII")


def load_rgb565(avi: bytes, index: int) -> bytes:
    off = index * FRAME_SIZE
    if off + FRAME_SIZE > len(avi):
        raise IndexError(index)
    im = Image.frombytes("RGB", (96, 96), avi[off:off + FRAME_SIZE])
    out = bytearray(96 * 96 * 2)
    p = 0
    for r, g, b in im.getdata():
        v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
        out[p] = (v >> 8) & 0xFF
        out[p + 1] = v & 0xFF
        p += 2
    return bytes(out)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--avi", required=True, type=Path)
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--fps", type=int, default=10)
    ap.add_argument("--seconds", type=float, default=15.0)
    ap.add_argument("--source-fps", type=float, default=60.0)
    ap.add_argument("--preview", type=Path)
    args = ap.parse_args()

    avi = args.avi.read_bytes()
    total = len(avi) // FRAME_SIZE
    step = max(1, round(args.source_fps / args.fps))
    count = min(int(args.seconds * args.fps), (total + step - 1) // step)
    if count < 1:
        raise SystemExit("no frames")

    frames = []
    raw_hashes = []
    for i in range(count):
        src = min(total - 1, i * step)
        raw = load_rgb565(avi, src)
        raw_hashes.append(hashlib.sha256(raw).hexdigest()[:16])
        frames.append(zlib.compress(raw, 9))

    table_size = (count + 1) * 4
    data_off = HEADER.size + table_size
    offsets = [data_off]
    for f in frames:
        offsets.append(offsets[-1] + len(f))
    blob = bytearray(HEADER.pack(b"DCPV", 1, 96, 96, args.fps, count, data_off))
    blob += struct.pack("<%dI" % len(offsets), *offsets)
    for f in frames:
        blob += f
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(blob)

    manifest = {
        "source": str(args.avi),
        "source_frames": total,
        "source_fps": args.source_fps,
        "fps": args.fps,
        "count": count,
        "step": step,
        "seconds": count / args.fps,
        "rotate_180": "none",
        "pixel_format": "RGB565 big-endian",
        "blob_bytes": len(blob),
        "blob_sha256": hashlib.sha256(blob).hexdigest(),
        "frame_raw_sha256_prefix": raw_hashes,
    }
    args.out.with_suffix(args.out.suffix + ".json").write_text(
        json.dumps(manifest, indent=2), encoding="utf-8")

    if args.preview:
        picks = [0, min(5, count - 1), min(10, count - 1), min(20, count - 1),
                 min(30, count - 1), min(50, count - 1), min(75, count - 1),
                 min(100, count - 1), min(125, count - 1), count - 1]
        th = 144
        sheet = Image.new("RGB", (th * 5, th * 2), (0, 0, 0))
        for n, idx in enumerate(dict.fromkeys(picks)):
            src = min(total - 1, idx * step)
            im = Image.frombytes("RGB", (96, 96), avi[src * FRAME_SIZE:(src + 1) * FRAME_SIZE])
            im = im.resize((th, th), Image.NEAREST)
            sheet.paste(im, ((n % 5) * th, (n // 5) * th))
            d = ImageDraw.Draw(sheet)
            d.text(((n % 5) * th + 4, (n // 5) * th + 4), str(idx), fill=(255, 255, 0))
        args.preview.parent.mkdir(parents=True, exist_ok=True)
        sheet.save(args.preview)
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()