import io, re
for ln in io.open("dbgtest/walk2.log", encoding="utf-8", errors="replace"):
    m = re.search(r"frames=(\d+) guest_fps=([\d.]+).*pc=(\w+).*guest_pct=(\d+) blit_pct=(\d+).*gmax=(\d+) smax=(\d+) tmax=(\d+) kmax=(\d+)", ln)
    if m:
        f = m.groups()
        if float(f[1]) < 55 or float(f[1]) > 59.9:
            print("frames=%5s fps=%6s pc=%s g%%=%2s b%%=%2s gmax=%6s smax=%2s tmax=%6s kmax=%5s" % f)
