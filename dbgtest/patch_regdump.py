import io

p = 'emu/main.cpp'
s = io.open(p, encoding='utf-8').read()

a = "\t\tstd::unordered_map<uint32_t, uint64_t> *hist = nullptr;\n\t};"
b = ("\t\tstd::unordered_map<uint32_t, uint64_t> *hist = nullptr;\n"
     "\t\tconst uint32_t *regs = nullptr;\n"
     "\t\tconst uint32_t *sbv = nullptr;\n"
     "\t\tFILE *regd = nullptr;\n"
     "\t\tlong regd_left = 0;\n"
     "\t};")
assert s.count(a) == 1, s.count(a)
s = s.replace(a, b)

a = "\tif (pc_hist_path) hook.hist = &pc_hist;"
b = ("\tif (pc_hist_path) hook.hist = &pc_hist;\n"
     "\t{\n"
     "\t\tconst char *rdp = std::getenv(\"GPCE4_REGDUMP\");\n"
     "\t\tif (rdp)\n"
     "\t\t{\n"
     "\t\t\thook.regd = std::fopen(rdp, \"wb\");\n"
     "\t\t\thook.regd_left = 40000;\n"
     "\t\t\thook.regs = cpu.m_core->m_r;\n"
     "\t\t\thook.sbv = &cpu.m_core->m_sb;\n"
     "\t\t}\n"
     "\t}")
assert s.count(a) == 1, s.count(a)
s = s.replace(a, b)

a = "\tif (hook.trace || hook.hist || hook.bp != 0xffffffffu || hook.ram_at >= 0)"
b = "\tif (hook.trace || hook.hist || hook.bp != 0xffffffffu || hook.ram_at >= 0 || hook.regd)"
assert s.count(a) == 1, s.count(a)
s = s.replace(a, b)

a = "\t\t\tif (c->trace && c->tracing) c->trace->emit(lpc);"
b = ("\t\t\tif (c->regd && c->regd_left > 0)\n"
     "\t\t\t{\n"
     "\t\t\t\t--c->regd_left;\n"
     "\t\t\t\tconst uint32_t *r = c->regs;\n"
     "\t\t\t\tstd::fprintf(c->regd, \"%06X %04X %04X %04X %04X %04X %04X %04X %X\\n\", pc,\n"
     "\t\t\t\t\tr[0] & 0xffffu, r[1] & 0xffffu, r[2] & 0xffffu, r[3] & 0xffffu,\n"
     "\t\t\t\t\tr[4] & 0xffffu, r[5] & 0xffffu, r[6] & 0xffffu, *c->sbv & 0xfu);\n"
     "\t\t\t}\n"
     "\t\t\tif (c->trace && c->tracing) c->trace->emit(lpc);")
assert s.count(a) == 1, s.count(a)
s = s.replace(a, b)

io.open(p, 'w', encoding='utf-8', newline='').write(s)
print('patched main.cpp ok')
