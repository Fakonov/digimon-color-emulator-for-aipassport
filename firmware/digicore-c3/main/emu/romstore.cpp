// ---------------------------------------------------------------------------
//  romstore.cpp - cold paths of the sparse page cache (see romstore.h).
// ---------------------------------------------------------------------------
#include "romstore.h"

void SparseRom::clear()
{
	for (int i = 0; i < SLOTS; i++)
		m_tag[i] = INDEX_NONE;
	for (int i = 0; i < DIRTY_SLOTS; i++)
		m_overlay_tag[i] = INDEX_NONE;
	m_overlay_next = 0;
	m_loads = 0;
	m_overlay_hits = 0;
	m_writes = 0;
	// lo > hi is the empty range: read16() and page_window() test it directly.
	m_ov_lo = 0xffffffffu;
	m_ov_hi = 0;
#ifdef ESP_PLATFORM
	// Null until alloc_cache(): a mapped build never has a page cache at all.
	if (m_cache)
		std::memset(m_cache, 0, size_t(SLOTS) * PAGE_WORDS * 2u);
#else
	std::memset(m_cache, 0, sizeof(m_cache));
#endif
	std::memset(m_overlay, 0, sizeof(m_overlay));
}

void SparseRom::begin(uint32_t image_size, Loader loader, void *ctx)
{
	m_size = image_size;
	m_loader = loader;
	m_ctx = ctx;
	clear();
}

int SparseRom::slot_holding(uint32_t page) const
{
	for (int i = 0; i < SLOTS; i++)
		if (m_tag[i] == page)
			return i;
	return -1;
}

uint16_t SparseRom::read16_miss(uint32_t off, uint32_t page, uint32_t h)
{
	if (!m_cache)
		return 0xffffu;                 // unreachable unless the store is unmounted
	uint16_t *dst = m_cache + h * PAGE_WORDS;

	// A written page must be served from the overlay, not from flash.
	for (int i = 0; i < DIRTY_SLOTS; i++) {
		if (m_overlay_tag[i] == page) {
			std::memcpy(dst, m_overlay + i * PAGE_WORDS, PAGE_SIZE);
			m_overlay_hits++;
			m_tag[h] = page;
			return dst[(off >> 1) & (PAGE_WORDS - 1)];
		}
	}

	if (!m_loader || !m_loader(m_ctx, page, dst))
		std::memset(dst, 0xff, PAGE_SIZE);
	else
		m_loads++;

	m_tag[h] = page;
	return dst[(off >> 1) & (PAGE_WORDS - 1)];
}

uint16_t SparseRom::read16_overlay(uint32_t off, uint32_t page)
{
	for (int i = 0; i < DIRTY_SLOTS; i++) {
		if (m_overlay_tag[i] != page)
			continue;
		m_overlay_hits++;
		uint16_t w;
		std::memcpy(&w, reinterpret_cast<const uint8_t *>(m_overlay + i * PAGE_WORDS) +
		                    (off & (PAGE_SIZE - 1)), 2);
		return w;
	}
	// Not (yet) a written page: fall through to the normal path.
	if (m_base) {
		const uint32_t po = m_index[page];
		if (po == INDEX_NONE)
			return 0;
		uint16_t w;
		std::memcpy(&w, m_base + po + (off & (PAGE_SIZE - 1)), 2);
		return w;
	}
	const uint32_t h = (page ^ (page >> 4)) & uint32_t(SLOTS - 1);
	if (!m_cache)
		return 0xffffu;
	if (m_tag[h] != page)
		return read16_miss(off, page, h);
	return m_cache[h * PAGE_WORDS + ((off >> 1) & (PAGE_WORDS - 1))];
}

