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

#ifdef __cplusplus
}
#endif
