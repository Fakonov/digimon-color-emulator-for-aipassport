import sys
sys.path.insert(0, r"C:\Users\DuWeisi\digimon-color-passport\tools\coverage")
import unsp_static as U
root = r"C:\Users\DuWeisi\digimon-color-passport"
img = U.Images(root + r"\roms\dmc_v1_base.bin", root + r"\roms\internal_rom_221017a.bin")
pc = 0x200000
hits = []
while pc < 0x3FFFFF:
    w = img.word(pc)
    if w is None: break
    ln, kind, tgt, op = U.decode(img, pc)
    if ln == 2 and w == 0x990C and img.word(pc+1) == 0x0C86:
        nxt = [img.word(pc+2+2*k) for k in range(8)]
        if any((x & 0xFFC0) == 0x2840 for x in nxt) or 0x6641 in nxt:
            hits.append((pc, nxt))
    pc += ln
for pc, nxt in hits:
    print("%06X: %s" % (pc, " ".join("%04X"%x for x in nxt)))
print("hits", len(hits), " scanned to", hex(pc))