uint16_t *SparseRom::overlay_page(uint32_t page, bool create)
{
	for (int i = 0; i < DIRTY_SLOTS; i++)
		if (m_overlay_tag[i] == page)
			return m_overlay + i * PAGE_WORDS;

	if (!create)
		return nullptr;

	int slot = -1;
	for (int i = 0; i < DIRTY_SLOTS; i++) {
		if (m_overlay_tag[i] == INDEX_NONE) {
			slot = i;
			break;
		}
	}
	if (slot < 0) {
		// The game only ever touches two pages; if that assumption breaks we
		// recycle round robin and lose the oldest write (documented).
		slot = m_overlay_next;
		m_overlay_next = (m_overlay_next + 1) % DIRTY_SLOTS;
	}

	uint16_t *dst = m_overlay + slot * PAGE_WORDS;
	if (!m_loader || !m_loader(m_ctx, page, dst))
		std::memset(dst, 0xff, PAGE_SIZE);
	m_overlay_tag[slot] = page;
	if (page < m_ov_lo)
		m_ov_lo = page;
	if (page > m_ov_hi)
		m_ov_hi = page;
	return dst;
}

void SparseRom::program_byte(uint32_t off, uint8_t data)
{
	if (off >= m_size)
		return;

	uint16_t *ov = overlay_page(off >> PAGE_BITS, true);
	if (!ov)
		return;

	const uint32_t wi = (off >> 1) & (PAGE_WORDS - 1);
	const bool hi = (off & 1u) != 0;
	const uint8_t cur = hi ? uint8_t(ov[wi] >> 8) : uint8_t(ov[wi] & 0xffu);
	const uint8_t nv = uint8_t(cur & data);          // NOR: programming clears bits
	ov[wi] = hi ? uint16_t((ov[wi] & 0x00ffu) | (uint16_t(nv) << 8))
	            : uint16_t((ov[wi] & 0xff00u) | nv);

	// Keep a resident cache line coherent so the fast path stays correct.
	const int s = slot_holding(off >> PAGE_BITS);
	if (s >= 0 && m_cache)
		m_cache[s * PAGE_WORDS + wi] = ov[wi];

	m_writes++;
}

void SparseRom::erase_sector(uint32_t off)
{
	const uint32_t page = (off & ~(PAGE_SIZE - 1u)) >> PAGE_BITS;
	if (page >= (m_size >> PAGE_BITS))
		return;

	uint16_t *ov = overlay_page(page, true);
	if (!ov)
		return;

	for (uint32_t i = 0; i < PAGE_WORDS; i++)
		ov[i] = 0xffffu;

	const int s = slot_holding(page);
	if (s >= 0 && m_cache) {
		for (uint32_t i = 0; i < PAGE_WORDS; i++)
			m_cache[s * PAGE_WORDS + i] = 0xffffu;
	}
	m_writes++;
}

int SparseRom::overlay_export(uint32_t *pages, uint8_t *dst, int max_pages) const
{
	int n = 0;
	for (int i = 0; i < DIRTY_SLOTS && n < max_pages; i++) {
		if (m_overlay_tag[i] == INDEX_NONE)
			continue;
		pages[n] = m_overlay_tag[i];
		std::memcpy(dst + size_t(n) * PAGE_SIZE, m_overlay + i * PAGE_WORDS, PAGE_SIZE);
		n++;
	}
	return n;
}

bool SparseRom::overlay_import(uint32_t page, const uint8_t *src)
{
	if (!src || page >= (m_size >> PAGE_BITS))
		return false;

	uint16_t *ov = overlay_page(page, true);
	if (!ov)
		return false;

	std::memcpy(ov, src, PAGE_SIZE);

	// Count it like a guest program would, so persist_save_image() notices the
	// change and starts its settle timer.  Without this an injected image sits
	// in the overlay and never reaches the partition.
	m_writes++;

	// Keep a resident cache line coherent, exactly like program_byte() does.
	const int s = slot_holding(page);
	if (s >= 0 && m_cache)
		std::memcpy(m_cache + s * PAGE_WORDS, ov, PAGE_SIZE);

	return true;
}
