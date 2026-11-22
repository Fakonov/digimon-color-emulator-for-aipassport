import io, re
p = r"firmware\digicore-c3\main\app_main.cpp"
s = io.open(p, encoding="utf-8").read()
start = s.index("static void blit_frame(void)")
end = s.index("\n}\n", start) + 3
new = '''static void blit_frame(void)
{
    const uint16_t *fb = s_soc.lcd.fb;
    // Tile hashes, not row hashes: a moving sprite only dirties the 16x16 tiles
    // it actually touches, so the SPI burst shrinks from the full 128x112 window
    // to a bounding box around the motion.  A row-level span still went out as
    // full-width rows, which is why every animation frame pushed the whole panel.
    static constexpr int kTile = 16;
    static constexpr int kTilesX = kWinW / kTile;
    static constexpr int kTilesY = kWinH / kTile;
    static uint32_t s_tile_hash[kTilesY][kTilesX];
    static bool s_have_prev = false;
    static uint32_t s_idle_frames = 0;

    int minx = kTilesX, miny = kTilesY, maxx = -1, maxy = -1;
    for (int ty = 0; ty < kTilesY; ty++) {
        for (int tx = 0; tx < kTilesX; tx++) {
            uint32_t h = 2166136261u;
            const uint16_t *p = fb + (kWinY + ty * kTile) * kFbPanel + kWinX + tx * kTile;
            for (int y = 0; y < kTile; y++) {
                for (int x = 0; x < kTile; x++) {
                    h ^= p[y * kFbPanel + x];
                    h *= 16777619u;
                }
            }
            if (!s_have_prev || h != s_tile_hash[ty][tx]) {
                s_tile_hash[ty][tx] = h;
                if (tx < minx) minx = tx;
                if (tx > maxx) maxx = tx;
                if (ty < miny) miny = ty;
                if (ty > maxy) maxy = ty;
            }
        }
    }

    if (maxx < 0) {
        // Nothing changed: the panel holds the picture, so write nothing.  One
        // full refresh every 120 frames repairs a transfer that was dropped.
        if (++s_idle_frames < 120) return;
        s_idle_frames = 0;
        s_have_prev = false;
        minx = 0; miny = 0; maxx = kTilesX - 1; maxy = kTilesY - 1;
    } else {
        s_idle_frames = 0;
    }

    const int wpx = (maxx - minx + 1) * kTile;
    const int hpx = (maxy - miny + 1) * kTile;
    const int rowb = wpx * kScale * 2;
    const int outx = kLeft + minx * kTile * kScale;
    const int outy0 = kTop + miny * kTile * kScale;

    esp_err_t err = ESP_OK;
    for (int base = 0; base < hpx; base += kChunkRows) {
        const int chunk = (hpx - base < kChunkRows) ? (hpx - base) : kChunkRows;
        for (int r = 0; r < chunk; r++) {
            const int y = miny * kTile + base + r;
            const uint16_t *src = fb + (kWinY + y) * kFbPanel + kWinX + minx * kTile;
            // Vertical scaling emits this source row kScale times; every output
            // row is exactly rowb bytes, which is what esp_lcd expects.
            for (int dy = 0; dy < kScale; dy++) {
                uint8_t *dst = s_strip + (r * kScale + dy) * rowb;
                if (kRot == 0) {
                    for (int x = 0; x < wpx; x++) {
                        const uint16_t c = src[x];
                        const uint16_t sw = (uint16_t)((c >> 8) | (c << 8));   // RGB565 big endian
                        const uint32_t pair = (uint32_t)sw | ((uint32_t)sw << 16);
                        *reinterpret_cast<uint32_t *>(dst + x * 4) = pair;
                    }
                } else {
                    for (int i = 0; i < wpx * kScale; i++) {
                        int sx, sy;
                        rot_map(minx * kTile + (i >> 1), y, &sx, &sy);
                        const uint16_t c = fb[(kWinY + sy) * kFbPanel + (kWinX + sx)];
                        dst[i * 2] = uint8_t(c >> 8);
                        dst[i * 2 + 1] = uint8_t(c & 0xff);
                    }
                }
            }
        }
        s_rows_sent += uint32_t(chunk * kScale);
        const int64_t t_blit = esp_timer_get_time();
        err = display_draw_bitmap(outx, outy0 + base * kScale,
                                  wpx * kScale, chunk * kScale, s_strip);
        const int64_t dt_blit = esp_timer_get_time() - t_blit;
        if (dt_blit > s_blit_max_us)
            s_blit_max_us = uint32_t(dt_blit);
        if (err != ESP_OK)
            break;
    }
    s_blits++;
    s_blits_total++;
    if (err != ESP_OK) {
        s_have_prev = false;
        s_blit_errs++;
        static bool warned = false;
        if (!warned) {
            warned = true;
            ESP_LOGE(TAG, "display_draw_bitmap failed at x=%d y=%d", outx, outy0);
        }
    }
}
'''
s = s[:start] + new + s[end:]
io.open(p, "w", encoding="utf-8", newline="").write(s)
print("ok", len(s))
