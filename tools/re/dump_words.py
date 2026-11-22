import struct,sys,os
base=r"C:\Users\DuWeisi\digimon-color-passport"
files=["dbgtest/emu_ram_t00200.bin","dbgtest/mame_ram_59cc.bin","dbgtest/emu_ram_t01000.bin","dbgtest/emu_ram_t05000.bin","dbgtest/cov/A/ram_t00000.bin"]
for f in files:
    p=os.path.join(base,f)
    if not os.path.exists(p): print(f,"MISSING"); continue
    d=open(p,"rb").read()
    w=struct.unpack("<%dH"%(len(d)//2),d)
    print("==",f,len(d))
    for a in list(range(0x600,0x608))+[0xc86,0xc9a,0xcb1,0xcb0,0xcb2,0xcad,0x850,0x851,0xc9d,0xc9e,0xca0,0xca1,0xca2,0xca3,0xca4]:
        if a*2+1 < len(d): print("  %04X = %04X"%(a,w[a]))
