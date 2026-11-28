#include "manualsave.h"

#include <cstring>

#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"

namespace {

constexpr const char *TAG = "manualsave";
constexpr uint32_t kMagic = 0x534D4744u;      // 'DGMS' little endian
constexpr uint16_t kVersion = 1;
constexpr uint32_t kOffset = 0x20000;         // right after the two saveimg slots
constexpr uint32_t kRegion = 0x3000;          // three erase sectors
constexpr uint32_t kHeaderSize = 32;

#pragma pack(push, 1)
struct Header {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved0;
    uint32_t tod;
    uint32_t length_a;
    uint32_t length_b;
    uint32_t crc;
    uint32_t reserved1[2];
};
#pragma pack(pop)

static_assert(sizeof(Header) == kHeaderSize, "manual save header must be 32 bytes");

const esp_partition_t *s_part = nullptr;

uint32_t crc_of(const void *a, uint32_t len_a, const void *b, uint32_t len_b)
{
    uint32_t crc = esp_rom_crc32_le(0, static_cast<const uint8_t *>(a), len_a);
    return esp_rom_crc32_le(crc, static_cast<const uint8_t *>(b), len_b);
}

bool read_header(Header &h)
{
    if (!s_part)
        return false;
    if (esp_partition_read(s_part, kOffset, &h, sizeof(h)) != ESP_OK)
        return false;
    return h.magic == kMagic && h.version == kVersion &&
           h.length_a > 0 && h.length_b > 0 &&
           h.length_a + h.length_b + kHeaderSize <= kRegion;
}

}  // namespace

bool manualsave_init()
{
    if (s_part)
        return true;
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                      esp_partition_subtype_t(0x41), "save");
    if (!s_part) {
        ESP_LOGW(TAG, "no `save` partition; manual saves disabled");
        return false;
    }
    if (s_part->size < kOffset + kRegion) {
        // Older partition tables end the save partition at 0x20000.  The slot
        // needs the three sectors after it, so a firmware-only update cannot
        // use manual saves until the partition table is flashed too.
        ESP_LOGW(TAG, "save partition is %u B; manual slot needs %u B",
                 unsigned(s_part->size), unsigned(kOffset + kRegion));
        s_part = nullptr;
        return false;
    }
    return true;
}

bool manualsave_ready()
{
    return manualsave_init();
}

bool manualsave_exists()
{
    Header h{};
    return read_header(h);
}

bool manualsave_store(const void *a, uint32_t len_a, const void *b, uint32_t len_b,
                      uint32_t tod)
{
    if (!manualsave_init() || !a || !b || len_a == 0 || len_b == 0)
        return false;
    if (len_a + len_b + kHeaderSize > kRegion)
        return false;

    Header h{};
    h.magic = kMagic;
    h.version = kVersion;
    h.tod = tod;
    h.length_a = len_a;
    h.length_b = len_b;
    h.crc = crc_of(a, len_a, b, len_b);

    esp_err_t rc = esp_partition_erase_range(s_part, kOffset, kRegion);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "erase failed: %d", int(rc));
        return false;
    }
    rc = esp_partition_write(s_part, kOffset, &h, sizeof(h));
    if (rc == ESP_OK)
        rc = esp_partition_write(s_part, kOffset + kHeaderSize, a, len_a);
    if (rc == ESP_OK)
        rc = esp_partition_write(s_part, kOffset + kHeaderSize + len_a, b, len_b);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "write failed: %d", int(rc));
        return false;
    }

    Header chk{};
    if (!read_header(chk) || chk.crc != h.crc || chk.length_a != len_a ||
        chk.length_b != len_b || chk.tod != tod) {
        ESP_LOGE(TAG, "slot did not verify");
        return false;
    }
    ESP_LOGI(TAG, "stored %u + %u B at 0x%05x", unsigned(len_a), unsigned(len_b),
             unsigned(kOffset));
    return true;
}

bool manualsave_load(void *a, uint32_t len_a, void *b, uint32_t len_b, uint32_t *tod)
{
    if (!manualsave_init() || !a || !b)
        return false;

    Header h{};
    if (!read_header(h))
        return false;
    if (h.length_a != len_a || h.length_b != len_b) {
        ESP_LOGW(TAG, "slot lengths %u/%u do not match %u/%u", unsigned(h.length_a),
                 unsigned(h.length_b), unsigned(len_a), unsigned(len_b));
        return false;
    }

    // Verify before touching the caller's buffers: a torn slot must not leave
    // half of an old save in guest RAM.  The payload is read twice (CRC pass,
    // then copy) because DRAM has no room for an 8 KiB staging block.
    uint8_t chunk[128];
    uint32_t crc = 0;
    uint32_t done = 0;
    const uint32_t total = len_a + len_b;
    while (done < total) {
        const uint32_t n = (total - done < sizeof(chunk)) ? (total - done)
                                                          : uint32_t(sizeof(chunk));
        if (esp_partition_read(s_part, kOffset + kHeaderSize + done, chunk, n) != ESP_OK)
            return false;
        crc = esp_rom_crc32_le(crc, chunk, n);
        done += n;
    }
    if (crc != h.crc) {
        ESP_LOGW(TAG, "slot crc mismatch");
        return false;
    }

    if (esp_partition_read(s_part, kOffset + kHeaderSize, a, len_a) != ESP_OK)
        return false;
    if (esp_partition_read(s_part, kOffset + kHeaderSize + len_a, b, len_b) != ESP_OK)
        return false;
    if (tod)
        *tod = h.tod;
    ESP_LOGI(TAG, "loaded %u + %u B from 0x%05x", unsigned(len_a), unsigned(len_b),
             unsigned(kOffset));
    return true;
}
