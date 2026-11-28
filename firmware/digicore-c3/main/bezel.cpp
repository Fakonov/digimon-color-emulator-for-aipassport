// RETAIL (style C) shell for the DMC port.
//
// The sibling project (digimon-passport) builds the same shell out of LVGL
// objects: a body-coloured screen, a tiled A8 brick mask behind everything, a
// rounded "rim" object with a 1 px border, and a rounded "well" inside it.  This
// firmware has no LVGL and no framebuffer to spare, so the same four layers are
// resolved per pixel into one scanline band and pushed straight to the panel.
//
// The tile is copied byte for byte from that project (brick_a8.h); what is added
// here is the bevel: a stone face one pixel below a joint is shaded, one pixel
// above a joint is lit.  Without it a single-colour red wall reads as printed
// mosaic, because style C relies on the shell being a flat grey plate.
//
// Geometry and colours both come from measurements, not taste:
//   glass 192x192 at (64,24)   = the guest's 96x96 visible area at 2x
//   rim   204x204 at (58,18)   = glass + 6 px each side, radius 8
//   well  198x198 at (61,21)   = glass + 3 px each side, radius 4
//   mask  r=30                 = the panel's own rounded corners (BSP rule)
//   colours                    = sampled from a retail photo, see below

#include "bezel.h"

#include <cstdio>
#include <cstring>

#include "brick_a8.h"
#include "display.h"
#include "esp_log.h"

// One definition lives in menu.cpp (it includes font5x7.inc); including the
// table here too made a second 295-byte copy in .rodata (R8 audit AP-01).
extern const uint8_t kFont5x7[];

