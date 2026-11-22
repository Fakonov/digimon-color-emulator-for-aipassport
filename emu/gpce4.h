#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
#include "unsp.h"          // UnspBus, unsp_device, UNSP_IRQ0_LINE..

// ===========================================================================
//  Generalplus GPCE4 SoC (Sunplus unSP 2.0 core) + ST7735 panel + SPI flash
//
//  Ported from MAME 0.289 (ref/generalplus_gpce4_soc.cpp,
//  ref/generalplus_gpce4.cpp, ref/st7735_lcdc.cpp, ref/ioport.cpp and
//  machine/generic_spi_flash.cpp).  Plain C++20, no MAME headers.
//
//  Word addresses are 23 bits (8M words = 16 MB of byte space); the byte
//  oriented images (internal ROM, SPI flash) are presented as little endian
//  16-bit words on the bus, which is how the harness expects to read them.
//
//  Every register handler below is named after the MAME handler it mirrors and
//  carries the MAME source line in a comment.  docs/SOC_PORTS.md tabulates all
//  of them.
// ===========================================================================


// ===========================================================================
//  St7735 - Sitronix ST7735 LCD controller
//  Mirrors st7735_lcdc_device (ref/st7735_lcdc.cpp:1-404).
// ===========================================================================
struct St7735 {
    uint16_t fb[128*128];  // RGB565, row major
    bool display_on = false;
    bool sleep = false;
    void reset();
    void command_w(uint8_t d);
    void data_w(uint8_t d);
    uint8_t read_data();          // for 0x3094 style LCD reads if the game needs them

    // --- internal state, mirrors st7735_lcdc_device ---
    // MAME renders out of a 256x256 GRAM (index = y*0x200 + x) and clips to the
    // 128x128 screen; keeping the full GRAM means the window/rotation maths is
    // bit-for-bit identical to the MAME model.  fb[] above is the clipped view.
    std::vector<uint16_t> m_displaybuffer;   // 256*256 words, GRAM
    int m_posx = 0;
    int m_posy = 0;
    int m_posminx = 0;
    int m_posmaxx = 0;
    int m_posminy = 0;
    int m_posmaxy = 0;
    uint8_t m_command = 0;
    int m_commandstep = 0;
    uint16_t m_madctl = 0;
    uint8_t m_colmod = 0;                    // 0x3a, recorded only
    uint16_t m_pixellatch = 0;
    uint8_t m_params[16] = {};               // last parameters of logged-only cmds
    bool trace = false;                      // log every command (MAME logerror)
    uint8_t m_unknown_seen[32] = {};         // bitmap: unknown commands logged once

    void ensure_buffer();                    // allocate/clear GRAM lazily
    void store_pixel(uint16_t pixel);        // GRAM write + fb[] update
    void log_unknown(uint8_t cmd);
    void soft_reset_regs();                  // 0x01 SWRESET
};


// ===========================================================================
//  Gpce4SpiFlash - port of MAME's generic_spi_flash_device
//  (src/devices/machine/generic_spi_flash.cpp).  Byte interface only: one
//  byte in (write), one byte out (read/m_latch), clocked by the SoC's SPI1
//  transmit timer.
// ===========================================================================
struct Gpce4SpiFlash {
    // generic_spi_flash.h command/state enumeration (values are internal).
    enum : int {
        READY_FOR_COMMAND = 0,
        COMMAND_01_WRSR,
        COMMAND_02_PP,
        COMMAND_03_READ,
        COMMAND_05_RDSR,
        COMMAND_06_WREN,            // never latched as a state
        COMMAND_04_WRDI,            // never latched as a state
        COMMAND_0B_FAST_READ,
        COMMAND_11_UNKNOWN,
        COMMAND_15_RDCR,
        COMMAND_20_SE,
        COMMAND_31_UNKNOWN,
        COMMAND_35_RDSR2,
        COMMAND_66_ENABLE_RESET,    // never latched as a state
        COMMAND_90_REMS,
        COMMAND_99_RESET,           // never latched as a state
        COMMAND_9F_RDID,
        COMMAND_AB_RDP,             // never latched as a state
        COMMAND_B9_DP,              // never latched as a state
        COMMAND_EB_4READ,
        COMMAND_EC_UNKNOWN,
        COMMAND_FF_CRMR             // never latched as a state
    };

