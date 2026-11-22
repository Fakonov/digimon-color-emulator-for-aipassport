#!/usr/bin/env python3
"""Turn a sampled PC-trace campaign into a coverage report.

Input: dbgtest/cov/<tag>/*.tr  (one PC per line, ``%06X``) plus the RAM
snapshots written by the same campaign, and the two ROM images.

Output: tools/coverage/out/<tag>.json (numbers) and, with --md, a markdown
table block printed to stdout.

Usage: report.py <tag> [--md]
"""
import glob
import json
import os
import struct
import sys
from collections import Counter, defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from unsp_static import Images, decode, TRANSFERS, KIND_IND, KIND_RET, KIND_RETI, KIND_COND, KIND_JMP, KIND_CALL, KIND_INVALID

BASE = r"C:\Users\DuWeisi\digimon-color-passport"
FLASH = os.path.join(BASE, "roms", "dmc_v1_base.bin")
IROM = os.path.join(BASE, "roms", "internal_rom_221017a.bin")

# recomp-translated ranges (see docs/HANDOFF.md)
RECOMP = [("hot", 0x2002F4, 0x200343), ("delay", 0x5376, 0x537F)]


def region_of(pc):
    if 0x200000 <= pc < 0x400000:
        return "extflash"
    if 0x4000 <= pc < 0x10000:
        return "irom"
    if 0 < pc < 0x1000:
        return "ram"
    if 0x3000 <= pc < 0x3100:
        return "io"
    return "other"


