import sys, collections
A = {}
for line in open(r"C:\Users\DuWeisi\digimon-color-passport\tmp\r6b\A\costs.txt"):
    if line.startswith('#'): continue
    p = line.split()
    pc = int(p[0],16); cyc = int(p[1]); insn = int(p[2])
    if insn: A[pc] = cyc/insn
B = {}
for line in open(r"C:\Users\DuWeisi\digimon-color-passport\tmp\r6b\B\counts.txt"):
    if line.startswith('#'): continue
    p = line.split()
    pc = int(p[0],16); cyc = int(p[1]); insn = int(p[2])
    if insn: B[pc] = insn
REG = [("blit_loop",0x207CA3,0x207CC6),("drawlib",0x207A00,0x207D00),("fill_loop",0x005B1E,0x005B49),
       ("push_loop",0x005B4A,0x005B8F),("mul32",0x005F8B,0x005FBF),("park",0x005000,0x0053FF),
       ("mainloop",0x230000,0x2303FF),("clock_idle",0x200340,0x20034F)]
tot=0; per=collections.Counter(); ins_tot=0; unknown=0
for pc,insn in B.items():
    c = A.get(pc)
    if c is None: unknown += insn; continue
    cy = c*insn
    tot += cy; ins_tot += insn
    name="other"
    for n,lo,hi in REG:
        if lo<=pc<=hi: name=n; break
    per[name]+=cy
print("composed cycles: %d (%.2f frames of 1.6M)"%(tot, tot/1.6e6))
print("instructions covered:", ins_tot, " uncovered:", unknown)
for n,lo,hi in REG:
    print("  %-11s %10.0f  %5.2f%%"%(n, per[n], 100.0*per[n]/tot))
print("  %-11s %10.0f  %5.2f%%"%("other", per["other"] if "other" in per else 0, 100.0*(per["other"] if "other" in per else 0)/tot))
print()
print("top 15 PCs by composed cycles:")
rows=[]
for pc,insn in B.items():
    c=A.get(pc)
    if c: rows.append((c*insn, pc, insn, c))
rows.sort(reverse=True)
for cy,pc,insn,c in rows[:15]:
    print("  %06X cyc=%9.0f insn=%8d cost/insn=%.2f"%(pc,cy,insn,c))
