"""Port wait_loop_collapse() from the PC tree into the C3 firmware tree.

The two execute_run() implementations differ (the C3 one counts guest
instructions and carries the profiling baskets), so the function body is copied
verbatim from emu/unsp_glue.cpp and only the call site is generated here.

Every anchor is mandatory: a drifted source aborts instead of building
something subtly different.
"""
import io
import re
import sys

SRC = "emu/unsp_glue.cpp"
DST = "firmware/digicore-c3/main/emu/unsp_glue.cpp"
HDR = "firmware/digicore-c3/main/emu/compat/unsp.h"

CALL = (
    "\t\t\tif (m_fastwait && wait_loop_collapse(lpc))\n"
    "\t\t\t{\n"
    "\t\t\t\tif (core->m_sirq)\n"
    "\t\t\t\t\tcheck_irqs();\n"
    "#if C3_PROF\n"
    "\t\t\t\tm_prof_spin += esp_cpu_get_cycle_count() - prof_t0;\n"
    "#endif\n"
    "\t\t\t\tcontinue;\n"
    "\t\t\t}\n"
)


def extract(text):
    start = text.find("// Collapse the \"wait for the next 64 Hz tick\"")
    if start < 0:
        sys.exit("wait_loop_collapse body not found in %s" % SRC)
    end = text.find("\nvoid unsp_device::execute_run()", start)
    if end < 0:
        sys.exit("execute_run() not found after the collapse body")
    return text[start:end].rstrip("\n") + "\n\n"


def main():
    src = io.open(SRC, encoding="utf-8").read()
    body = extract(src)

    dst = io.open(DST, encoding="utf-8").read()
    if "wait_loop_collapse(uint32_t lpc)" not in dst:
        anchor = "void unsp_device::execute_run()"
        i = dst.find(anchor)
        if i < 0:
            sys.exit("execute_run() not found in %s" % DST)
        dst = dst[:i] + body + dst[i:]

    if "wait_loop_collapse(lpc)" not in dst:
        anchor = "\t\t\tif (m_fastspin && spin_collapse(lpc))"
        if anchor not in dst:
            sys.exit("spin_collapse call site missing in %s" % DST)
        dst = dst.replace(anchor, CALL + anchor, 1)

    io.open(DST, "w", encoding="utf-8", newline="\n").write(dst)

    hdr = io.open(HDR, encoding="utf-8").read()
    if "wait_loop_collapse" not in hdr:
        anchor = "\tbool spin_collapse(uint32_t lpc);"
        if anchor not in hdr:
            m = re.search(r"\tbool spin_collapse\(uint32_t lpc\);[^\n]*\n", hdr)
            if not m:
                sys.exit("spin_collapse declaration missing in %s" % HDR)
            hdr = hdr[: m.end()] + "\tbool wait_loop_collapse(uint32_t lpc);\n" + hdr[m.end():]
        else:
            hdr = hdr.replace(anchor, anchor + "\n\tbool wait_loop_collapse(uint32_t lpc);", 1)
        if "bool m_fastwait" not in hdr:
            hdr = hdr.replace(
                "\tbool m_fasthot = true;",
                "\tbool m_fasthot = true;\n\tbool m_fastwait = true;  // collapse the PRNG busy-wait at 0x230321",
                1,
            )
        io.open(HDR, "w", encoding="utf-8", newline="\n").write(hdr)

    print("wait_loop_collapse ported to the C3 tree")
    return 0


if __name__ == "__main__":
    sys.exit(main())
