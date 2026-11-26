A={}
for line in open(r"C:\Users\DuWeisi\digimon-color-passport\tmp\r6b\A\costs.txt"):
    if line.startswith('#'): continue
    p=line.split(); pc=int(p[0],16); cyc=int(p[1]); insn=int(p[2])
    if insn: A[pc]=cyc/insn
store=0; skip=0; n=0
for pc in range(0x207CA3,0x207CC5):
    c=A.get(pc)
    if c is None: 
        print("missing", hex(pc)); continue
    n+=1
    if 0x207CB0 <= pc <= 0x207CBA:
        skip += c
    store += c
print("loop instructions 0x207CA3..0x207CC4:", n)
print("full (store) path cycles/pixel: %.1f"%store)
print("key path (skip 0x207CB0..0x207CBA) cycles/pixel: %.1f"%skip)
print("skip-path instructions: 11, their cycles: %.1f"%(store-skip))
mean = 1154486/9240
print("measured mean: %.1f cycles/px over 9240 px" % mean)
print("implied key-pixel share: %.1f%%" % (100.0*(store-mean)/(store-skip) if store!=skip else 0))
