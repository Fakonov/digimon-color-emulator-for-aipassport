import io, re
txt = io.open("dbgtest/blkcensus.log", encoding="utf-8", errors="replace").read().splitlines()
blk = [l for l in txt if l.startswith("BLK ")]
print("BLK lines:", len(blk))
for l in blk[:70]:
    print(l)
miss = [l for l in txt if l.startswith("MISS ")]
print("MISS lines:", len(miss))
for l in miss[:25]:
    print(l)
