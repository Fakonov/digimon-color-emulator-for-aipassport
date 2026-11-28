// unSP execution harness: the parts of MAME's unsp.cpp that are not ISA
// decoding (flags, stack, FR/DS registers, interrupt entry) plus the run loop.
// Semantics copied 1:1 from ref/unsp/unsp.cpp (MAME 0.289).
#include "unsp.h"
#include <cstring>
//
// C3 port: execute_run() also bumps the guest instruction counter, which is
// what the firmware reports as mi_s (million guest instructions per second).
#include "hotblock.h"
#include "aot.h"
#include <cstdio>
#include <utility>

// spin_collapse probe counters (defined here so the console can print them).
// C3_SPIN_DIAG=0 compiles the increments out: they are several DRAM
// read-modify-writes per dispatch, and they have already answered their
// question (the collapse at 0x5251 is taken roughly half the times it is
// offered).  With the counters off the STAT fields just read zero.
uint32_t g_spin_calls = 0;
uint32_t g_spin_seen = 0;
uint32_t g_spin_big = 0;
uint32_t g_spin_short = 0;
uint32_t g_spin_val = 0;      // last [bp+0] read at 0x5251/0x5376
uint32_t g_spin_bp = 0;       // last BP
int32_t g_spin_ic = 0;        // last icount on entry
uint32_t g_spin_ok = 0;       // collapses actually taken
#ifndef C3_SPIN_DIAG
#define C3_SPIN_DIAG 0
#endif
#if C3_SPIN_DIAG
#define SPIN_PROBE(expr) (void)(expr)
#else
#define SPIN_PROBE(expr) ((void)0)
#endif

// The C3 executes this file from flash through a 16 KiB icache, and the whole
// dispatch path - execute_run, aot_step, the inline aot_lookup - misses on
// almost every block.  The recompiled blocks themselves already sit in IRAM
// (AOT_IRAM), so the fetch of the dispatcher, not the block, is what costs
// ~260 cycles per guest instruction.  Keep the dispatcher in IRAM too.
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_cpu.h"
#define UNSP_HOT IRAM_ATTR
#else
#define UNSP_HOT
#endif

// Cycle split: how much of the host time each dispatch path actually costs.
// Three wrong guesses in a row (page cache, compact table, IRAM promotion) all
// came from inferring the split instead of measuring it, so the loop now reads
// the cycle counter around the two exits.  Two CSR reads per dispatch cost
// roughly 1.5% of a frame, which is the price of not guessing.
// Set C3_SPLIT=0 in CMakeLists.txt to compile the whole thing out.
#ifndef C3_SPLIT
#define C3_SPLIT 1
#endif

// ---------------------------------------------------------------------------
// Diagnostic accumulators that live inside the dispatch loop.  Both were added
// to answer one specific question and both answer it already:
//
//   C3_IRQ_DIAG   the 2026-11-23 interrupt investigation.  It proved IRQ7 is
//                 raised and taken (rise/fall 329/329, isr=987); AUDIT_2026-11-23
//                 has the numbers.  Keeping it on costs two compares plus a
//                 store per dispatch, i.e. a permanent tax on the hottest loop
//                 in the firmware to re-answer a settled question.
//   C3_REGION_STAT  the per-region instruction split that produced "rflash
//                 680k/s, rdraw 12k/s".  It is six compares and two DRAM
//                 read-modify-writes per AOT block, and a block averages 6.6
//                 guest instructions, so it is roughly one compare per guest
//                 instruction retired.
//
// Default 0 (off).  Re-enable by passing -DC3_IRQ_DIAG=1 / -DC3_REGION_STAT=1
// to this component in main/CMakeLists.txt; the console's S/O reports read the
// same globals either way and simply show zeros while the counters are off.
#ifndef C3_IRQ_DIAG
#define C3_IRQ_DIAG 0
#endif
#ifndef C3_REGION_STAT
#define C3_REGION_STAT 0
#endif
// wait_loop_collapse() has been permanently disabled since the 2026-11-23 probe
// ("TEMP PROBE: ... Disable the collapse to test that theory", left as
// `if (true) return false;`).  The probe answered its question, so the body is
// behind a compile switch instead of being called once per dispatch and
// returning false.  Re-enable with -DC3_WAIT_COLLAPSE=1 (docs/C3_PORT.md 8.2.1).
#ifndef C3_WAIT_COLLAPSE
#define C3_WAIT_COLLAPSE 0
#endif
// fast_step() is off (m_fastops defaults to false and measured slower on RV32).
// The fastops body and its counters are only worth compiling for an experiment.
#ifndef C3_FASTOPS
#define C3_FASTOPS 0
#endif
// Shadow-execution check of fast_step against the interpreter.  Off everywhere:
// it doubles the work of every interpreted instruction.
#ifndef C3_VERIFY
#define C3_VERIFY 0
#endif

#if C3_SPLIT && defined(ESP_PLATFORM)
uint32_t g_cyc_aot = 0;      // inside aot_step (block executed)
uint32_t g_cyc_inf = 0;      // fetch + fast_step/execute_one (no block)
uint32_t g_cyc_fast = 0;     // fast_hot / collapse / HLE paths
uint32_t g_disp_aot = 0;     // dispatch counts, for cycles-per-dispatch
uint32_t g_disp_inf = 0;
#else
// Same names exist in the production build so the console/STAT code links, but
// nothing ever increments them and the per-window percentages read 0.
uint32_t g_cyc_aot = 0, g_cyc_inf = 0, g_cyc_fast = 0;
uint32_t g_disp_aot = 0, g_disp_inf = 0;
#endif

#if C3_SPLIT && defined(ESP_PLATFORM)
// Finer split of aot_step itself: the table walk / context plumbing ("lk") and
// the generated block body ("fn").  If "fn" is a small share of "aot", the cost
// is per-dispatch overhead and no amount of block-body work will help.
uint32_t g_cyc_lk = 0;
uint32_t g_cyc_fn = 0;
// Cycles spent inside block bodies of two specific guest regions, so "how much
// would HLE-ing this region buy" is measured rather than extrapolated from
// instruction counts (which badly overstate cheap instructions).
uint32_t g_cyc_draw = 0;   // 0x207A00-0x207CFF: the flash drawing library
uint32_t g_cyc_push = 0;   // 0x005A00-0x005B60: the internal-ROM LCD byte push
#else
uint32_t g_cyc_lk = 0, g_cyc_fn = 0, g_cyc_draw = 0, g_cyc_push = 0;
#endif

#if C3_AOTACC
uint32_t g_rd_fast = 0, g_rd_slow = 0, g_wr_fast = 0, g_wr_slow = 0;
#endif

#if C3_SPLIT && defined(ESP_PLATFORM)
#define SPLIT_T0() const uint32_t __t0 = esp_cpu_get_cycle_count()
#define SPLIT_ADD(var) var += (esp_cpu_get_cycle_count() - __t0)
#else
#define SPLIT_T0() ((void)0)
#define SPLIT_ADD(var) ((void)0)
#endif

// ---- HLE attribution (C3_HLEPROF) ----------------------------------------
// The frame diagnostics could say how many guest cycles a long frame used and
// how many pixels went through each hook, but not where the *host* time went.
// Two rounds of auditing guessed at that ("must be the sprite data reads") and
// could not settle it.  One cycle-counter read around each hook call answers it
// directly: the hook is entered once per row/blit, not once per pixel.
//
// Cost, measured in the R7 audit: 2 rdcycle reads + one .bss read-modify-write
// per entry, i.e. 8-12 host cycles and not the "~3" this comment used to claim.
// A redraw frame enters the hooks ~2,650 times, so it is ~25 K cycles (0.33 %);
// the only consumer is the LONGF line's hp= field (STAT does not use it).
// Off by default since v1.17.0; -DC3_HLEPROF=1 brings the attribution back.
#ifndef C3_HLEPROF
#define C3_HLEPROF 0
#endif
#if C3_HLEPROF && defined(ESP_PLATFORM)
uint32_t g_h_band = 0;   // hle_render_band
uint32_t g_h_blit = 0;   // hle_blit_key
uint32_t g_h_push = 0;   // hle_push_pixels
uint32_t g_h_fill = 0;   // hle_fill_span
uint32_t g_h_mul = 0;    // hle_mul32
#define HLE_T0() const uint32_t __ht0 = esp_cpu_get_cycle_count()
#define HLE_ADD(var) var += (esp_cpu_get_cycle_count() - __ht0)
#else
uint32_t g_h_band = 0, g_h_blit = 0, g_h_push = 0, g_h_fill = 0, g_h_mul = 0;
#define HLE_T0() ((void)0)
#define HLE_ADD(var) ((void)0)
#endif

void unsp_device::unimplemented_opcode(uint16_t op)
{
	std::printf("unsp: unimplemented %04X at %06X\n", op, UNSP_LPC);
}

void unsp_device::unimplemented_opcode(uint16_t op, uint16_t x)
{
	std::printf("unsp: unimplemented %04X %04X at %06X\n", op, x, UNSP_LPC);
}

void unsp_device::unimplemented_opcode(uint16_t op, uint16_t x, uint16_t y)
{
	std::printf("unsp: unimplemented %04X %04X %04X at %06X\n", op, x, y, UNSP_LPC);
}

void unsp_device::update_nzsc(uint32_t value, uint16_t r0, uint16_t r1)
{
	m_core->m_r[REG_SR] &= ~(UNSP_N | UNSP_Z | UNSP_S | UNSP_C);
	if (BIT(value, 16) != BIT((r0 ^ r1), 15))
		m_core->m_r[REG_SR] |= UNSP_S;
	if (BIT(value, 15))
		m_core->m_r[REG_SR] |= UNSP_N;
	if ((uint16_t)value == 0)
		m_core->m_r[REG_SR] |= UNSP_Z;
	if (BIT(value, 16))
		m_core->m_r[REG_SR] |= UNSP_C;
}

void unsp_device::update_nz(uint32_t value)
{
	m_core->m_r[REG_SR] &= ~(UNSP_N | UNSP_Z);
	if (value & 0x8000)
		m_core->m_r[REG_SR] |= UNSP_N;
	if ((uint16_t)value == 0)
		m_core->m_r[REG_SR] |= UNSP_Z;
}

void unsp_device::push(uint32_t value, uint32_t *reg)
{
	write16(*reg, (uint16_t)value);
	*reg = (uint16_t)(*reg - 1);
}

uint16_t unsp_device::pop(uint32_t *reg)
{
	*reg = (uint16_t)(*reg + 1);
	return (uint16_t)read16(*reg);
}

void unsp_device::trigger_fiq()
{
	if (!m_core->m_enable_fiq || m_core->m_fiq)
		return;

	m_core->m_fiq = 1;

	push(m_core->m_r[REG_PC], &m_core->m_r[REG_SP]);
	push(m_core->m_r[REG_SR], &m_core->m_r[REG_SP]);
	m_core->m_r[REG_PC] = read16(m_vectorbase + 0x06);
	m_core->m_r[REG_SR] = 0;
}

extern uint32_t g_irq7_refused;

