// ---------------------------------------------------------------------------
//  buttons.h - the three AI Passport keys (one resistor ladder on GPIO0)
// ---------------------------------------------------------------------------
#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Bit mask values returned by buttons_scan().
#define BTN_KEY0 0x01   // ~0 mV     -> PORTA bit 0x0008
#define BTN_KEY1 0x02   // ~300 mV   -> PORTA bit 0x0010
#define BTN_KEY2 0x04   // ~900 mV   -> PORTA bit 0x0020

// Configures ADC1 channel 0 (GPIO0) with curve-fitting calibration.  Falls
// back to a digital read of GPIO0 when the ADC is unavailable; a key is then
// reported as BTN_KEY1 only.
//
// buttons_init() also starts a 20 ms polling task.  The guest only looks at the
// port once per emulated frame, and a frame takes ~0.9 s on this board, so a
// 200 ms tap would otherwise fall completely between two polls.  The poller
// keeps the live state *and* a latch of every key that went down since the last
// time the frame loop asked.
esp_err_t buttons_init(void);

// One debounced poll (also refreshes the latch).  Returns a mask of BTN_KEY*
// (0 = every key released).
int buttons_scan(void);

// Current debounced state, without sampling the ADC again.
int buttons_state(void);

// Keys that went down since the previous call; clears them.  OR this with
// buttons_state() to give the guest a down frame even for a very short tap.
int buttons_take_latched(void);

// Last voltage seen on the ladder, in mV (-1 before the first successful read).
int buttons_last_mv(void);

#ifdef __cplusplus
}
#endif
