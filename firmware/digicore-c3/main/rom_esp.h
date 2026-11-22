// ---------------------------------------------------------------------------
//  rom_esp.h - mount the sparse SPI flash image from the `roms` partition
//
//  tools/make_sparse_rom.py lays the 8 MiB Digimon Color flash image out as
//  header + bitmap + index + present pages; emu/romstore.h is the byte view the
//  SoC talks to.  This file is the glue: find the partition, parse the header,
//  keep the page index in RAM and feed the page cache with esp_partition_read.
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>

#include "emu/romstore.h"

namespace romstore {

struct Info
{
	bool     ok = false;
	uint32_t version = 0;
	uint32_t image_size = 0;        // 8 MiB for dmc_v1_base.bin
	uint32_t page_size = 0;
	uint32_t page_count = 0;
	uint32_t pages_present = 0;
	uint32_t part_offset = 0;       // absolute flash offset of the partition
	uint32_t part_size = 0;
	uint32_t data_offset = 0;       // first page inside the partition
	char     sha1[41] = {};         // hex, NUL terminated
};

// Finds the partition, validates the header and installs the page loader.
// Logs one line per fact (ESP_LOGI, tag "ROMSTORE").  Returns false when the
// partition is missing or the header does not match.
bool mount(Info *out);

// Non-null after a successful mount(); the SoC reads through this.
SparseRom *image();

} // namespace romstore
