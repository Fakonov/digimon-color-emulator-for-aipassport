import io
dev = set(int(l.split()[0], 16) for l in io.open("tools/coverage/out/AB_dev.txt", encoding="utf-8") if l.strip() and not l.startswith("#"))
force = set(int(l.split()[0], 16) for l in io.open("tools/coverage/out/AB_force.txt", encoding="utf-8") if l.strip() and not l.startswith("#"))
blocks = set(int(l.split()[0].rstrip(','), 16) for l in io.open("tools/coverage/out/AB_blocks.txt", encoding="utf-8") if l.strip() and not l.startswith("#"))
print("AB_blocks %d | AB_dev %d | AB_force %d" % (len(blocks), len(dev), len(force)))
