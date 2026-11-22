#!/usr/bin/env python3
"""Generate MAME debugger scripts that sample PC traces over a 60 s run.

Why sampling: tracing costs ~16 us per guest instruction in MAME 0.289
(unSP core is interpreted while tracing, no DRC), and one 60 s guest run is
~2.9e9 instructions (~46 GB of text). Full trace is not feasible. Instead we
take short windows: dense during boot, 4 ms every 20 ms up to 1 s, then 4 ms
every second up to 60 s.

Usage:
  python tools/coverage/gen_campaign.py [--keys]

Writes dbgtest/covA.cmd (or covB.cmd with key injection), dbgtest/cov_plan.json
and creates dbgtest/cov/<tag>/ .
"""
import json
import os
import sys

BASE = r"C:\Users\DuWeisi\digimon-color-passport"
DBG = os.path.join(BASE, "dbgtest")

# (start_ms, dur_ms)
BOOT = [(s, 20) for s in range(0, 120, 20)]
MID = [(s, 4) for s in range(120, 960, 20)]
LATE = [(s, 4) for s in range(1000, 60000, 1000)]

PLAN = BOOT + MID + LATE
RAM_DUMPS = [0, 200, 1000, 5000, 10000, 17000, 20000, 25000, 30000, 40000, 50000, 59500]


def main():
    keys = "--keys" in sys.argv
    tag = "B" if keys else "A"
    outdir = os.path.join(DBG, "cov", tag)
    os.makedirs(outdir, exist_ok=True)

    lines = []
    prev = 0
    for i, (start, dur) in enumerate(PLAN):
        delay = start - prev
        if delay > 0:
            lines.append("gtime #%d" % delay)
        tr = os.path.join(outdir, "%03d_t%05d.tr" % (i, start))
        lines.append('trace %s,0,noloop,{tracelog "%%06X\\n",pc}' % tr)
        lines.append("gtime #%d" % dur)
        lines.append("trace off,0")
        if start in RAM_DUMPS:
            lines.append("save %s,0,1000" % os.path.join(outdir, "ram_t%05d.bin" % start))
        prev = start + dur
    if prev < 60000:
        lines.append("gtime #%d" % (60000 - prev))
    lines.append("save %s,0,1000" % os.path.join(outdir, "ram_t060000.bin"))
    lines.append("quit")

    cmd = os.path.join(DBG, "cov%s.cmd" % tag)
    with open(cmd, "w", newline="\r\n", encoding="ascii") as f:
        f.write("\n".join(lines) + "\n")

    meta = {
        "tag": tag,
        "keys": keys,
        "plan": PLAN,
        "windows": len(PLAN),
        "dwell_ms": sum(d for _, d in PLAN),
        "script": cmd,
        "outdir": outdir,
        "ram_dumps": RAM_DUMPS + [60000],
    }
    with open(os.path.join(DBG, "cov_plan_%s.json" % tag), "w", encoding="utf-8") as f:
        json.dump(meta, f, indent=1)
    print("wrote %s  windows=%d dwell=%d ms out=%s" % (cmd, len(PLAN), meta["dwell_ms"], outdir))


if __name__ == "__main__":
    main()
