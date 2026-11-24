// ---------------------------------------------------------------------------
//  buttons.c - the three AI Passport keys sit on one ADC resistor ladder
//  (GPIO0 / ADC1 channel 0).  Measured bands, from the color-preview bring-up:
//
//      key0   0 .. 150 mV
//      key1 150 .. 447 mV
//      key2 447 .. 1900 mV
//      none      >1900 mV        (idle sits near the 3.3 V rail)
//
//  Only one key can be down at a time - it is a single ladder, not three GPIOs.
//
//  Why a polling task: the guest samples PORTA once per emulated frame, and one
//  frame is ~0.9 s of guest time on this board (the interpreter manages ~1.1
//  frames/s).  Polling from the frame loop therefore ran at ~1 Hz with a
//  two-sample debounce, i.e. a key had to be held for ~2 s before the guest
//  ever saw it - short taps vanished.  The task below samples every 20 ms and
//  keeps a latch of every press, so a single tap always turns into at least one
//  guest frame with the key down.
// ---------------------------------------------------------------------------
#include "buttons.h"

#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "BUTTONS"

#define BTN_GPIO        GPIO_NUM_0
#define BTN_ADC_UNIT    ADC_UNIT_1
#define BTN_ADC_CHANNEL ADC_CHANNEL_0
#define BTN_ADC_ATTEN   ADC_ATTEN_DB_12
#define BTN_ADC_SAMPLES 4
// One sample is enough at a 5 ms poll: two samples cost 10 ms of latency and
// the ADC reading at 5 ms is already stable for this switch ladder.
#define BTN_DEBOUNCE    1
#define BTN_POLL_MS     5

#define MV_KEY0_MAX 150
#define MV_KEY1_MAX 447
#define MV_KEY2_MAX 1900

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static bool s_adc_ready;
static int s_last_mv = -1;

static int s_raw = 0;        /* last raw classification */
static volatile int s_state; /* debounced classification */
static int s_same;
// A press queue, not a bitmask: two taps of the same key inside one guest
// frame used to collapse into a single bit, which is why fast repeated presses
// got swallowed.
#define BTN_QDEPTH 8
static uint8_t s_q[BTN_QDEPTH];
static uint8_t s_qhead;   /* next write slot */
static uint8_t s_qtail;   /* next read slot  */

static void button_task(void *arg);

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

esp_err_t buttons_init(void)
{
    esp_err_t err;

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

    s_same = 0;
    s_state = 0;
    s_qhead = 0;
    s_qtail = 0;

    s_adc_ready = true;
    ESP_LOGI(TAG, "ladder on GPIO0: key0 <%d mV, key1 <%d mV, key2 <%d mV, none above",
             MV_KEY0_MAX, MV_KEY1_MAX, MV_KEY2_MAX);
    xTaskCreate(button_task, "btn", 3072, NULL, 6, NULL);
    return ESP_OK;

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
    ESP_LOGW(TAG, "ADC init failed (%s); digital fallback on GPIO0 = key1",
             esp_err_to_name(err));
    return err;
}

static int classify_mv(int mv)
{
    if (mv < 0) {
        return 0;
    }
    if (mv < MV_KEY0_MAX) {
        return BTN_KEY0;
    }
    if (mv < MV_KEY1_MAX) {
        return BTN_KEY1;
    }
    if (mv < MV_KEY2_MAX) {
        return BTN_KEY2;
    }
    return 0;
}

int buttons_scan(void)
{
    int raw;

    if (!s_adc_ready) {
        raw = (gpio_get_level(BTN_GPIO) == 0) ? BTN_KEY1 : 0;
    } else {
        int sum = 0;
        for (int i = 0; i < BTN_ADC_SAMPLES; i++) {
            int sample = 0;
            if (adc_oneshot_read(s_adc, BTN_ADC_CHANNEL, &sample) != ESP_OK) {
                return s_state;
            }
            sum += sample;
        }
        int mv = -1;
        if (adc_cali_raw_to_voltage(s_cali, sum / BTN_ADC_SAMPLES, &mv) != ESP_OK) {
            return s_state;
        }
        s_last_mv = mv;
        raw = classify_mv(mv);
    }

    if (raw == s_raw) {
        if (s_same < BTN_DEBOUNCE) {
            s_same++;
        }
    } else {
        s_raw = raw;
        s_same = 0;
    }
    if (s_same >= BTN_DEBOUNCE && raw != s_state) {
        if (raw != 0) {
            const uint8_t next = (uint8_t)((s_qhead + 1) % BTN_QDEPTH);
            if (next != s_qtail) {        /* drop only when the queue is full */
                s_q[s_qhead] = (uint8_t)raw;
                s_qhead = next;
            }
        }
        s_state = raw;
    }
    return s_state;
}

int buttons_state(void)
{
    return s_state;
}

int buttons_take_latched(void)
{
    if (s_qtail == s_qhead)
        return 0;
    const int v = s_q[s_qtail];
    s_qtail = (uint8_t)((s_qtail + 1) % BTN_QDEPTH);
    return v;
}

static void button_task(void *arg)
{
    (void)arg;
    for (;;) {
        buttons_scan();
        vTaskDelay(pdMS_TO_TICKS(BTN_POLL_MS));
    }
}

int buttons_last_mv(void)
{
    return s_last_mv;
}
