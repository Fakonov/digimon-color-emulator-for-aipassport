# -*- coding: utf-8 -*-
"""Copy the stone tile from digimon-passport into this firmware as plain C.

Both firmwares draw the same 320x240 panel, and that tile (128x128, ROWS=3,
stones ~128x43, 5 px mortar) is the one the other project tuned across many
rounds - it is copied verbatim on purpose, not regenerated.  Only the LVGL
image descriptor is dropped; this firmware has no LVGL.

    python tools/gen_brick_a8.py
    -> firmware/digicore-c3/main/brick_a8.h
"""
import os
import re

SRC = r"C:\Users\DuWeisi\digimon-passport\firmware\digimon-passport\main\dg_brick.h"
OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                   "firmware", "digicore-c3", "main", "brick_a8.h")
W, H = 128, 128


def main() -> int:
    txt = open(SRC, encoding="utf-8").read()
    start = txt.index("dg_brick_px[")
    brace = txt.index("{", start)
    end = txt.index("};", brace)
    vals = [int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{2})", txt[brace:end])]
    assert len(vals) == W * H, (len(vals), W * H)

    lines = [
        "/* brick_a8.h -- %dx%d A8 stone tile for the RETAIL (style C) shell." % (W, H),
        " *",
        " * Copied byte for byte from the sibling project:",
        " *   digimon-passport/firmware/digimon-passport/main/dg_brick.h",
        " * which generates it with tools/gen_brick_tile.py.  That tile is the",
        " * tuned one - same panel, same shell treatment - so it is not regenerated",
        " * here.  Only the LVGL image descriptor is dropped (this firmware has no",
        " * LVGL); the pixel data, size and alpha semantics are untouched.",
        " *",
        " * A8: 250 = mortar joint, 0 = stone face.  The face is painted in the",
        " * shell body colour and the joint in the mortar colour, both in bezel.cpp;",
        " * bezel.cpp also adds a 1 px top highlight and bottom shade by looking at",
        " * this mask's neighbours, which is what keeps a flat wall from reading as",
        " * printed mosaic on a single-colour red shell.",
        " */",
        "#ifndef BRICK_A8_H_INCLUDED",
        "#define BRICK_A8_H_INCLUDED",
        "",
        "#include <stdint.h>",
        "",
        "#define BRICK_A8_W %d" % W,
        "#define BRICK_A8_H %d" % H,
        "",
        "static const uint8_t kBrickA8[BRICK_A8_W * BRICK_A8_H] = {",
    ]
    for i in range(0, len(vals), 16):
        lines.append("    " + ", ".join("0x%02X" % v for v in vals[i:i + 16]) + ",")
    lines += ["};", "", "#endif /* BRICK_A8_H_INCLUDED */", ""]
    open(OUT, "w", encoding="utf-8", newline="\n").write("\n".join(lines))
    print("wrote", OUT, os.path.getsize(OUT), "bytes,", len(vals), "px,",
          sum(1 for v in vals if v), "mortar")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
