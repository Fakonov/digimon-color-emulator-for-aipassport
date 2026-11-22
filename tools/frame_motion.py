import struct, zlib
from PIL import Image, ImageChops
p=r"C:\Users\DuWeisi\digimon-color-passport\firmware\color-preview\main\frames.bin"
d=open(p,'rb').read()
magic,ver,w,h,fps,count,off=struct.unpack_from('<4sHHHHII',d,0)
o=struct.unpack_from('<%dI'%(count+1),d,20)
ims=[]
for i in range(count):
    raw=zlib.decompress(d[o[i]:o[i+1]])
    rgb=bytearray(w*h*3)
    for q in range(w*h):
        v=(raw[q*2]<<8)|raw[q*2+1]
        rgb[q*3]=((v>>11)&31)*255//31
        rgb[q*3+1]=((v>>5)&63)*255//63
        rgb[q*3+2]=(v&31)*255//31
    ims.append(Image.frombytes('RGB',(w,h),bytes(rgb)))
for i in range(1,count):
    diff=ImageChops.difference(ims[i-1],ims[i])
    stat=sum(diff.convert('L').getdata())
    mean=stat/(w*h)
    print(i, round(mean,2))