void unsp_device::trigger_irq(int line)
{
	if ((m_core->m_ine == 0 && m_core->m_irq == 1) || m_core->m_pri <= line || !m_core->m_enable_irq)
	{
		g_irq7_refused++;
		return;
	}

	m_core->m_irq = 1;

	push(m_core->m_r[REG_PC], &m_core->m_r[REG_SP]);
	push(m_core->m_r[REG_SR], &m_core->m_r[REG_SP]);
	if (m_core->m_ine)
	{
		push(get_fr(), &m_core->m_r[REG_SP]);
	}

	if (m_core->m_ine)
		m_core->m_pri = line;

	m_core->m_r[REG_PC] = read16(m_vectorbase + 0x08 + line);
	m_core->m_r[REG_SR] = 0;
}

void unsp_device::check_irqs()
{
	if (!m_core->m_sirq)
		return;

	int highest_irq = -1;
	for (int i = 0; i <= 8; i++)
	{
		if (BIT(m_core->m_sirq, i))
		{
			highest_irq = i;
			break;
		}
	}
	if (highest_irq < 0)
		return;

	if (highest_irq == UNSP_FIQ_LINE)
		trigger_fiq();
	else
		trigger_irq(highest_irq - 1);
}

void unsp_device::execute_set_input(int inputnum, int state)
{
	set_state_unsynced(inputnum, state);
}

void unsp_device::set_state_unsynced(int inputnum, int state)
{
	m_core->m_sirq &= ~(1 << inputnum);

	if (!state)
		return;

	switch (inputnum)
	{
		case UNSP_IRQ0_LINE:
		case UNSP_IRQ1_LINE:
		case UNSP_IRQ2_LINE:
		case UNSP_IRQ3_LINE:
		case UNSP_IRQ4_LINE:
		case UNSP_IRQ5_LINE:
		case UNSP_IRQ6_LINE:
		case UNSP_IRQ7_LINE:
		case UNSP_FIQ_LINE:
			m_core->m_sirq |= (1 << inputnum);
			break;
		case UNSP_BRK_LINE:
			break;
	}
}

uint16_t unsp_device::get_ds()
{
	return (m_core->m_r[REG_SR] >> 10) & 0x3f;
}

void unsp_device::set_ds(uint16_t ds)
{
	m_core->m_r[REG_SR] &= 0x03ff;
	m_core->m_r[REG_SR] |= (ds & 0x3f) << 10;
}

void unsp_device::set_fr(uint16_t fr)
{
	m_core->m_aq = BIT(fr, 14);
	const uint32_t old_bank = m_core->m_bnk;
	m_core->m_bnk = BIT(fr, 13);
	if (m_core->m_bnk != old_bank)
	{
		std::swap<uint32_t>(m_core->m_r[REG_R1], m_core->m_secbank[REG_SR1]);
		std::swap<uint32_t>(m_core->m_r[REG_R2], m_core->m_secbank[REG_SR2]);
		std::swap<uint32_t>(m_core->m_r[REG_R3], m_core->m_secbank[REG_SR3]);
		std::swap<uint32_t>(m_core->m_r[REG_R4], m_core->m_secbank[REG_SR4]);
	}
	m_core->m_fra = BIT(fr, 12);
	m_core->m_fir_move = BIT(fr, 11);
	m_core->m_sb = (fr >> 7) & 0xf;
	m_core->m_enable_fiq = BIT(fr, 6);
	m_core->m_enable_irq = BIT(fr, 5);
	m_core->m_ine = BIT(fr, 4);
	m_core->m_pri = fr & 0xf;
}

uint16_t unsp_device::get_fr()
{
	uint16_t fr = m_core->m_aq << 14;
	fr |= m_core->m_bnk << 13;
	fr |= m_core->m_fra << 12;
	fr |= m_core->m_fir_move << 11;
	fr |= m_core->m_sb << 7;
	fr |= m_core->m_enable_fiq << 6;
	fr |= m_core->m_enable_irq << 5;
	fr |= m_core->m_ine << 4;
	fr |= m_core->m_pri;
	return fr;
}

void unsp_device::execute_one(const uint16_t op)
{
	const uint16_t op0 = (op >> 12) & 15;
	const uint16_t opa = (op >> 9) & 7;
	const uint16_t op1 = (op >> 6) & 7;

	if (op0 == 0xf)
		return execute_fxxx_group(op);

	if (opa == 0x7 && op1 < 2)         // op0 == 0xf already returned above
		return execute_jumps(op);

	if (op0 == 0xe)
		return execute_exxx_group(op);

	execute_remaining(op);
}

UNSP_HOT bool unsp_device::spin_collapse(const uint32_t lpc)
{
	// Probe counters.  The collapse had never fired on the handset (colltot stayed
	// 0 while pc sat on 0x5251), and the three ways it can decline are
	// indistinguishable from the outside.  g_spin_calls counts entries,
	// g_spin_seen counts entries whose PC matched a loop, and the two reject
	// counters name the test that turned it down.
	SPIN_PROBE(g_spin_calls++);
	// The game spends most of its time in three countdown loops whose only
	// observable effect is that time passes:
	//
	//   0x5F35  r1 -= 01 / cmp r1, 00 / jne 5f35                8 cycles per pass
	//   0x5251  r4 = [bp+0] / cmp r4, 3e7 / ja 525a / nop /
	//           r4 = [bp+0] / r4 += 01 / [bp+0] = r4 / jmp 5251  33 cycles per pass
	//   0x5376  same shape, exit 537f                           33 cycles per pass
	//
	// We retire as many passes as the current slice allows, charge the exact
	// cycle count and leave at least one pass to the interpreter, so flags and
	// the exit path stay byte-for-byte what the slow path produces.  The slice
	// itself ends on the next timer event, so interrupt latency is unchanged.
	internal_unsp_state *const core = m_core;
	int per_pass;
	int64_t passes;
	uint32_t *counter = nullptr;

	if (lpc == 0x005F35)
	{
		SPIN_PROBE(g_spin_seen++);
		per_pass = 8;
		passes = core->m_r[1];                     // r1 counts down to zero
		counter = &core->m_r[1];
	}
	else if (lpc == 0x005251 || lpc == 0x005376)
	{
		SPIN_PROBE(g_spin_seen++);
		per_pass = 33;
		const uint32_t addr = (uint16_t)(core->m_r[REG_BP] + 0);
		const uint16_t value = read16(addr);        // [bp+0]
		SPIN_PROBE((g_spin_val = value, g_spin_bp = addr, g_spin_ic = core->m_icount));
		if (value > 0x03e7)
		{
			SPIN_PROBE(g_spin_big++);
			return false;                       // loop already finished
		}
		passes = 0x03e7 - value;                    // last pass stays interpreted
		counter = nullptr;
		if (passes > 0 && core->m_icount >= per_pass)
		{
			int64_t k = core->m_icount / per_pass;
			if (k > passes) k = passes;
			if (k > 0)
			{
				write16(addr, (uint16_t)(value + k));
				core->m_icount -= (int)(k * per_pass);
				SPIN_PROBE(g_spin_ok++);
				return true;
			}
		}
		SPIN_PROBE(g_spin_short++);
		return false;
	}
	else
	{
		return false;
	}

	if (passes <= 1 || core->m_icount < per_pass)
		return false;

	int64_t k = core->m_icount / per_pass;
	if (k > passes - 1) k = passes - 1;              // keep one pass for the interpreter
	if (k <= 0)
		return false;

	*counter = (uint16_t)(*counter - k);
	core->m_icount -= (int)(k * per_pass);
	return true;
}

// Single-instruction fast path.
//
// The interpreter's dispatch chain (execute_one -> execute_remaining ->
// do_basic_alu_ops with reference arguments) costs far more than the work
// itself, and the game's instruction mix is dominated by a handful of forms:
// "rX, imm6" loads/ALU, "rX, [bp+imm6]" loads/stores and the conditional
// jumps.  This decodes those forms directly, with the exact semantics and
// cycle costs of ref/unsp/unsp_other.cpp / unsp_jumps.cpp (flags included),
// and leaves everything else to execute_one().  Correctness is checked by the
// trace comparison against MAME (tools/emu_validate.py).
unsigned long long g_fast_calls = 0, g_fast_hits = 0;
int g_verify = 0;
int g_verify_fail = 0;

bool unsp_device::fast_step(uint16_t op)
{
#if !C3_FASTOPS
	(void)op;
	return false;
#else
	++g_fast_calls;
	internal_unsp_state *const core = m_core;
	const uint16_t op0 = (op >> 12) & 15;
	const uint16_t opa = (op >> 9) & 7;
	const uint16_t op1 = (op >> 6) & 7;

	// 0x0e (exxx) and 0x0f (fxxx) are separate decode groups in execute_one();
	// their instructions reuse the op1 field, so they must never fall into the
	// generic forms below.
	const bool generic = (op0 != 0x0e && op0 != 0x0f);

	// When opa == 7 the opcode is a jump, not an ALU form (execute_one checks
	// the jump group first), so those must stay with execute_jumps().
	if (generic && op1 == 0x01 && opa != 7)
	{
		// rX, imm6 -- always 2 cycles; opa == 7 means "no destination".
		const uint16_t imm = op & 0x3f;
		const uint16_t r0 = (uint16_t)core->m_r[opa];
		const bool upd = (opa != 7);
		uint32_t lres;

		switch (op0)
		{
		case 0x09: // load
			core->m_icount -= 2;
			if (upd)
				update_nz(imm);
			core->m_r[opa] = imm;
			return true;
		case 0x00: // add
			lres = r0 + imm;
			if (upd)
				update_nzsc(lres, r0, imm);
			core->m_r[opa] = (uint16_t)lres;
			core->m_icount -= 2;
			return true;
		case 0x02: // sub
			lres = r0 + (uint16_t)~imm + 1;
			if (upd)
				update_nzsc(lres, r0, (uint16_t)~imm);
			core->m_r[opa] = (uint16_t)lres;
			core->m_icount -= 2;
			return true;
		case 0x04: // cmp
			lres = r0 + (uint16_t)~imm + 1;
			if (upd)
				update_nzsc(lres, r0, (uint16_t)~imm);
			core->m_icount -= 2;
			return true;
		case 0x0b: // and
			lres = r0 & imm;
			if (upd)
				update_nz(lres);
			core->m_r[opa] = (uint16_t)lres;
			core->m_icount -= 2;
			return true;
		case 0x0a: // or
			lres = r0 | imm;
			if (upd)
				update_nz(lres);
			core->m_r[opa] = (uint16_t)lres;
			core->m_icount -= 2;
			return true;
		case 0x08: // xor
			lres = r0 ^ imm;
			if (upd)
				update_nz(lres);
			core->m_r[opa] = (uint16_t)lres;
			core->m_icount -= 2;
			return true;
		default:
			break;
		}
		return false;
	}

	if (generic && op1 == 0x00 && opa != 7 && (op0 == 0x09 || op0 == 0x0d))
	{
		// rX, [bp+imm6] / [bp+imm6], rX -- 6 cycles.
		const uint32_t addr = (uint16_t)(core->m_r[REG_BP] + (op & 0x3f));
		core->m_icount -= 6;
		if (op0 == 0x09)
		{
			const uint16_t value = read16(addr);
			if (opa != 7)
				update_nz(value);
			core->m_r[opa] = value;
		}
		else
		{
			write16(addr, (uint16_t)core->m_r[opa]);
		}
		return true;
	}

	if (generic && opa == 7 && op1 < 2)
	{
		// Conditional jumps and JMP; offset is +imm6 forwards, -imm6 backwards.
		const uint32_t opimm = op & 0x3f;
		// add_lpc() takes a signed offset: backwards jumps must stay negative.
		const int32_t offset = (op1 == 0) ? (int32_t)opimm : -(int32_t)opimm;
		const uint16_t sr = (uint16_t)core->m_r[REG_SR];
		bool taken;

		switch (op0)
		{
		case 0: // jb
			taken = (sr & UNSP_C) == 0;
			break;
		case 4: // jne
			taken = (sr & UNSP_Z) == 0;
			break;
		case 5: // je
			taken = (sr & UNSP_Z) != 0;
			break;
		case 9: // ja
			taken = (sr & (UNSP_Z | UNSP_C)) == UNSP_C;
			break;
		case 14: // jmp
			add_lpc(offset);
			core->m_icount -= 4;
			return true;
		default:
			return false;
		}

		if (taken)
			add_lpc(offset);
		core->m_icount -= taken ? 4 : 2;
		return true;
	}

	return false;
#endif // C3_FASTOPS
}

