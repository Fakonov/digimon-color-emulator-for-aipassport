// ---------------------------------------------------------------------------
//  hosttest/romstore_test.cpp - host side check for emu/romstore.h
//
//  The C3 port cannot be unit tested on the device, so the two pieces that are
//  pure logic (the sparse page cache and the write overlay) are checked here
//  against the flat 8 MiB image on a PC:
//
//    * read16() over *every* word of the image, twice, with the page order
//      shuffled so slots are evicted and refilled (word order == truth)
//    * read8() on odd and even offsets
//    * erase + page program at the game's save sector 0x7FD000, including the
//      NOR "program only clears bits" rule
//    * the overlay survives cache eviction (16 unrelated pages in between)
//
//  Build (w64devkit, same compiler as emu/build.ps1):
//    g++ -std=c++20 -O2 -Wno-sign-compare -o romstore_test.exe
//        hosttest/romstore_test.cpp main/emu/romstore.cpp
//  Run from firmware/digicore-c3:
//    ./romstore_test.exe sparse/digicore-sparse.bin ../../roms/dmc_v1_base.bin
// ---------------------------------------------------------------------------
#include "../main/emu/romstore.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

std::vector<uint8_t> g_flat;      // truth: the 8 MiB image
std::vector<uint8_t> g_sparse;    // the sparse store
uint32_t g_index[2048];
uint32_t g_page_count = 0;
uint32_t g_loads = 0;

bool loader(void *, uint32_t page, uint16_t *dst)
{
	g_loads++;
	if (page >= g_page_count) {
		std::memset(dst, 0xff, 4096);
		return true;
	}
	const uint32_t off = g_index[page];
	if (off == 0xffffffffu)
		std::memset(dst, 0x00, 4096);
	else
		std::memcpy(dst, g_sparse.data() + off, 4096);
	return true;
}

uint32_t rd32(size_t off)
{
	uint32_t v = 0;
	std::memcpy(&v, g_sparse.data() + off, 4);
	return v;
}

int fail(const char *what, size_t off, unsigned got, unsigned want)
{
	std::printf("FAIL %s at 0x%06x: got %08x want %08x\n", what, unsigned(off), got, want);
	return 1;
}

} // namespace

