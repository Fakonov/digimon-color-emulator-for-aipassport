"""Point the C3 component at the amalgamated unSP core.

Replaces the six separate entries (unsp_glue + five ISA files) with the single
unsp_amalgam.cpp translation unit so the interpreter's dispatch chain can inline.
Idempotent.
"""
import io
import sys

P = "firmware/digicore-c3/main/CMakeLists.txt"

OLD = '''
    "emu/unsp_glue.cpp"
    "emu/isa/unsp_other.cpp"
    "emu/isa/unsp_fxxx.cpp"
    "emu/isa/unsp_exxx.cpp"
    "emu/isa/unsp_extended.cpp"
    "emu/isa/unsp_jumps.cpp"
'''

NEW = '''
    "emu/isa/unsp_amalgam.cpp"
'''


def main():
    s = io.open(P, encoding="utf-8").read()
    if "unsp_amalgam.cpp" in s:
        print("already updated")
        return 0
    if OLD not in s:
        sys.exit("source list anchor not found in %s" % P)
    s = s.replace(OLD, NEW, 1)
    io.open(P, "w", encoding="utf-8", newline="\n").write(s)
    print("updated %s" % P)
    return 0


if __name__ == "__main__":
    sys.exit(main())
