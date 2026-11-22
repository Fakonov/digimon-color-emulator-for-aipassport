import struct,os
base=r"C:\Users\DuWeisi\digimon-color-passport"
def dump(f,lo,hi):
    p=os.path.join(base,f); d=open(p,"rb").read(); w=struct.unpack("<%dH"%(len(d)//2),d)
    print("==",f)
    for a in range(lo,hi):
        if a*2+1<len(d): print("  %04X:%04X"%(a,w[a]),end="")
        if (a-lo)%8==7: print()
    print()
dump("dbgtest/cov/A/ram_t00000.bin",0x0C80,0x0CE0)
dump("dbgtest/emu_ram_t00200.bin",0x0C80,0x0CE0)
dump("dbgtest/emu_ram_t01000.bin",0x0C80,0x0CE0)
dump("dbgtest/emu_ram_t05000.bin",0x0C80,0x0CE0)
