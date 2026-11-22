// ---------------------------------------------------------------------------
//  audio_selftest.cpp - verification harness for the DAC/PPAM audio model
//
//  The game itself never touches the DAC (see dbgtest/audio/io_stats.txt), so
//  tour.wav is digital silence.  That makes it impossible to tell "model broken"
//  from "game silent" by listening, so this harness drives the SoC directly:
//  it pokes 0x3041 at known cadences, runs tick(), drains audio_read() and
//  checks the numbers, then round-trips a tone through audio_dump.cpp.
//
//  Build/run: powershell -File dbgtest/audio/run_audio_test.ps1
// ---------------------------------------------------------------------------
#include "gpce4.h"
#include "audio_dump.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

int g_pass = 0;
int g_fail = 0;

void check(bool ok, const char *what, const char *detail = "")
{
    if (ok) { g_pass++; std::printf("  ok   %s\n", what); }
    else    { g_fail++; std::printf("  FAIL %s %s\n", what, detail); }
}

// One emulated second in 2000-cycle slices, like the main loop does it.
void run_cycles(Gpce4 &soc, uint64_t cycles, uint32_t step = 2000)
{
    while (cycles) {
        const uint32_t n = (cycles < step) ? uint32_t(cycles) : step;
        soc.tick(n);
        cycles -= n;
    }
}

constexpr int RATE = Gpce4::AUDIO_RATE;

} // anonymous namespace

