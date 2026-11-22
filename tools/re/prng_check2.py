import struct
root=r"C:\Users\DuWeisi\digimon-color-passport\dbgtest\re\\"
def ld(n):
    d=open(root+n,"rb").read(); return struct.unpack("<%dH"%(len(d)//2),d)
def st(w): return " ".join("%04X"%w[0x600+i] for i in range(8))
print("post2:", st(ld("prng_post2.bin")))
