"""Stop the 300 kHz SPI2 driver source from costing 15 timer events per slice.

MAME raises IRQ3 from a machine-level 300 kHz timer whose only job is to push the
next SPI pixel to the LCD panel.  In this model the SPI2 transfer completes
synchronously on the register write, so the source never changes guest state: it
only sets INT_Status bit 24, which the game never enables (INT_Ctrl stays 0x0000)
and never polls.

Leaving it scheduled cost 15 events per 4.8 kcycle slice -- 7,500 events per
guest frame, about a third of the host budget.  The flag now defaults to off and
can be re-enabled at runtime (Gpce4::timer_300k_enable) if a scene turns out to
depend on it.

Idempotent.
"""
import io
import sys

FILES = ("emu/gpce4.h", "firmware/digicore-c3/main/emu/gpce4.h")

OLD = "bool timer_300k_enable = true;"
NEW = (
    "// The 300 kHz SPI2 driver source only sets INT_Status bit 24; nothing in\n"
    "\t// the model or the game consumes it (see docs/C3_PORT.md 8.4).  Off by\n"
    "\t// default: it costs 15 events per 4.8 kcycle slice for no state change.\n"
    "\tbool timer_300k_enable = false;"
)


def main():
    for path in FILES:
        s = io.open(path, encoding="utf-8").read()
        if "bool timer_300k_enable = false;" in s:
            print("%s: already off" % path)
            continue
        if OLD not in s:
            sys.exit("%s: flag declaration not found" % path)
        s = s.replace(OLD, NEW, 1)
        io.open(path, "w", encoding="utf-8", newline="\n").write(s)
        print("%s: disabled" % path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
