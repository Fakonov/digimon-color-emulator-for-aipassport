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

// ROM buzzer (GPCE4 TimerB pitch + PWM0 gate, mask ROM 0x55A9).
//
// The emulator does not produce a DAC stream: the cartridge drives IOA0 through
// the SoC's IO_PWM block, with TimerB setting the pitch.  The frame loop calls
// this once per frame with the current note; 0 means silent.  The audio task
// synthesises a 50 % square wave at that frequency and streams it to the codec,
// so what comes out of the speaker is the game's own tone, not a canned beep.
void audio_set_buzzer(int hz);
bool audio_buzzer_active(void);

// Diagnostics for the console `p` command: current note, how many I2S blocks the
// buzzer wrote, codec power state and the software volume.
void audio_debug(int *hz, unsigned *blocks, int *codec_on, int *volume);

// Blocking silence, used to flush the DMA queue so the next beep starts clean.
void audio_silence(int ms);

// The I2C0 bus this module opened, or NULL before audio_init() succeeds.
//
// The CW2017 fuel gauge sits on the same two pins (SDA 10 / SCL 7) as the
// ES8311 codec, so the battery module has to attach to this bus instead of
// opening its own: two masters driving the same pins is not a thing this board
// can survive.  Returned as void* to keep driver headers out of this one.
void *audio_i2c_bus(void);

// Idle power-down.  The codec and the I2S clock run from boot otherwise: MCLK
// stays at 12.288 MHz, the ES8311 keeps its analog rails up and the I2S DMA
// streams silence for as long as the handset is on, which is the largest
// standing load the firmware can switch off without changing how the machine
// plays.  A key press is the only thing that makes sound on this board, so the
// caller polls this from the frame loop; after AUDIO_IDLE_OFF_US without a beep
// the codec is powered down, and the next beep brings it back (the audio task
// does that itself, off the main loop, in ~2 ms of I2C).
void audio_idle_poll(void);

#ifdef __cplusplus
}
#endif