// Collapse the "wait for the next 64 Hz tick" busy-wait at 0x230321.
//
// The loop calls the PRNG block (already native) and then re-reads the tick
// counter at 0x0850 until it changes.  Inside a slice with no pending
// interrupt that counter cannot change, so the whole wait can be retired here
// at ~1/20 of the interpreter's cost, with identical final state.
// Set PC exactly like add_lpc() does: the low 16 bits live in REG_PC, the
// upper 6 address bits live in SR[0..5].  Writing only REG_PC leaves a stale
// LPC high half behind and the next fetch lands in the wrong flash page.
inline void unsp_device::set_pc_lpc(uint32_t address)
{
	m_core->m_r[REG_SR] = (uint16_t)((m_core->m_r[REG_SR] & 0xffc0) | ((address >> 16) & 0x3f));
	m_core->m_r[REG_PC] = (uint16_t)address;
}

// Hand-scheduled re-implementation of hot_block_prng() for the busy-wait
// collapse below.  The generated block walks the 32-bit LFSR at 0x0600..0x0607
// through ~68 unSP instructions per pass, each of which re-enters hot_read16 /
// hot_write16 with an address mask; on the C3 that is the dominant cost of the
// idle scene (~5k passes per frame).  This version keeps the eight state words
// and the shift register in locals and writes memory once per pass.
//
// Cycle accounting still happens in wait_loop_collapse: this function only
// performs state changes, exactly like hot_block_prng() does.
static inline void hot_prng_n(HotState *s, uint32_t n, uint16_t tick)
{
	if (n == 0)
		return;

	uint16_t *const m = s->ram;
	const uint16_t sp_in = (uint16_t)s->r[0];
	const uint16_t sp0 = (uint16_t)(sp_in - 4);
	uint16_t sb = (uint16_t)s->sb;
	uint16_t r1 = (uint16_t)s->r[1];
	uint16_t r2 = (uint16_t)s->r[2];
	uint16_t r3 = (uint16_t)s->r[3];
	uint16_t r4 = tick;

	// `push bp` stores BP at [SP]; the block then reuses [bp+0..2] as scratch.
	m[sp_in & 0x0fffu] = (uint16_t)s->r[5];

	uint16_t ylo = 0, yhi = 0, r4_rot = 0;

	// The eight LFSR words live in locals for the whole run.  The loop below
	// used to touch m[0x0600..0x0607] on every pass (11 reads + 8 writes); on
	// the C3 that is DRAM with no data cache, and at ~285k collapsed passes/s
	// it was the largest remaining idle-scene cost (R8 audit R8-C1).  Nothing
	// outside this function can observe the intermediate words: the collapse
	// defers interrupts and only calls check_irqs() after it returns, so
	// writing them back once at the end is equivalent to writing per pass.
	uint16_t w0 = m[0x0600], w1 = m[0x0601];
	const uint16_t w2 = m[0x0602], w3 = m[0x0603];
	uint16_t w4 = m[0x0604], w5 = m[0x0605];
	uint16_t w6 = m[0x0606], w7 = m[0x0607];

	for (uint32_t k = 0; k < n; k++)
	{
		const uint32_t a = ((uint32_t)w3 << 16) | w2;
		const uint32_t y = (a << 11) ^ a;
		ylo = (uint16_t)y;
		yhi = (uint16_t)(y >> 16);

		// Shift the 4-word state down and feed the newest word in at 0x0606.
		const uint16_t t0 = w0, t1 = w1;
		w0 = w4;
		w1 = w5;
		w4 = w6;
		w5 = w7;
		w6 = t0;
		w7 = t1;

		// r3 = yhi >> 7, r4 = ylo ror 7, r3 >>= 7, r4 ror= 7 (16-bit, via sb).
		uint32_t sh = ((uint32_t)yhi << 4) | sb;
		sh >>= 4;
		sb = (uint16_t)(sh & 0x0fu);
		r3 = (uint16_t)(sh >> 4);

		sh = ((((uint32_t)sb << 16) | ylo) << 4) | sb;
		sh >>= 4;
		sb = (uint16_t)(sh & 0x0fu);
		r4 = (uint16_t)(sh >> 4);

		sh = ((uint32_t)r3 << 4) | sb;
		sh >>= 4;
		sb = (uint16_t)(sh & 0x0fu);
		r3 = (uint16_t)(sh >> 4);

		sh = ((((uint32_t)sb << 16) | r4) << 4) | sb;
		sh >>= 4;
		sb = (uint16_t)(sh & 0x0fu);
		r4 = (uint16_t)(sh >> 4);

		r4 ^= ylo;
		r3 ^= yhi;
		r4_rot = r4;

		// r4 = [0x0601] >> 9 (through sb), then mixed into the two words.
		sh = ((uint32_t)w1 << 4) | sb;
		sh >>= 3;
		sb = (uint16_t)(sh & 0x0fu);
		r4 = (uint16_t)(sh >> 4);
		r4 ^= w0;
		r2 = w1;
		r1 = (uint16_t)(r4_rot ^ r4);
		r3 ^= r2;

		w0 = r1;
		w1 = r3;
	}

	m[0x0600] = w0;
	m[0x0601] = w1;
	m[0x0604] = w4;
	m[0x0605] = w5;
	m[0x0606] = w6;
	m[0x0607] = w7;

	// The scratch locals live at [bp+0..2]; only the last pass survives.
	m[(uint16_t)(sp0 + 1) & 0x0fffu] = ylo;
	m[(uint16_t)(sp0 + 2) & 0x0fffu] = yhi;
	m[(uint16_t)(sp0 + 3) & 0x0fffu] = r4_rot;

	s->sb = sb;
	s->r[1] = r1;
	s->r[2] = r2;
	s->r[3] = r3;
	s->r[4] = r4;
}

UNSP_HOT bool unsp_device::wait_loop_collapse(uint32_t lpc)
{
#if !C3_WAIT_COLLAPSE
	(void)lpc;
	return false;
#else
	if (!m_fastwait)
		return false;
	// The wait loop exists twice: once in flash at 0x230321 and once as a RAM
	// copy the game runs from 0x000321 (it relocates the block before entering
	// this scene, which is why the RAM range never shows up in the PC-side
	// sampler).  Same instruction stream, different base - the collapse only
	// needs the registers and the RAM data area, so both spellings collapse
	// identically.  A RAM hit is validated against the flash copy so a stale
	// or unrelated RAM block can never be mistaken for the loop.
	constexpr uint32_t kFlashLoop = 0x230321u;
	constexpr uint32_t kRamLoop = 0x000321u;
	if (lpc != kFlashLoop)
	{
		if (lpc != kRamLoop || read16(kRamLoop) != read16(kFlashLoop))
			return false;
	}

	internal_unsp_state *const core = m_core;
	const uint32_t budget = (uint32_t)(core->m_icount > 0 ? core->m_icount : 0);

	// 298 cycles per pass (measured per-instruction modal charges).
	constexpr uint32_t CYCLES_PER_PASS = 298;
	if (budget < CYCLES_PER_PASS)
		return false;

	// One bus read per collapse, not one per pass.
	//
	// The loop body rewrites `ds = 00` and re-reads the tick counter at 0x0850
	// on every pass, but within a slice that counter cannot move: only the 64 Hz
	// handler stores to it, and a pending interrupt ends the collapse below.  So
	// every pass sees the same value and a single read covers all of them.
	const uint16_t tick = m_ram ? m_ram[0x0850] : read16(0x0850);

	uint32_t passes = budget / CYCLES_PER_PASS;
	if (passes > 8192) passes = 8192;           // bounded work per call

	set_ds(0);

	HotState hs;
	for (int i = 0; i < 8; i++) hs.r[i] = core->m_r[i];
	hs.sb = (uint16_t)core->m_sb;
	hs.ram = m_ram;

	// The callee's `pop bp, pc` is what keeps the stack balanced: the block
	// pushes BP at [SP] and decrements, and the return pops it back.  The
	// pushed value is BP itself, so the pop restores a constant - hoist it.
	const uint32_t bp_saved = hs.r[REG_BP];

	// Neither of the two conditions below can change while we hold the CPU:
	// the counter only moves in the 64 Hz handler, and only tick() (which is
	// not called here) raises the pending-interrupt flag.  So they are tested
	// once, not once per pass.
	// The wait loop runs the block once and re-tests the tick: when the tick is
	// still zero it keeps looping, so `passes` retirements all happen here.
	const uint32_t done = (tick == 0 && !core->m_sirq) ? passes : 1u;
	hs.r[4] = tick;                     // r4 = ds:[0850]
	hot_prng_n(&hs, done, tick);
	hs.r[REG_BP] = bp_saved;

	for (int i = 0; i < 8; i++) core->m_r[i] = hs.r[i];
	core->m_sb = hs.sb;

	// The wrapper's last two instructions decide where the guest resumes:
	// `cmp r4, 00` sets the flags and `je` either loops or falls through.
	{
		const uint16_t r0 = tick;
		const uint32_t lres = (uint32_t)r0 + (uint16_t)~0u + 1u;
		update_nzsc(lres, r0, (uint16_t)~0u);
	}
	core->m_r[4] = tick;

	if (tick != 0)
	{
		set_pc_lpc(0x230329u);          // je not taken: leave the loop
		core->m_icount -= (int)(done * CYCLES_PER_PASS - 2);
	}
	else
	{
		set_pc_lpc(0x230321u);          // back to the top of the loop
		core->m_icount -= (int)(done * CYCLES_PER_PASS);
	}

	m_collapse_passes += done;
	// Keep mi_s comparable with the AOT path: the collapsed passes really do
	// execute 68 instructions of the PRNG block plus the six wrapper
	// instructions of the wait loop, they just never reach the dispatcher that
	// normally counts them.  74 = 68 (0x2002F4..0x200342) + 1 (0x230321) + 5
	// (0x230323..0x230327).
	m_insn_count += 74ull * done;
	m_collapse_calls++;
	return true;
#endif // C3_WAIT_COLLAPSE
}

// ---- AOT blocks (emu/aot_blocks.c, generated by tools/aot.py) ----------------
//
// A block keeps the register file in an AotCtx mirror and only writes it back
// when it returns.  Everything a block cannot do itself is routed through the
// callbacks below; each of them re-enters the interpreter exactly where the
// plain run loop would have been, so the observable instruction stream is
// identical whether a given PC came from a block or from the interpreter.

