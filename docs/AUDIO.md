# PPAM / DAC audio (GPCE4) — what the hardware does, what the game does, what the emulator models

Status: model + WAV export implemented and verified; **the game itself is silent in every
scene tested so far** and the evidence for that is below. Nothing here touches `emu/main.cpp`
or `emu/player.cpp`.

---

## 1. The hardware block (reference: MAME 0.289 `ref/generalplus_gpce4_soc.cpp`)

| addr | MAME name | MAME handler | what MAME does with it |
|---|---|---|---|
| `0x3040` | `DAC_Ctrl` | `:608 dac_ctrl_w` | `logerror` only — no state |
| `0x3041` | `DAC_CH1_Data` | `:613 dac_cha1_data_w` | `m_dac->data_w(data)` → **the only register that drives a device** |
| `0x3042` | `DAC_CH2_Data` | `:620 dac_cha2_data_w` | `logerror` only |
| `0x3043` | `PPAMCtrl` | `:625 ppam_ctrl_w` | `logerror` only |

The device is `DAC_16BIT_R2R_TWOS_COMPLEMENT(config, "dac", 0)` (`:147`), i.e. a 16-bit
two's-complement R2R ladder: the 16-bit word written to `0x3041` **is** the sample, and it is
held until the next write (there is no internal rate — the rate is whatever the software
writes at). A second, nominally more interesting path exists on paper: `timer_a` is a
divider on the 96 MHz clock whose MAME comment (`:139`) is literally `// audio`, so a 20 kHz
timer interrupt paired with one `0x3041` write per tick would be the "proper" way to play
PCM. `PPAMCtrl` (pulse-density/pulse-amplitude modulation) is documented in the SoC manual as
the alternative output mode but MAME does nothing with it, so there is no reference behaviour
to copy.

## 2. What the game actually does — measurements

Instrumentation was added to `emu/gpce4.cpp` behind environment variables (no CLI change, no
edit to `main.cpp`):

| variable | effect |
|---|---|
| `GPCE4_AUDIO_STATS=<path>` | dump the register/IO statistics to `<path>` when the `Gpce4` object is destroyed (`1` or `-` = stdout) |
| `GPCE4_WAV=<path>` | record the model's 48 kHz stream to a WAV while running |
| `GPCE4_AUDIO_OFF=1` | disable sample production (A/B measurements) |

Every write to `0x3040..0x3043` is counted with value range, a top-8 value histogram and (for
`0x3041`) the inter-write spacing; every IO write is also counted per offset. With no
statistics requested the cost is a couple of integer ops per DAC write and nothing per tick.

### 2.1 The DAC/PPAM block is never written

| run | emulated | writes to `3040` | `3041` | `3042` | `3043` |
|---|---|---|---|---|---|
| `--seconds 20 --autoplay` | 20.0 s | 0 | 0 | 0 | 0 |
| `--seconds 60 --autoplay` | 60.0 s | 0 | 0 | 0 | 0 |
| `--seconds 30 --press …` (dense B1/B2/B3, 1800 frames) | 30.0 s | 0 | 0 | 0 | 0 |

`dbgtest/audio/io_stats.txt` (20 s) and `dbgtest/audio/io_stats_60s.txt` (60 s) hold the full
dumps; `dbgtest/audio/io_stats_keys.txt` is the button-mashing probe.

So: **0 writes/second, no value range at all, no PPAM mode bit, no timer_a sample clock.**
11 000 emulated seconds would be needed before this says anything other than "never".

### 2.2 The IO profile it *does* have

20 s autoplay: 583 066 IO writes over 39 distinct offsets (60 s: 1 408 179 writes, same 39
offsets — the profile is perfectly stationary).

