"""Put the worst-case blit duration into the once-a-second log line.

The per-second average hid the SPI cost: on the clock screen almost no rows are
dirty, so blit_pct stayed at 3% while a full-screen animation would push 73 KB
through SPI.  blitmax shows the real worst case per second.
"""
import io

PATH = "firmware/digicore-c3/main/app_main.cpp"


def main():
    s = io.open(PATH, encoding="utf-8").read()
    if "blitmax=" in s:
        print("already wired")
        return 0

    if '"guest_pct=%u blit_pct=%u hot=%u' not in s:
        raise SystemExit("format anchor missing")
    s = s.replace('"guest_pct=%u blit_pct=%u hot=%u',
                  '"guest_pct=%u blit_pct=%u blitmax=%u hot=%u', 1)

    anchor = "unsigned(uint32_t((uint64_t(s_blit_us) * 100u) / dt_us)),"
    if anchor not in s:
        raise SystemExit("arg anchor missing")
    s = s.replace(anchor, anchor + "\n                     unsigned(s_blit_max_us),", 1)

    if "s_blit_us = 0;" not in s:
        raise SystemExit("reset anchor missing")
    s = s.replace("s_blit_us = 0;", "s_blit_us = 0;\n            s_blit_max_us = 0;", 1)

    io.open(PATH, "w", encoding="utf-8", newline="\n").write(s)
    print("blitmax wired")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
