import struct
root=r"C:\Users\DuWeisi\digimon-color-passport"
keys=[0x0600,0x0601,0x0602,0x0603,0x0C86,0x0C87,0x0C9A,0x0CB1,0x0C88,0x0C89]
import os
for f in ["dbgtest\\emu_ram_t00200.bin","dbgtest\\emu_ram_t01000.bin","dbgtest\\emu_ram_t05000.bin"]:
    d=open(os.path.join(root,f),"rb").read(); w=struct.unpack("<%dH"%(len(d)//2),d)
    print("%-28s " % os.path.basename(f) + " ".join("%04X"%w[k] for k in keys))
