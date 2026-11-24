// Minimal ES8311 + I2S0 output for the AI Passport (ESP32-C3) board.
//
// Why this file exists
// --------------------
// The GPCE4 ROM in roms/dmc_v1_base.bin never writes the DAC registers.  That
// was measured four independent ways (see docs/DELIVERY_2026-11-22.md section
// 5): zero DAC operands in the 8 MB flash image, sixteen in the internal ROM
// of which all are production-test or dead fade helpers, zero DAC writes in a
// 60 s dynamic trace, and an empty IRQ0 handler that is a bare reti.  So the
// emulated console has no sound to forward - there is no audio stream to tap.
//
// The hardware, however, is fully wired: ES8311 codec at I2C 0x18 sharing the
// bus with the CW2017 fuel gauge, I2S0 full duplex on MCLK=6 BCLK=5 WS=3
// DOUT=2 DIN=4, and the speaker amplifier is tied always-on (no GPIO).  What
// was missing was firmware.  This module fills that gap with a square-wave
// synthesiser so the device can at least answer a key press with a beep.
//
// This is explicitly a synthetic sound, not emulation.  Nothing here claims to
// reproduce the original console's audio, because the original console's audio
// does not exist in this cartridge.
//
// Register values follow the local esp_codec_dev ES8311 driver
// (digimon-passport/firmware/digimon-passport/managed_components/
//  espressif__esp_codec_dev/device/es8311/es8311.c) for 48 kHz, 16 bit, MCLK =
// 256 * fs = 12.288 MHz, codec as I2S slave, DAC only.

#include "audio.h"

#include <math.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "audio";

#define AUDIO_I2C_PORT 0
#define AUDIO_I2C_SDA  10
#define AUDIO_I2C_SCL  7
#define AUDIO_ES8311_ADDR 0x18

#define AUDIO_MCLK_GPIO 6
#define AUDIO_BCLK_GPIO 5
#define AUDIO_WS_GPIO   3
#define AUDIO_DOUT_GPIO 2

#define AUDIO_RATE      48000
#define AUDIO_MCLK_HZ   (AUDIO_RATE * 256)

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_codec;
static i2s_chan_handle_t s_tx;
static bool s_ready;
// A beep request is queued and rendered by audio_task.  It must not block the
// caller: the emulator loop calls this from update_buttons(), and an 18 ms
// synchronous I2S write there would add 18 ms to every frame that sees a press.
typedef struct
{
	int freq;
	int ms;
} beep_req_t;

static QueueHandle_t s_req;

static void render_beep(int freq, int ms);

// ---- ROM buzzer state -----------------------------------------------------
// s_buz_hz is written by the frame loop (audio_set_buzzer) and read by the
// audio task.  32-bit aligned ints are atomic on this core, so no lock.
static volatile int s_buz_hz;
static uint32_t s_phase;                 // Q32 phase of the square wave
static bool s_codec_on;
static volatile int s_volume_pct = 55;
static uint32_t s_buz_blocks;            // I2S blocks written by the buzzer

static esp_err_t codec_power_on(void);   // defined with the power gate below