def main():
    tag = sys.argv[1] if len(sys.argv) > 1 else "A"
    want_md = "--md" in sys.argv
    trdir = os.path.join(BASE, "dbgtest", "cov", tag)
    files = sorted(glob.glob(os.path.join(trdir, "*.tr")))
    if not files:
        sys.exit("no trace files in " + trdir)

    img = Images(FLASH, IROM)
    dec_cache = {}

    def dec(pc):
        d = dec_cache.get(pc)
        if d is None:
            d = decode(img, pc)
            dec_cache[pc] = d
        return d

    seen = set()
    dyn_region = Counter()
    dyn_total = 0
    irq_lines = 0
    len_mismatch = Counter()
    recomp_dyn = Counter()
    block_starts = set()
    block_execs = 0
    ind_targets = defaultdict(set)      # JMPR site -> observed targets
    ind_hits = Counter()               # JMPR site -> times
    indirect_transfers = Counter()     # prev pc -> times, for non-direct transfers
    ret_sites = Counter()
    invalid_hits = Counter()
    window_stats = []
    cold_first = None

    for path in files:
        w_total = 0
        prev_pc = None
        prev_len = 1
        prev_kind = None
        with open(path, "rb") as f:
            for line in f:
                if line[:1] == b"(":
                    irq_lines += 1
                    continue
                # the trace writes two lines per instruction: a bare "%06X"
                # (from our tracelog action) and the standard "%06X: disasm".
                # Only the second one is used.
                if line[6:8] != b": ":
                    continue
                pc = int(line[:6], 16)
                w_total += 1
                dyn_total += 1
                dyn_region[region_of(pc)] += 1
                if pc not in seen:
                    seen.add(pc)
                for name, lo, hi in RECOMP:
                    if lo <= pc <= hi:
                        recomp_dyn[name] += 1
                        break

                if prev_pc is not None:
                    if prev_kind in TRANSFERS:
                        newblock = True
                    elif pc == prev_pc + prev_len:
                        newblock = False
                    else:
                        newblock = True
                        len_mismatch[prev_pc] += 1
                    if newblock:
                        block_execs += 1
                        block_starts.add(pc)
                        if prev_kind == KIND_IND:
                            ind_targets[prev_pc].add(pc)
                            ind_hits[prev_pc] += 1
                        elif prev_kind in (KIND_RET, KIND_RETI):
                            ret_sites[prev_pc] += 1
                        elif prev_kind not in (KIND_COND, KIND_JMP, KIND_CALL):
                            indirect_transfers[prev_pc] += 1
                else:
                    block_starts.add(pc)

                ln, kind, tgt, op = dec(pc)
                if kind == KIND_INVALID:
                    invalid_hits[pc] += 1
                prev_pc, prev_len, prev_kind = pc, ln, kind
        window_stats.append((os.path.basename(path), w_total, irq_lines))
        if w_total == 0:
            pass

    # ---- block extents (static walk from every observed block entry) ----
    full_blocks = 0
    part_blocks = 0
    block_sizes = Counter()
    uncovered_in_part = defaultdict(list)
    for b in block_starts:
        pcs = []
        pc = b
        for _ in range(400):
            pcs.append(pc)
            ln, kind, tgt, op = dec(pc)
            if kind in TRANSFERS:
                break
            pc += ln
        block_sizes[len(pcs)] += 1
        miss = [p for p in pcs if p not in seen]
        if not miss:
            full_blocks += 1
        else:
            part_blocks += 1
            if len(uncovered_in_part) < 4000:
                uncovered_in_part[b] = miss

    # ---- per-region static footprint ----
    irom_pcs = sorted(p for p in seen if region_of(p) == "irom")
    flash_pcs = sorted(p for p in seen if region_of(p) == "extflash")
    ram_pcs = sorted(p for p in seen if region_of(p) == "ram")
    io_pcs = sorted(p for p in seen if region_of(p) == "io")
    other_pcs = sorted(p for p in seen if region_of(p) == "other")

    def gaps(pcs, limit=20):
        out = []
        for a, b in zip(pcs, pcs[1:]):
            if b - a > 1:
                out.append((a + 1, b - 1, b - a - 1))
        out.sort(key=lambda t: -t[2])
        return out[:limit]

    # ---- self-modifying code evidence ----
    snaps = sorted(glob.glob(os.path.join(trdir, "ram_t*.bin")))
    ram_series = []
    for s in snaps:
        with open(s, "rb") as f:
            ram_series.append((os.path.basename(s), f.read()))
    smc_pcs = []
    if ram_series and ram_pcs:
        words0 = ram_series[0][1]
        changing = set()
        for name, blob in ram_series[1:]:
            for i in range(0, min(len(blob), len(words0)) - 1, 2):
                if blob[i:i + 2] != words0[i:i + 2]:
                    changing.add(i // 2)
        smc_pcs = [p for p in ram_pcs if p in changing]

    res = {
        "tag": tag,
        "files": len(files),
        "dyn_total": dyn_total,
        "unique_pcs": len(seen),
        "unique_blocks": len(block_starts),
        "block_execs": block_execs,
        "dyn_region": dict(dyn_region),
        "unique_region": {
            "irom": len(irom_pcs),
            "extflash": len(flash_pcs),
            "ram": len(ram_pcs),
            "io": len(io_pcs),
            "other": len(other_pcs),
        },
        "irom_span": (irom_pcs[0], irom_pcs[-1]) if irom_pcs else None,
        "flash_span": (flash_pcs[0], flash_pcs[-1]) if flash_pcs else None,
        "irom_gaps": gaps(irom_pcs),
        "flash_gaps": gaps(flash_pcs),
        "blocks_full": full_blocks,
        "blocks_partial": part_blocks,
        "block_size_hist": dict(sorted(block_sizes.items())),
        "recomp_dyn": dict(recomp_dyn),
        "recomp_pcs": {
            name: sorted(p for p in seen if lo <= p <= hi)
            for name, lo, hi in RECOMP
        },
        "ind_sites": {
            "%06X" % k: {"hits": ind_hits[k], "targets": sorted(v)}
            for k, v in sorted(ind_targets.items(), key=lambda kv: -ind_hits[kv[0]])
        },
        "ind_other_transfers": {"%06X" % k: v for k, v in indirect_transfers.most_common(40)},
        "ret_sites": {"%06X" % k: v for k, v in ret_sites.most_common(20)},
        "invalid_pcs": {"%06X" % k: v for k, v in invalid_hits.most_common(20)},
        "irq_events": irq_lines,
        "len_mismatch": {"%06X" % k: v for k, v in len_mismatch.most_common(20)},
        "len_mismatch_total": sum(len_mismatch.values()),
        "ram_pcs": ram_pcs[:200],
        "smc_pcs": smc_pcs[:200],
        "smc_count": len(smc_pcs),
        "ram_snapshots": [n for n, _ in ram_series],
        "empty_windows": [w for w, n, _ in window_stats if n == 0],
        "window_count": len(window_stats),
    }

    outdir = os.path.join(BASE, "tools", "coverage", "out")
    os.makedirs(outdir, exist_ok=True)
    with open(os.path.join(outdir, tag + ".json"), "w", encoding="utf-8") as f:
        json.dump(res, f, indent=1)

    # full PC / block-start sets, for unioning campaigns and for the recompiler
    with open(os.path.join(outdir, tag + "_pcs.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join("%06X" % p for p in sorted(seen)))
    with open(os.path.join(outdir, tag + "_blocks.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join("%06X" % p for p in sorted(block_starts)))

    print("tag=%s windows=%d dyn=%d unique_pcs=%d blocks=%d full=%d partial=%d" % (
        tag, len(files), dyn_total, len(seen), len(block_starts), full_blocks, part_blocks))
    print("region dyn:", dict(dyn_region))
    print("region uniq:", res["unique_region"])
    print("irom span:", res["irom_span"], "flash span:", res["flash_span"])
    print("recomp dyn:", dict(recomp_dyn))
    print("ind sites:", len(ind_targets), "top:", list(res["ind_sites"].items())[:5])
    print("ram pcs:", len(ram_pcs), "smc:", len(smc_pcs), "irq lines:", irq_lines)
    print("empty windows:", res["empty_windows"][:10], "count", len(res["empty_windows"]))
    print("wrote", os.path.join(outdir, tag + ".json"))


if __name__ == "__main__":
    main()
