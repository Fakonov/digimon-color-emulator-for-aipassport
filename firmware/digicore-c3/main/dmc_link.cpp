// DMC1 transfer protocol - see dmc_link.h for the wire format.
//
// Two jobs, one cable:
//
//   ROM   The published build has no ROM in it.  The browser parses the user's
//         8 MiB image, drops the all-zero pages and sends only the 611 that
//         carry data, each as a 4 KiB PAGE frame.  We write them into the
//         `roms` partition as the same sparse store tools/make_sparse_rom.py
//         produces, so rom_esp.cpp cannot tell the difference.
//
//   SAVE  The `save` partition is 128 KiB of two 64 KiB slots; it is moved as an
//         opaque blob.  Interpreting saveimg.cpp's slot format here would mean
//         two parsers for one format, and a bug in either one eats the save.
//
// Erase budget: the partition is 2.5 MiB and NOR has to be erased before it is
// programmed.  64 KiB blocks are erased one step ahead of the write cursor, so
// the cost is spread over the transfer instead of stalling at the start
// (~40 block erases, 2.6 s total measured on this part).

#include "dmc_link.h"

#include <cstdio>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/uart_ll.h"
#include "hal/usb_serial_jtag_ll.h"

#include "rom_esp.h"
#include "saveimg.h"
#include "wallclock.h"

namespace {

const char *TAG = "DMC1";

constexpr uint32_t kPageSize = 4096;
constexpr uint32_t kMaxPages = 2048;          // the index is sized for 8 MiB
constexpr uint32_t kBitmapOffset = 0x1000;
constexpr uint32_t kIndexOffset = 0x1100;
constexpr uint32_t kDataOffset = 0x3100;
constexpr uint32_t kCartridgeSize = 8u * 1024u * 1024u;   // the retail SPI flash
constexpr uint32_t kEraseBlock = 64 * 1024;
// header + bitmap + index rounded up to a flash sector: 0x3100 is 12544 bytes,
// which esp_partition_erase_range rejects because the length has to be a
// multiple of the 4 KiB sector size (it returns INVALID_ARG, not a partial erase).
constexpr uint32_t kHeaderErase = 0x4000;
constexpr uint32_t kMagic = 0x31434d44u;      // "DMC1" little endian
constexpr uint32_t kStoreVersion = 1;

struct __attribute__((packed)) StoreHeader
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

enum class Mode { Idle, Rom, Save };

const esp_partition_t *s_rom_part = nullptr;
const esp_partition_t *s_save_part = nullptr;

Mode     s_mode = Mode::Idle;
bool     s_rom_ok = false;
uint32_t s_image_size = 0;
uint32_t s_page_count = 0;
uint32_t s_present = 0;
uint32_t s_cursor = 0;          // next byte to program inside the partition
uint32_t s_erased_to = 0;       // everything below this is already erased
uint32_t s_done = 0;            // frames accepted in this transfer
uint8_t  s_sha1[20] = {};
bool     s_have_sha1 = false;

uint32_t *s_index = nullptr;    // page_count * 4, allocated per transfer
uint8_t   s_bitmap[kMaxPages / 8];
alignas(4) uint8_t s_page[kPageSize];
alignas(4) uint8_t s_header[kPageSize];

uint8_t hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return uint8_t(c - '0');
    if (c >= 'a' && c <= 'f') return uint8_t(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return uint8_t(c - 'A' + 10);
    return 0xff;
}

bool parse_sha1(const char *hex, uint8_t out[20])
{
    if (!hex || std::strlen(hex) != 40)
        return false;
    for (int i = 0; i < 20; i++) {
        const uint8_t hi = hex_nibble(hex[i * 2]);
        const uint8_t lo = hex_nibble(hex[i * 2 + 1]);
        if (hi > 0xf || lo > 0xf)
            return false;
        out[i] = uint8_t((hi << 4) | lo);
    }
    return true;
}

// Blocking read of exactly `len` bytes from either console front end.  The line
// that announced the frame has already been consumed by debug_poll(), so the
// FIFO starts at the first raw byte.
bool read_exact(uint8_t *dst, uint32_t len, uint32_t timeout_ms)
{
    uint32_t got = 0;
    const int64_t deadline = esp_timer_get_time() + int64_t(timeout_ms) * 1000;
    while (got < len) {
        uint8_t b = 0;
        bool had = false;
        if (uart_ll_get_rxfifo_len(&UART0) > 0) {
            uart_ll_read_rxfifo(&UART0, &b, 1);
            had = true;
        } else if (usb_serial_jtag_ll_rxfifo_data_available() > 0) {
            if (usb_serial_jtag_ll_read_rxfifo(&b, 1) > 0)
                had = true;
        }
        if (had) {
            dst[got++] = b;
            continue;
        }
        if (esp_timer_get_time() > deadline) {
            ESP_LOGE(TAG, "frame timeout at %u/%u B", unsigned(got), unsigned(len));
            return false;
        }
    }
    return true;
}

// Erase one 64 KiB block ahead of the cursor.  Never touches bytes that are
// already programmed: the cursor only ever moves forward.
bool ensure_erased(uint32_t off, uint32_t len)
{
    if (off + len <= s_erased_to)
        return true;
    uint32_t start = off & ~(kEraseBlock - 1);
    if (start < s_erased_to)
        start = s_erased_to;
    uint32_t end = (off + len + kEraseBlock - 1) & ~(kEraseBlock - 1);
    if (end > s_rom_part->size)
        end = s_rom_part->size;
    if (start >= end)
        return true;
    const esp_err_t err = esp_partition_erase_range(s_rom_part, start, end - start);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "erase 0x%05x+%u failed: %s", unsigned(start),
                 unsigned(end - start), esp_err_to_name(err));
        return false;
    }
    s_erased_to = end;
    return true;
}

