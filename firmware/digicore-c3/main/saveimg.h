// ---------------------------------------------------------------------------
//  saveimg.h - the game's flash writes, made to survive a power cut
//
//  The SoC's SPI flash is the cartridge: 8 MiB of code plus a tiny save area
//  the game erases and reprograms when it wants to persist something.  On the
//  device that image is a read-only memory mapped partition (rom_esp.cpp), so
//  the writes land in SparseRom's copy-on-write overlay and vanish on reset -
//  which is exactly what "the game cannot save" looks like.
//
//  This module closes that loop: it snapshots the overlay pages into a small
//  dedicated partition.  Two slots are written alternately, each with its own
//  CRC and sequence number, so a power cut in the middle of a save always
//  leaves the previous one intact.
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>

// Pages the save area is expected to touch.  The game writes exactly two 4 KiB
// sectors (0x7FD000 and 0x7FE000), and DRAM on the C3 is tight enough that the
// transient buffer has to be sized for reality rather than for comfort: every
// extra page here costs 4 KiB of .bss in two different translation units.
enum : int { SAVEIMG_MAX_PAGES = 2 };
enum : int { SAVEIMG_PAGE_SIZE = 4096 };

// Locate the partition and remember it.  Returns false when the build has no
// `save` partition (older partition tables) - the caller then simply runs
// without persistence instead of failing.
bool saveimg_init();

// Read the newest valid slot.  Fills `pages`/`count` and copies the page
// contents into `data` (SAVEIMG_MAX_PAGES * SAVEIMG_PAGE_SIZE bytes).
bool saveimg_load(uint32_t *pages, uint8_t *data, int *count);

// Write one slot.  Returns true when the image reached flash and verified.
bool saveimg_store(const uint32_t *pages, const uint8_t *data, int count);

// True when saveimg_init() found the partition.
bool saveimg_ready();
