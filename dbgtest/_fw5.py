import re, io
rows = []
for ln in io.open("dbgtest/final2.log", encoding="utf-8", errors="replace"):
    if "frames=" not in ln: continue
    g = lambda k: (re.search(k + r"=([0-9a-f]+|[\d.]+)", ln) or [None, None])[1]
    if g("frames") and g("guest_fps"):
        rows.append(dict(frames=int(g("frames")), fps=float(g("guest_fps")), pc=g("pc"), guest=int(g("guest_pct")), gmax=int(g("gmax")), tmax=int(g("tmax")), blocks=int(g("aotb")), insns=int(g("aoti")), miss=int(g("aotmiss"))))
if not rows:
    print("no samples"); raise SystemExit
idle = [r for r in rows if r["pc"] in ("200343", "230321")]
move = [r for r in rows if r not in idle]
def stat(name, rs):
    if not rs: return
    f = [r["fps"] for r in rs]
    print("%-6s n=%2d mean=%5.2f min=%5.2f max=%5.2f  gmax=%6d us  tmax=%5d us  miss=%d/%d" % (
        name, len(f), sum(f)/len(f), min(f), max(f), max(r["gmax"] for r in rs), max(r["tmax"] for r in rs),
        rs[-1]["miss"], rs[-1]["blocks"]))
stat("idle", idle); stat("move", move); stat("all", rows)
f = [r["fps"] for r in rows]
print("exact60=%d/%d  frames %d -> %d over %.0fs" % (sum(1 for x in f if x >= 59.99), len(f), rows[0]["frames"], rows[-1]["frames"], 75))
print("miss ratio last:", "%.1f%%" % (100.0*rows[-1]["miss"]/(rows[-1]["miss"]+rows[-1]["blocks"])))
