import re, io, sys
path = sys.argv[1] if len(sys.argv) > 1 else "dbgtest/hle2.log"
rows = []
for ln in io.open(path, encoding="utf-8", errors="replace"):
    m = re.search(r"frames=(\d+) guest_fps=([\d.]+).*?pc=([0-9a-f]+).*?guest_pct=(\d+).*?gmax=(\d+) smax=(\d+) tmax=(\d+) kmax=(\d+).*?aotb=(\d+) aoti=(\d+) aotfb=(\d+) aotmiss=(\d+)", ln)
    if m:
        rows.append(tuple(m.groups()))
if not rows:
    print("no samples in", path)
    print(io.open(path, encoding="utf-8", errors="replace").read()[-1200:])
else:
    fps = [float(r[1]) for r in rows]
    print("%s samples=%d mean=%.2f min=%.2f max=%.2f exact60=%d" % (path, len(fps), sum(fps)/len(fps), min(fps), max(fps), sum(1 for f in fps if f >= 59.99)))
    for r in rows[-10:]:
        print("frames=%s fps=%s pc=%s g%%=%s gmax=%s smax=%s tmax=%s kmax=%s aotb=%s aoti=%s fb=%s miss=%s" % r)
