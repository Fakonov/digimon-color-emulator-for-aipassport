// MAME unSP device classes, reduced to plain C++ so the original ISA
// implementations (ref/unsp/unsp_{other,fxxx,exxx,extended,jumps}.cpp) compile
// verbatim. Device/DRC/disassembler machinery is replaced by a bus callback and
// a small execution harness in emu/unsp_glue.cpp.
#pragma once

#include "emu.h"

#define UNSP_LPC            (((m_core->m_r[REG_SR] & 0x3f) << 16) | m_core->m_r[REG_PC])
#define UNSP_LREG_I(reg)    (((m_core->m_r[REG_SR] << 6) & 0x3f0000) | m_core->m_r[reg])

#define UNSP_N  0x0200
#define UNSP_Z  0x0100
#define UNSP_S  0x0080
#define UNSP_C  0x0040

#define UNSP_N_SHIFT    9
#define UNSP_Z_SHIFT    8
#define UNSP_S_SHIFT    7
#define UNSP_C_SHIFT    6

enum
{
	UNSP_FIQ_LINE = 0,
	UNSP_IRQ0_LINE,
	UNSP_IRQ1_LINE,
	UNSP_IRQ2_LINE,
	UNSP_IRQ3_LINE,
	UNSP_IRQ4_LINE,
	UNSP_IRQ5_LINE,
	UNSP_IRQ6_LINE,
	UNSP_IRQ7_LINE,
	UNSP_BRK_LINE,

	UNSP_NUM_LINES
};

// word-addressed buses seen by the CPU core
struct UnspBus
{
	virtual ~UnspBus() {}
	virtual uint16_t read16(uint32_t address) = 0;
	virtual void write16(uint32_t address, uint16_t data) = 0;
	// Optional instruction-fetch window: returns a host pointer that directly
	// backs `address` (aligned down to `window` words) and stores the guest base
	// of that window in `base`. NULL when the address has no such window (IO,
	// unmapped). Lets the interpreter fetch opcodes with one compare and one
	// array read instead of a virtual call plus page-table walk.
	virtual const uint16_t *fetch_window(uint32_t address, uint32_t *base, uint32_t *mask)
	{
		(void)address; (void)base; (void)mask;
		return nullptr;
	}
};

class unsp_device
{
public:
	enum : uint32_t
	{
		REG_SP = 0,
		REG_R1,
		REG_R2,
		REG_R3,
		REG_R4,
		REG_BP,
		REG_SR,
		REG_PC,

		REG_SR1 = 0,
		REG_SR2,
		REG_SR3,
		REG_SR4
	};

	struct internal_unsp_state
	{
		uint32_t m_r[16]; // 32 bits because the original core allows 32-bit regs
		uint32_t m_secbank[4];
		uint32_t m_enable_irq;
		uint32_t m_enable_fiq;
		uint32_t m_fir_move;
		uint32_t m_fiq;
		uint32_t m_irq;
		uint32_t m_sirq;
		uint32_t m_sb;
		uint32_t m_aq;
		uint32_t m_fra;
		uint32_t m_bnk;
		uint32_t m_ine;
		uint32_t m_pri;

		uint32_t m_divq_bit;
		uint32_t m_divq_dividend;
		uint32_t m_divq_divisor;
		uint32_t m_divq_a;

		uint32_t m_arg0;
		uint32_t m_arg1;
		uint32_t m_jmpdest;

		int m_icount;
	};

	internal_unsp_state *m_core;
	int m_iso;
	internal_unsp_state m_local_core;


	// Guest instruction counter.  execute_run() bumps this once per executed
	// instruction; the harness folds it into a 64-bit total per frame (32 bits
	// never wraps inside one frame - a frame is < 2 M instructions).  One
	// increment is much cheaper than a 64-bit add on RV32IMC.
	uint32_t m_insn_count = 0;
	uint32_t take_insns()
	{
		const uint32_t n = m_insn_count;
		m_insn_count = 0;
		return n;
	}

