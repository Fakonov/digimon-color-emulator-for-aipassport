import io

p = 'emu/main.cpp'
s = io.open(p, encoding='utf-8').read()

if '#include "aot.h"' not in s:
    a = '#include "unsp.h"'
    assert s.count(a) == 1
    s = s.replace(a, '#include "unsp.h"\n#include "aot.h"')

a = "\t\tconst uint32_t *regs = nullptr;"
b = ("\t\tunsp_device *dev = nullptr;\n"
     "\t\tconst uint32_t *regs = nullptr;")
assert s.count(a) == 1
s = s.replace(a, b)

a = "\t\t\thook.regs = cpu.m_core->m_r;"
b = "\t\t\thook.dev = &cpu;\n\t\t\thook.regs = cpu.m_core->m_r;"
assert s.count(a) == 1
s = s.replace(a, b)

a = ("\t\t\t\tconst uint32_t *r = c->regs;\n"
     "\t\t\t\tstd::fprintf(c->regd, \"%06X %04X %04X %04X %04X %04X %04X %04X %X\\n\", pc,\n"
     "\t\t\t\t\tr[0] & 0xffffu, r[1] & 0xffffu, r[2] & 0xffffu, r[3] & 0xffffu,\n"
     "\t\t\t\t\tr[4] & 0xffffu, r[5] & 0xffffu, r[6] & 0xffffu, *c->sbv & 0xfu);")
b = ("\t\t\t\tconst AotCtx *ac = (const AotCtx *)c->dev->m_aot_cur;\n"
     "\t\t\t\tconst uint32_t *r = ac ? ac->r : c->regs;\n"
     "\t\t\t\tconst uint32_t sbv = ac ? ac->sb : *c->sbv;\n"
     "\t\t\t\tstd::fprintf(c->regd, \"%06X %04X %04X %04X %04X %04X %04X %04X %X\\n\", pc,\n"
     "\t\t\t\t\tr[0] & 0xffffu, r[1] & 0xffffu, r[2] & 0xffffu, r[3] & 0xffffu,\n"
     "\t\t\t\t\tr[4] & 0xffffu, r[5] & 0xffffu, r[6] & 0xffffu, sbv & 0xfu);")
assert s.count(a) == 1, s.count(a)
s = s.replace(a, b)

io.open(p, 'w', encoding='utf-8', newline='').write(s)
print('patched main.cpp ok')
