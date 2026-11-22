// ---------------------------------------------------------------------------
//  gueststate.cpp - ring of full machine snapshots in the `save` partition
//
//  See gueststate.h for the record layout and the wear argument.  The ring is
//  addressed by sequence number modulo GUESTSTATE_RECORDS, so a store always
//  overwrites the oldest record: a power cut that lands in the middle of an
//  erase or a write costs that one record, and the previous seven survive.
// ---------------------------------------------------------------------------
#include "gueststate.h"

#include <cstring>

#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"

namespace {

constexpr const char *TAG = "gueststate";
constexpr uint32_t kMagic = 0x54534744u;         // 'DGST' little endian
constexpr uint16_t kVersion = 1;

#pragma pack(push, 1)
struct RecordHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved0;
    uint32_t sequence;
    uint32_t crc;
    uint32_t length_a;
    uint32_t length_b;
    uint32_t reserved1;
    uint32_t reserved2;
};
#pragma pack(pop)

static_assert(sizeof(RecordHeader) == GUESTSTATE_HEADER_SIZE,
              "record header must match GUESTSTATE_HEADER_SIZE");

const esp_partition_t *s_part = nullptr;
uint32_t s_next_seq = 1;
uint32_t s_newest_seq = 0;

uint32_t record_offset(uint32_t index)
{
    return index * uint32_t(GUESTSTATE_RECORD_SIZE);
}

bool read_header(uint32_t index, RecordHeader &h)
{
    const uint32_t off = record_offset(index);
    if (off + uint32_t(GUESTSTATE_HEADER_SIZE) > s_part->size)
        return false;
    if (esp_partition_read(s_part, off, &h, sizeof(h)) != ESP_OK)
        return false;
    return h.magic == kMagic && h.version == kVersion &&
           h.length_a + h.length_b <= uint32_t(GUESTSTATE_MAX_PAYLOAD);
}

}  // namespace

bool gueststate_init()
{
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                      esp_partition_subtype_t(0x41), "save");
    if (!s_part) {
        ESP_LOGW(TAG, "no `save` partition; snapshots will not persist");
        return false;
    }
    // saveimg.cpp owns the tail of the partition; the ring lives in the front.
    const uint32_t ring_bytes = uint32_t(GUESTSTATE_RECORDS) * uint32_t(GUESTSTATE_RECORD_SIZE);
    if (ring_bytes > s_part->size) {
        ESP_LOGW(TAG, "save partition too small for the snapshot ring (%u < %u)",
                 unsigned(s_part->size), unsigned(ring_bytes));
        s_part = nullptr;
        return false;
    }

    // Scan once so a later store continues the sequence instead of restarting
    // it, which would make the newest record look like the oldest.
    uint32_t best = 0;
    for (int i = 0; i < GUESTSTATE_RECORDS; i++) {
        RecordHeader h{};
        if (read_header(uint32_t(i), h) && h.sequence > best)
            best = h.sequence;
    }
    s_newest_seq = best;
    s_next_seq = best + 1;
    ESP_LOGI(TAG, "ring %d x %u B newest seq=%u", GUESTSTATE_RECORDS,
             unsigned(GUESTSTATE_RECORD_SIZE), unsigned(best));
    return true;
}

bool gueststate_ready()
{
    return s_part != nullptr;
}

uint32_t gueststate_newest_seq()
{
    return s_newest_seq;
}

