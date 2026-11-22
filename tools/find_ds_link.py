
import re
p=r'C:\Users\DuWeisi\digimon-color-passport\ref\gpce4064b2.html'
s=open(p,'rb').read().decode('latin-1')
for m in re.finditer(r'.{0,300}GPCE4XXXXV13_ds\.pdf.{0,300}', s):
    print(m.group(0))
