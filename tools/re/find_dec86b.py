import struct
root = r"C:\Users\DuWeisi\digimon-color-passport"
flash = open(root + r"\roms\dmc_v1_base.bin","rb").read()
def w(off): return struct.unpack_from("<H", flash, off)[0]
for load in (0x96E4, 0x98E4):
    pat = bytes([0x0C,0x99,0x86,0x0C, load&0xFF, load>>8])
    i=0; n=0
    while True:
        i = flash.find(pat, i)
        if i < 0: break
        pc = 0x200000 + i//2
        nxt = [w(i+6+2*k) for k in range(5)]
        print("%06X load=%04X next: %s" % (pc, load, " ".join("%04X"%x for x in nxt)))
        i += 2; n+=1
    print("-- %04X total %d" % (load, n))
