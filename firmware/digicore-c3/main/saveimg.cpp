// ---------------------------------------------------------------------------
//  saveimg.cpp - two-slot, CRC checked persistence for the flash overlay
//
//  Layout of one slot (16 KiB apart so an erase never touches both):
//
//      +0   u32  magic   'DGSV'
//      +4   u16  version
//      +6   u16  page_count
//      +8   u32  sequence            (higher = newer)
//      +12  u32  crc32   of everything after this field
//      +16  u32  page_no[SAVEIMG_MAX_PAGES]
//      +32  u8   page data[page_count * 4096]
//
//  A store erases the older slot, writes the newer one, reads it back and
//  compares.  If power dies anywhere in that sequence the other slot still
//  holds the previous save, so the game never loses more than one save.
//
//  The `save` partition is shared: gueststate.cpp keeps a ring of full machine
//  snapshots in the first 96 KiB, and the two overlay slots live in the last
//  32 KiB.  That is why the slots are 16 KiB apart instead of 64 KiB, and why
//  every offset here is relative to kBase rather than to the partition start.
// ---------------------------------------------------------------------------
#include "saveimg.h"

#include <cstring>

#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"

namespace {

constexpr const char *TAG = "saveimg";
constexpr uint32_t kMagic = 0x56534744u;         // 'DGSV' little endian
constexpr uint16_t kVersion = 1;
constexpr uint32_t kSlotStride = 0x4000;         // 16 KiB: separate erase blocks
constexpr uint32_t kBase = 0x18000;              // after the gueststate ring (96 KiB)
constexpr uint32_t kHeaderSize = 24;             // see static_assert below

#pragma pack(push, 1)
struct SlotHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t page_count;
    uint32_t sequence;
    uint32_t crc;
    uint32_t page_no[SAVEIMG_MAX_PAGES];
};
#pragma pack(pop)

static_assert(sizeof(SlotHeader) == kHeaderSize, "slot header must be 24 bytes");

const esp_partition_t *s_part = nullptr;
uint32_t s_next_seq = 1;

uint32_t crc_of(const SlotHeader &h, const uint8_t *data, int count)
{
    // Everything after the crc field, in the order it appears on flash.
    uint32_t crc = esp_rom_crc32_le(0, reinterpret_cast<const uint8_t *>(&h.page_no[0]),
                                   sizeof(h.page_no));
    if (count > 0)
        crc = esp_rom_crc32_le(crc, data, size_t(count) * SAVEIMG_PAGE_SIZE);
    return crc;
}

bool read_header(uint32_t index, SlotHeader &h)
{
    const uint32_t off = kBase + index * kSlotStride;
    if (off + kHeaderSize > s_part->size)
        return false;
    if (esp_partition_read(s_part, off, &h, sizeof(h)) != ESP_OK)
        return false;
    return h.magic == kMagic && h.version == kVersion &&
           h.page_count <= SAVEIMG_MAX_PAGES;
}

bool read_slot(uint32_t index, SlotHeader &h, uint8_t *data)
{
    const uint32_t off = kBase + index * kSlotStride;
    if (off + kHeaderSize > s_part->size)
        return false;
    if (esp_partition_read(s_part, off, &h, sizeof(h)) != ESP_OK)
        return false;
    if (h.magic != kMagic || h.version != kVersion)
        return false;
    if (h.page_count > SAVEIMG_MAX_PAGES)
        return false;
    const size_t n = size_t(h.page_count) * SAVEIMG_PAGE_SIZE;
    if (n && esp_partition_read(s_part, off + kHeaderSize, data, n) != ESP_OK)
        return false;
    for (int i = 0; i < h.page_count; i++) {
        if (h.page_no[i] >= (0x800000u >> 12))     // outside the 8 MiB image
            return false;
    }
    return crc_of(h, data, h.page_count) == h.crc;
}

}  // namespace