void release_transfer()
{
    if (s_index) {
        heap_caps_free(s_index);
        s_index = nullptr;
    }
    s_mode = Mode::Idle;
}

void fail(const char *why)
{
    ESP_LOGE(TAG, "%s", why);
    std::printf("DMC1 ERR %s\n", why);
    std::fflush(stdout);
    release_transfer();
}

void rom_begin(uint32_t image_size, uint32_t pages, const char *sha1_hex)
{
    if (s_mode != Mode::Idle) {
        fail("busy");
        return;
    }
    s_rom_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                          esp_partition_subtype_t(0x40), "roms");
    if (!s_rom_part) {
        fail("no roms partition");
        return;
    }
    // Exactly 8 MiB, not "up to".  A truncated image mounts happily - magic
    // matches, page_count agrees with image_size - and then the guest reads past
    // the end of the cartridge and runs off into 0x00.  The 64 KiB internal mask
    // ROM that sits next to a dump in most ROM folders is 4 KiB aligned and would
    // otherwise pass every other check.
    if (image_size != kCartridgeSize) {
        fail("image must be 8388608 bytes");
        return;
    }
    const uint32_t count = (image_size + kPageSize - 1) / kPageSize;
    const uint32_t capacity = (s_rom_part->size - kDataOffset) / kPageSize;
    if (pages == 0 || pages > count || pages > capacity) {
        // capacity is 637 pages on the 2.5 MiB partition: a retail image needs
        // 611, so a real cartridge always fits and a wrong file does not.
        fail("too many pages");
        return;
    }
    s_index = static_cast<uint32_t *>(heap_caps_malloc(kMaxPages * 4, MALLOC_CAP_8BIT));
    if (!s_index) {
        fail("no ram for the page index");
        return;
    }
    std::memset(s_index, 0xff, kMaxPages * 4);
    std::memset(s_bitmap, 0, sizeof(s_bitmap));
    s_image_size = image_size;
    s_page_count = count;
    s_present = 0;
    s_cursor = kDataOffset;
    s_done = 0;
    s_have_sha1 = parse_sha1(sha1_hex, s_sha1);
    // header + bitmap + index are rewritten at END; wipe them now so a failed
    // transfer can never be mistaken for a complete store.
    if (esp_partition_erase_range(s_rom_part, 0, kHeaderErase) != ESP_OK) {
        fail("cannot erase the store header");
        return;
    }
    s_erased_to = kHeaderErase;
    s_mode = Mode::Rom;
    ESP_LOGI(TAG, "rom upload: %u B, %u pages, %u non-zero, sha1 %s",
             unsigned(image_size), unsigned(count), unsigned(pages),
             s_have_sha1 ? sha1_hex : "(none)");
    std::printf("DMC1 OK BEGIN\n");
}

