// ---------------------------------------------------------------------------
//  wallclock.h - what time it actually is, for a board with no RTC
//
//  The handset has no RTC chip at all (docs/RE_RTC.md): the ROM keeps hh:mm:ss
//  in RAM and advances it from the SoC's 2 Hz interrupt.  Everything about that
//  clock depends on the guest running its frame body, so on this emulator it
//  loses time whenever the guest parks, disables interrupts, or simply cannot
//  be run 60 times per wall second.
//
//  This module owns the *host* side of the clock: one anchor (a Unix epoch plus
//  the esp_timer reading at that moment), persisted in NVS.  app_main pushes it
//  into the guest's registers, so the game displays real time instead of
//  "wall time since the image was compiled".
//
//  There is no time source on the board, so the anchor has to come from
//  outside:
//    * NVS     - the epoch stored with the previous snapshot; the best a cold
//                boot can do is "the time it was when we last saved", which is
//                off by however long the unit was powered down.
//    * console - `t <epoch> [tz_minutes]`, i.e. the PC that is already
//                connected over USB telling the firmware what time it is.
//    * build   - the compile timestamp, used only when NVS is empty.  This is
//                the old behaviour and it is wrong by the age of the image;
//                it exists so a freshly flashed board is not stuck at 0:00.
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>

// Read the persisted anchor (falls back to the build timestamp) and start the
// monotonic extrapolation.  Call once, after nvs_flash_init().
void wallclock_init();

// True when the anchor is better than the build timestamp: a stored epoch from
// a previous snapshot, or one set over the console this session.
bool wallclock_live();

// "build" / "nvs" / "console" - which source the current anchor came from.
const char *wallclock_source();

// Install a new anchor.  `tz_minutes` is the local offset east of UTC (480 for
// UTC+8) and is persisted with the epoch.  Called by the console command and
// by nothing else.
void wallclock_set(int64_t epoch_sec, int tz_minutes);

// Persist the current anchor (epoch at this instant) so the next boot resumes
// from here.  Called with every machine snapshot.
void wallclock_persist();

// Adopt a time of day that came from the guest: the player committed it in the
// game's own clock editor (flash 0x204179, detected as a Timebase_Clear write
// with the clock enabled).  The date is kept and the time of day becomes the
// anchor.  This is the only path that lets the player overrule the host clock,
// and it is driven by a hardware write instead of guessing at RAM jumps.
void wallclock_adopt_local(int64_t local_sec);

// Follow the guest clock while the machine runs at an accelerated rate.  The
// time-lapse injector makes the guest advance many seconds per host second, so
// the anchor has to move with it or clock_sync() would drag it back.  Nothing
// is written to NVS here: the next snapshot persists whatever the guest says,
// and a power cut costs at most one autosave period of virtual time.
void wallclock_track_local(int64_t local_sec);

// Local time of day.  `local_sec` is 0..86399.
int64_t wallclock_local_sec();
void wallclock_hms(int *h, int *m, int *s);
