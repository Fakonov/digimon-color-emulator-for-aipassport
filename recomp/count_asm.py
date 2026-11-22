import re,sys
from pathlib import Path
for p in sys.argv[1:]:
    lines=Path(p).read_text(encoding='utf-8').splitlines()
    ins=[]
    for l in lines:
        s=l.split('#')[0].strip()
        if not s or s.startswith('.') or s.endswith(':'): continue
        if s.startswith('.size') or s.startswith('.ident') or s.startswith('.section'): continue
        ins.append(s)
    from collections import Counter
    ops=Counter(s.split()[0] for s in ins)
    print(p, 'instructions', len(ins))
    print('  ', ' '.join(f'{k}={v}' for k,v in ops.most_common()))