    //  The image pointer is mutable so page program / sector erase can model a
    //  writable NOR part; callers own the buffer and decide when to persist it.
    void set_image(uint8_t *rom, size_t size);
    void reset();                            // device_reset(): addr/state/latch
    void write(uint8_t data);
    uint8_t read() const { return m_latch; }
    void set_single_byte_status_read()  { m_multibyte_status_read = false; }
    void set_single_byte_status_writes() { m_multibyte_status_write = false; }

    uint8_t *m_rom = nullptr;
    bool writable = true;                    // false = behave like MAME (read-only image)
    size_t m_size = 0;
    uint32_t m_addr = 0;
    int m_state = READY_FOR_COMMAND;
    uint8_t m_latch = 0;
    int m_state_step = 0;
    uint8_t m_statusreg = 0;
    uint8_t m_configreg = 0;
    bool m_multibyte_status_read = true;
    bool m_multibyte_status_write = true;
    bool trace = false;

    uint8_t rom_byte(uint32_t addr) const;   // rom[addr & (size-1)]
};


// ===========================================================================
//  Gpce4 - the SoC
// ===========================================================================
struct Gpce4 : public UnspBus {
    Gpce4();
    ~Gpce4();                                // closes the WAV / dumps audio stats
    void set_images(const uint8_t *irom, size_t irom_size, uint8_t *flash, size_t flash_size);
    void set_cpu(unsp_device *cpu);          // may be null in selftest
    void reset();
    uint16_t read16(uint32_t addr) override;
    void write16(uint32_t addr, uint16_t data) override;
    // Direct host window for opcode fetch (RAM / internal ROM / SPI flash).
    const uint16_t *fetch_window(uint32_t addr, uint32_t *base, uint32_t *mask) override;
    void tick(uint32_t cycles);              // advance timers by `cycles` CPU cycles
    // Cycles until the next timer event that can reach the CPU (i.e. the next
    // point at which a timer IRQ may become pending), clamped to `cap`.  The
    // main loop runs the CPU exactly up to that cycle so an interrupt is taken
    // at the same instruction boundary MAME's scheduler would pick.
    uint32_t cycles_to_next_event(uint32_t cap) const;
    uint64_t now() const { return m_now; }  // CPU cycles since reset()
    void set_port(int port, uint16_t bits);  // port 0=A,1=B,2=C; bit value 1 = released, 0 = pressed (active low)
    St7735 lcd;
    uint16_t ram[4096];

    // ---- integration knobs ------------------------------------------------
    // The 300 kHz machine "timer" (ref/generalplus_gpce4.cpp:322-325) that
    // raises SPI2 interrupt 24; on by default, off makes the SoC purely passive.
    // The 300 kHz SPI2 driver source only sets INT_Status bit 24; nothing in
	// the model or the game consumes it (see docs/C3_PORT.md 8.4).  Off by
	// default: it costs 15 events per 4.8 kcycle slice for no state change.
	bool timer_300k_enable = false;

