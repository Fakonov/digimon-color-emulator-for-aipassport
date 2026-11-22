
import pypdf, re
p=r'C:\Users\DuWeisi\digimon-color-passport\ref\gpce4064a_ds.pdf'
r=pypdf.PdfReader(p)
print('pages',len(r.pages))
alltxt=[]
for i,pg in enumerate(r.pages):
    t=pg.extract_text() or ''
    alltxt.append(t)
full='\n'.join(alltxt)
print('chars',len(full))
for pat in ['48MHz','48 MHz','96MHz','96 MHz','CPU Clock','System Clock','PLL','m.nSP','unSP','Max. CPU']:
    print('===',pat)
    for m in re.finditer(re.escape(pat),full,re.I):
        s=max(0,m.start()-200); e=min(len(full),m.end()+300)
        print(full[s:e].replace('\n',' | ')[:600])
