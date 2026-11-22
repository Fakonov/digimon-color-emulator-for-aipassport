"""Draw the measured frame rate onto the black border of the panel.

The serial log says 60.00 fps, but that is my instrument, not the user's eyes.
This paints three 5x7 digits in the top-left corner of the border once a second
so the speed is visible on the device itself, with no cable attached.
"""
import io

PATH = "firmware/digicore-c3/main/app_main.cpp"

OVERLAY = r'''
// ---------------------------------------------------------------------------
// Frame-rate readout in the border.
//
// The guest owns the middle 192x192 square; the surrounding border is ours, so
// printing the instantaneous frame rate there costs nothing and lets anyone
// check the speed without a serial console.
// ---------------------------------------------------------------------------
static const uint8_t kFont3x5[10][5] = {
    {0x7, 0x5, 0x5, 0x5, 0x7},  // 0
    {0x2, 0x6, 0x2, 0x2, 0x7},  // 1
    {0x7, 0x1, 0x7, 0x4, 0x7},  // 2
    {0x7, 0x1, 0x7, 0x1, 0x7},  // 3
    {0x5, 0x5, 0x7, 0x1, 0x1},  // 4
    {0x7, 0x4, 0x7, 0x1, 0x7},  // 5
    {0x7, 0x4, 0x7, 0x5, 0x7},  // 6
    {0x7, 0x1, 0x1, 0x1, 0x1},  // 7
    {0x7, 0x5, 0x7, 0x5, 0x7},  // 8
    {0x7, 0x5, 0x7, 0x1, 0x7},  // 9
};

static void draw_fps_overlay(int fps)
{
    constexpr int kDigits = 3;
    constexpr int kCell = 4;             // 3 px glyph + 1 px gap
    constexpr int kPx = 2;               // scale: 2x2 per font pixel
    constexpr int kBoxW = kDigits * kCell * kPx;   // 24
    constexpr int kBoxH = 5 * kPx + 2;             // 12
    static uint8_t box[kBoxW * kBoxH * 2];

    if (!s_display_ok)
        return;

    memset(box, 0, sizeof(box));  // black background, RGB565 0x0000
    if (fps < 0)
        fps = 0;
    if (fps > 999)
        fps = 999;
    for (int d = 0; d < kDigits; d++) {
        const int div = (d == 0) ? 100 : (d == 1 ? 10 : 1);
        const int digit = (fps / div) % 10;
        const int x0 = d * kCell * kPx + kPx;
        for (int row = 0; row < 5; row++) {
            const uint8_t bits = kFont3x5[digit][row];
            for (int col = 0; col < 3; col++) {
                if (!(bits & (4 >> col)))
                    continue;
                for (int dy = 0; dy < kPx; dy++) {
                    for (int dx = 0; dx < kPx; dx++) {
                        const int px = x0 + col * kPx + dx;
                        const int py = kPx - 1 + row * kPx + dy;
                        const int off = (py * kBoxW + px) * 2;
                        // white in RGB565, stored little-endian like the blit path
                        box[off] = 0xFF;
                        box[off + 1] = 0xFF;
                    }
                }
            }
        }
    }
    if (display_draw_bitmap(2, 2, kBoxW, kBoxH, box) != ESP_OK)
        ESP_LOGW(TAG, "fps overlay failed");
}
'''

ANCHOR = "static void dump_frame_hex(void)"


def main():
    s = io.open(PATH, encoding="utf-8").read()
    if "draw_fps_overlay" in s:
        print("overlay already present")
        return 0
    if ANCHOR not in s:
        raise SystemExit("anchor missing")
    s = s.replace(ANCHOR, OVERLAY + "\n" + ANCHOR, 1)

    # Call it from the once-a-second log block, right before the log line.
    log_anchor = "            ESP_LOGI(TAG, \"frames=%u"
    if log_anchor not in s:
        raise SystemExit("log anchor missing")
    s = s.replace(log_anchor,
                  "            draw_fps_overlay(int(fps_x100 / 100));\n" + log_anchor, 1)
    io.open(PATH, "w", encoding="utf-8", newline="\n").write(s)
    print("fps overlay added")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