namespace {

const char *TAG = "bezel";

constexpr int kPanelW = 320;
constexpr int kPanelH = 240;

// RGB565, sampled from tmp/dmc_photos/zoom_device.png (the Ver.1 "Original
// Brown" shell).  The joint is deliberately darker than the sampled #723631:
// on a red plate the joints need the extra contrast the retail print gets from
// its own shading.
constexpr uint16_t kStoneFace  = 0x9269;   // #974E4A wall face
constexpr uint16_t kStoneLit   = 0xAACA;   // face +14%: stone top edge
constexpr uint16_t kStoneShade = 0x7A07;   // face -16%: stone bottom edge
constexpr uint16_t kMortar     = 0x6985;   // #6B302C joints
constexpr uint16_t kRimBody    = 0x7BAE;   // #7A7470 metal frame
constexpr uint16_t kRimHi      = 0xB595;   // #B7B1AF frame highlight
constexpr uint16_t kRecess     = 0x4A08;   // #4A4341 well behind the glass
constexpr uint16_t kHudInk     = 0xEF7C;   // #EDEDE6 battery readout

constexpr int kRimMargin  = 6;
constexpr int kWellMargin = 3;
constexpr int kRimR  = 8;
constexpr int kWellR = 4;
constexpr int kCornerR = 30;

constexpr int kRimX = BEZEL_GLASS_X - kRimMargin;
constexpr int kRimY = BEZEL_GLASS_Y - kRimMargin;
constexpr int kRimW = BEZEL_GLASS_W + 2 * kRimMargin;
constexpr int kRimH = BEZEL_GLASS_H + 2 * kRimMargin;
constexpr int kWellX = BEZEL_GLASS_X - kWellMargin;
constexpr int kWellY = BEZEL_GLASS_Y - kWellMargin;
constexpr int kWellW = BEZEL_GLASS_W + 2 * kWellMargin;
constexpr int kWellH = BEZEL_GLASS_H + 2 * kWellMargin;

constexpr int kHudPadX = 16;      // same margins as the reference plate
constexpr int kHudPadY = 4;
constexpr int kHudPx = 2;         // 5x7 face at 2x, like the settings card
constexpr int kHudAdv = 6 * kHudPx;

inline uint16_t be16(uint16_t c)
{
    return uint16_t((c >> 8) | (c << 8));      // the panel wants big endian
}

// One wall pixel.  The mask is tiled from (0,0); everything wraps, so the wall
// has no seam and needs no edge handling.
inline uint16_t stone_px(int x, int y)
{
    const int xx = x & (BRICK_A8_W - 1);
    const int yy = y & (BRICK_A8_H - 1);
    if (kBrickA8[yy * BRICK_A8_W + xx])
        return kMortar;
    const int up = (yy - 1) & (BRICK_A8_H - 1);
    const int dn = (yy + 1) & (BRICK_A8_H - 1);
    if (kBrickA8[up * BRICK_A8_W + xx])
        return kStoneLit;
    if (kBrickA8[dn * BRICK_A8_W + xx])
        return kStoneShade;
    return kStoneFace;
}

// Pixel-in-rounded-rectangle, integer only, same rule LVGL's rounded rects and
// the reference plate use (no anti-aliasing anywhere in this panel path).
bool inside_round(int x, int y, int x0, int y0, int w, int h, int r)
{
    const int lx = x - x0;
    const int ly = y - y0;
    if (lx < 0 || ly < 0 || lx >= w || ly >= h)
        return false;
    int dx = 0;
    int dy = 0;
    if (lx < r)
        dx = r - lx;
    else if (lx > w - 1 - r)
        dx = lx - (w - 1 - r);
    if (ly < r)
        dy = r - ly;
    else if (ly > h - 1 - r)
        dy = ly - (h - 1 - r);
    return dx * dx + dy * dy <= r * r;
}

// Visible span of one row under the 30 px corner mask.  Straight out of the BSP
// rounding rule: rows inside [r, h-r) are full width, corner rows inset by the
// integer amount that keeps (inset+1)^2 + edge^2 <= r^2.
void row_span(int y, int *x0, int *x1)
{
    if (y < kCornerR || y >= kPanelH - kCornerR) {
        const int edge = (y < kCornerR) ? (kCornerR - y)
                                        : (y - (kPanelH - 1 - kCornerR));
        int inset = 0;
        while ((inset + 1) * (inset + 1) + edge * edge <= kCornerR * kCornerR)
            inset++;
        *x0 = kCornerR - inset;
        *x1 = kPanelW - kCornerR + inset - 1;
    } else {
        *x0 = 0;
        *x1 = kPanelW - 1;
    }
}

inline uint16_t shell_px(int x, int y)
{
    // The glass is a hole in this painting: the blitter owns it, and writing
    // anything here would be overwritten by the next frame's dirty tiles only if
    // they happen to be dirty at all.
    if (x >= BEZEL_GLASS_X && x < BEZEL_GLASS_X + BEZEL_GLASS_W &&
        y >= BEZEL_GLASS_Y && y < BEZEL_GLASS_Y + BEZEL_GLASS_H)
        return 0;
    if (inside_round(x, y, kWellX, kWellY, kWellW, kWellH, kWellR))
        return kRecess;
    if (inside_round(x, y, kRimX, kRimY, kRimW, kRimH, kRimR)) {
        const bool edge = !inside_round(x, y, kRimX + 1, kRimY + 1,
                                        kRimW - 2, kRimH - 2, kRimR - 1);
        return edge ? kRimHi : kRimBody;
    }
    return stone_px(x, y);
}

}  // namespace

bool bezel_visible(int x, int y)
{
    if (x < 0 || y < 0 || x >= kPanelW || y >= kPanelH)
        return false;
    int x0 = 0;
    int x1 = 0;
    row_span(y, &x0, &x1);
    return x >= x0 && x <= x1;
}

void bezel_paint(uint8_t *strip, size_t strip_bytes, int batt_pct)
{
    if (strip == nullptr)
        return;
    int rows = int(strip_bytes / (size_t(kPanelW) * 2u));
    if (rows > 16)
        rows = 16;
    if (rows <= 0) {
        ESP_LOGE(TAG, "strip too small for one band: %u B", unsigned(strip_bytes));
        return;
    }
    uint16_t *buf = reinterpret_cast<uint16_t *>(strip);
    for (int y0 = 0; y0 < kPanelH; y0 += rows) {
        const int n = (y0 + rows <= kPanelH) ? rows : (kPanelH - y0);
        for (int r = 0; r < n; r++) {
            const int y = y0 + r;
            int x0 = 0;
            int x1 = kPanelW - 1;
            row_span(y, &x0, &x1);
            uint16_t *row = buf + r * kPanelW;
            for (int x = 0; x < kPanelW; x++) {
                const uint16_t c = (x >= x0 && x <= x1) ? shell_px(x, y) : 0;
                row[x] = be16(c);
            }
        }
        if (display_draw_bitmap(0, y0, kPanelW, n, strip) != ESP_OK) {
            ESP_LOGE(TAG, "shell band failed at y=%d", y0);
            return;
        }
    }
    bezel_draw_battery(strip, strip_bytes, batt_pct);
}

