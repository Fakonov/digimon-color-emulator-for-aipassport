import struct
base=r'C:/Users/DuWeisi/digimon-color-passport/dbgtest/'
mem=[0]*4096
b=open(base+'entry_ram.bin','rb').read()
for i in range(4096): mem[i]=struct.unpack('<H',b[i*2:i*2+2])[0]
r=[0]*16
r[0]=0x0FF6; r[1]=0x0000; r[2]=0x089D; r[3]=0x0C86; r[4]=0x0000; r[5]=0x0FF9; r[6]=0x02A0; r[7]=0x2002F4
sb=0
N=0x0200; Z=0x0100; S=0x0080; C=0x0040
def rd(a):
    a &= 0xffff
    return mem[a]
def wr(a,v):
    a &= 0xffff
    mem[a]=v & 0xffff
def nzsc(val, a, bb):
    global r
    r[6] &= ~(N|Z|S|C)
    if ((val>>16)&1) != ((a^bb)>>15)&1: r[6]|=S
    if (val>>15)&1: r[6]|=N
    if (val & 0xffff)==0: r[6]|=Z
    if (val>>16)&1: r[6]|=C
def nz(val):
    global r
    r[6] &= ~(N|Z)
    if val & 0x8000: r[6]|=N
    if (val & 0xffff)==0: r[6]|=Z
def ds(): return (r[6]>>10)&0x3f
def setds(v):
    r[6] = (r[6] & 0x03ff) | ((v & 0x3f)<<10)
def push(v, reg):
    wr(r[reg],v); r[reg]=(r[reg]-1)&0xffff
def pop(reg):
    r[reg]=(r[reg]+1)&0xffff
    return rd(r[reg])
def lreg(reg): return (r[reg] | (ds()<<16)) & 0xffffff

