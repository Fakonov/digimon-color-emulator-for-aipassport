
import re, html
p=r'C:\Users\DuWeisi\digimon-color-passport\ref\gpce4064b2.html'
raw=open(p,'rb').read()
for enc in ['utf-8','big5','cp950','latin-1']:
    try:
        s=raw.decode(enc)
        print('ENC',enc,'OK',len(s))
        break
    except Exception as e:
        print('ENC',enc,'fail',e)
lines=s.splitlines()
for i in range(1770,1835):
    t=re.sub(r'<[^>]+>',' ',lines[i])
    t=html.unescape(t)
    t=re.sub(r'\s+',' ',t).strip()
    if t: print(i+1, t[:500])
