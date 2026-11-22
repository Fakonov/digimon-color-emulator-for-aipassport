
import os
for f in ['PROGRESS.md','docs/HANDOFF.md','docs/FEASIBILITY.md']:
 p=os.path.join(r'C:\Users\DuWeisi\digimon-color-passport',f)
 s=open(p,encoding='utf-8').read()
 print(f, os.path.getsize(p), 'chars', len(s), 'lines', s.count(chr(10))+1)
