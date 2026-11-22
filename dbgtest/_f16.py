import io, re
rows = []
for ln in io.open("dbgtest/probe2.log", encoding="utf-8", errors="replace"):
    m = re.search(r"frames=(\d+) guest_fps=([\d.]+).*pc=(\w+) sr=(\w+).*guest_pct=(\d+) blit_pct=(\d+) .*slices=(\d+) .*gmax=(\d+) smax=(\d+)", ln)
    if m: rows.append(tuple(m.groups()))
print("samples", len(rows))
print("frame  fps   pc      guest% blit% slices gmax_us smax")
for r in rows:
    print("%5s %6s %s %5s %5s %6s %7s %5s" % r)
