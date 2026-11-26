// ---------------------------------------------------------------------------
// tools/r6b/probe.cpp - READ-ONLY instrumented runner for the guest-side
// behaviour audit (docs/notes/R6B_guest.md).
//
// Links the unmodified emulator objects (emu/obj/*.o minus main.o) and adds:
//   * per-frame guest cycle / instruction totals
//   * per-PC cycle attribution inside a frame window
//   * keyed-blit call log (0x207CA3: src pointer, length, dest bias)
//   * LCD window epochs rebuilt from the ST7735 model state (CASET/RASET/RAMWR)
//   * per-frame framebuffer hash (screen change detection)
//
// No emulator source file is modified: the SoC bus is subclassed so every write
// can be observed without touching Gpce4.
// ---------------------------------------------------------------------------
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>
#include <ctime>

#include "unsp.h"
#include "aot.h"
#include "gpce4.h"

static const uint32_t CPU_CLOCK  = 96000000;
static const int      FPS        = 60;
static const int      SLICE_CYCLES = 2000;
static const int64_t  FRAME_CYCLES = CPU_CLOCK / FPS;

// ------------------------------ output sinks -------------------------------
static FILE *g_f_csv    = nullptr;   // per frame
static FILE *g_f_blit   = nullptr;   // per blit call
static FILE *g_f_win    = nullptr;   // per LCD window epoch
static FILE *g_f_evt    = nullptr;   // misc events

// ---------------------------- global counters ------------------------------
static bool     g_count = false;          // detailed per-PC attribution on
static std::vector<uint64_t> g_cyc_pc;    // [pc] cycles
static std::vector<uint32_t> g_cnt_pc;    // [pc] instructions
static std::vector<uint64_t> g_cyc_frame;
static std::vector<uint32_t> g_cnt_frame;
static uint64_t g_insn_total = 0;
static int64_t  g_frame = 0;

static int64_t  g_win_from = -1, g_win_to = -1;   // detailed window (frames)

static const int NREG_MAX = 8;

// per-frame accumulation
struct FrameAcc
{
	int64_t  cyc0 = 0;
	uint64_t insn0 = 0;
	uint32_t lcd_px = 0;
	uint32_t lcd_cmd = 0;
	uint32_t blit_calls = 0;
	uint64_t blit_px = 0;
	uint32_t push_windows = 0;
	// [0..7] = PC regions, [7] doubles as "everything else" until the region
	// table is known at run time; 16 slots keeps the CSV self-describing.
	uint64_t blk_cyc[16] = {};
	uint64_t blk_insn[16] = {};
	uint64_t blk_cyc_total = 0;
	uint32_t src_min = 0, src_max = 0, dst_min = 0, dst_max = 0;
	uint32_t blit_runs = 0, last_src = 0;
};
static FrameAcc g_fa;

// per blit-call tracking
struct BlitState
{
	bool     active = false;
	uint16_t last_i = 0;
	uint32_t src0 = 0;
	uint32_t len = 0;
	uint16_t bias = 0;
	uint32_t dstmax = 0;
	int64_t  cyc0 = 0;
	uint64_t px = 0;
};
static BlitState g_bs;

// LCD window epoch tracking
struct WinState
{
	bool     open = false;
	int      minx = 0, maxx = 0, miny = 0, maxy = 0;
	uint32_t px = 0;
	int64_t  cyc0 = 0;
	int64_t  frame0 = 0;
};
static WinState g_ws;

static uint64_t g_win_epochs = 0;
static int      g_nreg = 0;   // filled in after REGIONS is known

// ---- block-level cycle attribution (fast path, no hooks) ------------------
// The AOT dispatcher increments m_block_hist[entry_pc] once per block entry and
// every generated block charges the per-block cycle sum from the block table, so
// sum(entries * table_cyc) is the frame's cycle split without disturbing the
// interpreter path at all.  See report section on why hooks cannot be used.
static bool     g_blockprof = false;
static uint32_t *g_bhist = nullptr;
static std::vector<uint16_t> g_cyc_tab;   // [pc] block cycle charge (block starts only)
static std::vector<uint8_t>  g_cnt_tab;   // [pc] instructions in that block
static std::vector<uint8_t>  g_isblk;

// The lambdas below are converted to plain function pointers (unsp_device only
// stores a void*), so they cannot capture anything: the live state they need is
// published here instead.
static uint16_t *g_ram = nullptr;
static Gpce4   *g_soc = nullptr;

