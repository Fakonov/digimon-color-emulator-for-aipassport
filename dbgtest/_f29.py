import io, re
txt = io.open("dbgtest/ramloop1.log", encoding="utf-8", errors="replace").read().splitlines()
fps = []
for l in txt:
    m = re.search(r"guest_fps=\s*([\d.]+).*pc=(\w+).*collapse=(\d+) collcalls=(\d+).*gmax=(\d+)", l)
    if m:
        fps.append((float(m.group(1)), m.group(2), int(m.group(3)), int(m.group(4)), int(m.group(5))))
vals=[f[0] for f in fps]
print("samples:", len(vals), "mean=%.2f min=%.2f max=%.2f" % (sum(vals)/len(vals), min(vals), max(vals)))
low=[f for f in fps if f[0]<55]
print("below 55:", len(low))
for f in low[:20]:
    print("  fps=%6.2f pc=%s collapse=%7d collcalls=%5d gmax=%6d" % f)
