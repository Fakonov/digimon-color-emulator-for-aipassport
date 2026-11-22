import io, re
rows = []
for ln in io.open("dbgtest/guard1.log", encoding="utf-8", errors="replace"):
    m = re.search(r"frames=(\d+) guest_fps=([\d.]+).*guest_pct=(\d+) blit_pct=(\d+) blitmax=(\d+) rows=(\d+) .*blits=(\d+)", ln)
    if m: rows.append(tuple(m.groups()))
print("samples: %d" % len(rows))
for r in rows[:6]: print("  start frames=%5s fps=%6s guest%%=%s blit%%=%s blitmax=%s rows=%s blits=%s" % r)
print("  ...")
for r in rows[-8:]: print("  end   frames=%5s fps=%6s guest%%=%s blit%%=%s blitmax=%s rows=%s blits=%s" % r)
fps = [float(r[1]) for r in rows]
print("min=%.2f  below59=%d" % (min(fps), sum(1 for v in fps if v < 59.0)))
