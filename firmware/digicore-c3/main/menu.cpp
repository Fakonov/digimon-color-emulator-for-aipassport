// Settings menu: values, persistence and a 2x-scaled 5x7 text renderer.
//
// The visual target is the digimon-passport settings panel: a 300x200 ivory
// card centred on the panel, a 2 px dark olive border with 8 px rounded
// corners, a bright yellow bar behind the selected row, labels flush left and
// values flush right.  That project builds its menu out of LVGL widgets; this
// one has nowhere near the RAM for LVGL, so the same look is drawn by hand and
// pushed straight to the panel.
//
// Settings, all of which the emulator itself acts on:
//   AUTO OFF   - how long the panel stays lit after the last key press.  16 s
//                matches the retail machine (its ROM writes System_Sleep about
//                17 s after the last input); 0 means never sleep.
//   BRIGHT     - backlight duty once the panel is lit.
//   VOLUME     - software amplitude for the emulator's own beeps.  The codec
//                register stays pinned at 0 dB (see audio.c), so this only
//                scales the square wave.
//   AUTO SAVE  - how often the guest state is snapshotted to flash.  Off by
//                default is not an option: the whole point of the feature is
//                that a power cut does not cost progress.
//   ABOUT      - read only, shows the firmware version.
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

#ifndef DIGICORE_VERSION
#define DIGICORE_VERSION "?"
#endif

static const char *TAG = "menu";

// AUTO OFF choices, seconds.  16 is the retail idle timeout; the rest exist so
// the emulator can outlive the machine, which is the case where the "machine is
// dark" hint matters.
static const int kAutoOffChoices[] = {16, 30, 60, 120, 0};
static const char *const kAutoOffNames[] = {"FOLLOW 16S", "30S", "60S", "2M", "NEVER"};
static const int kAutoOffCount = (int)(sizeof(kAutoOffChoices) / sizeof(kAutoOffChoices[0]));

static const int kBrightChoices[] = {20, 40, 60, 80, 100};
static const int kBrightCount = (int)(sizeof(kBrightChoices) / sizeof(kBrightChoices[0]));

static const int kVolumeChoices[] = {0, 20, 40, 60, 80, 100};
static const int kVolumeCount = (int)(sizeof(kVolumeChoices) / sizeof(kVolumeChoices[0]));

static const int kAutoSaveChoices[] = {60, 120, 300, 0};
static const char *const kAutoSaveNames[] = {"60S", "2M", "5M", "OFF"};
static const int kAutoSaveCount = (int)(sizeof(kAutoSaveChoices) / sizeof(kAutoSaveChoices[0]));

static int s_auto_idx = 0;
static int s_bright_idx = 3;      // 80%, the value the firmware shipped with
static int s_volume_idx = 1;      // 20%: quiet enough to be safe on a fresh flash
static int s_save_idx = 0;        // 60 s, the period the feature was asked for
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
    if (s_nvs_ok)
        return;             // app_main loads early for the backlight, guest_init again
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

    // Volume is deliberately reset on every flash.  A freshly written image
    // should never come up loud in someone's hands, so the stored level is
    // compared against the build stamp: a different binary means the volume goes
    // back to the 20% default while every other setting survives.
    char stamp[24];
    size_t stamp_len = sizeof(stamp);
    const bool fresh_build =
        nvs_get_str(s_nvs, "volbuild", stamp, &stamp_len) != ESP_OK ||
        strcmp(stamp, __DATE__ " " __TIME__) != 0;
    if (fresh_build) {
        s_volume_idx = 1;
        nvs_set_u8(s_nvs, "vol", (uint8_t)s_volume_idx);
        nvs_set_str(s_nvs, "volbuild", __DATE__ " " __TIME__);
        nvs_commit(s_nvs);
    }

    uint8_t v = 0;
    if (nvs_get_u8(s_nvs, "auto", &v) == ESP_OK && v < kAutoOffCount)
        s_auto_idx = v;
    if (nvs_get_u8(s_nvs, "bright", &v) == ESP_OK && v < kBrightCount)
        s_bright_idx = v;
    if (!fresh_build && nvs_get_u8(s_nvs, "vol", &v) == ESP_OK && v < kVolumeCount)
        s_volume_idx = v;
    if (nvs_get_u8(s_nvs, "save", &v) == ESP_OK && v < kAutoSaveCount)
        s_save_idx = v;

    ESP_LOGI(TAG, "auto-off %s, bright %d%%, volume %d%%, auto-save %s",
             kAutoOffNames[s_auto_idx], kBrightChoices[s_bright_idx],
             kVolumeChoices[s_volume_idx], kAutoSaveNames[s_save_idx]);
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
    s_dirty = true;
    switch (s_sel) {
    case MENU_IDX_AUTO_OFF:
        s_auto_idx = (s_auto_idx + 1) % kAutoOffCount;
        persist_u8("auto", (uint8_t)s_auto_idx);
        ESP_LOGI(TAG, "auto-off -> %s", kAutoOffNames[s_auto_idx]);
        break;
    case MENU_IDX_BRIGHT:
        s_bright_idx = (s_bright_idx + 1) % kBrightCount;
        persist_u8("bright", (uint8_t)s_bright_idx);
        ESP_LOGI(TAG, "bright -> %d%%", kBrightChoices[s_bright_idx]);
        break;
    case MENU_IDX_VOLUME:
        s_volume_idx = (s_volume_idx + 1) % kVolumeCount;
        persist_u8("vol", (uint8_t)s_volume_idx);
        ESP_LOGI(TAG, "volume -> %d%%", kVolumeChoices[s_volume_idx]);
        break;
    case MENU_IDX_AUTOSAVE:
        s_save_idx = (s_save_idx + 1) % kAutoSaveCount;
        persist_u8("save", (uint8_t)s_save_idx);
        ESP_LOGI(TAG, "auto-save -> %s", kAutoSaveNames[s_save_idx]);
        break;
    default:
        // ABOUT has nothing to change; pressing B just re-pushes the row so the
        // bar blinks, which is the same feedback the other rows give.
        break;
    }
}

