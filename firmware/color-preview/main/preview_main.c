#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "display.h"
#include "driver/gpio.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "miniz.h"

#define TAG "color-preview"

#define SRC_W 96
#define SRC_H 96
#define SCALE 2
#define DST_W 320
#define DST_H 240
#define TOP ((DST_H - SRC_H * SCALE) / 2)
#define LEFT ((DST_W - SRC_W * SCALE) / 2)
#define CHUNK_ROWS 16
#define FRAME_BYTES (SRC_W * SRC_H * 2)
#define UI_START_FRAME 20

#define BTN_GPIO 0
#define BTN_ADC_UNIT ADC_UNIT_1
#define BTN_ADC_CHANNEL ADC_CHANNEL_0
#define BTN_ADC_ATTEN ADC_ATTEN_DB_12
#define BTN_ADC_SAMPLES 4
#define BTN_PRESS_MV 1900
#define BTN_RELEASE_MV 2100
#define STATIC_BOOT_MS 15000
#define STATIC_AFTER_KEY_MS 5000
#define STATIC_POLL_MS 20

typedef struct __attribute__((packed)) {
    char magic[4];
    uint16_t version;
    uint16_t width;
    uint16_t height;
    uint16_t fps;
    uint32_t count;
    uint32_t data_off;
} dcpv_header_t;

extern const uint8_t _binary_frames_bin_start[] asm("_binary_frames_bin_start");
extern const uint8_t _binary_frames_bin_end[] asm("_binary_frames_bin_end");

static uint8_t s_frame[FRAME_BYTES] __attribute__((aligned(4)));
static uint8_t s_dma[DST_W * CHUNK_ROWS * 2] __attribute__((aligned(4)));
static int s_rot = 2;  /* 0, 90, 180, 270 clockwise; 2 = 180 */

/* GPIO0 is an ADC resistor ladder on AI Passport, not a plain digital key.
 * The thresholds below are the BSP_BTN_MV_TABLE used by digimon-passport:
 * key0 0-150 mV, key1 150-447 mV, key2 447-1900 mV, released >1900 mV. */
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static bool s_adc_ready;
static bool s_btn_down;
static int s_btn_mv = -1;

static uint32_t read_u32(const uint8_t *p)
{
    uint32_t value;
    memcpy(&value, p, sizeof(value));
    return value;
}

static uint32_t frame_offset(const uint8_t *base, const dcpv_header_t *header, uint32_t index)
{
    const uint8_t *table = base + sizeof(*header);
    return read_u32(table + index * 4u);
}

static bool decode_frame(const uint8_t *base, const dcpv_header_t *header, uint32_t index)
{
    uint32_t start = frame_offset(base, header, index);
    uint32_t end = frame_offset(base, header, index + 1u);
    if (end <= start) {
        ESP_LOGE(TAG, "bad frame range %u..%u", (unsigned)start, (unsigned)end);
        return false;
    }

    size_t produced = tinfl_decompress_mem_to_mem(s_frame, sizeof(s_frame),
                                                  base + start, end - start,
                                                  TINFL_FLAG_PARSE_ZLIB_HEADER);
    if (produced != sizeof(s_frame)) {
        ESP_LOGE(TAG, "frame %u inflate failed (%u bytes)", (unsigned)index,
                 (unsigned)produced);
        return false;
    }
    return true;
}

static void src_to_dst(int x, int y, int *sx, int *sy)
{
    switch (s_rot & 3) {
    case 0:
        *sx = x;
        *sy = y;
        break;
    case 1: /* 90 deg clockwise */
        *sx = y;
        *sy = SRC_H - 1 - x;
        break;
    case 2: /* 180 */
        *sx = SRC_W - 1 - x;
        *sy = SRC_H - 1 - y;
        break;
    default: /* 270 deg clockwise */
        *sx = SRC_W - 1 - y;
        *sy = x;
        break;
    }
}

static void render_chunk(int out_y0)
{
    for (int row = 0; row < CHUNK_ROWS; row++) {
        int out_y = out_y0 + row;
        uint8_t *dst = s_dma + row * DST_W * 2;

        if (out_y < TOP || out_y >= TOP + SRC_H * SCALE) {
            memset(dst, 0, DST_W * 2);
            continue;
        }

        memset(dst, 0, LEFT * 2);
        for (int i = 0; i < SRC_W * SCALE; i++) {
            int x = i / SCALE;
            int y = (out_y - TOP) / SCALE;
            int sx, sy;
            src_to_dst(x, y, &sx, &sy);
            const uint8_t *src = s_frame + (sy * SRC_W + sx) * 2;
            dst[(LEFT + i) * 2] = src[0];
            dst[(LEFT + i) * 2 + 1] = src[1];
        }
        memset(dst + (LEFT + SRC_W * SCALE) * 2, 0,
               (DST_W - (LEFT + SRC_W * SCALE)) * 2);
    }
}

