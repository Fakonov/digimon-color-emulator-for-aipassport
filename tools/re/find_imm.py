import struct,sys
base=r"C:\Users\DuWeisi\digimon-color-passport"
irom=open(base+r"\roms\internal_rom_221017a.bin","rb").read()
flash=open(base+r"\roms\dmc_v1_base.bin","rb").read()
# unSP: rN = imm16 encodings seen in dasm: 990C (r4), 970B (r3), 9909? Let's scan all words where low byte pattern 0x0C9? etc.
# Instead: brute force all word positions w such that next word in a target set and opcode in known imm16 set.
# Known from unsp_other.cpp: opcodes 0x9xxx with op1==4? We'll scan opcodes that dasm renders as imm and check next word.
targets=set([0x0600+i for i in range(8)]+[0x0c80+i for i in range(0x40)]+[0x0c86,0x0c9a,0x0cb1,0x0cb0,0x0cb2,0x0cad])
def scan(img,name,isflash):
    hits=[]
    n=len(img)//2
    for i in range(n-1):
        op=img[i*2]|(img[i*2+1]<<8)
        nxt=img[i*2+2]|(img[i*2+3]<<8)
        if nxt in targets and (op & 0xF800)==0x9800:
            hits.append((i,op,nxt))
    print("==",name,len(hits))
    for i,op,nxt in hits[:200]:
        pc = (0x200000+i) if isflash else (0x4000+i)
        print("  %06X op=%04X imm=%04X"%(pc,op,nxt))
scan(irom,"irom",False)
scan(flash,"flash",True)
