
import pypdf, os
p=r'C:\Users\DuWeisi\digimon-color-passport\ref\gpce4064a_ds.pdf'
out=r'C:\Users\DuWeisi\digimon-color-passport\ref\ds_pages'
os.makedirs(out,exist_ok=True)
r=pypdf.PdfReader(p)
for i in [26,27,28,29,30,31,32,33]:
    pg=r.pages[i]
    print('PAGE',i+1,'images',len(pg.images),'textlen',len(pg.extract_text() or ''))
    for j,im in enumerate(pg.images):
        fn=os.path.join(out,f'p{i+1:02d}_{j}.png')
        try:
            im.image.save(fn)
            print(' saved',fn,im.image.size)
        except Exception as e:
            print(' err',e)
