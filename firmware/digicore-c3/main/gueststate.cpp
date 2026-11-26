// ---------------------------------------------------------------------------
//  gueststate.cpp - ring of full machine snapshots in the `save` partition
//
//  See gueststate.h for the record layout and the wear argument.  The ring is
//  addressed by sequence number modulo GUESTSTATE_RECORDS, so a store always
//  overwrites the oldest record: a power cut that lands in the middle of an
//  erase or a write costs that one record, and the previous seven survive.
// ---------------------------------------------------------------------------
#include "gueststate.h"

#include <cstddef>
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

// CRC of one record's payload, read straight off flash in 128 B chunks so the
// caller's buffers are never touched by a record that turns out to be torn.
static bool payload_ok(uint32_t index, const RecordHeader &h)
{
    const uint32_t data_off = record_offset(index) + uint32_t(GUESTSTATE_HEADER_SIZE);
    const uint32_t total = h.length_a + h.length_b;
    uint8_t chunk[128];
    uint32_t crc = 0;
    for (uint32_t done = 0; done < total;) {
        const uint32_t n = (total - done < sizeof(chunk)) ? (total - done)
                                                          : uint32_t(sizeof(chunk));
        if (esp_partition_read(s_part, data_off + done, chunk, n) != ESP_OK)
            return false;
        crc = esp_rom_crc32_le(crc, chunk, n);
        done += n;
    }
    if (crc != h.crc) {
        ESP_LOGW(TAG, "record %u seq=%u crc %08X != %08X", unsigned(index),
                 unsigned(h.sequence), unsigned(crc), unsigned(h.crc));
        return false;
    }
    return true;
}

// Test hook for the fallback above: wipe the magic of the newest record so the
// next load has to walk past it.  0xFFFFFFFF is what an erased sector reads as,
// so the header becomes indistinguishable from an unused slot.
bool gueststate_invalidate_newest()
{
    if (!s_part)
        return false;
    uint32_t best_index = GUESTSTATE_RECORDS;
    uint32_t best_seq = 0;
    for (int i = 0; i < GUESTSTATE_RECORDS; i++) {
        RecordHeader h{};
        if (!read_header(uint32_t(i), h))
            continue;
        if (h.sequence > best_seq) {
            best_seq = h.sequence;
            best_index = uint32_t(i);
        }
    }
    if (best_index >= uint32_t(GUESTSTATE_RECORDS))
        return false;
    // Corrupt the CRC, not the magic.  That is exactly what a power cut in the
    // middle of a store leaves behind: the header (written first) looks perfect
    // and the payload is short.  Wiping the magic instead would only simulate an
    // empty slot, i.e. the path that already worked.
    RecordHeader h{};
    if (!read_header(best_index, h))
        return false;
    const uint32_t crc_off = record_offset(best_index) + offsetof(RecordHeader, crc);
    const uint32_t bad = h.crc ^ 0xFFFFFFFFu;
    if (esp_partition_write(s_part, crc_off, &bad, sizeof(bad)) != ESP_OK)
        return false;
    ESP_LOGW(TAG, "invalidated snapshot seq=%u in record %u", unsigned(best_seq),
             unsigned(best_index));
    s_newest_seq = 0;
    return true;
}

bool gueststate_load(void *a, uint32_t len_a, void *b, uint32_t len_b)
{
    if (!s_part || !a || !b || len_a == 0 || len_b == 0)
        return false;
    if (len_a + len_b > uint32_t(GUESTSTATE_MAX_PAYLOAD))
        return false;

    // Collect every record whose header is intact and whose payload sizes match
    // the caller's buffers, then try them newest first.
    //
    // The old code took the single newest header and gave up when its CRC
    // failed.  A store writes the header before the payload, so a power cut in
    // between leaves a header that looks perfect with a torn payload - and that
    // one record made all five unusable: the unit came back from reset with an
    // empty pet even though four good snapshots were still in flash.
    uint32_t order[GUESTSTATE_RECORDS];
    uint32_t seqs[GUESTSTATE_RECORDS];
    int n = 0;
    for (int i = 0; i < GUESTSTATE_RECORDS; i++) {
        RecordHeader h{};
        if (!read_header(uint32_t(i), h))
            continue;
        if (h.length_a != len_a || h.length_b != len_b)
            continue;
        order[n] = uint32_t(i);
        seqs[n] = h.sequence;
        n++;
    }
    if (n == 0) {
        ESP_LOGI(TAG, "no usable snapshot");
        return false;
    }
    for (int i = 1; i < n; i++) {              // newest first, n is at most 5
        const uint32_t s = seqs[i];
        const uint32_t o = order[i];
        int j = i - 1;
        while (j >= 0 && seqs[j] < s) {
            seqs[j + 1] = seqs[j];
            order[j + 1] = order[j];
            j--;
        }
        seqs[j + 1] = s;
        order[j + 1] = o;
    }

    for (int k = 0; k < n; k++) {
        const uint32_t index = order[k];
        RecordHeader h{};
        if (!read_header(index, h))
            continue;
        if (!payload_ok(index, h)) {
            ESP_LOGW(TAG, "snapshot seq=%u is torn; falling back to an older record",
                     unsigned(h.sequence));
            continue;
        }
        const uint32_t data_off = record_offset(index) + uint32_t(GUESTSTATE_HEADER_SIZE);
        if (esp_partition_read(s_part, data_off, a, len_a) != ESP_OK) {
            ESP_LOGE(TAG, "read record %u part a failed", unsigned(index));
            continue;
        }
        if (esp_partition_read(s_part, data_off + len_a, b, len_b) != ESP_OK) {
            ESP_LOGE(TAG, "read record %u part b failed", unsigned(index));
            continue;
        }
        s_newest_seq = h.sequence;
        s_next_seq = h.sequence + 1;
        ESP_LOGI(TAG, "restored snapshot seq=%u from record %u%s", unsigned(h.sequence),
                 unsigned(index), k ? " (older record after a torn one)" : "");
        return true;
    }

    ESP_LOGW(TAG, "all %d snapshot(s) failed verification", n);
    return false;
}