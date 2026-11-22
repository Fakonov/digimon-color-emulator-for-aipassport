import io, re
rows = []
for ln in io.open("dbgtest/walk1.log", encoding="utf-8", errors="replace"):
    m = re.search(r"frames=(\d+) guest_fps=([\d.]+).*insns=(\d+) K cyc=(\d+) K .*guest_pct=(\d+).*aotb=(\d+) aoti=(\d+) aotfb=(\d+) aotmiss=(\d+).*gmax=(\d+)", ln)
    if not m: continue
    g = m.groups()
    rows.append((int(g[0]), float(g[1]), int(g[2]), int(g[3]), int(g[4]), int(g[5]), int(g[6]), int(g[7]), int(g[8]), int(g[9])))
prev = None
print(" frame  fps  insns/frame  cyc/frame  gmax_us  ns/insn  miss/frame")
for r in rows:
    if prev and r[0] > prev[0]:
        df = r[0]-prev[0]
        di = (r[2]-prev[2])*1000
        dc = (r[3]-prev[3])*1000
        dm = r[8]-prev[8]
        # wall ns per instruction assuming the frame took 1e9/fps ns total
        frame_ns = 1e9 / max(r[1], 0.1)
        print("%6d %6.1f %11.0f %10.0f %8d %8.1f %10.0f" % (r[0], r[1], di/df, dc/df, r[9], frame_ns/max(di/df,1), dm/df))
    prev = r
