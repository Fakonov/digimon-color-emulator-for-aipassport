import struct
root = r"C:\Users\DuWeisi\digimon-color-passport"
flash = open(root + r"\roms\dmc_v1_base.bin","rb").read()
def find(low):
    pat = bytes([0x60,0xF0, low & 0xFF, low >> 8])
    i=0; out=[]
    while True:
        i = flash.find(pat, i)
        if i < 0: break
        out.append(0x200000 + i//2); i += 2
    return out
for name, low in (("20068F",0x068F),("2002F4",0x02F4),("204179",0x4179),("2041C5",0x41C5),("200C1A",0x0C1A),("209349",0x9349),("203FAB",0x3FAB),("2002CC",0x02CC),("204123",0x4123)):
    print("%s <- %s" % (name, ", ".join("%06X"%p for p in find(low)) or "none"))
