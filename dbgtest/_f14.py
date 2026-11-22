import io, re
rows = []
for ln in io.open("dbgtest/log5s.log", encoding="utf-8", errors="replace"):
    m = re.search(r"frames=(\d+) guest_fps=([\d.]+).*pc=(\w+) .*guest_pct=(\d+) blit_pct=(\d+)", ln)
    if m: rows.append(tuple(m.groups()))
print("samples", len(rows))
for r in rows: print("  frames=%s fps=%s pc=%s guest=%s blit=%s" % r)
