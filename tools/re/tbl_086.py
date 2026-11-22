import glob, struct, os
root = r"C:\Users\DuWeisi\digimon-color-passport"
files = sorted(glob.glob(root + r"\dbgtest\re\emu_auto_t*.bin"))
keys = [0x0C86,0x0C87,0x0C88,0x0C89,0x0C8A,0x0C8B,0x0850,0x0CB1,0x0CAD,0x0C97,0x0CAA,0x0CAB]
print("file           " + " ".join("%04X"%k for k in keys))
for f in files:
    d = open(f,"rb").read()
    w = struct.unpack("<%dH"%(len(d)//2), d)
    print("%-14s " % os.path.basename(f) + " ".join("%04X"%w[k] for k in keys))
