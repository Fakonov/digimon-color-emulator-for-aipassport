import glob, struct, os, re
root = r"C:\Users\DuWeisi\digimon-color-passport"
pat = re.compile(r"t(\d+)\.bin$")
def load(f):
    d = open(f,"rb").read()
    return struct.unpack("<%dH"%(len(d)//2), d)
keys=[0x0600,0x0601,0x0602,0x0603,0x0C86,0x0C87,0x0C9A,0x0CB1,0x0C88,0x0C89]
print("MAME snapshots (dbgtest/cov/A/ram_t*.bin):")
for f in sorted(glob.glob(root+r"\dbgtest\cov\A\ram_t*.bin")):
    w = load(f)
    print("%-22s "%os.path.basename(f) + " ".join("%04X"%w[k] for k in keys))
print("key: " + " ".join("%04X"%k for k in keys))