static esp_err_t draw_frame(void)
{
    for (int y = 0; y < DST_H; y += CHUNK_ROWS) {
        render_chunk(y);
        esp_err_t err = display_draw_bitmap(0, y, DST_W, CHUNK_ROWS, s_dma);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

/* Dump one output row as RGB565 runs.  The serial side only needs the
 * framebuffer that was handed to the panel; this does not read ST7789 GRAM. */
static void dump_rle_row(int y, const uint8_t *row)
{
    int x = 0;
    while (x < DST_W) {
        uint16_t color = (uint16_t)((row[x * 2] << 8) | row[x * 2 + 1]);
        int run = 1;
        while (x + run < DST_W) {
            uint16_t next = (uint16_t)((row[(x + run) * 2] << 8) |
                                       row[(x + run) * 2 + 1]);
            if (next != color) {
                break;
            }
            run++;
        }
        printf("SHOT_ROW %d %d %d %04X\n", y, x, run, color);
        x += run;
    }
}

static void dump_screen(const char *why)
{
    printf("\nSHOT_BEGIN %d %d RGB565 rot=%d why=%s\n",
           DST_W, DST_H, s_rot * 90, why);
    for (int y0 = 0; y0 < DST_H; y0 += CHUNK_ROWS) {
        render_chunk(y0);
        for (int row = 0; row < CHUNK_ROWS; row++) {
            int y = y0 + row;
            uint8_t *p = s_dma + row * DST_W * 2;
            if (y < 8) {
                for (int x = 0; x < DST_W; x++) {
                    p[x * 2] = 0xF8;
                    p[x * 2 + 1] = 0x00;
                }
            } else if (y >= DST_H - 8) {
                for (int x = 0; x < DST_W; x++) {
                    p[x * 2] = 0x07;
                    p[x * 2 + 1] = 0xE0;
                }
            }
            dump_rle_row(y, p);
        }
        fflush(stdout);
        vTaskDelay(1);
    }
    printf("SHOT_END\n");
    fflush(stdout);
}

static void fill_output_bar(int y, uint16_t color)
{
    for (int row = 0; row < 8; row++) {
        uint8_t *dst = s_dma + row * DST_W * 2;
        for (int x = 0; x < DST_W; x++) {
            dst[x * 2] = (uint8_t)(color >> 8);
            dst[x * 2 + 1] = (uint8_t)(color & 0xFF);
        }
    }
    display_draw_bitmap(0, y, DST_W, 8, s_dma);
}

static void put_px(int x, int y, uint16_t color)
{
    if ((unsigned)x >= SRC_W || (unsigned)y >= SRC_H) {
        return;
    }
    uint8_t *p = s_frame + (y * SRC_W + x) * 2;
    p[0] = (uint8_t)(color >> 8);
    p[1] = (uint8_t)(color & 0xFF);
}

static void fill_rect(int x0, int y0, int w, int h, uint16_t color)
{
    for (int y = y0; y < y0 + h; y++) {
        for (int x = x0; x < x0 + w; x++) {
            put_px(x, y, color);
        }
    }
}

/* A synthetic, asymmetric frame: four corner colours plus a white arrow
 * pointing up in source space.  Rotating the frame must move the arrow and
 * the corners, while the red/green bars stay fixed in output space. */
static esp_err_t show_static_pattern(void)
{
    memset(s_frame, 0, sizeof(s_frame));
    fill_rect(0, 0, 16, 16, 0xF800);    /* top-left: red */
    fill_rect(SRC_W - 16, 0, 16, 16, 0x07E0); /* top-right: green */
    fill_rect(0, SRC_H - 16, 16, 16, 0x001F); /* bottom-left: blue */
    fill_rect(SRC_W - 16, SRC_H - 16, 16, 16, 0xFFFF); /* bottom-right: white */

    fill_rect(44, 38, 8, 38, 0xFFFF);   /* arrow shaft */
    for (int y = 18; y < 38; y++) {
        int half = y - 18;
        for (int x = 48 - half; x <= 48 + half; x++) {
            put_px(x, y, 0xFFFF);
        }
    }

    esp_err_t err = draw_frame();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "static pattern draw failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Output-space bars: red is always physical top, green always bottom. */
    fill_output_bar(0, 0xF800);
    fill_output_bar(DST_H - 8, 0x07E0);
    display_backlight_set(80);
    ESP_LOGI(TAG, "PREVIEW_STATIC rot=%d (red top, green bottom; arrow up at rot=0)",
             s_rot * 90);
    return ESP_OK;
}

static void show_error_screen(void)
{
    for (int y = 0; y < DST_H; y += CHUNK_ROWS) {
        for (int row = 0; row < CHUNK_ROWS; row++) {
            uint8_t *dst = s_dma + row * DST_W * 2;
            for (int x = 0; x < DST_W; x++) {
                int phase = ((x / 32) + ((y + row) / 32)) & 1;
                uint16_t color = phase ? 0x001F : 0xF800;
                dst[x * 2] = (uint8_t)(color >> 8);
                dst[x * 2 + 1] = (uint8_t)(color & 0xFF);
            }
        }
        display_draw_bitmap(0, y, DST_W, CHUNK_ROWS, s_dma);
    }
    display_backlight_set(60);
}

static void button_fallback(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BTN_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
}

static void button_init(void)
{
    esp_err_t err = ESP_OK;
    adc_oneshot_unit_init_cfg_t unit = {
        .unit_id = BTN_ADC_UNIT,
    };
    err = adc_oneshot_new_unit(&unit, &s_adc);
    if (err != ESP_OK) {
        goto fallback;
    }

    adc_oneshot_chan_cfg_t channel = {
        .atten = BTN_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_oneshot_config_channel(s_adc, BTN_ADC_CHANNEL, &channel);
    if (err != ESP_OK) {
        goto fallback;
    }

    adc_cali_curve_fitting_config_t cal = {
        .unit_id = BTN_ADC_UNIT,
        .chan = BTN_ADC_CHANNEL,
        .atten = BTN_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_cali_create_scheme_curve_fitting(&cal, &s_cali);
    if (err != ESP_OK) {
        goto fallback;
    }

    s_adc_ready = true;
    ESP_LOGI(TAG, "button ADC ready: press < %d mV, release > %d mV",
             BTN_PRESS_MV, BTN_RELEASE_MV);
    return;

fallback:
    if (s_cali) {
        adc_cali_delete_scheme_curve_fitting(s_cali);
        s_cali = NULL;
    }
    if (s_adc) {
        adc_oneshot_del_unit(s_adc);
        s_adc = NULL;
    }
    button_fallback();
    ESP_LOGW(TAG, "button ADC init failed (%s); using digital fallback",
             esp_err_to_name(err));
}

static bool button_down(void)
{
    if (!s_adc_ready) {
        return gpio_get_level(BTN_GPIO) == 0;
    }

    int sum = 0;
    for (int i = 0; i < BTN_ADC_SAMPLES; i++) {
        int raw = 0;
        if (adc_oneshot_read(s_adc, BTN_ADC_CHANNEL, &raw) != ESP_OK) {
            return s_btn_down;
        }
        sum += raw;
    }

    int mv = -1;
    if (adc_cali_raw_to_voltage(s_cali, sum / BTN_ADC_SAMPLES, &mv) != ESP_OK) {
        return s_btn_down;
    }
    s_btn_mv = mv;

    if (s_btn_down) {
        if (mv > BTN_RELEASE_MV) {
            s_btn_down = false;
        }
    } else if (mv < BTN_PRESS_MV) {
        s_btn_down = true;
    }
    return s_btn_down;
}

static bool validate_blob(const uint8_t *base, size_t size,
                          dcpv_header_t *header, uint32_t *ui_start)
{
    if (size < sizeof(*header)) {
        ESP_LOGE(TAG, "frames.bin too small (%u bytes)", (unsigned)size);
        return false;
    }
    memcpy(header, base, sizeof(*header));

    if (memcmp(header->magic, "DCPV", 4) != 0) {
        ESP_LOGE(TAG, "bad magic");
        return false;
    }
    if (header->version != 1) {
        ESP_LOGE(TAG, "unsupported version %u", (unsigned)header->version);
        return false;
    }
    if (header->width != SRC_W || header->height != SRC_H) {
        ESP_LOGE(TAG, "unexpected frame size %ux%u",
                 (unsigned)header->width, (unsigned)header->height);
        return false;
    }
    if (header->fps == 0 || header->fps > 60) {
        ESP_LOGE(TAG, "bad fps %u", (unsigned)header->fps);
        return false;
    }
    if (header->count == 0) {
        ESP_LOGE(TAG, "no frames");
        return false;
    }
    if (header->data_off != sizeof(*header) + (header->count + 1u) * 4u) {
        ESP_LOGE(TAG, "bad data offset %u", (unsigned)header->data_off);
        return false;
    }
    uint32_t end = frame_offset(base, header, header->count);
    if (end > size) {
        ESP_LOGE(TAG, "frame table ends at %u, blob is %u",
                 (unsigned)end, (unsigned)size);
        return false;
    }
    *ui_start = (header->count > UI_START_FRAME) ? UI_START_FRAME : 0;
    return true;
}

static void preview_task(void *arg)
{
    (void)arg;
    const uint8_t *base = _binary_frames_bin_start;
    size_t size = (size_t)(_binary_frames_bin_end - _binary_frames_bin_start);
    dcpv_header_t header;
    uint32_t ui_start = 0;

    if (!validate_blob(base, size, &header, &ui_start)) {
        ESP_LOGE(TAG, "PREVIEW_RESULT FAIL invalid frames.bin");
        show_error_screen();
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    ESP_LOGI(TAG, "blob %u bytes, %ux%u, %u frames @ %u fps",
             (unsigned)size, (unsigned)header.width, (unsigned)header.height,
             (unsigned)header.count, (unsigned)header.fps);
    ESP_LOGI(TAG, "render %dx%d -> %dx%d, centered at (%d,%d), loop from frame %u",
             SRC_W, SRC_H, DST_W, DST_H, LEFT, TOP, (unsigned)ui_start);

    TickType_t static_until = 0;
    button_init();
    if (show_static_pattern() != ESP_OK) {
        show_error_screen();
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    dump_screen("boot-static");
    static_until = xTaskGetTickCount() + pdMS_TO_TICKS(STATIC_BOOT_MS);

    TickType_t period = pdMS_TO_TICKS(1000u / header.fps);
    if (period == 0) {
        period = 1;
    }
    TickType_t next = xTaskGetTickCount();
    uint32_t index = 0;
    bool first = true;
    bool static_mode = true;
    uint32_t static_resume = 0;
    TickType_t last_change = 0;
    bool last_down = button_down();  /* do not rotate on a key held at boot */
    ESP_LOGI(TAG, "button boot mv=%d down=%d", s_btn_mv, last_down);

    for (;;) {
        TickType_t now = xTaskGetTickCount();
        bool down = button_down();
        bool pressed = down && !last_down;
        last_down = down;

        if (pressed && (now - last_change) > pdMS_TO_TICKS(250)) {
            s_rot = (s_rot + 1) & 3;
            last_change = now;
            static_mode = true;
            static_resume = index;
            ESP_LOGI(TAG, "button edge mv=%d -> rot=%d", s_btn_mv, s_rot * 90);
            if (show_static_pattern() != ESP_OK) {
                show_error_screen();
                for (;;) {
                    vTaskDelay(pdMS_TO_TICKS(1000));
                }
            }
            dump_screen("key");
            static_until = xTaskGetTickCount() + pdMS_TO_TICKS(STATIC_AFTER_KEY_MS);
            continue;
        }

        if (static_mode) {
            if ((int32_t)(now - static_until) < 0) {
                vTaskDelay(pdMS_TO_TICKS(STATIC_POLL_MS));
                continue;
            }
            static_mode = false;
            index = static_resume;
            next = xTaskGetTickCount();
            ESP_LOGI(TAG, "static done; video from frame %u", (unsigned)index);
        }

        if (!decode_frame(base, &header, index)) {
            ESP_LOGE(TAG, "PREVIEW_RESULT FAIL frame %u", (unsigned)index);
            show_error_screen();
            for (;;) {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }

        esp_err_t err = draw_frame();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "PREVIEW_RESULT FAIL draw %s", esp_err_to_name(err));
            for (;;) {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }

        if (first) {
            display_backlight_set(80);
            ESP_LOGI(TAG, "PREVIEW_READY first_frame=%u rot=%d", (unsigned)index, s_rot * 90);
            first = false;
        }
        if ((index % 30u) == 0u) {
            ESP_LOGI(TAG, "frame %u/%u", (unsigned)index, (unsigned)header.count);
        }

        vTaskDelayUntil(&next, period);

        index++;
        if (index >= header.count) {
            index = ui_start;
            ESP_LOGI(TAG, "loop UI from frame %u", (unsigned)index);
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Digimon Color reference-frame preview");
    ESP_LOGI(TAG, "display bring-up only; frames.bin, no ROM, no unSP core");

    esp_err_t err = display_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "display init failed: %s", esp_err_to_name(err));
        return;
    }

    /* tinfl's in-memory decoder keeps ~11 KB of state on the caller stack;
       8 KB overflowed before the first frame. 32 KB leaves headroom. */
    xTaskCreate(preview_task, "preview", 32768, NULL, 5, NULL);
}