	// A timer source whose interrupt is masked cannot change guest state: it
	// only sets a pending bit in INT_Status.  timer_a (bit 15 of INT_Ctrl) is
	// enabled by nothing in this game, so its 20 kHz events are pure overhead.
	// The 20 kHz timer_a only ever raises IRQ0, and this game keeps INT_Ctrl at
	// 0x0000 for its whole run.  "The CPU cannot take the interrupt" is not the
	// same as "the event does not matter": the guest also *polls* INT_Status,
	// and the boot-time check at ~18 s waits for a status bit that only moves
	// when the source is actually serviced.  Masking it parks the game inside
	// the internal-ROM timeout loop (0x005251 / 0x005376) forever, which is why
	// the save never fires.  Keep it live by default; tick() coalesces the
	// events anyway, so the cost is a fraction of what it was before the wheel
	// rewrite.
	bool mask_irq0_timer = true;   // measured: unmasked costs guest_pct 30->61 and does NOT fix the 18 s stall
	bool source_enabled(int i) const
	{
		if (i == 1)
			return !mask_irq0_timer || (m_interrupt_ctrl & 0x8000) != 0;
		return true;
	}
    // MAME's update_interrupts() clears IRQ7 twice in a row, so a pending 2 Hz
    // source can never reach the CPU.  false = hardware behaviour (OR of both
    // sources), true = reproduce MAME's double else-clear verbatim.
    bool irq7_mame_quirk = false;
    // MAME's SoC answers every SPI2 status read with a constant: TXStatus 0
    // (:1030 returns m_spi2_txstatus, which nothing ever sets), RXStatus 0
    // (:1042), MISC 0 (:1054).  Since the mask ROM polls these registers
    // (0x3091 x5, 0x3093 x5, 0x3095 x6 static hits) and was only ever run
    // against that model, the MAME-exact reads are the default; the real
    // 12-byte FIFO fill level / TXFLEV / SPITXIF are still tracked internally
    // and become visible when this is set.
    bool spi2_status_regs = true;    // trial: the guest polls 0x3095 ten times; MAME returns a constant
    bool trace_io = false;

    // =====================================================================
    //  AUDIO - DAC / PPAM model (docs/AUDIO.md)
    //  ---------------------------------------------------------------------
    //  MAME wires 0x3041 to DAC_16BIT_R2R_TWOS_COMPLEMENT ("dac") and 0x3043
    //  to a PPAM pulse driver, but the machine never connects the resulting
    //  stream to a speaker, so both handlers only log.  This section adds the
    //  missing sink.
    //
    //  Hardware behaviour that is modelled: 0x3041 latches a signed 16-bit
    //  R-2R code that holds until the next write (zero order hold); timer_a
    //  runs at 20 kHz ("// audio" in ref/generalplus_gpce4_soc.cpp:139) and
    //  is what the game clocks samples with.  The model integrates the held
    //  code over every 2000 CPU cycles - exactly one frame at 96 MHz / 48 kHz
    //  - and pushes the average into a ring buffer, so the producer cost is
    //  O(1) per frame no matter how fast the game writes, and an arbitrary
    //  sample rate (20 kHz, 8 kHz, burst writes) still lands correctly.
    // =====================================================================
    static constexpr int      AUDIO_RATE           = 48000;  // frames per second
    static constexpr uint32_t AUDIO_CYCLES_PER_FRM = 2000;   // 96 MHz / 48 kHz
    static constexpr uint32_t AUDIO_RING_FRAMES    = 65536;  // 1.37 s, power of 2

    bool   audio_enabled = true;             // model on/off; see docs/AUDIO.md
    // 48 kHz mono.  Copies min(frames, available) real frames and zero fills
    // the rest so a host callback can always play the whole buffer; returns
    // the number of real frames.  A frame is only produced by tick(), so a
    // host that never calls tick() reads silence.
    size_t audio_read(int16_t *dst, size_t frames);
    void   audio_reset();                    // drop the buffered history
    uint64_t audio_frames_produced() const { return m_audio_frames; }
    uint64_t audio_overruns() const { return m_audio_overruns; }   // producer outran the ring
    uint64_t audio_underruns() const { return m_audio_underruns; } // consumer starved

    // Per register write statistics for 0x3040..0x3043; filled unconditionally
    // (a handful of integer ops per write) and printed by ~Gpce4() when the
    // environment variable GPCE4_AUDIO_STATS names a file ("1" = stdout).
    struct AudioRegStats {
        const char *name = "";
        uint64_t writes = 0;
        uint16_t min_value = 0xffff;
        uint16_t max_value = 0;
        uint64_t hist[8] = {};               // 8 most frequent values ...
        uint32_t hist_value[8] = {};         // ... and the values themselves
        uint64_t deltas = 0;                 // 0x3041 only: cycles between writes
        uint64_t delta_sum = 0;
        uint64_t delta_min = ~uint64_t(0);
        uint64_t delta_max = 0;
        uint64_t delta_hist[34] = {};        // bucket = cycles / 480 (10 us), 33 = rest
    };
    enum { AUDIO_REG_COUNT = 4 };            // 0x3040..0x3043
    AudioRegStats m_audio_stat[AUDIO_REG_COUNT];
    uint32_t m_io_hist[256] = {};            // every IO write, by offset (proof of §1)
    uint16_t m_io_last[256] = {};            // last value written, by offset
    uint16_t m_dac_ctrl = 0;                 // 0x3040, last DAC_Ctrl
    uint16_t m_ppam_ctrl = 0;                // 0x3043, last PPAM_Ctrl
    uint16_t m_dac_cha2 = 0;                 // 0x3042, channel 2 (unused by the game)

