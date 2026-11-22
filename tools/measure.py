
import os
base=r'C:\Users\DuWeisi\digimon-color-passport'
files=[
 'docs/FEASIBILITY.md',
 'PROGRESS.md',
 'docs/HANDOFF.md',
 'dbgtest/g2s.tr',
 'dbgtest/analyze_official.py',
 'dbgtest/pcpages.py',
 'ref/unsp2.0.pdf',
 'gateA_keepalive.avi',
 'gateA_keepalive_f2400.png',
 'gateA_keepalive_f2863.png',
]
for f in files:
    p=os.path.join(base,f)
    if not os.path.exists(p):
        print(f,'MISSING'); continue
    b=os.path.getsize(p)
    if f.endswith(('.md','.py')):
        s=open(p,encoding='utf-8').read()
        print(f, b, 'chars',len(s),'lines',s.count(chr(10))+1,'nonempty',sum(1 for l in s.split(chr(10)) if l.strip()))
    else:
        print(f,b)
