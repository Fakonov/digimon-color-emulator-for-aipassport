base=r"C:\Users\DuWeisi\digimon-color-passport"
irom=open(base+r"\roms\internal_rom_221017a.bin","rb").read()
flash=open(base+r"\roms\dmc_v1_base.bin","rb").read()
def scan(img,name,targets,isflash,limit=200):
    n=len(img)//2; hits=[]
    for i in range(n-1):
        op=img[i*2]|(img[i*2+1]<<8)
        nxt=img[i*2+2]|(img[i*2+3]<<8)
        if nxt in targets and (op & 0xF800)==0x9800:
            pc=(0x200000+i) if isflash else (0x4000+i)
            hits.append((pc,op))
    print("==",name,[hex(t) for t in targets],"hits",len(hits))
    for pc,op in hits[:limit]: print("  %06X op=%04X"%(pc,op))
for t in (0x0CAE,0x0CAF,0x0D03,0x0CA9,0x0CA8):
    scan(irom,"irom",{t},False); scan(flash,"flash",{t},True,80)
