// Minimal square-wave output for the AI Passport board (ES8311 + I2S0).
// See audio.c for why this is synthesis rather than emulation: the GPCE4 ROM
// in this cartridge never writes the DAC, so there is no audio stream to tap.

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Returns false when the codec is absent or the bus does not answer; callers
// must keep working silently in that case.
bool audio_init(void);

// 0..100, applied to both the DAC register and the software amplitude.
void audio_set_volume(int pct);

// Blocking square wave, then nothing.  Feed it short effects only.
void audio_beep(int freq, int ms);

// Blocking silence, used to flush the DMA queue so the next beep starts clean.
void audio_silence(int ms);

// The I2C0 bus this module opened, or NULL before audio_init() succeeds.
//
// The CW2017 fuel gauge sits on the same two pins (SDA 10 / SCL 7) as the
// ES8311 codec, so the battery module has to attach to this bus instead of
// opening its own: two masters driving the same pins is not a thing this board
// can survive.  Returned as void* to keep driver headers out of this one.
void *audio_i2c_bus(void);

#ifdef __cplusplus
}
#endif
