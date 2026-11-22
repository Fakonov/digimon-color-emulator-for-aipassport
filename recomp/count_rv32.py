import re
p=r'C:/Users/DuWeisi/digimon-color-passport/recomp/hot_func_rv32.s'
lines=open(p,encoding='utf-8').read().splitlines()
start=None; end=None
for i,l in enumerate(lines):
    if l.strip()=='hot_func:': start=i
    if l.strip().startswith('.size') and 'hot_func' in l: end=i
ins=[]
for l in lines[start+1:end]:
    s=l.split('#')[0].strip()
    if not s or s.startswith('.') or s.endswith(':'): continue
    ins.append(s)
print('start',start+2,'end',end,'instructions',len(ins))
from collections import Counter
ops=Counter(s.split()[0] for s in ins)
for k,v in ops.most_common(): print(k,v)
print('branches', sum(v for k,v in ops.items() if k.startswith('b') or k.startswith('j')))
print('loads', sum(v for k,v in ops.items() if k.startswith('l')))
print('stores', sum(v for k,v in ops.items() if k.startswith('s')))
