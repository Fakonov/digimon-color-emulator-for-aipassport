
import re, collections
p=r'C:\Users\DuWeisi\digimon-color-passport\dbgtest\g2s.tr'
c=collections.Counter(); n=0; pages=collections.Counter()
for line in open(p,encoding='utf-8',errors='ignore'):
    m=re.match(r'^([0-9A-Fa-f]{6}): ',line)
    if not m: continue
    pc=int(m.group(1),16); n+=1
    if 0x200000<=pc<0x400000: c['external_flash']+=1
    elif 0x4000<=pc<0x10000: c['internal_rom']+=1
    elif pc<0x1000: c['ram']+=1
    elif 0x3000<=pc<0x3100: c['io']+=1
    else: c['other']+=1
    pages[pc>>12]+=1
print('total',n)
for k,v in c.most_common(): print(k,v,round(v/n*100,4))
print('top pages')
for k,v in pages.most_common(10): print(hex(k<<12),v,round(v/n*100,4))
