from pypdf import PdfReader
r=PdfReader(r'C:\Users\DuWeisi\digimon-color-passport\ref\unsp2.0.pdf')
print('pages',len(r.pages))
out=[]
for i,p in enumerate(r.pages):
    t=p.extract_text() or ''
    out.append(f'\n===== PAGE {i+1} =====\n'+t)
open(r'C:\Users\DuWeisi\digimon-color-passport\ref\unsp2.0.txt','w',encoding='utf-8').write(''.join(out))
print('chars',sum(len(x) for x in out))
