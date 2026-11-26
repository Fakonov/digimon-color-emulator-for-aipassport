p=r"C:\Users\DuWeisi\digimon-color-passport\tools\r6b\probe.cpp"
s=open(p,encoding='utf-8').read()
s=s.replace("""				const uint64_t c = g_cyc_frame[a];
				if (!c) continue;""","""				const uint64_t c = g_cyc_frame[a];
				const uint32_t n = g_cnt_frame[a];
				if (!c && !n) continue;""",1)
s=s.replace("				g_cnt_pc[a] += g_cnt_frame[a];","				g_cnt_pc[a] += n;",1)
s=s.replace("					if (a >= REGIONS[k].lo && a <= REGIONS[k].hi) { g_fa.blk_cyc[k] += c; g_fa.blk_insn[k] += g_cnt_frame[a]; break; }",
            "					if (a >= REGIONS[k].lo && a <= REGIONS[k].hi) { g_fa.blk_cyc[k] += c; g_fa.blk_insn[k] += n; break; }",1)
open(p,'w',encoding='utf-8',newline='\n').write(s)
print('ok')
