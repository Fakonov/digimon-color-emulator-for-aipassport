
from PIL import Image
p=r"C:\Users\DuWeisi\digimon-color-passport\short_frames\f0300.png"
im=Image.open(p)
im.rotate(180).save(r"C:\Users\DuWeisi\digimon-color-passport\short_frames\f0300_rot180.png")
im.rotate(90, expand=True).save(r"C:\Users\DuWeisi\digimon-color-passport\short_frames\f0300_rot90.png")
im.rotate(270, expand=True).save(r"C:\Users\DuWeisi\digimon-color-passport\short_frames\f0300_rot270.png")
print("ok")
