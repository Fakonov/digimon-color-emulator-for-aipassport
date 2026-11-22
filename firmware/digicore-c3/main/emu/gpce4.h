#pragma once
#include <cstdint>
#include <cstddef>
#include "unsp.h"          // UnspBus, unsp_device, UNSP_IRQ0_LINE..
#include "romstore.h"      // SparseRom: byte view of the SPI NOR image

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

    // Constant-colour run through RAMWR.  The guest's fill loop writes the same
    // 16-bit value many times in a row; feeding data_w() twice per pixel pays
    // the whole SPI2 register model (address decode, FIFO bookkeeping, TX
    // status) for every byte.  This walks the same window maths as data_w() and
    // lands the same pixels in fb[] - only the register plumbing is skipped.
    void fill_run(uint16_t pixel, uint32_t count);

    // --- internal state, mirrors st7735_lcdc_device ---
    // MAME renders out of a 256x256 word GRAM (index = y*0x200 + x) and clips to
    // the 128x128 screen.  The C3 port drops the 128 KiB GRAM and keeps only the
    // clipped 128x128 view in fb[]: store_pixel() computes the same index and
    // discards everything outside the cliprect, which is bit-for-bit what
    // render_to_bitmap() would show.  See docs/C3_PORT.md ("deviations").
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

    void store_pixel(uint16_t pixel);        // GRAM index -> fb[] clip
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

    //  The image lives in the `roms` partition as a sparse page store; see
    //  romstore.h.  `writable` keeps the MAME model's read-only switch.
    void set_image(SparseRom *rom);
    void reset();                            // device_reset(): addr/state/latch
    void write(uint8_t data);
    uint8_t read() const { return m_latch; }
    void set_single_byte_status_read()  { m_multibyte_status_read = false; }
    void set_single_byte_status_writes() { m_multibyte_status_write = false; }

    SparseRom *m_rom = nullptr;
    bool writable = true;                    // false = behave like MAME (read-only image)
    uint32_t m_addr = 0;
    int m_state = READY_FOR_COMMAND;
    uint8_t m_latch = 0;
    int m_state_step = 0;
    uint8_t m_statusreg = 0;
    uint8_t m_configreg = 0;
    bool m_multibyte_status_read = true;
    bool m_multibyte_status_write = true;
    bool trace = false;

    //  Command census.  The save path is a black box from the host side: the
    //  guest only touches the two save sectors once every 60 s of game time,
    //  so "no flash write in 10 minutes" can mean either "the game never got
    //  there" or "we dropped the command".  Counting the opcodes separates the
    //  two cases without a logic analyser.
    uint32_t m_n_se = 0;        // 0x20 sector erase
    uint32_t m_n_pp = 0;        // 0x02 page program
    uint32_t m_n_wren = 0;      // 0x06 write enable
    uint32_t m_n_rdid = 0;      // 0x9f
    uint32_t m_n_bytes_pp = 0;  // bytes actually programmed
    uint32_t m_n_unknown = 0;

    uint8_t rom_byte(uint32_t addr) const;   // rom[addr & (size-1)]
};


// ===========================================================================
//  Gpce4 - the SoC
// ===========================================================================
struct Gpce4 : public UnspBus {
    Gpce4();
    void set_images(const uint8_t *irom, size_t irom_size, SparseRom *flash);
    void set_cpu(unsp_device *cpu);          // may be null in selftest
    void reset();
    uint16_t read16(uint32_t addr) override;
    void write16(uint32_t addr, uint16_t data) override;
	// Direct host window for opcode fetch (RAM / internal ROM / SPI flash page).
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

    // Called by the HLE fill loop: paint `count` pixels of one colour straight
    // into the panel when the LCD is inside a RAMWR window.  Returns false when
    // it is not, so the caller writes the SPI2 data register instead and the
    // command/parameter state machine still sees every byte.
    bool lcd_fill_pixels(uint16_t value, uint32_t count)
    {
        if (lcd.m_command != 0x2c)
            return false;
        lcd.fill_run(value, count);
        return true;
    }

    // ---- integration knobs ------------------------------------------------
    // The 300 kHz machine "timer" (ref/generalplus_gpce4.cpp:322-325) that
    // raises SPI2 interrupt 24; on by default, off makes the SoC purely passive.
    // The 300 kHz SPI2 driver source only sets INT_Status bit 24; nothing in
	// the model or the game consumes it (see docs/C3_PORT.md 8.4).  Off by
	// default: it costs 15 events per 4.8 kcycle slice for no state change.
	bool timer_300k_enable = false;

	// timer_a (bit 15 of INT_Ctrl) is the 20 kHz source and the only one this
	// gate touches.  Nothing in this game enables it: INT_Ctrl stays 0x0000 for
	// the whole run, so every event it raises lands in a masked status bit and
	// changes no guest-visible state - it is pure overhead, and masking it keeps
	// guest_pct at ~30 instead of ~61.
	//
	// This used to be unmasked on the theory that a masked source could starve
	// the boot-time poll at ~18 s.  The PC ring buffer disproved that: the guest
	// leaves the flash main loop because it is *waiting for a key press*, and
	// returns as soon as PORT A shows one (measured: `rflash` recovers to 80 k/s
	// the second after a press).  Unmasking changed nothing except the cost.
	bool mask_irq0_timer = true;
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

    // Output-register probe.  The mask ROM writes the factory test vector into
    // the DAC at boot and then never touches it again, and MAME flags this
    // machine MACHINE_NO_SOUND.  Counting the writes is how we prove that on
    // hardware instead of trusting the disassembly: if dac stays at a handful
    // of boot-time writes and pwm never moves, this ROM has no runtime audio.
    uint32_t m_pwm_writes = 0;
    uint32_t audio_writes() const { return m_dac_writes; }
    uint32_t pwm_writes() const { return m_pwm_writes; }

    // ---- images / cpu ----------------------------------------------------
    const uint8_t *m_irom = nullptr;
    size_t m_irom_size = 0;
    SparseRom *m_flash = nullptr;
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
    uint16_t m_dac_cha2 = 0;                 // 0x3042
    // How often the guest writes the DAC.  There is no sample clock in the
    // chip: the DAC holds whatever was last written, so the *guest* sets the
    // rate by how often it writes.  Counting the writes is the only honest way
    // to learn what sample rate the game expects - if it is a few hundred a
    // second the channel is a beeper, if it is tens of thousands it is PCM.
    uint32_t m_dac_writes = 0;
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
