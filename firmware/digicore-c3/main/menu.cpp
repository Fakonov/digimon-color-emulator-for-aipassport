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
#include "esp_timer.h"
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
static int s_action = MENU_ACTION_NONE;   // SAVE OUT / SAVE IN, drained by app_main
static bool s_open = false;
// The menu owns the panel through menu_present(), so it only has to be pushed
// again when something on it changed -- the values, or which row is selected.
static bool s_dirty = true;
static int s_capture = 0;   // non-zero: dump the next menu pushes to the console
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
    // Row 0 is the A+C chord and it is the reason most people open this card, so
    // the cursor starts there.  The value rows are one A press away.
    s_sel = MENU_IDX_CHORD;
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
    case MENU_IDX_CHORD:
        // Rows 0 and 1 are the chords themselves.  The menu only queues them:
        // app_main owns the injection timer, and it closes the card on the way
        // out so the user is looking at the game when the chord lands.
        s_action = MENU_ACTION_CHORD;
        ESP_LOGI(TAG, "A+C chord requested");
        break;
    case MENU_IDX_CHORD_AB:
        s_action = MENU_ACTION_CHORD_AB;
        ESP_LOGI(TAG, "A+B chord requested");
        break;
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
    case MENU_IDX_SAVE_OUT:
        s_action = MENU_ACTION_SAVE;
        ESP_LOGI(TAG, "save now requested");
        break;
    case MENU_IDX_SAVE_IN:
        s_action = MENU_ACTION_LOAD;
        ESP_LOGI(TAG, "load last requested");
        break;
    default:
        // ABOUT has nothing to change; pressing B just re-pushes the row so the
        // bar blinks, which is the same feedback the other rows give.
        break;
    }
}

