#pragma once

// ---------------------------------------------------------------------------
//  St7735.h - Sitronix ST7735 LCD controller (128x128 RGB565 panel)
//
//  The struct itself lives in "gpce4.h", because the integration contract for
//  this project keeps St7735 and Gpce4 in one header (Gpce4 holds a St7735 by
//  value, so a caller that wants to poke the SoC must be able to see the LCD).
//  This header exists so LCD-only translation units have a natural include and
//  so the declaration is reachable without knowing that split.
//
//  Implementation: emu/st7735.cpp, a port of MAME 0.289's
//  src/devices/video/st7735_lcdc.cpp (ref/st7735_lcdc.cpp on disk).
// ---------------------------------------------------------------------------

#include "gpce4.h"
