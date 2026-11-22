"""Log the interrupt controller state once a second.

Hypothesis: the transition stall is not the drawing code but the guest unmasking
the 20 kHz timer (its audio clock) for a moment - every tick then costs an
interpreted handler.  Printing INT_Ctrl and INT_Status next to the frame rate
confirms or kills that in one boot.
"""
import io

PATH = "firmware/digicore-c3/main/app_main.cpp"


def main():
    s = io.open(PATH, encoding="utf-8").read()
    if "int_ctrl=" in s:
        print("already wired")
        return 0

    fmt = '"guest_pct=%u blit_pct=%u blitmax=%u rows=%u hot=%u'
    if fmt not in s:
        raise SystemExit("format anchor missing")
    s = s.replace(fmt, '"int_ctrl=%04x int_stat=%08x guest_pct=%u blit_pct=%u blitmax=%u rows=%u hot=%u', 1)

    arg = "                     unsigned(rows_prev),"
    if arg not in s:
        raise SystemExit("arg anchor missing")
    s = s.replace(arg,
                  "                     unsigned(s_soc.m_interrupt_ctrl),\n"
                  "                     unsigned(s_soc.m_interrupt_status),\n"
                  + arg, 1)
    io.open(PATH, "w", encoding="utf-8", newline="\n").write(s)
    print("int ctrl/status wired")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
