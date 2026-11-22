
import re, struct, sys, collections
base=r'C:\Users\DuWeisi\digimon-color-passport'
flash=open(base+r'\roms\dmc_v1_base.bin','rb').read()
irom=open(base+r'\roms\internal_rom_221017a.bin','rb').read()

def fetch(pc):
    if 0x200000 <= pc < 0x400000:
        off=(pc-0x200000)*2
        if off+1 < len(flash): return struct.unpack_from('<H',flash,off)[0]
    elif 0x4000 <= pc < 0x10000:
        off=(pc-0x4000)*2
        if off+1 < len(irom): return struct.unpack_from('<H',irom,off)[0]
    return None

def official(op, pc, nxt, txt):
    op0=(op>>12)&15; opa=(op>>9)&7; op1=(op>>6)&7; opn=(op>>3)&7; opb=op&7
    lower=(op1<<4)|op0
    mn=txt.split()[0] if txt else ''
    # conditional short branch / jmp
    if op0<0xf and opa==7 and op1<2:
        imm=op&0x3f
        if op0==14: return 4,'jmp'
        target=(pc+1+(imm if op1==0 else -imm)) & 0xffffff
        return (4 if nxt==target else 1), ('jcc_t' if nxt==target else 'jcc_n')
    if lower==0x2d: return opn+2,'push'
    if lower==0x29:
        if op==0x9a98: return 6,'reti'
        return opn+3,'pop'
    if op0==0xf:
        if op1 in (0x00,0x04) and opn==1 and opa!=7: return 1,'mul'
        if op1==0x01: return (3 if (opa&1)==0 else 4),'call'
        if op1==0x02: return (3 if opa==7 else 2),'goto'
        if op1==0x05: return 1,'flags'
        if op1 in (0x06,0x07):
            N=opn if opn else 16
            return 2*N+2,'muls'
        return None,'fxxx_other'
    # ALU
    if op1==0x00: c=1; k='bp'
    elif op1==0x01: c=1; k='imm6'
    elif op1==0x03: c=(3 if (opn&3)==3 else 2); k='ind'
    elif op1==0x04:
        if opn==0: c=1; k='r'
        elif opn==1: c=2; k='imm16'
        elif opn in (2,3): c=2; k='mem16'
        else: c=1; k='shift'
    elif op1==0x05: c=1; k='shift2'
    elif op1==0x06: c=1; k='rot'
    elif op1==0x07: c=1; k='dir6'
    else: c=None; k='other'
    return c,k

def show(path, limit=0):
    rows=[]
    for line in open(path,encoding='utf-8',errors='ignore'):
        m=re.match(r'^([0-9A-Fa-f]{6}): (.*)$',line.rstrip('\n'))
        if m: rows.append((int(m.group(1),16),m.group(2).strip()))
    if limit: rows=rows[:limit]
    tot=0; known=0; unk=collections.Counter(); cats=collections.Counter(); catc=collections.Counter()
    for i,(pc,txt) in enumerate(rows):
        nxt=rows[i+1][0] if i+1<len(rows) else None
        op=fetch(pc)
        if op is None: unk['nofetch']+=1; continue
        c,k=official(op,pc,nxt,txt)
        cats[k]+=1
        if c is None: unk[k]+=1
        else: tot+=c; known+=1; catc[k]+=c
    print('FILE',path,'instr',len(rows),'known',known,'cycles',tot,'CPI',round(tot/known,3),'unknown',dict(unk))
    print('categories:')
    for k,n in cats.most_common(): print(f'  {k:10s} n={n:7d} cyc={catc[k]:8d} avg={catc[k]/n if n else 0:.2f} {n/len(rows)*100:6.2f}%')
    return tot,known

for p in sys.argv[1:]:
    show(p)