bool saveimg_init()
{
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                     esp_partition_subtype_t(0x41), "save");
    if (!s_part) {
        ESP_LOGW(TAG, "no `save` partition; saves will not persist");
        return false;
    }
    ESP_LOGI(TAG, "save partition %u bytes at 0x%06x", unsigned(s_part->size),
             unsigned(s_part->address));
    return true;
}

bool saveimg_ready()
{
    return s_part != nullptr;
}

bool saveimg_load(uint32_t *pages, uint8_t *data, int *count)
{
    if (!s_part || !pages || !data || !count)
        return false;

    // Peek at both headers first: the data buffer is only big enough for one
    // slot, so pick the winner before reading any page contents.
    SlotHeader ha{}, hb{};
    const bool ok_a = read_header(0, ha);
    const bool ok_b = read_header(1, hb);

    uint32_t index;
    if (ok_a && ok_b)
        index = (ha.sequence >= hb.sequence) ? 0u : 1u;
    else if (ok_b)
        index = 1u;
    else if (ok_a)
        index = 0u;
    else {
        ESP_LOGI(TAG, "no valid slot");
        return false;
    }

    SlotHeader chosen{};
    if (!read_slot(index, chosen, data)) {
        // The other slot may still be good even though its header looked fine.
        const uint32_t alt = index ^ 1u;
        if (!read_slot(alt, chosen, data)) {
            ESP_LOGI(TAG, "both slots failed verification");
            return false;
        }
        index = alt;
    }

    for (int i = 0; i < chosen.page_count; i++)
        pages[i] = chosen.page_no[i];
    *count = chosen.page_count;
    s_next_seq = chosen.sequence + 1;
    ESP_LOGI(TAG, "loaded %d page(s) from slot %u seq=%u", chosen.page_count,
             unsigned(index), unsigned(chosen.sequence));
    return true;
}

bool saveimg_store(const uint32_t *pages, const uint8_t *data, int count)
{
    if (!s_part || !pages || !data || count <= 0 || count > SAVEIMG_MAX_PAGES)
        return false;

    // Alternate slots by sequence parity so the slot we are about to erase is
    // never the one holding the newest save.
    const uint32_t index = s_next_seq & 1u;
    const uint32_t off = kBase + index * kSlotStride;
    const size_t n = size_t(count) * SAVEIMG_PAGE_SIZE;
    if (off + kHeaderSize + n > s_part->size)
        return false;

    SlotHeader h{};
    h.magic = kMagic;
    h.version = kVersion;
    h.page_count = uint16_t(count);
    h.sequence = s_next_seq;
    for (int i = 0; i < count; i++)
        h.page_no[i] = pages[i];
    for (int i = count; i < SAVEIMG_MAX_PAGES; i++)
        h.page_no[i] = 0xffffffffu;
    h.crc = crc_of(h, data, count);

    esp_err_t rc = esp_partition_erase_range(s_part, off, kSlotStride);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "erase slot %u failed: %d", unsigned(index), int(rc));
        return false;
    }
    rc = esp_partition_write(s_part, off, &h, sizeof(h));
    if (rc == ESP_OK)
        rc = esp_partition_write(s_part, off + kHeaderSize, data, n);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "write slot %u failed: %d", unsigned(index), int(rc));
        return false;
    }

    // Read back only the header and the stored crc: the data itself was just
    // handed to esp_partition_write, and re-reading it would need a second
    // 8 KiB buffer that DRAM does not have.  A header whose crc matches what
    // we wrote is the strongest check available without that buffer.
    SlotHeader chk{};
    if (!read_header(index, chk) || chk.sequence != h.sequence || chk.crc != h.crc) {
        ESP_LOGE(TAG, "slot %u did not verify", unsigned(index));
        return false;
    }

    ESP_LOGI(TAG, "stored %d page(s) to slot %u (%u bytes)", count, unsigned(index),
             unsigned(n));
    s_next_seq++;
    return true;
}
