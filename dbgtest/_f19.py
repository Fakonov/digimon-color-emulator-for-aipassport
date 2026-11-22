import io, re
rows = []
for ln in io.open("dbgtest/walk1.log", encoding="utf-8", errors="replace"):
    m = re.search(r"frames=(\d+) guest_fps=([\d.]+).*insns=(\d+) K cyc=(\d+) K .*guest_pct=(\d+).*aotb=(\d+) aoti=(\d+) aotfb=(\d+) aotmiss=(\d+).*gmax=(\d+)", ln)
    if m: rows.append([int(x) for x in m.groups()])
prev = None
print(" frame  fps  insns/frame  cyc/frame  gmax_us  insns/cyc  miss/frame")
for r in rows:
    if prev and r[0] > prev[0]:
        df = r[0]-prev[0]
        di = (r[2]-prev[2])*1000
        dc = (r[3]-prev[3])*1000
        dm = r[8]-prev[8]
        print("%6d %5d %11.0f %10.0f %8d %9.3f %10.0f" % (r[0], r[1], di/df, dc/df, r[9], di/max(dc,1), dm/df))
    prev = r