| ofs | register | 20 s writes | last value |
|---|---|---|---|
| `0092` | `SPI2_TXData` | 526 093 (26 305/s) | `4cab` |
| `0054` | `INT2_Status` | 41 236 (2 062/s) | `0005` |
| `0005` | `IOB_Buffer` | 10 273 | `1c03` |
| `00e2` | `SPI_TXData` | 1 767 | `0014` |
| `0034` | `Watchdog_Clear` | 1 267 | `5555` |
| `0001` | `IOA_Buffer` | 561 | `ffff` |
| `0016` | `Timer_Ctrl` | 145 | `0fff` |
| `0020` | `PWM0_Ctrl` | 76 | `0000` |
| `0010`/`0011` | `TimerA_Data` / `TimerA_Counter` | 2 each | `f9c0` |
| `0050` | `INT_Ctrl` | 1 | `0000` |

Interrupt state at the end of every run:

```
INT_Ctrl    = 0000   bit15 (timer_a / 20 kHz) and bit13 (timer_c) masked
INT2_Ctrl   = 0205   timer_2hz | timer_64hz | timer_2khz enabled
INT_Status  = a1000000  bit31 = timer_a pending, but the CPU never takes it
unmapped writes = 0
```

`PWM0_Ctrl` is written 76 times in 20 s, always `0x0000` — that is the game *disabling*
PWM, not driving a buzzer. `TimerA_Data` is loaded once with `0xf9c0` (63 936) and that is
the last thing the timer ever hears; with the 20 kHz interrupt masked there is nothing to
clock a sample rate.

### 2.3 The code for it exists in the mask ROM

`dbgtest/audio/scan_rom.py` scans the ROM images for the little-endian operand bytes of the
four registers:

| image | `3040` | `3041` | `3042` | `3043` |
|---|---|---|---|---|
| `roms/internal_rom_221017a.bin` (64 KiB) | 1 @ `0x4a4` | **7** @ `0x4c2, 0x31b2, 0x31cc, 0x31de, 0x3226, 0x3240, 0x3252` | 7 | 1 @ `0x4ba` |
| `roms/dmc_v1_base.bin` (8 MiB) | 0 | **0** | 7 | 13 |

So the mask ROM contains an audio routine (a DAC init at `0x4a4`/`0x4ba` plus a block of
seven `0x3041` stores around `0x31b2..0x3252` — consistent with a short sample player), and
the 8 MiB flash asset image contains none of the `0x3040`/`0x3041` operands at all. The
routine simply never runs in the boot → clock → menu → tour path: zero writes to the block,
zero unmapped writes, and the 20 kHz interrupt left masked the whole time. Finding the scene
that calls it (if any — the flash has no DAC code) is future work, see §7.

## 3. The model (`emu/gpce4.cpp`, section `AUDIO`)

Design constraint was "cheap at 1.6 M cycles/frame, no per-cycle loops, only per-write work".

* **Ring buffer** of `AUDIO_RING_FRAMES = 65536` mono `int16_t` samples (indexed with a mask,
  ~1.365 s of slack), heap allocated so a `Gpce4` on the stack stays small (the SoC selftest
  creates them by value).
* **Producer**: `0x3041` write → `audio_sink_code()` latches `int16_t(data)` as the current
  level (zero-order hold, exactly what the R2R ladder does) and calls `audio_stat_note()`.
  That is the only per-write cost, a handful of integer ops.
* **Resampler**: `audio_advance(from, to)` runs at the end of `tick()`. Output frames sit on a
  grid of `AUDIO_CYCLES_PER_FRM = 2000` cycles — 96 MHz / 2000 = 48 000 Hz exactly, so the
  grid never drifts — and each frame is the **time-weighted average** of the level over its
  2000-cycle window (`sum(level·cycles)/sum(cycles)`). Averaging rather than sampling means
  the model is correct for *any* producer rate: one write per frame (DC), 20 kHz DAC updates
  (2.4 writes per frame), or a 200 kHz PPAM carrier all integrate to the right value instead
  of aliasing. Cost is one loop per tick that crosses a frame boundary, i.e. O(1) amortised.
* **API** (`emu/gpce4.h`):

  ```cpp
  static constexpr int AUDIO_RATE = 48000;
  bool   audio_enabled = true;                                  // stop/start production
  size_t audio_read(int16_t *dst, size_t frames);               // 48 kHz mono, drains the ring
  void   audio_reset();                                         // re-arm buffers and counters
  uint64_t audio_frames_produced();                             // + overruns / underruns
  ```
  `audio_read()` returns the number of frames actually available (0 = starved) and pads the
  remainder with silence while counting underruns, so a host may call it with any block size.
  Overruns (host not draining) drop samples silently by design — a real DAC does the same.
