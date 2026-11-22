import struct
root = r"C:\Users\DuWeisi\digimon-color-passport"
flash = open(root + r"\roms\dmc_v1_base.bin","rb").read()
def w(a): return struct.unpack_from("<H", flash, (a-0x200000)*2)[0]
lines = ["UI state dispatch table at 0x200623 (entry = 2 words: offset in segment 0x20, segment)"]
for i in range(24):
    off = w(0x200623 + 2*i); seg = w(0x200624 + 2*i)
    lines.append("state %02X -> %04X:%04X  (linear %06X)" % (i, seg, off, 0x200000 + off if seg == 0x20 else -1))
open(root + r"\dbgtest\re\ui_state_table.txt","w",encoding="utf-8").write("\n".join(lines) + "\n")
print("\n".join(lines))
