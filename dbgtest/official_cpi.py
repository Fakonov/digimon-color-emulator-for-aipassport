import re, sys
from collections import Counter

path = sys.argv[1] if len(sys.argv) > 1 else r'C:\Users\DuWeisi\digimon-color-passport\dbgtest\g2s.tr'
rows = []
with open(path, 'r', encoding='utf-8', errors='ignore') as f:
    for line in f:
        m = re.match(r'^([0-9A-Fa-f]{6}): (.*)$', line.rstrip('\n'))
        if m:
            rows.append((int(m.group(1), 16), m.group(2).strip()))

def regs_before(s, sep):
    a = s.split(sep, 1)[0].strip()
    if not a:
        return 0
    return len([x for x in a.split(',') if x.strip()])

def branch_target(pc, text):
    m = re.search(r'\b([0-9A-Fa-f]{4,6})\b\s*$', text)
    if not m:
        return None
    return int(m.group(1), 16)

def cycles(pc, text, nxt):
    t = text.lower().strip()
    # control
    if re.match(r'^(je|jne|ja|jae|jb|jbe|jg|jge|jl|jle|js|jns|jz|jnz|jc|jnc|jv|jnv)\b', t):
        target = branch_target(pc, t)
        return (4, 4) if target == nxt else (1, 1), 'branch'
    if t.startswith('goto'):
        return (2, 2) if 'mr' in t else (3, 3), 'goto'
    if t.startswith('call'):
        return (4, 4) if 'mr' in t else (3, 3), 'call'
    if t.startswith('retf'):
        return (6, 6), 'retf'
    if t.startswith('reti'):
        return (6, 7), 'reti'
    if t.startswith('break'):
        return (4, 4), 'break'
    if t.startswith('push'):
        n = regs_before(t, ' to ')
        return (n + 1, n + 1), 'push'
    if t.startswith('pop'):
        n = regs_before(t, ' from ')
        return (n + 2, n + 2), 'pop'
    if t.startswith(('int ', 'irq ', 'fiq ', 'irqnest ', 'secbank ', 'fir_mov ', 'fraction ')):
        return (1, 1), 'ctrl'
    if t.startswith(('divq', 'divs', 'exp', 'mul ')):
        return (1, 1), 'muldiv'
    if t.startswith('muls'):
        return (2, 2), 'muls_unknown'
    # shifts by register are 2, immediate 1
    if re.search(r'\b(lsl|lsr|asr|asl|rol|ror|lslor|lsror)\s+r\d', t):
        return (2, 2), 'shift_reg'
    if re.search(r'\b(lsl|lsr|asr|asl|rol|ror|lslor|lsror)\s+[0-9a-f]+', t):
        return (1, 1), 'shift_imm'
    # bit operations / ALU-shift forms
    if re.search(r'\b(bit|tstb|setb|clrb|invb)\b', t):
        return (1, 1), 'bit'
    # memory/ALU forms
    # indirect [r] with optional ++/--
    inc = '++' in t or '--' in t
    # DS indirect
    if re.search(r'ds:\[r\d+\+*\-*\]', t):
        return (3 if inc else 2, 3 if inc else 3), 'ds_ind'
    if re.search(r'\[r\d+\+*\-*\]', t):
        return (3 if inc else 2, 3 if inc else 3), 'ind'
    # BP-relative
    if re.search(r'\[bp\s*[+\-]', t):
        return (1, 2), 'bp_rel'
    # direct 16-bit [xxxx]
    if re.search(r'\[[0-9a-f]{4}\]', t):
        return (2, 2), 'a16'
    # immediate 16-bit assignment/ALU: rX = 1234 / rX += 1234
    if re.search(r'=\s*[0-9a-f]{3,4}\b', t) and not re.search(r'=\s*[0-9a-f]{1,2}\b', t):
        return (2, 2), 'imm16'
    # register assignment/ALU
    if re.search(r'=\s*r\d+\b', t) or re.search(r'[+\-&|^]=\s*r\d+\b', t):
        return (1, 1), 'reg'
    # immediate 6-bit
    if re.search(r'=\s*[0-9a-f]{1,2}\b', t):
        return (1, 1), 'imm6'
    # default ALU/LD
    return (1, 1), 'other'

tot_lo = tot_hi = 0
kinds = Counter()
unknown = Counter()
for i, (pc, text) in enumerate(rows):
    nxt = rows[i+1][0] if i+1 < len(rows) else None
    (lo, hi), kind = cycles(pc, text, nxt)
    tot_lo += lo; tot_hi += hi; kinds[kind] += 1
    if kind in ('other', 'muls_unknown'):
        unknown[text] += 1

n = len(rows)
print('instructions', n)
print('official CPI low/high', tot_lo/n, tot_hi/n)
print('official cycles low/high', tot_lo, tot_hi)
print('kinds', dict(kinds))
print('unknown top')
for k, v in unknown.most_common(20):
    print(v, k)
