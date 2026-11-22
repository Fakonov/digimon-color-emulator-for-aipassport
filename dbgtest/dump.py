import struct
d=open(r'C:\Users\DuWeisi\digimon-color-passport\roms\dmc_v1_base.bin','rb').read()
off=0x2F8*2
print('off',hex(off),'len',len(d))
print('LE',' '.join(f'{struct.unpack_from("<H",d,off+i*2)[0]:04X}' for i in range(20)))
print('BE',' '.join(f'{struct.unpack_from(">H",d,off+i*2)[0]:04X}' for i in range(20)))
ir=open(r'C:\Users\DuWeisi\digimon-color-passport\roms\internal_rom_221017a.bin','rb').read()
o2=(0x485D-0x4000)*2
print('IR off',hex(o2))
print('IR LE',' '.join(f'{struct.unpack_from("<H",ir,o2+i*2)[0]:04X}' for i in range(10)))
print('IR BE',' '.join(f'{struct.unpack_from(">H",ir,o2+i*2)[0]:04X}' for i in range(10)))
