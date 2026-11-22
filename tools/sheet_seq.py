
from PIL import Image
import os
p=r'C:\Users\DuWeisi\digimon-color-passport\gateA_seq.avi'
size=96*96*3
data=open(p,'rb').read(); n=len(data)//size
print('frames',n)
out=r'C:\Users\DuWeisi\digimon-color-passport\gateA_seq_frames'; os.makedirs(out,exist_ok=True)
frames=list(range(0,min(n,1500),30))
th=144; cols=8; rows=(len(frames)+cols-1)//cols
sheet=Image.new('RGB',(th*cols,th*rows),(0,0,0))
for idx,i in enumerate(frames):
    off=i*size
    if off+size>len(data): continue
    im=Image.frombytes('RGB',(96,96),data[off:off+size]).resize((th,th),Image.NEAREST)
    sheet.paste(im,((idx%cols)*th,(idx//cols)*th))
    if i%120==0: im.resize((288,288),Image.NEAREST).save(os.path.join(out,f'f{i:04d}.png'))
sheet.save(os.path.join(out,'contact.png'))
print('saved', len(frames))
