from pypdf import PdfReader
r=PdfReader(r'C:\Users\DuWeisi\digimon-color-passport\ref\unsp2.0.pdf')
for pno in [143,144,145,146,147,153,154,155,156,157]:
    p=r.pages[pno-1]
    print('page',pno,'images',len(p.images))
    for i,img in enumerate(p.images):
        print(' ',i,img.name,len(img.data))
        open(fr'C:\Users\DuWeisi\digimon-color-passport\ref\p{pno}_{i}.png','wb').write(img.data)
