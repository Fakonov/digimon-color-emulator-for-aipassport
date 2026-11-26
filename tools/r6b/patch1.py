import io
p=r"C:\Users\DuWeisi\digimon-color-passport\tools\r6b\probe.cpp"
lines=open(p,encoding='utf-8').read().split('\n')
# 1-based 358..388 == index 357..387
start=357; end=387
print("FIRST:", repr(lines[start]))
print("LAST :", repr(lines[end]))
new = r'''		// Keyed-blit accounting.  The loop head runs once per *pixel*; the caller
		// leaves end=[bp+2], i=[bp+6], bias=[bp+4], src=[bp+8]:[bp+7].  Segmenting
		// this into "calls" from inside the hook is not reliable (the loop head is
		// also the back-edge target of nested passes), so the probe records
		// per-frame totals plus the source-pointer span instead.
		if (pc == 0x207CA3u)
		{
			const uint32_t bp = hctx.dev->m_core->m_r[unsp_device::REG_BP] & 0xfffu;
			const uint16_t curi = g_ram[(bp + 6) & 0xfff];
			const uint16_t bias = g_ram[(bp + 4) & 0xfff];
			const uint32_t src  = (((uint32_t)g_ram[(bp + 8) & 0xfff] << 16) | g_ram[(bp + 7) & 0xfff]) & 0xffffffu;
			const uint32_t dst  = (uint32_t)curi + (uint32_t)bias + 0x07BAu;
			if (g_fa.blit_px == 0) { g_fa.src_min = src; g_fa.src_max = src; g_fa.dst_min = dst; g_fa.dst_max = dst; }
			else
			{
				if (src < g_fa.src_min) g_fa.src_min = src;
				if (src > g_fa.src_max) g_fa.src_max = src;
				if (dst < g_fa.dst_min) g_fa.dst_min = dst;
				if (dst > g_fa.dst_max) g_fa.dst_max = dst;
				if (src != g_fa.last_src + 1u) g_fa.blit_runs++;
			}
			g_fa.last_src = src;
			g_fa.blit_px++;            // one pixel per loop-head execution
		}'''.split('\n')
lines[start:end+1]=new
open(p,'w',encoding='utf-8',newline='\n').write('\n'.join(lines))
print("patched")
