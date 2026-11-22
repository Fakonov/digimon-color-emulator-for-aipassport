M = 0xFFFFFFFF
def step(s, sh):
    W0,W1,W2,W3 = s
    t = ((W1 << 11) & M) ^ W1
    lo, hi = t & 0xFFFF, (t >> 16) & 0xFFFF
    a = (((lo >> sh(8)) | (lo << (16-sh(8)))) & 0xFFFF) ^ lo
    b = ((hi >> sh(8)) & 0xFFFF) ^ hi
    c = (((W0 >> 16) >> sh(3)) ^ (W0 & 0xFFFF)) & 0xFFFF
    nlo = (a ^ c) & 0xFFFF
    nhi = (b ^ ((W0 >> 16) & 0xFFFF)) & 0xFFFF
    return (nlo | (nhi << 16), W2, W3, W0)
seed = (0x1333 | (0x0549<<16), 0xCD15 | (0x075B<<16), 0x55E5 | (0x159A<<16), 0x3BB5 | (0x1F12<<16))
want = (0x6CE35552, 0xD3D4F35E, 0x637AA046)
for name, sh in (("imm", lambda n: n), ("imm+1", lambda n: n+1)):
    s = seed; hit=None
    for k in range(1, 3000000):
        s = step(s, sh)
        if (s[0],s[2],s[3]) == want: hit=k; break
    print(name, "triple hit at", hit, "first:", " ".join("%08X"%x for x in step(seed,sh)))