int menu_take_action()
{
    const int a = s_action;
    s_action = MENU_ACTION_NONE;
    return a;
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
static constexpr int kCardY = 16;
static constexpr int kCardW = 300;
static constexpr int kCardH = 212;
static constexpr int kCardR = 8;
static constexpr int kEdge = 2;
static constexpr int kPad = 3;
static constexpr int kRowX = kCardX + kEdge + kPad + 8;    // 23
static constexpr int kRowW = 278;
// Nine rows now that both chords are in the card (A+C and A+B), and the 20/22
// that fitted eight does not: 45 + 8*22 + 20 = 241 against a card bottom of 228.
// 18 px rows at a 20 px pitch put the last row's bottom at 223, five px clear of
// the border, and still leave the 14 px glyphs 2 px of air above and below.
static constexpr int kRowH = 18;
static constexpr int kRowY0 = kCardY + kEdge + kPad + 24;  // 45; nine 18 px rows, 20 px apart
static constexpr int kRowPitch = 20;                       // 45 + 8*20 + 18 = 223, inside the 212 px card
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

// The reference port lets the game show through its settings card, so the card
// is a 90 % wash of kPanelBg over the guest pixels instead of a flat fill, plus
// a 12 px 40 % black drop shadow.  Both are 565-space blends; the alpha is
// 0..256 so that 256 is a plain copy and no rounding is needed at the top end.
static constexpr int kShadowPx = 12;
static constexpr int kCardAlpha = 230;    // 90 %, exactly what the reference panel sets (LV_OPA_90)
static constexpr int kShadowAlpha = 102;  // 40 %

static inline uint16_t blend565(uint16_t dst, uint16_t src, int a)
{
    const int dr = (dst >> 11) & 0x1F, dg = (dst >> 5) & 0x3F, db = dst & 0x1F;
    const int sr = (src >> 11) & 0x1F, sg = (src >> 5) & 0x3F, sb = src & 0x1F;
    const int r = dr + (((sr - dr) * a) >> 8);
    const int g = dg + (((sg - dg) * a) >> 8);
    const int b = db + (((sb - db) * a) >> 8);
    return uint16_t((r << 11) | (g << 5) | b);
}

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

// Proportional spacing.  The 5x7 cell is only a cell: most glyphs leave one or
// both edge columns blank, and the reference panel closes that up so "AUTO OFF"
// stops looking like a punched card.  Span is the ink columns of one glyph,
// advance is that span plus a single scaled column of tracking.
static void glyph_span(unsigned char c, int *first, int *last)
{
    *first = 0;
    *last = 4;
    if (c < 32 || c > 90)
        return;
    const uint8_t *g = &kFont5x7[(c - 32) * 5];
    int f = 5, l = -1;
    for (int col = 0; col < 5; ++col) {
        if (g[col] != 0) {
            if (f == 5)
                f = col;
            l = col;
        }
    }
    if (l >= 0) {
        *first = f;
        *last = l;
    }
}

static int glyph_advance(unsigned char c)
{
    if (c == ' ')
        return kTextScale * 3;   // 6 px word gap
    int f, l;
    glyph_span(c, &f, &l);
    return (l - f + 1) * kTextScale + kTextScale;
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

    for (; *s != '\0'; ++s) {
        const int code = (unsigned char)*s;
        const int advance = glyph_advance((unsigned char)code);
        if (code >= 32 && code <= 90) {
            int first, last;
            glyph_span((unsigned char)code, &first, &last);
            const uint8_t *g = &kFont5x7[(code - 32) * 5];
            for (int col = first; col <= last; ++col) {
                if ((g[col] & (1u << src_row)) == 0)
                    continue;
                for (int sx = 0; sx < kTextScale; ++sx) {
                    const int px = x + (col - first) * kTextScale + sx;
                    if (px >= 0 && px < width)
                        dst[px] = color;
                }
            }
        }
        x += advance;
    }
}

static int text_width(const char *s)
{
    int w = 0;
    for (; *s != '\0'; ++s)
        w += glyph_advance((unsigned char)*s);
    return w;
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

    // Drop shadow first, so the card itself is blended on top of it.  The halo
    // is a plain rectangle; at this radius the rounded corners do not matter.
    if (y >= kCardY - kShadowPx && y < kCardY + kCardH + kShadowPx) {
        const int sx0 = kCardX - kShadowPx;
        const int sx1 = kCardX + kCardW - 1 + kShadowPx;
        for (int x = sx0; x <= sx1; ++x) {
            if (x < 0 || x >= 320)
                continue;
            if (x >= kCardX && x < kCardX + kCardW && y >= kCardY &&
                y < kCardY + kCardH)
                continue;  // inside the card: the wash covers it
            s_row[x] = blend565(s_row[x], 0x0000, kShadowAlpha);
        }
    }

    int inset = 0;
    if (!round_span(y - kCardY, kCardH, kCardRound, 8, &inset))
        return;

    const int x0 = kCardX + inset;
    const int x1 = kCardX + kCardW - 1 - inset;
    for (int x = x0; x <= x1; ++x)
        s_row[x] = blend565(s_row[x], kPanelBg, kCardAlpha);

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
    // Command rows: the name already says what it does, so the value column
    // stays empty rather than repeating it.
    case MENU_IDX_CHORD:    buf[0] = '\0'; break;
    case MENU_IDX_CHORD_AB: buf[0] = '\0'; break;
    case MENU_IDX_AUTO_OFF: snprintf(buf, n, "%s", kAutoOffNames[s_auto_idx]); break;
    case MENU_IDX_BRIGHT:   snprintf(buf, n, "%d%%", kBrightChoices[s_bright_idx]); break;
    case MENU_IDX_VOLUME:   snprintf(buf, n, "%d%%", kVolumeChoices[s_volume_idx]); break;
    case MENU_IDX_AUTOSAVE: snprintf(buf, n, "%s", kAutoSaveNames[s_save_idx]); break;
    case MENU_IDX_SAVE_OUT: snprintf(buf, n, "%s", "SAVE NOW"); break;
    case MENU_IDX_SAVE_IN:  snprintf(buf, n, "%s", "LOAD LAST"); break;
    default:                snprintf(buf, n, "%s", DIGICORE_VERSION); break;
    }
}

