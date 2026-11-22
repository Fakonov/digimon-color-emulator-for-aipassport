from PIL import Image, ImageDraw

W, H = 320, 240
SCALE = 2
SRC = 96
LEFT = (W - SRC * SCALE) // 2
TOP = (H - SRC * SCALE) // 2

def src_pattern():
    im = Image.new("RGB", (SRC, SRC), (0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rectangle([0, 0, 15, 15], fill=(255, 0, 0))
    d.rectangle([SRC - 16, 0, SRC - 1, 15], fill=(0, 255, 0))
    d.rectangle([0, SRC - 16, 15, SRC - 1], fill=(0, 0, 255))
    d.rectangle([SRC - 16, SRC - 16, SRC - 1, SRC - 1], fill=(255, 255, 255))
    d.rectangle([44, 38, 51, 75], fill=(255, 255, 255))
    for y in range(18, 38):
        half = y - 18
        d.rectangle([48 - half, y, 48 + half, y], fill=(255, 255, 255))
    return im

def compose(rot):
    src = src_pattern().rotate(-90 * rot, expand=False)
    out = Image.new("RGB", (W, H), (0, 0, 0))
    big = src.resize((SRC * SCALE, SRC * SCALE), Image.Resampling.NEAREST)
    out.paste(big, (LEFT, TOP))
    d = ImageDraw.Draw(out)
    d.rectangle([0, 0, W - 1, 7], fill=(255, 0, 0))
    d.rectangle([0, H - 8, W - 1, H - 1], fill=(0, 255, 0))
    return out

compose(2).save("docs/static_expected_180.png")
compose(0).save("docs/static_expected_0.png")
sheet = Image.new("RGB", (W * 2, H * 2), (32, 32, 32))
for i, rot in enumerate([0, 1, 2, 3]):
    sheet.paste(compose(rot), ((i % 2) * W, (i // 2) * H))
sheet.save("docs/static_expected_rotations.png")
print("wrote docs/static_expected_180.png, docs/static_expected_0.png, docs/static_expected_rotations.png")
