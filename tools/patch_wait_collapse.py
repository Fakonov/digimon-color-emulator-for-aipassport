"""Rewrite wait_loop_collapse() so a pass costs one PRNG call instead of a
dozen bus round-trips.

The old body re-read SP/BP and the 64 Hz tick counter through the bus on every
pass.  Inside a slice neither can change (the tick counter only moves from the
interrupt handler, and a pending interrupt ends the collapse), so both are read
once and the loop reduces to `hot_block_prng()` plus the wrapper's flag update.

Idempotent: detects the new body by its marker comment.
"""
import io
import sys

FILES = ("emu/unsp_glue.cpp", "firmware/digicore-c3/main/emu/unsp_glue.cpp")

MARK = "// One bus read per collapse, not one per pass."

NEW = r'''bool unsp_device::wait_loop_collapse(uint32_t lpc)
{
	if (!m_fastwait || lpc != 0x230321u)
		return false;

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

	uint32_t done = 0;
	while (done < passes)
	{
		hs.r[4] = tick;                     // r4 = ds:[0850]
		hot_block_prng(&hs);
		hs.r[REG_SP] = (uint16_t)(hs.r[REG_SP] + 1);   // pop bp
		hs.r[REG_BP] = bp_saved;
		++done;

		if (tick != 0)
			break;                      // cmp r4, 00 / je not taken: loop exits
		if (core->m_sirq)
			break;                      // an interrupt is due: hand back
	}

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
	m_collapse_calls++;
	return true;
}

'''


def main():
    for path in FILES:
        s = io.open(path, encoding="utf-8").read()
        if MARK in s:
            print("%s: already patched" % path)
            continue
        start = s.find("bool unsp_device::wait_loop_collapse(uint32_t lpc)")
        if start < 0:
            sys.exit("%s: wait_loop_collapse not found" % path)
        end = s.find("void unsp_device::execute_run()", start)
        if end < 0:
            sys.exit("%s: execute_run anchor not found" % path)
        s = s[:start] + NEW + s[end:]
        io.open(path, "w", encoding="utf-8", newline="\n").write(s)
        print("%s: rewritten" % path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
