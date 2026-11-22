import struct, zlib
from PIL import Image

BLOB = r"C:/Users/DuWeisi/digimon-color-passport/firmware/color-preview/main/frames.bin"
OUT = r"C:/Users/DuWeisi/digimon-color-passport/docs/preview_rotations.png"

b = open(BLOB, "rb").read()
magic, ver, w, h, fps, cnt, doff = struct.unpack_from("<4sHHHHII", b, 0)
offs = struct.unpack_from("<%dI" % (cnt + 1), b, 16)
raw = zlib.decompress(b[offs[60]:offs[61]])
assert len(raw) == 96 * 96 * 2, len(raw)

def rgb565(hi, lo):
    v = (hi << 8) | lo
    r = ((v >> 11) & 31) * 255 // 31
    g = ((v >> 5) & 63) * 255 // 63
    bl = (v & 31) * 255 // 31
    return (r, g, bl)

src = Image.new("RGB", (96, 96))
px = src.load()
for y in range(96):
    for x in range(96):
        i = (y * 96 + x) * 2
        px[x, y] = rgb565(raw[i], raw[i + 1])

out = Image.new("RGB", (1280, 300), "black")
for k in range(4):
    im = src.rotate(-90 * k, expand=False).resize((192, 192), Image.NEAREST)
    canvas = Image.new("RGB", (320, 240), "black")
    canvas.paste(im, (64, 24))
    canvas.paste(Image.new("RGB", (320, 8), "red"), (0, 0))
    canvas.paste(Image.new("RGB", (320, 8), "green"), (0, 232))
    out.paste(canvas, (k * 320, 30))
out.save(OUT)
print(magic, ver, w, h, fps, cnt, len(raw), OUT)
