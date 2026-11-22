"""Diff the host emulator's per-instruction cycle charges against MAME's.

Both inputs are "PC CYCLES" logs (MAME's tracelog lines interleaved with its
disassembly lines are skipped).  The script aligns the two PC streams, then
reports every instruction index whose cycle charge differs, grouped by opcode
and by PC so the offending decoder path is obvious.
"""
import sys
from collections import Counter, defaultdict


def load_mame(path):
    pcs, cyc = [], []
    with open(path, "r", encoding="ascii", errors="replace") as fp:
        for line in fp:
            if ": " in line:            # MAME's own disassembly line
                continue
            parts = line.split()
            if len(parts) != 2:
                continue
            try:
                pc = int(parts[0], 16)
                c = int(parts[1], 10)
            except ValueError:
                continue
            pcs.append(pc)
            cyc.append(c)
    return pcs, cyc


def load_mine(path):
    pcs, cyc = [], []
    with open(path, "r", encoding="ascii", errors="replace") as fp:
        for line in fp:
            parts = line.split()
            if len(parts) != 2:
                continue
            pcs.append(int(parts[0], 16))
            cyc.append(int(parts[1], 10))
    return pcs, cyc


def main():
    mame_pcs, mame_cyc = load_mame(sys.argv[1])
    my_pcs, my_cyc = load_mine(sys.argv[2])
    print("mame instructions:", len(mame_pcs), " mine:", len(my_pcs))

    # align on the first matching PC pair with a 24 instruction tolerance
    shift = None
    for s in range(0, 25):
        if my_pcs[s:s + 40] == mame_pcs[0:40]:
            shift = s
            break
    if shift is None:
        print("could not align")
        return 1
    print("my stream starts %d instructions before the mame window" % shift)

    my_pcs = my_pcs[shift:]
    my_cyc = my_cyc[shift:]

    n = min(len(mame_pcs), len(my_pcs))
    bad = []
    # mame_cyc[i + 1] is read inside the loop, so the last usable index is n - 2.
    for i in range(1, n - 1):
        if my_pcs[i] != mame_pcs[i]:
            print("PC stream diverges at %d: mame=%06X mine=%06X" % (i, mame_pcs[i], my_pcs[i]))
            n = i
            break
        dm = mame_cyc[i + 1] - mame_cyc[i]
        de = my_cyc[i]
        if dm != de:
            bad.append((i, mame_pcs[i], dm, de))

    print("compared %d instructions, %d cycle mismatches" % (n - 1, len(bad)))
    if bad:
        by_pc = Counter((pc, dm, de) for _, pc, dm, de in bad)
        print("top mismatching instructions (pc, mame_cycles, mine):")
        for (pc, dm, de), cnt in by_pc.most_common(25):
            print("  %06X  mame=%d mine=%d  x%d" % (pc, dm, de, cnt))
        print("first 10 mismatches:")
        for i, pc, dm, de in bad[:10]:
            print("  idx=%d pc=%06X mame=%d mine=%d" % (i, pc, dm, de))
        print("total cycle delta (mine - mame) at end of window:",
              my_cyc[n - 1] - mame_cyc[n - 1])
    return 0


if __name__ == "__main__":
    sys.exit(main())
