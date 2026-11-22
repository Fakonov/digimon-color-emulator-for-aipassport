"""Temporary diagnostics: auto-press B1 once, and count the panel rows pushed.

The user sees the frame rate fall to ~20 while the game is on an animated screen.
Two very different causes produce that number: the guest doing more work, or the
panel link moving a full frame every time.  Counting pushed rows separates them:
192 rows per frame means every row was dirty, a handful means only sprites moved.
"""
import io

PATH = "firmware/digicore-c3/main/app_main.cpp"


def main():
    s = io.open(PATH, encoding="utf-8").read()

    if "s_rows_sent" not in s:
        anchor = "static uint32_t s_blit_max_us = 0;"
        if anchor not in s:
            raise SystemExit("counter anchor missing")
        s = s.replace(anchor, anchor + "\nstatic uint32_t s_rows_sent = 0;", 1)

        draw = "        const int64_t t_blit = esp_timer_get_time();"
        if draw not in s:
            raise SystemExit("draw anchor missing")
        s = s.replace(draw, "        s_rows_sent += uint32_t(rows * kScale);\n" + draw, 1)

        reset = "            s_blit_max_us = 0;"
        if reset not in s:
            raise SystemExit("reset anchor missing")
        s = s.replace(reset, reset + "\n            const uint32_t rows_prev = s_rows_sent;\n            s_rows_sent = 0;", 1)

        fmt = '"guest_pct=%u blit_pct=%u blitmax=%u hot=%u'
        if fmt not in s:
            raise SystemExit("format anchor missing")
        s = s.replace(fmt, '"guest_pct=%u blit_pct=%u blitmax=%u rows=%u hot=%u', 1)

        arg = "                     unsigned(s_blit_max_us),"
        if arg not in s:
            raise SystemExit("arg anchor missing")
        s = s.replace(arg, arg + "\n                     unsigned(rows_prev),", 1)

    if "kAutoPressAt" not in s:
        decl = "static constexpr int kPressFrames = 5;"
        if decl not in s:
            raise SystemExit("press anchor missing")
        s = s.replace(decl, decl + "\n// diagnostic: press B1 once at this guest frame (0 = disabled)\nstatic constexpr uint32_t kAutoPressAt = 300;", 1)

        hook = "        update_buttons();"
        if hook not in s:
            raise SystemExit("update_buttons anchor missing")
        s = s.replace(hook,
                      "        if (kAutoPressAt != 0 && s_frames == kAutoPressAt) {\n"
                      "            ESP_LOGI(TAG, \"auto press B1 at frame %u\", unsigned(s_frames));\n"
                      "            s_hold_mask = 0x0008;\n"
                      "            s_hold_left = kPressFrames;\n"
                      "        } else {\n"
                      "            update_buttons();\n"
                      "        }", 1)

    io.open(PATH, "w", encoding="utf-8", newline="\n").write(s)
    print("diag: auto press + rows counter")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
