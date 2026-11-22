
import pypdf, re
p=r'C:\Users\DuWeisi\digimon-color-passport\ref\gpce4064a_ds.pdf'
r=pypdf.PdfReader(p)
for i,pg in enumerate(r.pages):
    t=pg.extract_text() or ''
    if re.search(r'48\s*MHz|CPU Clock|System Clock|PLL|clock',t,re.I):
        print('===== PAGE',i+1)
        for line in t.splitlines():
            if re.search(r'48\s*MHz|CPU Clock|System Clock|PLL|clock|MHz',line,re.I):
                print(line.strip())
