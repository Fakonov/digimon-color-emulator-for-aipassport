// ---------------------------------------------------------------------------
//  romstore.h - the SPI NOR image as the SoC sees it (byte addressed, 8 MiB)
//
//  The PC harness (emu/gpce4.cpp) maps roms/dmc_v1_base.bin with mmap() and
//  indexes it directly.  On the C3 those 8 MiB live in the `roms` data
//  partition as a *sparse* page store instead (tools/make_sparse_rom.py), so
//  every byte the SoC touches has to go through this class:
//
//    * a 16 entry, direct mapped 4 KiB page cache (64 KiB of internal RAM).
//      The index hash is (page ^ (page>>4)) & 15 because the two hot code
//      pages 0x200 and 0x230 would collide under a plain & 15.
//    * a 4 entry copy-on-write overlay (16 KiB) so the game's save writes
//      (8 bytes at 0x7FD000 / 0x7FE000, after a sector erase) are visible to
//      its own reads.  Nothing is written back to flash: see docs/C3_PORT.md.
//
//  No ESP-IDF headers here on purpose - the page fetch is a callback, so this
//  file also compiles in a host side unit test.
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <cstring>

class SparseRom
{
public:
	enum : uint32_t
	{
		PAGE_BITS  = 12,
		PAGE_SIZE  = 1u << PAGE_BITS,
		PAGE_WORDS = PAGE_SIZE / 2,
		INDEX_NONE = 0xffffffffu,
		// page cache: 16 * 4 KiB = 64 KiB, write overlay: 4 * 4 KiB = 16 KiB
		SLOTS       = 16,
		DIRTY_SLOTS = 4
	};

	// Fetch page `page` of the image into `dst` (4 KiB, natural byte order).
	// Returns false when the page cannot be read; the caller then sees 0xff.
	using Loader = bool (*)(void *ctx, uint32_t page, uint16_t *dst);

	// Direct view of a memory mapped sparse store: `base` points at the start
	// of the partition and `index[i]` is the byte offset of page i (or
	// INDEX_NONE).  With this installed read16() never copies a page - it just
	// indexes into the flash-mapped window, which the hardware cache serves.
	// The page cache below stays as the fallback (host unit test, and any
	// platform where the partition cannot be mapped).
	void set_mapped(const uint8_t *base, const uint32_t *index)
	{
		m_base = base;
		m_index = index;
	}

	void begin(uint32_t image_size, Loader loader, void *ctx);
	void clear();

	uint32_t size() const { return m_size; }

	// Little endian 16 bit word at byte offset `off` (always even: the SoC's
	// word address * 2).  This is the hot path.
	inline uint16_t read16(uint32_t off)
	{
		const uint32_t page = off >> PAGE_BITS;
		// Overlay pages are rare (the game writes 8 bytes at 0x7FD000/0x7FE000)
		// so a range test keeps the common case to two compares.
		if (page - m_ov_lo <= m_ov_hi - m_ov_lo)
			return read16_overlay(off, page);
		if (m_base)
		{
			const uint32_t po = m_index[page];
			if (po == INDEX_NONE)
				return 0;
			uint16_t w;
			std::memcpy(&w, m_base + po + (off & (PAGE_SIZE - 1)), 2);
			return w;
		}
		const uint32_t h = (page ^ (page >> 4)) & uint32_t(SLOTS - 1);
		if (m_tag[h] != page)
			return read16_miss(off, page, h);
		return m_cache[h * PAGE_WORDS + ((off >> 1) & (PAGE_WORDS - 1))];
	}

	// Host pointer for the 4 KiB page holding byte offset off, or null when the
	// page is absent from the packed image.  Used by the CPU opcode window.
	const uint16_t *page_window(uint32_t off)
	{
		const uint32_t page = off >> PAGE_BITS;
		if (page - m_ov_lo <= m_ov_hi - m_ov_lo)
			return nullptr;                 // overlay pages are patched copies
		if (!m_base)
			return nullptr;
		const uint32_t po = m_index[page];
		if (po == INDEX_NONE)
			return nullptr;
		return reinterpret_cast<const uint16_t *>(m_base + po);
	}

	// Single byte view for the SPI1 command path (generic_spi_flash model).
	inline uint8_t read8(uint32_t off)
	{
		const uint16_t w = read16(off & ~1u);
		return uint8_t((off & 1u) ? (w >> 8) : (w & 0xffu));
	}

	// NOR page program: only clears bits, exactly like the MAME model's
	// `m_rom[m_addr] &= data`.
	void program_byte(uint32_t off, uint8_t data);
	// 4 KiB sector erase inside the overlay (the SoC's 0x20 command).
	void erase_sector(uint32_t off);

	// ---- persistence (saveimg.cpp) ---------------------------------------
	// Copy every overlay page out: `pages[i]` gets the page number and
	// `dst + i * PAGE_SIZE` its 4 KiB of bytes.  Returns the page count.
	int overlay_export(uint32_t *pages, uint8_t *dst, int max_pages) const;
	// Install a page that came back from flash.  Replaces whatever the overlay
	// held for that page, so a restore always wins over a stale write.
	bool overlay_import(uint32_t page, const uint8_t *src);

	// ---- statistics (logged once per second) ------------------------------
	uint32_t page_loads() const { return m_loads; }
	uint32_t overlay_hits() const { return m_overlay_hits; }
	uint32_t overlay_writes() const { return m_writes; }

private:
	uint16_t read16_miss(uint32_t off, uint32_t page, uint32_t h);
	uint16_t read16_overlay(uint32_t off, uint32_t page);
	int slot_holding(uint32_t page) const;
	uint16_t *overlay_page(uint32_t page, bool create);

	uint32_t m_size = 0;
	Loader m_loader = nullptr;
	void *m_ctx = nullptr;

	// memory mapped sparse store (see set_mapped)
	const uint8_t *m_base = nullptr;
	const uint32_t *m_index = nullptr;
	uint32_t m_ov_lo = 1;
	uint32_t m_ov_hi = 0;

	uint32_t m_tag[SLOTS];
	uint32_t m_overlay_tag[DIRTY_SLOTS];
	int m_overlay_next = 0;

	uint32_t m_loads = 0;
	uint32_t m_overlay_hits = 0;
	uint32_t m_writes = 0;

	alignas(4) uint16_t m_cache[SLOTS * PAGE_WORDS];
	alignas(4) uint16_t m_overlay[DIRTY_SLOTS * PAGE_WORDS];
};
