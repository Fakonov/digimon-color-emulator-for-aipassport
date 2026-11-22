// ---------------------------------------------------------------------------
//  audio_dump.cpp - see audio_dump.h
//
//  Buffered: frames are collected in a 4096-frame staging buffer and handed to
//  fwrite() in bulk, which keeps a per-sample producer (one DAC tick at 20 kHz,
//  one resampled frame at 48 kHz, ...) from turning into a syscall per frame.
// ---------------------------------------------------------------------------
#include "audio_dump.h"
#include "gpce4.h"          // audio_wav_pump() drains the SoC ring

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

FILE     *g_fp       = nullptr;
int       g_rate     = 48000;
uint64_t  g_frames   = 0;
bool      g_atexit_installed = false;
std::vector<int16_t> g_stage;

constexpr size_t STAGE_FRAMES = 4096;

void put16(uint8_t *p, uint32_t v) { p[0] = uint8_t(v & 0xff); p[1] = uint8_t((v >> 8) & 0xff); }
void put32(uint8_t *p, uint32_t v)
{
    p[0] = uint8_t(v & 0xff); p[1] = uint8_t((v >> 8) & 0xff);
    p[2] = uint8_t((v >> 16) & 0xff); p[3] = uint8_t((v >> 24) & 0xff);
}

// 16-bit mono PCM, the size fields filled in later by the same function.
void write_header(uint32_t data_bytes)
{
    uint8_t h[44] = {};
    std::memcpy(h + 0, "RIFF", 4);
    put32(h + 4, 36u + data_bytes);
    std::memcpy(h + 8, "WAVEfmt ", 8);
    put32(h + 16, 16);                                   // fmt chunk size
    put16(h + 20, 1);                                    // PCM
    put16(h + 22, 1);                                    // mono
    put32(h + 24, uint32_t(g_rate));
    put32(h + 28, uint32_t(g_rate) * 2);                 // byte rate
    put16(h + 32, 2);                                    // block align
    put16(h + 34, 16);                                   // bits per sample
    std::memcpy(h + 36, "data", 4);
    put32(h + 40, data_bytes);
    std::fseek(g_fp, 0, SEEK_SET);
    std::fwrite(h, 1, sizeof(h), g_fp);
    std::fseek(g_fp, 0, SEEK_END);
}

void flush_stage()
{
    if (g_fp && !g_stage.empty())
    {
        std::fwrite(g_stage.data(), sizeof(int16_t), g_stage.size(), g_fp);
        g_stage.clear();
    }
}

void atexit_close() { audio_wav_close(); }

} // anonymous namespace


bool audio_wav_open(const char *path, int rate)
{
    audio_wav_close();

    if (!path || !*path || rate <= 0)
        return false;

    g_fp = std::fopen(path, "wb");
    if (!g_fp)
        return false;

    g_rate = rate;
    g_frames = 0;
    g_stage.clear();
    write_header(0);                       // placeholder, patched by close()

    if (!g_atexit_installed)
    {
        std::atexit(atexit_close);
        g_atexit_installed = true;
    }
    return true;
}

void audio_wav_write(const int16_t *frames, size_t count)
{
    if (!g_fp || !frames || count == 0)
        return;

    size_t done = 0;
    while (done < count)
    {
        const size_t room = STAGE_FRAMES - g_stage.size();
        const size_t take = (count - done < room) ? (count - done) : room;
        g_stage.insert(g_stage.end(), frames + done, frames + done + take);
        done += take;
        if (g_stage.size() >= STAGE_FRAMES)
            flush_stage();
    }
    g_frames += count;
}

void audio_wav_close()
{
    if (!g_fp)
        return;

    flush_stage();
    std::fflush(g_fp);
    write_header(uint32_t(g_frames * 2));
    std::fclose(g_fp);
    g_fp = nullptr;
    g_stage.clear();
}

bool     audio_wav_is_open() { return g_fp != nullptr; }
uint64_t audio_wav_frames()  { return g_frames; }
int      audio_wav_rate()    { return g_rate; }

size_t audio_wav_pump(Gpce4 *soc)
{
    if (!soc || !g_fp)
        return 0;

    int16_t buf[512];
    size_t total = 0;
    size_t n;
    while ((n = soc->audio_read(buf, sizeof(buf) / sizeof(buf[0]))) > 0) {
        audio_wav_write(buf, n);
        total += n;
    }
    return total;
}
