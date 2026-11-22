import io, re
txt = io.open("dbgtest/force1.log", encoding="utf-8", errors="replace").read().splitlines()
rows = [l for l in txt if "guest_fps" in l]
print("stat lines:", len(rows))
bad = 0
for l in rows:
    m = re.search(r"frames=\s*(\d+) guest_fps=\s*([\d.]+).*pc=(\w+).*aotb=(\d+) aoti=(\d+) aotfb=(\d+) aotmiss=(\d+).*gmax=(\d+) smax=(\d+) tmax=(\d+)", l)
    if m:
        f = m.groups()
        if float(f[1]) < 55:
            bad += 1
            print("frames=%5s fps=%6s pc=%s aotb=%8s aoti=%9s fb=%6s miss=%7s gmax=%6s smax=%2s tmax=%5s" % f)
print("slow samples:", bad)