// Non-static on purpose: aot.h declares these and calls them directly from
// aot_rd/aot_wr, so the LTO pass can inline the trampoline into every block
// body.  Routed through the old function-pointer fields it stayed an indirect
// call, and the whole out-of-window path cost three levels of indirection.
uint16_t aot_io_rd(void *dev, uint32_t address)
{
	return ((unsp_device *)dev)->m_bus->read16(address);
}

void aot_io_wr(void *dev, uint32_t address, uint16_t value)
{
	((unsp_device *)dev)->m_bus->write16(address, value);
}

// A block walked past the end of its translated instructions: publish the
// register file, let the interpreter retire exactly one instruction, and hand
// the result back to the block (which returns to the dispatcher immediately).
// Diagnostic histograms: which PC falls back, and which PC has no block at all.
// Both are sizeable enough during tuning that a plain table beats a map; a slot
// collision just restarts the slot's count, which is fine for finding hot spots.
#ifndef ESP_PLATFORM
namespace
{
struct AotPcHist
{
	uint32_t key[4096];
	uint32_t cnt[4096];
	AotPcHist() { std::memset(key, 0, sizeof(key)); std::memset(cnt, 0, sizeof(cnt)); }
	void add(uint32_t pc)
	{
		const unsigned slot = pc & 4095u;
		if (key[slot] != pc + 1u) { key[slot] = pc + 1u; cnt[slot] = 0; }
		cnt[slot]++;
	}
};

AotPcHist g_aot_fb_hist;
AotPcHist g_aot_miss_hist;
} // namespace

void aot_dump_pc_hist(const char *path)
{
	FILE *f = std::fopen(path, "w");
	if (!f)
		return;
	std::fprintf(f, "# pc count kind\n");
	for (int pass = 0; pass < 2; pass++)
	{
		const AotPcHist &h = pass ? g_aot_miss_hist : g_aot_fb_hist;
		for (int rank = 0; rank < 40; rank++)
		{
			int best = -1;
			for (int i = 0; i < 4096; i++)
				if (h.key[i] && (best < 0 || h.cnt[i] > h.cnt[best]))
					best = i;
			if (best < 0 || !h.cnt[best])
				break;
			std::fprintf(f, "%06X %u %s\n", h.key[best] - 1u, h.cnt[best], pass ? "miss" : "fallback");
			const_cast<AotPcHist &>(h).cnt[best] = 0;
		}
	}
	std::fclose(f);
}
#endif // !ESP_PLATFORM

// One unsp_device is instantiated per machine, so a single resident
// context is enough (see aot_step).
AotCtx g_aot_ctx;
bool g_aot_ctx_ready = false;

#ifdef ESP_PLATFORM
// Distinct-block counter (512-byte bitmap).  The full frequency census was a
// one-off measurement kept in dbgtest/iram_hot.txt; what stays on the device is
// just this cheap "how many distinct blocks have we run" gauge.
uint32_t g_aot_uniq_bits[128];
uint32_t g_aot_uniq = 0;

// Missed block entries, recorded on the device so the block list can be extended
// with the PCs the handset actually runs.  Small open-addressed table: a miss is
// already the slow path, and 1024 entries is enough to see the shape of a scene.
//
// 8 KiB of DRAM for a one-off census.  Compiled out with the rest of the miss
// census: the device build has C3_MISS_CENSUS=0, the `M` console command then
// answers with an empty table instead of paying for the storage forever.
#if C3_MISS_CENSUS
static uint32_t g_miss_pc[1024];
static uint32_t g_miss_cnt[1024];
static uint32_t g_miss_used = 0;
#else
static inline void aot_note_miss(uint32_t) {}
#endif

// Block-entry frequency census.  The device runs scenes the PC sampler never
// visits, so IRAM placement has to be decided from hardware data: this table
// counts entries per block and is dumped once at frame 2400.
#if defined(C3_BLOCK_CENSUS) && C3_BLOCK_CENSUS
static uint32_t g_blk_pc[1024];
static uint32_t g_blk_cnt[1024];
static uint32_t g_blk_used = 0;

static void aot_note_block(uint32_t lpc)
{
	unsigned h = (lpc * 2654435761u) >> 22;
	for (unsigned probe = 0; probe < 8; probe++)
	{
		const unsigned i = (h + probe) & 1023u;
		if (g_blk_pc[i] == lpc) { g_blk_cnt[i]++; return; }
		if (g_blk_pc[i] == 0) { g_blk_pc[i] = lpc ? lpc : 1u; g_blk_cnt[i] = 1; g_blk_used++; return; }
	}
}

void aot_dump_blocks(void)
{
	// The table is a hash: a live counter can sit in any of the 1024 slots, so
	// the scan has to cover 1024 and not g_blk_used.  Bounding it by the number
	// of occupied slots silently skipped every entry whose hash landed past
	// that count, which is why a dump could print a full BLKHEAD and no rows.
	const unsigned n = 1024u;
	// ESP_LOG, not printf: this is a diagnostic that has to survive a cold UART
	// buffer, and the log path flushes each line.
	ESP_LOGI("aotc", "BLKHEAD %u", unsigned(g_blk_used));
	for (unsigned k = 0; k < 200u; k++)
	{
		unsigned best = 0;
		for (unsigned i = 1; i < n; i++)
			if (g_blk_cnt[i] > g_blk_cnt[best])
				best = i;
		if (g_blk_cnt[best] == 0)
			break;
		ESP_LOGI("aotc", "BLK %06X %u", unsigned(g_blk_pc[best]), unsigned(g_blk_cnt[best]));
		g_blk_cnt[best] = 0;
	}
	ESP_LOGI("aotc", "BLKEND");
}
#else
static void aot_note_block(uint32_t) {}
void aot_dump_blocks(void) {}
#endif

#if C3_MISS_CENSUS
static void aot_note_miss(uint32_t lpc)
{
	unsigned h = (lpc * 2654435761u) >> 22;
	for (unsigned probe = 0; probe < 8; probe++)
	{
		const unsigned i = (h + probe) & 1023u;
		if (g_miss_pc[i] == lpc) { g_miss_cnt[i]++; return; }
		if (g_miss_pc[i] == 0) { g_miss_pc[i] = lpc ? lpc : 1u; g_miss_cnt[i] = 1; g_miss_used++; return; }
	}
}

void aot_dump_miss(void)
{
	const unsigned n = g_miss_used < 1024u ? g_miss_used : 1024u;
	std::printf("MISSHEAD %u\n", n);
	// Repeated selection of the largest remaining count: no qsort, no stack.
	for (unsigned k = 0; k < 200u && k < n; k++)
	{
		unsigned best = 0, bi = 0xffffffffu;
		for (unsigned i = 0; i < 1024u; i++)
			if (g_miss_cnt[i] > best) { best = g_miss_cnt[i]; bi = i; }
		if (bi == 0xffffffffu) break;
		std::printf("MISS %06X %u\n", (unsigned)g_miss_pc[bi], (unsigned)best);
		g_miss_cnt[bi] = 0;
	}
	std::printf("CENSUS end %u\n", (unsigned)g_aot_uniq);
}
#else
void aot_dump_miss(void)
{
	std::printf("MISSHEAD 0\nCENSUS end %u\n", (unsigned)g_aot_uniq);
}
#endif

static void aot_mark_uniq(uint32_t lpc)
{
	const uint32_t h = (lpc * 2654435761u) >> 20;
	uint32_t &w = g_aot_uniq_bits[h >> 5];
	const uint32_t m = 1u << (h & 31u);
	if (!(w & m)) { w |= m; g_aot_uniq++; }
}
#else
static void aot_mark_uniq(uint32_t) {}
#endif

static void aot_fallback_one(void *dev, AotCtx *c, uint32_t lpc)
{
#ifndef ESP_PLATFORM
	g_aot_fb_hist.add(lpc);
#endif
	unsp_device *const d = (unsp_device *)dev;
	unsp_device::internal_unsp_state *const core = d->m_core;
	d->m_aot_fallback++;
	d->m_insn_count++;
	// c->r aliases core->m_r, so only the scalars need pushing back.
	core->m_sb = c->sb;
	core->m_icount = c->icount;

	// A block does not maintain r7 while it runs, so park PC on the instruction
	// that is about to execute before anything reads it.
	d->set_pc_lpc(lpc);
	const uint16_t op = d->read16(lpc);
	d->add_lpc(1);
	if (d->m_cycle_hook)
	{
		// Same probe trick as the run loop: park the countdown out of reach so
		// the instruction cannot observe a slice boundary, then restore it.
		const int saved = core->m_icount;
		constexpr int probe = 1 << 28;
		core->m_icount = probe;
		d->execute_one(op);
		const int charged = probe - core->m_icount;
		core->m_icount = saved - charged;
		d->m_cycle_hook(d->m_cycle_hook_opaque, lpc, charged);
	}
	else
	{
		d->execute_one(op);
	}
	if (op != 0x9a98 && core->m_sirq)
		d->check_irqs();

	// Pull the post-instruction scalar state back so the caller can publish it
	// as if the block had produced it itself.  c->r already aliases core->m_r.
	c->sb = core->m_sb;
	c->icount = core->m_icount;
}

// Instruction accounting by region, so "where do the 58k guest instructions of a
// redraw frame go" is measured instead of guessed.  Cheap: four compares per block.
// Declared here, above aot_step(), because that is where they are accumulated.
uint32_t g_ins_irom = 0;      // 0x000000-0x00FFFF (internal mask ROM)
uint32_t g_ins_flash = 0;     // 0x200000-0x3FFFFF (SPI flash window)
uint32_t g_ins_push = 0;      // subset of irom: the LCD byte-push routine 0x005A00-0x005B60
uint32_t g_ins_band = 0;      // narrowest slice: the render_band record body
uint32_t g_ins_draw = 0;      // subset of flash: the drawing code 0x207A00-0x207D00
uint32_t g_ins_ram = 0;       // 0x000000-0x000FFF

