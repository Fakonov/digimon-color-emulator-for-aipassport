
import pypdf
p=r'C:\Users\DuWeisi\digimon-color-passport\ref\gpce4064a_ds.pdf'
r=pypdf.PdfReader(p)
for i in [27,28,29,30]:
    t=r.pages[i].extract_text() or ''
    print('===== PAGE',i+1,'len',len(t))
    print(t.encode('utf-8','replace').decode('utf-8')[:6000])
