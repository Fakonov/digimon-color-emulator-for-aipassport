import io, re
rows = [l for l in io.open("dbgtest/ramloop1.log", encoding="utf-8", errors="replace").read().splitlines() if "guest_fps" in l]
print("n=", len(rows))
print(rows[0])
print()
for l in rows:
    m = re.search(r"frames=\s*(\d+) guest_fps=\s*([\d.]+)", l)
    if m and float(m.group(2)) < 25:
        print(l)
