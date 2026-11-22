"""Port the single-instruction fast handlers (fast_step) into the C3 firmware tree.

The C3 tree carries a copy of emu/ with device-side instrumentation, so the
handler body is copied verbatim from emu/unsp_glue.cpp and the call site is
inserted before execute_one() in the hot loop.  Anchors are mandatory: a
missing anchor means the C3 copy drifted from the PC sources and the script
stops instead of producing a half-patched build.
"""
import io
import sys

SRC = "emu/unsp_glue.cpp"
DST = "firmware/digicore-c3/main/emu/unsp_glue.cpp"
HDR = "firmware/digicore-c3/main/emu/compat/unsp.h"

START = "// Single-instruction fast path."
END = "void unsp_device::execute_run()"


def main():
    src = io.open(SRC, encoding="utf-8").read()
    i = src.find(START)
    j = src.find(END)
    if i < 0 or j < 0 or j <= i:
        sys.exit("cannot locate fast_step in the PC sources")
    block = src[i:j]

    dst = io.open(DST, encoding="utf-8").read()
    if "fast_step" not in dst:
        k = dst.find(END)
        if k < 0:
            sys.exit("cannot locate execute_run() in the C3 copy")
        dst = dst[:k] + block + dst[k:]

    anchor = "\t\t\texecute_one(op);\n\t\t\tm_insn_count++;\n"
    if "m_fastops && fast_step(op)" not in dst:
        n = dst.count(anchor)
        if n < 1:
            sys.exit("hot-loop execute_one() site not found")
        dst = dst.replace(
            anchor,
            "\t\t\tif (m_fastops && fast_step(op))\n"
            "\t\t\t{\n"
            "\t\t\t\tm_insn_count++;\n"
            "\t\t\t\tif (core->m_sirq)\n"
            "\t\t\t\t\tcheck_irqs();\n"
            "\t\t\t\tcontinue;\n"
            "\t\t\t}\n" + anchor,
            1,
        )
    io.open(DST, "w", encoding="utf-8", newline="\n").write(dst)

    hdr = io.open(HDR, encoding="utf-8").read()
    if "bool fast_step" not in hdr:
        anchor = "\tbool m_fasthot = true;"
        if anchor not in hdr:
            sys.exit("header anchor missing")
        hdr = hdr.replace(
            anchor,
            anchor + "\n\tbool m_fastops = true;\n\tbool fast_step(uint16_t op);",
            1,
        )
        io.open(HDR, "w", encoding="utf-8", newline="\n").write(hdr)

    print("patched %s and %s" % (DST, HDR))
    return 0


if __name__ == "__main__":
    sys.exit(main())