    // ---- images / cpu ----------------------------------------------------
    const uint8_t *m_irom = nullptr;
    size_t m_irom_size = 0;
    uint8_t *m_flash = nullptr;
    size_t m_flash_size = 0;
    unsp_device *m_cpu = nullptr;

    // ---- ports (ref/generalplus_gpce4_soc.cpp:294-457) -------------------
    uint16_t m_port_in[3] = { 0xffff, 0xffef, 0xffff };  // A/B/C, idle w/ pull-ups
    uint16_t m_ioa_buffer = 0;
    uint16_t m_iob_buffer = 0;
    uint16_t m_ioa_direction = 0;
    uint16_t m_iob_direction = 0;
    uint16_t m_ioc_direction = 0;
    uint16_t m_ioa_attribute = 0;
    uint16_t m_iob_attribute = 0;
    uint16_t m_ioc_attribute = 0;
    uint16_t m_io_ctrl = 0;
    uint16_t m_iob_out = 0;                  // machine driver's m_iob (D/C pin latch)

    // ---- misc registers --------------------------------------------------
    uint16_t m_timer_ctrl = 0;
    uint16_t m_cache_ctrl = 0;
    uint16_t m_wait_ctrl = 0;                // 0x3035, recorded but not acted on
    uint16_t m_dac_cha1 = 0;                 // 0x3041, last sample (no audio sink)
    uint16_t m_spi_auto_ctrl = 0;
    uint16_t m_spi_bank = 0;
    uint16_t m_spi_ctrl = 0;
    uint16_t m_spi2_ctrl = 0;

    // ---- interrupt controller -------------------------------------------
    uint32_t m_interrupt_status = 0;         // 32 sources, bit index = source
    uint16_t m_interrupt_ctrl = 0;           // 0x3050
    uint16_t m_interrupt2_ctrl = 0;          // 0x3053
    uint16_t m_fiq_sel = 0;
    uint16_t m_fiq2_sel = 0;
    bool m_irq_line[9] = {};                 // last state driven onto each line

    // ---- SPI1 (flash) FIFOs, ref/...soc.cpp:90-101 -----------------------
    Gpce4SpiFlash m_spi;
    uint8_t m_spi_tx_fifo[8] = {};
    uint8_t m_spi_rx_fifo[8] = {};
    int m_tx_fifo_entries = 0;
    int m_rx_fifo_entries = 0;
    bool m_spi_tx_armed = false;
    uint64_t m_spi_tx_next = 0;

    // ---- SPI2 (LCD) FIFO -------------------------------------------------
    // MAME transmits synchronously inside spi2_txdata_w(); the 12 byte FIFO is
    // modelled (and drained at the 48 MHz shifter rate, i.e. instantly as far
    // as the 96 MHz CPU is concerned) so the status counters are real.
    uint16_t m_spi2_txstatus = 0;            // bit15 TXIF, bit14 TXIEN, bits7:4 TXFLEV
    int m_spi2_tx_fill = 0;

    // ---- cycle driven timers --------------------------------------------
    struct TimerSource {
        uint64_t period;                     // CPU cycles
        uint64_t next;                       // absolute CPU cycle
        int irq;                             // bit index in m_interrupt_status
    };
    enum { SRC_COUNT = 6, SRC_SPI_TX = 6 };
    TimerSource m_timers[SRC_COUNT] = {};
    uint64_t m_now = 0;                      // CPU cycles since reset()

