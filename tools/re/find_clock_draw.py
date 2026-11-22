import os,struct
root=r"C:\Users\DuWeisi\digimon-color-passport"
d=open(os.path.join(root,"roms","dmc_v1_base.bin"),"rb").read()
pat=bytes([0x60,0xF0,0x73,0x3F])
hits=[i for i in range(0,len(d)-4) if d[i:i+4]==pat]
print("call 203f73 hits:", ["%06X"%(0x200000+(h//2)) for h in hits])
pat2=bytes([0x60,0xF0,0xAB,0x3F])
print("call 203fab hits:", ["%06X"%(0x200000+(i//2)) for i in range(0,len(d)-4) if d[i:i+4]==pat2])
pat3=bytes([0x60,0xF0,0xE4,0x3F])
print("call 203fe4 hits:", ["%06X"%(0x200000+(i//2)) for i in range(0,len(d)-4) if d[i:i+4]==pat3])
