import io, re
rows = []
for ln in io.open("dbgtest/soak60.log", encoding="utf-8", errors="replace"):
    m = re.search(r"frames=(\d+) guest_fps=([\d.]+).*pc=(\w+) .*guest_pct=(\d+) blit_pct=(\d+).*blits=(\d+)", ln)
    if m: rows.append(tuple(m.groups()))
fps = [float(r[1]) for r in rows[1:]]
print("60s soak  samples=%d min=%.2f max=%.2f mean=%.3f exact60=%d/%d" % (len(fps), min(fps), max(fps), sum(fps)/len(fps), sum(1 for v in fps if v>=59.995), len(fps)))
bad = [r for r in rows[1:] if float(r[1]) < 59.9]
print("samples below 59.9: %d" % len(bad))
for r in bad[:5]: print("  frames=%5s fps=%6s pc=%s guest%%=%s blit%%=%s" % r)
print("--- last ---")
for r in rows[-2:]: print("  frames=%5s fps=%6s pc=%s guest%%=%s blit%%=%s blits=%s" % r)
pcs = {}
for r in rows[1:]: pcs[r[2]] = pcs.get(r[2], 0) + 1
print("pc distribution:", sorted(pcs.items(), key=lambda kv: -kv[1])[:6])
