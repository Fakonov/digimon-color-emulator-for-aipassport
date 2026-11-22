
import re
for name in ['dmc_v1_base.bin','internal_rom_221017a.bin']:
    d=open(r'C:\Users\DuWeisi\digimon-color-passport\roms\'+name,'rb').read()
    ss=re.findall(rb'[ -~]{4,}',d)
    print('===',name,len(ss))
    for s in ss[:200]:
        print(s.decode('latin1'))
