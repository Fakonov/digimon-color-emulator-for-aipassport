import struct,os
root=r"C:\Users\DuWeisi\digimon-color-passport"
for f in ["ram_t17000","ram_t20000","ram_t50000","ram_t600000"]:
    p=os.path.join(root,"dbgtest","cov","A",f+".bin")
    d=open(p,"rb").read(); w=struct.unpack("<%dH"%(len(d)//2),d)
    print("%-12s" % f, " ".join("%04X"%w[0x600+i] for i in range(8)), "| 0C88=%04X 0C86=%04X 0CB1=%04X"%(w[0xC88],w[0xC86],w[0xCB1]))
