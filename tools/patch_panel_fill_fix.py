"""Give the panel fill its own correctly sized buffer.

The first version reused the window-sized strip buffer while claiming to draw
320 pixels per row, so it read 6 KB past the end of a 9 KB buffer - that is
exactly the kind of thing that paints noise instead of black.  The fill now owns
a 320 * kChunkRows * 2 buffer, and the blit path reports its worst-case duration
so an SPI-bound frame is visible in the log instead of being guessed at.
"""
import io

PATH = "firmware/digicore-c3/main/app_main.cpp"

OLD_FILL = """static void panel_fill_black(void)
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
}"""

NEW_FILL = """static void panel_fill_black(void)
{
    // 320 pixels wide * kChunkRows rows * 2 bytes: the fill must own a buffer of
    // exactly this size, it is wider than the window strip below.
    static uint8_t fill[320 * kChunkRows * 2];
    if (!s_display_ok)
        return;
    memset(fill, 0, sizeof(fill));
    for (int y = 0; y < 240; y += kChunkRows) {
        const int rows = (y + kChunkRows <= 240) ? kChunkRows : (240 - y);
        if (display_draw_bitmap(0, y, 320, rows, fill) != ESP_OK) {
            ESP_LOGE(TAG, "panel fill failed at y=%d", y);
            return;
        }
    }
    ESP_LOGI(TAG, "panel cleared to black (320x240)");
}"""

OLD_BLIT = """        if (display_draw_bitmap(kLeft, kTop + y0, kWinW * kScale, rows, s_strip) != ESP_OK) {"""
NEW_BLIT = """        const int64_t t_blit = esp_timer_get_time();
        const esp_err_t err = display_draw_bitmap(kLeft, kTop + y0, kWinW * kScale, rows, s_strip);
        const int64_t dt_blit = esp_timer_get_time() - t_blit;
        if (dt_blit > s_blit_max_us)
            s_blit_max_us = uint32_t(dt_blit);
        if (err != ESP_OK) {"""


def main():
    s = io.open(PATH, encoding="utf-8").read()
    if "static uint8_t fill[320 * kChunkRows * 2];" in s:
        print("fill already sized")
        return 0
    if OLD_FILL not in s:
        raise SystemExit("fill anchor missing")
    s = s.replace(OLD_FILL, NEW_FILL, 1)
    if OLD_BLIT not in s:
        raise SystemExit("blit anchor missing")
    s = s.replace(OLD_BLIT, NEW_BLIT, 1)
    s = s.replace("static uint32_t s_blit_us = 0;",
                  "static uint32_t s_blit_us = 0;\nstatic uint32_t s_blit_max_us = 0;", 1)
    io.open(PATH, "w", encoding="utf-8", newline="\n").write(s)
    print("fill buffer fixed, blit max instrumented")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
