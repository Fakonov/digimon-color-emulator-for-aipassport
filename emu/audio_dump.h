// ---------------------------------------------------------------------------
//  audio_dump.h - streaming PCM WAV writer for the headless tools
//
//  Deliberately independent of the SoC: it only knows about 16-bit mono frames
//  at a fixed rate, so the same three calls can be dropped into main.cpp,
//  player.cpp or a harness.  See docs/AUDIO.md.
//
//      audio_wav_open("out.wav", 48000);
//      audio_wav_write(frames, count);      // any number of calls, any size
//      audio_wav_close();
//
//  The file is written with a 44-byte header whose sizes are patched on close,
//  so the stream is playable even if the process is killed (close() is also
//  registered with atexit as a safety net - the header then covers everything
//  that actually reached the disk).
// ---------------------------------------------------------------------------
#pragma once
#include <cstdint>
#include <cstddef>

// Opens (or re-opens) `path` and writes the WAV header for `rate` Hz, 16-bit,
// mono.  Returns false if the file cannot be created.
bool audio_wav_open(const char *path, int rate);

// Appends `count` mono frames.  No-op when no file is open.
void audio_wav_write(const int16_t *frames, size_t count);

// Flushes, patches the RIFF/data sizes and closes.  No-op when already closed.
void audio_wav_close();

// Diagnostics (used by the tools and by docs/AUDIO.md's measurement runs).
bool     audio_wav_is_open();
uint64_t audio_wav_frames();     // frames accepted since open()
int      audio_wav_rate();

// Convenience wrapper for a per-frame host loop: drains everything the SoC has
// produced so far into the open file and returns the number of frames written.
// Declared with a forward declaration so this header stays free of gpce4.h.
//   audio_wav_pump(&soc);            // once per emulated frame, after tick()
struct Gpce4;
size_t audio_wav_pump(Gpce4 *soc);