UNSP_HOT bool unsp_device::aot_step(const uint32_t lpc)
{
	// AotCtx lives in .bss, not on the stack: refilling the invariant fields on
	// every block entry cost more than the block itself once blocks got short.
	AotCtx &ac = g_aot_ctx;
	if (!g_aot_ctx_ready)
	{
		ac.ram = m_ram;
		ac.ram_limit = m_ram_limit;
		ac.dev = this;
		ac.bus_rd = aot_io_rd;
		ac.bus_wr = aot_io_wr;
		ac.fallback = aot_fallback_one;
		ac.sirq = &m_core->m_sirq;
		ac.r = m_core->m_r;
		g_aot_ctx_ready = true;
	}
#ifndef AOT_NO_HOOKS
	ac.insn_hook = m_insn_hook;
	ac.insn_opaque = m_insn_hook_opaque;
	ac.cycle_hook = m_cycle_hook;
	ac.cycle_opaque = m_cycle_hook_opaque;
	ac.blk_hist = m_block_hist;
	m_aot_cur = &ac;
#endif

	// Without hooks the block never calls out, so the only per-block work left is
	// the register-file pointers, the shift buffer and the cycle budget.
	uint8_t cnt = 0;
#if C3_SPLIT && defined(ESP_PLATFORM)
	SPLIT_T0();
	const uint32_t t_lk = esp_cpu_get_cycle_count();
#endif
	const AotFn fn = aot_lookup(lpc, &cnt, nullptr);
#if C3_SPLIT && defined(ESP_PLATFORM)
	g_cyc_lk += (esp_cpu_get_cycle_count() - t_lk);
#endif
	if (!fn)
	{
		m_aot_miss++;
#ifdef ESP_PLATFORM
		aot_note_miss(lpc);
#else
		g_aot_miss_hist.add(lpc);
#endif
		return false;
	}
	m_aot_insns += cnt;
	m_insn_count += cnt;
	ac.sb = m_core->m_sb;
	ac.icount = m_core->m_icount;
#if C3_SPLIT && defined(ESP_PLATFORM)
	const uint32_t t_fn = esp_cpu_get_cycle_count();
#endif
	fn(&ac);
#if C3_SPLIT && defined(ESP_PLATFORM)
	{
		const uint32_t d = esp_cpu_get_cycle_count() - t_fn;
		g_cyc_fn += d;
		// Same cycles, attributed to the guest region that produced them. This is
		// the number that decides whether HLE-ing a region is worth the work:
		// instruction counts lie, because a pixel-push instruction and an
		// arithmetic instruction cost very different amounts of host time.
		if (lpc >= 0x207A00u && lpc < 0x207D00u)
			g_cyc_draw += d;
		else if (lpc >= 0x005A00u && lpc < 0x005B60u)
			g_cyc_push += d;
	}
#endif
	m_core->m_sb = ac.sb;
	m_core->m_icount = ac.icount;

#ifndef AOT_NO_HOOKS
	aot_mark_uniq(lpc);
#endif
	m_aot_blocks++;
#if C3_SPLIT && defined(ESP_PLATFORM)
	// The fast path used to return without touching these, so caot/daot/cpa/cfn
	// all printed as 0 whenever the game ran out of AOT blocks (which is always).
	SPLIT_ADD(g_cyc_aot);
	g_disp_aot++;
#endif
#ifdef ESP_PLATFORM
	aot_note_block(lpc);            // device-side frequency census only
#if C3_REGION_STAT
	// Region accounting: which part of the guest map is eating the frame.
	if (lpc < 0x001000u)            g_ins_ram   += cnt;
	else if (lpc < 0x010000u)       g_ins_irom  += cnt;
	else                            g_ins_flash += cnt;
	if (lpc >= 0x005A00u && lpc < 0x005B60u) g_ins_push += cnt;
	if (lpc >= 0x207A00u && lpc < 0x207D00u) g_ins_draw += cnt;
	// Narrower slice: the per-record body the render_band HLE is meant to
	// replace.  If rband stays high while the HLE reports records consumed, the
	// guest is still walking that range by another route.
	if (lpc >= 0x207A3Eu && lpc < 0x207D10u) g_ins_band += cnt;
#endif
#endif
	return true;
}

#ifdef ESP_PLATFORM
// ---------------------------------------------------------------------------
// Blit with a transparent colour key (0x207CA3..0x207CC4)
//
// The game draws every sprite through this word loop: read a 16 bit pixel from
// a source pointer, skip it when it equals 0x07E0 (pure green in RGB565, the
// key colour), otherwise store it at `index + bias + 0x07BA`.  Sixteen unSP
// instructions per pixel, 9216 pixels per 96x96 sprite: ~150k instructions per
// blit, which is the single biggest cost on a moving screen.
//
// The loop only touches registers and three stack slots, so the whole pass is
// reproduced here in C.  Frame layout (all bp relative, see the disassembly):
//   [bp+2]  end index      [bp+4]  destination bias   [bp+6]  current index
//   [bp+7]  source low     [bp+8]  source high (ds)   [bp+9]  scratch
// Entry expects the loop head (0x207CA3); on success we continue at 0x207CC5.
// ---------------------------------------------------------------------------
#define AOT_HLE_BLIT_ENTRY 0x207CA3u
#define AOT_HLE_BLIT_EXIT  0x207CC5u
#define AOT_HLE_BLIT_KEY   0x07E0u
#define AOT_HLE_BLIT_BIAS  0x07BAu
// Per-pixel charge measured from the interpreter's own accounting.  Every pixel
// this hook paints costs that many virtual cycles whether or not the C loop
// above spent a fraction of them, so a wide repaint is charged by area alone and
// that charge - not the SPI transfer - is what makes a full-screen move crawl.
//
// Left overridable from the build so the coefficient can be calibrated against
// the real handset without a second code path: -DC3_BLIT_CYC=58.
#ifndef C3_BLIT_CYC
#define C3_BLIT_CYC 139
#endif
#define AOT_HLE_BLIT_CYCLES_PER_PIXEL ((uint32_t)C3_BLIT_CYC)

// Runtime override: the console command "B <n>" trims the coefficient while the
// handset is running, so one image can be compared against two coefficients
// without a reflash.  Clamped where it is written, never below 8 or the pixel
// loop stops making progress against the slice budget.
uint32_t g_blit_cyc_per_px = AOT_HLE_BLIT_CYCLES_PER_PIXEL;

// Diagnostics: pixels painted by the hook.  g_blit_px_win is the running window
// the application clears once per log period; the total survives until reset.
// Together they turn "the moving screen is slow" into a number that can be
// multiplied by the coefficient and compared against the frame budget.
uint32_t g_blit_px_win = 0;
uint32_t g_blit_px_total = 0;

bool unsp_device::hle_blit_key()
{
	internal_unsp_state *const core = m_core;
	const uint32_t bp = (uint32_t)core->m_r[REG_BP];

	const uint16_t end = read16((uint16_t)(bp + 2));
	uint16_t i = read16((uint16_t)(bp + 6));
	if ((uint16_t)(i - end) < 0x8000u)
		return false;                       // first compare already exits

	const uint16_t bias = read16((uint16_t)(bp + 4));
	uint32_t src = ((uint32_t)read16((uint16_t)(bp + 8)) << 16) |
	               read16((uint16_t)(bp + 7));

	// Budget: never charge more than the current slice still has left.  A
	// 96x96 sprite is 9216 pixels * 139 cycles = 1.28M cycles, i.e. 27 slices,
	// so finishing the whole loop in one gulp would blow straight through the
	// slice boundary: `consumed` would jump to millions, soc.tick() would stall
	// for tens of milliseconds and a whole frame would be swallowed.  Stopping
	// on budget keeps the per-slice cost bounded; the next slices resume the
	// same loop exactly where this one left off, which is what the interpreter
	// does one instruction at a time.
	const int32_t budget = core->m_icount > 0 ? core->m_icount : 0;
	// Snapshot the per-pixel charge once.  It is a mutable global (console "B"
	// trims it live), and reading it per pixel put a load + multiply in the
	// innermost loop of every sprite (R7 audit R7-06).  Within one hook entry it
	// cannot change, so a local is bit-for-bit equivalent.
	const uint32_t cyc_per_px = g_blit_cyc_per_px;
	if (budget < (int32_t)cyc_per_px)
		return false;                   // no room for even one pixel: interpret
	// Sprite source window.  The clip check above already decided there is work
	// to do; the source itself is read one 16 bit word per pixel out of the flash
	// window, and going through read16() costs an indirect bus call, four address
	// range compares and the SparseRom page lookup for *every* pixel.  A 2 KiB
	// page window covers 2048 consecutive sprite words, which is more than any
	// 96 pixel row, so the whole sprite is read through one host pointer with a
	// single range test per pixel.  fetch_window() returns null for pages that
	// are absent or overlaid, and the loop then falls back to read16().
	uint32_t win_base = 0xffffffffu;
	uint32_t win_mask = 0u;
	const uint16_t *win = nullptr;

	uint32_t cycles = 0;
	uint32_t pixels = 0;
	while ((uint16_t)(i - end) >= 0x8000u && (int32_t)cycles < budget)
	{
		uint16_t val;
		if (win != nullptr && (src - win_base) <= win_mask)
			val = win[src & win_mask];
		else
		{
			win = m_bus->fetch_window(src, &win_base, &win_mask);
			val = (win != nullptr) ? win[src & win_mask] : read16(src);
		}
		if (val != AOT_HLE_BLIT_KEY)
		{
			const uint32_t dst = (uint32_t)i + (uint32_t)bias + AOT_HLE_BLIT_BIAS;
			// The destination is the guest's 96 pixel row buffer, i.e. always
			// plain RAM.  Going through write16() costs a virtual call per
			// pixel (measured: the draw region runs at 260 host cycles per
			// guest instruction); the flat window is a direct array store.
			if (dst <= m_ram_limit)
				m_ram[dst] = val;
			else
				write16(dst, val);
		}
		src = (src + 1u) & 0xFFFFFFu;
		i++;
		cycles += cyc_per_px;
		if (++pixels > 0x10000u)
			return false;
	}

	g_blit_px_win += pixels;
	g_blit_px_total += pixels;
	write16((uint16_t)(bp + 6), i);
	write16((uint16_t)(bp + 7), (uint16_t)src);
	write16((uint16_t)(bp + 8), (uint16_t)(src >> 16));
	core->m_icount -= (int32_t)cycles;
	// Out of budget with pixels left: resume the loop head next slice.
	const bool finished = (uint16_t)(i - end) < 0x8000u;
	set_pc_lpc(finished ? AOT_HLE_BLIT_EXIT : AOT_HLE_BLIT_ENTRY);
	return true;
}

// ---------------------------------------------------------------------------
// LCD pixel push (0x005B41, hooked at the loop head 0x005B4A)
//
// Every LCD update goes through this routine: one 16 bit word per pass, high
// byte then low byte into SPI2 TXDATA (0x3092), i.e. big endian RGB565.  The
// loop body is fourteen guest instructions per pixel, 128 of them per row and
// 16384 for a full screen - a busy frame spends 34% of all its guest
// instructions here, more than in any other single block.
//
// The hook sits on the loop head instead of on the function entry, for three
// reasons:
//   * 0x5B41..0x5B45 is the prologue: it reads the two stack arguments and
//     pushes r1..r4.  Letting the guest run it keeps the frame byte exact, so
//     the epilogue (0x5B5D setb, 0x5B5F pop, 0x5B60 retf) still balances and
//     the 28 + 20 cycles around the loop are charged by the blocks themselves.
//   * The loop is bottom tested: r2 == 0 at the entry means 65536 pixels, not
//     zero.  At the loop head that state can simply be handed back to the
//     interpreter, while an entry hook would have to reproduce the wrap.
//   * Slicing.  If the budget runs out mid-push, an entry hook would have to
//     resume at 0x5B41 - re-reading the arguments and pushing the first pixels
//     a second time.  Resuming at the loop head is idempotent: r1/r2 already
//     carry the progress, exactly as the recompiled blocks leave them.
//
// Per pixel the pass is charged 63 cycles: 6+3+3+3+7+7+3 (0x5B4A..0x5B52)
// +7+2 (0x5B53 wait, 0x5B55 test) +2 (0x5B56 "jne 5b53" NOT taken: the FIFO
// never reports busy, spi2_misc_r() returns 0x0003 so bit 0x10 is clear and
// the test sets Z) +7+7 (the two 0x3094 reads) +2 (0x5B5B "r2 -= 1")
// +4 (0x5B5C "jne 5b4a" taken).  The last pixel of a run falls through the
// back edge and costs 2 less, which is noise over a whole row.
//
// 65 would be the figure if the FIFO wait were taken on every pixel; it is
// not, so charging 65 would make the guest clock run ~3% fast per pixel and
// every animation would play 3% slow.
//
// The FIFO wait at 0x5B53 and the two dummy reads of 0x3094 are charge-only
// here: spi2_misc_r() returns 0x0003 (bit 0x10 clear) so the wait falls through
// on its first test, and spi2_rxdata_r() has no side effect at all.
// ---------------------------------------------------------------------------
#define AOT_HLE_PUSH_ENTRY 0x005B4Au
#define AOT_HLE_PUSH_EXIT  0x005B5Du
#define AOT_HLE_PUSH_CYCLES 63u

