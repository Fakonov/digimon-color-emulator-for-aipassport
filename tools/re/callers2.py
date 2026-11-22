import struct
root = r"C:\Users\DuWeisi\digimon-color-passport"
flash = open(root + r"\roms\dmc_v1_base.bin","rb").read()
def find(low, hi=0xF0, lo_byte=0x60):
    pat = bytes([lo_byte,hi, low & 0xFF, low >> 8]); i=0; out=[]
    while True:
        i = flash.find(pat, i)
        if i < 0: break
        out.append(0x200000 + i//2); i += 2
    return out
print("call 203DCE <-", ", ".join("%06X"%p for p in find(0x3DCE)) or "none")
print("call 203E9E <-", ", ".join("%06X"%p for p in find(0x3E9E)) or "none")
print("call 203D9E <-", ", ".join("%06X"%p for p in find(0x3D9E)) or "none")
print("call 2005F3 <-", ", ".join("%06X"%p for p in find(0x05F3)) or "none")
print("call 20091E <-", ", ".join("%06X"%p for p in find(0x091E)) or "none")