	UnspBus *m_bus;

	// Optional shortcut for HLE blocks that push a constant colour to the panel:
	// returns true when the SoC filled the whole run itself, false when the
	// caller has to fall back to writing 0x3092 byte by byte (the LCD is not in
	// a RAMWR window, so the register model must see the traffic).  Set by the
	// harness during guest_init().
	bool (*m_lcd_fill)(void *opaque, uint16_t value, uint32_t count) = nullptr;
	void *m_lcd_fill_opaque = nullptr;

	uint16_t m_vectorbase;

	// coverage / debugging hooks (set by the harness, may be null)
	void (*m_insn_hook)(void *opaque, uint32_t lpc) = nullptr;
	void *m_insn_hook_opaque = nullptr;
	// Optional per-instruction cycle report: fired after each instruction with
	// the number of cycles the instruction charged.  Used to diff the cycle
	// model against MAME's trace.
	void (*m_cycle_hook)(void *opaque, uint32_t lpc, int cycles) = nullptr;
	void *m_cycle_hook_opaque = nullptr;

	unsp_device() : m_core(&m_local_core), m_iso(20), m_bus(nullptr), m_vectorbase(0) { reset_state(); }

	virtual ~unsp_device() {}

	void reset_state()
	{
		std::memset(&m_local_core, 0, sizeof(m_local_core));
		m_local_core.m_sb = 0;
		m_local_core.m_icount = 0;
	}

	// Flat RAM window: the SoC hands us its word array once, so the hot path
	// (stack traffic, all of the game's zero-page state) never pays a virtual
	// call.  Addresses outside the window fall back to the bus.
	uint16_t *m_ram = nullptr;
	uint32_t m_ram_limit = 0;

	uint16_t read16(uint32_t address)
	{
		if (address <= m_ram_limit)
			return m_ram[address];
		return m_bus->read16(address);
	}
	void write16(uint32_t address, uint16_t data)
	{
		if (address <= m_ram_limit)
			m_ram[address] = data;
		else
			m_bus->write16(address, data);
	}

	void add_lpc(const int32_t offset)
	{
		const uint32_t new_lpc = UNSP_LPC + offset;
		m_core->m_r[REG_PC] = (uint16_t)new_lpc;
		m_core->m_r[REG_SR] &= 0xffc0;
		m_core->m_r[REG_SR] |= (new_lpc >> 16) & 0x3f;
	}

	// Instruction-fetch window cache: code lives in one or two 2 KiB pages at a
	// time (internal ROM 0x4800.. / flash 0x230000..), so a single-entry cache
	// turns every opcode fetch into "compare base, index array".
	const uint16_t *m_win_ptr = nullptr;
	uint32_t m_win_base = 0;
	uint32_t m_win_mask = 0;

	inline uint16_t fetch16(uint32_t address)
	{
		if ((address & ~m_win_mask) == m_win_base)
			return m_win_ptr[address & m_win_mask];
		uint32_t base = 0, mask = 0;
		const uint16_t *p = m_bus->fetch_window(address, &base, &mask);
		if (p)
		{
			m_win_ptr = p;
			m_win_base = base;
			m_win_mask = mask;
			return p[address & mask];
		}
		m_win_ptr = nullptr;
		m_win_base = 0xffffffffu;
		m_win_mask = 0;
		return read16(address);
	}