// Same role as m_hle_blit: flip it to false (debugger, or a one line patch) to
// hand the loop back to the recompiled blocks for an A/B comparison.
bool g_hle_push_px = true;

// Pixels actually pushed by the hook, so the STAT line can prove the path is
// live (and how much work it is absorbing) instead of silently falling back.
uint32_t g_hle_push_px_win = 0;
uint32_t g_hle_push_px_total = 0;

// Hooked at the loop head with r1 = source pointer, r2 = pixels left.  Returns
// false when the guest has to run the pass itself: the arguments are not
// trustworthy, or the slice budget cannot afford a single pixel.
static bool hle_push_pixels(unsp_device *const d)
{
	unsp_device::internal_unsp_state *const core = d->m_core;

	// r2 is a 16 bit register, so the loop cannot run past 65536 pixels even
	// when it wraps; the bound is here to keep a corrupted state from spinning.
	const uint32_t count = (uint16_t)core->m_r[unsp_device::REG_R2];
	if (count == 0u || count > 0x10000u)
		return false;

	// Interrupts are not tested here, same policy as hle_blit_key: the pass is
	// bounded by the slice budget, and the dispatcher runs check_irqs() as soon
	// as this hook gives the loop back, so a pending interrupt is taken within
	// one slice instead of one pixel.  Bailing out on m_sirq would be tighter,
	// but a line that stays pending while the guest has interrupts masked (a
	// critical section around the whole push is common) would then disable the
	// hook for the entire frame, which is the worse failure mode.
	const int32_t budget = core->m_icount > 0 ? core->m_icount : 0;
	if (budget < (int32_t)AOT_HLE_PUSH_CYCLES)
		return false;                   // not even one pixel fits: interpret

	uint16_t src = (uint16_t)core->m_r[unsp_device::REG_R1];
	uint32_t remaining = count;
	uint32_t cycles = 0;

	// Fast path: hand the whole span to the LCD model in one call.  Two SPI2
	// TXDATA writes per pixel means an indirect call, an I/O address decode, the
	// FIFO bookkeeping and the command switch *per byte* - measured on the round-6
	// build as the largest remaining per-pixel cost of a full-screen move.  The
	// source has to sit inside the flat RAM window (the 96 word scan-line buffer
	// always does) and the span must not run past its end, so the pointer the
	// panel model receives can never read outside guest RAM.  Outside a RAMWR
	// window the callback returns false and the byte path below still runs.
	if (d->m_lcd_push != nullptr && d->m_ram != nullptr)
	{
		const uint32_t affordable = (uint32_t)budget / AOT_HLE_PUSH_CYCLES;
		uint32_t n = remaining < affordable ? remaining : affordable;
		const uint32_t room = 0x1000u - (uint32_t)src;      // words left in the window
		if (n > room)
			n = room;
		if (n != 0u && (uint32_t)src + n - 1u <= d->m_ram_limit &&
		    d->m_lcd_push(d->m_lcd_push_opaque, d->m_ram + src, n))
		{
			src = (uint16_t)(src + n);
			remaining -= n;
			cycles += n * AOT_HLE_PUSH_CYCLES;
		}
	}

	while (remaining != 0u && cycles + AOT_HLE_PUSH_CYCLES <= (uint32_t)budget)
	{
		// 0x5B4A is "r3 = [r1++]" with the ds: bit clear (opn bit 2 == 0), so
		// the address is the bare 16 bit register: no data segment base and no
		// carry into SR when the pointer wraps.  The source therefore lives
		// below 0x10000, inside the flat RAM window when it is mapped there.
		const uint16_t pixel = d->read16(src);
		// [3092] = r3 (pixel >> 8) and then [3092] = r4 (pixel).
		// spi2_txdata_w() only uses data & 0xff, so the two writes are the two
		// bytes of the word, most significant first.
		d->write16(0x3092u, (uint16_t)(pixel >> 8));
		d->write16(0x3092u, pixel);
		src++;
		remaining--;
		cycles += AOT_HLE_PUSH_CYCLES;
	}

	core->m_r[unsp_device::REG_R1] = src;
	core->m_r[unsp_device::REG_R2] = (uint16_t)remaining;
	core->m_icount -= (int32_t)cycles;
	g_hle_push_px_win += (count - remaining);
	g_hle_push_px_total += (count - remaining);

	if (remaining != 0u)
	{
		// Out of budget with pixels left: the next dispatch re-enters here
		// with r1/r2 already pointing at the rest of the buffer.
		d->set_pc_lpc(AOT_HLE_PUSH_ENTRY);
		return true;
	}

	// Flags of the final "r2 -= 1" (0x5B5B, with r2 == 1): the 16 bit add of
	// 1 + ~1 + 1 comes out as 0x10000, so C is set, N and S are clear and Z is
	// set.  Z is rewritten by the setb at 0x5B5D anyway, but N/C/S reach the
	// caller, so they have to match the interpreted pass exactly.
	d->update_nzsc(0x10000u, (uint16_t)1u, (uint16_t)~1u);
	// Continue at 0x5B5D, not at the retf: setb releases the LCD chip select
	// and pop restores r1..r4 plus SP.  The push at 0x5B45 is still sitting on
	// the stack, so jumping to 0x5B60 would leave both of them wrong.
	d->set_pc_lpc(AOT_HLE_PUSH_EXIT);
	return true;
}

// ---------------------------------------------------------------------------
// LCD constant fill: 0x005B1D..0x005B2E
//
// The block census for a redraw frame shows 0x005B1E executing 9,215 times and
// 0x005B29 9,216 -- once per pixel of the 96x96 window.  That is not the keyed
// blit the other hook covers: this loop never touches memory.  It is
//
//   0x005B1D  r4 = r2                  (count)
//   0x005B1E  r2 = r1 ror4             (4-bit shift register: result r1 >> 4)
//   0x005B1F  r2 = r2 ror4             (result r1 >> 8)
//   0x005B20  [3092] = r2              (high byte first: spi2_txdata_w uses data & 0xff)
//   0x005B22  [3092] = r1              (low byte)
//   0x005B24  nop
//   0x005B25  r2 = [3095]              (SPI status; bit 0x10 is never set here)
//   0x005B27  test r2 & 0x0010
//   0x005B28  jne 0x005B25             (falls through on the first test)
//   0x005B29  r2 = [3094]              (RX data, always 0: no side effect)
//   0x005B2B  r2 = [3094]
//   0x005B2D  r4 -= 1
//   0x005B2E  jne 0x005B1E
//
// r1 is never written inside the loop and the ror4 pair lands on r1 >> 8 for
// any incoming sb, so every iteration pushes the *same* two bytes: this is a
// constant colour fill (render_band's solid-fill path).  54 guest cycles per
// pixel of pure interpreter overhead disappears; the two write16 calls, which
// are the actual work, stay.
// ---------------------------------------------------------------------------
#define AOT_HLE_FILL_ENTRY  0x005B1Eu
#define AOT_HLE_FILL_REST   0x005B1Eu
#define AOT_HLE_FILL_EXIT   0x005B2Fu
#define AOT_HLE_FILL_CYCLES 54u

bool g_hle_fill_px = true;

// Stall post-mortem: the game leaves the flash main loop at ~18 s and never
// comes back (see docs/DELIVERY_2026-11-22.md 鎼?.5).  Keeping the last few
// distinct internal-ROM block entries is enough to see which call walked into
// the waiter.  Only internal-ROM entries are recorded, so the hot flash loop
// pays a single compare-and-branch per block.
uint32_t g_pc_ring[48];
uint32_t g_pc_ring_ri = 0;
uint32_t g_pc_ring_prev = 0xffffffffu;

// IRQ7 handler hits.  0x486B is the vector target (read16(0x401F)); the body
// runs 0x486B..0x4897 and ends in `9A98 reti`.  If this stays 0 the CPU never
// took a single IRQ7, which is exactly what a starved 64 Hz source looks like.
uint32_t g_isr7_hits = 0;
uint32_t g_isr7_insn = 0;           // m_insn_count at the most recent entry
uint32_t g_irq7_refused = 0;        // trigger_irq() bailed out
// Per-PC census of the IRQ7 handler body (0x486B..0x48AF), so the exact exit
// point of the handler is visible instead of guessed from an instruction count.
uint32_t g_isr_hist[0x45];
const uint32_t kIsrHistBase = 0x00486Bu;

// Phase counters for the 64 Hz frame chain.  The host harness runs the exact
// same ROM and retires 0x230321 about 300k times a second while leaving through
// 0x230329 once per tick; if the device leaves zero times, the tick never
// arrives even though the ISR body runs.
uint32_t g_wait_top = 0;        // 0x230321 / 0x000321
uint32_t g_wait_exit = 0;       // 0x230329 / 0x000329
uint32_t g_frame_body = 0;      // 0x2300E8 / 0x0000E8
uint32_t g_body_alt = 0;        // 0x230139 / 0x000139 (loop back edge)

void aot_dump_pc_ring(void)
{
	std::printf("PCRINGHEAD\n");
	const uint32_t n = g_pc_ring_ri < 48u ? g_pc_ring_ri : 48u;
	for (uint32_t i = 0; i < n; i++)
	{
		const uint32_t idx = (g_pc_ring_ri + 48u - n + i) % 48u;
		std::printf("PC %06X\n", unsigned(g_pc_ring[idx]));
	}
	std::printf("PCRINGEND\n");
}

uint32_t g_hle_fill_px_win = 0;
uint32_t g_hle_fill_px_total = 0;
// Pixels that took the straight-to-panel path instead of two SPI2 register
// writes each.  If this stays at zero the LCD was never inside a RAMWR window
// when the fill loop ran, and the fast path is dead code for this game.
uint32_t g_hle_fill_fast = 0;

