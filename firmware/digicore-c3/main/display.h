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

/* Same geometry, but returns as soon as the transfer is queued: the DMA runs
 * while the caller keeps working.  The buffer stays owned by the driver until
 * the next display_* call returns, so the caller may only touch it after that.
 * One call per large region, not one per band: the io layer splits a single
 * tx_color() into 32 KiB bursts with CS held low, while every extra call waits
 * for spi_device_acquire_bus() and therefore serialises behind the previous
 * transfer. */
esp_err_t display_draw_bitmap_async(int x, int y, int w, int h, const void *data);

/* Block until any queued transfer has finished. */
void display_flush(void);

#ifdef __cplusplus
}
#endif
