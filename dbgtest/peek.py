import struct
p=r"C:\Users\DuWeisi\digimon-color-passport\dbgtest\entry_ram.bin"
b=open(p,'rb').read()
print(len(b))
print(b[:32].hex())
for a in [0x0600,0x0601,0x0602,0x0603,0x0604,0x0605,0x0606,0x0607,0x0ff2,0x0ff3,0x0ff4,0x0ff5,0x0ff6]:
    o=a*2
    print(hex(a), b[o:o+2].hex(), struct.unpack('<H',b[o:o+2])[0], struct.unpack('>H',b[o:o+2])[0])
