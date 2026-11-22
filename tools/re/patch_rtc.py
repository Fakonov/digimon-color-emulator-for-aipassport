import struct,sys
p=r"C:\Users\DuWeisi\digimon-color-passport\dbgtest\re\flash_rtc_on.bin"
d=bytearray(open(p,"rb").read())
WORD=0x2041D7; off=(WORD-0x200000)*2
old=struct.unpack_from("<H",d,off)[0]
struct.pack_into("<H",d,off,0x9641)
open(p,"wb").write(d)
print("patch @0x%X file 0x%X: %04X -> 9641"%(WORD,off,old))
