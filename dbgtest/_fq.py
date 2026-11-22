import re, io, collections
# where does the 34% interpreter fallback actually sit?  use the block census log
pcs = collections.Counter()
for ln in io.open("dbgtest/miss3.log", encoding="utf-8", errors="replace"):
    m = re.match(r"MISS ([0-9A-F]{6}) (\d+)", ln.strip())
    if m: pcs[m.group(1)] = int(m.group(2))
tot = sum(pcs.values())
print("census rows=%d hits=%d" % (len(pcs), tot))
for i, (pc, c) in enumerate(pcs.most_common(12)):
    print("  %2d. %s %6d (%.1f%%)" % (i+1, pc, c, 100.0*c/tot))
