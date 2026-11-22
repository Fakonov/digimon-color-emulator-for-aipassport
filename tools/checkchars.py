
p=r'C:\Users\DuWeisi\digimon-color-passport\docs\FEASIBILITY.md'
s=open(p,encoding='utf-8').read()
from collections import Counter
c=Counter(ch for ch in s if ch in '\u2028\u2029\u0085\r\n\t')
print({hex(ord(k)):v for k,v in c.items()})
print('len',len(s))