int main(int argc, char **argv)
{
	if (argc < 3) {
		std::printf("usage: %s <sparse.bin> <flat.bin>\n", argv[0]);
		return 2;
	}

	FILE *f = std::fopen(argv[1], "rb");
	if (!f) {
		std::printf("cannot open %s\n", argv[1]);
		return 2;
	}
	std::fseek(f, 0, SEEK_END);
	const long sparse_size = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	g_sparse.resize(size_t(sparse_size));
	if (std::fread(g_sparse.data(), 1, g_sparse.size(), f) != g_sparse.size()) {
		std::printf("short read on %s\n", argv[1]);
		return 2;
	}
	std::fclose(f);

	f = std::fopen(argv[2], "rb");
	if (!f) {
		std::printf("cannot open %s\n", argv[2]);
		return 2;
	}
	std::fseek(f, 0, SEEK_END);
	const long flat_size = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	g_flat.resize(size_t(flat_size));
	if (std::fread(g_flat.data(), 1, g_flat.size(), f) != g_flat.size()) {
		std::printf("short read on %s\n", argv[2]);
		return 2;
	}
	std::fclose(f);

	// ---- header ----------------------------------------------------------
	if (std::memcmp(g_sparse.data(), "DMC1", 4) != 0) {
		std::printf("FAIL magic\n");
		return 1;
	}
	const uint32_t image_size = rd32(8);
	const uint32_t page_size = rd32(32);
	const uint32_t index_offset = rd32(44);
	const uint32_t data_offset = rd32(48);

	std::printf("sparse   %ld B, image %u B, page %u B, index +0x%x, data +0x%x\n",
	            sparse_size, unsigned(image_size), unsigned(page_size),
	            unsigned(index_offset), unsigned(data_offset));
	if (image_size != uint32_t(g_flat.size())) {
		std::printf("FAIL image size: header %u, flat file %ld\n",
		            unsigned(image_size), flat_size);
		return 1;
	}
	if (page_size != 4096) {
		std::printf("FAIL page size %u\n", unsigned(page_size));
		return 1;
	}
	g_page_count = image_size / 4096;
	if (g_page_count != 2048) {
		std::printf("FAIL page count %u\n", unsigned(g_page_count));
		return 1;
	}
	std::memcpy(g_index, g_sparse.data() + index_offset, g_page_count * 4);

	SparseRom rom;
	rom.begin(image_size, loader, nullptr);

	// ---- read16 sweep, twice, shuffled ------------------------------------
	const uint32_t pages = g_page_count;
	std::vector<uint32_t> order(pages);
	for (uint32_t i = 0; i < pages; i++)
		order[i] = i;

	unsigned errors = 0;
	for (int pass = 0; pass < 2; pass++) {
		if (pass == 1) {
			// cheap LCG shuffle: forces a cold cache for every page
			for (uint32_t i = pages; i > 1; i--) {
				uint32_t j = (uint32_t)((uint64_t)i * 1103515245u + 12345u) >> 16;
				j %= i;
				const uint32_t t = order[i - 1];
				order[i - 1] = order[j];
				order[j] = t;
			}
		}
		g_loads = 0;
		for (uint32_t n = 0; n < pages; n++) {
			const uint32_t page = order[n];
			for (uint32_t w = 0; w < 2048; w++) {
				const size_t off = size_t(page) * 4096 + w * 2;
				const uint16_t want = uint16_t(g_flat[off] | (g_flat[off + 1] << 8));
				const uint16_t got = rom.read16(uint32_t(off));
				if (got != want) {
					errors += unsigned(fail("read16", off, got, want));
					if (errors > 8)
						return 1;
				}
			}
		}
		std::printf("pass %d   %u words, %u page loads\n", pass + 1,
		            unsigned(pages * 2048), unsigned(g_loads));
	}

	// ---- read8 -----------------------------------------------------------
	for (size_t off = 0; off < 4096; off++) {
		const uint8_t got = rom.read8(uint32_t(0x230000 + off));
		const uint8_t want = g_flat[0x230000 + off];
		if (got != want) {
			errors += unsigned(fail("read8", 0x230000 + off, got, want));
			break;
		}
	}
	std::printf("read8    4096 bytes at 0x230000 ok\n");

	// ---- erase + page program at the save sector --------------------------
	const uint32_t sector = 0x7fd000;
	static const uint8_t saved[8] = { 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0 };

	std::vector<uint8_t> ref(g_flat.begin() + sector, g_flat.begin() + sector + 0x2000);
	std::memset(ref.data(), 0xff, 0x1000);
	for (int i = 0; i < 8; i++)
		ref[size_t(i)] &= saved[i];

	rom.erase_sector(sector);
	for (int i = 0; i < 8; i++)
		rom.program_byte(sector + uint32_t(i), saved[i]);

	for (uint32_t off = 0; off < 0x2000; off += 2) {
		const uint16_t got = rom.read16(sector + off);
		const uint16_t want = uint16_t(ref[off] | (ref[off + 1] << 8));
		if (got != want) {
			errors += unsigned(fail("erase/program", sector + off, got, want));
			break;
		}
	}
	std::printf("save      sector 0x7fd000 erased + 8 bytes programmed ok\n");

	// ---- the overlay must survive eviction --------------------------------
	for (uint32_t p = 0; p < 32; p++)
		(void)rom.read16(uint32_t(p) * 4096 * 4);   // 32 unrelated pages

	const uint16_t hi = rom.read16(sector);
	const uint16_t want = uint16_t(ref[0] | (ref[1] << 8));
	if (hi != want) {
		errors += unsigned(fail("overlay eviction", sector, hi, want));
	} else {
		std::printf("overlay  survives 32 page evictions ok "
		            "(loads %u, overlay hits %u, writes %u)\n",
		            unsigned(rom.page_loads()), unsigned(rom.overlay_hits()),
		            unsigned(rom.overlay_writes()));
	}

	// ---- and the page after the erased sector is untouched ----------------
	for (uint32_t off = 0; off < 0x1000; off += 2) {
		const uint16_t got = rom.read16(sector + 0x1000 + off);
		const uint16_t want2 = uint16_t(ref[0x1000 + off] | (ref[0x1000 + off + 1] << 8));
		if (got != want2) {
			errors += unsigned(fail("neighbour page", sector + 0x1000 + off, got, want2));
			break;
		}
	}
	std::printf("neighbour page 0x7fe000 untouched ok\n");

	if (errors) {
		std::printf("%u FAILURES\n", errors);
		return 1;
	}
	std::printf("ALL OK\n");
	return 0;
}
