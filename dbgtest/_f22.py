import io, re
lines = io.open("dbgtest/walk2.log", encoding="utf-8", errors="replace").read().splitlines()
rows = [l for l in lines if "guest_fps" in l]
print("total stat lines:", len(rows))
print(rows[0])
print()
for l in rows:
    m = re.search(r"frames=\s*(\d+) guest_fps=\s*([\d.]+)", l)
    if m and (float(m.group(2)) < 20):
        print(l)
