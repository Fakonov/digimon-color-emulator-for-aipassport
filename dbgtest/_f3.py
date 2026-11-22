import io, re
rows = []
for ln in io.open("dbgtest/prng_soak.log", encoding="utf-8", errors="replace"):
    m = re.search(r"frames=(\d+) guest_fps=([\d.]+).*guest_pct=(\d+) .*aotmiss=(\d+).*collapse=(\d+)", ln)
    if m: rows.append(tuple(m.groups()))
print("samples: %d" % len(rows))
fps = [float(r[1]) for r in rows]
low = [(r[0], r[1]) for r in rows if float(r[1]) < 59.0]
print("min=%.2f max=%.2f" % (min(fps), max(fps)))
print("below 59:", len(low), low[:8])
print("guest_pct set:", sorted({r[2] for r in rows}))
