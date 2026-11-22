import sys
sys.path.insert(0, "tools/coverage")
from unsp_static import Images
img = Images("roms/dmc_v1_base.bin", "roms/internal_rom_221017a.bin")
for pc in (0x207CAE, 0x207CB4, 0x207CB3):
    w = img.word(pc)
    print("%06X = %04X (%d)" % (pc, w, w))
# full listing with imm16 resolved
def w(pc): return img.word(pc)
print()
print("imm16 @207CAE (transparent key?) = %04X" % w(0x207CAE))
print("imm16 @207CB4 (dst base lo?)      = %04X" % w(0x207CB4))
