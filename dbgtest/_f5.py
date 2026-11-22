import io, re
rows = []
for ln in io.open("dbgtest/ship2.log", encoding="utf-8", errors="replace"):
    m = re.search(r"frames=(\d+) guest_fps=([\d.]+).*guest_pct=(\d+) blit_pct=(\d+) blitmax=(\d+) rows=(\d+) .*aotmiss=(\d+) .*blits=(\d+)", ln)
    if m: rows.append(tuple(m.groups()))
print("samples: %d" % len(rows))
fps = [float(r[1]) for r in rows]
print("min=%.2f max=%.2f" % (min(fps), max(fps)))
print("below 59:", [(r[0], r[1]) for r in rows if float(r[1]) < 59.0])
for r in rows[-6:]:
    print("  frames=%5s fps=%6s guest%%=%s blit%%=%s blitmax=%s rows=%s miss=%s blits=%s" % r)
print("SELFTEST lines:", sum(1 for ln in io.open("dbgtest/ship2.log", encoding="utf-8", errors="replace") if "SELFTEST" in ln))
