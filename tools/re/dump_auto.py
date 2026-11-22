import struct,os
base=r"C:\Users\DuWeisi\digimon-color-passport\dbgtest\re"
keys=[0x0CAF,0x0CAE,0x0CAD,0x0CB0,0x0CB1,0x0CB2,0x0D03,0x0D07,0x085C,0x085D,0x0850,0x0CB4,0x0CB5,0x0CB3]
print("t     "+" ".join("%04X"%k for k in keys))
for t in range(2,9):
    p=os.path.join(base,"emu_auto_t%d.bin"%t); d=open(p,"rb").read(); w=struct.unpack("<%dH"%(len(d)//2),d)
    print("%-5d "%t+" ".join("%04X"%w[k] for k in keys))
