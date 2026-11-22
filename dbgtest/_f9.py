import io, re
rows = []
for ln in io.open("dbgtest/final1.log", encoding="utf-8", errors="replace"):
    m = re.search(r"frames=(\d+) guest_fps=([\d.]+).*guest_pct=(\d+) blit_pct=(\d+) .*blits=(\d+)", ln)
    if m: rows.append(tuple(m.groups()))
fps = [float(r[1]) for r in rows[2:]]
if fps:
    print("samples=%d min=%.2f max=%.2f mean=%.3f  exact60=%d" % (len(fps), min(fps), max(fps), sum(fps)/len(fps), sum(1 for v in fps if v >= 59.995)))
for r in rows[-3:]: print("  frames=%5s fps=%6s guest%%=%s blit%%=%s blits=%s" % r)