static void audio_task(void *arg)
{
	static int16_t buf[1024];            // 512 stereo frames = 10.7 ms at 48 kHz
	beep_req_t r;
	for (;;)
	{
		// A queued beep (boot chime / UI click) takes priority and blocks
		// until it has been written out.
		if (xQueueReceive(s_req, &r, 0) == pdTRUE)
		{
			render_beep(r.freq, r.ms);
			continue;
		}

		const int hz = s_buz_hz;
		if (hz <= 0)
		{
			vTaskDelay(pdMS_TO_TICKS(8));
			continue;
		}
		// audio_idle_poll() powers the codec down after 15 s of silence; a note
		// arriving later has to bring it back, otherwise the first press after
		// an idle period is swallowed (observed on hardware 2026-11-24).
		if (!s_codec_on)
		{
			const esp_err_t perr = codec_power_on();
			ESP_LOGI(TAG, "buzzer: codec wake -> %d", (int)perr);
			if (perr != ESP_OK)
			{
				vTaskDelay(pdMS_TO_TICKS(2));
				continue;
			}
		}

		const uint32_t inc = (uint32_t)(((uint64_t)hz << 32) / AUDIO_RATE);
		// 1250: fourth calibration pass on hardware (2026-11-24).  Each round
		// the user asked for "this loudness should be the next step up", so the
		// base has gone 20000 -> 10000 -> 5000 -> 2500 -> 1250.  Now vol=20 is
		// 250 counts (0.8 % FS) and vol=40 is 500 (the previous 20 % level).
		// `a <n>` retunes it live if this overshoots in the quiet direction.
		const int16_t amp = (int16_t)(1250 * s_volume_pct / 100);
		for (int i = 0; i < 512; i++)
		{
			s_phase += inc;
			const int16_t v = (s_phase & 0x80000000u) ? amp : (int16_t)-amp;
			buf[i * 2 + 0] = v;
			buf[i * 2 + 1] = v;
		}
		size_t sent = 0;
		if (i2s_channel_write(s_tx, buf, sizeof(buf), &sent, 100) != ESP_OK)
			vTaskDelay(pdMS_TO_TICKS(2));
		// One line per second of tone: proves the samples really reach the
		// codec when nobody is sure whether the speaker is dead or silent.
		if ((++s_buz_blocks % 95u) == 1u)
			ESP_LOGI(TAG, "buzzer hz=%d blocks=%u codec=%d", hz,
			         (unsigned)s_buz_blocks, (int)s_codec_on);
	}
}

static bool s_wrote_ok;

// Codec + I2S power gate.  s_codec_on tracks what the hardware is doing (not
// what we would like it to do), so a failed power-up is retried on the next
// beep rather than assumed.  s_last_beep_us is stamped on every request; the
// frame loop calls audio_idle_poll() and that is the only place that powers the
// codec down, so a beep can never race a power-down that started first.
// (s_codec_on is declared above the audio task, which reads it.)
static volatile int64_t s_last_beep_us;
static const int64_t kIdleOffUs = 15 * 1000000;   // 15 s without a beep

// ---------------------------------------------------------------------------
// ES8311
// ---------------------------------------------------------------------------

static esp_err_t reg_w(uint8_t reg, uint8_t val)
{
	uint8_t buf[2] = { reg, val };
	return i2c_master_transmit(s_codec, buf, sizeof(buf), 100);
}

static esp_err_t reg_r(uint8_t reg, uint8_t *out)
{
	return i2c_master_transmit_receive(s_codec, &reg, 1, out, 1, 100);
}

