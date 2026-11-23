// ---------------------------------------------------------------------------
//  fbsave.cpp - one 32 KiB erase block holding the visible panel GRAM
//
//  See fbsave.h for the region map and the frame layout.  The block sits in the
//  gap between the gueststate ring (5 x 12 KiB) and the saveimg slots, so no
//  existing offset moves and both neighbours keep working untouched.
//
//  The load path streams through a 256 B window instead of staging all 28672 B:
//  the C3 only has ~10 KiB of DRAM left, and a full copy would not fit.  The
//  CRC therefore runs over the flash bytes first and the copy only starts once
//  it matches, so a torn write can never leave half a frame on the panel.
// ---------------------------------------------------------------------------
#include "fbsave.h"

#include <cstring>

#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"

namespace {

constexpr const char *TAG = "fbsave";
constexpr uint32_t kMagic = 0x42464744u;        // 'DGFB' little endian
constexpr uint16_t kVersion = 1;
constexpr uint32_t kBase = 0x0F000;             // right after the 5-record ring
constexpr uint32_t kRegion = 0x08000;           // one 32 KiB erase block
constexpr uint32_t kWindow = 256;               // streaming chunk for load

#pragma pack(push, 1)
struct FrameHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved0;
    uint32_t bytes;
    uint32_t crc;
    uint32_t reserved1[4];
};
#pragma pack(pop)

static_assert(sizeof(FrameHeader) == 32, "frame header must be 32 bytes");

const esp_partition_t *s_part = nullptr;

}  // namespace

bool fbsave_init()
{
    if (s_part != nullptr)
        return true;
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                      ESP_PARTITION_SUBTYPE_ANY, "save");
    if (s_part == nullptr) {
        ESP_LOGW(TAG, "no save partition; panel frame will not persist");
        return false;
    }
    if (kBase + kRegion > s_part->size) {
        ESP_LOGW(TAG, "save partition too small (%u B) for the frame block",
                 unsigned(s_part->size));
        s_part = nullptr;
        return false;
    }
    return true;
}

bool fbsave_store(const void *pixels, uint32_t bytes)
{
    if (!fbsave_init())
        return false;
    if (pixels == nullptr || bytes + sizeof(FrameHeader) > kRegion)
        return false;

    FrameHeader h;
    std::memset(&h, 0, sizeof(h));
    h.magic = kMagic;
    h.version = kVersion;
    h.bytes = bytes;
    h.crc = esp_rom_crc32_le(0, static_cast<const uint8_t *>(pixels), bytes);

    esp_err_t err = esp_partition_erase_range(s_part, kBase, kRegion);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "erase failed: %d", int(err));
        return false;
    }
    err = esp_partition_write(s_part, kBase, &h, sizeof(h));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "header write failed: %d", int(err));
        return false;
    }
    err = esp_partition_write(s_part, kBase + sizeof(h), pixels, bytes);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "pixel write failed: %d", int(err));
        return false;
    }
    return true;
}

bool fbsave_load(void *pixels, uint32_t bytes)
{
    if (!fbsave_init())
        return false;
    if (pixels == nullptr || bytes + sizeof(FrameHeader) > kRegion)
        return false;

    FrameHeader h;
    if (esp_partition_read(s_part, kBase, &h, sizeof(h)) != ESP_OK)
        return false;
    if (h.magic != kMagic || h.version != kVersion || h.bytes != bytes)
        return false;

    // Pass 1: CRC over the stored pixels, 256 B at a time.
    uint8_t window[kWindow];
    const uint32_t data_at = kBase + sizeof(h);
    uint32_t crc = 0;
    for (uint32_t done = 0; done < bytes;) {
        const uint32_t n = (bytes - done < kWindow) ? (bytes - done) : kWindow;
        if (esp_partition_read(s_part, data_at + done, window, n) != ESP_OK)
            return false;
        crc = esp_rom_crc32_le(crc, window, n);
        done += n;
    }
    if (crc != h.crc) {
        ESP_LOGW(TAG, "frame crc mismatch; keeping the boot frame");
        return false;
    }

    // Pass 2: copy into the panel, again streaming.
    uint8_t *dst = static_cast<uint8_t *>(pixels);
    for (uint32_t done = 0; done < bytes;) {
        const uint32_t n = (bytes - done < kWindow) ? (bytes - done) : kWindow;
        if (esp_partition_read(s_part, data_at + done, dst + done, n) != ESP_OK)
            return false;
        done += n;
    }
    return true;
}
