// ---------------------------------------------------------------------------
//  fbsave.h - persist the visible part of the panel GRAM next to the snapshot
//
//  gueststate.h carries the CPU file, the guest RAM and the SoC registers, but
//  not the 32 KiB framebuffer: two of those would not fit in the `save`
//  partition, and the guest only ever draws the 16x16 tiles it thinks changed.
//  After a snapshot restore it believes the screen is already up to date, so
//  nothing repaints and the panel stays black until the game decides to redraw
//  the whole screen.
//
//  This module keeps one copy of the visible 128x112 RGB565 window (28672 B) in
//  the gap the `save` partition has between the gueststate ring (5 records now)
//  and the two saveimg slots.  Only the snapshot path touches it, so the wear
//  follows the same 60 s cadence as the snapshots themselves.
//
//  Region layout inside the `save` partition (128 KiB):
//
//      0x00000  gueststate ring, 5 x 12 KiB            (0xF000 bytes)
//      0x0F000  this module, one 32 KiB erase block    (0x8000 bytes)
//      0x18000  saveimg.cpp, two 16 KiB slots          (unchanged)
//
//  Frame layout:
//
//      +0   u32  magic   'DGFB'
//      +4   u16  version
//      +6   u16  reserved
//      +8   u32  bytes
//      +12  u32  crc32   of the pixel bytes that follow
//      +16  u32  reserved[4]
//      +32  u8   pixels[bytes]
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>

// Locate the `save` partition.  False when the build has no such partition or
// it is too small, in which case the caller simply keeps the cold-boot path.
bool fbsave_init();

// Replace the stored frame with `bytes` of RGB565 pixels.  Returns true once
// the erase and both writes came back OK.
bool fbsave_store(const void *pixels, uint32_t bytes);

// Read the stored frame back into `pixels`.  False when nothing was ever
// stored, the sizes disagree or the CRC does not match; `pixels` is then left
// holding whatever the panel already had, which the caller overwrites anyway.
bool fbsave_load(void *pixels, uint32_t bytes);
