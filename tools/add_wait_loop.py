"""Add wait_loop_collapse(): collapse the game's PRNG busy-wait loop.

The main loop at 0x230321 is

    230321  f060 02f4   call 2002f4        ; 68-instruction PRNG step
    230323  fe00        ds = 00
    230324  990c 0850   r4 = 0850
    230326  98e4        r4 = ds:[r4]
    230327  4840        cmp r4, 00
    230328  5e48        je 230321

i.e. "spin until the 64 Hz tick counter at 0x0850 becomes non-zero".  Per pass
that is 74 guest instructions and 298 cycles (measured, see dbgtest/cyc900.txt:
body 0x2002F4..0x200343 = 273, wrapper = 25).  The PRNG body already runs as
native code, but the wrapper's 7 instructions go through the interpreter and
cost ~20x more host time than the 68 native ones.

This pass runs whole iterations natively while the slice has budget and no
interrupt is pending - the same contract as spin_collapse() - and leaves the
core at 0x230321 with DS/r4/flags/SP exactly as the interpreter would have left
them after the last "je taken".

Anchors are mandatory so a drifted source stops the patch instead of silently
building something else.
"""
import io
import sys

GLUE = "emu/unsp_glue.cpp"

FUNC = r'''
// Collapse the "wait for the next 64 Hz tick" busy-wait at 0x230321.
//
// The loop calls the PRNG block (already native) and then re-reads the tick
// counter at 0x0850 until it changes.  Inside a slice with no pending
// interrupt that counter cannot change, so the whole wait can be retired here
// at ~1/20 of the interpreter's cost, with identical final state.
bool unsp_device::wait_loop_collapse(uint32_t lpc)
{
	if (!m_fastwait || lpc != 0x230321u)
		return false;

	internal_unsp_state *const core = m_core;
	const uint32_t budget = (uint32_t)(core->m_icount > 0 ? core->m_icount : 0);

	// 298 cycles per pass (measured per-instruction modal charges).
	constexpr uint32_t CYCLES_PER_PASS = 298;
	if (budget < CYCLES_PER_PASS * 2)
		return false;                       // leave short slices to the interpreter

	uint32_t passes = budget / CYCLES_PER_PASS;
	if (passes > 4096) passes = 4096;          // bounded work per call
	if (passes == 0) return false;

	// call 2002f4: push the return address and SR, then jump.
	push(0x230323u, &core->m_r[REG_SP]);
	push(core->m_r[REG_SR], &core->m_r[REG_SP]);

	HotState hs;
	uint32_t done = 0;
	while (done < passes)
	{
		for (int i = 0; i < 8; i++) hs.r[i] = core->m_r[i];
		hs.sb = (uint16_t)core->m_sb;
		hs.ram = m_ram;
		hot_block_prng(&hs);
		for (int i = 0; i < 8; i++) core->m_r[i] = hs.r[i];
		core->m_sb = hs.sb;

		// 0x200343 pop bp, pc
		core->m_r[REG_SP] = (uint16_t)(core->m_r[REG_SP] + 1);
		core->m_r[REG_BP] = read16(core->m_r[REG_SP]);
		core->m_r[REG_SP] = (uint16_t)(core->m_r[REG_SP] + 1);
		core->m_r[REG_PC] = read16(core->m_r[REG_SP]);

		// ds = 00                    (no cycles, no flags)
		set_ds(0);

		// r4 = 0850                  (ld imm16: N/Z from the loaded value)
		core->m_r[4] = 0x0850;
		update_nz(0x0850);

		// r4 = ds:[r4]               (ld indirect: N/Z from the loaded word)
		{
			const uint16_t value = read16((uint16_t)(UNSP_LREG_I(4)));
			update_nz(value);
			core->m_r[4] = value;
		}

		// cmp r4, 00                 (r0 - 0, all four flags)
		{
			const uint16_t r0 = (uint16_t)core->m_r[4];
			const uint32_t lres = r0 + (uint16_t)~0u + 1u;
			update_nzsc(lres, r0, (uint16_t)~0u);
		}

		++done;

		if (core->m_sirq)
			break;                          // an interrupt is due: hand back

		if ((core->m_r[REG_SR] & UNSP_Z) == 0)
		{
			// je not taken: fall through to 0x230329 and stop collapsing.
			core->m_r[REG_PC] = 0x230329u;
			core->m_icount -= (int)(done * CYCLES_PER_PASS - 2);
			return true;
		}
	}

	core->m_r[REG_PC] = 0x230321u;
	core->m_icount -= (int)(done * CYCLES_PER_PASS);
	return true;
}
'''


def main():
    s = io.open(GLUE, encoding="utf-8").read()
    if "wait_loop_collapse" not in s:
        anchor = "void unsp_device::execute_run()"
        i = s.find(anchor)
        if i < 0:
            sys.exit("execute_run() not found in %s" % GLUE)
        s = s[:i] + FUNC.lstrip("\n") + "\n" + s[i:]

    call = (
        "\t\t\tif (wait_loop_collapse(lpc))\n"
        "\t\t\t{\n"
        "\t\t\t\tif (core->m_sirq)\n"
        "\t\t\t\t\tcheck_irqs();\n"
        "\t\t\t\tcontinue;\n"
        "\t\t\t}\n"
    )
    if "wait_loop_collapse(lpc)" not in s:
        anchor = "\t\t\tif (m_fastspin && spin_collapse(lpc))"
        if anchor not in s:
            sys.exit("spin_collapse call site missing")
        s = s.replace(anchor, call + anchor, 1)

    io.open(GLUE, "w", encoding="utf-8", newline="\n").write(s)
    print("wait_loop_collapse installed in %s" % GLUE)
    return 0


if __name__ == "__main__":
    sys.exit(main())
