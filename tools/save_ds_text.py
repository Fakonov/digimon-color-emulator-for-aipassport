
import pypdf
p=r'C:\Users\DuWeisi\digimon-color-passport\ref\gpce4064a_ds.pdf'
r=pypdf.PdfReader(p)
out=[]
for i in range(26,32):
    t=r.pages[i].extract_text() or ''
    out.append('===== PAGE %d len %d =====\n'%(i+1,len(t))+t)
open(r'C:\Users\DuWeisi\digimon-color-passport\ref\ds_pages\clock_pages.txt','w',encoding='utf-8').write('\n'.join(out))
print('done')