void bezel_draw_battery(uint8_t *strip, size_t strip_bytes, int batt_pct)
{
    if (strip == nullptr || batt_pct < 0)
        return;
    char txt[16];   // a full int plus %% needs 13 bytes, -Werror=format-truncation
    snprintf(txt, sizeof(txt), "%d%%", batt_pct);
    const int n = int(strlen(txt));
    if (n <= 0 || n > 4)
        return;
    const int tw = n * kHudAdv - kHudPx;      // last glyph carries no gap
    const int th = 7 * kHudPx;
    const int bx = kPanelW - kHudPadX - tw;
    const int by = kPanelH - kHudPadY - th;
    if (bx < 0 || by < 0 || size_t(tw) * size_t(th) * 2u > strip_bytes)
        return;
    uint16_t *px = reinterpret_cast<uint16_t *>(strip);
    for (int r = 0; r < th; r++) {
        const int y = by + r;
        uint16_t *row = px + r * tw;
        for (int i = 0; i < tw; i++) {
            const int x = bx + i;
            // The wall shows through wherever the ink does not: no black box.
            uint16_t c = bezel_visible(x, y) ? shell_px(x, y) : 0;
            const int ci = i / kHudAdv;
            const int col = (i - ci * kHudAdv) / kHudPx;
            const int gy = r / kHudPx;
            if (ci < n && col < 5) {
                const uint8_t *g = &kFont5x7[((unsigned char)txt[ci] - 32) * 5];
                if (g[col] & (1u << gy))
                    c = kHudInk;
            }
            row[i] = be16(c);
        }
    }
    if (display_draw_bitmap(bx, by, tw, th, strip) != ESP_OK)
        ESP_LOGW(TAG, "battery corner push failed");
}

void bezel_draw_message(uint8_t *strip, size_t strip_bytes,
                        const char *line1, const char *line2)
{
    if (strip == nullptr)
        return;
    const char *lines[2] = {line1, line2};
    // Two 14 px rows, centred on the glass: the card is the only thing on screen,
    // so it sits in the middle rather than tucked under the rim.
    const int tops[2] = {BEZEL_GLASS_Y + 96 - 18, BEZEL_GLASS_Y + 96 + 6};
    for (int pass = 0; pass < 2; pass++) {
        const char *t = lines[pass];
        if (t == nullptr || t[0] == 0)
            continue;
        const int n = int(std::strlen(t));
        const int tw = n * kHudAdv - kHudPx;
        const int th = 7 * kHudPx;
        if (tw <= 0 || tw > BEZEL_GLASS_W ||
            size_t(tw) * size_t(th) * 2u > strip_bytes)
            continue;
        const int bx = BEZEL_GLASS_X + (BEZEL_GLASS_W - tw) / 2;
        uint16_t *px = reinterpret_cast<uint16_t *>(strip);
        for (int r = 0; r < th; r++) {
            uint16_t *row = px + r * tw;
            for (int i = 0; i < tw; i++) {
                uint16_t c = 0;                 // the glass is black behind the card
                const int ci = i / kHudAdv;
                const int col = (i - ci * kHudAdv) / kHudPx;
                const int gy = r / kHudPx;
                if (ci < n && col < 5) {
                    const uint8_t *g = &kFont5x7[((unsigned char)t[ci] - 32) * 5];
                    if (g[col] & (1u << gy))
                        c = kHudInk;
                }
                row[i] = be16(c);
            }
        }
        if (display_draw_bitmap(bx, tops[pass], tw, th, strip) != ESP_OK)
            ESP_LOGW(TAG, "message push failed");
    }
}
