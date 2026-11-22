
import re, html
p=r'C:\Users\DuWeisi\digimon-color-passport\ref\gpce4064b2.html'
s=open(p,'rb').read().decode('latin-1')
# decode Big5 chunks? just print raw ASCII-ish text
lines=s.splitlines()
for i in range(1785,1835):
    t=re.sub(r'<[^>]+>',' ',lines[i])
    t=html.unescape(t)
    t=re.sub(r'\s+',' ',t).strip()
    if t: print(i+1, t[:800])
