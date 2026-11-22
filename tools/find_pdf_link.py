
import re
p=r'C:\Users\DuWeisi\digimon-color-passport\ref\gpce4064a_ds.pdf'
s=open(p,'rb').read().decode('utf-8','replace')
for m in re.finditer(r'(?:href|src)="([^"]+)"',s):
    u=m.group(1)
    if 'pdf' in u.lower() or 'download' in u.lower() or 'file' in u.lower():
        print(u)
print('---')
print(s[:3000])
