
from PIL import Image
import os
p = r"C:\Users\DuWeisi\digimon-color-passport\button.avi"
size=96*96*3
data=open(p,'rb').read()
out=r"C:\Users\DuWeisi\digimon-color-passport\button_frames"
os.makedirs(out, exist_ok=True)
for i in [0,25,30,60,100,200,300,600,900,1000,1022,1080,1200,1400,1500]:
    off=i*size
    if off+size>len(data): continue
    im=Image.frombytes('RGB',(96,96),data[off:off+size])
    im.resize((288,288), Image.NEAREST).save(os.path.join(out, f"f{i:04d}.png"))
    print(i, "mean", sum(data[off:off+size])/size)
# contact sheet of 4x4 thumbs 144x144
frames=[0,25,30,60,100,200,300,600,900,1000,1022,1080,1200,1400,1500]
th=144
sheet=Image.new('RGB',(th*4, th*4),(0,0,0))
for idx,i in enumerate(frames):
    off=i*size
    if off+size>len(data): continue
    im=Image.frombytes('RGB',(96,96),data[off:off+size]).resize((th,th),Image.NEAREST)
    sheet.paste(im,((idx%4)*th,(idx//4)*th))
sheet.save(os.path.join(out,"contact.png"))
print("saved")
