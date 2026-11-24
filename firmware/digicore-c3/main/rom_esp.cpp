// ---------------------------------------------------------------------------
//  rom_esp.cpp - `roms` partition -> SparseRom (see rom_esp.h)
// ---------------------------------------------------------------------------
#include "rom_esp.h"

#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_partition.h"

namespace romstore {
namespace {

const char *TAG = "ROMSTORE";

constexpr uint32_t kMagic = 0x31434d44u;   // "DMC1" little endian
constexpr uint32_t kPageSize = 4096;
constexpr uint32_t kMaxPages = 2048;
constexpr uint32_t kIndexNone = 0xffffffffu;

struct __attribute__((packed)) Header
{
	uint32_t magic;
	uint32_t version;
	uint32_t image_size;
	uint8_t  sha1[20];
	uint32_t page_size;
	uint32_t page_count;
	uint32_t bitmap_offset;
	uint32_t index_offset;
	uint32_t data_offset;
	uint32_t flags;
	uint32_t present_pages;
};

const esp_partition_t *s_part = nullptr;
SparseRom s_rom;
uint32_t s_page_count = 0;
uint32_t s_image_size = 0;
// Page offsets, relative to the partition start.  8 KiB of internal RAM so a
// page miss costs exactly one esp_partition_read().
uint32_t s_index[kMaxPages];

void hex20(const uint8_t *in, char *out)
{
	static const char *digits = "0123456789abcdef";
	for (int i = 0; i < 20; i++) {
		out[i * 2] = digits[in[i] >> 4];
		out[i * 2 + 1] = digits[in[i] & 15];
	}
	out[40] = 0;
}

// Page loader handed to SparseRom: one 4 KiB esp_partition_read per miss, or a
// memset for an all-zero page (the 1,437 pages the sparse store omits).
bool load_page(void *ctx, uint32_t page, uint16_t *dst)
{
	(void)ctx;
	if (!s_part || page >= s_page_count) {
		std::memset(dst, 0xff, kPageSize);
		return true;
	}

	const uint32_t off = s_index[page];
	if (off == kIndexNone) {
		std::memset(dst, 0x00, kPageSize);
		return true;
	}

	if (esp_partition_read(s_part, off, dst, kPageSize) != ESP_OK) {
		std::memset(dst, 0xff, kPageSize);
		return false;
	}
	return true;
}

} // namespace

bool mount(Info *out)
{
	if (out)
		*out = Info{};

	s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
	                                  esp_partition_subtype_t(0x40), "roms");
	if (!s_part) {
		ESP_LOGE(TAG, "partition `roms` (data/0x40) not found - check partitions.csv");
		return false;
	}

	Header hdr;
	if (esp_partition_read(s_part, 0, &hdr, sizeof(hdr)) != ESP_OK) {
		ESP_LOGE(TAG, "cannot read the sparse header at 0x%06x", unsigned(s_part->address));
		return false;
	}
	if (hdr.magic != kMagic) {
		ESP_LOGE(TAG, "bad magic %08x (expected DMC1) - run tools/make_sparse_rom.py "
		              "and flash the sparse image at 0x210000", unsigned(hdr.magic));
		return false;
	}
	if (hdr.page_size != kPageSize || hdr.page_count == 0 || hdr.page_count > kMaxPages) {
		ESP_LOGE(TAG, "unsupported sparse geometry: page_size=%u page_count=%u",
		         unsigned(hdr.page_size), unsigned(hdr.page_count));
		return false;
	}

	s_page_count = hdr.page_count;
	s_image_size = hdr.image_size;
	if (esp_partition_read(s_part, hdr.index_offset, s_index,
	                       s_page_count * 4) != ESP_OK) {
		ESP_LOGE(TAG, "cannot read the page index at 0x%06x",
		         unsigned(s_part->address + hdr.index_offset));
		return false;
	}

	s_rom.begin(hdr.image_size, load_page, nullptr);

	// Map the whole partition and serve guest fetches straight out of the
	// flash-mapped window: no page cache misses, no esp_partition_read() on the
	// hot path.  Without this the 193 KiB of guest code thrash the 16 entry
	// (64 KiB) page cache and the interpreter drops to ~0.7 M instr/s.
	//
	// Order matters for DRAM.  A mapped store never reads the page cache, so the
	// cache is only allocated when the mapping is refused: on a 320 KiB budget
	// already at 99.6% that is a 64 KiB difference, and it is the same 64 KiB
	// that later buys the decoder/hot-region work some room.
	const void *mapped = nullptr;
	esp_partition_mmap_handle_t handle = 0;
	const esp_err_t err = esp_partition_mmap(s_part, 0, s_part->size,
	                                         ESP_PARTITION_MMAP_DATA, &mapped, &handle);
	if (err == ESP_OK) {
		s_rom.begin(hdr.image_size, load_page, nullptr);
		s_rom.set_mapped(static_cast<const uint8_t *>(mapped), s_index);
		ESP_LOGI(TAG, "sparse store mapped at %p (%u KiB), direct read path on, "
		              "no page cache allocated",
		         mapped, unsigned(s_part->size / 1024));
	} else {
		if (!s_rom.alloc_cache()) {
			ESP_LOGE(TAG, "esp_partition_mmap failed (%s) and the 64 KiB page "
			              "cache cannot be allocated", esp_err_to_name(err));
			return false;
		}
		s_rom.begin(hdr.image_size, load_page, nullptr);
		ESP_LOGW(TAG, "esp_partition_mmap failed (%s), falling back to the "
		              "64 KiB page cache", esp_err_to_name(err));
	}

	char sha1[41];
	hex20(hdr.sha1, sha1);

	ESP_LOGI(TAG, "partition `roms` data/0x40 at 0x%06x, %u KiB",
	         unsigned(s_part->address), unsigned(s_part->size / 1024));
	ESP_LOGI(TAG, "sparse v%u: image %u B, %u pages (%u present), data at +0x%04x",
	         unsigned(hdr.version), unsigned(hdr.image_size),
	         unsigned(hdr.page_count), unsigned(hdr.present_pages),
	         unsigned(hdr.data_offset));
	ESP_LOGI(TAG, "image sha1 %s", sha1);

	if (out) {
		out->ok = true;
		out->version = hdr.version;
		out->image_size = hdr.image_size;
		out->page_size = hdr.page_size;
		out->page_count = hdr.page_count;
		out->pages_present = hdr.present_pages;
		out->part_offset = s_part->address;
		out->part_size = s_part->size;
		out->data_offset = hdr.data_offset;
		std::memcpy(out->sha1, sha1, sizeof(out->sha1));
	}
	return true;
}

SparseRom *image()
{
	return s_part ? &s_rom : nullptr;
}

} // namespace romstore
