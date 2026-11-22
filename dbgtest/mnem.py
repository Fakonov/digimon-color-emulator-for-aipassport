
import re, collections
p=r'C:\Users\DuWeisi\digimon-color-passport\dbgtest\g2s.tr'
c=collections.Counter(); forms=collections.Counter()
n=0
for line in open(p,encoding='utf-8',errors='ignore'):
    m=re.match(r'^([0-9A-Fa-f]{6}): (.*)$',line.rstrip())
    if not m: continue
    t=m.group(2).strip(); n+=1
    mn=t.split()[0]
    c[mn]+=1
    # operand shape
    if '[' in t: forms['mem']+=1
    if 'ds:[' in t: forms['dsmem']+=1
print('n',n)
for k,v in c.most_common(60): print(f'{k:12s} {v:7d} {v/n*100:6.2f}%')
print(forms)
