// Settings menu: values, persistence and a 5x7 text renderer.
//
// Two settings for now, both of which the emulator itself acts on:
//   AUTO OFF  - how long the panel stays lit after the last key press.  16 s
//               matches the retail machine (its ROM writes System_Sleep about
//               17 s after the last input); 0 means never sleep.
//   BRIGHT    - backlight duty once the panel is lit.
//
// Everything is written to NVS namespace "digicore" as one-byte keys, and only
// when a value actually changes, so flash wear is a non-issue.
#include "menu.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "display.h"
#include "font5x7.inc"

static const char *TAG = "menu";

// AUTO OFF choices, seconds.  16 is the retail idle timeout; the rest exist so
// the emulator can outlive the machine, which is the case where the "machine is
// dark" hint matters.
static const int kAutoOffChoices[] = {16, 30, 60, 120, 0};
static const char *const kAutoOffNames[] = {"FOLLOW 16S", "30S", "60S", "2M", "NEVER"};
static const int kAutoOffCount = (int)(sizeof(kAutoOffChoices) / sizeof(kAutoOffChoices[0]));

static const int kBrightChoices[] = {20, 40, 60, 80, 100};
static const int kBrightCount = (int)(sizeof(kBrightChoices) / sizeof(kBrightChoices[0]));

static int s_auto_idx = 0;
static int s_bright_idx = 3;      // 80%, the value the firmware shipped with
static int s_sel = MENU_IDX_AUTO_OFF;
static bool s_open = false;
// The menu owns the panel through menu_present(), so it only has to be pushed
// again when something on it changed -- the values, or which row is selected.
static bool s_dirty = true;
static nvs_handle_t s_nvs;
static bool s_nvs_ok = false;

static void persist_u8(const char *key, uint8_t v)
{
    if (!s_nvs_ok)
        return;
    if (nvs_set_u8(s_nvs, key, v) == ESP_OK)
        nvs_commit(s_nvs);
}

void menu_load()
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // A partition left over from an older layout.  Nothing of value lives
        // there yet, so start clean rather than failing to boot.
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs init failed: %d, using defaults", int(err));
        return;
    }
    if (nvs_open("digicore", NVS_READWRITE, &s_nvs) != ESP_OK) {
        ESP_LOGW(TAG, "nvs open failed, using defaults");
        return;
    }
    s_nvs_ok = true;

    uint8_t v = 0;
    if (nvs_get_u8(s_nvs, "auto", &v) == ESP_OK && v < kAutoOffCount)
        s_auto_idx = v;
    if (nvs_get_u8(s_nvs, "bright", &v) == ESP_OK && v < kBrightCount)
        s_bright_idx = v;

    ESP_LOGI(TAG, "auto-off %s, bright %d%%",
             kAutoOffNames[s_auto_idx], kBrightChoices[s_bright_idx]);
}

void menu_open()
{
    s_open = true;
    s_sel = MENU_IDX_AUTO_OFF;
    s_dirty = true;
    ESP_LOGI(TAG, "open");
}

void menu_close()
{
    s_open = false;
    ESP_LOGI(TAG, "close");
}

bool menu_is_open() { return s_open; }

void menu_press_a()
{
    s_sel = (s_sel + 1) % MENU_ITEMS;
    s_dirty = true;
}

void menu_press_b()
{
    if (s_sel == MENU_IDX_AUTO_OFF) {
        s_auto_idx = (s_auto_idx + 1) % kAutoOffCount;
        s_dirty = true;
        persist_u8("auto", (uint8_t)s_auto_idx);
        ESP_LOGI(TAG, "auto-off -> %s", kAutoOffNames[s_auto_idx]);
    } else {
        s_bright_idx = (s_bright_idx + 1) % kBrightCount;
        s_dirty = true;
        persist_u8("bright", (uint8_t)s_bright_idx);
        ESP_LOGI(TAG, "bright -> %d%%", kBrightChoices[s_bright_idx]);
    }
}

void menu_press_c()
{
    menu_close();
}

int menu_auto_off_sec() { return kAutoOffChoices[s_auto_idx]; }
int menu_bright_pct()   { return kBrightChoices[s_bright_idx]; }

