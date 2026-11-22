import sys
sys.path.insert(0, "tools/coverage")
from unsp_static import Images, decode
img = Images("roms/dmc_v1_base.bin", "roms/internal_rom_221017a.bin")
for pc in [0x207CB8,0x207CB9,0x207CBA,0x207CBB,0x207CBC,0x207CBD,0x207CBE,0x207CBF,0x207CC0,0x207CC1,0x207A8F,0x005B5D,0x230334]:
    L, kind, tgt, op = decode(img, pc)
    print("%06X len=%d kind=%-8s tgt=%s op=%04X" % (pc, L, kind, ("%06X"%tgt) if tgt is not None else "-", op))
