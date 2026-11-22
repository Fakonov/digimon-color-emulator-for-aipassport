from pathlib import Path
for name in ['delay_entry_ram.bin','delay_exit_ram.bin']:
    b=Path(r'C:/Users/DuWeisi/digimon-color-passport/dbgtest/'+name).read_bytes()
    w=[int.from_bytes(b[i:i+2],'little') for i in range(0,len(b),2)]
    print(name, 'bp[0ff5]=%04X'%w[0x0ff5], 'ram[0ff2..0ff6]=', ' '.join('%04X'%x for x in w[0x0ff2:0x0ff7]))
    print('  nonFF count', sum(1 for x in w if x!=0xffff), 'nonzero', sum(1 for x in w if x!=0))
