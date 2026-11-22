from PIL import Image
import glob,struct
base=r"C:\Users\DuWeisi\digimon-color-passport\dbgtest\re\shots_rtc"
for f in sorted(glob.glob(base+r"\*.bmp")):
    Image.open(f).save(f[:-4]+".png")
d=open(r"C:\Users\DuWeisi\digimon-color-passport\dbgtest\re\rtc_on_t6.bin","rb").read(); w=struct.unpack("<%dH"%(len(d)//2),d)
for k in (0x0CAF,0x0CAE,0x0CAD,0x0CB0,0x0CB1,0x0CB2,0x085C,0x085D):
    print("%04X=%04X"%(k,w[k]),end="  ")
print()
