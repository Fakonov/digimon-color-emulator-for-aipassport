import io, re
rows = []
for ln in io.open("dbgtest/walk1.log", encoding="utf-8", errors="replace"):
    m = re.search(r"frames=(\d+) guest_fps=([\d.]+).*pc=(\w+) .*guest_pct=(\d+) blit_pct=(\d+) .*aotmiss=(\d+) .*gmax=(\d+) smax=(\d+)", ln)
    if m: rows.append(m.groups())
print("samples", len(rows))
print("frame   fps   pc      g%  b%   miss   gmax  smax")
slow = []
for r in rows:
    print("%5s %6s %s %3s %3s %7s %6s %4s" % r)
    if float(r[1]) < 55: slow.append(r)
print("\n--- frames below 55 fps: %d ---" % len(slow))
for r in slow[:20]: print("  frames=%s fps=%s pc=%s g%%=%s b%%=%s miss=%s gmax=%s" % r)