void menu_press_c()
{
    menu_close();
}

int menu_auto_off_sec() { return kAutoOffChoices[s_auto_idx]; }
int menu_bright_pct()   { return kBrightChoices[s_bright_idx]; }
int menu_volume_pct()   { return kVolumeChoices[s_volume_idx]; }
int menu_autosave_sec() { return kAutoSaveChoices[s_save_idx]; }

// ---------------------------------------------------------------- rendering

// Everything below draws at 1:1 into a 320-wide scanline buffer.  Text is the
// 5x7 set blown up 2x, which comes out 10x14 with a 12 px advance -- close
// enough to the reference panel's Montserrat 14 that the two read the same.
static constexpr int kTextScale = 2;
static constexpr int kTextW = 5 * kTextScale;      // 10
static constexpr int kTextH = 7 * kTextScale;      // 14
static constexpr int kAdvance = 6 * kTextScale;    // 12

// Card geometry, copied from the reference menu: 300x200 centred, 2 px border,
// 8 px corners.  LVGL places children inside the parent's border+padding box,
// so the row origin works out to 10+2+3 laterally and 20+2+3 vertically; the
// rows themselves are 278x24 at a 30 px pitch (36..180 inside the card).
static constexpr int kCardX = 10;
static constexpr int kCardY = 20;
static constexpr int kCardW = 300;
static constexpr int kCardH = 200;
static constexpr int kCardR = 8;
static constexpr int kEdge = 2;
static constexpr int kPad = 3;
static constexpr int kRowX = kCardX + kEdge + kPad + 8;    // 23
static constexpr int kRowW = 278;
static constexpr int kRowH = 24;
static constexpr int kRowY0 = kCardY + kEdge + kPad + 36;  // 61
static constexpr int kRowPitch = 30;
static constexpr int kTitleY = kCardY + kEdge + kPad - 1;  // 24, TOP_MID offset -1
static constexpr int kVerY = kCardY + kEdge + kPad + 1;    // 26, TOP_RIGHT offset +1
static constexpr int kVerRight = kCardX + kCardW - kEdge - kPad - 6;  // 299
static constexpr int kLabelPad = 4;   // LV_ALIGN_LEFT_MID, 2 plus the row pad of 2
static constexpr int kValuePad = 4;   // LV_ALIGN_RIGHT_MID, -2 plus the row pad of 2

// Colour literals from the reference theme.  The reference hands 24-bit values
// to lv_color_hex(), which does the RGB565 conversion internally; here that
// conversion is done once, by hand, with the source value kept in the comment.
static constexpr uint16_t kPanelBg   = 0xFFDD;   // #FBF9F0 ivory card
static constexpr uint16_t kPanelEdge = 0x2984;   // #2C3324 dark olive border
static constexpr uint16_t kInk       = 0x2163;   // #252C1F body text
static constexpr uint16_t kSelBar    = 0xFEC5;   // #FFD928 yellow selection bar
static constexpr uint16_t kSelInk    = 0x2163;   // #252C1F -- the reference sets a bar-local colour but its labels override it, so selected rows render like any other
static constexpr uint16_t kMuted     = 0x5B0A;   // #5A6350 version tag, help text
static constexpr uint16_t kPage      = 0x0000;   // everything outside the card

