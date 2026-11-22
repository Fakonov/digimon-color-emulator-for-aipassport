from PIL import Image
import glob,os
base=r"C:\Users\DuWeisi\digimon-color-passport\dbgtest\re\shots"
for f in sorted(glob.glob(base+r"\*.bmp")):
    im=Image.open(f); out=f[:-4]+".png"; im.save(out); print(out, im.size)
