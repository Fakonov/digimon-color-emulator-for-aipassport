#!/usr/bin/env python3
"""Validate frames.bin and render the exact 320x240 panel composition."""
import argparse
import struct
import zlib
from pathlib import Path

from PIL import Image, ImageDraw

HEADER = struct.Struct("<4sHHHHII")
SRC = 96
DST_W, DST_H = 320, 240
SCALE = 2
LEFT = (DST_W - SRC * SCALE) // 2
TOP = (DST_H - SRC * SCALE) // 2


def rgb565_to_rgb(raw: bytes) -> Image.Image:
    im = Image.new("RGB", (SRC, SRC))
    px = im.load()
    for y in range(SRC):
        for x in range(SRC):
            i = (y * SRC + x) * 2
            v = (raw[i] << 8) | raw[i + 1]
            r = ((v >> 11) & 0x1F) * 255 // 31
            g = ((v >> 5) & 0x3F) * 255 // 63
            b = (v & 0x1F) * 255 // 31
            px[x, y] = (r, g, b)
    return im


def compose(frame: Image.Image) -> Image.Image:
    out = Image.new("RGB", (DST_W, DST_H), (0, 0, 0))
    out.paste(frame.resize((SRC * SCALE, SRC * SCALE), Image.NEAREST), (LEFT, TOP))
    return out


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("blob", type=Path)
    ap.add_argument("--device", type=Path)
    ap.add_argument("--contact", type=Path)
    args = ap.parse_args()

    data = args.blob.read_bytes()
    magic, version, width, height, fps, count, data_off = HEADER.unpack_from(data, 0)
    assert magic == b"DCPV", magic
    assert version == 1 and width == SRC and height == SRC
    assert data_off == HEADER.size + (count + 1) * 4
    offsets = struct.unpack_from("<%dI" % (count + 1), data, HEADER.size)
    assert offsets[0] == data_off and offsets[-1] == len(data)
    assert list(offsets) == sorted(offsets), "offsets not monotonic"

    frames = []
    for i in range(count):
        raw = zlib.decompress(data[offsets[i]:offsets[i + 1]])
        assert len(raw) == SRC * SRC * 2, (i, len(raw))
        frames.append(rgb565_to_rgb(raw))

    print(f"OK frames={count} fps={fps} bytes={len(data)} sha256={__import__('hashlib').sha256(data).hexdigest()}")

    if args.device:
        args.device.parent.mkdir(parents=True, exist_ok=True)
        compose(frames[100]).save(args.device)
        print("device", args.device, args.device.stat().st_size)
    if args.contact:
        picks = [0, 30, 60, 100, 125, count - 1]
        cell_w, cell_h = 320, 240
        sheet = Image.new("RGB", (cell_w * 3, cell_h * 2), (0, 0, 0))
        for n, idx in enumerate(picks):
            sheet.paste(compose(frames[idx]), ((n % 3) * cell_w, (n // 3) * cell_h))
            d = ImageDraw.Draw(sheet)
            d.text(((n % 3) * cell_w + 4, (n // 3) * cell_h + 4), str(idx), fill=(255, 255, 0))
        args.contact.parent.mkdir(parents=True, exist_ok=True)
        sheet.save(args.contact)
        print("contact", args.contact, args.contact.stat().st_size)


if __name__ == "__main__":
    main()
