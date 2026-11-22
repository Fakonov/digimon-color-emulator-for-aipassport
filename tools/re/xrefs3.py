from collections import defaultdict
base=r"C:\Users\DuWeisi\digimon-color-passport"
IMM={0x9108:0,0x9309:1,0x950A:2,0x970B:3,0x990C:4,0x9B0D:5,0x9D0E:6,0x9F0F:7}
imgs=[("irom",open(base+r"\roms\internal_rom_221017a.bin","rb").read(),0x4000),
      ("flash",open(base+r"\roms\dmc_v1_base.bin","rb").read(),0x200000)]
for name,img,org in imgs:
    n=len(img)//2
    for target in (0x085C,0x085D,0x085B,0x085E,0x085F,0x0853,0x0854,0x0855,0x0856):
        hits=[]
        for i in range(n-1):
            op=img[i*2]|(img[i*2+1]<<8); nxt=img[i*2+2]|(img[i*2+3]<<8)
            if op in IMM and nxt==target: hits.append(org+i)
        print("%s %04X n=%d %s"%(name,target,len(hits)," ".join("%06X"%a for a in hits[:20])))
