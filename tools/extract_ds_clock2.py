
import pypdf, re
p=r'C:\Users\DuWeisi\digimon-color-passport\ref\gpce4064a_ds.pdf'
r=pypdf.PdfReader(p)
full='\n'.join((pg.extract_text() or '') for pg in r.pages)
out=[]
for pat in ['System_Clock','System Clock','FOSC','PLL','6MHz','12MHz','24MHz','48MHz']:
    out.append('===== '+pat)
    for m in re.finditer(re.escape(pat),full,re.I):
        s=max(0,m.start()-300); e=min(len(full),m.end()+500)
        out.append(full[s:e].replace('\n',' | ')[:900])
open(r'C:\Users\DuWeisi\digimon-color-passport\ref\ds_pages\clock_refs.txt','w',encoding='utf-8').write('\n'.join(out))
print('ok',len(out))
