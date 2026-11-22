import struct, zlib
from PIL import Image
p=r"C:\Users\DuWeisi\digimon-color-passport\firmware\color-preview\main\frames.bin"
d=open(p,'rb').read()
magic,ver,w,h,fps,count,off=struct.unpack_from('<4sHHHHII',d,0)
o=struct.unpack_from('<%dI'%(count+1),d,20)
def dec(i):
    raw=zlib.decompress(d[o[i]:o[i+1]])
    rgb=bytearray(w*h*3)
    for q in range(w*h):
        v=(raw[q*2]<<8)|raw[q*2+1]
        rgb[q*3]=((v>>11)&31)*255//31
        rgb[q*3+1]=((v>>5)&63)*255//63
        rgb[q*3+2]=(v&31)*255//31
    return Image.frombytes('RGB',(w,h),bytes(rgb))
for i in (20,30,40,60,90,120,149):
    im=dec(i)
    im.resize((384,384),Image.Resampling.NEAREST).save(rf"C:\Users\DuWeisi\digimon-color-passport\docs\f{i:03d}_raw.png")
    im.rotate(180).resize((384,384),Image.Resampling.NEAREST).save(rf"C:\Users\DuWeisi\digimon-color-passport\docs\f{i:03d}_r180.png")
print('ok')