* **Statistics**: `m_audio_stat[4]` (per register: writes, min/max, top-8 values, inter-write
  spacing histogram at 10 µs resolution) and `m_io_hist[256]` for all IO writes, written by
  `audio_stats_dump()`. With statistics off the counters still run (a few integer ops) so a
  run with and without the file reports identical numbers.

Measured cost (`emu/audio_selftest.cpp` section `[7]`, tick-only loop, 10 emulated seconds
each, same process): `tick()` costs ≈ 5.6–5.9 ms per emulated second with audio off and
≈ 6.4 ms with it on, i.e. **+0.45…0.80 ms per emulated second, ≤ 0.08 % of a 1.0x run's wall
time**. Whole-emulator speed is in §6.

## 4. WAV export (`emu/audio_dump.cpp|.h`)

Standalone, SoC-independent writer: 44-byte RIFF header whose sizes are patched on close,
4096-frame staging buffer so a per-sample producer does not become a syscall per sample, and
an `atexit` safety close so a killed process still leaves a playable file.

```cpp
bool audio_wav_open(const char *path, int rate);   // 16-bit mono
void audio_wav_write(const int16_t *frames, size_t count);
void audio_wav_close();
size_t audio_wav_pump(Gpce4 *soc);                 // drain everything pending; 0 when dry
```

`audio_wav_pump()` exists so the host loop needs exactly one call per frame.

### How it is wired today (no `main.cpp` change)

`reset()` opens the WAV if `GPCE4_WAV` is set; `audio_push()` writes into it; `~Gpce4()`
dumps statistics and closes the file (the `Gpce4` in `main()` is a stack object, so the
destructor is the only hook that is guaranteed to run before `main()` returns — an
`atexit` handler would run *after* the object died).

```powershell
$env:GPCE4_WAV = "dbgtest\audio\tour.wav"
$env:GPCE4_AUDIO_STATS = "dbgtest\audio\io_stats.txt"
.\emu\digicore.exe --seconds 20 --autoplay
Remove-Item Env:GPCE4_WAV, Env:GPCE4_AUDIO_STATS
```

### If you want `--wav out.wav` on the command line

`main.cpp` needs one parse line, one open line and one pump line (plus `#include "audio_dump.h"`):

```cpp
else if (a == "--wav") wav_path = next("wav");          // with const char *wav_path = nullptr;
...
if (wav_path) audio_wav_open(wav_path, Gpce4::AUDIO_RATE);   // before the frame loop
...
audio_wav_pump(&soc);                                   // after soc.tick(consumed), every frame
```

`audio_wav_pump()` drains whatever the frame produced (~800 frames per 1.6 M-cycle frame),
so one call per frame is enough; the file is closed automatically by the `atexit` handler,
and calling `audio_wav_close()` after the loop is also fine.

## 5. Measured artefacts

`dbgtest/audio/tour.wav` — 20 s autoplay, `digicore.exe --seconds 20 --autoplay`:

```
bytes       : 1920046 (1.83 MiB)
channels    : 1, 16-bit, 48000 Hz
frames      : 960001 -> 20.000 s
peak        : 0 (0.00% FS)
rms         : 0.0 (0.00% FS, -inf dBFS)
nonzero     : 0 of 960001 samples
verdict     : SILENT (all samples zero)
data_bytes  : 1920002 (header says 1920002)
```

It is **digital silence: not a bug in the writer but the truth about the scene** (§2.1). The
model still produced 960 001 frames, `overruns = 894 465` (nothing consumes the ring in
`digicore`) and `underruns = 0`, which proves the producer path is alive and it is the DAC
input that is zero, not the plumbing. Once a scene writes `0x3041`, the same command produces
sound with no further changes.

