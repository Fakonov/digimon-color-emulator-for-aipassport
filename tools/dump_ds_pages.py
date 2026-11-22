
import pypdf
p=r'C:\Users\DuWeisi\digimon-color-passport\ref\gpce4064a_ds.pdf'
r=pypdf.PdfReader(p)
for i in range(27,31):
    print('===== PAGE',i+1)
    print((r.pages[i].extract_text() or '')[:8000])
