import io, re
rows = []
for ln in io.open("dbgtest/ship5.log", encoding="utf-8", errors="replace"):
    m = re.search(r"frames=(\d+) guest_fps=([\d.]+).*guest_pct=(\d+) blit_pct=(\d+)", ln)
    if m: rows.append(tuple(m.groups()))
fps = [float(r[1]) for r in rows]
print("samples=%d min=%.2f max=%.2f mean=%.3f" % (len(fps), min(fps), max(fps), sum(fps)/len(fps)))
print("below 59.5: %d, exactly 60: %d" % (sum(1 for v in fps if v < 59.5), sum(1 for v in fps if v >= 59.995)))
for r in rows[-4:]: print("  frames=%5s fps=%6s guest%%=%s blit%%=%s" % r)
