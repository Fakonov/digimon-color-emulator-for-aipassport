import re
p=r"C:\Users\DuWeisi\digimon-color-passport\tools\r6b\probe.cpp"
s=open(p,encoding='utf-8').read()
s=s.replace("\tuint64_t blk_cyc_total = 0;", "\tuint64_t blk_cyc_total = 0;\n\tuint32_t src_min = 0, src_max = 0, dst_min = 0, dst_max = 0;\n\tuint32_t blit_runs = 0, last_src = 0;",1)

# per-frame region cycle attribution in hook mode + per-frame blit summary row
anchor = "\t\tconst uint64_t h = fb_hash(soc.lcd.fb);"
add = '''		if (g_count)
		{
			// per-frame cycle split from the interpreter's own per-instruction
			// charges (exact, no block-table guessing)
			for (int r = 0; r <= NREG; r++) { g_fa.blk_cyc[r] = 0; g_fa.blk_insn[r] = 0; }
			g_fa.blk_cyc_total = 0;
			for (uint32_t a = 0; a < 0x400000u; ++a)
			{
				const uint64_t c = g_cyc_pc[a];
				if (!c) continue;
				g_fa.blk_cyc_total += c;
				for (int k = 0; k < NREG; k++)
					if (a >= REGIONS[k].lo && a <= REGIONS[k].hi) { g_fa.blk_cyc[k] += c; g_fa.blk_insn[k] += g_cnt_pc[a]; break; }
			}
			g_fa.blk_cyc[NREG] = g_fa.blk_cyc_total;
			for (int k = 0; k < NREG; k++) g_fa.blk_cyc[NREG] -= g_fa.blk_cyc[k];
			if (g_f_blit)
				std::fprintf(g_f_blit, "%lld,%llu,%u,%06X,%06X,%06X,%06X,%llu,%llu\\n",
				             (long long)f, (unsigned long long)g_fa.blit_px, g_fa.blit_runs,
				             g_fa.src_min, g_fa.src_max, g_fa.dst_min, g_fa.dst_max,
				             (unsigned long long)g_fa.blk_cyc[0], (unsigned long long)g_fa.blk_cyc_total);
			std::fill(g_cyc_pc.begin(), g_cyc_pc.end(), 0);
			std::fill(g_cnt_pc.begin(), g_cnt_pc.end(), 0);
		}

'''
s=s.replace(anchor, add+anchor,1)
s=s.replace('\tif (g_f_blit) std::fprintf(g_f_blit, "frame,src,len,bias,dstmax,px,cyc0\\n");',
            '\tif (g_f_blit) std::fprintf(g_f_blit, "frame,blit_px,runs,src_min,src_max,dst_min,dst_max,cyc_blitloop,cyc_total\\n");',1)
open(p,'w',encoding='utf-8',newline='\n').write(s)
print("ok")
