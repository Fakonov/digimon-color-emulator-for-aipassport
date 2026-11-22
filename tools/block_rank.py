#!/usr/bin/env python
"""Rank basic blocks by dynamic weight using a `--trace-cycles` capture.

Input: a file of "PC CHARGE" lines in execution order (emu/digicore.exe
--trace-cycles). Because the trace is in program order, a PC whose predecessor
is not PC-1 (mod 2^24) starts a new basic block, which is exactly how the MAME
core would enter one.

Output: JSON + a human-readable table of the hottest entries with their
instruction lists, and per-PC cycle costs split into taken/fall-through for
conditional branches.
"""
import collections
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools", "coverage"))
from unsp_static import Images, decode  # noqa: E402


def load(path):
    pcs = []
    charges = []
    with open(path) as fp:
        for line in fp:
            parts = line.split()
            if len(parts) != 2:
                continue
            pcs.append(int(parts[0], 16))
            charges.append(int(parts[1]))
    return pcs, charges


def block_entries(pcs):
    counts = collections.Counter()
    prev = None
    for pc in pcs:
        if prev is None or pc != ((prev + 1) & 0xFFFFFF):
            counts[pc] += 1
        prev = pc
    return counts


def branch_costs(pcs, charges):
    """Per-PC (taken cost, fall-through cost) for conditional branches."""
    taken = collections.Counter()
    fall = collections.Counter()
    for i in range(len(pcs) - 1):
        src, dst, cost = pcs[i], pcs[i + 1], charges[i]
        # Only meaningful for PCs we later identify as conditional jumps; store
        # both guesses keyed by whether the stream continued sequentially.
        if dst == ((src + 1) & 0xFFFFFF):
            fall[(src, cost)] += 1
        else:
            taken[(src, cost)] += 1
    return taken, fall


def main():
    trace = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "dbgtest", "cyc900.txt")
    top_n = int(sys.argv[2]) if len(sys.argv) > 2 else 40
    out_json = os.path.join(ROOT, "tools", "coverage", "block_rank.json")

    pcs, charges = load(trace)
    total = len(pcs)
    counts = block_entries(pcs)
    taken, fall = branch_costs(pcs, charges)

    images = Images(
        os.path.join(ROOT, "roms", "dmc_v1_base.bin"),
        os.path.join(ROOT, "roms", "internal_rom_221017a.bin"),
    )

    rows = []
    for entry, n in counts.most_common():
        length = 0
        stats = []
        pc = entry
        while True:
            ln, kind, target, op = decode(images, pc)
            stats.append((pc, ln, kind, target, op))
            length += ln
            if kind in ("cond", "jmp", "call", "ind_r3", "ret", "reti", "invalid"):
                break
            pc += ln
            if length > 400:
                break
        rows.append({"entry": entry, "execs": n, "insns": length, "dyn": n * length, "code": stats})

    rows.sort(key=lambda r: -r["dyn"])
    covered = sum(r["dyn"] for r in rows[:top_n])
    # per-PC cost: mode of the observed charge
    per_pc = collections.defaultdict(collections.Counter)
    for pc, c in zip(pcs, charges):
        per_pc[pc][c] += 1

    print("# %d instructions, %d block entries, top %d cover %.1f%% of dynamic instructions"
          % (total, len(rows), top_n, 100.0 * covered / total))
    print("%-8s %10s %6s %12s %6s" % ("entry", "execs", "insns", "dyn", "share"))
    for r in rows[:top_n]:
        print("%06X %10d %6d %12d %5.2f%%"
              % (r["entry"], r["execs"], r["insns"], r["dyn"], 100.0 * r["dyn"] / total))
        if len(sys.argv) > 3:
            for pc, ln, kind, target, op in r["code"]:
                cost = per_pc[pc].most_common(1)[0][0] if per_pc[pc] else -1
                tk = taken.most_common(1, key=lambda kv: kv[1]) if False else None
                print("    %06X %04X len=%d %-8s tgt=%06X cost=%d"
                      % (pc, op, ln, kind, target, cost))

    blob = {
        "total": total,
        "blocks": [
            {
                "entry": r["entry"],
                "execs": r["execs"],
                "insns": r["insns"],
                "dyn": r["dyn"],
                "code": [
                    {
                        "pc": pc,
                        "op": op,
                        "len": ln,
                        "kind": kind,
                        "target": target,
                        "cost": per_pc[pc].most_common(1)[0][0] if per_pc[pc] else None,
                        "costs": {str(c): n for c, n in per_pc[pc].most_common(3)},
                    }
                    for pc, ln, kind, target, op in r["code"]
                ],
            }
            for r in rows
        ],
    }
    with open(out_json, "w") as fp:
        json.dump(blob, fp, indent=1)
    print("# wrote %s" % out_json)
    return 0


if __name__ == "__main__":
    sys.exit(main())
