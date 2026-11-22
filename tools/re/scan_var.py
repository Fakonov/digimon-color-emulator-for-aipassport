import sys, re
sys.path.insert(0, r"C:\Users\DuWeisi\digimon-color-passport\tools\coverage")
import unsp_static as U
root = r"C:\Users\DuWeisi\digimon-color-passport"
flash = open(root + r"\roms\dmc_v1_base.bin","rb").read()
irom  = open(root + r"\roms\internal_rom_221017a.bin","rb").read()
img = U.Images(flash, irom)

def scan(lo, hi, targets, usec=False):
    pc = lo; hits=[]
    while pc < hi:
        try:
            d = U.decode(img, pc)
        except Exception as e:
            pc += 1; continue
        if d is None:
            pc += 1; continue
        txt = getattr(d, "text", None) or str(d)
        for t in targets:
            if ("%04x"%t) in txt.lower():
                hits.append((pc, txt))
        pc += getattr(d, "size", 1)
    return hits

h = scan(0x200020, 0x200B00, [0x0C86, 0x0C87, 0x0CAD, 0x0CB2])
for pc,t in h[:80]:
    print("%06X: %s" % (pc, t))
print("total", len(h))