static bool hle_fill_span(unsp_device *const d)
{
	unsp_device::internal_unsp_state *const core = d->m_core;

	// Entered at the loop body, so r4 is the copies left.  The first pass of a
	// span runs inside the 0x005B1D block (which sets r4 = r2 and falls through);
	// every later pass re-enters here from the jne at 0x005B2E.
	const uint32_t count = (uint16_t)core->m_r[unsp_device::REG_R4];
	if (count == 0u || count > 0x10000u)
		return false;

	const int32_t budget = core->m_icount > 0 ? core->m_icount : 0;
	if (budget < (int32_t)AOT_HLE_FILL_CYCLES)
		return false;

	const uint16_t value = (uint16_t)core->m_r[unsp_device::REG_R1];
	const uint16_t high = (uint16_t)(value >> 8);
	const uint16_t low = value;

	uint32_t remaining = count;
	uint32_t cycles = 0;

	// Straight-to-panel fast path: one call paints the whole span.  Only taken
	// when the SoC says the LCD is inside a RAMWR window (the hook checks), and
	// only up to the cycle budget, because the guest is charged per pixel either
	// way and the frame remainder must stay correct.
	if (d->m_lcd_fill != nullptr)
	{
		const uint32_t affordable = (uint32_t)budget / AOT_HLE_FILL_CYCLES;
		const uint32_t n = remaining < affordable ? remaining : affordable;
		if (n != 0u && d->m_lcd_fill(d->m_lcd_fill_opaque, value, n))
		{
			remaining -= n;
			cycles += n * AOT_HLE_FILL_CYCLES;
			g_hle_fill_fast += n;
		}
	}

	while (remaining != 0u && cycles + AOT_HLE_FILL_CYCLES <= (uint32_t)budget)
	{
		d->write16(0x3092u, high);
		d->write16(0x3092u, low);
		remaining--;
		cycles += AOT_HLE_FILL_CYCLES;
	}

	core->m_r[unsp_device::REG_R4] = (uint16_t)remaining;
	core->m_icount -= (int32_t)cycles;
	g_hle_fill_px_win += (count - remaining);
	g_hle_fill_px_total += (count - remaining);

	if (remaining != 0u)
	{
		// Out of budget: re-enter at the loop body with r4 = copies left.
		d->set_pc_lpc(AOT_HLE_FILL_REST);
		return true;
	}

	// Final state of the interpreted pass: r2 held the last [3094] read (the
	// RX register returns 0), the 4-bit shift register sits at the value the
	// second ror4 left behind, and the final "r4 -= 1" wrapped through 0x10000.
	core->m_r[unsp_device::REG_R2] = 0u;
	core->m_sb = ((uint32_t)value >> 4) & 0xfu;
	d->update_nzsc(0x10000u, (uint16_t)1u, (uint16_t)~1u);
	d->set_pc_lpc(AOT_HLE_FILL_EXIT);
	return true;
}

// 0x005F8B / 0x005F8E: the mask-ROM 32x32->32 multiply helper.  Every "idx*12"
// and "w*row" in the renderer goes through it; a full-screen redraw calls it
// about 2,157 times, which is 45% of that frame's guest instructions by count.
// The body is a handful of 16-bit multiplies over four stack words, so it has no
// I/O and no side effects outside r1/r2 - exactly the shape worth lifting out of
// the generic ALU dispatch.
//
// The stack layout was measured rather than guessed (emu/digicore.exe --bp 207A4C,
// which showed [0FB0]=0x000C, [0FB1]=0, [0FB2]=idx, [0FB3]=0 for a request of
// idx*12): entry SP=S, "push r5" stores BP at [S] and leaves SP=S-1, so the
// helper's r5 (= SP+1) is S and the arguments sit at [S+3]..[S+6].
// Only the arithmetic is replaced; the push above and the pops at 0x005F9B/9C
// still run as guest code, so the stack is left exactly as it was.
#define AOT_HLE_MUL_ENTRY  0x005F8Bu
#define AOT_HLE_MUL_RESUME 0x005F9Bu
#define AOT_HLE_MUL_CYCLES 94u

bool g_hle_mul32 = true;
uint32_t g_hle_mul_win = 0;
uint32_t g_hle_mul_total = 0;

static bool hle_mul32(unsp_device *const d)
{
	unsp_device::internal_unsp_state *const core = d->m_core;

	const int32_t budget = core->m_icount > 0 ? core->m_icount : 0;
	if (budget < (int32_t)AOT_HLE_MUL_CYCLES)
		return false;

	const uint16_t sp = (uint16_t)core->m_r[unsp_device::REG_SP];
	const uint16_t bp = (uint16_t)core->m_r[unsp_device::REG_BP];

	// "push r5 / r5 = r0 + 1", both from the 0x005F8B block.
	d->write16(sp, bp);
	core->m_r[unsp_device::REG_SP] = (uint16_t)(sp - 1u);

	const uint16_t a_lo = d->read16((uint16_t)(sp + 3u));
	const uint16_t a_hi = d->read16((uint16_t)(sp + 4u));
	const uint16_t b_lo = d->read16((uint16_t)(sp + 5u));
	const uint16_t b_hi = d->read16((uint16_t)(sp + 6u));

	// a_lo*b_lo is unsigned; the two cross terms are signed 16x16 and only their
	// low halves reach the high word, because the guest accumulates them with
	// plain 16-bit adds that drop the carry (see aot_b005F96 / aot_b005F9A).
	const uint32_t p = (uint32_t)a_lo * (uint32_t)b_lo;
	uint16_t hi = (uint16_t)(p >> 16);
	hi = (uint16_t)(hi + (uint16_t)((int32_t)(int16_t)a_lo * (int32_t)(int16_t)b_hi));
	hi = (uint16_t)(hi + (uint16_t)((int32_t)(int16_t)a_hi * (int32_t)(int16_t)b_lo));

	core->m_r[unsp_device::REG_R1] = (uint16_t)p;
	core->m_r[unsp_device::REG_R2] = hi;
	core->m_icount -= (int32_t)AOT_HLE_MUL_CYCLES;
	g_hle_mul_win++;
	g_hle_mul_total++;

	// Resume at the "pop r5" so BP comes back from the word pushed above.
	d->set_pc_lpc(AOT_HLE_MUL_RESUME);
	return true;
}

// ---------------------------------------------------------------------------
// render_band, the sprite record loop
// ---------------------------------------------------------------------------
// 0x2079FC..0x207D29 is `render_band(y0, h)`.  It walks the scan lines and, per
// line, walks the sprite record table at 0x0608 (12 words each, count in
// ds:[0x07B8]).  The per-record body 0x207A3E..0x207D0F culls, clips, then does
// either a solid fill or a keyed blit into the 96 word line buffer at 0x07BA.
// 0x207D15 pushes that buffer to the panel.
//
// Hooking 0x207A35 (the record-loop condition) is the cheap seam: consume every
// remaining record for this line here, leave [bp+1] equal to the record count,
// and let the original code take its own "done" branch into 0x207D15.  The
// ST7735 window command and the 0x005B41 bulk write stay untouched, and all the
// work below is pure RAM arithmetic.
#define AOT_HLE_BAND_LOOP 0x207A35u
#define AOT_HLE_BAND_REC  0x0608u   // sprite record table
#define AOT_HLE_BAND_NREC 0x07B8u   // record count
#define AOT_HLE_BAND_LINE 0x07BAu   // 96 word line buffer
#define AOT_HLE_BAND_KEY  0x07E0u   // transparent colour
#define AOT_HLE_BAND_CYC  520u      // guest cycles charged per record consumed

bool g_hle_render_band = true;
uint32_t g_hle_band_recs = 0;
uint32_t g_hle_band_calls = 0;      // entered the hook
uint32_t g_hle_band_bail = 0;       // ...and returned false
uint32_t g_hle_band_flip = 0;       // lines handed back because a record is flipped

static inline int16_t hle_s16(uint16_t v)
{
	return (int16_t)v;
}

static bool hle_render_band(unsp_device *const d)
{
	unsp_device::internal_unsp_state *const core = d->m_core;
	uint32_t *const r = core->m_r;
	const uint32_t budget = core->m_icount > 0 ? (uint32_t)core->m_icount : 0u;

	// The scan-line buffer is written through the flat RAM window below; without
	// it (no SoC attached) hand the line back to the guest rather than crash.
	if (d->m_ram == nullptr)
		return false;

	const uint16_t bp = (uint16_t)r[unsp_device::REG_BP];
	const uint16_t y = d->read16((uint32_t)bp);
	const uint16_t count = d->read16(AOT_HLE_BAND_NREC);
	uint16_t idx = d->read16((uint32_t)bp + 1u);
	g_hle_band_calls++;
	if (idx >= count)
	{
		g_hle_band_bail++;
		return false;                   // nothing left, let the guest finish
	}

	// The horizontally flipped blit has never executed in any trace we have of
	// this cartridge, and forcing the flag from the console is not a usable test
	// (the game rewrites the record table every frame, so two redraws never share
	// a state).  Rather than ship an unverified branch, hand any line that uses
	// it back to the guest: correct, just not accelerated.
	for (uint16_t i = idx; i < count; i++)
	{
		if (d->read16(AOT_HLE_BAND_REC + (uint32_t)i * 12u + 0x0Bu) != 0u)
		{
			g_hle_band_flip++;
			return false;
		}
	}

	uint32_t spent = 0;
	unsigned done = 0;

	for (; idx < count; idx++)
	{
		if (spent + AOT_HLE_BAND_CYC > budget)
			break;

		const uint32_t rec = AOT_HLE_BAND_REC + (uint32_t)idx * 12u;
		const uint16_t gfx = d->read16(rec + 0u);
		const uint16_t ry0 = d->read16(rec + 3u);
		const uint16_t w = d->read16(rec + 4u);
		const uint16_t x0 = d->read16(rec + 2u);
		const uint16_t x1 = d->read16(rec + 9u);
		const uint16_t ry1 = d->read16(rec + 0x0Au);

		if (d->read16(rec + 8u) != 1u) continue;   // disabled
		if (ry0 > y) continue;                     // starts below this line
		if (ry1 < y) continue;                     // ended above this line
		if (hle_s16(x1) < 0) continue;             // fully off the left edge
		if (hle_s16(x0) > 0x5f) continue;          // fully off the right edge

		// Clip, exactly as 0x207AE1..0x207BE9 computes it.
		uint16_t cnt, srcskip, bias;
		if (hle_s16(x0) < 0)
		{
			const uint16_t over = (uint16_t)(-(int32_t)hle_s16(x0));
			cnt = (uint16_t)(w - over);
			srcskip = over;
			bias = 0;
		}
		else if (hle_s16(x1) > 0x5f)
		{
			cnt = (uint16_t)(96u - x0);
			srcskip = 0;
			bias = x0;
		}
		else
		{
			cnt = w;
			srcskip = 0;
			bias = x0;
		}
		if (cnt > 0x5fu) cnt = 0x60u;

		if (cnt != 0)
		{
			const uint16_t row = (uint16_t)(y - ry0);

			// The scan-line buffer at 0x07BA is plain guest RAM and the flat window
			// covers it, so both the fill and the blit write it through m_ram[]
			// instead of the bus.  dst + 0..95 always stays inside 0x1000.
			uint16_t *const line = d->m_ram + AOT_HLE_BAND_LINE;

			if (gfx == 0xFFFEu)
			{
				const uint16_t v = d->read16(rec + 1u);
				for (uint16_t i = 0; i < cnt; i++)
					line[bias + i] = v;
			}
			else
			{
				// src = rec[7]:rec[6] + w*row, then skip the clipped left part.
				uint32_t src = ((uint32_t)d->read16(rec + 7u) << 16) | d->read16(rec + 6u);
				src += (uint32_t)w * (uint32_t)row;
				// One page window per record: the sprite row is cnt <= 96
				// consecutive words, and a 2 KiB window covers it whole.  Without
				// it every pixel pays an indirect bus call plus the SparseRom page
				// lookup (see hle_blit_key).
				uint32_t win_base = 0xffffffffu;
				uint32_t win_mask = 0u;
				const uint16_t *win = nullptr;
				auto px_at = [&](uint32_t a) -> uint16_t {
					if (win != nullptr && (a - win_base) <= win_mask)
						return win[a & win_mask];
					win = d->m_bus->fetch_window(a, &win_base, &win_mask);
					return (win != nullptr) ? win[a & win_mask] : d->read16(a);
				};
				if (d->read16(rec + 0x0Bu) != 0)   // horizontally flipped
				{
					src += (uint32_t)w - 1u;
					src -= srcskip;
					for (uint16_t i = 0; i < cnt; i++, src--)
					{
						const uint16_t px = px_at(src);
						if (px != AOT_HLE_BAND_KEY)
							line[bias + i] = px;
					}
				}
				else
				{
					src += srcskip;
					for (uint16_t i = 0; i < cnt; i++, src++)
					{
						const uint16_t px = px_at(src);
						if (px != AOT_HLE_BAND_KEY)
							line[bias + i] = px;
					}
				}
			}
		}

		spent += AOT_HLE_BAND_CYC;
		done++;
	}

	if (done == 0)
	{
		g_hle_band_bail++;
		return false;
	}

	d->write16((uint32_t)bp + 1u, idx);
	core->m_icount -= (int32_t)spent;
	g_hle_band_recs += done;

	// Re-enter the loop condition so the guest itself decides to push the line.
	d->set_pc_lpc(AOT_HLE_BAND_LOOP);
	return true;
}
#endif

