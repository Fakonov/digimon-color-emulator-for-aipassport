import struct
base=r'C:/Users/DuWeisi/digimon-color-passport/dbgtest/'
a=open(base+'entry_ram.bin','rb').read()
b=open(base+'exit_ram.bin','rb').read()
def w(buf,i): return struct.unpack('<H',buf[i*2:i*2+2])[0]
d=[]
for i in range(4096):
    x,y=w(a,i),w(b,i)
    if x!=y: d.append((i,x,y))
print('diff count',len(d))
for i,x,y in d: print(hex(i),hex(x),'->',hex(y))