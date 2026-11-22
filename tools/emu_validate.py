#!/usr/bin/env python3
"""Compare the PC stream of emu/digicore.exe against a MAME debugger trace window.

MAME trace files contain two lines per instruction: a bare "%06X" from the
trace action and "%06X: <disassembly>" from the debugger itself.  Only the
disassembly line carries the authoritative PC, so that is what we read.

Usage:
  python tools/emu_validate.py dbgtest/cov/A/000_t00000.tr my_trace.tr
"""
import struct
import sys


def read_mame_pcs(path):
    pcs = []
    with open(path, "rb") as fp:
        for line in fp:
            if len(line) > 8 and line[6:8] == b": ":
                try:
                    pcs.append(int(line[:6], 16))
                except ValueError:
                    pass
    return pcs


def read_mine(path):
    pcs = []
    with open(path, "rb") as fp:
        for line in fp:
            line = line.strip()
            if len(line) == 6:
                try:
                    pcs.append(int(line, 16))
                except ValueError:
                    pass
    return pcs


def find_alignment(mine, mame, window=32, max_skip=16384):
    """Best (skip, start) such that mame[start:] lines up with mine[skip:].

    Several candidates can match inside a polling loop, so every candidate is
    scored by how many instructions agree from that point on and the longest
    agreement wins.  max_skip has to be generous: the debugger's gtime lands a
    few hundred cycles off, which is ~70 instructions at a CPI of 4, and a
    narrow search silently reports a 12% match for an otherwise identical run.

    The lookup is a hash join over 8-PC prefixes rather than a bytes.find scan
    per skip: a scan costs max_skip * len(mame) bytes of memchr work, which is
    minutes on a 682k-instruction stream.
    """
    if not mine or not mame:
        return None
    window = min(window, len(mame) - 8, len(mine) - 8)
    if window <= 0:
        return None
    index = {}
    for start in range(0, len(mame) - 8 + 1):
        index.setdefault(tuple(mame[start:start + 8]), []).append(start)
    best = None
    for skip in range(0, min(max_skip, len(mine) - window)):
        starts = index.get(tuple(mine[skip:skip + 8]))
        if not starts:
            continue
        for start in starts[:8]:
            score = match_length(mame, start, mine, skip)
            if best is None or score > best[0]:
                best = (score, skip, start)
    if best is None:
        return None
    return best[1], best[2]


def match_length(mame, start, mine, skip, cap=None):
    n = min(len(mame) - start, len(mine) - skip)
    if cap is not None:
        n = min(n, cap)
    i = 0
    while i < n and mame[start + i] == mine[skip + i]:
        i += 1
    return i


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    mame = read_mame_pcs(sys.argv[1])
    mine = read_mine(sys.argv[2])
    print("mame pcs: %d   emulator pcs: %d" % (len(mame), len(mine)))
    if not mame or not mine:
        print("FAIL: empty stream (mame=%d mine=%d)" % (len(mame), len(mine)))
        return 1

    start = find_alignment(mine, mame)
    if start is None:
        print("FAIL: emulator stream does not appear in the mame window at all")
        print("  mame starts at %06X, emulator starts at %06X" % (mame[0], mine[0]))
        return 1
    skip, start = start
    if skip:
        print("emulator ran %d instruction(s) before the mame window opened" % skip)
        mine = mine[skip:]

    m = mame[start:]
    n = min(len(m), len(mine))
    same = 0
    while same < n and m[same] == mine[same]:
        same += 1

    print("aligned at mame[%d] (pc %06X)" % (start, m[0]))
    print("matched %d / %d instructions (%.3f%%)" % (same, n, 100.0 * same / n if n else 0))
    if same >= 0.999 * n:
        print("PASS: identical stream (%d / %d, tail may be truncated)" % (same, n))
        return 0
    lo = max(0, same - 8)
    hi = min(n, same + 12)
    print("first divergence at index %d:" % same)
    for i in range(lo, hi):
        mark = "  <<<" if i == same else ""
        a = "%06X" % m[i] if i < len(m) else "------"
        b = "%06X" % mine[i] if i < len(mine) else "------"
        print("  %6d  mame=%s  emu=%s%s" % (i, a, b, mark))
    return 0 if same >= 0.999 * n else 1


if __name__ == "__main__":
    sys.exit(main())