static const char *name_for(int item)
{
    switch (item) {
    case MENU_IDX_CHORD:    return "SEND A+C";
    case MENU_IDX_CHORD_AB: return "SEND A+B";
    case MENU_IDX_AUTO_OFF: return "AUTO OFF";
    case MENU_IDX_BRIGHT:   return "BRIGHT";
    case MENU_IDX_VOLUME:   return "VOLUME";
    case MENU_IDX_AUTOSAVE: return "AUTO SAVE";
    case MENU_IDX_SAVE_OUT: return "SAVE OUT";
    case MENU_IDX_SAVE_IN:  return "SAVE IN";
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
    const int64_t t_start = esp_timer_get_time();
    int64_t t_render_us = 0;

    // w * 2 bytes per line; the caller's strip holds 24 of them.
    const int band_rows = 24;
    int bands = 0;

    for (int y0 = 0; y0 < h; y0 += band_rows) {
        const int rows = (h - y0 < band_rows) ? (h - y0) : band_rows;
        uint8_t *p = strip;

        const int64_t t_render0 = esp_timer_get_time();
        for (int r = 0; r < rows; ++r) {
            render_line(y0 + r);
            for (int x = 0; x < w; ++x) {
                *p++ = (uint8_t)(s_row[x] >> 8);
                *p++ = (uint8_t)(s_row[x] & 0xFF);
            }

            // Serial capture: the menu never writes fb[], so this is the only
            // way to see what the panel actually received without a camera.
            // Reads s_row and not strip, because display_draw_bitmap() owns the
            // caller's buffer once it has been handed over.  Every other row
            // and every other pixel, so a whole frame is ~7 s at 115200 baud.
            if (s_capture > 0 && (r & 1) == 0) {
                char out[16 + 160 * 4 + 2];
                int n = snprintf(out, sizeof(out), "B %d %d ", bands, y0 + r);
                for (int x = 0; x < w; x += 2)
                    n += snprintf(out + n, sizeof(out) - size_t(n), "%04X", s_row[x]);
                printf("%s\n", out);
            }
        }
        t_render_us += esp_timer_get_time() - t_render0;

        const esp_err_t err = display_draw_bitmap(0, y0, w, rows, strip);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "panel push failed: %d", int(err));
            return;
        }
        ++bands;

        if (s_capture > 0 && --s_capture == 0)
            printf("CAPTURE DONE\n");
    }
    // Split the cost: the render figure is the translucent compositing (guest
    // sample + drop shadow + 90 % wash + text) for all 240 lines, and the total
    // minus it is the SPI push.  This only runs when the menu is dirty, i.e. on
    // open and on a key, so the steady-state cost is zero either way - but the
    // number is what decides whether a 90 % wash is affordable.
    const int64_t total_us = esp_timer_get_time() - t_start;
    ESP_LOGI(TAG, "pushed %d band(s), %dx%d: render %d ms, total %d ms",
             bands, w, h, int(t_render_us / 1000), int(total_us / 1000));
}

void menu_set_capture(int frames)
{
    s_capture = (frames > 0) ? frames : 0;
}


// ===========================================================================
//  A+C confirmation card
//
//  The chord used to be injected the moment A had been held for 1.2 s, which
//  broke the handset outright: the cartridge's title screen only accepts a
//  plain A, so a pad reading A+C looked like no key at all and the game never
//  started.  The chord is now a two step gesture instead - long A raises this
//  card and the guest receives nothing at all while it is up, B sends A+C, A or
//  C drops it.
// ===========================================================================
static constexpr int kCfmX = 44;
static constexpr int kCfmY = 72;
static constexpr int kCfmW = 232;
static constexpr int kCfmH = 96;

