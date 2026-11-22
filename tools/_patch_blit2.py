import io
p = r"firmware\digicore-c3\main\app_main.cpp"
s = io.open(p, encoding="utf-8").read()
start = s.index("static void blit_frame(void)")
end = s.index("\n}\n", start) + 3
new = '''static void blit_frame(void)
{
    const uint16_t *fb = s_soc.lcd.fb;
    // Dirty tracking is per 16x16 tile and the output is sent one tile row at a
    // time.  A minimum bounding box was worse than the old row scan: the clock
    // ticks in the top-left tile and the sprite moves in the middle, so the box
    // covered the whole window on every frame (blit_pct 78, fps 39).  Per band
    // the burst is only as wide as the tiles that actually changed.
    static constexpr int kTile = 16;
    static constexpr int kTilesX = kWinW / kTile;
    static constexpr int kTilesY = kWinH / kTile;
    static uint32_t s_tile_hash[kTilesY][kTilesX];
    static bool s_have_prev = false;
    static uint32_t s_idle_frames = 0;

    bool dirty[kTilesY][kTilesX];
    bool any = false;
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
            dirty[ty][tx] = (!s_have_prev || h != s_tile_hash[ty][tx]);
            if (dirty[ty][tx]) {
                s_tile_hash[ty][tx] = h;
                any = true;
            }
        }
    }

    if (!any) {
        if (++s_idle_frames < 120) return;
        s_idle_frames = 0;
        s_have_prev = false;
        for (int ty = 0; ty < kTilesY; ty++)
            for (int tx = 0; tx < kTilesX; tx++)
                dirty[ty][tx] = true;
    } else {
        s_idle_frames = 0;
    }

    esp_err_t err = ESP_OK;
    for (int ty = 0; ty < kTilesY && err == ESP_OK; ty++) {
        int bminx = kTilesX, bmaxx = -1;
        for (int tx = 0; tx < kTilesX; tx++) {
            if (!dirty[ty][tx]) continue;
            if (tx < bminx) bminx = tx;
            bmaxx = tx;
        }
        if (bmaxx < 0) continue;

        const int wpx = (bmaxx - bminx + 1) * kTile;
        const int rowb = wpx * kScale * 2;
        const int outx = kLeft + bminx * kTile * kScale;
        const int outy = kTop + ty * kTile * kScale;

        for (int r = 0; r < kTile; r++) {
            const uint16_t *src = fb + (kWinY + ty * kTile + r) * kFbPanel + kWinX + bminx * kTile;
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
                        rot_map(bminx * kTile + (i >> 1), ty * kTile + r, &sx, &sy);
                        const uint16_t c = fb[(kWinY + sy) * kFbPanel + (kWinX + sx)];
                        dst[i * 2] = uint8_t(c >> 8);
                        dst[i * 2 + 1] = uint8_t(c & 0xff);
                    }
                }
            }
        }
        s_rows_sent += uint32_t(kTile * kScale);
        const int64_t t_blit = esp_timer_get_time();
        err = display_draw_bitmap(outx, outy, wpx * kScale, kTile * kScale, s_strip);
        const int64_t dt_blit = esp_timer_get_time() - t_blit;
        if (dt_blit > s_blit_max_us)
            s_blit_max_us = uint32_t(dt_blit);
        s_blits++;
        s_blits_total++;
    }
    if (err != ESP_OK) {
        s_have_prev = false;
        s_blit_errs++;
        static bool warned = false;
        if (!warned) {
            warned = true;
            ESP_LOGE(TAG, "display_draw_bitmap failed in band blit");
        }
    }
}
'''
s = s[:start] + new + s[end:]
s = s.replace("static constexpr int kChunkRows = 12;", "static constexpr int kChunkRows = 16;")
io.open(p, "w", encoding="utf-8", newline="").write(s)
print("ok")