void rom_end()
{
    StoreHeader h;
    std::memset(&h, 0, sizeof(h));
    h.magic = kMagic;
    h.version = kStoreVersion;
    h.image_size = s_image_size;
    if (s_have_sha1)
        std::memcpy(h.sha1, s_sha1, sizeof(h.sha1));
    h.page_size = kPageSize;
    h.page_count = s_page_count;
    h.bitmap_offset = kBitmapOffset;
    h.index_offset = kIndexOffset;
    h.data_offset = kDataOffset;
    h.flags = 0;
    h.present_pages = s_present;

    std::memset(s_header, 0, sizeof(s_header));
    std::memcpy(s_header, &h, sizeof(h));

    esp_err_t err = esp_partition_write(s_rom_part, 0, s_header, kPageSize);
    if (err == ESP_OK)
        err = esp_partition_write(s_rom_part, kBitmapOffset, s_bitmap, sizeof(s_bitmap));
    if (err == ESP_OK)
        err = esp_partition_write(s_rom_part, kIndexOffset, s_index, s_page_count * 4);
    if (err != ESP_OK) {
        fail("header write failed");
        return;
    }

    // Re-mount from flash so the next boot has a store rom_esp.cpp accepts.
    romstore::Info info;
    const bool mounted = romstore::mount(&info);
    s_rom_ok = mounted;
    release_transfer();
    ESP_LOGI(TAG, "rom upload done: %u pages present, mount %s", unsigned(s_present),
             mounted ? "ok" : "FAILED");
    std::printf("DMC1 OK END\n");
    std::fflush(stdout);
    // The running engine still holds the *old* mapping and the old page index;
    // there is no in-place way to re-seat it, so the upload ends with a reset.
    // 300 ms is enough for the reply to leave the USB FIFO.
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
}

void save_begin(uint32_t size)
{
    s_save_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                           esp_partition_subtype_t(0x41), "save");
    if (!s_save_part) {
        fail("no save partition");
        return;
    }
    if (size == 0 || size > s_save_part->size) {
        fail("bad save size");
        return;
    }
    const esp_err_t err = esp_partition_erase_range(s_save_part, 0, size);
    if (err != ESP_OK) {
        fail("cannot erase the save slot");
        return;
    }
    s_cursor = 0;
    s_done = 0;
    s_mode = Mode::Save;
    std::printf("DMC1 OK BEGIN\n");
}

void save_end()
{
    s_rom_ok = s_rom_ok;         // untouched
    ESP_LOGI(TAG, "save image written: %u B", unsigned(s_cursor));
    release_transfer();
    std::printf("DMC1 OK END\n");
}

// Raw bytes out of the USB-JTAG FIFO.  stdio is not usable here: both newlib and
// the usb_serial_jtag VFS translate 0x0A on the way out, which inserts an 0x0D
// into the middle of a binary blob and shifts everything after it.
void usb_write(const uint8_t *p, size_t n)
{
    size_t off = 0;
    while (off < n) {
        const size_t wrote = usb_serial_jtag_ll_write_txfifo(p + off, n - off);
        if (wrote == 0) {
            vTaskDelay(1);
            continue;
        }
        off += wrote;
    }
    usb_serial_jtag_ll_txfifo_flush();
}

void save_send()
{
    s_save_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                           esp_partition_subtype_t(0x41), "save");
    if (!s_save_part) {
        fail("no save partition");
        return;
    }
    const uint32_t size = s_save_part->size;
    char head[32];
    const int hn = snprintf(head, sizeof(head), "DMC1 SAVE %u\n", unsigned(size));
    std::fflush(stdout);              // drain the log lines queued ahead of us
    usb_write(reinterpret_cast<const uint8_t *>(head), size_t(hn));
    for (uint32_t off = 0; off < size; off += kPageSize) {
        const uint32_t n = (off + kPageSize <= size) ? kPageSize : (size - off);
        if (esp_partition_read(s_save_part, off, s_page, n) != ESP_OK) {
            fail("save read failed");
            return;
        }
        usb_write(s_page, n);
    }
    static const char kDone[] = "DMC1 OK SAVE\n";
    usb_write(reinterpret_cast<const uint8_t *>(kDone), sizeof(kDone) - 1);
}

