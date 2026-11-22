import struct
root=r"C:\Users\DuWeisi\digimon-color-passport\dbgtest\re\\"
def ld(n):
    d=open(root+n,"rb").read(); return struct.unpack("<%dH"%(len(d)//2),d)
def st(w): return " ".join("%04X"%w[0x600+i] for i in range(8))
a=ld("prng_pre.bin"); b=ld("prng_post.bin")
print("pre :", st(a))
print("post:", st(b))
W0=(a[0x600]|a[0x601]<<16); W1=(a[0x602]|a[0x603]<<16); W2=(a[0x604]|a[0x605]<<16); W3=(a[0x606]|a[0x607]<<16)
M=0xFFFFFFFF
t=((W1<<11)&M)^W1
lo=(((t&M)>>8)^(t&M))&0xFFFF
hi=((((t>>16)>>8)^(t>>16)))&0xFFFF
nW0=(((hi^((W0>>16)&0xFFFF))<<16)|((lo^((((W0>>16)&0xFFFF)>>3)^(W0&0xFFFF)))&0xFFFF))&M
print("pred:", " ".join("%04X"%x for x in (nW0&M, (nW0>>16)&0xFFFF, W1&M, (W1>>16)&0xFFFF, W2&M, (W2>>16)&0xFFFF, W3&M, (W3>>16)&0xFFFF)))