static esp_err_t es8311_init(void)
{
	// Soft reset, then release.  There is no reset GPIO on this board.
	esp_err_t err = reg_w(0x00, 0x1F);
	if (err != ESP_OK)
		return err;
	vTaskDelay(pdMS_TO_TICKS(20));
	err = reg_w(0x00, 0x00);
	if (err != ESP_OK)
		return err;
	err = reg_w(0x00, 0x80);        // power on, slave mode
	if (err != ESP_OK)
		return err;

	// Clock manager: MCLK from the SoC, 256 * fs, codec derives BCLK/LRCK.
	reg_w(0x01, 0x3F);              // all clocks on
	reg_w(0x02, 0x00);              // pre_div = 1
	reg_w(0x05, 0x00);              // adc/dac divider = 1
	reg_w(0x03, 0x10);              // fs_mode 48k, single speed
	reg_w(0x04, 0x10);              // dac osr
	reg_w(0x07, 0x00);              // lrck high
	reg_w(0x08, 0xFF);              // lrck low
	reg_w(0x06, 0x03);              // bclk divider low 5 bits

	// Format: 16 bit, I2S, DAC only.
	reg_w(0x09, 0x0C);              // SDP in: 16 bit
	reg_w(0x0A, 0x0C);              // SDP out: 16 bit
	reg_w(0x0B, 0x00);              // no tri-state
	reg_w(0x0C, 0x00);              // single speed, no dither

	// The vendor driver brings the chip up in two passes and the *running* value
	// of the system-power register is 0x01, not the 0xFC used during bring-up.
	// 0xFC only appears in the middle of the sequence; leaving it there parks the
	// chip in a state where I2S happily consumes samples, the DMA drains, and the
	// speaker stays silent - which is exactly what the hardware did.
	//
	// Pass 1, mirrored from es8311.c:246-258.
	reg_w(0x17, 0x00);              // ADC reference off while configuring
	reg_w(0x0E, 0xFF);              // analog PGA off
	reg_w(0x12, 0x02);              // DAC powered down
	reg_w(0x0D, 0xFA);              // system power: analog off
	reg_w(0x15, 0x00);              // ADC ramp rate 0
	reg_w(0x45, 0x00);              // GPIO/DAC output routing
	reg_w(0x0D, 0xFC);              // system power: bring up both rails

	// Pass 2, mirrored from es8311.c:310-329 - this is the final state.
	reg_w(0x17, 0xBF);              // ADC reference on
	reg_w(0x0E, 0x02);              // analog PGA on
	reg_w(0x12, 0x00);              // DAC on
	reg_w(0x14, 0x1A);              // analog mic bias (unused, vendor value)
	reg_w(0x0D, 0x01);              // <-- the running system-power value
	reg_w(0x15, 0x40);              // ADC ramp rate
	reg_w(0x37, 0x08);              // bypass DAC equaliser
	reg_w(0x45, 0x00);              // DAC drives the analog output pins

	reg_w(0x13, 0x10);              // enable headphone / line driver
	reg_w(0x1C, 0x6A);              // ADC equaliser bypass (not used)
	reg_w(0x31, 0x00);              // unmute
	reg_w(0x32, 0xBF);              // 0 dB

	uint8_t id = 0;
	// Logged once: this sequence runs again every time the codec is woken from
	// the idle power-down, and a line per key press would be noise.
	static bool id_logged = false;
	if (!id_logged && reg_r(0xFD, &id) == ESP_OK)
	{
		id_logged = true;
		ESP_LOGI(TAG, "ES8311 id=%02X at 0x%02X", id, AUDIO_ES8311_ADDR);
	}
	return ESP_OK;
}

// ---------------------------------------------------------------------------
// I2S
// ---------------------------------------------------------------------------

static esp_err_t i2s_start_output(void)
{
	i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
	chan.auto_clear = true;
	esp_err_t err = i2s_new_channel(&chan, &s_tx, NULL);
	if (err != ESP_OK)
		return err;

	i2s_std_config_t cfg = {
		.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_RATE),
		.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
		                                              I2S_SLOT_MODE_STEREO),
		.gpio_cfg = {
			.mclk = AUDIO_MCLK_GPIO,
			.bclk = AUDIO_BCLK_GPIO,
			.ws = AUDIO_WS_GPIO,
			.dout = AUDIO_DOUT_GPIO,
			.din = I2S_GPIO_UNUSED,
			.invert_flags = {
				.mclk_inv = false,
				.bclk_inv = false,
				.ws_inv = false,
			},
		},
	};
	// The codec wants MCLK = 256 * fs; the default multiple is 256 already, but
	// pin it so a future rate change cannot silently break the codec PLL.
	cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

	err = i2s_channel_init_std_mode(s_tx, &cfg);
	if (err != ESP_OK)
		return err;
	return i2s_channel_enable(s_tx);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// Bring the codec and its clocks up.  The full register sequence is replayed
// instead of a partial "resume": the ES8311 keeps most of its state while MCLK
// is stopped, but which registers survive a deep power-down depends on the
// depth, and guessing wrong means silence with no error anywhere.  Thirty I2C
// writes at 100 kHz is ~2 ms, and it only happens on the first key press after
// a quiet period.
static esp_err_t codec_power_on(void)
{
	if (!s_tx || !s_codec)
		return ESP_ERR_INVALID_STATE;
	const esp_err_t en = i2s_channel_enable(s_tx);
	if (en != ESP_OK && en != ESP_ERR_INVALID_STATE)
		return en;
	vTaskDelay(pdMS_TO_TICKS(2));       // MCLK must run before the codec PLL locks
	const esp_err_t err = es8311_init();
	if (err != ESP_OK)
		return err;
	s_codec_on = true;
	return ESP_OK;
}

