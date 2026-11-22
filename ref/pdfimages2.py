from pypdf import PdfReader
r=PdfReader(r'C:\Users\DuWeisi\digimon-color-passport\ref\unsp2.0.pdf')
for pno in [148,149,150,151,152,158,159,160,161]:
    p=r.pages[pno-1]
    print('page',pno,'images',len(p.images))
    for i,img in enumerate(p.images):
        print(' ',i,img.name,len(img.data))
        open(fr'C:\Users\DuWeisi\digimon-color-passport\ref\p{pno}_{i}.png','wb').write(img.data)
