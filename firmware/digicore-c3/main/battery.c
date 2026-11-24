// CW2017 fuel gauge driver for the AI Passport board.
//
// The gauge shares I2C0 with the ES8311 codec (SDA 10 / SCL 7), so it attaches
// to the bus audio.c already opened instead of starting a second master on those
// pins.  audio_init() must have run first.
//
// Why a battery profile is written
// --------------------------------
// CW2017 does not compute state-of-charge from voltage alone: it needs an 80-byte
// model of the pack (capacity curve at several temperatures).  Without one it
// reports 0 % forever, which is exactly what the first cut of this driver did
// before it grew a profile.
//
// The bytes are the vendor profile for the pack this board ships with - Wanli
// New Energy 500 mAh, profile2 / XIL40X / 2026-10-20 - taken verbatim from
// ai-passport-tamago/components/bsp/src/bsp_battery.c, which drives the same part
// on the same board.  Their sequence is: enter sleep, write the 80 bytes one at a
// time, read all 80 back to verify, set UPDATE_FLAG, then restart into active.
// Writing it outside sleep is silently ignored, which is why the order matters.
//
// Registers
//   0x00 version  0x02 VCELL (14 bit, 312.5 uV/LSB)  0x04 SOC (whole %)
//   0x05 SOC fraction  0x08 CONFIG (00 active / 30 restart / F0 sleep)
//   0x0B SOC_ALERT (bit7 = profile UPDATE_FLAG)  0x10..0x5F profile
#include "battery.h"

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "audio.h"

static const char *TAG = "batt";

#define CW_ADDR           0x63
#define CW_REG_VERSION    0x00
#define CW_REG_VCELL_H    0x02
#define CW_REG_SOC_H      0x04
#define CW_REG_CONFIG     0x08
#define CW_REG_SOC_ALERT  0x0B
#define CW_REG_PROFILE    0x10

#define CW_CONFIG_ACTIVE  0x00
#define CW_CONFIG_RESTART 0x30
#define CW_CONFIG_SLEEP   0xF0
#define CW_UPDATE_FLAG    0x80
#define CW_PROFILE_SIZE   80

// Wanli New Energy 500 mAh, profile2 / XIL40X / 2026-10-20.
static const uint8_t s_profile[CW_PROFILE_SIZE] = {
    0x5A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xAF, 0xCA, 0xCA, 0xC0, 0xB4, 0xA8, 0xCB, 0xAA,
    0x9F, 0xCF, 0xB5, 0x8A, 0x70, 0x56, 0x4B, 0x40,
    0x34, 0x2B, 0x22, 0x55, 0x41, 0xD2, 0x30, 0xD7,
    0xC9, 0xBE, 0xAE, 0xC8, 0xAE, 0x97, 0x9D, 0x98,
    0x94, 0x9F, 0xB6, 0x89, 0x73, 0x65, 0x57, 0x49,
    0x3D, 0x50, 0x74, 0x87, 0xA3, 0x9B, 0x5E, 0x38,
    0x00, 0x00, 0xAB, 0x02, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x64, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xB7,
};

static i2c_master_dev_handle_t s_dev;

static esp_err_t cw_read(uint8_t reg, uint8_t *buf, size_t n)
{
    if (!s_dev)
        return ESP_ERR_INVALID_STATE;
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, n, 100);
}

static esp_err_t cw_write(uint8_t reg, uint8_t val)
{
    if (!s_dev)
        return ESP_ERR_INVALID_STATE;
    uint8_t b[2] = { reg, val };
    return i2c_master_transmit(s_dev, b, 2, 100);
}

static int cw_enter_sleep(void)
{
    if (cw_write(CW_REG_CONFIG, CW_CONFIG_RESTART) != ESP_OK) return -1;
    vTaskDelay(pdMS_TO_TICKS(20));
    if (cw_write(CW_REG_CONFIG, CW_CONFIG_SLEEP) != ESP_OK) return -1;
    vTaskDelay(pdMS_TO_TICKS(10));
    return 0;
}

static int cw_enter_active(void)
{
    if (cw_write(CW_REG_CONFIG, CW_CONFIG_RESTART) != ESP_OK) return -1;
    vTaskDelay(pdMS_TO_TICKS(20));
    if (cw_write(CW_REG_CONFIG, CW_CONFIG_ACTIVE) != ESP_OK) return -1;
    vTaskDelay(pdMS_TO_TICKS(10));
    return 0;
}