// ---------------------------------------------------------------- rendering

static void fill_rect(uint16_t *fb, int panel, int x, int y, int w, int h,
                      int x0, int y0, int cw, int ch, uint16_t color)
{
    for (int row = 0; row < h; ++row) {
        const int py = y + row;
        if (py < y0 || py >= y0 + ch)
            continue;
        for (int col = 0; col < w; ++col) {
            const int px = x + col;
            if (px < x0 || px >= x0 + cw)
                continue;
            fb[py * panel + px] = color;
        }
    }
}

static void draw_char(uint16_t *fb, int panel, int x, int y, char c,
                      int x0, int y0, int cw, int ch, uint16_t color)
{
    const int code = (unsigned char)c;
    if (code < 32 || code > 90)
        return;
    const uint8_t *g = &kFont5x7[(code - 32) * 5];
    for (int col = 0; col < 5; ++col) {
        const uint8_t bits = g[col];
        if (bits == 0)
            continue;
        const int px = x + col;
        if (px < x0 || px >= x0 + cw)
            continue;
        for (int row = 0; row < 7; ++row) {
            if ((bits & (1u << row)) == 0)
                continue;
            const int py = y + row;
            if (py < y0 || py >= y0 + ch)
                continue;
            fb[py * panel + px] = color;
        }
    }
}

static void draw_text(uint16_t *fb, int panel, int x, int y, const char *s,
                      int x0, int y0, int cw, int ch, uint16_t color)
{
    for (; *s != '\0'; ++s, x += 6)
        draw_char(fb, panel, x, y, *s, x0, y0, cw, ch, color);
}

void menu_paint(uint16_t *fb, int panel, int x0, int y0, int w, int h)
{
    const uint16_t bg = 0x0000;      // black
    const uint16_t fg = 0xFFFF;      // white
    const uint16_t dim = 0x8410;     // grey, for the help line

    fill_rect(fb, panel, x0, y0, w, h, x0, y0, w, h, bg);

    char line[32];
    int y = y0 + 8;

    draw_text(fb, panel, x0 + 34, y, "SETTINGS", x0, y0, w, h, fg);
    y += 16;

    // Item 0
    snprintf(line, sizeof(line), "%c AUTO OFF", s_sel == MENU_IDX_AUTO_OFF ? '>' : ' ');
    draw_text(fb, panel, x0 + 4, y, line, x0, y0, w, h, fg);
    snprintf(line, sizeof(line), "    %s", kAutoOffNames[s_auto_idx]);
    draw_text(fb, panel, x0 + 4, y + 8, line, x0, y0, w, h, fg);
    y += 26;

    // Item 1
    snprintf(line, sizeof(line), "%c BRIGHT", s_sel == MENU_IDX_BRIGHT ? '>' : ' ');
    draw_text(fb, panel, x0 + 4, y, line, x0, y0, w, h, fg);
    snprintf(line, sizeof(line), "    %d%%", kBrightChoices[s_bright_idx]);
    draw_text(fb, panel, x0 + 4, y + 8, line, x0, y0, w, h, fg);

    draw_text(fb, panel, x0 + 4, y0 + 82, "A NEXT  B CHANGE", x0, y0, w, h, dim);
    draw_text(fb, panel, x0 + 4, y0 + 94, "C EXIT", x0, y0, w, h, dim);
}

// --- direct-to-panel path -------------------------------------------------
// Same layout as menu_paint(), one text row at a time.  See menu.h for why the
// menu cannot live in fb[].
static uint16_t s_row[128];
// One panel-wide line for the banner that sits in the top margin, outside
// the guest window.  Not scaled: the margin is a 1:1 strip of the panel.
static uint16_t s_wide[320];

static void row_text(uint16_t *dst, int cw, int x, const char *s, int r, uint16_t color)
{
    for (; *s != '\0'; ++s, x += 6) {
        const int code = (unsigned char)*s;
        if (code < 32 || code > 90)
            continue;
        const uint8_t *g = &kFont5x7[(code - 32) * 5];
        for (int col = 0; col < 5; ++col) {
            if ((g[col] & (1u << r)) == 0)
                continue;
            const int px = x + col;
            if (px >= 0 && px < cw)
                dst[px] = color;
        }
    }
}

