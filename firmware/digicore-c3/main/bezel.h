#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* RETAIL (style C) shell, ported from the sibling digimon-passport firmware.
 *
 * Four layers painted straight to the panel, in this order:
 *   stone wall (A8 tile, recoloured, with a 1 px bevel read from the mask)
 *   rim        204x204 at (58,18), radius 8, 1 px highlight
 *   well       198x198 at (61,21), radius 4
 *   glass      BEZEL_GLASS_* - left alone, the blitter owns it
 * then the panel's own 30 px rounded-corner mask, then the battery corner.
 *
 * Geometry is fixed to the panel: the rim is part of the shell and does not
 * follow the debug window offset.  The glass is 192x192, not the reference
 * plate's 192x183, because that one is 32x16 dots at pitch 6 while this console
 * is a 96x96 colour LCD at pitch 2. */
#define BEZEL_GLASS_X 64
#define BEZEL_GLASS_Y 24
#define BEZEL_GLASS_W 192
#define BEZEL_GLASS_H 192

/* Whole shell, once per layout change (boot, settings card closing, E, G 0).
 * The glass window is written black, so the caller has to follow with a
 * full-window blit or the picture stays black.  batt_pct < 0 skips the corner. */
void bezel_paint(uint8_t *strip, size_t strip_bytes, int batt_pct);

/* Just the battery corner, for the 30 s refresh. */
void bezel_draw_battery(uint8_t *strip, size_t strip_bytes, int batt_pct);

/* Two centred lines inside the glass, black behind them.  The "no ROM" card has
 * to be readable before the engine exists, so it cannot go through the guest
 * framebuffer. */
void bezel_draw_message(uint8_t *strip, size_t strip_bytes,
                        const char *line1, const char *line2);

/* 30 px rounded-corner mask: false means the pixel is outside the visible area. */
bool bezel_visible(int x, int y);

#ifdef __cplusplus
}
#endif