// Analog rails first, then the clock: cutting MCLK while the DAC still drives
// the speaker is what produces the click this ordering avoids.
static void codec_power_off(void)
{
	if (!s_codec_on)
		return;
	reg_w(0x12, 0x02);                  // DAC powered down
	reg_w(0x0E, 0xFF);                  // analog PGA off
	reg_w(0x0D, 0x00);                  // system power: both rails down
	if (s_tx)
		i2s_channel_disable(s_tx);      // stops MCLK/BCLK into the codec
	s_codec_on = false;
}

// ROM buzzer: the frame loop publishes the current note; the audio task turns
// it into a square wave.  Calling this every frame also keeps the codec awake.
void audio_set_buzzer(int hz)
{
	s_buz_hz = (hz > 0) ? hz : 0;
	if (s_buz_hz > 0)
		s_last_beep_us = esp_timer_get_time();
}

bool audio_buzzer_active(void)
{
	return s_buz_hz > 0;
}

void audio_debug(int *hz, unsigned *blocks, int *codec_on, int *volume)
{
	if (hz) *hz = s_buz_hz;
	if (blocks) *blocks = s_buz_blocks;
	if (codec_on) *codec_on = s_codec_on ? 1 : 0;
	if (volume) *volume = s_volume_pct;
}

void audio_idle_poll(void)
{
	if (!s_ready || !s_codec_on)
		return;
	if (s_buz_hz > 0)                    // a note is playing: never power down
		return;
	if (esp_timer_get_time() - s_last_beep_us < kIdleOffUs)
		return;
	codec_power_off();
	ESP_LOGI(TAG, "codec + I2S off after %d s without a beep",
	         (int)(kIdleOffUs / 1000000));
}

bool audio_init(void)
{
	i2c_master_bus_config_t bus_cfg = {
		.i2c_port = AUDIO_I2C_PORT,
		.sda_io_num = AUDIO_I2C_SDA,
		.scl_io_num = AUDIO_I2C_SCL,
		.clk_source = I2C_CLK_SRC_DEFAULT,
		.glitch_ignore_cnt = 7,
		.flags.enable_internal_pullup = true,
	};
	if (i2c_new_master_bus(&bus_cfg, &s_bus) != ESP_OK)
	{
		ESP_LOGW(TAG, "i2c bus init failed");
		return false;
	}
	i2c_device_config_t dev_cfg = {
		.dev_addr_length = I2C_ADDR_BIT_LEN_7,
		.device_address = AUDIO_ES8311_ADDR,
		.scl_speed_hz = 100000,
	};
	if (i2c_master_bus_add_device(s_bus, &dev_cfg, &s_codec) != ESP_OK)
	{
		ESP_LOGW(TAG, "es8311 not on the bus");
		return false;
	}
	if (es8311_init() != ESP_OK)
	{
		ESP_LOGW(TAG, "es8311 init failed");
		return false;
	}
	if (i2s_start_output() != ESP_OK)
	{
		ESP_LOGW(TAG, "i2s init failed");
		return false;
	}
	s_req = xQueueCreate(4, sizeof(beep_req_t));
	if (!s_req)
	{
		ESP_LOGW(TAG, "beep queue alloc failed");
		return false;
	}
	// Priority 3, above the emulator loop (app_main runs at 1).  Equal priority
	// meant the round-robin only scheduled this task every ~10 ms tick, so a
	// 5.3 ms block was written every 16.8 ms and the DAC starved: measured on
	// hardware 2026-11-24 as "buzzer hz=830 blocks=191" over 3.2 s, i.e. a third
	// of the required rate and no audible tone.  The write itself blocks on the
	// I2S DMA semaphore, so a higher priority does not burn CPU - it only lets
	// the task wake when the DMA actually has room.
	if (xTaskCreate(audio_task, "audio", 3072, NULL, 3, NULL) != pdPASS)
	{
		ESP_LOGW(TAG, "audio task spawn failed");
		return false;
	}
	s_ready = true;
	s_codec_on = true;
	s_last_beep_us = esp_timer_get_time();
	ESP_LOGI(TAG, "ready: ES8311 + I2S0 %d Hz, amp always on", AUDIO_RATE);
	return true;
}

