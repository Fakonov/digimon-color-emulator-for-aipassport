import io, re
rows = []
for ln in io.open("dbgtest/soak60.log", encoding="utf-8", errors="replace"):
    m = re.search(r"frames=(\d+) guest_fps=([\d.]+).*pc=(\w+) .*guest_pct=(\d+) blit_pct=(\d+) .*blits=(\d+)", ln)
    if m: rows.append(tuple(m.groups()))
print("total samples", len(rows))
bad = [r for r in rows[1:] if float(r[1]) < 59.9]
print("below 59.9:", len(bad))
for r in bad:
    print("  frames=%s fps=%s pc=%s guest=%s blit=%s blits=%s" % r)
pcs = {}
for r in rows[1:]:
    pcs[r[2]] = pcs.get(r[2], 0) + 1
print("pc distribution:", sorted(pcs.items(), key=lambda kv: -kv[1])[:8])