// ------------------------------ SoC subclass -------------------------------
struct ProbeSoc : Gpce4
{
	void write16(uint32_t addr, uint16_t data) override
	{
		if (addr == 0x3092)                       // SPI2 TX data -> LCD byte
		{
			const uint8_t  cmd  = lcd.m_command;
			const int      step = lcd.m_commandstep;
			const bool     is_cmd = (m_iob_out & 0x0400) == 0;
			const bool     pixel = (cmd == 0x2c) && ((step & 1) == 1);
			if (is_cmd && g_fa.lcd_cmd < 0xffffffffu)
				g_fa.lcd_cmd++;
			if (pixel)
			{
				g_fa.lcd_px++;
				if (!g_ws.open || lcd.m_posminx != g_ws.minx || lcd.m_posmaxx != g_ws.maxx ||
				    lcd.m_posminy != g_ws.miny || lcd.m_posmaxy != g_ws.maxy)
				{
					if (g_ws.open && g_f_win)
						std::fprintf(g_f_win, "%lld,%d,%d,%d,%d,%u,%lld\n",
						             (long long)g_ws.frame0, g_ws.minx, g_ws.maxx,
						             g_ws.miny, g_ws.maxy, g_ws.px, (long long)g_ws.cyc0);
					if (g_ws.open)
						g_win_epochs++;
					g_ws.open = true;
					g_ws.minx = lcd.m_posminx; g_ws.maxx = lcd.m_posmaxx;
					g_ws.miny = lcd.m_posminy; g_ws.maxy = lcd.m_posmaxy;
					g_ws.px = 0; g_ws.cyc0 = (int64_t)m_now; g_ws.frame0 = g_frame;
					g_fa.push_windows++;
				}
				g_ws.px++;
			}
		}
		Gpce4::write16(addr, data);
	}
};

// ------------------------------- helpers -----------------------------------
static std::vector<uint8_t> load_file(const char *path, const char *what)
{
	FILE *fp = std::fopen(path, "rb");
	if (!fp) { std::fprintf(stderr, "cannot open %s: %s\n", what, path); std::exit(2); }
	std::fseek(fp, 0, SEEK_END);
	long len = std::ftell(fp);
	std::fseek(fp, 0, SEEK_SET);
	std::vector<uint8_t> buf((size_t)len);
	if (std::fread(buf.data(), 1, buf.size(), fp) != buf.size()) { std::fprintf(stderr, "short read %s\n", path); std::exit(2); }
	std::fclose(fp);
	return buf;
}

static void write_bmp(const char *path, const uint16_t *fb, int w, int h)
{
	FILE *fp = std::fopen(path, "wb");
	if (!fp) return;
	const int row = (w * 3 + 3) & ~3;
	const int image = row * h;
	uint8_t hdr[54] = {};
	hdr[0] = 'B'; hdr[1] = 'M';
	uint32_t fsize = 54 + image;
	std::memcpy(hdr + 2, &fsize, 4);
	uint32_t off = 54;
	std::memcpy(hdr + 10, &off, 4);
	uint32_t ihs = 40;
	std::memcpy(hdr + 14, &ihs, 4);
	int32_t bw = w, bh = h;
	std::memcpy(hdr + 18, &bw, 4);
	std::memcpy(hdr + 22, &bh, 4);
	uint16_t planes = 1, bpp = 24;
	std::memcpy(hdr + 26, &planes, 2);
	std::memcpy(hdr + 28, &bpp, 2);
	std::fwrite(hdr, 1, 54, fp);
	std::vector<uint8_t> line((size_t)row, 0);
	for (int y = h - 1; y >= 0; y--)
	{
		for (int x = 0; x < w; x++)
		{
			uint16_t c = fb[y * w + x];
			uint8_t r = (uint8_t)((c >> 11) & 0x1f), g = (uint8_t)((c >> 5) & 0x3f), b = (uint8_t)(c & 0x1f);
			line[x * 3 + 0] = (uint8_t)((b << 3) | (b >> 2));
			line[x * 3 + 1] = (uint8_t)((g << 2) | (g >> 4));
			line[x * 3 + 2] = (uint8_t)((r << 3) | (r >> 2));
		}
		std::fwrite(line.data(), 1, line.size(), fp);
	}
	std::fclose(fp);
}

