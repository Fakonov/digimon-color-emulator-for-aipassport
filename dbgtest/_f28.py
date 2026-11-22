import io, re
tot = {}
for ln in io.open("dbgtest/hist2.txt", encoding="utf-8"):
    m = re.match(r"([0-9A-Fa-f]{6})\s+(\d+)", ln.strip())
    if m:
        tot[int(m.group(1),16)] = int(m.group(2))
for pc in (0x000321,0x000323,0x0002F4,0x230321,0x230323,0x2002F4):
    print("%06X %d" % (pc, tot.get(pc,0)))
ram = {pc:c for pc,c in tot.items() if pc < 0x4000}
print("RAM-range PCs in PC histogram:", len(ram), "weight", sum(ram.values()))
for pc,c in sorted(ram.items(), key=lambda kv:-kv[1])[:12]:
    print("  %06X %d" % (pc,c))
