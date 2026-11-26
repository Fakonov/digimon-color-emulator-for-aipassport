import collections
A = {}
for line in open(r"C:\Users\DuWeisi\digimon-color-passport\tmp\r6b\A\costs.txt"):
    if line.startswith('#'): continue
    p=line.split(); pc=int(p[0],16); cyc=int(p[1]); insn=int(p[2])
    if insn: A[pc]=cyc/insn
B={}
for line in open(r"C:\Users\DuWeisi\digimon-color-passport\tmp\r6b\B\counts.txt"):
    if line.startswith('#'): continue
    p=line.split(); pc=int(p[0],16); cyc=int(p[1]); insn=int(p[2])
    if insn: B[pc]=insn
REG=[("blit_loop",0x207CA3,0x207CC6),("drawlib",0x207A00,0x207D00),("fill_loop",0x005B1E,0x005B49),
     ("push_loop",0x005B4A,0x005B8F),("mul32",0x005F8B,0x005FBF),("park",0x005000,0x0053FF),
     ("mainloop",0x230000,0x2303FF),("clock_idle",0x200340,0x20034F)]
def reg(pc):
    for n,lo,hi in REG:
        if lo<=pc<=hi: return n
    return "other"
tot=0; per=collections.Counter(); insn_per=collections.Counter(); rows=[]
for pc,insn in B.items():
    c=A.get(pc)
    if c is None: continue
    cy=int(c*insn); tot+=cy; per[reg(pc)]+=cy; insn_per[reg(pc)]+=insn; rows.append((cy,pc,insn,c))
print("composed total %d cycles = %.2f frames;  insns %d"%(tot, tot/1.6e6, sum(insn_per.values())))
print("%-12s %12s %7s %10s %6s"%("region","cycles","share","insns","CPI"))
for n,_,_ in REG+[("other",0,0)]:
    print("%-12s %12d %6.2f%% %10d %6.2f"%(n, per[n], 100.0*per[n]/tot, insn_per[n], per[n]/max(1,insn_per[n])))
print()
print("drawlib total (blit+rest) = %d (%.2f%%)"%(per["blit_loop"]+per["drawlib"], 100.0*(per["blit_loop"]+per["drawlib"])/tot))
print()
print("top 20 'other' PCs:")
oth=[r for r in rows if reg(r[1])=="other"]
oth.sort(reverse=True)
for cy,pc,insn,c in oth[:20]:
    print("  %06X cyc=%8d insn=%7d cost=%.1f  %5.2f%%"%(pc,cy,insn,c,100.0*cy/tot))
print()
pages=collections.Counter()
for cy,pc,insn,c in oth: pages[pc>>12]+=cy
print("other by 64K page:")
for pg,cy in pages.most_common(8):
    print("  page %02X %06X-%06X  %8d  %5.2f%%"%(pg,pg<<12,(pg<<12)+0xfff,cy,100.0*cy/tot))