bool gueststate_store(const void *a, uint32_t len_a, const void *b, uint32_t len_b)
{
    if (!s_part || !a || !b || len_a == 0 || len_b == 0)
        return false;
    if (len_a + len_b > uint32_t(GUESTSTATE_MAX_PAYLOAD))
        return false;

    const uint32_t index = s_next_seq % uint32_t(GUESTSTATE_RECORDS);
    const uint32_t off = record_offset(index);
    if (off + uint32_t(GUESTSTATE_RECORD_SIZE) > s_part->size)
        return false;

    RecordHeader h{};
    h.magic = kMagic;
    h.version = kVersion;
    h.sequence = s_next_seq;
    h.length_a = len_a;
    h.length_b = len_b;
    // Computed straight off the caller's buffers: DRAM has no room for an 8 KiB
    // staging copy just to checksum it.
    uint32_t crc = esp_rom_crc32_le(0, static_cast<const uint8_t *>(a), len_a);
    crc = esp_rom_crc32_le(crc, static_cast<const uint8_t *>(b), len_b);
    h.crc = crc;

    // Erase exactly this record's three sectors; the other seven records and
    // the saveimg slots in the partition tail are untouched.
    if (esp_partition_erase_range(s_part, off, uint32_t(GUESTSTATE_RECORD_SIZE)) != ESP_OK) {
        ESP_LOGE(TAG, "erase record %u failed", unsigned(index));
        return false;
    }

    esp_err_t rc = esp_partition_write(s_part, off, &h, sizeof(h));
    if (rc == ESP_OK)
        rc = esp_partition_write(s_part, off + uint32_t(GUESTSTATE_HEADER_SIZE), a, len_a);
    if (rc == ESP_OK)
        rc = esp_partition_write(s_part, off + uint32_t(GUESTSTATE_HEADER_SIZE) + len_a, b,
                                len_b);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "write record %u failed: %d", unsigned(index), int(rc));
        return false;
    }

    // Verify the header only: re-reading the payload would need a second 8 KiB
    // buffer, and a header whose crc and lengths match what we wrote is the
    // strongest check available without one.
    RecordHeader chk{};
    if (!read_header(index, chk) || chk.sequence != h.sequence || chk.crc != h.crc) {
        ESP_LOGE(TAG, "record %u did not verify", unsigned(index));
        return false;
    }

    s_newest_seq = h.sequence;
    s_next_seq++;
    ESP_LOGI(TAG, "snapshot seq=%u -> record %u (%u + %u B)", unsigned(h.sequence),
             unsigned(index), unsigned(len_a), unsigned(len_b));
    return true;
}

bool gueststate_load(void *a, uint32_t len_a, void *b, uint32_t len_b)
{
    if (!s_part || !a || !b || len_a == 0 || len_b == 0)
        return false;
    if (len_a + len_b > uint32_t(GUESTSTATE_MAX_PAYLOAD))
        return false;

    // Find the newest header that matches the caller's buffer sizes.
    uint32_t best_index = uint32_t(GUESTSTATE_RECORDS);
    uint32_t best_seq = 0;
    for (int i = 0; i < GUESTSTATE_RECORDS; i++) {
        RecordHeader h{};
        if (!read_header(uint32_t(i), h))
            continue;
        if (h.length_a != len_a || h.length_b != len_b)
            continue;
        if (h.sequence > best_seq) {
            best_seq = h.sequence;
            best_index = uint32_t(i);
        }
    }
    if (best_index >= uint32_t(GUESTSTATE_RECORDS)) {
        ESP_LOGI(TAG, "no usable snapshot");
        return false;
    }

    RecordHeader h{};
    if (!read_header(best_index, h))
        return false;
    const uint32_t off = record_offset(best_index);
    if (esp_partition_read(s_part, off + uint32_t(GUESTSTATE_HEADER_SIZE), a, len_a) != ESP_OK) {
        ESP_LOGE(TAG, "read record %u part a failed", unsigned(best_index));
        return false;
    }
    if (esp_partition_read(s_part, off + uint32_t(GUESTSTATE_HEADER_SIZE) + len_a, b, len_b) !=
        ESP_OK) {
        ESP_LOGE(TAG, "read record %u part b failed", unsigned(best_index));
        return false;
    }

    uint32_t crc = esp_rom_crc32_le(0, static_cast<const uint8_t *>(a), len_a);
    crc = esp_rom_crc32_le(crc, static_cast<const uint8_t *>(b), len_b);
    if (crc != h.crc) {
        ESP_LOGW(TAG, "record %u crc %08X != %08X", unsigned(best_index), unsigned(crc),
                 unsigned(h.crc));
        return false;
    }

    s_newest_seq = best_seq;
    s_next_seq = best_seq + 1;
    ESP_LOGI(TAG, "restored snapshot seq=%u from record %u", unsigned(best_seq),
             unsigned(best_index));
    return true;
}
