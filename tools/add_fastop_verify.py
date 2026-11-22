"""Add a temporary self-check to the interpreter's fast path.

With GPCE4_FASTVERIFY=1 every instruction claimed by fast_step() is executed
twice: once by the fast handler and once by the reference interpreter, and the
register file, SB and cycle count are compared.  The interpreter's result wins,
so the guest keeps running correctly while the first mismatches are printed.
"""
import io
import sys

GLUE = "emu/unsp_glue.cpp"
MAIN = "emu/main.cpp"

VERIFY_BLOCK = """			if (g_verify)
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
						std::printf("[verify] op=%04X lpc=%06X fast(pc=%04X sp=%04X sr=%04X ic=%d) ref(pc=%04X sp=%04X sr=%04X ic=%d)\\n",
							op, lpc, fast_regs[7], fast_regs[6], fast_regs[REG_SR], fast_ic,
							core->m_r[7], core->m_r[6], core->m_r[REG_SR], core->m_icount);
					}
					continue;
				}
			}
"""


def main():
    s = io.open(GLUE, encoding="utf-8").read()
    if "g_verify" in s:
        print("glue already patched")
    else:
        s = s.replace(
            "unsigned long long g_fast_calls = 0, g_fast_hits = 0;",
            "unsigned long long g_fast_calls = 0, g_fast_hits = 0;\nint g_verify = 0;\nint g_verify_fail = 0;",
            1,
        )
        anchor = "\t\t\tif (m_fastops && (fast_step(op) ? (++g_fast_hits, true) : false))"
        if anchor not in s:
            sys.exit("glue anchor missing")
        s = s.replace(anchor, VERIFY_BLOCK + anchor, 1)
        io.open(GLUE, "w", encoding="utf-8", newline="\n").write(s)
        print("glue patched")

    m = io.open(MAIN, encoding="utf-8").read()
    if "GPCE4_FASTVERIFY" in m:
        print("main already patched")
        return 0
    anchor = '\tif (std::getenv("GPCE4_FASTOPS")'
    if anchor not in m:
        sys.exit("main anchor missing")
    m = m.replace(
        anchor,
        '\tif (std::getenv("GPCE4_FASTVERIFY"))\n\t{\n\t\textern int g_verify;\n\t\tg_verify = 1;\n\t}\n' + anchor,
        1,
    )
    io.open(MAIN, "w", encoding="utf-8", newline="\n").write(m)
    print("main patched")
    return 0


if __name__ == "__main__":
    sys.exit(main())
