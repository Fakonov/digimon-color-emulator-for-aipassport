"""Fill the whole panel black once, then keep only the window dirty-tracked.

The partial blit only ever addresses the 192x192 game window, so everything
outside it stays whatever the panel's GRAM held at power-on - which looks like a
big field of colour noise around a small picture.  Clear the full 320x240 once
after display_init(), and again whenever the backlight comes up.
"""
import io

PATH = "firmware/digicore-c3/main/app_main.cpp"

HELPER = """
// One-off full-panel fill.  The per-frame blit only touches the 192x192 window,
// so the surround has to be painted once or the panel shows its power-on GRAM.
static void panel_fill_black(void)
{
    static uint8_t zero_strip[kWinW * kScale * kChunkRows * 2];
    if (!s_display_ok)
        return;
    memset(zero_strip, 0, sizeof(zero_strip));
    for (int y = 0; y < 320 / 2 + 120; y += kChunkRows) {
        const int rows = (y + kChunkRows <= 240) ? kChunkRows : (240 - y);
        if (y >= 240)
            break;
        display_draw_bitmap(0, y, 320, rows, zero_strip);
    }
}
"""

CALL_OLD = """        // Wipe the panel's random power-on GRAM before the guest draws anything,
        // otherwise the first ~1 s of boot shows coloured noise.
        blit_frame();"""

CALL_NEW = """        // Wipe the panel's random power-on GRAM before the guest draws anything,
        // otherwise the first ~1 s of boot shows coloured noise.
        panel_fill_black();
        blit_frame();"""


def main():
    s = io.open(PATH, encoding="utf-8").read()
    if "panel_fill_black" in s:
        print("already present")
        return 0
    anchor = "static void blit_frame(void)"
    if anchor not in s:
        raise SystemExit("blit_frame anchor missing")
    s = s.replace(anchor, HELPER.strip("\n") + "\n\n" + anchor, 1)
    if CALL_OLD not in s:
        raise SystemExit("init call anchor missing")
    s = s.replace(CALL_OLD, CALL_NEW, 1)
    io.open(PATH, "w", encoding="utf-8", newline="\n").write(s)
    print("panel fill added")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
