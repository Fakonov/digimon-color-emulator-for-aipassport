import io, re
rows = []
for ln in io.open("dbgtest/log5s.log", encoding="utf-8", errors="replace"):
    m = re.search(r"\((\d+)\) DIGICORE: frames=(\d+) guest_fps=([\d.]+).*guest_pct=(\d+) blit_pct=(\d+)", ln)
    if m: rows.append(tuple(int(m.group(1))==int(m.group(1)) and m.groups() or m.groups()))
rows = [(int(a),int(b),float(c),int(d),int(e)) for a,b,c,d,e in rows]
# use steady region: from first sample with fps>=59.9
start = next(i for i,r in enumerate(rows) if r[2] >= 59.9 and r[0] > 5000)
a, b = rows[start], rows[-1]
dt = (b[0]-a[0])/1e6
df = b[1]-a[1]
print("window %.3f s, %d frames -> %.3f fps" % (dt, df, df/dt))
# per-second
prev = None
bad = 0
for r in rows[start:]:
    if prev:
        dt = (r[0]-prev[0])/1e6
        if dt > 0.5:
            f = (r[1]-prev[1])/dt
            if f < 59.5: bad += 1
    prev = r
print("windows counted=%d, below 59.5: %d" % (len(rows)-start-1, bad))
