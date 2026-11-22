import io, re
rows = []
for ln in io.open("dbgtest/prng1.log", encoding="utf-8", errors="replace"):
    m = re.search(r"frames=(\d+) guest_fps=([\d.]+).*pc=(\w+).*guest_pct=(\d+) blit_pct=(\d+).*collapse=(\d+) collcalls=(\d+)", ln)
    if m:
        rows.append(m.groups())
print("samples: %d" % len(rows))
for r in rows[-12:]:
    print("  frames=%5s fps=%6s pc=%s guest%%=%s blit%%=%s collapse=%s calls=%s" % r)
if rows:
    fps = [float(r[1]) for r in rows]
    print("min=%.2f max=%.2f mean=%.2f" % (min(fps), max(fps), sum(fps)/len(fps)))
