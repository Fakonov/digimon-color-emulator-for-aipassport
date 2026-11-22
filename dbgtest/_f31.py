import sys
sys.path.insert(0, "tools/coverage")
from unsp_static import Images, decode
img = Images("roms/dmc_v1_base.bin", "roms/internal_rom_221017a.bin")
def dis(pc):
    L, kind, tgt, op = decode(img, pc)
    op0=(op>>12)&15; opa=(op>>9)&7; op1=(op>>6)&7; opn=(op>>3)&7; opb=op&7
    return "%06X: %04X  op0=%X opa=%d op1=%X opn=%d opb=%d  %s%s" % (
        pc, op, op0, opa, op1, opn, opb, kind, (" -> %06X"%tgt) if tgt is not None else "")
pc = 0x207CA3
for i in range(70):
    print(dis(pc))
    L, kind, tgt, op = decode(img, pc)
    pc = (pc + (L or 1)) & 0x3FFFFF