    // ---- audio model state (docs/AUDIO.md) -------------------------------
    std::vector<int16_t> m_audio_ring;       // heap: selftest keeps Gpce4 on the stack
    uint32_t m_audio_wr = 0;                 // absolute frame counters; index = & mask
    uint32_t m_audio_rd = 0;
    uint64_t m_audio_next = AUDIO_CYCLES_PER_FRM;  // CPU cycle of the next frame edge
    int32_t  m_audio_acc = 0;                // integral of the held code, current frame
    uint32_t m_audio_acc_cycles = 0;
    int32_t  m_audio_level = 0;              // held signed 16-bit DAC code (ZOH)
    uint64_t m_audio_frames = 0;
    uint64_t m_audio_overruns = 0;
    uint64_t m_audio_underruns = 0;
    uint64_t m_audio_peak = 0;               // |sample| peak since reset
    uint64_t m_audio_sumsq = 0;              // for the RMS report
    uint64_t m_audio_nonzero = 0;            // frames whose code was != 0
    uint32_t m_audio_sec_writes[64] = {};    // 0x3041 writes per emulated second
    uint64_t m_audio_last_write_cycle = 0;   // for the 0x3041 delta histogram
    bool     m_audio_wav = false;            // GPCE4_WAV is open
    bool     m_audio_stats_on = false;       // GPCE4_AUDIO_STATS is set
    char     m_audio_stats_path[512] = {};

    // ---- unmapped access log --------------------------------------------
    enum { UNMAPPED_LOG_MAX = 64 };
    uint32_t m_unmapped_read_log[UNMAPPED_LOG_MAX] = {};
    int m_unmapped_read_count = 0;
    uint32_t m_unmapped_write_log[UNMAPPED_LOG_MAX] = {};
    int m_unmapped_write_count = 0;

    // =====================================================================
    //  memory map
    // =====================================================================
    uint16_t internal_rom_r(uint32_t word_ofs);      // 0x004000-0x00ffff
    uint16_t spi_direct_r(uint32_t offset);          // 0x200000-0x3fffff
    uint16_t unmapped_read(uint32_t addr);
    void     unmapped_write(uint32_t addr, uint16_t data);
    bool     note_unmapped(uint32_t *list, int &count, uint32_t addr);

    // =====================================================================
    //  IO block 0x003000-0x0030ef
    // =====================================================================
    uint16_t io_r(uint16_t ofs);
    void     io_w(uint16_t ofs, uint16_t data);

    // ports A/B/C
    uint16_t ioa_data_r();          void ioa_data_w(uint16_t data);       // :294
    uint16_t ioa_buffer_r();        void ioa_buffer_w(uint16_t data);     // :302
    uint16_t ioa_direction_r();     void ioa_direction_w(uint16_t data);  // :310
    uint16_t ioa_attribute_r();     void ioa_attribute_w(uint16_t data);  // :318
    uint16_t iob_data_r();          void iob_data_w(uint16_t data);       // :326
    uint16_t iob_buffer_r();        void iob_buffer_w(uint16_t data);     // :334
    uint16_t iob_direction_r();     void iob_direction_w(uint16_t data);  // :342
    uint16_t iob_attribute_r();     void iob_attribute_w(uint16_t data);  // :350
    uint16_t ioc_data_r();          void ioc_data_w(uint16_t data);       // :358
    uint16_t ioc_buffer_r();        void ioc_buffer_w(uint16_t data);     // :366
    uint16_t ioc_direction_r();     void ioc_direction_w(uint16_t data);  // :374
    uint16_t ioc_attribute_r();     void ioc_attribute_w(uint16_t data);  // :382
    uint16_t io_ctrl_r();           void io_ctrl_w(uint16_t data);        // :390

    // timers
    void timera_data_w(uint16_t data);      // :399
    void timera_counter_w(uint16_t data);   // :404
    void timerb_data_w(uint16_t data);      // :409
    void timerb_counter_w(uint16_t data);   // :414
    void timerc_data_w(uint16_t data);      // :419
    void timerc_counter_w(uint16_t data);   // :424
    uint16_t timer_ctrl_r();    void timer_ctrl_w(uint16_t data);    // :429

    // PWM
    uint16_t pwm0_ctrl_r();     void pwm0_ctrl_w(uint16_t data);     // :434
    uint16_t pwm1_ctrl_r();     void pwm1_ctrl_w(uint16_t data);     // :439
    uint16_t pwm2_ctrl_r();     void pwm2_ctrl_w(uint16_t data);     // :444
    uint16_t pwm3_ctrl_r();     void pwm3_ctrl_w(uint16_t data);     // :449

