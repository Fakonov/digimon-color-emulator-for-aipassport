import io, re
rows = []
for ln in io.open("dbgtest/g6.log", encoding="utf-8", errors="replace"):
    m = re.search(r"frames=(\d+) guest_fps=([\d.]+).*guest_pct=(\d+) blit_pct=(\d+).*blitmax=(\d+) rows=(\d+)", ln)
    if m: rows.append(tuple(m.groups()))
fps = [float(r[1]) for r in rows[2:]]
print("kGuardRows=6  samples=%d min=%.2f max=%.2f mean=%.3f exact60=%d/%d" % (len(fps), min(fps), max(fps), sum(fps)/len(fps), sum(1 for v in fps if v>=59.995), len(fps)))
for r in rows[-3:]: print("  frames=%5s fps=%6s guest%%=%s blit%%=%s blitmax=%s rows=%s" % r)
