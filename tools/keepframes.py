
from PIL import Image
p=r'C:\Users\DuWeisi\digimon-color-passport\gateA_keepalive.avi'
size=96*96*3; d=open(p,'rb').read(); n=len(d)//size
print('frames',n)
for i in [600,1200,1800,2400,n-1]:
    if i>=n: continue
    im=Image.frombytes('RGB',(96,96),d[i*size:(i+1)*size])
    im.resize((288,288),Image.NEAREST).save(rf'C:\Users\DuWeisi\digimon-color-passport\gateA_keepalive_f{i:04d}.png')
    print(i, round(sum(d[i*size:(i+1)*size])/(size*3),2))
