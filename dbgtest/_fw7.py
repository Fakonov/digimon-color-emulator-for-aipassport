import re, io, sys
path = sys.argv[1]
rows = []
for ln in io.open(path, encoding="utf-8", errors="replace"):
    if "frames=" not in ln: continue
    g = lambda k: (re.search(k + r"=([0-9a-f]+|[\d.]+)", ln) or [None, None])[1]
    if g("frames") and g("guest_fps"):
        rows.append(dict(frames=int(g("frames")), fps=float(g("guest_fps")), pc=g("pc"),
                         blocks=int(g("aotb")), insns=int(g("aoti")), miss=int(g("aotmiss"))))
if not rows: print("no samples"); raise SystemExit
idle = [r for r in rows if r["pc"] in ("200343", "230321")]
move = [r for r in rows if r["pc"] not in ("200343", "230321")]
for name, rs in (("idle", idle), ("move", move), ("all", rows)):
    if not rs: continue
    f = [r["fps"] for r in rs]
    print("%-5s n=%2d mean=%5.2f min=%5.2f max=%5.2f" % (name, len(f), sum(f)/len(f), min(f), max(f)))
f = [r["fps"] for r in rows]
print("exact60=%d/%d  last miss %.1f%%" % (sum(1 for x in f if x >= 59.99), len(f), 100.0*rows[-1]["miss"]/(rows[-1]["miss"]+rows[-1]["blocks"])))
