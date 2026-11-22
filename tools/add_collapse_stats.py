"""Add collapse-pass counters to the unSP core (host tree and C3 copy).

Both trees share the same wait_loop_collapse() body, so the same two return
sites are patched in each.  Idempotent, and it refuses to write a half-patched
file when an anchor moved.
"""
import io
import sys

FILES = ("emu/unsp_glue.cpp", "firmware/digicore-c3/main/emu/unsp_glue.cpp")

SITE_EARLY = (
    "\t\t\tset_pc_lpc(0x230329u);\n"
    "\t\t\tcore->m_icount -= (int)(done * CYCLES_PER_PASS - 2);\n"
    "\t\t\treturn true;"
)
SITE_EARLY_NEW = (
    "\t\t\tset_pc_lpc(0x230329u);\n"
    "\t\t\tcore->m_icount -= (int)(done * CYCLES_PER_PASS - 2);\n"
    "\t\t\tm_collapse_passes += done;\n"
    "\t\t\tm_collapse_calls++;\n"
    "\t\t\treturn true;"
)

SITE_TAIL = (
    "\tset_pc_lpc(0x230321u);\n"
    "\tcore->m_icount -= (int)(done * CYCLES_PER_PASS);\n"
    "\treturn true;"
)
SITE_TAIL_NEW = (
    "\tset_pc_lpc(0x230321u);\n"
    "\tcore->m_icount -= (int)(done * CYCLES_PER_PASS);\n"
    "\tm_collapse_passes += done;\n"
    "\tm_collapse_calls++;\n"
    "\treturn true;"
)


def main():
    for path in FILES:
        s = io.open(path, encoding="utf-8").read()
        if "m_collapse_passes +=" in s:
            print("%s: already patched" % path)
            continue
        if SITE_EARLY not in s or SITE_TAIL not in s:
            sys.exit("%s: collapse return sites not found" % path)
        s = s.replace(SITE_EARLY, SITE_EARLY_NEW, 1)
        s = s.replace(SITE_TAIL, SITE_TAIL_NEW, 1)
        io.open(path, "w", encoding="utf-8", newline="\n").write(s)
        print("%s: patched 2 sites" % path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