// How far the left edge of a rounded row pulls in, per row from the corner.
// Integer approximations of r - sqrt(r^2 - (r-dy)^2); four rows are enough at
// this size because the arc is only a few pixels tall.
static const uint8_t kCardRound[8] = {8, 4, 3, 2, 1, 1, 0, 0};
static const uint8_t kRowRound[5] = {5, 2, 1, 0, 0};

static uint16_t s_row[320];
static uint16_t s_wide[320];
static const MenuBackdrop *s_bg = nullptr;

// Panel pixel -> guest pixel.  Anything outside the window is black, which is
// the same thing the panel shows there when the menu is closed.
static uint16_t backdrop_px(int x, int y)
{
    if (s_bg == nullptr)
        return kPage;
    const int dx = (x - s_bg->out_x) / s_bg->scale;
    const int dy = (y - s_bg->out_y) / s_bg->scale;
    if (dx < 0 || dy < 0 || dx >= s_bg->w || dy >= s_bg->h)
        return kPage;
    return s_bg->pixels[(s_bg->y + dy) * s_bg->stride + s_bg->x + dx];
}

// Writes one scanline of a 2x-scaled string.  `out_row` is 0..kTextH-1 within
// the glyph cell, so callers can render a string one line at a time without
// materialising the whole block.
static void text_scanline(uint16_t *dst, int width, int x, const char *s,
                          int out_row, uint16_t color)
{
    if (out_row < 0 || out_row >= kTextH)
        return;
    const int src_row = out_row / kTextScale;

    for (; *s != '\0'; ++s, x += kAdvance) {
        const int code = (unsigned char)*s;
        if (code < 32 || code > 90)
            continue;
        const uint8_t *g = &kFont5x7[(code - 32) * 5];
        for (int col = 0; col < 5; ++col) {
            if ((g[col] & (1u << src_row)) == 0)
                continue;
            for (int sx = 0; sx < kTextScale; ++sx) {
                const int px = x + col * kTextScale + sx;
                if (px >= 0 && px < width)
                    dst[px] = color;
            }
        }
    }
}

static int text_width(const char *s)
{
    int n = 0;
    for (; *s != '\0'; ++s)
        ++n;
    return n * kAdvance;
}

// Row span of the inset rounded rectangle that starts at `dy` rows from its
// top, given the corner table and the total height.  Returns false when the row
// is outside the shape.
static bool round_span(int dy, int h, const uint8_t *tab, int tabn, int *xoff)
{
    if (dy < 0 || dy >= h)
        return false;
    int d = dy;
    if (d > h - 1 - dy)
        d = h - 1 - dy;
    *xoff = (d < tabn) ? tab[d] : 0;
    return true;
}

// Fills the card, its border and the selected row's bar for one screen line.
static void card_line(int y)
{
    // The card floats over the game, not over a black page: sample the guest
    // framebuffer so the pixels the card does not cover keep showing it.
    for (int x = 0; x < 320; ++x)
        s_row[x] = backdrop_px(x, y);

    int inset = 0;
    if (!round_span(y - kCardY, kCardH, kCardRound, 8, &inset))
        return;

    const int x0 = kCardX + inset;
    const int x1 = kCardX + kCardW - 1 - inset;
    for (int x = x0; x <= x1; ++x)
        s_row[x] = kPanelBg;

    // Border: the top and bottom two lines are solid, the sides are vertical
    // strips, and the rounded rows are painted solid because at this size the
    // arc is narrower than the 2 px stroke.
    const bool horizontal = (y - kCardY) < kEdge || (kCardY + kCardH - 1 - y) < kEdge;
    if (horizontal) {
        for (int x = x0; x <= x1; ++x)
            s_row[x] = kPanelEdge;
    } else {
        for (int i = 0; i < kEdge; ++i) {
            s_row[x0 + i] = kPanelEdge;
            s_row[x1 - i] = kPanelEdge;
        }
    }

    // Selection bar.
    const int bar_y = kRowY0 + s_sel * kRowPitch;
    int row_inset = 0;
    if (round_span(y - bar_y, kRowH, kRowRound, 5, &row_inset) && !horizontal) {
        const int bx0 = kRowX + row_inset;
        const int bx1 = kRowX + kRowW - 1 - row_inset;
        for (int x = bx0; x <= bx1; ++x)
            s_row[x] = kSelBar;
    }
}

