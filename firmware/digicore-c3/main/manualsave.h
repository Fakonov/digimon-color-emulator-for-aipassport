// ---------------------------------------------------------------------------
//  manualsave.h - one user-owned snapshot slot, independent of the autosave ring
//
//  The autosave ring (gueststate.cpp) is a rolling five-record history: a manual
//  save stored there would be overwritten within one autosave period.  This
//  module keeps the user's "save now" state in its own three-sector slot at the
//  tail of the `save` partition, so it survives any number of autosaves.
//
//  Reading it back is a deliberate two-step contract with app_main:
//    * the manual slot is restored into the live machine, then
//    * the same state is written into the autosave ring as the newest record,
//      so a power cut right after a manual load does not lose it.
//
//  Slot layout (12 KiB, three 4 KiB erase sectors, at partition offset 0x20000):
//
//      +0   u32  magic   'DGMS'
//      +4   u16  version
//      +6   u16  reserved
//      +8   u32  tod              time of day when saved (0 when unknown)
//      +12  u32  length_a
//      +16  u32  length_b
//      +20  u32  crc32              of a[] then b[]
//      +24  u32  reserved[2]
//      +32  u8   a[length_a]
//      ...  u8   b[length_b]
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>

// Locate the save partition and check that it is large enough for the slot.
bool manualsave_init();
bool manualsave_ready();

// True when a valid header is present (payload CRC is not checked here).
bool manualsave_exists();

// Erase the slot and write both blocks.  Returns true once the header read back
// matches what was written.
bool manualsave_store(const void *a, uint32_t len_a, const void *b, uint32_t len_b,
                      uint32_t tod);

// Read both blocks and verify the payload CRC.  `tod` receives the saved time
// of day (0 when unknown).  False when the slot is empty or corrupt.
bool manualsave_load(void *a, uint32_t len_a, void *b, uint32_t len_b, uint32_t *tod);