    // system
    void system_clock_w(uint16_t data);     // :454
    void timebase_clear_w(uint16_t data);   // :459
    void watchdog_clear_w(uint16_t data);   // :464
    void wait_ctrl_w(uint16_t data);        // :469
    uint16_t cache_ctrl_r();    void cache_ctrl_w(uint16_t data);    // :474

    // DAC / PPAM (no audio sink in this port)
    void dac_ctrl_w(uint16_t data);         // :479
    void dac_cha1_data_w(uint16_t data);    // :484
    void dac_cha2_data_w(uint16_t data);    // :489
    void ppam_ctrl_w(uint16_t data);        // :494

    // audio model plumbing - see the AUDIO section at the top of the struct
    void audio_sink_code(uint16_t data);             // 0x3041 write -> held level
    void audio_advance(uint64_t from, uint64_t to);  // integrate + emit frame edges
    void audio_push(int16_t frame);                  // ring append (+ optional WAV)
    void audio_stat_note(int idx, uint16_t data);    // per register counters
    void audio_stats_dump(const char *path);         // io_stats.txt

    // interrupt controller
    uint16_t interrupt_ctrl_r();    void interrupt_ctrl_w(uint16_t data);    // :907
    uint16_t interrupt_status_r();  void interrupt_status_w(uint16_t data);  // :915
    uint16_t fiq_sel_r();           void fiq_sel_w(uint16_t data);           // :927
    uint16_t interrupt2_ctrl_r();   void interrupt2_ctrl_w(uint16_t data);   // :932
    uint16_t interrupt2_status_r(); void interrupt2_status_w(uint16_t data); // :940
    uint16_t fiq2_sel_r();          void fiq2_sel_w(uint16_t data);          // :952
    void request_interrupt(int which);      // :960
    void clear_interrupt(int which);        // :966
    void update_interrupts();               // :972
    void set_line(int line, bool state);

    // ADC
    uint16_t adc_ctrl_r();          void adc_ctrl_w(uint16_t data);          // :1000
    uint16_t adc_data_r();          void adc_data_w(uint16_t data);          // :1006
    uint16_t adc_linein_bitctrl_r(); void adc_linein_bitctrl_w(uint16_t data); // :1012

    // SPI2 (LCD)
    uint16_t spi2_ctrl_r();         void spi2_ctrl_w(uint16_t data);         // :1024
    uint16_t spi2_txstatus_r();     void spi2_txstatus_w(uint16_t data);     // :1030
    void spi2_txdata_w(uint16_t data);                                      // :1036
    uint16_t spi2_rxstatus_r();     void spi2_rxstatus_w(uint16_t data);     // :1042
    uint16_t spi2_rxdata_r();                                                // :1048
    uint16_t spi2_misc_r();         void spi2_misc_w(uint16_t data);         // :1054

    // SPI1 (flash)
    uint16_t spi_ctrl_r();          void spi_ctrl_w(uint16_t data);          // :1066
    void spi_txstatus_w(uint16_t data);                                     // :1075
    void spi_txdata_w(uint16_t data);                                       // :1080
    uint16_t spi_rxstatus_r();      void spi_rxstatus_w(uint16_t data);      // :1085
    uint16_t spi_rxdata_r();                                                // :1091
    uint16_t spi_misc_r();          void spi_misc_w(uint16_t data);          // :1101
    uint16_t spi_man_ctrl_r();      void spi_man_ctrl_w(uint16_t data);      // :1107
    uint16_t spi_auto_ctrl_r();     void spi_auto_ctrl_w(uint16_t data);     // :1113
    uint16_t spi_bank_r();          void spi_bank_w(uint16_t data);          // :1119

    // SPI FIFO plumbing, ref/...soc.cpp:733-829
    void reset_spi_fifos();
    void write_to_tx_fifo(uint8_t data);
    void timer_spi_tx();                    // one-shot: clock one byte out
    void recieve_spi_fifo_data(uint8_t data);
};
