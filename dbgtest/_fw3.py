import re, io
rows = []
for ln in io.open("dbgtest/hle6.log", encoding="utf-8", errors="replace"):
    if "frames=" not in ln: continue
    g = lambda k: (re.search(k + r"=([0-9a-f]+|[\d.]+)", ln) or [None, None])[1]
    rows.append((g("frames"), g("guest_fps"), g("pc"), g("guest_pct"), g("gmax"), g("tmax"), g("aotb"), g("aoti"), g("aotfb"), g("aotmiss"), g("insns")))
rows = [r for r in rows if r[0] and r[1]]
if not rows:
    print("no rows")
else:
    fps = [float(r[1]) for r in rows]
    print("rows=%d mean=%.2f min=%.2f max=%.2f exact60=%d" % (len(fps), sum(fps)/len(fps), min(fps), max(fps), sum(1 for f in fps if f >= 59.99)))
    for r in rows[-6:]:
        print("frames=%s fps=%s pc=%s g%%=%s gmax=%s tmax=%s aotb=%s aoti=%s fb=%s miss=%s insns=%s" % r)
