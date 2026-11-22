"""Rewrite blit_frame() in the C3 app: window-only transfers, dirty-row skip,
32-bit stores for the 2x horizontal expansion.

Run from the project root:  python tools/patch_blit.py
Idempotent: a second run finds the marker and does nothing.
"""
import io
import sys

P = "firmware/digicore-c3/main/app_main.cpp"

MARK = "// Push the 96x96 guest window to the panel."

NEW = r'''// Push the 96x96 guest window to the panel.
//
// Three things matter here, in order of how much time they used to cost:
//
//   * bytes on the wire - the window is 192x192 on a 320x240 panel, so the old
//     code spent 40% of every transfer on letterbox that never changes.  Now
//     only the 192-wide window is addressed.
//   * unchanged rows    - the panel keeps its content, so a strip whose source
//     rows did not change is skipped entirely.  Menus and the clock repaint a
//     few rows per frame, not all 192.
//   * per-pixel work    - kRot == 0 makes the mapping an identity copy with 2x
//     horizontal expansion, which is one 32-bit store per two pixels.
static void blit_frame(void)
{
    const uint16_t *fb = s_soc.lcd.fb;
    static uint16_t s_prev[kWinH * kWinW];
    static bool s_have_prev = false;

    for (int y0 = 0; y0 < kWinH; y0 += kChunkRows) {
        const int rows = (y0 + kChunkRows <= kWinH) ? kChunkRows : (kWinH - y0);

        bool dirty = !s_have_prev;
        for (int r = 0; r < rows && !dirty; r++) {
            const uint16_t *src = fb + (kWinY + y0 + r) * kFbPanel + kWinX;
            const uint16_t *old = s_prev + (y0 + r) * kWinW;
            for (int x = 0; x < kWinW; x++)
                if (src[x] != old[x]) { dirty = true; break; }
        }
        if (!dirty)
            continue;

        for (int r = 0; r < rows; r++) {
            const uint16_t *src = fb + (kWinY + y0 + r) * kFbPanel + kWinX;
            uint16_t *old = s_prev + (y0 + r) * kWinW;
            uint8_t *dst = s_strip + r * kWinW * kScale * 2;
            if (kRot == 0) {
                for (int x = 0; x < kWinW; x++) {
                    const uint16_t c = src[x];
                    old[x] = c;
                    const uint16_t sw = (uint16_t)((c >> 8) | (c << 8));   // RGB565 big endian
                    const uint32_t pair = (uint32_t)sw | ((uint32_t)sw << 16);
                    *reinterpret_cast<uint32_t *>(dst + x * 4) = pair;
                }
            } else {
                for (int i = 0; i < kWinW * kScale; i++) {
                    int sx, sy;
                    rot_map(i >> 1, y0 + r, &sx, &sy);
                    const uint16_t c = fb[(kWinY + sy) * kFbPanel + (kWinX + sx)];
                    old[i >> 1] = c;
                    dst[i * 2] = uint8_t(c >> 8);
                    dst[i * 2 + 1] = uint8_t(c & 0xff);
                }
            }
        }
        s_have_prev = true;

        if (display_draw_bitmap(kLeft, kTop + y0, kWinW * kScale, rows, s_strip) != ESP_OK) {
            static bool warned = false;
            if (!warned) {
                warned = true;
                ESP_LOGE(TAG, "display_draw_bitmap failed at y=%d", kTop + y0);
            }
        }
    }
}

'''


def main():
    s = io.open(P, encoding="utf-8").read()
    if MARK in s:
        print("blit_frame already patched")
        return 0

    start = s.find("// Compose the 96x96 guest window into 320-wide strips and push them out.")
    if start < 0:
        sys.exit("blit comment anchor not found")
    end = s.find("// ---------------------------------------------------------------------------\n//  input", start)
    if end < 0:
        sys.exit("input section anchor not found")

    s = s[:start] + NEW + s[end:]

    old_strip = "static uint8_t s_strip[kDstW * kChunkRows * 2] __attribute__((aligned(4)));"
    if old_strip in s:
        s = s.replace(
            old_strip,
            "static uint8_t s_strip[kWinW * kScale * kChunkRows * 2] __attribute__((aligned(4)));",
            1,
        )

    io.open(P, "w", encoding="utf-8", newline="\n").write(s)
    print("blit_frame rewritten in %s" % P)
    return 0


if __name__ == "__main__":
    sys.exit(main())
