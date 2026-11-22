"""Gate timer sources whose interrupt the CPU cannot take.

timer_a (20 kHz) only ever raises IRQ0; when INT_Ctrl bit 15 is clear the CPU
cannot see it, so the event changes nothing but a pending bit in INT_Status - and
this game keeps INT_Ctrl at 0x0000 for its whole run.  Skipping those events
removes 20 of them per 4.8 kcycle slice, which was the second largest cost in the
frame loop.

Patches both places that walk the timer wheel:
  * Gpce4::cycles_to_next_event()  - the slice length
  * Gpce4::tick()                  - the event service loop
"""
import io
import sys

PAIRS = (
    ("emu/gpce4.h", "firmware/digicore-c3/main/emu/gpce4.h"),
    ("emu/gpce4.cpp", "firmware/digicore-c3/main/emu/gpce4.cpp"),
)

HELPER = """	// A source whose interrupt is masked cannot change guest state: it only sets
	// a pending bit in INT_Status.  timer_a's enable lives in INT_Ctrl bit 15 and
	// stays clear for the whole run, so its 20 kHz events are pure overhead.
	bool source_enabled(int i) const
	{
		return i != 1 || (m_interrupt_ctrl & 0x8000) != 0;
	}
"""

CTX_OLD = """        if (i == 5)                             // 300 kHz driver source
            continue;"""
CTX_NEW = """        if (i == 5)                             // 300 kHz driver source
            continue;
        if (!source_enabled(i))
            continue;"""

TICK_OLD = """            if (i == 5 && !timer_300k_enable)      // machine driver "timer"
                continue;"""
TICK_NEW = """            if (i == 5 && !timer_300k_enable)      // machine driver "timer"
                continue;
            if (!source_enabled(i))
                continue;"""


def patch_headers():
    for path in PAIRS[0]:
        s = io.open(path, encoding="utf-8").read()
        if "bool source_enabled(int i) const" in s:
            print("%s: helper present" % path)
            continue
        anchor = "\tbool timer_300k_enable = false;"
        if anchor not in s:
            sys.exit("%s: timer_300k_enable anchor missing" % path)
        s = s.replace(anchor, anchor + "\n\n" + HELPER.rstrip("\n"), 1)
        io.open(path, "w", encoding="utf-8", newline="\n").write(s)
        print("%s: helper added" % path)


def patch_impls():
    for path in PAIRS[1]:
        s = io.open(path, encoding="utf-8").read()
        hits = 0
        if CTX_OLD in s:
            s = s.replace(CTX_OLD, CTX_NEW, 1)
            hits += 1
        if TICK_OLD in s:
            s = s.replace(TICK_OLD, TICK_NEW, 1)
            hits += 1
        if hits == 0:
            print("%s: already gated or anchors moved" % path)
            continue
        io.open(path, "w", encoding="utf-8", newline="\n").write(s)
        print("%s: gated %d site(s)" % (path, hits))


def main():
    patch_headers()
    patch_impls()
    return 0


if __name__ == "__main__":
    sys.exit(main())