int main()
{
    std::printf("audio model selftest (48 kHz ring, DAC 0x3041)\n");

    // ---- 1. silence -------------------------------------------------------
    {
        Gpce4 soc;
        run_cycles(soc, 96000000);              // 1 emulated second, no writes
        std::vector<int16_t> buf(static_cast<size_t>(RATE));
        const size_t n = soc.audio_read(buf.data(), buf.size());
        bool quiet = true;
        for (int16_t v : buf) if (v) { quiet = false; break; }
        std::printf("[1] silence\n");
        check(n == size_t(RATE), "one emulated second == 48000 frames");
        check(soc.audio_frames_produced() == uint64_t(RATE), "frame edges land on the 48 kHz grid");
        check(quiet, "no DAC write -> all frames are 0");
    }

    // ---- 2. DC level (zero-order hold) -----------------------------------
    {
        Gpce4 soc;
        soc.write16(0x3041, 0x4000);            // +16384, twos complement
        run_cycles(soc, 96000000 / 10);         // 100 ms
        std::vector<int16_t> buf(static_cast<size_t>(RATE / 10 + 8));
        const size_t n = soc.audio_read(buf.data(), buf.size());
        bool flat = true;
        for (size_t i = 0; i < n; i++) if (buf[i] != 16384) { flat = false; break; }
        std::printf("[2] DC hold\n");
        check(n == size_t(RATE / 10), "100 ms == 4800 frames");
        check(flat, "held code 0x4000 -> every frame is +16384");

        soc.write16(0x3041, 0xC000);            // -16384
        run_cycles(soc, 9600000);
        soc.audio_read(buf.data(), buf.size());
        bool neg = true;
        for (size_t i = 0; i < size_t(RATE / 10); i++) if (buf[i] != -16384) { neg = false; break; }
        check(neg, "held code 0xC000 -> every frame is -16384 (twos complement)");
    }

    // ---- 3. 1 kHz tone, DAC updated at 20 kHz (timer_a cadence) ----------
    {
        Gpce4 soc;
        const int dac_rate = 20000;
        const int cycles_per_sample = 96000000 / dac_rate;   // 4800
        const double amp = 12000.0;
        const int seconds = 1;
        const int nsamples = dac_rate * seconds;

        std::vector<int16_t> buf(static_cast<size_t>(RATE * seconds));
        size_t got = 0;
        for (int i = 0; i < nsamples; i++) {
            const double ph = 2.0 * 3.14159265358979323846 * 1000.0 * double(i) / double(dac_rate);
            const int32_t code = int32_t(std::lround(amp * std::sin(ph))) & 0xffff;
            soc.write16(0x3041, uint16_t(code));
            run_cycles(soc, uint64_t(cycles_per_sample));
            if (got < buf.size()) {
                const size_t want = buf.size() - got;
                got += soc.audio_read(buf.data() + got, want);
            }
        }
        got += soc.audio_read(buf.data() + got, buf.size() - got);

        uint64_t peak = 0;
        double sumsq = 0.0;
        int crossings = 0;
        for (size_t i = 0; i < buf.size(); i++) {
            const int v = buf[i];
            peak = (uint64_t(std::abs(v)) > peak) ? uint64_t(std::abs(v)) : peak;
            sumsq += double(v) * double(v);
            if (i && ((buf[i - 1] < 0) != (v < 0))) crossings++;
        }
        const double rms = std::sqrt(sumsq / double(buf.size()));
        const double freq = double(crossings) / 2.0 / double(seconds);

        std::printf("[3] 1 kHz tone, 20 kHz DAC updates (offset %llu frames)\n",
                    (unsigned long long)got);
        check(got == size_t(RATE * seconds), "full second drained");
        check(peak > 11000 && peak <= 12100, "peak tracks the 12000 code");
        check(rms > 7800 && rms < 9200, "rms ~ 12000/sqrt(2) = 8485");
        check(freq > 970 && freq < 1030, "zero crossings -> 1 kHz");
        check(soc.audio_overruns() == 0, "no ring overrun while draining");
    }

    // ---- 4. read semantics ----------------------------------------------
    {
        Gpce4 soc;
        run_cycles(soc, 9600000);               // 100 ms -> 4800 frames
        std::vector<int16_t> buf(6000, int16_t(-1));
        const size_t n = soc.audio_read(buf.data(), buf.size());
        std::printf("[4] short read\n");
        check(n == 4800, "returns the number of real frames");
        check(buf[4799] == 0, "real frames written");
        check(buf[4800] == 0 && buf[5999] == 0, "remainder zero filled");
        check(soc.audio_underruns() == 1200, "underrun counted");

        soc.audio_reset();
        check(soc.audio_read(buf.data(), 16) == 0, "audio_reset() drops the history");
        run_cycles(soc, 9600000);
        check(soc.audio_frames_produced() == 4800, "audio_reset() restarts the frame grid");
    }

    // ---- 5. switch off / ring integrity ----------------------------------
    {
        Gpce4 soc;
        std::printf("[5] enable switch\n");
        soc.write16(0x3041, 0x1000);
        soc.audio_enabled = false;
        run_cycles(soc, 9600000);
        std::vector<int16_t> buf(64);
        check(soc.audio_read(buf.data(), buf.size()) == 0, "audio_enabled=false stops production");
        soc.audio_enabled = true;
        run_cycles(soc, 9600000);
        const size_t n = soc.audio_read(buf.data(), buf.size());
        check(n == 64 && buf[0] == 4096, "re-enabling resumes immediately with the held code");
    }

    // ---- 6. WAV round trip (proves audio_dump.cpp itself) ----------------
    {
        const char *path = "dbgtest/audio/_selftest_tone.wav";
        std::vector<int16_t> tone(4800);
        for (int i = 0; i < 4800; i++)
            tone[size_t(i)] = int16_t(std::lround(9000.0 * std::sin(2.0 * 3.14159265358979323846 * 440.0 * i / 48000.0)));

        const bool opened = audio_wav_open(path, 48000);
        check(opened, "audio_wav_open");
        audio_wav_write(tone.data(), 1000);     // split writes exercise the staging buffer
        audio_wav_write(tone.data() + 1000, 3800);
        check(audio_wav_frames() == 4800, "audio_wav_frames");
        audio_wav_close();
        check(!audio_wav_is_open(), "audio_wav_close");

        FILE *fp = std::fopen(path, "rb");
        bool hdr_ok = false;
        bool data_ok = false;
        if (fp) {
            unsigned char h[44];
            if (std::fread(h, 1, sizeof(h), fp) == sizeof(h)) {
                const uint32_t riff = uint32_t(h[4] | (h[5] << 8) | (h[6] << 16) | (uint32_t(h[7]) << 24));
                const uint32_t datasz = uint32_t(h[40] | (h[41] << 8) | (h[42] << 16) | (uint32_t(h[43]) << 24));
                const uint32_t rate = uint32_t(h[24] | (h[25] << 8) | (h[26] << 16) | (uint32_t(h[27]) << 24));
                hdr_ok = std::memcmp(h, "RIFF", 4) == 0 && std::memcmp(h + 8, "WAVEfmt ", 8) == 0 &&
                         std::memcmp(h + 36, "data", 4) == 0 && h[22] == 1 && h[34] == 16 &&
                         rate == 48000 && datasz == 9600 && riff == 36 + 9600;
            }
            std::vector<int16_t> back(4800);
            const size_t rd = std::fread(back.data(), 2, back.size(), fp);
            data_ok = rd == 4800 && std::memcmp(back.data(), tone.data(), 9600) == 0;
            std::fclose(fp);
            std::remove(path);
        }
        std::printf("[6] wav writer\n");
        check(hdr_ok, "RIFF/WAVE/fmt/data fields, 48 kHz mono s16, patched sizes");
        check(data_ok, "sample payload round trips byte for byte");
    }

    // ---- 7. throughput: the model's share of tick() ----------------------
    //  Informational only (no pass/fail): the wall clock of the whole emulator
    //  is dominated by the CPU core and by whatever else the box is running, so
    //  the interesting figure is the model's own cost inside tick().
    {
        const uint64_t secs = 10;
        const uint64_t total = 96000000ull * secs;
        double t[2] = { 0.0, 0.0 };
        for (int pass = 0; pass < 2; pass++) {
            Gpce4 soc;
            soc.write16(0x3041, 0x2000);              // keep the DAC live
            soc.audio_enabled = (pass == 1);
            const auto t0 = std::chrono::steady_clock::now();
            run_cycles(soc, total);
            const auto t1 = std::chrono::steady_clock::now();
            t[pass] = std::chrono::duration<double>(t1 - t0).count();
        }
        const double us_off = t[0] * 1e6 / double(secs);
        const double us_on  = t[1] * 1e6 / double(secs);
        std::printf("[7] throughput (tick() only, %llu emulated seconds each)\n",
                    (unsigned long long)secs);
        std::printf("     audio off : %8.1f us per emulated second\n", us_off);
        std::printf("     audio on  : %8.1f us per emulated second (+%.2f us, %.1f%% of tick cost)\n",
                    us_on, us_on - us_off, us_off > 0 ? 100.0 * (us_on - us_off) / us_off : 0.0);
        std::printf("     budget    : %.1f%% of one emulated second at 1.0x speed\n",
                    100.0 * us_on / 1e6);
        check(us_on - us_off < 200000.0, "model cost stays far below real time");
    }

    std::printf("=== %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
