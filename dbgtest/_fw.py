import re, io
rows = []
for ln in io.open("dbgtest/hle1.log", encoding="utf-8", errors="replace"):
    m = re.search(r"frames=(\d+) guest_fps=([\d.]+).*?pc=([0-9a-f]+).*?guest_pct=(\d+).*?gmax=(\d+) smax=(\d+) tmax=(\d+) kmax=(\d+)", ln)
    if m:
        rows.append(tuple(m.groups()))
if rows:
    fps = [float(r[1]) for r in rows]
    print("samples=%d mean=%.2f min=%.2f max=%.2f exact60=%d" % (len(fps), sum(fps)/len(fps), min(fps), max(fps), sum(1 for f in fps if f >= 59.99)))
    for r in rows[-8:]:
        print("frames=%s fps=%s pc=%s g%%=%s gmax=%s smax=%s tmax=%s kmax=%s" % r)
else:
    print("no samples")
    print(io.open("dbgtest/hle1.log", encoding="utf-8", errors="replace").read()[-1500:])
