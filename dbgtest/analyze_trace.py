import re, struct, sys
from collections import Counter
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

def guest_cycles(op, pc, nxt):
    op0=(op>>12)&15; opa=(op>>9)&7; op1=(op>>6)&7; opn=(op>>3)&7; opb=op&7
    lower=(op1<<4)|op0
    if op0<0xf and opa==7 and op1<2:
        imm=op&0x3f
        if op0==14: return 4, 'jmp'
        target=(pc+1+(imm if op1==0 else -imm)) & 0xffffff
        return (4 if nxt==target else 2), ('jcc_t' if nxt==target else 'jcc_n')
    if op0==0xf: return None, 'fxxx'
    if op0==0xe: return None, 'exxx'
    if lower==0x2d: return 4+2*opn, 'push'
    if lower==0x29:
        if op==0x9a98: return 8, 'reti'
        return 4+2*opn, 'pop'
    if op1==0x00: return 6, 'bp'
    if op1==0x01: return 2, 'imm6'
    if op1==0x03: return (7 if opa==7 else 6), 'ind'
    if op1==0x04:
        if opn==0: return (5 if opa==7 else 3), 'r'
        if opn==1: return (5 if opa==7 else 4), 'imm16'
        if opn in (2,3): return (8 if opa==7 else 7), 'mem16'
        return (5 if opa==7 else 3), 'shift'
    if op1==0x05: return (5 if opa==7 else 3), 'shift2'
    if op1==0x06: return (5 if opa==7 else 3), 'rot'
    if op1==0x07: return (6 if opa==7 else 5), 'dir6'
    return None, 'other'

path=sys.argv[1] if len(sys.argv)>1 else base+r'\dbgtest\g2s.tr'
limit=int(sys.argv[2]) if len(sys.argv)>2 else 0
rows=[]
with open(path,'r',encoding='utf-8',errors='ignore') as f:
    for line in f:
        m=re.match(r'^([0-9A-Fa-f]{6}): (.*)$',line.rstrip('\n'))
        if m: rows.append((int(m.group(1),16),m.group(2)))
if limit: rows=rows[:limit]
cycles=0; unknown=Counter(); op0c=Counter(); pcc=Counter()
for i,(pc,txt) in enumerate(rows):
    nxt=rows[i+1][0] if i+1<len(rows) else None
    op=fetch(pc)
    if op is None:
        unknown['nofetch']+=1; continue
    c,kind=guest_cycles(op,pc,nxt)
    op0c[(op>>12)&15]+=1
    if c is None:
        unknown[kind]+=1
    else:
        cycles+=c
print('file',path,'instructions',len(rows),'known_cycles',cycles,'unknown',dict(unknown))
print('op0',dict(sorted(op0c.items())))
print('avg CPI known', cycles/sum(1 for i,(pc,txt) in enumerate(rows) if fetch(pc) is not None and guest_cycles(fetch(pc),pc,rows[i+1][0] if i+1<len(rows) else None)[0] is not None))
# opcode histogram top 30
ops=Counter()
for pc,txt in rows:
    op=fetch(pc)
    if op is not None: ops[op]+=1
print('top opcodes')
for op,n in ops.most_common(30):
    print(f'{op:04X}\t{n}\t{n/len(rows)*100:.2f}%')
