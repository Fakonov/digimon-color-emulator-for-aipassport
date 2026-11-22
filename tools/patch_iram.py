#!/usr/bin/env python
"""Place the unSP interpreter in IRAM on the ESP32-C3.

The interpreter is ~40 KB of code that the CPU jumps around inside for every
guest instruction; on this part that costs an I-cache miss almost every time
because the code lives in flash.  Moving it to IRAM removes the fetch stalls.

Every `unsp_device::` member definition in main/emu/{unsp_glue.cpp,isa/*.cpp}
gets an IRAM_ATTR prefix, plus a tiny include so the macro exists.  Idempotent.
"""
import io
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EMU = os.path.join(ROOT, "firmware", "digicore-c3", "main", "emu")

FILES = [os.path.join(EMU, "unsp_glue.cpp")] + [
    os.path.join(EMU, "isa", name)
    for name in sorted(os.listdir(os.path.join(EMU, "isa")))
    if name.endswith(".cpp")
]

DEF = re.compile(r"^([A-Za-z_][\w:<>,\s\*&]*\s+)?(unsp_\w+::[\w~]+)\s*\(", re.M)

HEADER = """// IRAM placement: the interpreter runs on every guest instruction, so keeping it
// in flash costs an I-cache miss per dispatch.  See tools/patch_iram.py.
#include "esp_attr.h"
"""


def patch(path):
    text = io.open(path, encoding="utf-8").read()
    if "#include \"esp_attr.h\"" not in text:
        # insert after the first include block
        idx = text.find("\n\n")
        text = HEADER + text
    added = 0
    lines = text.split("\n")
    for i, line in enumerate(lines):
        if line.startswith("void unsp_device::") or line.startswith("uint16_t unsp_device::") \
                or line.startswith("uint8_t unsp_device::") or line.startswith("bool unsp_device::") \
                or line.startswith("int unsp_device::") or line.startswith("void unsp_device::"):
            if not line.startswith("IRAM_ATTR"):
                lines[i] = "IRAM_ATTR " + line
                added += 1
    text = "\n".join(lines)
    io.open(path, "w", encoding="utf-8", newline="\n").write(text)
    print("%-52s %d definition(s)" % (os.path.relpath(path, ROOT), added))


def main():
    for path in FILES:
        patch(path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
