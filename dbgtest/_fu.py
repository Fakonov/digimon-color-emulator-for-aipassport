import sys
sys.path.insert(0, "tools/coverage")
from unsp_static import Images
img = Images("roms/dmc_v1_base.bin", "roms/internal_rom_221017a.bin")
def show(base, n=26):
    for k in range(n):
        pc = base + k
        op = img.word(pc)
        op0 = (op >> 12) & 15; opa = (op >> 9) & 7; op1 = (op >> 6) & 7
        opn = (op >> 3) & 7; opb = op & 7
        print("  %06X %04X  op0=%X opa=%d op1=%X opn=%d opb=%d" % (pc, op, op0, opa, op1, opn, opb))
print("=== 0x207A57 (7.5%% of misses)")
show(0x207A57, 24)
print("=== 0x207A73 (33.1%% of misses)")
show(0x207A73, 18)
