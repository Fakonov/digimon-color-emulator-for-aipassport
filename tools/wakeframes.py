
from PIL import Image
p=r'C:\Users\DuWeisi\digimon-color-passport\gateA_wake.avi'
size=96*96*3; d=open(p,'rb').read(); n=len(d)//size
print('frames',n)
out=r'C:\Users\DuWeisi\digimon-color-passport\gateA_wake_frames'; import os; os.makedirs(out,exist_ok=True)
for i in [0,600,1000,1100,1200,1300,1500,1800,2000,2400]:
    if i>=n: continue
    im=Image.frombytes('RGB',(96,96),d[i*size:(i+1)*size])
    im.resize((288,288),Image.NEAREST).save(os.path.join(out,f'f{i:04d}.png'))
    print(i, round(sum(d[i*size:(i+1)*size])/(size*3),2))