# 2002F4 push bp, bp to [sp]
push(r[5],0)
print('after push', [hex(x) for x in r[:8]], 'sb',hex(sb), 'memff6',hex(rd(0xff6)))
# 2002F5 sp -= 3
old=r[0]; val=old+((~3)&0xffff)+1; nzsc(val, old, (~3)&0xffff); r[0]=val&0xffff
print('sp-=3',hex(r[0]),hex(r[6]))
# 2002F6 bp=sp+1
old=r[0]; val=old+1; nzsc(val, old, 1); r[5]=val&0xffff
print('bp',hex(r[5]),hex(r[6]))
# 2002F8 ds=0
setds(0); print('ds0',hex(r[6]))
# 2002F9 r4=0x0602
r[4]=0x0602; nz(r[4])
# 2002FB r3=ds:[r4++]
addr=lreg(4); r[3]=rd(addr); r[4]=(r[4]+1)&0xffff; nz(r[3])
print('r3/r4',hex(r[3]),hex(r[4]),hex(r[6]))
# 2002FC r4=ds:[r4]
r[4]=rd(lreg(4)); nz(r[4])
print('r4',hex(r[4]),hex(r[6]))
# 2002FD r2=0x0b
r[2]=0x0b; nz(r[2])
# 2002FE r4 lsl r2
r[4]=(r[4] << (r[2]&0x1f)) & 0xffff
# 2002FF r3 lslor r2
res=(r[3] << (r[2]&0x1f)) & 0xffffffff
r[3]=res & 0xffff; r[4] |= (res>>16)&0xffff
print('shift',hex(r[3]),hex(r[4]))
# 200300 ds=0
setds(0)
# 200301 r2=0x0602
r[2]=0x0602; nz(r[2])
# 200303 r3 ^= ds:[r2++]
r[3] ^= rd(lreg(2)); r[2]=(r[2]+1)&0xffff; nz(r[3])
# 200304 r4 ^= ds:[r2]
r[4] ^= rd(lreg(2)); nz(r[4])
print('xor',hex(r[3]),hex(r[4]),hex(r[2]),hex(r[6]))
# 200305 [bp+0]=r3
wr((r[5]+0)&0xffff,r[3])
# 200306 [bp+1]=r4
wr((r[5]+1)&0xffff,r[4])
# 200307 ds=0
setds(0)
# 200308 r4=0x0604
r[4]=0x0604; nz(r[4])
# 20030A r2=ds:[r4++]
r[2]=rd(lreg(4)); r[4]=(r[4]+1)&0xffff; nz(r[2])
# 20030B r3=ds:[r4]
r[3]=rd(lreg(4)); nz(r[3])
# 20030C ds=0
setds(0)
# 20030D r4=0x0602
r[4]=0x0602; nz(r[4])
# 20030F ds:[r4++]=r2
wr(lreg(4),r[2]); r[4]=(r[4]+1)&0xffff
# 200310 ds:[r4]=r3
wr(lreg(4),r[3])
# 200311 ds=0
setds(0)
# 200312 r4=0x0606
r[4]=0x0606; nz(r[4])
# 200314 r2=ds:[r4++]
r[2]=rd(lreg(4)); r[4]=(r[4]+1)&0xffff; nz(r[2])
# 200315 r3=ds:[r4]
r[3]=rd(lreg(4)); nz(r[3])
# 200316 ds=0
setds(0)
# 200317 r4=0x0604
r[4]=0x0604; nz(r[4])
# 200319 ds:[r4++]=r2
wr(lreg(4),r[2]); r[4]=(r[4]+1)&0xffff
# 20031A ds:[r4]=r3
wr(lreg(4),r[3])
# 20031B ds=0
setds(0)
# 20031C r4=0x0600
r[4]=0x0600; nz(r[4])
# 20031E r2=ds:[r4++]
r[2]=rd(lreg(4)); r[4]=(r[4]+1)&0xffff; nz(r[2])
# 20031F r3=ds:[r4]
r[3]=rd(lreg(4)); nz(r[3])
# 200320 ds=0
setds(0)
# 200321 r4=0x0606
r[4]=0x0606; nz(r[4])
# 200323 ds:[r4++]=r2
wr(lreg(4),r[2]); r[4]=(r[4]+1)&0xffff
# 200324 ds:[r4]=r3
wr(lreg(4),r[3])
print('after block', [hex(mem[a]) for a in range(0x600,0x608)])
# 200325 r2=[bp+0]
r[2]=rd((r[5]+0)&0xffff); nz(r[2])
# 200326 r4=[bp+1]
r[4]=rd((r[5]+1)&0xffff); nz(r[4])
# 200327 r3=r4 lsr 4
shift=((r[4]<<4)|sb)>>(7-3); sb=shift&0xf; r[3]=(shift>>4)&0xffff; nz(r[3])
# 200328 r4=r2 ror 4
shift=(((sb<<16)|r[2])<<4)|sb
shift >>= (7-3); sb=shift&0xf; r[4]=(shift>>4)&0xffff; nz(r[4])
# 200329 r3=r3 lsr 4
shift=((r[3]<<4)|sb)>>(7-3); sb=shift&0xf; r[3]=(shift>>4)&0xffff; nz(r[3])
# 20032A r4=r4 ror 4
shift=(((sb<<16)|r[4])<<4)|sb
shift >>= (7-3); sb=shift&0xf; r[4]=(shift>>4)&0xffff; nz(r[4])
print('after shifts',hex(r[2]),hex(r[3]),hex(r[4]),'sb',hex(sb),hex(r[6]))
# 20032B r4 ^= [bp+0]
r[4] ^= rd((r[5]+0)&0xffff); nz(r[4])
# 20032C [bp+2]=r4
wr((r[5]+2)&0xffff,r[4])
# 20032D r3 ^= [bp+1]
r[3] ^= rd((r[5]+1)&0xffff); nz(r[3])
print('after xor2',hex(r[3]),hex(r[4]),hex(rd(0xff5)))
# 20032E ds=0
setds(0)
# 20032F r4=0x0600
r[4]=0x0600; nz(r[4])
# 200331 test r4, ds:[r4++]
r2v=rd(lreg(4)); r[4]=(r[4]+1)&0xffff; nz(r[4] & r2v)
# 200332 r4=ds:[r4]
r[4]=rd(lreg(4)); nz(r[4])
# 200333 r4=r4 lsr 3
shift=((r[4]<<4)|sb)>>(6-3); sb=shift&0xf; r[4]=(shift>>4)&0xffff; nz(r[4])
# 200334 r2=0
r[2]=0; nz(r[2])
# 200335 ds=0
setds(0)
# 200336 r1=0x0600
r[1]=0x0600; nz(r[1])
# 200338 r4 ^= ds:[r1++]
r[4] ^= rd(lreg(1)); r[1]=(r[1]+1)&0xffff; nz(r[4])
# 200339 r2 ^= ds:[r1]
r[2] ^= rd(lreg(1)); nz(r[2])
# 20033A r1=[bp+2]
r[1]=rd((r[5]+2)&0xffff); nz(r[1])
# 20033B r1 ^= r4
r[1] ^= r[4]; nz(r[1])
# 20033C r3 ^= r2
r[3] ^= r[2]; nz(r[3])
# 20033D ds=0
setds(0)
# 20033E r4=0x0600
r[4]=0x0600; nz(r[4])
# 200340 ds:[r4++]=r1
wr(lreg(4),r[1]); r[4]=(r[4]+1)&0xffff
# 200341 ds:[r4]=r3
wr(lreg(4),r[3])
# 200342 sp += 3
old=r[0]; val=old+3; nzsc(val,old,3); r[0]=val&0xffff
print('FINAL regs', [hex(x) for x in r[:8]], 'sb',hex(sb))
print('FINAL mem', [hex(mem[a]) for a in range(0x600,0x608)])
print('stack', [hex(mem[a]) for a in range(0xff2,0xff7)])