static void label_for(char *buf, size_t n, int item)
{
    switch (item) {
    case MENU_IDX_AUTO_OFF: snprintf(buf, n, "%s", kAutoOffNames[s_auto_idx]); break;
    case MENU_IDX_BRIGHT:   snprintf(buf, n, "%d%%", kBrightChoices[s_bright_idx]); break;
    case MENU_IDX_VOLUME:   snprintf(buf, n, "%d%%", kVolumeChoices[s_volume_idx]); break;
    case MENU_IDX_AUTOSAVE: snprintf(buf, n, "%s", kAutoSaveNames[s_save_idx]); break;
    default:                snprintf(buf, n, "%s", DIGICORE_VERSION); break;
    }
}

static const char *name_for(int item)
{
    switch (item) {
    case MENU_IDX_AUTO_OFF: return "AUTO OFF";
    case MENU_IDX_BRIGHT:   return "BRIGHT";
    case MENU_IDX_VOLUME:   return "VOLUME";
    case MENU_IDX_AUTOSAVE: return "AUTO SAVE";
    default:                return "VERSION";
    }
}

// Renders one screen line into s_row.
static void render_line(int y)
{
    card_line(y);

    // Title, centred on the card and 1 px above the content box top, matching
    // the reference panel's TOP_MID label.
    {
        const char *t = "SETTINGS";
        const int tx = kCardX + (kCardW - text_width(t)) / 2;
        text_scanline(s_row, 320, tx, t, y - kTitleY, kInk);
    }

    // Version tag, top right in the muted tone.
    {
        char tag[16];
        snprintf(tag, sizeof(tag), "%s", DIGICORE_VERSION);
        text_scanline(s_row, 320, kVerRight - text_width(tag), tag, y - kVerY, kMuted);
    }

    for (int i = 0; i < MENU_ITEMS; ++i) {
        const int row_y = kRowY0 + i * kRowPitch;
        const int ty = row_y + (kRowH - kTextH) / 2;
        // The reference tries to darken the selected row's text but its own
        // label styles override that, so both states render in kInk.
        const uint16_t ink = kInk;

        text_scanline(s_row, 320, kRowX + kLabelPad, name_for(i), y - ty, ink);

        char val[16];
        label_for(val, sizeof(val), i);
        text_scanline(s_row, 320, kRowX + kRowW - kValuePad - text_width(val), val,
                      y - ty, ink);
    }

    // The reference's first-level menu carries no help line and no separator,
    // so neither does this one: the card ends where it ends.
}

// One line of text in the panel's top margin, above the guest window.  Used to
// say "the machine would be dark by now" when the emulator's auto-off is set
// longer than the real 16 s.  It is 1:1, not scaled, and never overlaps fb[].
void menu_banner_present(uint8_t *strip, int width, const char *text)
{
    const int h = 8;
    const int x0 = (width - text_width(text)) / 2;
    if (x0 < 0)
        return;

    // The banner keeps the small 1:1 glyphs: it is a status line, not a menu.
    uint8_t *p = strip;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < width; ++x)
            s_wide[x] = 0x0000;
        if (y < 7) {
            int cx = x0;
            for (const char *s = text; *s != '\0'; ++s, cx += 6) {
                const int code = (unsigned char)*s;
                if (code < 32 || code > 90)
                    continue;
                const uint8_t *g = &kFont5x7[(code - 32) * 5];
                for (int col = 0; col < 5; ++col) {
                    const int px = cx + col;
                    if (px >= 0 && px < width && (g[col] & (1u << y)) != 0)
                        s_wide[px] = 0xFFFF;
                }
            }
        }
        for (int x = 0; x < width; ++x) {
            *p++ = (uint8_t)(s_wide[x] >> 8);
            *p++ = (uint8_t)(s_wide[x] & 0xFF);
        }
    }
    const esp_err_t err = display_draw_bitmap(0, 0, width, h, strip);
    if (err != ESP_OK)
        ESP_LOGE(TAG, "banner push failed: %d", int(err));
}

void menu_present(uint8_t *strip, int w, int h, const MenuBackdrop *bg)
{
    if (!s_dirty)
        return;
    s_dirty = false;
    s_bg = bg;

    // w * 2 bytes per line; the caller's strip holds 24 of them.
    const int band_rows = 24;
    int bands = 0;

    for (int y0 = 0; y0 < h; y0 += band_rows) {
        const int rows = (h - y0 < band_rows) ? (h - y0) : band_rows;
        uint8_t *p = strip;

        for (int r = 0; r < rows; ++r) {
            render_line(y0 + r);
            for (int x = 0; x < w; ++x) {
                *p++ = (uint8_t)(s_row[x] >> 8);
                *p++ = (uint8_t)(s_row[x] & 0xFF);
            }
        }

        const esp_err_t err = display_draw_bitmap(0, y0, w, rows, strip);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "panel push failed: %d", int(err));
            return;
        }
        ++bands;
    }
    ESP_LOGI(TAG, "pushed %d band(s), %dx%d", bands, w, h);
}