// 1 = the stored profile is ours, 0 = missing or different, -1 = bus error.
static int cw_profile_matches(bool *matches)
{
    uint8_t val = 0;
    *matches = false;
    if (cw_read(CW_REG_SOC_ALERT, &val, 1) != ESP_OK) return -1;
    if ((val & CW_UPDATE_FLAG) == 0) return 0;      // nothing was ever written
    for (size_t i = 0; i < CW_PROFILE_SIZE; i++) {
        if (cw_read((uint8_t)(CW_REG_PROFILE + i), &val, 1) != ESP_OK) return -1;
        if (val != s_profile[i]) return 0;
    }
    *matches = true;
    return 0;
}

static int cw_update_profile(void)
{
    uint8_t val = 0;
    if (cw_enter_sleep() != 0) return -1;
    for (size_t i = 0; i < CW_PROFILE_SIZE; i++) {
        if (cw_write((uint8_t)(CW_REG_PROFILE + i), s_profile[i]) != ESP_OK) {
            ESP_LOGE(TAG, "profile write failed at %u", (unsigned)i);
            return -1;
        }
    }
    for (size_t i = 0; i < CW_PROFILE_SIZE; i++) {
        if (cw_read((uint8_t)(CW_REG_PROFILE + i), &val, 1) != ESP_OK) return -1;
        if (val != s_profile[i]) {
            ESP_LOGE(TAG, "profile verify failed at %u: %02X != %02X",
                     (unsigned)i, val, s_profile[i]);
            return -1;
        }
    }
    if (cw_read(CW_REG_SOC_ALERT, &val, 1) != ESP_OK) return -1;
    if (cw_write(CW_REG_SOC_ALERT, (uint8_t)(val | CW_UPDATE_FLAG)) != ESP_OK) return -1;
    return cw_enter_active();
}

bool battery_init(void)
{
    i2c_master_bus_handle_t bus = (i2c_master_bus_handle_t)audio_i2c_bus();
    if (!bus) {
        ESP_LOGW(TAG, "no I2C bus (audio init failed); no battery readout");
        return false;
    }

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CW_ADDR,
        .scl_speed_hz = 100000,
    };
    if (i2c_master_bus_add_device(bus, &cfg, &s_dev) != ESP_OK) {
        ESP_LOGW(TAG, "CW2017 could not attach at 0x%02X", CW_ADDR);
        s_dev = NULL;
        return false;
    }

    uint8_t ver = 0;
    if (cw_read(CW_REG_VERSION, &ver, 1) != ESP_OK) {
        ESP_LOGW(TAG, "CW2017 silent at 0x%02X", CW_ADDR);
        s_dev = NULL;
        return false;
    }

    bool matches = false;
    if (cw_profile_matches(&matches) != 0) {
        ESP_LOGW(TAG, "profile read failed; SOC will not be trustworthy");
    } else if (!matches) {
        ESP_LOGI(TAG, "writing the 500 mAh pack profile (first boot on this gauge)");
        if (cw_update_profile() != 0)
            ESP_LOGW(TAG, "profile write failed; SOC may stay at 0");
    } else {
        uint8_t cfg_now = 0;
        if (cw_read(CW_REG_CONFIG, &cfg_now, 1) == ESP_OK && cfg_now != CW_CONFIG_ACTIVE)
            cw_enter_active();
    }

    ESP_LOGI(TAG, "CW2017 v%02X ready, %d mV, SOC %d%%", ver, battery_mv(), battery_soc());
    return true;
}

int battery_soc(void)
{
    uint8_t b[2] = { 0, 0 };
    if (cw_read(CW_REG_SOC_H, b, 2) != ESP_OK)
        return -1;
    if (b[0] > 100)
        return -1;              // 0xFF while the gauge is still computing
    return (int)b[0];
}

int battery_mv(void)
{
    uint8_t b[2] = { 0, 0 };
    if (cw_read(CW_REG_VCELL_H, b, 2) != ESP_OK)
        return -1;
    const uint32_t raw = (((uint32_t)b[0] << 8) | b[1]) & 0x3FFF;   // 14 bit
    return (int)((raw * 3125u) / 10000u);                           // 312.5 uV/LSB
}