UNSP_HOT void unsp_device::execute_run()
{
	internal_unsp_state *const core = m_core;

	if (m_insn_hook == nullptr && m_cycle_hook == nullptr)
	{
		// Hot path: no tracing, so the hooks are hoisted out of the loop and
		// the interrupt check becomes a single flag test per instruction.
		while (core->m_icount >= 0)
		{
			const uint32_t lpc = UNSP_LPC;
#if C3_IRQ_DIAG
			if (lpc >= 0x004000u && lpc < 0x006000u)
			{
				if (lpc >= 0x00486Bu && lpc <= 0x0048AFu)
				{
					g_isr7_hits++;
					g_isr7_insn = m_insn_count;
					g_isr_hist[lpc - 0x00486Bu]++;
				}
				if (lpc != g_pc_ring_prev)
				{
					g_pc_ring_prev = lpc;
					g_pc_ring[g_pc_ring_ri % 48u] = lpc;
					g_pc_ring_ri++;
				}
			}
#endif
			// The block histogram belongs to the PC harness (--block-hist); on
			// the device nobody ever assigns m_block_hist, so this was a DRAM
			// load + branch on every dispatch for a guaranteed "no" (R7 audit
			// R7-07).  AOT_NO_HOOKS already removed the per-instruction copy of
			// the same test from aot.h.
#ifndef ESP_PLATFORM
			if (m_block_hist) m_block_hist[lpc & 0x3ffffu]++;
#endif
			// The PRNG block runs from flash (0x2002F4) and, in the scenes that
			// relocate it, from its RAM copy (0x0002F4).  Same code, same data
			// area - the recompiled block only touches registers and m_ram, so
			// both spellings take the same fast path.  The RAM hit is validated
			// against the flash original so a relocated or stale block cannot be
			// mistaken for it.
			const bool fast_hot = m_fasthot &&
				(lpc == 0x2002F4u ||
				 (lpc == 0x0002F4u && read16(0x0002F4u) == read16(0x2002F4u)));
			if (fast_hot)
			{
				// Statically recompiled PRNG step (0x2002F4..0x200342). This one
				// block is ~87% of all executed instructions, and the generated
				// code is the version that already matched the interpreter word
				// for word in the on-device golden test (recomp/hot_func.c).
				// The trailing "pop bp, pc" at 0x200343 stays with the
				// interpreter so the return path keeps its exact semantics.
				HotState hs;
				for (int i = 0; i < 8; i++)
					hs.r[i] = core->m_r[i];
				hs.sb = (uint16_t)core->m_sb;
				hs.ram = m_ram;
				hot_block_prng(&hs);
				for (int i = 0; i < 8; i++)
					core->m_r[i] = hs.r[i];
				core->m_sb = hs.sb;
				core->m_r[REG_PC] = 0x200343u;
				core->m_icount -= (int)HOT_BLOCK_CYCLES;
				m_insn_count += 68; // 0x2002F4..0x200342
				if (core->m_sirq)
					check_irqs();
				continue;
			}
#if C3_WAIT_COLLAPSE
			// Two-PC filter inline: the collapse only ever fires on the PRNG
			// key-wait head (0x230321) and its RAM relocation (0x000321).  On
			// every other dispatch this reads one DRAM flag and fails, and it
			// used to pay a real cross-TU call for that "no" (R7 audit: 838 B
			// function, ~11 cycles, 1.0 % of a redraw frame).  Same shape as the
			// spin_collapse call below, which has always pre-filtered.
			if ((lpc == 0x230321u || lpc == 0x000321u) && wait_loop_collapse(lpc))
			{
				if (core->m_sirq)
					check_irqs();
				continue;
			}
#endif
			// Three compares inline, and the call only for the three PCs the
			// collapse handles.  Calling it unconditionally spent a cross-TU
			// function call on every dispatch to answer "is this one of three
			// addresses", so ~97% of dispatches paid for a lookup that was going
			// to return false.
			if (m_fastspin &&
				(lpc == 0x005251u || lpc == 0x005376u || lpc == 0x005F35u) &&
				spin_collapse(lpc))
			{
				if (core->m_sirq)
					check_irqs();
				continue;
			}
#ifdef ESP_PLATFORM
			// Five HLE entry points, one bit test.  Shifted right by one, their
			// guest PCs land on bits {5, 15, 17, 26} of a 32-bit mask:
			//   0x005B4A->5  0x005B1E->15  0x005F8B->5  0x207CA3->17  0x207A35->26
			// One shift/and/bit-test rejects 87.5 % of the dispatches before any
			// global is read.  The old shape ran five (bool && pc) tests plus
			// five DRAM loads on every dispatch - measured ~22 cycles, 1.9 % of a
			// redraw frame (R7 audit R7-02).  A mask hit still runs all five
			// exact tests, so every hook keeps its own guard and its own flag,
			// and a new hook only needs its bit added here.
			if ((0x04028020u >> ((lpc >> 1) & 31u)) & 1u)
			{
				// LCD pixel push: 65 virtual cycles per pixel in C instead of
				// fourteen guest instructions (see hle_push_pixels).
				if (g_hle_push_px && lpc == AOT_HLE_PUSH_ENTRY)
				{
					HLE_T0();
					const bool hit = hle_push_pixels(this);
					HLE_ADD(g_h_push);
					if (hit)
						continue;
				}
				// Constant-colour fill: 0x005B1D runs once per pixel of a redraw
				// and never reads memory, so the whole span collapses to two
				// writes per pixel (see hle_fill_span).
				if (g_hle_fill_px && lpc == AOT_HLE_FILL_ENTRY)
				{
					HLE_T0();
					const bool hit = hle_fill_span(this);
					HLE_ADD(g_h_fill);
					if (hit)
						continue;
				}
				// Keyed sprite blit: one C loop instead of ~150k interpreted
				// instructions per 96x96 sprite (see hle_blit_key).
				if (m_hle_blit && lpc == AOT_HLE_BLIT_ENTRY)
				{
					HLE_T0();
					const bool hit = hle_blit_key();
					HLE_ADD(g_h_blit);
					if (hit)
						continue;
				}
				// 32x32->32 multiply: 0x005F8B is called for every sprite offset
				// and every fill pixel.  Replaces the arithmetic only; the push and
				// both pops stay in the guest, so the stack stays balanced.
				if (g_hle_mul32 && lpc == AOT_HLE_MUL_ENTRY)
				{
					HLE_T0();
					const bool hit = hle_mul32(this);
					HLE_ADD(g_h_mul);
					if (hit)
						continue;
				}
				// Sprite record loop: consume every remaining record for this scan
				// line in C, then let the guest re-test and push the line buffer.
				if (g_hle_render_band && lpc == AOT_HLE_BAND_LOOP)
				{
					HLE_T0();
					const bool hit = hle_render_band(this);
					HLE_ADD(g_h_band);
					if (hit)
						continue;
				}
			}
#endif
			{
				SPLIT_T0();
				const bool hit_aot = m_aot && aot_step(lpc);
#if C3_SPLIT && defined(ESP_PLATFORM)
				if (hit_aot) { SPLIT_ADD(g_cyc_aot); g_disp_aot++; }
				else { g_cyc_fast += (esp_cpu_get_cycle_count() - __t0); }
#endif
				if (hit_aot)
				{
					if (core->m_sirq)
						check_irqs();
					continue;
				}
			}
#if C3_SPLIT && defined(ESP_PLATFORM)
			const uint32_t t_inf = esp_cpu_get_cycle_count();
#endif
			const uint16_t op = fetch16(lpc);
			add_lpc(1);
#if C3_VERIFY
			if (g_verify)
			{
				internal_unsp_state save = *core;
				const bool hit = fast_step(op);
				if (hit)
				{
					uint32_t fast_regs[16];
					for (int i = 0; i < 16; i++) fast_regs[i] = core->m_r[i];
					const uint16_t fast_sb = (uint16_t)core->m_sb;
					const int fast_ic = core->m_icount;
					*core = save;
					execute_one(op);
					bool bad = false;
					for (int i = 0; i < 16; i++)
						if (fast_regs[i] != core->m_r[i]) bad = true;
					if (fast_sb != (uint16_t)core->m_sb) bad = true;
					if (fast_ic != core->m_icount) bad = true;
					if (bad && g_verify_fail < 12)
					{
						++g_verify_fail;
						std::printf("[verify] op=%04X lpc=%06X fast(pc=%04X sp=%04X sr=%04X ic=%d) ref(pc=%04X sp=%04X sr=%04X ic=%d)\n",
							op, lpc, fast_regs[7], fast_regs[6], fast_regs[REG_SR], fast_ic,
							core->m_r[7], core->m_r[6], core->m_r[REG_SR], core->m_icount);
					}
					continue;
				}
			}
#endif
#if C3_FASTOPS
			if (m_fastops && (fast_step(op) ? (++g_fast_hits, true) : false))
			{
				m_insn_count++;
#if C3_SPLIT && defined(ESP_PLATFORM)
				g_cyc_inf += (esp_cpu_get_cycle_count() - t_inf);
				g_disp_inf++;
#endif
				if (core->m_sirq)
					check_irqs();
				continue;
			}
#endif
			execute_one(op);
			m_insn_count++;
#if C3_SPLIT && defined(ESP_PLATFORM)
			g_cyc_inf += (esp_cpu_get_cycle_count() - t_inf);
			g_disp_inf++;
#endif
			if (op != 0x9a98 && core->m_sirq)
				check_irqs();
		}
		return;
	}

	while (core->m_icount >= 0)
	{
		const uint32_t lpc = UNSP_LPC;
		if (m_aot && aot_step(lpc))
		{
			if (core->m_sirq)
				check_irqs();
			continue;
		}
		if (m_insn_hook)
			m_insn_hook(m_insn_hook_opaque, lpc);
		const uint32_t op = read16(lpc);
		add_lpc(1);
		if (m_cycle_hook)
		{
			// Measure the charge exactly: park the countdown far away so no
			// instruction can observe a slice boundary, then restore it.
			const int saved = core->m_icount;
			constexpr int probe = 1 << 28;
			core->m_icount = probe;
			execute_one(op);
			m_insn_count++;
			const int charged = probe - core->m_icount;
			core->m_icount = saved - charged;
			m_cycle_hook(m_cycle_hook_opaque, lpc, charged);
		}
		else
		{
			execute_one(op);
			m_insn_count++;
		}
		if (op != 0x9a98)
			check_irqs();
	}
}

