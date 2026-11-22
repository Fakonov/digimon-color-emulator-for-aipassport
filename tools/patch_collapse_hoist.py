"""Hoist the loop-invariant tests out of the collapsed wait loop.

Inside a collapse neither the 64 Hz counter (tick) nor the pending-interrupt
flag can change - tick() is what raises interrupts, and it does not run while
the collapse holds the CPU.  So the per-pass `if (tick != 0)` and
`if (m_sirq)` tests were pure overhead: tested once up front instead.
"""
import io
import sys

FILES = ("emu/unsp_glue.cpp", "firmware/digicore-c3/main/emu/unsp_glue.cpp")

MARK = "// Neither of the two conditions below can change while we hold the CPU."

OLD = """	uint32_t done = 0;
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
"""

NEW = """	// Neither of the two conditions below can change while we hold the CPU:
	// the counter only moves in the 64 Hz handler, and only tick() (which is
	// not called here) raises the pending-interrupt flag.  So they are tested
	// once, not once per pass.
	uint32_t done = 0;
	if (tick == 0 && !core->m_sirq)
	{
		while (done < passes)
		{
			hs.r[4] = 0;                    // r4 = ds:[0850]
			hot_block_prng(&hs);
			hs.r[REG_SP] = (uint16_t)(hs.r[REG_SP] + 1);   // pop bp
			hs.r[REG_BP] = bp_saved;
			++done;
		}
	}
	else
	{
		hs.r[4] = tick;                     // r4 = ds:[0850]
		hot_block_prng(&hs);
		hs.r[REG_SP] = (uint16_t)(hs.r[REG_SP] + 1);
		hs.r[REG_BP] = bp_saved;
		done = 1;
	}
"""


def main():
    for path in FILES:
        s = io.open(path, encoding="utf-8").read()
        if MARK in s:
            print("%s: already patched" % path)
            continue
        if OLD not in s:
            sys.exit("%s: collapse loop body not found" % path)
        s = s.replace(OLD, NEW, 1)
        io.open(path, "w", encoding="utf-8", newline="\n").write(s)
        print("%s: hoisted" % path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
