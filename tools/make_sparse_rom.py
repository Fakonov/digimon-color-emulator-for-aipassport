#!/usr/bin/env python3
"""Pack the 8 MiB Digimon Color SPI flash image into a sparse page store.

The Digimon Color firmware (roms/dmc_v1_base.bin, 8,388,608 B) is 1,437
all-zero 4 KiB pages plus 611 pages that actually carry data.  The AI Passport
only has 8 MiB of flash total, so the image goes into the 6 MiB `roms` data
partition as a sparse store instead of a flat 8 MiB blob:

    offset  size      content
    0x0000  4096      header (see below)
    0x1000  256       page bitmap, bit i set = page i present
    0x1100  8192      index: page_count * uint32 offsets, relative to the
                      partition start; 0xFFFFFFFF marks an all-zero page
    0x3100  n*4096    the present pages, ascending page order

Header (little endian):

    +0   char   magic[4]          "DMC1"
    +4   u32    version           1
    +8   u32    image_size        8388608
    +12  u8     sha1[20]          sha1 of the source image
    +32  u32    page_size         4096
    +36  u32    page_count        2048
    +40  u32    bitmap_offset     0x1000
    +44  u32    index_offset      0x1100
    +48  u32    data_offset       0x3100
    +52  u32    flags             0
    +56  u32    present_pages     convenience copy of the popcount
    +60  ...                      zero padding to 4096

The reader is firmware/digicore-c3/main/emu/romstore.h (64 KiB page cache) and
main/rom_esp.cpp (esp_partition + index).  Nothing here is device specific:
run it on the host, then flash the result at 0x210000.

Usage:
    python tools/make_sparse_rom.py                  # defaults, writes the artifact
    python tools/make_sparse_rom.py --verify         # re-read and re-hash the result
    python tools/make_sparse_rom.py --stats          # page statistics only
"""

from __future__ import annotations

import argparse
import hashlib
import struct
import sys
from pathlib import Path

MAGIC = b"DMC1"
VERSION = 1
PAGE_SIZE = 4096
HEADER_SIZE = 4096
BITMAP_OFFSET = HEADER_SIZE
INDEX_OFFSET = BITMAP_OFFSET + 256
DATA_OFFSET = INDEX_OFFSET + 8192
NONE = 0xFFFFFFFF

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_SRC = ROOT / "roms" / "dmc_v1_base.bin"
DEFAULT_OUT = ROOT / "firmware" / "digicore-c3" / "sparse" / "digicore-sparse.bin"


def build_header(image_size: int, image_sha1: bytes, page_count: int, present: int) -> bytes:
    head = bytearray(HEADER_SIZE)
    struct.pack_into("<4sIII", head, 0, MAGIC, VERSION, image_size,
                     PAGE_SIZE)
    head[12:32] = image_sha1
    struct.pack_into("<IIIII", head, 32, PAGE_SIZE, page_count,
                     BITMAP_OFFSET, INDEX_OFFSET, DATA_OFFSET)
    struct.pack_into("<I", head, 52, 0)                 # flags
    struct.pack_into("<I", head, 56, present)           # present_pages
    return bytes(head)


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--src", type=Path, default=DEFAULT_SRC)
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT)
    ap.add_argument("--stats", action="store_true", help="only print page statistics")
    ap.add_argument("--verify", action="store_true", help="verify the written file")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args(argv)

    if not args.src.exists():
        print(f"error: source image not found: {args.src}", file=sys.stderr)
        return 2

    image = args.src.read_bytes()
    if len(image) % PAGE_SIZE:
        print(f"error: {args.src} is not a multiple of {PAGE_SIZE} B", file=sys.stderr)
        return 2

    page_count = len(image) // PAGE_SIZE
    if page_count > 2048:
        # The index/bitmap are sized for 8 MiB (2048 pages).  The partition has
        # room for more, but nothing in the C3 port needs it.
        print(f"error: {page_count} pages does not fit the 2048 page index", file=sys.stderr)
        return 2

    pages = [image[i * PAGE_SIZE:(i + 1) * PAGE_SIZE] for i in range(page_count)]
    zero = bytes(PAGE_SIZE)
    present = [p != zero for p in pages]
    n_present = sum(present)

    if args.stats:
        print(f"image      {len(image)} B ({len(image) / 1048576:.3f} MiB)")
        print(f"pages      {page_count} ({PAGE_SIZE} B each)")
        print(f"non-zero   {n_present}  zero {page_count - n_present}")
        print(f"sparse     {DATA_OFFSET + n_present * PAGE_SIZE} B "
              f"({(DATA_OFFSET + n_present * PAGE_SIZE) / 1048576:.3f} MiB)")
        print(f"sha1       {hashlib.sha1(image).hexdigest()}")
        return 0

    image_sha1 = hashlib.sha1(image).digest()

    bitmap = bytearray(256)
    index = bytearray(8192)
    data = bytearray()
    cursor = DATA_OFFSET
    for i, page in enumerate(pages):
        if not present[i]:
            struct.pack_into("<I", index, i * 4, NONE)
            continue
        bitmap[i >> 3] |= 1 << (i & 7)
        struct.pack_into("<I", index, i * 4, cursor)
        data += page
        cursor += PAGE_SIZE

    blob = bytearray()
    blob += build_header(len(image), image_sha1, page_count, n_present)
    blob += bitmap
    blob += index
    blob += data
    assert len(blob) == DATA_OFFSET + n_present * PAGE_SIZE

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(blob)

    if args.verify:
        back = args.out.read_bytes()
        assert back[:4] == MAGIC, "magic mismatch"
        assert len(back) == len(blob), "size mismatch"
        for i, page in enumerate(pages):
            off = struct.unpack_from("<I", back, INDEX_OFFSET + i * 4)[0]
            got = bytes(PAGE_SIZE) if off == NONE else back[off:off + PAGE_SIZE]
            if got != page:
                print(f"error: page {i} round trip mismatch", file=sys.stderr)
                return 3
        print("verify     all pages round trip byte exact")

    if not args.quiet:
        print(f"src        {args.src} ({len(image)} B)")
        print(f"sha1       {image_sha1.hex()}")
        print(f"pages      {page_count}, present {n_present}, zero {page_count - n_present}")
        print(f"sparse     {len(blob)} B ({len(blob) / 1048576:.3f} MiB), "
              f"data at 0x{DATA_OFFSET:04x}")
        print(f"out        {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