static uint64_t fb_hash(const uint16_t *fb)
{
	uint64_t h = 1469598103934665603ull;
	for (int i = 0; i < 128 * 128; i++)
	{
		h ^= fb[i];
		h *= 1099511628211ull;
	}
	return h;
}

// PC region buckets used for the coarse per-frame split.
struct Region { const char *name; uint32_t lo, hi; };
static const Region REGIONS[] = {
	{ "blit_loop",   0x207CA3, 0x207CC7 },   // keyed transparent blit, per pixel
	{ "drawlib",     0x207A00, 0x207D01 },   // sprite/rect library incl. blit
	{ "fill_loop",   0x005B1E, 0x005B4A },   // constant fill -> LCD
	{ "push_loop",   0x005B4A, 0x005B90 },   // LCD pixel push
	{ "mul32",       0x005F8B, 0x005FC0 },   // 32x32 multiply helper
	{ "park",        0x005000, 0x005400 },   // park/idle wait loops
	{ "mainloop",    0x230000, 0x230400 },   // top-level state machine
	{ "clock_idle",  0x200340, 0x200350 },   // clock screen idle
};
static const int NREG = (int)(sizeof(REGIONS) / sizeof(REGIONS[0]));

// ------------------------------- main --------------------------------------
int main(int argc, char **argv)
{
	int64_t frames = 600;
	struct PressSpec { uint16_t mask; int64_t start, end; };
	std::vector<PressSpec> press;
	const char *csv_path = nullptr, *blit_path = nullptr, *win_path = nullptr, *evt_path = nullptr;
	const char *pc_dump_path = nullptr;
	const char *dump_dir = nullptr;
	int64_t dump_from = -1, dump_to = -1;
	int dump_every = 1;
	const char *load_flash = nullptr;
	const char *ram_out = nullptr;
	int64_t ram_frame = -1;
	bool quiet = false;
	bool nohooks = false;
	bool clock_sync = false;
	int64_t clock_frame = 120;
	bool blockprof = false;

	for (int i = 1; i < argc; i++)
	{
		std::string a = argv[i];
		auto nx = [&]() { return (i + 1 < argc) ? argv[++i] : (const char *)nullptr; };
		if (a == "--frames") frames = std::atoll(nx());
		else if (a == "--press")
		{
			std::string spec = nx();
			size_t pos = 0;
			while (pos <= spec.size())
			{
				size_t comma = spec.find(',', pos);
				std::string it = spec.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
				if (!it.empty())
				{
					size_t c1 = it.find(':');
					size_t c2 = (c1 == std::string::npos) ? std::string::npos : it.find(':', c1 + 1);
					PressSpec ps{};
					ps.mask  = (uint16_t)std::strtoul(it.substr(0, c1).c_str(), nullptr, 16);
					ps.start = (c1 == std::string::npos) ? 0 : std::atoll(it.substr(c1 + 1, c2 == std::string::npos ? std::string::npos : c2 - c1 - 1).c_str());
					ps.end   = (c2 == std::string::npos) ? ps.start + 6 : std::atoll(it.substr(c2 + 1).c_str());
					press.push_back(ps);
				}
				if (comma == std::string::npos) break;
				pos = comma + 1;
			}
		}
		else if (a == "--csv") csv_path = nx();
		else if (a == "--blits") blit_path = nx();
		else if (a == "--windows") win_path = nx();
		else if (a == "--events") evt_path = nx();
		else if (a == "--pc-dump") pc_dump_path = nx();
		else if (a == "--dump-dir") dump_dir = nx();
		else if (a == "--dump-from") dump_from = std::atoll(nx());
		else if (a == "--dump-to") dump_to = std::atoll(nx());
		else if (a == "--dump-every") dump_every = std::atoi(nx());
		else if (a == "--load-flash") load_flash = nx();
		else if (a == "--quiet") quiet = true;
		else if (a == "--ram-out") ram_out = nx();
		else if (a == "--ram-frame") ram_frame = std::atoll(nx());
		else if (a == "--win-from") g_win_from = std::atoll(nx());
		else if (a == "--win-to")   g_win_to   = std::atoll(nx());
		else if (a == "--nohooks") nohooks = true;
		else if (a == "--clock-sync") clock_sync = true;
		else if (a == "--clock-frame") clock_frame = std::atoll(nx());
		else if (a == "--blockprof") blockprof = true;
		else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
	}

	std::vector<uint8_t> irom  = load_file("roms/internal_rom_221017a.bin", "internal rom");
	std::vector<uint8_t> flash = load_file("roms/dmc_v1_base.bin", "spi flash");
	if (load_flash)
	{
		std::vector<uint8_t> saved = load_file(load_flash, "flash save");
		if (saved.size() == flash.size()) std::memcpy(flash.data(), saved.data(), flash.size());
	}

	ProbeSoc soc;
	soc.set_images(irom.data(), irom.size(), flash.data(), flash.size());
	soc.reset();

	unsp_20_device cpu;
	cpu.m_bus = &soc;
	soc.set_cpu(&cpu);
	// Same knobs as emu/main.cpp so the fast path can be pinned down when the
	// hook path has to produce a bit-identical timeline (see report 3.2).
	if (std::getenv("GPCE4_AOT") && std::getenv("GPCE4_AOT")[0] == '0')        cpu.m_aot = false;
	if (std::getenv("GPCE4_HOT") && std::getenv("GPCE4_HOT")[0] == '0')      cpu.m_fasthot = false;
	if (std::getenv("GPCE4_FASTSPIN") && std::getenv("GPCE4_FASTSPIN")[0] == '0') cpu.m_fastspin = false;
	if (std::getenv("GPCE4_FASTWAIT") && std::getenv("GPCE4_FASTWAIT")[0] == '0') cpu.m_fastwait = false;

	cpu.reset_state();
	cpu.m_vectorbase = 0x4010;
	cpu.m_core->m_r[0] = 0;
	for (int r = 1; r < 16; r++) cpu.m_core->m_r[r] = 0;
	cpu.m_core->m_r[unsp_device::REG_PC] = soc.read16(0x4010 + 0x7);
	cpu.m_core->m_enable_irq = 0;
	cpu.m_core->m_enable_fiq = 0;
	cpu.m_core->m_fir_move = 1;
	cpu.m_core->m_pri = 8;
	cpu.m_core->m_divq_bit = 0xffffffffu;

	if (blockprof)
	{
		g_blockprof = true;
		g_cyc_tab.assign(0x400000, 0);
		g_cnt_tab.assign(0x400000, 0);
		g_isblk.assign(0x400000, 0);
		for (uint32_t pc = 0; pc < 0x400000u; ++pc)
		{
			uint8_t cnt = 0; uint16_t cyc = 0;
			if (aot_lookup(pc, &cnt, &cyc)) { g_cyc_tab[pc] = cyc; g_cnt_tab[pc] = cnt; g_isblk[pc] = 1; }
		}
		g_bhist = (uint32_t *)std::calloc(0x400000u, sizeof(uint32_t));
		cpu.m_block_hist = g_bhist;
	}

	g_cyc_pc.assign(0x400000, 0);      // accumulator over the whole window
	g_cnt_pc.assign(0x400000, 0);
	g_cyc_frame.assign(0x400000, 0);   // this frame only
	g_cnt_frame.assign(0x400000, 0);

	g_ram = soc.ram;
	g_soc = &soc;

	// ---- hooks -------------------------------------------------------------
	struct HookCtx { unsp_device *dev; };
	static HookCtx hctx;
	hctx.dev = &cpu;

	if (!nohooks)
	cpu.m_insn_hook = [](void *o, uint32_t lpc) {
		(void)o;
		const uint32_t pc = lpc & 0xffffffu;
		g_insn_total++;
		if (!g_count) return;
		g_cnt_frame[pc]++;
		// Keyed-blit accounting.  The loop head runs once per *pixel*; the caller
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
		}
	};
	if (!nohooks) {
	cpu.m_insn_hook_opaque = &hctx;

	cpu.m_cycle_hook = [](void *o, uint32_t lpc, int n) {
		(void)o;
		if (!g_count) return;
		const uint32_t pc = lpc & 0xffffffu;
		g_cyc_frame[pc] += (uint64_t)n;
	};
	cpu.m_cycle_hook_opaque = &hctx;
	}

	// ---- files -------------------------------------------------------------
	if (csv_path)  g_f_csv  = std::fopen(csv_path, "w");
	if (blit_path) g_f_blit = std::fopen(blit_path, "w");
	if (win_path)  g_f_win  = std::fopen(win_path, "w");
	if (evt_path)  g_f_evt  = std::fopen(evt_path, "w");
	if (g_f_csv)
	{
		std::fprintf(g_f_csv, "frame,cyc0,cyc_used,insn,lcd_px,lcd_cmd,blit_calls,blit_px,win_epochs,fbhash,blk_cyc_total");
		for (int r = 0; r <= NREG; r++) std::fprintf(g_f_csv, ",cyc_%s", r < NREG ? REGIONS[r].name : "other");
		std::fprintf(g_f_csv, "\n");
	}
	if (g_f_blit) std::fprintf(g_f_blit, "frame,blit_px,runs,src_min,src_max,dst_min,dst_max,cyc_blitloop,cyc_total\n");
	if (g_f_win)  std::fprintf(g_f_win, "frame,minx,maxx,miny,maxy,px,cyc0\n");

	int64_t cycle = 0;
	uint64_t prev_hash = ~0ull;
	static uint16_t prev_pv = 0xffff;

	for (int64_t f = 0; f < frames; f++)
	{
		g_frame = f;
		g_fa = FrameAcc();
		g_fa.cyc0 = cycle;
		g_fa.insn0 = g_insn_total;

		// keep the blit/window trackers in step with the frame boundary
		g_bs.active = false;

		if (!press.empty())
		{
			uint16_t pv = 0xffff;
			for (const PressSpec &ps : press)
				if (f >= ps.start && f < ps.end) pv &= (uint16_t)~ps.mask;
			if ((pv & (uint16_t)~prev_pv) != 0)
			{
				const uint32_t wpc = ((cpu.m_core->m_r[unsp_device::REG_SR] & 0x3fu) << 16) | (cpu.m_core->m_r[unsp_device::REG_PC] & 0xffffu);
				if (g_f_evt) std::fprintf(g_f_evt, "press frame=%lld mask=%04X at_pc=%06X halted=%d\n", (long long)f, unsigned(pv), unsigned(wpc), soc.halted() ? 1 : 0);
				soc.wake_from_sleep();
			}
			prev_pv = pv;
			soc.set_port(0, pv);
		}

		if (clock_sync && f == clock_frame)
		{
			// emu/main.cpp:441 seed_host_clock(): the handset has no RTC chip, so the
			// host time is injected once and the guest's own 2 Hz handler advances it.
			time_t now = time(nullptr);
			struct tm lt;
			localtime_s(&lt, &now);
			soc.ram[0x0CB0] = 1;
			soc.ram[0x0CAF] = (uint16_t)lt.tm_hour;
			soc.ram[0x0CAE] = (uint16_t)lt.tm_min;
			soc.ram[0x0CAD] = (uint16_t)lt.tm_sec;
			soc.ram[0x0CB1] = 0;
			soc.ram[0x0CB2] = 1;
			if (g_f_evt) std::fprintf(g_f_evt, "clock seed frame=120 %02d:%02d:%02d\n", lt.tm_hour, lt.tm_min, lt.tm_sec);
		}

		if (g_win_from >= 0 && f == g_win_from) { g_count = true; std::fill(g_cyc_pc.begin(), g_cyc_pc.end(), 0); std::fill(g_cnt_pc.begin(), g_cnt_pc.end(), 0); }

		const int64_t frame_end = cycle + FRAME_CYCLES;
		while (cycle < frame_end)
		{
			if (soc.halted())
			{
				int64_t remain = frame_end - cycle;
				uint32_t slice = soc.cycles_to_next_event((uint32_t)std::min<int64_t>(remain, SLICE_CYCLES));
				if (slice == 0) slice = 1;
				soc.tick(slice);
				cycle += slice;
				continue;
			}
			int64_t remain = frame_end - cycle;
			uint32_t slice = soc.cycles_to_next_event((uint32_t)std::min<int64_t>(remain, SLICE_CYCLES));
			if (slice == 0) slice = 1;
			cpu.m_core->m_icount = (int32_t)slice - 1;
			cpu.execute_run();
			const int64_t consumed = (int64_t)((int32_t)slice - 1) - (int64_t)(int32_t)cpu.m_core->m_icount;
			soc.tick((uint32_t)(consumed > 0 ? consumed : slice));
			cycle += consumed > 0 ? consumed : slice;
		}

		// close the pending blit / window records
		if (g_count && g_bs.active && g_f_blit)
			std::fprintf(g_f_blit, "%lld,%u,%u,%u,%u,%llu,%lld\n",
			             (long long)f, g_bs.src0, g_bs.len, g_bs.bias, g_bs.dstmax,
			             (unsigned long long)g_bs.px, (long long)g_bs.cyc0);

		if (g_blockprof && g_bhist)
		{
			for (int r = 0; r <= NREG; r++) { g_fa.blk_cyc[r] = 0; g_fa.blk_insn[r] = 0; }
			g_fa.blk_cyc_total = 0;
			for (uint32_t pc = 0; pc < 0x400000u; ++pc)
			{
				const uint32_t n = g_bhist[pc];
				if (!n) continue;
				if (g_isblk[pc])
				{
					const uint64_t c = (uint64_t)n * g_cyc_tab[pc];
					int r = NREG;
					for (int k = 0; k < NREG; k++)
						if (pc >= REGIONS[k].lo && pc <= REGIONS[k].hi) { r = k; break; }
					g_fa.blk_cyc[r] += c;
					g_fa.blk_insn[r] += (uint64_t)n * g_cnt_tab[pc];
					g_fa.blk_cyc_total += c;
				}
				g_bhist[pc] = 0;
			}
		}

		if (g_count)
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
				const uint32_t n = g_cnt_frame[a];
				if (!c && !n) continue;
				g_cyc_pc[a] += c;
				g_cnt_pc[a] += n;
				g_fa.blk_cyc_total += c;
				for (int k = 0; k < NREG; k++)
					if (a >= REGIONS[k].lo && a <= REGIONS[k].hi) { g_fa.blk_cyc[k] += c; g_fa.blk_insn[k] += n; break; }
				g_cyc_frame[a] = 0;
				g_cnt_frame[a] = 0;
			}
			g_fa.blk_cyc[NREG] = g_fa.blk_cyc_total;
			for (int k = 0; k < NREG; k++) g_fa.blk_cyc[NREG] -= g_fa.blk_cyc[k];
			if (g_f_blit)
				std::fprintf(g_f_blit, "%lld,%llu,%u,%06X,%06X,%06X,%06X,%llu,%llu\n",
				             (long long)f, (unsigned long long)g_fa.blit_px, g_fa.blit_runs,
				             g_fa.src_min, g_fa.src_max, g_fa.dst_min, g_fa.dst_max,
				             (unsigned long long)g_fa.blk_cyc[0], (unsigned long long)g_fa.blk_cyc_total);
		}

		const uint64_t h = fb_hash(soc.lcd.fb);
		if (g_f_csv)
		{
			std::fprintf(g_f_csv, "%lld,%lld,%lld,%llu,%u,%u,%u,%llu,%u,%016llX,%llu",
			             (long long)f, (long long)g_fa.cyc0, (long long)(cycle - g_fa.cyc0),
			             (unsigned long long)(g_insn_total - g_fa.insn0),
			             g_fa.lcd_px, g_fa.lcd_cmd, g_fa.blit_calls,
			             (unsigned long long)g_fa.blit_px, g_fa.push_windows,
			             (unsigned long long)h, (unsigned long long)g_fa.blk_cyc_total);
			for (int r = 0; r <= NREG; r++)
				std::fprintf(g_f_csv, ",%llu", (unsigned long long)g_fa.blk_cyc[r]);
			std::fprintf(g_f_csv, "\n");
		}
		if (g_f_evt && h != prev_hash)
			std::fprintf(g_f_evt, "fbchange frame=%lld pc=%06X hash=%016llX lcd_px=%u blit_px=%llu\n",
			             (long long)f, (unsigned)(((cpu.m_core->m_r[unsp_device::REG_SR] & 0x3fu) << 16) | (cpu.m_core->m_r[unsp_device::REG_PC] & 0xffffu)),
			             (unsigned long long)h, g_fa.lcd_px, (unsigned long long)g_fa.blit_px);
		prev_hash = h;

		if (dump_dir && dump_from >= 0 && f >= dump_from && (dump_to < 0 || f <= dump_to) &&
		    dump_every > 0 && ((f - dump_from) % dump_every) == 0)
		{
			char name[512];
			std::snprintf(name, sizeof(name), "%s/fb_%06lld.bmp", dump_dir, (long long)f);
			write_bmp(name, soc.lcd.fb, 128, 128);
		}

		if (ram_out && f == ram_frame)
		{
			FILE *fp = std::fopen(ram_out, "wb");
			if (fp) { std::fwrite(soc.ram, 2, 4096, fp); std::fclose(fp); }
			if (g_f_evt) std::fprintf(g_f_evt, "ramdump frame=%lld -> %s\n", (long long)f, ram_out);
		}

		if (g_win_from >= 0 && f == g_win_to) g_count = false;
	}

	// flush the last open window epoch
	if (g_ws.open && g_f_win)
		std::fprintf(g_f_win, "%lld,%d,%d,%d,%d,%u,%lld\n",
		             (long long)g_ws.frame0, g_ws.minx, g_ws.maxx, g_ws.miny, g_ws.maxy, g_ws.px, (long long)g_ws.cyc0);

	// ---- report ------------------------------------------------------------
	if (!quiet)
	{
		std::printf("frames=%lld cycles=%lld guest_s=%.3f insns=%llu\n",
		            (long long)frames, (long long)cycle, (double)cycle / CPU_CLOCK,
		            (unsigned long long)g_insn_total);
	}
	{
		extern unsigned long gpce4_ioa_reads();
		std::printf("final PC=%06X SR=%04X SP=%04X display_on=%d ioa_reads=%lu\n",
		            (unsigned)(((cpu.m_core->m_r[unsp_device::REG_SR] & 0x3f) << 16) | (cpu.m_core->m_r[unsp_device::REG_PC])),
		            (unsigned)(cpu.m_core->m_r[unsp_device::REG_SR] & 0xffff),
		            (unsigned)(cpu.m_core->m_r[0] & 0xffff), soc.lcd.display_on ? 1 : 0, gpce4_ioa_reads());
	}

	// per-PC cycle attribution summary for the detailed window
	if (g_win_from >= 0)
	{
		struct Row { uint32_t pc; uint64_t cyc, cnt; };
		std::vector<Row> rows;
		uint64_t tot = 0, totc = 0;
		for (uint32_t a = 0; a < g_cyc_pc.size(); ++a)
			if (g_cyc_pc[a]) { rows.push_back({a, g_cyc_pc[a], g_cnt_pc[a]}); tot += g_cyc_pc[a]; totc += g_cnt_pc[a]; }
		std::sort(rows.begin(), rows.end(), [](const Row &x, const Row &y) { return x.cyc > y.cyc; });
		std::printf("[window] frames %lld..%lld  cycles=%llu  insns=%llu  distinct_pc=%zu\n",
		            (long long)g_win_from, (long long)g_win_to,
		            (unsigned long long)tot, (unsigned long long)totc, rows.size());
		std::printf("[window] region split:\n");
		for (int r = 0; r < NREG; r++)
		{
			uint64_t c = 0, n = 0;
			for (uint32_t a = REGIONS[r].lo; a <= REGIONS[r].hi; ++a) { c += g_cyc_pc[a]; n += g_cnt_pc[a]; }
			std::printf("  %-11s %08X..%08X  cyc=%9llu (%5.2f%%)  insn=%9llu  CPI=%.2f\n",
			            REGIONS[r].name, REGIONS[r].lo, REGIONS[r].hi,
			            (unsigned long long)c, tot ? 100.0 * c / (double)tot : 0.0,
			            (unsigned long long)n, n ? (double)c / (double)n : 0.0);
		}
		std::printf("[window] top 25 PCs by cycles:\n");
		for (size_t i = 0; i < rows.size() && i < 25; i++)
			std::printf("  %06X cyc=%9llu (%5.2f%%) insn=%8llu CPI=%.2f\n",
			            rows[i].pc, (unsigned long long)rows[i].cyc, 100.0 * rows[i].cyc / (double)tot,
			            (unsigned long long)rows[i].cnt, rows[i].cnt ? (double)rows[i].cyc / (double)rows[i].cnt : 0.0);
	}

	if (pc_dump_path)
	{
		FILE *fp = std::fopen(pc_dump_path, "w");
		if (fp)
		{
			std::fprintf(fp, "# pc cyc insn\n");
			for (uint32_t a = 0; a < 0x400000u; ++a)
				if (g_cyc_pc[a] || g_cnt_pc[a])
					std::fprintf(fp, "%06X %llu %llu\n", a,
					             (unsigned long long)g_cyc_pc[a], (unsigned long long)g_cnt_pc[a]);
			std::fclose(fp);
		}
	}

	if (g_f_csv)  std::fclose(g_f_csv);
	if (g_f_blit) std::fclose(g_f_blit);
	if (g_f_win)  std::fclose(g_f_win);
	if (g_f_evt)  std::fclose(g_f_evt);
	return 0;
}