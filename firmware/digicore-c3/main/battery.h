// CW2017 fuel gauge on the AI Passport board.
//
// The gauge hangs off the same I2C0 pins as the ES8311 codec (SDA 10 / SCL 7),
// so it attaches to the bus audio.c already opened rather than starting a second
// master on those pins.  audio_init() must have succeeded first.
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Attach to the shared bus and wake the gauge.  False when the bus is missing or
// the chip does not answer; callers keep working without a battery readout.
bool battery_init(void);

// State of charge, 0..100, or -1 when the gauge is not answering or is still
// computing its first value.
int battery_soc(void);

// Pack voltage in millivolts, or -1 on a bus error.  Diagnostic: 0 mV means the
// gauge sees no cell, which is the usual reason a readable part reports 0 %.
int battery_mv(void);

#ifdef __cplusplus
}
#endif