Other files: `dbgtest/audio/io_stats.txt` (20 s), `io_stats_60s.txt` (60 s),
`io_stats_keys.txt` (30 s button probe), `reg.tr` (instruction trace used for validation),
`wav_stats.py` (the reporter above), `scan_rom.py` (ROM operand scan),
`run_audio_test.ps1` (build+run `emu/audio_selftest.cpp`), `measure_speed.ps1` (A/B CPU-time
measurement).

## 6. Verification

| check | result |
|---|---|
| `emu/gpce4_selftest.exe` (SoC, 83 assertions) | `=== 83 passed, 0 failed ===`, exit 0 |
| `emu/audio_selftest` (model, 25 assertions) | `=== 25 passed, 0 failed ===`, exit 0 |
| `emu_validate.py … dbgtest/audio/reg.tr` | `matched 682005 / 682005 instructions (100.000%)` |
| `--seconds 10 --autoplay` speed, audio on | 1.5x, 1.5x, 1.4x |
| same with `GPCE4_AUDIO_OFF=1` | 1.5x, 1.3x, 1.2x |
| `--seconds 20 --autoplay` (recording WAV) | `speed=1.4x…1.6x` |

Nothing about the CPU changed: the model only reads the latched DAC code and the cycle
counter, so the instruction stream is bit-identical to the pre-audio emulator (100 % match
above). The speed A/B is inside the run-to-run spread of a box that also runs four other DSH
sessions; the deterministic microbenchmark in §3 bounds the model's own cost.

## 7. Known gaps

1. **No sound, because the game never asks for any** (in boot / clock / menu / 20 s tour /
   60 s tour / 30 s button-mashing). The mask-ROM routine at `0x31b2..0x3252` is unreached;
   the trigger scene is unidentified. Candidate next steps: hook those PCs with a breakpoint
   (`--trace` + grep for `0031bx`), or scan the flash asset image for a data-driven sound
   table that would be loaded by a game mode the tour does not enter.
2. **`PPAMCtrl` (0x3043) is recorded, not modelled.** MAME has no behaviour to copy; a PDM
   carrier model would need a documented carrier frequency, and until a scene writes the
   register there is nothing to calibrate against.
3. **`DAC_CH2` (0x3042) is stored but not mixed.** MAME does not mix it either; the two
   channels would need summing or panning rules that no reference implements.
4. `DAC_Ctrl` (0x3040) bits (enable, channel routing, PPAM/PWM select) are stored but do not
   gate the model — the model produces the held code regardless. Fine while nothing writes
   it, worth revisiting when something does.
5. `digimon-player.exe` has no sound output; it would need a `waveOut`/WASAPI ring fed from
   the same `audio_read()`.
6. Output is fixed at 48 kHz mono. `audio_read()` has no resampler for 44.1 kHz hosts; the
   WAV writer likewise assumes the rate it was opened with.
7. `docs/SOC_PORTS.md` §11's selftest link line now needs `emu/audio_dump.cpp` (any link that
   pulls in `gpce4.cpp` does) and `-fpermissive` — `gpce4_selftest.cpp:129` and `:374` pass
   the `const` result of `std::vector<uint8_t>::data()` to `set_images(..., uint8_t*, ...)`,
   which g++ 16 rejects. The clean fix is declaring those two buffers non-const.

## 8. Reproduce everything

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\emu\build.ps1          # digicore + player
powershell -NoProfile -ExecutionPolicy Bypass -File dbgtest\audio\run_audio_test.ps1   # 25 assertions

$env:GPCE4_WAV = "dbgtest\audio\tour.wav"; $env:GPCE4_AUDIO_STATS = "dbgtest\audio\io_stats.txt"
.\emu\digicore.exe --seconds 20 --autoplay
Remove-Item Env:GPCE4_WAV, Env:GPCE4_AUDIO_STATS
C:\Users\DuWeisi\py311\python.exe dbgtest\audio\wav_stats.py dbgtest\audio\tour.wav

.\emu\digicore.exe --frames 2 --trace dbgtest\audio\reg.tr --trace-seconds 0.02
C:\Users\DuWeisi\py311\python.exe tools\emu_validate.py dbgtest\cov\A\000_t00000.tr dbgtest\audio\reg.tr
```
