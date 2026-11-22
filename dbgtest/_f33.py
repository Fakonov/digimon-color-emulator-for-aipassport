import io, re
fps=[]
for l in io.open("dbgtest/iram60.log", encoding="utf-8", errors="replace"):
    m = re.search(r"guest_fps=\s*([\d.]+).*pc=(\w+).*guest_pct=(\d+).*gmax=(\d+) smax=(\d+) tmax=(\d+)", l)
    if m:
        fps.append((float(m.group(1)), m.group(2), int(m.group(3)), int(m.group(6))))
vals=[f[0] for f in fps]
print("samples:", len(vals))
print("mean=%.2f min=%.2f max=%.2f  >=55fps: %d/%d" % (sum(vals)/len(vals), min(vals), max(vals), sum(1 for v in vals if v>=55), len(vals)))
for f in fps[:35]:
    print("  fps=%6.2f pc=%s guest%%=%2d tmax=%6d" % f)