static bool s_confirm = false;
static bool s_confirm_dirty = true;

static void confirm_line(int y)
{
    for (int x = 0; x < 320; ++x)
        s_row[x] = backdrop_px(x, y);

    if (y >= kCfmY - kShadowPx && y < kCfmY + kCfmH + kShadowPx) {
        const int sx0 = kCfmX - kShadowPx;
        const int sx1 = kCfmX + kCfmW - 1 + kShadowPx;
        for (int x = sx0; x <= sx1; ++x) {
            if (x < 0 || x >= 320)
                continue;
            if (x >= kCfmX && x < kCfmX + kCfmW && y >= kCfmY && y < kCfmY + kCfmH)
                continue;   // inside the card: the wash covers it
            s_row[x] = blend565(s_row[x], 0x0000, kShadowAlpha);
        }
    }

    int inset = 0;
    if (!round_span(y - kCfmY, kCfmH, kCardRound, 8, &inset))
        return;

    const int x0 = kCfmX + inset;
    const int x1 = kCfmX + kCfmW - 1 - inset;
    for (int x = x0; x <= x1; ++x)
        s_row[x] = blend565(s_row[x], kPanelBg, kCardAlpha);

    const bool horizontal = (y - kCfmY) < kEdge || (kCfmY + kCfmH - 1 - y) < kEdge;
    if (horizontal) {
        for (int x = x0; x <= x1; ++x)
            s_row[x] = kPanelEdge;
    } else {
        for (int i = 0; i < kEdge; ++i) {
            s_row[x0 + i] = kPanelEdge;
            s_row[x1 - i] = kPanelEdge;
        }
    }

    const char *title = "SEND A+C?";
    const int ty = kCfmY + 16;
    if (y >= ty && y < ty + kTextH)
        text_scanline(s_row, 320, (320 - text_width(title)) / 2, title, y - ty, kInk);

    const char *hint = "B=YES  C=NO";
    const int hy = kCfmY + 56;
    if (y >= hy && y < hy + kTextH)
        text_scanline(s_row, 320, (320 - text_width(hint)) / 2, hint, y - hy, kMuted);
}

void menu_confirm_set(bool on)
{
    s_confirm = on;
    s_confirm_dirty = true;
}

bool menu_confirm_is_open()
{
    return s_confirm;
}

void menu_confirm_present(uint8_t *strip, int w, int h, const MenuBackdrop *bg)
{
    if (!s_confirm || !s_confirm_dirty)
        return;
    s_confirm_dirty = false;
    s_bg = bg;

    const int band_rows = 24;
    int bands = 0;
    for (int y0 = 0; y0 < h; y0 += band_rows) {
        const int rows = (h - y0 < band_rows) ? (h - y0) : band_rows;
        uint8_t *p = strip;
        for (int r = 0; r < rows; ++r) {
            confirm_line(y0 + r);
            for (int x = 0; x < w; ++x) {
                *p++ = (uint8_t)(s_row[x] >> 8);
                *p++ = (uint8_t)(s_row[x] & 0xFF);
            }
            // Same console capture as the settings card: this overlay never
            // touches fb[], so the serial dump is the only way to see it.
            if (s_capture > 0 && (r & 1) == 0) {
                char out[16 + 160 * 4 + 2];
                int n = snprintf(out, sizeof(out), "C %d %d ", bands, y0 + r);
                for (int x = 0; x < w; x += 2)
                    n += snprintf(out + n, sizeof(out) - size_t(n), "%04X", s_row[x]);
                printf("%s\n", out);
            }
        }
        const esp_err_t err = display_draw_bitmap(0, y0, w, rows, strip);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "chord card push failed: %d", int(err));
            return;
        }
        ++bands;
        if (s_capture > 0 && --s_capture == 0)
            printf("CAPTURE DONE\n");
    }
}
