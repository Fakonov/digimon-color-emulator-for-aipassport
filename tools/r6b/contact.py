import glob, os, sys
from PIL import Image, ImageDraw
src = sys.argv[1]; out = sys.argv[2]
files = sorted(glob.glob(os.path.join(src, "*.bmp")))
ims = [Image.open(f).convert("RGB") for f in files]
w, h = ims[0].size
scale = 3
cols = min(6, len(ims)); rows = (len(ims)+cols-1)//cols
sheet = Image.new("RGB", (cols*w*scale, rows*(h*scale+14)), "black")
d = ImageDraw.Draw(sheet)
for i, im in enumerate(ims):
    x = (i % cols) * w * scale; y = (i // cols) * (h*scale+14)
    sheet.paste(im.resize((w*scale, h*scale), Image.Resampling.NEAREST), (x, y+14))
    d.text((x+2, y+1), os.path.basename(files[i]), fill=(255,255,0))
sheet.save(out)
print("saved", out, sheet.size, len(ims))
