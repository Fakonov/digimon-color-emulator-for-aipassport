import struct
for name in ['entry_ram.bin','exit_ram.bin']:
 p=r"C:\Users\DuWeisi\digimon-color-passport\dbgtest\"+name
 b=open(p,'rb').read()
 print(name)
 for a in range(0x0600,0x0608):
  o=a*2
  print(hex(a), hex(struct.unpack('<H',b[o:o+2])[0]))
 for a in [0x0ff2,0x0ff3,0x0ff4,0x0ff5,0x0ff6]:
  o=a*2
  print(hex(a), hex(struct.unpack('<H',b[o:o+2])[0]))
