p=r"C:\Users\DuWeisi\digimon-color-passport\tools\r6b\probe.cpp"
s=open(p,encoding='utf-8').read()

# 1) new vectors: per-frame + accumulator
s=s.replace("\tg_cyc_pc.assign(0x400000, 0);\n\tg_cnt_pc.assign(0x400000, 0);",
"""	g_cyc_pc.assign(0x400000, 0);      // accumulator over the whole window
	g_cnt_pc.assign(0x400000, 0);
	g_cyc_frame.assign(0x400000, 0);   // this frame only
	g_cnt_frame.assign(0x400000, 0);""",1)
s=s.replace("static std::vector<uint32_t> g_cnt_pc;    // [pc] instructions",
            "static std::vector<uint32_t> g_cnt_pc;    // [pc] instructions\nstatic std::vector<uint64_t> g_cyc_frame;\nstatic std::vector<uint32_t> g_cnt_frame;",1)

# 2) hooks write into the frame vectors
s=s.replace("		g_cnt_pc[pc]++;","		g_cnt_frame[pc]++;",1)
s=s.replace("		g_cyc_pc[pc] += (uint64_t)n;","		g_cyc_frame[pc] += (uint64_t)n;",1)

# 3) frame-end block: fold frame vectors into the accumulator
old_start = s.index("		if (g_count)\n		{\n			// per-frame cycle split")
old_end   = s.index("		const uint64_t h = fb_hash(soc.lcd.fb);")
new_block = '''		if (g_count)
		{
			// Fold this frame's per-PC charges into the window accumulator and
			// publish the frame's region split.  The charges are the ISA model's
			// own numbers, so the split is exact for the instruction stream that
			// actually ran in this frame.
			for (int r = 0; r <= NREG; r++) { g_fa.blk_cyc[r] = 0; g_fa.blk_insn[r] = 0; }
			g_fa.blk_cyc_total = 0;
			for (uint32_t a = 0; a < 0x400000u; ++a)
			{
				const uint64_t c = g_cyc_frame[a];
				if (!c) continue;
				g_cyc_pc[a] += c;
				g_cnt_pc[a] += g_cnt_frame[a];
				g_fa.blk_cyc_total += c;
				for (int k = 0; k < NREG; k++)
					if (a >= REGIONS[k].lo && a <= REGIONS[k].hi) { g_fa.blk_cyc[k] += c; g_fa.blk_insn[k] += g_cnt_frame[a]; break; }
				g_cyc_frame[a] = 0;
				g_cnt_frame[a] = 0;
			}
			g_fa.blk_cyc[NREG] = g_fa.blk_cyc_total;
			for (int k = 0; k < NREG; k++) g_fa.blk_cyc[NREG] -= g_fa.blk_cyc[k];
			if (g_f_blit)
				std::fprintf(g_f_blit, "%lld,%llu,%u,%06X,%06X,%06X,%06X,%llu,%llu\\n",
				             (long long)f, (unsigned long long)g_fa.blit_px, g_fa.blit_runs,
				             g_fa.src_min, g_fa.src_max, g_fa.dst_min, g_fa.dst_max,
				             (unsigned long long)g_fa.blk_cyc[0], (unsigned long long)g_fa.blk_cyc_total);
		}

'''
s = s[:old_start] + new_block + s[old_end:]
open(p,'w',encoding='utf-8',newline='\n').write(s)
print("patched")
