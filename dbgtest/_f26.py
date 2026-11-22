import io, re
txt = io.open("dbgtest/noaot1.log", encoding="utf-8", errors="replace").read().splitlines()
rows = [l for l in txt if "guest_fps" in l]
fps = []
for l in rows:
    m = re.search(r"guest_fps=\s*([\d.]+).*pc=(\w+).*gmax=(\d+) smax=(\d+) tmax=(\d+)", l)
    if m:
        fps.append((float(m.group(1)), m.group(2), int(m.group(3)), int(m.group(5))))
vals = [f[0] for f in fps]
print("samples:", len(vals), "mean=%.2f min=%.2f max=%.2f" % (sum(vals)/len(vals), min(vals), max(vals)))
bad = sorted([f for f in fps if f[0] < 55])
print("below 55 fps:", len(bad))
for f in bad[:25]:
    print("  fps=%6.2f pc=%s gmax=%6d tmax=%6d" % f)