void audio_set_volume(int pct)
{
	if (pct < 0)
		pct = 0;
	if (pct > 100)
		pct = 100;
	s_volume_pct = pct;
	// ES8311 REG32 runs 0x00 = -95.5 dB up to 0xFF = +32 dB at 0.5 dB per step
	// (es8311.c:145-156), so 0xBF is exactly 0 dB.  The mapping that used to
	// live here - (100 - pct) * 255 / 100, later (100 - pct) * 0x40 / 100 - was
	// wrong in both direction and span: at the default 55 % the first gave
	// 0x72 (-38.5 dB) and the second 0x1C (-81.5 dB, i.e. silence).  Park the
	// codec at 0 dB and do the level control in the 16-bit domain inside
	// render_beep() (amp = 12000 * s_volume_pct / 100), where the scaling is
	// linear and cannot fall off the bottom of the register.
	if (s_ready)
		reg_w(0x32, 0xBF);
}

static void render_beep(int freq, int ms)
{
	if (!s_ready || freq <= 0 || ms <= 0)
		return;
	// Woken here, on the audio task, not in audio_beep(): this is the only
	// path that may block for the ~2 ms of I2C a codec power-up costs.
	if (!s_codec_on && codec_power_on() != ESP_OK)
		return;

	const int total = AUDIO_RATE * ms / 1000;
	const int half = AUDIO_RATE / (freq * 2);
	if (half <= 0)
		return;

	static int16_t buf[512];
	int written = 0;
	int phase = 0;
	const int16_t amp = (int16_t)(12000 * s_volume_pct / 100);

	while (written < total)
	{
		int n = 0;
		while (n < 256 && written + n < total)
		{
			const int16_t v = (phase < half) ? amp : (int16_t)-amp;
			buf[n * 2 + 0] = v;
			buf[n * 2 + 1] = v;
			if (++phase >= half * 2)
				phase = 0;
			n++;
		}
		size_t sent = 0;
		const esp_err_t werr = i2s_channel_write(s_tx, buf, (size_t)n * 4, &sent, 100);
		if (!s_wrote_ok)
		{
			s_wrote_ok = true;
			ESP_LOGI(TAG, "first i2s write: err=%d sent=%u of %u",
			         (int)werr, (unsigned)sent, (unsigned)(n * 4));
		}
		if (werr != ESP_OK)
			break;
		written += n;
	}
}

void audio_beep(int freq, int ms)
{
	if (!s_ready || !s_req || freq <= 0 || ms <= 0)
		return;
	// Stamped before the queue, not after: the idle gate must never power the
	// codec down between "user pressed a key" and "the task renders it".
	s_last_beep_us = esp_timer_get_time();
	const beep_req_t r = { freq, ms };
	// Drop when the queue is full: a backlog of clicks is worse than a missed one.
	if (xQueueSend(s_req, &r, 0) != pdTRUE)
		ESP_LOGD(TAG, "beep queue full, dropping %d Hz", freq);
}

void audio_silence(int ms)
{
	if (!s_ready || ms <= 0)
		return;
	if (!s_codec_on && codec_power_on() != ESP_OK)
		return;
	static int16_t zeros[256];
	const int total = AUDIO_RATE * ms / 1000;
	int written = 0;
	while (written < total)
	{
		int n = (total - written) < 128 ? (total - written) : 128;
		size_t sent = 0;
		if (i2s_channel_write(s_tx, zeros, (size_t)n * 4, &sent, 100) != ESP_OK)
			break;
		written += n;
	}
}

// See audio.h: the fuel gauge shares these pins, so it must not open a bus.
void *audio_i2c_bus(void)
{
	return s_ready ? (void *)s_bus : NULL;
}