	// ---- implemented in unsp_glue.cpp ----
	void execute_run();
	bool spin_collapse(uint32_t lpc);
	bool m_fastspin = true;     // collapse the game's countdown loops
	bool m_fasthot = true;      // run the recompiled PRNG block natively
	bool m_fastops = false;   // measured slower than the interpreter on RV32, kept for experiments      // single-instruction fast handlers for the hottest forms
	bool fast_step(uint16_t op);
	bool wait_loop_collapse(uint32_t lpc);
	uint32_t *m_block_hist = nullptr;
	// AOT block execution (emu/aot_blocks.c, generated by tools/aot.py).
	// aot_step() returns false when lpc has no block entry, in which case the
	// caller must run the instruction with the interpreter.
	bool m_aot = true;
	bool m_hle_blit = true;           // GPCE4_AOT=0 disables the generated blocks
	bool aot_step(uint32_t lpc);
#ifdef ESP_PLATFORM
	// Whole-loop replacement for the keyed sprite blit at 0x207CA3.
	bool hle_blit_key();
#endif
	uint64_t m_aot_blocks = 0;   // blocks retired natively (statistics only)
	void *m_aot_cur = nullptr;   // AotCtx of the running block (debug views only)
	uint64_t m_aot_fallback = 0; // native-block instructions that had to fall back
	uint64_t m_aot_miss = 0;     // dispatch attempts with no block at that PC
	uint64_t m_aot_insns = 0;    // instructions executed inside native blocks
	uint32_t m_collapse_passes = 0;   // wait-loop passes retired natively
	uint32_t m_collapse_calls = 0;    // collapse invocations   // --block-hist: per-PC dynamic counts
	bool m_fastwait = true;  // draft: breaks the call frame, see docs/C3_PORT.md 8.2.1     // collapse the PRNG busy-wait loop at 0x230321
	void execute_set_input(int inputnum, int state);
	void set_state_unsynced(int inputnum, int state);
	void set_ds(uint16_t ds);
	void set_pc_lpc(uint32_t address);
	uint16_t get_ds();
	void set_fr(uint16_t fr);
	uint16_t get_fr();
	void push(uint32_t value, uint32_t *reg);
	uint16_t pop(uint32_t *reg);
	void update_nz(uint32_t value);
	void update_nzsc(uint32_t value, uint16_t r0, uint16_t r1);
	void unimplemented_opcode(uint16_t op);
	void unimplemented_opcode(uint16_t op, uint16_t ximm);
	void unimplemented_opcode(uint16_t op, uint16_t ximm, uint16_t ximm_2);
	void check_irqs();
	void trigger_fiq();
	void trigger_irq(int line);

	// ---- implemented in ref/unsp/*.cpp ----
	void execute_one(const uint16_t op);
	void execute_jumps(const uint16_t op);
	void execute_remaining(const uint16_t op);
	void execute_fxxx_000_group(uint16_t op);
	void execute_fxxx_001_group(uint16_t op);
	void execute_fxxx_010_group(uint16_t op);
	void execute_fxxx_011_group(uint16_t op);
	virtual void execute_fxxx_101_group(uint16_t op);
	void execute_fxxx_110_group(uint16_t op);
	void execute_fxxx_111_group(uint16_t op);
	void execute_fxxx_group(uint16_t op);
	void execute_fxxx_100_group(uint16_t op);
	virtual void execute_extended_group(uint16_t op);
	virtual void execute_exxx_group(uint16_t op);
	void execute_muls_ss(const uint16_t rd, const uint16_t rs, const uint16_t size);
	bool do_basic_alu_ops(const uint16_t &op0, uint32_t &lres, uint16_t &r0, uint16_t &r1, uint32_t &r2, bool update_flags);
	virtual bool op_is_divq(const uint16_t op) { return false; }
};

class unsp_11_device : public unsp_device
{
public:
	unsp_11_device() { m_iso = 11; }
};

class unsp_12_device : public unsp_11_device
{
public:
	unsp_12_device() { m_iso = 12; }
	virtual void execute_fxxx_101_group(uint16_t op) override;
	virtual void execute_exxx_group(uint16_t op) override;
	void execute_divq(uint16_t op);
	bool op_is_divq(const uint16_t op) override;
};

class unsp_20_device : public unsp_12_device
{
public:
	unsp_20_device() { m_iso = 20; }
	virtual void execute_extended_group(uint16_t op) override;
};