void hello()
{
    // Report the partition, not saveimg: with no ROM the engine never gets to
    // guest_init(), so saveimg_init() never ran either - but the save slot is
    // still there and export/import work on it.
    const esp_partition_t *sp = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, esp_partition_subtype_t(0x41), "save");
    std::printf("DMC1 READY %d %s %d\n", s_rom_ok ? 1 : 0,
                s_rom_ok ? "loaded" : "-", sp != nullptr ? 1 : 0);
}

}  // namespace

void dmclink_note_rom_state(bool ok)
{
    s_rom_ok = ok;
}

bool dmclink_rom_ok()
{
    return s_rom_ok;
}

bool dmclink_waiting_for_rom()
{
    return !s_rom_ok && s_mode == Mode::Idle;
}

bool dmclink_busy()
{
    return s_mode != Mode::Idle;
}

bool dmclink_exec(const char *line)
{
    if (std::strncmp(line, "DMC1", 4) != 0)
        return false;
    if (std::strncmp(line, "DMC1 HELLO", 10) == 0) {
        // The web page already announces itself here, and it is the only party
        // on this cable that knows what time it is.  An optional
        // "DMC1 HELLO <epoch> [tz_min]" therefore doubles as the clock sync:
        // the board has no RTC, so without this the guest clock can only resume
        // from the last snapshot (docs/AUDIT_SAVE_CLOCK_2026-11-24_R3.md).
        long long epoch = 0;
        int tz = 480;
        const int n = std::sscanf(line, "DMC1 HELLO %lld %d", &epoch, &tz);
        if (n >= 1 && epoch > 1600000000LL)
            wallclock_set(int64_t(epoch), n >= 2 ? tz : 480);
        hello();
        return true;
    }
    if (std::strcmp(line, "DMC1 FINISH") == 0) {
        std::printf("DMC1 OK FINISH\n");
        std::fflush(stdout);
        esp_restart();
        return true;
    }
    if (std::strcmp(line, "DMC1 END") == 0) {
        if (s_mode == Mode::Rom)
            rom_end();
        else if (s_mode == Mode::Save)
            save_end();
        else
            std::printf("DMC1 ERR idle\n");
        return true;
    }
    if (std::strcmp(line, "DMC1 SAVE GET") == 0) {
        save_send();
        return true;
    }
    {
        unsigned a = 0, b = 0;
        char sha[41] = {};
        const int n = std::sscanf(line, "DMC1 ROM BEGIN %u %u %40s", &a, &b, sha);
        if (n >= 2) {
            rom_begin(a, b, n >= 3 ? sha : nullptr);
            return true;
        }
        if (std::sscanf(line, "DMC1 SAVE BEGIN %u", &a) == 1) {
            save_begin(a);
            return true;
        }
        if (std::sscanf(line, "DMC1 PAGE %u %u", &a, &b) == 2) {
            if (s_mode == Mode::Idle) {
                std::printf("DMC1 ERR idle\n");
                return true;
            }
            if (b != kPageSize) {
                fail("bad frame size");
                return true;
            }
            if (!read_exact(s_page, kPageSize, 8000)) {
                fail("frame timeout");
                return true;
            }
            if (s_mode == Mode::Rom) {
                if (a >= s_page_count) {
                    fail("page out of range");
                    return true;
                }
                if (!ensure_erased(s_cursor, kPageSize)) {
                    fail("erase failed");
                    return true;
                }
                if (esp_partition_write(s_rom_part, s_cursor, s_page, kPageSize) != ESP_OK) {
                    fail("page write failed");
                    return true;
                }
                s_index[a] = s_cursor;
                s_bitmap[a >> 3] |= uint8_t(1u << (a & 7));
                s_cursor += kPageSize;
                s_present++;
            } else {
                if (a + kPageSize > s_save_part->size) {
                    fail("save offset out of range");
                    return true;
                }
                if (esp_partition_write(s_save_part, a, s_page, kPageSize) != ESP_OK) {
                    fail("save write failed");
                    return true;
                }
                s_cursor = a + kPageSize;
            }
            s_done++;
            std::printf("DMC1 ACK %u\n", unsigned(s_done));
            return true;
        }
    }
    std::printf("DMC1 ERR unknown\n");
    return true;
}
