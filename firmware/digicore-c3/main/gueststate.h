// ---------------------------------------------------------------------------
//  gueststate.h - full machine snapshots, so a power cut costs at most a minute
//
//  saveimg.cpp only mirrors the SPI flash overlay, which is what the game
//  writes when it decides to save.  This retail ROM never issues a single page
//  program, so that mechanism carries no progress at all: everything the player
//  earned lives in the 8 KiB of guest RAM and the unSP register file.
//
//  This module snapshots two caller-chosen blocks (the CPU state and the RAM
//  window) into a ring of fixed-size records inside the `save` partition.  Two
//  separate blocks rather than one concatenated blob because DRAM on the C3 has
//  10 KiB free: an 8 KiB staging copy would not fit, and the two sources are
//  not adjacent in memory.
//
//  A record is three 4 KiB sectors, so erasing one only costs those three and
//  the ring spreads the wear across all of them: a snapshot every 60 s is ~1440
//  writes a day over eight positions, well inside the flash's endurance.
//
//  Record layout (GUESTSTATE_RECORD_SIZE = 12 KiB, three erase sectors):
//
//      +0   u32  magic   'DGST'
//      +4   u16  version
//      +6   u16  reserved
//      +8   u32  sequence           higher = newer
//      +12  u32  crc32              of a[] then b[]
//      +16  u32  length_a
//      +20  u32  length_b
//      +24  u32  reserved[2]
//      +32  u8   a[length_a]
//      ...  u8   b[length_b]
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>

// Three 4 KiB erase sectors per record; the ring holds five of them in the
// first 60 KiB of the partition.  fbsave.cpp takes the next 32 KiB for the
// panel frame and saveimg.cpp keeps the last 32 KiB, so the 128 KiB partition
// is fully spoken for: 60 + 32 + 32 = 124 KiB plus a 4 KiB tail.
enum : int { GUESTSTATE_RECORD_SIZE = 0x3000 };
enum : int { GUESTSTATE_RECORDS = 5 };
enum : int { GUESTSTATE_HEADER_SIZE = 32 };
enum : int { GUESTSTATE_MAX_PAYLOAD = GUESTSTATE_RECORD_SIZE - GUESTSTATE_HEADER_SIZE };

// Locate the partition and remember it.  False when the build has no `save`
// partition, in which case the caller simply runs without snapshots.
bool gueststate_init();

// True when gueststate_init() found the partition.
bool gueststate_ready();

// Snapshot `len_a` + `len_b` bytes.  Returns true once the record reached flash
// and its header verified.
bool gueststate_store(const void *a, uint32_t len_a, const void *b, uint32_t len_b);

// Restore the newest record whose lengths match.  False when there is none or
// the CRC disagrees; the buffers then hold a rejected snapshot, which is
// harmless at boot where they have no valid contents to lose.
bool gueststate_load(void *a, uint32_t len_a, void *b, uint32_t len_b);

// Sequence number of the newest record, 0 when there is none.
uint32_t gueststate_newest_seq();
