import io, re
rows = []
for ln in io.open("dbgtest/ship3.log", encoding="utf-8", errors="replace"):
    m = re.search(r"frames=(\d+) guest_fps=([\d.]+).*guest_pct=(\d+) blit_pct=(\d+)", ln)
    if m: rows.append(tuple(m.groups()))
print("samples: %d" % len(rows))
fps = [float(r[1]) for r in rows]
print("min=%.2f max=%.2f mean=%.3f" % (min(fps), max(fps), sum(fps)/len(fps)))
print("below 59.5:", sum(1 for v in fps if v < 59.5), "of", len(fps))
print("exactly 60.00:", sum(1 for v in fps if v >= 59.995))
for r in rows[-5:]: print("  frames=%5s fps=%6s guest%%=%s blit%%=%s" % r)
