#include "display.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define TAG "display"

#define LCD_MOSI_GPIO 9
#define LCD_SCLK_GPIO 8
#define LCD_CS_GPIO   1
#define LCD_DC_GPIO   20
#define LCD_BL_GPIO   21

#define LCD_H_RES 320
#define LCD_V_RES 240
// 40 MHz, not 80: at 80 the panel showed occasional horizontal displacement
// of moving sprites, the classic symptom of a marginal SPI clock on this flex
// cable.  Halving it doubles the transfer time (0.33 -> 0.66 ms per frame) and
// costs nothing measurable against a 16.7 ms frame.
#define LCD_PCLK_HZ (80 * 1000 * 1000)

#define BL_LEDC_MODE      LEDC_LOW_SPEED_MODE
#define BL_LEDC_TIMER     LEDC_TIMER_0
#define BL_LEDC_CHANNEL   LEDC_CHANNEL_0
#define BL_LEDC_RES       LEDC_TIMER_10_BIT
#define BL_LEDC_FREQ_HZ   5000
#define BL_MAX_DUTY       ((1u << 10) - 1u)

typedef struct {
    uint8_t cmd;
    uint8_t data[16];
    uint8_t len;
    uint16_t delay_ms;
} st_init_cmd_t;

/* Vendor sequence for the ST7789P3 on this board.  Do not replace with the
 * generic ST7789 defaults: porch/power/gamma are panel-specific. */
static const st_init_cmd_t ST7789P3_CMDS[] = {
    {0xB2, {0x05, 0x05, 0x00, 0x33, 0x33}, 5, 0},
    {0xB7, {0x35}, 1, 0},
    {0xBB, {0x21}, 1, 0},
    {0xC0, {0x2C}, 1, 0},
    {0xC2, {0x01}, 1, 0},
    {0xC3, {0x0B}, 1, 0},
    {0xC4, {0x20}, 1, 0},
    {0xC6, {0x0F}, 1, 0},
    {0xD0, {0xA7, 0xA1}, 2, 0},
    {0xD0, {0xA4, 0xA1}, 2, 0},
    {0xD6, {0xA1}, 1, 0},
    {0xE0, {0xD0, 0x04, 0x08, 0x0A, 0x09, 0x05, 0x2D, 0x43,
            0x49, 0x09, 0x16, 0x15, 0x26, 0x2B}, 14, 0},
    {0xE1, {0xD0, 0x03, 0x09, 0x0A, 0x0A, 0x06, 0x2E, 0x44,
            0x40, 0x3A, 0x15, 0x15, 0x26, 0x2A}, 14, 10},
};

static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;
static SemaphoreHandle_t s_trans_done;
static bool s_backlight_ready;

static bool on_color_trans_done(esp_lcd_panel_io_handle_t panel_io,
                                esp_lcd_panel_io_event_data_t *edata,
                                void *user_ctx)
{
    (void)panel_io;
    (void)edata;
    (void)user_ctx;
    BaseType_t high_task_woken = pdFALSE;
    if (s_trans_done) {
        xSemaphoreGiveFromISR(s_trans_done, &high_task_woken);
    }
    return high_task_woken == pdTRUE;
}

static esp_err_t backlight_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode = BL_LEDC_MODE,
        .timer_num = BL_LEDC_TIMER,
        .duty_resolution = BL_LEDC_RES,
        .freq_hz = BL_LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "ledc timer");

    ledc_channel_config_t channel = {
        .gpio_num = LCD_BL_GPIO,
        .speed_mode = BL_LEDC_MODE,
        .channel = BL_LEDC_CHANNEL,
        .timer_sel = BL_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
        .intr_type = LEDC_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel), TAG, "ledc channel");
    s_backlight_ready = true;
    return ESP_OK;
}

esp_err_t display_init(void)
{
    if (s_panel) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "init ST7789P3 %dx%d", LCD_H_RES, LCD_V_RES);

    s_trans_done = xSemaphoreCreateBinary();
    if (!s_trans_done) {
        return ESP_ERR_NO_MEM;
    }

    spi_bus_config_t bus = {
        .mosi_io_num = LCD_MOSI_GPIO,
        .miso_io_num = -1,
        .sclk_io_num = LCD_SCLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        // Only a transfer-size hint: esp_lcd splits a larger draw_bitmap into
        // several DMA bursts without touching the address window, so the frame
        // still reaches the panel as one uninterrupted stream.
        .max_transfer_sz = LCD_H_RES * 80 * 2,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO),
                        TAG, "spi bus");

    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = LCD_CS_GPIO,
        .dc_gpio_num = LCD_DC_GPIO,
        .pclk_hz = LCD_PCLK_HZ,
        .spi_mode = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .trans_queue_depth = 10,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST,
                                                 &io_config, &s_io),
                        TAG, "panel io");

    esp_lcd_panel_io_callbacks_t callbacks = {
        .on_color_trans_done = on_color_trans_done,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_register_event_callbacks(s_io, &callbacks, NULL),
                        TAG, "panel io callbacks");

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(s_io, &panel_config, &s_panel),
                        TAG, "panel create");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel init");

    for (size_t i = 0; i < sizeof(ST7789P3_CMDS) / sizeof(ST7789P3_CMDS[0]); i++) {
        const st_init_cmd_t *cmd = &ST7789P3_CMDS[i];
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(s_io, cmd->cmd, cmd->data, cmd->len),
                            TAG, "vendor cmd 0x%02X", cmd->cmd);
        if (cmd->delay_ms) {
            vTaskDelay(pdMS_TO_TICKS(cmd->delay_ms));
        }
    }

    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, true), TAG, "invert");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(s_panel, true), TAG, "swap xy");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel, true, false), TAG, "mirror");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(s_panel, 0, 0), TAG, "gap");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "disp on");

    ESP_RETURN_ON_ERROR(backlight_init(), TAG, "backlight");
    display_backlight_set(0);

    ESP_LOGI(TAG, "ready, landscape %dx%d, backlight off", LCD_H_RES, LCD_V_RES);
    return ESP_OK;
}

void display_backlight_set(int percent)
{
    if (!s_backlight_ready) {
        return;
    }
    if (percent < 0) {
        percent = 0;
    } else if (percent > 100) {
        percent = 100;
    }
    uint32_t duty = (BL_MAX_DUTY * (uint32_t)percent) / 100u;
    ledc_set_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL, duty);
    ledc_update_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL);
}

esp_err_t display_draw_bitmap(int x, int y, int w, int h, const void *data)
{
    if (!s_panel || !data || w <= 0 || h <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (x < 0 || y < 0 || x + w > LCD_H_RES || y + h > LCD_V_RES) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, x, y, x + w, y + h, data);
    if (err != ESP_OK) {
        return err;
    }

    if (xSemaphoreTake(s_trans_done, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "color transfer timeout at y=%d", y);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}
