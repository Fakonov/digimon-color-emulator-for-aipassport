import struct, zlib, os
from PIL import Image, ImageDraw

path = r"C:\Users\DuWeisi\digimon-color-passport\firmware\color-preview\main\frames.bin"
outdir = r"C:\Users\DuWeisi\digimon-color-passport\docs"
data = open(path, "rb").read()
magic, ver, w, h, fps, count, off = struct.unpack_from("<4sHHHHII", data, 0)
offs = struct.unpack_from("<%dI" % (count + 1), data, 20)
ims = []
for i in range(count):
    raw = zlib.decompress(data[offs[i]:offs[i+1]])
    assert len(raw) == w*h*2, (i, len(raw))
    rgb = bytearray(w*h*3)
    for p in range(w*h):
        v = (raw[p*2] << 8) | raw[p*2+1]
        rgb[p*3] = ((v >> 11) & 31) * 255 // 31
        rgb[p*3+1] = ((v >> 5) & 63) * 255 // 63
        rgb[p*3+2] = (v & 31) * 255 // 31
    ims.append(Image.frombytes("RGB", (w, h), bytes(rgb)))
print("decoded", len(ims))
cols, rows = 10, 15
cell = 96*2
sheet = Image.new("RGB", (cols*cell, rows*cell), "black")
d = ImageDraw.Draw(sheet)
for i, im in enumerate(ims):
    x = (i % cols) * cell
    y = (i // cols) * cell
    sheet.paste(im.resize((cell, cell), Image.Resampling.NEAREST), (x, y))
    d.text((x+4, y+4), str(i), fill=(255,255,0))
sheet.save(os.path.join(outdir, "frames_contact.png"))
strip = Image.new("RGB", (15*96*3, 96*3), "black")
for j in range(15):
    strip.paste(ims[20+j].resize((96*3,96*3), Image.Resampling.NEAREST), (j*96*3,0))
strip.save(os.path.join(outdir, "frames_20_34.png"))
print("saved")
