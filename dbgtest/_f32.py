import io, re
txt = io.open("dbgtest/blk2.log", encoding="utf-8", errors="replace").read().splitlines()
blk = [l for l in txt if l.startswith("BLK ")]
print("BLK lines:", len(blk), [l for l in txt if l.startswith("BLKHEAD")])
tot = 0
rows = []
for l in blk:
    m = re.match(r"BLK ([0-9A-F]{6}) (\d+)", l)
    if m:
        rows.append((m.group(1), int(m.group(2))))
        tot += int(m.group(2))
print("total dispatch:", tot)
cum = 0
for i, (pc, c) in enumerate(rows, 1):
    cum += c
    if i in (20, 40, 60, 80, 100, 120, 150, 200):
        print("  top%-4d covers %.1f%%  (last %s %d)" % (i, 100.0*cum/tot, pc, c))
print("first 30:", " ".join(p for p, _ in rows[:30]))
