import sys
sys.path.insert(0, r"C:\Users\DuWeisi\digimon-color-passport\tools\coverage")
import unsp_static as U, inspect
print(inspect.signature(U.Images.__init__))
print(inspect.signature(U.decode))
d = U.decode(U.Images(r"C:\Users\DuWeisi\digimon-color-passport\roms\dmc_v1_base.bin", r"C:\Users\DuWeisi\digimon-color-passport\roms\internal_rom_221017a.bin"), 0x2002F4)
print(type(d), d)
