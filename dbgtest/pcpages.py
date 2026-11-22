
import re, collections
p=r'C:\Users\DuWeisi\digimon-color-passport\dbgtest\g2s.tr'
c=collections.Counter(); n=0
for line in open(p,encoding='utf-8',errors='ignore'):
    m=re.match(r'^([0-9A-Fa-f]{6}):',line)
    if m:
        pc=int(m.group(1),16); n+=1
        c[pc>>12]+=1
print('instr',n,'unique 4k pages',len(c))
tot=0
for i,(pg,cnt) in enumerate(c.most_common(20)):
    tot+=cnt
    print(f'{pg:03X} {cnt:8d} {cnt/n*100:6.2f}% cum {tot/n*100:6.2f}%')