static void render_row(int y, int cw)
{
    const uint16_t fg = 0xFFFF;      // white
    const uint16_t dim = 0x8410;     // grey, for the help lines
    char line[32];

    for (int x = 0; x < cw; ++x)
        s_row[x] = 0x0000;

    int t = y - 8;
    if (t >= 0 && t < 7)
        row_text(s_row, cw, 34, "SETTINGS", t, fg);

    t = y - 24;
    if (t >= 0 && t < 7) {
        snprintf(line, sizeof(line), "%c AUTO OFF", s_sel == MENU_IDX_AUTO_OFF ? '>' : ' ');
        row_text(s_row, cw, 4, line, t, fg);
    }
    t = y - 32;
    if (t >= 0 && t < 7) {
        snprintf(line, sizeof(line), "    %s", kAutoOffNames[s_auto_idx]);
        row_text(s_row, cw, 4, line, t, fg);
    }

    t = y - 50;
    if (t >= 0 && t < 7) {
        snprintf(line, sizeof(line), "%c BRIGHT", s_sel == MENU_IDX_BRIGHT ? '>' : ' ');
        row_text(s_row, cw, 4, line, t, fg);
    }
    t = y - 58;
    if (t >= 0 && t < 7) {
        snprintf(line, sizeof(line), "    %d%%", kBrightChoices[s_bright_idx]);
        row_text(s_row, cw, 4, line, t, fg);
    }

    t = y - 82;
    if (t >= 0 && t < 7)
        row_text(s_row, cw, 4, "A NEXT  B CHANGE", t, dim);
    t = y - 94;
    if (t >= 0 && t < 7)
        row_text(s_row, cw, 4, "C EXIT", t, dim);
}

// One line of text in the panel's top margin, above the guest window.  Used to
// say "the machine would be dark by now" when the emulator's auto-off is set
// longer than the real 16 s.  It is 1:1, not scaled, and never overlaps fb[].
void menu_banner_present(uint8_t *strip, int width, const char *text)
{
    const int h = 8;
    int len = 0;
    for (const char *q = text; *q != '\0'; ++q)
        ++len;
    const int x0 = (width - len * 6 + 1) / 2;
    if (x0 < 0)
        return;

    uint8_t *p = strip;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < width; ++x)
            s_wide[x] = 0x0000;
        if (y < 7)
            row_text(s_wide, width, x0, text, y, 0xFFFF);
        for (int x = 0; x < width; ++x) {
            *p++ = (uint8_t)(s_wide[x] >> 8);
            *p++ = (uint8_t)(s_wide[x] & 0xFF);
        }
    }
    const esp_err_t err = display_draw_bitmap(0, 0, width, h, strip);
    if (err != ESP_OK)
        ESP_LOGE(TAG, "banner push failed: %d", int(err));
}

void menu_present(uint8_t *strip, int left, int top, int w, int h, int scale)
{
    if (!s_dirty)
        return;
    s_dirty = false;

    // 16 source rows become 32 output rows, which is exactly what the caller's
    // tile strip holds (w * scale * 2 bytes per output row).
    const int band_rows = 16;
    int bands = 0;

    for (int y0 = 0; y0 < h; y0 += band_rows) {
        const int rows = (h - y0 < band_rows) ? (h - y0) : band_rows;
        uint8_t *p = strip;

        for (int r = 0; r < rows; ++r) {
            render_row(y0 + r, w);
            for (int sy = 0; sy < scale; ++sy) {
                for (int x = 0; x < w; ++x) {
                    const uint16_t v = s_row[x];
                    for (int sx = 0; sx < scale; ++sx) {
                        *p++ = (uint8_t)(v >> 8);
                        *p++ = (uint8_t)(v & 0xFF);
                    }
                }
            }
        }

        const esp_err_t err =
            display_draw_bitmap(left, top + y0 * scale, w * scale, rows * scale, strip);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "panel push failed: %d", int(err));
            return;
        }
        ++bands;
    }
    ESP_LOGI(TAG, "pushed %d band(s), %dx%d", bands, w * scale, h * scale);
}
