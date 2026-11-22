
from PIL import Image
import os, sys
p = sys.argv[1]
out = sys.argv[2]
size=96*96*3
data=open(p,'rb').read()
os.makedirs(out, exist_ok=True)
frames=[0,25,30,60,100,150,200,300,600,900,1000,1022,1080,1100,1150,1199]
th=144
sheet=Image.new('RGB',(th*4, th*4),(0,0,0))
for idx,i in enumerate(frames):
    off=i*size
    if off+size>len(data): continue
    im=Image.frombytes('RGB',(96,96),data[off:off+size])
    im.resize((288,288), Image.NEAREST).save(os.path.join(out, f"f{i:04d}.png"))
    sheet.paste(im.resize((th,th),Image.NEAREST),((idx%4)*th,(idx//4)*th))
    print(i, "mean", round(sum(data[off:off+size])/size,2))
sheet.save(os.path.join(out,"contact.png"))
print("saved", out)
