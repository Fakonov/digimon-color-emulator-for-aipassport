import io, re
rows = []
for ln in io.open("dbgtest/probe2.log", encoding="utf-8", errors="replace"):
    m = re.search(r"frames=(\d+) guest_fps=([\d.]+).*pc=(\w+) sr=(\w+).*insns=(\d+) K cyc=(\d+) K .*guest_pct=(\d+) blit_pct=(\d+) blitmax=(\d+) rows=(\d+) .*slices=(\d+) .*aotb=(\d+) aoti=(\d+) aotfb=(\d+) aotmiss=(\d+).*gmax=(\d+) smax=(\d+)", ln)
    if m: rows.append(m.groups())
print("samples", len(rows))
hdr = ["frame","fps","pc","sr","insnsK","cycK","g%","b%","bmax","rows","slices","aotb","aoti","aotfb","miss","gmax","smax"]
print(" ".join("%7s" % h for h in hdr))
for r in rows:
    print(" ".join("%7s" % v for v in r))
