import io, re
fps = []
for ln in io.open("dbgtest/st2.log", encoding="utf-8", errors="replace"):
    m = re.search(r"frames=(\d+) guest_fps=([\d.]+)", ln)
    if m:
        fps.append((int(m.group(1)), float(m.group(2))))
print("samples: %d" % len(fps))
for f, v in fps:
    print("  frames=%5d fps=%5.2f" % (f, v))
low = [(f, v) for f, v in fps if v < 59.5]
print("below 59.5: %d  %s" % (len(low), low[:6]))
