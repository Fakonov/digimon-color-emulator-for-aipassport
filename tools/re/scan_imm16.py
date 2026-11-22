import sys
sys.path.insert(0, r"C:\Users\DuWeisi\digimon-color-passport\tools\coverage")
import unsp_static as U
root = r"C:\Users\DuWeisi\digimon-color-passport"
img = U.Images(root + r"\roms\dmc_v1_base.bin", root + r"\roms\internal_rom_221017a.bin")
IMM16 = {0x9108,0x9309,0x950A,0x970B,0x990C,0x9B0D,0x9D0E,0x9F0F}
out = open(root + r"\dbgtest\re\imm16_pairs.txt","w",encoding="utf-8")
pc = 0x200000
n = 0
while pc < 0x3FFFFF:
    w = img.word(pc)
    if w is None: break
    ln, kind, tgt, op = U.decode(img, pc)
    if ln == 2:
        nw = img.word(pc+1)
        reg = (w >> 9) & 7
        out.write("%06X r%d %04X\n" % (pc, reg, nw))
        n += 1
    pc += ln
out.close()
print("pairs", n)
