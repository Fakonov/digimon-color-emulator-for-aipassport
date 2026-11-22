#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ST7789P3 panel on the FoloToy AI Passport, landscape 320x240. */
esp_err_t display_init(void);

/* percent 0..100; 0 = backlight off. Safe before/after init. */
void display_backlight_set(int percent);

/* x/y are top-left, w/h are in pixels, end coordinates are exclusive.
 * data is RGB565 big-endian, exactly w*h*2 bytes.
 * Blocks until the SPI color transfer has completed. */
esp_err_t display_draw_bitmap(int x, int y, int w, int h, const void *data);

#ifdef __cplusplus
}
#endif
