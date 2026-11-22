import re, io
rows = []
for ln in io.open("dbgtest/fix1.log", encoding="utf-8", errors="replace"):
    if "frames=" not in ln: continue
    g = lambda k: (re.search(k + r"=([0-9a-f]+|[\d.]+)", ln) or [None, None])[1]
    if g("frames") and g("guest_fps"):
        rows.append(dict(frames=int(g("frames")), fps=float(g("guest_fps")), pc=g("pc"),
                         guest=int(g("guest_pct")), gmax=int(g("gmax")), tmax=int(g("tmax")),
                         blocks=int(g("aotb")), insns=int(g("aoti")), miss=int(g("aotmiss")),
                         blitmax=int(g("blitmax")), rows=int(g("rows")), errs=int(g("errs") or 0)))
if not rows:
    print("no samples"); raise SystemExit
idle = [r for r in rows if r["pc"] in ("200343", "230321")]
move = [r for r in rows if r["pc"] not in ("200343", "230321")]
def stat(name, rs):
    if not rs: return
    f = [r["fps"] for r in rs]
    print("%-5s n=%2d mean=%5.2f min=%5.2f max=%5.2f  blitmax=%5d errs=%d" % (
        name, len(f), sum(f)/len(f), min(f), max(f), max(r["blitmax"] for r in rs), max(r["errs"] for r in rs)))
stat("idle", idle); stat("move", move); stat("all", rows)
f = [r["fps"] for r in rows]
print("exact60=%d/%d   frames %d -> %d" % (sum(1 for x in f if x >= 59.99), len(f), rows[0]["frames"], rows[-1]["frames"]))
print("last miss ratio %.1f%%" % (100.0*rows[-1]["miss"]/(rows[-1]["miss"]+rows[-1]["blocks"])))
