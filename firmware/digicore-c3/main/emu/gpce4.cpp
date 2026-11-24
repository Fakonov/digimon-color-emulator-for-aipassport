// ---------------------------------------------------------------------------
//  gpce4.cpp - Generalplus GPCE4 SoC (unSP 2.0, Sunplus)
//
//  Port of MAME 0.289:
//    ref/generalplus_gpce4_soc.cpp   the SoC (memory map, IO, timers, IRQs, SPI)
//    ref/generalplus_gpce4.cpp       the digicolr machine wiring (96 MHz CPU,
//                                    PORTA/B/C, D/C on IOB 0x0400, 300 kHz SPI2
//                                    interrupt source)
//    machine/generic_spi_flash.cpp   the SPI flash device behind SPI1
//
//  The MAME device tree is flattened: what used to be callbacks into the
//  machine driver (m_porta_in(), m_portb_out(), m_spi_out(), m_spi2_out(),
//  m_spi_reset()) is wired directly to the port latches, the SPI flash model
//  and the St7735 in this file.  Timers are driven by tick(cycles) with the CPU
//  clock (96 MHz) instead of MAME's attotime scheduler.
// ---------------------------------------------------------------------------

#include "gpce4.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {

// ref/generalplus_gpce4.cpp:370  "GENERALPLUS_GPCE4_" CPU at 96 MHz
constexpr uint32_t CPU_CLOCK = 96000000;

// Timer periods in CPU cycles.  MAME creates these as attotime frequencies in
// device_add_mconfig() (ref/generalplus_gpce4_soc.cpp:138-144); at 96 MHz every
// one of them is an exact integer number of cycles.
constexpr uint64_t TICKS_1KHZ    = CPU_CLOCK / 1000;       // "timer_c"   ->  96000
constexpr uint64_t TICKS_20KHZ   = CPU_CLOCK / 20000;      // "timer_a"   ->   4800
constexpr uint64_t TICKS_2HZ     = CPU_CLOCK / 2;          // "timer_2hz" -> 48e6
constexpr uint64_t TICKS_64HZ    = CPU_CLOCK / 64;         // "timer_64hz"-> 1.5e6
constexpr uint64_t TICKS_2KHZ    = CPU_CLOCK / 2000;       // "timer_2khz"->  48000
constexpr uint64_t TICKS_300KHZ  = CPU_CLOCK / 300000;     // driver timer ->   320
constexpr uint64_t TICKS_SPI_TX  = CPU_CLOCK / 48000000;   // 48 MHz shifter ->  2

// Bit indices inside m_interrupt_status, i.e. the "which" argument of MAME's
// request_interrupt() (ref/generalplus_gpce4_soc.cpp:964).
enum {
    SRC_TIMER_2HZ   = 0,     // "timer_2hz"   -> INT2 bit 0 -> IRQ7
    SRC_TIMER_64HZ  = 2,     // "timer_64hz"  -> INT2 bit 2 -> IRQ7
    SRC_TIMER_2KHZ  = 9,     // "timer_2khz"  -> INT2 bit 9 -> IRQ6
    SRC_SPI2        = 24,    // driver 300 kHz timer -> INT bit 8 -> IRQ3
    SRC_TIMER_C     = 29,    // "timer_c"     -> INT bit 13 -> IRQ2 (game speed)
    SRC_TIMER_A     = 31     // "timer_a"     -> INT bit 15 -> IRQ0
};

constexpr uint32_t ADDR_MASK = 0x7fffffu;   // 23-bit word address

} // anonymous namespace


// ===========================================================================
//  Gpce4SpiFlash - port of MAME's generic_spi_flash_device
// ===========================================================================
void Gpce4SpiFlash::set_image(SparseRom *rom)
{
    m_rom = rom;
}

// generic_spi_flash.cpp: device_reset() - addressing, state and latch only; the
// status/config registers survive a reset (they live in device_start there).
void Gpce4SpiFlash::reset()
{
    m_addr = 0;
    m_state = READY_FOR_COMMAND;
    m_latch = 0;
    m_state_step = 0;
}

uint8_t Gpce4SpiFlash::rom_byte(uint32_t addr) const
{
    if (!m_rom)
        return 0xff;
    const uint32_t size = m_rom->size();
    if (size == 0)
        return 0xff;
    if ((size & (size - 1)) == 0)              // power of two: MAME's mask
        return m_rom->read8(addr & (size - 1));
    return m_rom->read8(addr % size);
}

// generic_spi_flash.cpp: write(u8 data) - the only entry point; one byte per
// clock edge, driven by the SoC's transmit timer.
void Gpce4SpiFlash::write(uint8_t data)
{
    switch (m_state) {
    case READY_FOR_COMMAND:
        switch (data) {
        case 0x01: m_state = COMMAND_01_WRSR;        m_state_step = 0; break;
        case 0x02: m_state = COMMAND_02_PP;          m_state_step = 0; m_n_pp++; break;
        case 0x03: m_state = COMMAND_03_READ;        m_state_step = 0; break;
        case 0x04: m_statusreg = uint8_t(m_statusreg & ~0x02); break;   // WRDI
        case 0x05: m_state = COMMAND_05_RDSR;        m_state_step = 0; break;
        case 0x06: m_statusreg = uint8_t(m_statusreg | 0x02); m_n_wren++; break;  // WREN
        case 0x0b: m_state = COMMAND_0B_FAST_READ;   m_state_step = 0; break;
        case 0x11: m_state = COMMAND_11_UNKNOWN;     m_state_step = 0; break;
        case 0x15: m_state = COMMAND_15_RDCR;        m_state_step = 0; break;
        case 0x20: m_state = COMMAND_20_SE;          m_state_step = 0; m_n_se++; break;
        case 0x31: m_state = COMMAND_31_UNKNOWN;     m_state_step = 0; break;
        case 0x35: m_state = COMMAND_35_RDSR2;       m_state_step = 0; break;
        case 0x66: break;                                            // enable reset
        case 0x90: m_state = COMMAND_90_REMS;        m_state_step = 0; break;
        case 0x99: break;                                            // reset
        case 0x9f: m_state = COMMAND_9F_RDID;        m_state_step = 0; m_n_rdid++; break;
        case 0xab: break;                                            // release DP
        case 0xb9: break;                                            // deep power down
        case 0xeb: m_state = COMMAND_EB_4READ;       m_state_step = 0; break;
        case 0xec: m_state = COMMAND_EC_UNKNOWN;     m_state_step = 0; break;
        case 0xff: break;                                            // CRMR
        default:
            // MAME: fatalerror("SPI set to unknown/unhandled command %02x")
            m_n_unknown++;
            std::printf("[spi_flash] unhandled command %02x (ignored)\n", unsigned(data));
            break;
        }
        if (trace)
            std::printf("[spi_flash] command %02x -> state %d\n", unsigned(data), m_state);
        return;

    // ---- write status register -------------------------------------------
    case COMMAND_01_WRSR:
        if (m_state_step == 0) {
            m_configreg = data;
            if (m_multibyte_status_write) m_state_step++;
            else                          m_state = READY_FOR_COMMAND;
        } else {
            m_statusreg = data;
            m_state = READY_FOR_COMMAND;
        }
        return;

    // ---- page program ----------------------------------------------------
    case COMMAND_02_PP:
        if (m_state_step == 0) {
            m_addr = (m_addr & 0x00ffff) | (uint32_t(data) << 16);
            m_state_step++;
        } else if (m_state_step == 1) {
            m_addr = (m_addr & 0xff00ff) | (uint32_t(data) << 8);
            m_state_step++;
        } else if (m_state_step == 2) {
            m_addr = (m_addr & 0xffff00) | data;
            m_state_step++;
        } else if (m_statusreg & 0x02) {
            if (writable && m_rom && m_addr < m_rom->size()) {
                // A NOR page program can only clear bits.
                m_rom->program_byte(m_addr, data);
                m_n_bytes_pp++;
            } else if (!writable) {
                std::printf("[spi_flash] page program %06x = %02x ignored (read-only image)\n",
                            m_addr, unsigned(data));
            }
            m_addr++;
        }
        return;

    // ---- reads -----------------------------------------------------------
    case COMMAND_03_READ:
        if (m_state_step == 0) {
            m_addr = (m_addr & 0x00ffff) | (uint32_t(data) << 16);
            m_state_step++;
        } else if (m_state_step == 1) {
            m_addr = (m_addr & 0xff00ff) | (uint32_t(data) << 8);
            m_state_step++;
        } else if (m_state_step == 2) {
            m_addr = (m_addr & 0xffff00) | data;
            m_state_step++;
        } else {
            m_latch = rom_byte(m_addr++);
        }
        return;

    case COMMAND_0B_FAST_READ:
        if (m_state_step == 0) {
            m_addr = (m_addr & 0x00ffff) | (uint32_t(data) << 16);
            m_state_step++;
        } else if (m_state_step == 1) {
            m_addr = (m_addr & 0xff00ff) | (uint32_t(data) << 8);
            m_state_step++;
        } else if (m_state_step == 2) {
            m_addr = (m_addr & 0xffff00) | data;
            m_state_step++;
        } else if (m_state_step == 3) {          // dummy byte
            m_state_step++;
        } else {
            m_latch = rom_byte(m_addr++);
        }
        return;

    case COMMAND_EB_4READ:
        if (m_state_step == 0) {
            m_addr = (m_addr & 0x00ffff) | (uint32_t(data) << 16);
            m_state_step++;
        } else if (m_state_step == 1) {
            m_addr = (m_addr & 0xff00ff) | (uint32_t(data) << 8);
            m_state_step++;
        } else if (m_state_step == 2) {
            m_addr = (m_addr & 0xffff00) | data;
            m_state_step++;
        } else if (m_state_step < 6) {           // three dummy bytes
            m_state_step++;
        } else {
            m_latch = rom_byte(m_addr++);
        }
        return;

    // ---- status / config -------------------------------------------------
    case COMMAND_05_RDSR:
        if (m_state_step == 0) {
            m_latch = m_statusreg;
            if (m_multibyte_status_read) m_state_step++;
            else                         m_state = READY_FOR_COMMAND;
        } else {
            m_latch = 0x00;
            m_state = READY_FOR_COMMAND;
        }
        return;

    case COMMAND_35_RDSR2:                       // status register 2
        m_latch = m_statusreg;
        m_state = READY_FOR_COMMAND;
        return;

    case COMMAND_15_RDCR:                        // configuration register
        m_latch = m_configreg;
        m_state = READY_FOR_COMMAND;
        return;

    // ---- JEDEC id --------------------------------------------------------
    case COMMAND_9F_RDID:
        if (m_state_step == 0) {
            m_latch = 0xfe;
            m_state_step++;
        } else if (m_state_step == 1) {
            m_latch = 0xfe;
            m_state_step++;
        } else {
            m_latch = 0x00;                      // MAME keeps repeating id[2]
        }
        return;

    // ---- mode reset ------------------------------------------------------
    case COMMAND_90_REMS:
        if (m_state_step < 5) m_state_step++;
        else                  m_state = READY_FOR_COMMAND;
        return;

    // ---- sector erase (MAME only decodes the address) --------------------
    case COMMAND_20_SE:
        if (m_state_step == 0) {
            m_addr = (m_addr & 0x00ffff) | (uint32_t(data) << 16);
            m_state_step++;
        } else if (m_state_step == 1) {
            m_addr = (m_addr & 0xff00ff) | (uint32_t(data) << 8);
            m_state_step++;
        } else if (m_state_step == 2) {
            m_addr = (m_addr & 0xffff00) | data;
            if (writable && m_rom) {
                m_rom->erase_sector(m_addr);
            } else {
                std::printf("[spi_flash] sector erase %06x ignored (read-only image)\n", m_addr);
            }
            m_state = READY_FOR_COMMAND;
        }
        return;

    // ---- commands that only swallow their payload -------------------------
    case COMMAND_11_UNKNOWN:
    case COMMAND_31_UNKNOWN:
    case COMMAND_EC_UNKNOWN:
        return;

    default:
        std::printf("[spi_flash] byte %02x in unhandled state %d\n", unsigned(data), m_state);
        m_state = READY_FOR_COMMAND;
        return;
    }
}


// ===========================================================================
//  Gpce4 - construction / reset / images
// ===========================================================================
Gpce4::Gpce4()
{
    reset();
}

void Gpce4::set_images(const uint8_t *irom, size_t irom_size, SparseRom *flash)
{
    m_irom = irom;
    m_irom_size = irom_size;
    m_flash = flash;
    m_spi.set_image(flash);
}

void Gpce4::set_cpu(unsp_device *cpu)
{
    m_cpu = cpu;
    for (int i = 0; i < 9; i++) m_irq_line[i] = false;   // lines start released
    // Flat RAM view: the CPU hot path skips the bus vtable, and the statically
    // recompiled block operates directly on the live word array.
    cpu->m_ram = ram;
    cpu->m_ram_limit = 0x0fff;
    update_interrupts();
}

void Gpce4::set_port(int port, uint16_t bits)
{
    if (port < 0 || port > 2)
        return;
    m_port_in[port] = bits;
}

// ref/generalplus_gpce4_soc.cpp device_reset(): every register cleared, FIFOs
// reset.  The MAME timer devices keep running across a reset; here the cycle
// counter and all deadlines restart so the harness gets a deterministic phase.
void Gpce4::reset()
{
    std::memset(ram, 0, sizeof(ram));
    lcd.reset();

    // ioport defaults from ref/generalplus_gpce4.cpp:284-299: active low
    // buttons released, battery "not charging", charger "not connected".
    m_port_in[0] = 0xffff;
    m_port_in[1] = 0xffef;
    m_port_in[2] = 0xffff;

    m_ioa_buffer = 0;
    m_iob_buffer = 0;
    m_ioa_direction = 0;
    m_iob_direction = 0;
    m_ioc_direction = 0;
    m_ioa_attribute = 0;
    m_iob_attribute = 0;
    m_ioc_attribute = 0;
    m_io_ctrl = 0;
    m_iob_out = 0;

    m_timer_ctrl = 0;
    m_cache_ctrl = 0;
    m_wait_ctrl = 0;
    m_dac_cha1 = 0;
    m_spi_auto_ctrl = 0;
    m_spi_bank = 0;
    m_spi_ctrl = 0;
    m_spi2_ctrl = 0;

    m_interrupt_status = 0;
    m_interrupt_ctrl = 0;
    m_interrupt2_ctrl = 0;
    m_fiq_sel = 0;
    m_fiq2_sel = 0;
    std::fill(m_irq_line, m_irq_line + 9, false);

    m_spi2_txstatus = 0;
    m_spi2_tx_fill = 0;

    // digicolr's machine_start(): single byte status reads *and* writes
    // (ref/generalplus_gpce4.cpp:301-320).
    m_spi.set_single_byte_status_read();
    m_spi.set_single_byte_status_writes();
    m_spi.reset();
    reset_spi_fifos();

    m_now = 0;
    const uint64_t periods[SRC_COUNT] = {
        TICKS_1KHZ, TICKS_20KHZ, TICKS_2HZ, TICKS_64HZ, TICKS_2KHZ, TICKS_300KHZ
    };
    const int irqs[SRC_COUNT] = {
        SRC_TIMER_C, SRC_TIMER_A, SRC_TIMER_2HZ, SRC_TIMER_64HZ, SRC_TIMER_2KHZ, SRC_SPI2
    };
    for (int i = 0; i < SRC_COUNT; i++) {
        m_timers[i].period = periods[i];
        m_timers[i].next = periods[i];
        m_timers[i].irq = irqs[i];
    }
    m_spi_tx_armed = false;
    m_spi_tx_next = 0;

    m_unmapped_read_count = 0;
    m_unmapped_write_count = 0;
    std::memset(m_unmapped_read_log, 0, sizeof(m_unmapped_read_log));
    std::memset(m_unmapped_write_log, 0, sizeof(m_unmapped_write_log));
}


// ===========================================================================
//  Machine snapshots
//
//  reset() above is the list of everything that has to survive a resume; the
//  two functions below mirror it field for field.  Timers are left out on
//  purpose: their phase is derived from m_now, and a snapshot taken between two
//  host frames only ever costs one tick of jitter, never a lost interrupt.
// ===========================================================================
void Gpce4::save_state(Gpce4State &st) const
{
    std::memset(&st, 0, sizeof(st));
    st.magic = GPCE4_STATE_MAGIC;
    st.version = GPCE4_STATE_VERSION;
    st.size = uint16_t(sizeof(Gpce4State));

    for (int i = 0; i < 3; i++)
        st.port_in[i] = m_port_in[i];
    st.ioa_buffer = m_ioa_buffer;
    st.iob_buffer = m_iob_buffer;
    st.ioa_direction = m_ioa_direction;
    st.iob_direction = m_iob_direction;
    st.ioc_direction = m_ioc_direction;
    st.ioa_attribute = m_ioa_attribute;
    st.iob_attribute = m_iob_attribute;
    st.ioc_attribute = m_ioc_attribute;
    st.io_ctrl = m_io_ctrl;
    st.iob_out = m_iob_out;
    st.timer_ctrl = m_timer_ctrl;
    st.cache_ctrl = m_cache_ctrl;
    st.wait_ctrl = m_wait_ctrl;
    st.wake_mask_a = m_wake_mask_a;
    st.wake_mask_b = m_wake_mask_b;
    st.wake_mask_c = m_wake_mask_c;
    st.dac_cha1 = m_dac_cha1;
    st.dac_cha2 = m_dac_cha2;

    st.spi_auto_ctrl = m_spi_auto_ctrl;
    st.spi_bank = m_spi_bank;
    st.spi_ctrl = m_spi_ctrl;
    st.spi2_ctrl = m_spi2_ctrl;
    st.spi2_txstatus = m_spi2_txstatus;
    for (int i = 0; i < 8; i++) {
        st.spi_tx_fifo[i] = m_spi_tx_fifo[i];
        st.spi_rx_fifo[i] = m_spi_rx_fifo[i];
    }
    st.tx_fifo_entries = int8_t(m_tx_fifo_entries);
    st.rx_fifo_entries = int8_t(m_rx_fifo_entries);
    st.spi_tx_armed = m_spi_tx_armed ? 1 : 0;
    st.spi2_tx_fill = int8_t(m_spi2_tx_fill);

    st.interrupt_status = m_interrupt_status;
    st.interrupt_ctrl = m_interrupt_ctrl;
    st.interrupt2_ctrl = m_interrupt2_ctrl;
    st.fiq_sel = m_fiq_sel;
    st.fiq2_sel = m_fiq2_sel;
    for (int i = 0; i < 9; i++)
        st.irq_line[i] = m_irq_line[i] ? 1 : 0;

    st.sleep_writes = m_sleep_writes;
    st.asleep = m_asleep ? 1 : 0;

    st.lcd_display_on = lcd.display_on ? 1 : 0;
    st.lcd_sleep = lcd.sleep ? 1 : 0;
    st.lcd_madctl = lcd.m_madctl;
    st.lcd_colmod = lcd.m_colmod;
}

bool Gpce4::restore_state(const Gpce4State &st)
{
    if (st.magic != GPCE4_STATE_MAGIC || st.version != GPCE4_STATE_VERSION ||
        st.size != uint16_t(sizeof(Gpce4State)))
        return false;

    for (int i = 0; i < 3; i++)
        m_port_in[i] = st.port_in[i];
    m_ioa_buffer = st.ioa_buffer;
    m_iob_buffer = st.iob_buffer;
    m_ioa_direction = st.ioa_direction;
    m_iob_direction = st.iob_direction;
    m_ioc_direction = st.ioc_direction;
    m_ioa_attribute = st.ioa_attribute;
    m_iob_attribute = st.iob_attribute;
    m_ioc_attribute = st.ioc_attribute;
    m_io_ctrl = st.io_ctrl;
    m_iob_out = st.iob_out;
    m_timer_ctrl = st.timer_ctrl;
    m_cache_ctrl = st.cache_ctrl;
    m_wait_ctrl = st.wait_ctrl;
    m_wake_mask_a = st.wake_mask_a;
    m_wake_mask_b = st.wake_mask_b;
    m_wake_mask_c = st.wake_mask_c;
    m_dac_cha1 = st.dac_cha1;
    m_dac_cha2 = st.dac_cha2;

    m_spi_auto_ctrl = st.spi_auto_ctrl;
    m_spi_bank = st.spi_bank;
    m_spi_ctrl = st.spi_ctrl;
    m_spi2_ctrl = st.spi2_ctrl;
    m_spi2_txstatus = st.spi2_txstatus;
    for (int i = 0; i < 8; i++) {
        m_spi_tx_fifo[i] = st.spi_tx_fifo[i];
        m_spi_rx_fifo[i] = st.spi_rx_fifo[i];
    }
    m_tx_fifo_entries = st.tx_fifo_entries;
    m_rx_fifo_entries = st.rx_fifo_entries;
    m_spi_tx_armed = st.spi_tx_armed != 0;
    m_spi2_tx_fill = st.spi2_tx_fill;

    m_interrupt_status = st.interrupt_status;
    m_interrupt_ctrl = st.interrupt_ctrl;
    m_interrupt2_ctrl = st.interrupt2_ctrl;
    m_fiq_sel = st.fiq_sel;
    m_fiq2_sel = st.fiq2_sel;
    for (int i = 0; i < 9; i++)
        m_irq_line[i] = st.irq_line[i] != 0;

    m_sleep_writes = st.sleep_writes;
    m_asleep = st.asleep != 0;

    // The panel comes back with its mode registers intact but its RAMWR window
    // closed.  The guest opens a window before every burst, so a closed one only
    // costs the pixels of a burst that was in flight when the snapshot was taken
    // - and the next full repaint covers them.  Restoring a half-open window
    // instead scatters that frame into the two hidden 8-row strips.
    lcd.display_on = st.lcd_display_on != 0;
    lcd.sleep = st.lcd_sleep != 0;
    lcd.m_madctl = st.lcd_madctl;
    lcd.m_colmod = st.lcd_colmod;
    lcd.m_posx = lcd.m_posy = 0;
    // Full-screen window, not a 1x1 one: a RAMWR burst that arrives before the
    // guest reopens CASET/RASET must still land somewhere sensible.  With
    // min==max==0 every pixel outside [0,0] is dropped and the screen stays
    // black while the guest happily keeps pushing colours.
    lcd.m_posminx = 0;
    lcd.m_posmaxx = 127;
    lcd.m_posminy = 0;
    lcd.m_posmaxy = 127;
    lcd.m_command = 0;
    lcd.m_commandstep = 0;
    lcd.m_pixellatch = 0;

    // Re-drive the CPU interrupt lines so the very next instruction sees the
    // restored gate instead of waiting for the next timer edge.
    update_interrupts();
    return true;
}


// ===========================================================================
//  Memory map - ref/generalplus_gpce4_soc.cpp:150-267 (internal_map)
// ===========================================================================
uint16_t Gpce4::internal_rom_r(uint32_t word_ofs)
{
    const size_t ofs = size_t(word_ofs) * 2;
    if (!m_irom || ofs + 1 >= m_irom_size)
        return 0xffff;
    return uint16_t(m_irom[ofs] | (m_irom[ofs + 1] << 8));
}

// ref/generalplus_gpce4_soc.cpp:879  spi_direct_r()
//   return m_spiregion[(offset*2)+0 + (m_spi_bank*0x400000)]
//        | m_spiregion[(offset*2)+1 + (m_spi_bank*0x400000)] << 8;
// SPI_Auto_Ctrl is *not* consulted by MAME; the window always serves the flash.
uint16_t Gpce4::spi_direct_r(uint32_t offset)
{
    if (!m_flash)
        return 0x0000;
    const uint64_t base = uint64_t(offset) * 2u + uint64_t(m_spi_bank) * 0x400000u;
    if (base + 1 >= m_flash->size()) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            std::printf("[gpce4] flash window read past end (%08x, bank %d) -> ffff\n",
                        unsigned(base), int(m_spi_bank));
        }
        return 0xffff;
    }
    return m_flash->read16(uint32_t(base));
}

bool Gpce4::note_unmapped(uint32_t *list, int &count, uint32_t addr)
{
    for (int i = 0; i < count; i++)
        if (list[i] == addr)
            return false;
    if (count >= UNMAPPED_LOG_MAX)
        return false;
    list[count++] = addr;
    return true;
}

uint16_t Gpce4::unmapped_read(uint32_t addr)
{
    if (note_unmapped(m_unmapped_read_log, m_unmapped_read_count, addr))
        std::printf("[gpce4] unmapped read  %06x -> ffff\n", unsigned(addr));
    return 0xffff;
}

void Gpce4::unmapped_write(uint32_t addr, uint16_t data)
{
    if (note_unmapped(m_unmapped_write_log, m_unmapped_write_count, addr))
        std::printf("[gpce4] unmapped write %06x = %04x (ignored)\n", unsigned(addr), unsigned(data));
}

// Opcode fetch fast path: hand the CPU a host pointer for the 4 KiB page that
// holds `addr`.  The sparse flash store keeps each present page contiguous, so
// only pages that are actually in the image get a window; anything else (IO,
// absent pages) falls back to read16().
const uint16_t *Gpce4::fetch_window(uint32_t addr, uint32_t *base, uint32_t *mask)
{
    constexpr uint32_t WINDOW_MASK = 0x7ff;                 // 2048 words = 4 KiB
    addr &= ADDR_MASK;

    if (addr < 0x001000) {                                  // word-addressed RAM
        *base = addr & ~WINDOW_MASK;
        *mask = WINDOW_MASK;
        return ram + *base;
    }

    if (addr >= 0x004000 && addr <= 0x00bfff && m_irom) {   // 64 KB internal mask ROM
        *base = addr & ~WINDOW_MASK;
        *mask = WINDOW_MASK;
        return reinterpret_cast<const uint16_t *>(m_irom + (*base - 0x004000) * 2);
    }

    if (addr >= 0x200000 && addr <= 0x3fffff && m_flash) {  // SPI flash page store
        const uint32_t off = (addr - 0x200000) * 2;
        const uint16_t *page = m_flash->page_window(off);
        if (page) {
            *base = addr & ~WINDOW_MASK;
            *mask = WINDOW_MASK;
            return page;
        }
    }

    *base = 0xffffffffu;
    *mask = 0;
    return nullptr;
}

uint16_t Gpce4::read16(uint32_t addr)
{
    addr &= ADDR_MASK;

    if (addr < 0x001000)                       // 0x000000-0x000fff  RAM
        return ram[addr];

    if (addr >= 0x004000 && addr <= 0x00bfff)  // 64 KB internal mask ROM
        return internal_rom_r(addr - 0x004000);

    if (addr >= 0x00c000 && addr <= 0x00ffff)  // first 32 KB mirrored
        return internal_rom_r(addr - 0x00c000);

    if (addr >= 0x003000 && addr <= 0x0030ef)  // IO block
        return io_r(uint16_t(addr));

    if (addr >= 0x200000 && addr <= 0x3fffff)  // SPI flash direct window
        return spi_direct_r(addr - 0x200000);

    return unmapped_read(addr);
}

// Frame-tick store counters, defined next to request_interrupt() further down;
// declared here because write16() needs them on the RAM fast path.
extern uint32_t g_tick_writes;
extern uint32_t g_tick_pc;
extern uint32_t g_tick_val;

void Gpce4::write16(uint32_t addr, uint16_t data)
{
    addr &= ADDR_MASK;

    if (addr < 0x001000) {
        if (addr == 0x0850) {           // frame-tick counter, diagnostic only
            g_tick_writes++;
            g_tick_val = data;
            if (m_cpu)
                g_tick_pc = (((uint32_t(m_cpu->m_core->m_r[unsp_device::REG_SR]) >> 10) & 0x3fu) << 16) |
                            (m_cpu->m_core->m_r[unsp_device::REG_PC] & 0xffffu);
        }
        ram[addr] = data;
        return;
    }

    if (addr >= 0x004000 && addr <= 0x00ffff) {  // ROM is read only
        unmapped_write(addr, data);
        return;
    }

    if (addr >= 0x003000 && addr <= 0x0030ef) {
        io_w(uint16_t(addr), data);
        return;
    }

    if (addr >= 0x200000 && addr <= 0x3fffff) {  // flash window, read only
        unmapped_write(addr, data);
        return;
    }

    unmapped_write(addr, data);
}


// ===========================================================================
//  IO block dispatch - ref/generalplus_gpce4_soc.cpp:150-267
// ===========================================================================
uint16_t Gpce4::io_r(uint16_t ofs)
{
    switch (ofs) {
    case 0x3000: return ioa_data_r();
    case 0x3001: return ioa_buffer_r();
    case 0x3002: return ioa_direction_r();
    case 0x3003: return ioa_attribute_r();
    case 0x3004: return iob_data_r();
    case 0x3005: return iob_buffer_r();
    case 0x3006: return iob_direction_r();
    case 0x3007: return iob_attribute_r();
    case 0x3008: return ioc_data_r();
    case 0x3009: return ioc_buffer_r();
    case 0x300a: return ioc_direction_r();
    case 0x300b: return ioc_attribute_r();
    case 0x300f: return io_ctrl_r();
    case 0x3016: return timer_ctrl_r();
    case 0x3020: return pwm0_ctrl_r();
    case 0x3021: return pwm1_ctrl_r();
    case 0x3022: return pwm2_ctrl_r();
    case 0x3023: return pwm3_ctrl_r();
    case 0x3039: return cache_ctrl_r();
    case 0x3050: return interrupt_ctrl_r();
    case 0x3051: return interrupt_status_r();
    case 0x3052: return fiq_sel_r();
    case 0x3053: return interrupt2_ctrl_r();
    case 0x3054: return interrupt2_status_r();
    case 0x3055: return fiq2_sel_r();
    case 0x3070: return adc_ctrl_r();
    case 0x3071: return adc_data_r();
    case 0x3072: return adc_linein_bitctrl_r();
    case 0x3090: return spi2_ctrl_r();
    case 0x3091: return spi2_txstatus_r();
    case 0x3093: return spi2_rxstatus_r();
    case 0x3094: return spi2_rxdata_r();
    case 0x3095: return spi2_misc_r();
    case 0x30e0: return spi_ctrl_r();
    case 0x30e3: return spi_rxstatus_r();
    case 0x30e4: return spi_rxdata_r();
    case 0x30e5: return spi_misc_r();
    case 0x30e6: return spi_man_ctrl_r();
    case 0x30e7: return spi_auto_ctrl_r();
    case 0x30e9: return spi_bank_r();
    default:
        // Write-only registers (0x3010-0x3015, 0x3030, 0x3033-0x3035,
        // 0x3040-0x3043, 0x3092, 0x30e1, 0x30e2) and the holes in the map all
        // read back as 0xffff in MAME.
        return unmapped_read(ofs);
    }
}

void Gpce4::io_w(uint16_t ofs, uint16_t data)
{
    switch (ofs) {
    case 0x3000: ioa_data_w(data); return;
    case 0x3001: ioa_buffer_w(data); return;
    case 0x3002: ioa_direction_w(data); return;
    case 0x3003: ioa_attribute_w(data); return;
    case 0x3004: iob_data_w(data); return;
    case 0x3005: iob_buffer_w(data); return;
    case 0x3006: iob_direction_w(data); return;
    case 0x3007: iob_attribute_w(data); return;
    case 0x3008: ioc_data_w(data); return;
    case 0x3009: ioc_buffer_w(data); return;
    case 0x300a: ioc_direction_w(data); return;
    case 0x300b: ioc_attribute_w(data); return;
    case 0x300f: io_ctrl_w(data); return;
    case 0x3010: timera_data_w(data); return;
    case 0x3011: timera_counter_w(data); return;
    case 0x3012: timerb_data_w(data); return;
    case 0x3013: timerb_counter_w(data); return;
    case 0x3014: timerc_data_w(data); return;
    case 0x3015: timerc_counter_w(data); return;
    case 0x3016: timer_ctrl_w(data); return;
    case 0x3020: pwm0_ctrl_w(data); return;
    case 0x3021: pwm1_ctrl_w(data); return;
    case 0x3022: pwm2_ctrl_w(data); return;
    case 0x3023: pwm3_ctrl_w(data); return;
    case 0x3030: system_clock_w(data); return;
    case 0x3033: timebase_clear_w(data); return;
    case 0x3034: watchdog_clear_w(data); return;
    case 0x3035: wait_ctrl_w(data); return;

    case 0x3036: system_sleep_w(data); return;
    case 0x300c: wakeup_mask_w(0, data); return;
    case 0x300d: wakeup_mask_w(1, data); return;
    case 0x300e: wakeup_mask_w(2, data); return;
    case 0x3039: cache_ctrl_w(data); return;
    case 0x3040: dac_ctrl_w(data); return;
    case 0x3041: dac_cha1_data_w(data); return;
    case 0x3042: dac_cha2_data_w(data); return;
    case 0x3043: ppam_ctrl_w(data); return;
    case 0x3050: interrupt_ctrl_w(data); return;
    case 0x3051: interrupt_status_w(data); return;
    case 0x3052: fiq_sel_w(data); return;
    case 0x3053: interrupt2_ctrl_w(data); return;
    case 0x3054: interrupt2_status_w(data); return;
    case 0x3055: fiq2_sel_w(data); return;
    case 0x3070: adc_ctrl_w(data); return;
    case 0x3071: adc_data_w(data); return;
    case 0x3072: adc_linein_bitctrl_w(data); return;
    case 0x3090: spi2_ctrl_w(data); return;
    case 0x3091: spi2_txstatus_w(data); return;
    case 0x3092: spi2_txdata_w(data); return;
    case 0x3093: spi2_rxstatus_w(data); return;
    case 0x3095: spi2_misc_w(data); return;
    case 0x30e0: spi_ctrl_w(data); return;
    case 0x30e1: spi_txstatus_w(data); return;
    case 0x30e2: spi_txdata_w(data); return;
    case 0x30e3: spi_rxstatus_w(data); return;
    case 0x30e5: spi_misc_w(data); return;
    case 0x30e6: spi_man_ctrl_w(data); return;
    case 0x30e7: spi_auto_ctrl_w(data); return;
    case 0x30e9: spi_bank_w(data); return;
    default:
        unmapped_write(ofs, data);
        return;
    }
}


// ===========================================================================
//  Ports A/B/C - ref/generalplus_gpce4_soc.cpp:296-440
//  ioa_data_r/ioa_buffer_r return the *input* port; iob_buffer_r returns the
//  latched buffer; ioc_buffer_r returns the input port again.  That asymmetry
//  is MAME's, not a typo here.
// Port A read counter, defined with the interrupt counters further down.
extern uint32_t g_ioa_reads;

uint16_t Gpce4::ioa_data_r()
{
#ifdef ESP_PLATFORM
    g_ioa_reads++;
    {
        static unsigned n = 0;
        if ((++n % 5000u) == 1u) {
            uint32_t pc = 0;
            if (m_cpu) {
                pc = (((uint32_t(m_cpu->m_core->m_r[unsp_device::REG_SR]) >> 10) & 0x3fu) << 16) |
                     m_cpu->m_core->m_r[unsp_device::REG_PC];
            }
            std::printf("[ioa] reads=%u val=%04x pc=%06x\n", n,
                        unsigned(m_port_in[0]), unsigned(pc));
        }
    }
#endif
    return m_port_in[0];
}                                                                            // :296
void     Gpce4::ioa_data_w(uint16_t) { /* m_porta_out(data): nothing wired */ } // :302

uint16_t Gpce4::ioa_buffer_r()
{
#ifdef ESP_PLATFORM
    {
        static unsigned n = 0;
        if ((++n % 5000u) == 1u)
            std::printf("[iob] reads=%u val=%04x\n", n, unsigned(m_port_in[0]));
    }
#endif
    return m_port_in[0];
}                                                                            // :308
void     Gpce4::ioa_buffer_w(uint16_t data){ m_ioa_buffer = data; }             // :314

uint16_t Gpce4::ioa_direction_r(){ return m_ioa_direction; }                    // :321
void     Gpce4::ioa_direction_w(uint16_t data){ m_ioa_direction = data; }       // :327

uint16_t Gpce4::ioa_attribute_r(){ return m_ioa_attribute; }                    // :333
void     Gpce4::ioa_attribute_w(uint16_t data){ m_ioa_attribute = data; }       // :339

uint16_t Gpce4::iob_data_r()     { return m_port_in[1]; }                       // :347
void     Gpce4::iob_data_w(uint16_t data){ m_iob_out = data; }                  // :353

uint16_t Gpce4::iob_buffer_r()   { return m_iob_buffer; }                       // :359
void     Gpce4::iob_buffer_w(uint16_t data){ m_iob_buffer = data; m_iob_out = data; } // :365

uint16_t Gpce4::iob_direction_r(){ return m_iob_direction; }                    // :372
void     Gpce4::iob_direction_w(uint16_t data){ m_iob_direction = data; }       // :378

uint16_t Gpce4::iob_attribute_r(){ return m_iob_attribute; }                    // :390
void     Gpce4::iob_attribute_w(uint16_t data){ m_iob_attribute = data; }       // :384

uint16_t Gpce4::ioc_data_r()     { return m_port_in[2]; }                       // :398
void     Gpce4::ioc_data_w(uint16_t) { /* m_portc_out(data): nothing wired */ } // :404

uint16_t Gpce4::ioc_buffer_r()   { return m_port_in[2]; }                       // :410
void     Gpce4::ioc_buffer_w(uint16_t) { /* m_portc_out(data) only */ }          // :416

uint16_t Gpce4::ioc_direction_r(){ return m_ioc_direction; }                    // :422
void     Gpce4::ioc_direction_w(uint16_t data){ m_ioc_direction = data; }       // :428

uint16_t Gpce4::ioc_attribute_r(){ return m_ioc_attribute; }                    // :440
void     Gpce4::ioc_attribute_w(uint16_t data){ m_ioc_attribute = data; }       // :434

uint16_t Gpce4::io_ctrl_r()      { return m_io_ctrl; }                          // :447
void     Gpce4::io_ctrl_w(uint16_t data){ m_io_ctrl = data; }                   // :453


// ===========================================================================
//  Timers (registers only - the periodic sources are in tick())
// ===========================================================================
void Gpce4::timera_data_w(uint16_t)     { }                                     // :473  log only
void Gpce4::timera_counter_w(uint16_t)  { }                                     // :478  log only
void Gpce4::timerb_data_w(uint16_t)     { }                                     // :483  log only
void Gpce4::timerb_counter_w(uint16_t)  { }                                     // :488  log only
void Gpce4::timerc_data_w(uint16_t)     { }                                     // :493  log only
void Gpce4::timerc_counter_w(uint16_t)  { }                                     // :498  log only
uint16_t Gpce4::timer_ctrl_r()          { return m_timer_ctrl; }                // :461
void Gpce4::timer_ctrl_w(uint16_t data) { m_timer_ctrl = data; }                // :467

// PWM: MAME only logs the write and never stores it, so reads stay 0.
uint16_t Gpce4::pwm0_ctrl_r() { return 0; }                                     // :505
uint16_t Gpce4::pwm1_ctrl_r() { return 0; }                                     // :511
uint16_t Gpce4::pwm2_ctrl_r() { return 0; }                                     // :517
uint16_t Gpce4::pwm3_ctrl_r() { return 0; }                                     // :523
void Gpce4::pwm0_ctrl_w(uint16_t) { m_pwm_writes++; }                         // :529
void Gpce4::pwm1_ctrl_w(uint16_t) { m_pwm_writes++; }                         // :534
void Gpce4::pwm2_ctrl_w(uint16_t) { m_pwm_writes++; }                         // :539
void Gpce4::pwm3_ctrl_w(uint16_t) { m_pwm_writes++; }                         // :544


// ===========================================================================
//  System / DAC / ADC
// ===========================================================================
void Gpce4::system_clock_w(uint16_t data)                              // :551
{
    if (trace_io) std::printf("[gpce4] System_Clock = %04x (ignored)\n", unsigned(data));
}

void Gpce4::timebase_clear_w(uint16_t data)                            // :556
{
    if (trace_io) std::printf("[gpce4] Timebase_Clear = %04x\n", unsigned(data));
}

void Gpce4::watchdog_clear_w(uint16_t data)                            // :561
{
    if (trace_io) std::printf("[gpce4] Watchdog_Clear = %04x\n", unsigned(data));
}

// ref/generalplus_gpce4_soc.cpp:567 - MAME stores nothing and does nothing.
// The unSP core in this project has no HALT/WAIT state to enter, and MAME does
// not model one either, so the write is recorded for inspection only.
void Gpce4::wait_ctrl_w(uint16_t data)                                 // :567
{
    m_wait_ctrl = data;
    if (trace_io) std::printf("[gpce4] Wait_Ctrl = %04x (not modelled)\n", unsigned(data));
}

// 0x3036 System_Sleep.  MAME leaves it unmapped; the ROM writes 0x5555 here
// 16 s after the last key press (measured: nothing at 17.0 s, the write is
// there at 17.4 s, and the boot animation accounts for the ~1.2 s offset).
// On hardware this parks the SoC and the backlight goes out with it; any of
// the three buttons wakes it through the masks at 0x300c-0x300e.
//
// We do not halt: the host has to keep polling the key ladder, and the ROM's
// wait loop is nearly free.  Recording the request is enough for the rest of
// the firmware to dim the panel and for the console to report the state.
void Gpce4::system_sleep_w(uint16_t data)
{
    m_sleep_writes++;
    // One line per boot is worth the UART time: whether the retail ROM ever asks
    // for System_Sleep on this board is the whole question behind "the panel goes
    // dark and the game asks CONTINUE/RESET", and it is invisible otherwise.
    static bool announced = false;
    if (!announced) {
        announced = true;
        std::printf("[gpce4] first System_Sleep write = %04x\n", unsigned(data));
    }
    // The ROM's power-down path writes 0x3036 and then runs off the end of its
    // own code (0x5340 and up is blank), so letting it continue means the next
    // key press can only replay power-on: the game comes back on CONTINUE/RESET
    // and the clock has to be set again.  On the handset the panel simply goes
    // dark and the machine picks up where it left off, so park the PC on the
    // game's frame loop before the ROM reaches the peripheral-shutdown writes
    // (0x3035/0x3030/0x3050/0x3051/0x3054).  The guest keeps running behind the
    // dark backlight; a key only has to bring the backlight back.
    m_asleep = true;
    if (m_sleep_resume_pc != 0 && m_cpu) {
        // The resume point was chosen for the clock screen, and dropping the
        // title screen onto the clock loop is what made a handset left alone for
        // 16 s come back in a state that no key could leave: [0x0C88] jumped
        // straight to 0x1C and the logo was still on the panel.  While the ROM
        // is still on its setup screen ([0x0C88] == 0) park it back on its own
        // key-wait loop (0x5240, the one that reads PORTA into [0x0853]) so the
        // pads keep working; the frame-wait loop it idles in is 0x230321, but
        // that one never samples the ports and a press parked there is lost.
        uint32_t resume = m_sleep_resume_pc;
        if (resume == 0x200300u && ram[0x0C88] == 0)
            resume = 0x200300u;
        m_cpu->m_core->m_r[unsp_device::REG_SR] =
            uint16_t((m_cpu->m_core->m_r[unsp_device::REG_SR] & 0xffc0u) |
                     ((resume >> 16) & 0x3fu));
        m_cpu->m_core->m_r[unsp_device::REG_PC] = uint16_t(resume & 0xffffu);
        if (trace_io)
            std::printf("[gpce4] System_Sleep = %04x -> parked on pc=%06x\n",
                        unsigned(data), unsigned(resume));
    } else if (trace_io) {
        std::printf("[gpce4] System_Sleep = %04x (asleep)\n", unsigned(data));
    }
}

void Gpce4::set_sleep_inhibit(uint32_t resume_pc)
{
    if (resume_pc != m_sleep_resume_pc) {
        std::printf("[gpce4] sleep resume point %06x -> %06x\n",
                    unsigned(m_sleep_resume_pc), unsigned(resume_pc));
    }
    m_sleep_resume_pc = resume_pc;
}

void Gpce4::wake_from_sleep()
{
    if (!m_asleep)
        return;
    m_asleep = false;
    // With a resume point configured the guest never really stopped: the panel
    // was dark, the frame loop kept ticking, and the RAM still holds the pet and
    // the clock.  All a wake-up has to do is clear the flag.
    if (m_sleep_resume_pc != 0) {
        if (trace_io) std::printf("[gpce4] wake from sleep (resume pc=%06x)\n",
                                  unsigned(m_sleep_resume_pc));
        return;
    }
    // Fallback (no resume point): a wake-up key on the handset is a reset, so
    // replay the same power-on sequence app_main performs: clear the register
    // file, then take the reset vector from the ROM header at 0x4017.  RAM is
    // untouched, which is why the pet survives.
    if (m_cpu) {
        for (int i = 0; i < 16; ++i)
            m_cpu->m_core->m_r[i] = 0;
        m_cpu->m_vectorbase = 0x4010;
        m_cpu->m_core->m_r[unsp_device::REG_PC] = read16(0x4010 + 0x7);
        m_cpu->m_core->m_enable_irq = 0;
        m_cpu->m_core->m_enable_fiq = 0;
        m_cpu->m_core->m_fir_move = 1;
        m_cpu->m_core->m_pri = 8;
        m_cpu->m_core->m_divq_bit = 0xffffffffu;
    }
    if (trace_io)
        std::printf("[gpce4] wake from sleep -> reset pc=%04x\n",
                    m_cpu ? unsigned(m_cpu->m_core->m_r[unsp_device::REG_PC]) : 0u);
}

void Gpce4::wakeup_mask_w(uint8_t port, uint16_t data)
{
    if (port == 0)
        m_wake_mask_a = data;
    else if (port == 1)
        m_wake_mask_b = data;
    else
        m_wake_mask_c = data;
}

uint16_t Gpce4::cache_ctrl_r()          { return uint16_t(m_cache_ctrl & ~0x0002); }  // :594
void     Gpce4::cache_ctrl_w(uint16_t data) { m_cache_ctrl = data; }                  // :600

void Gpce4::dac_ctrl_w(uint16_t data)                                  // :608
{
    if (trace_io) std::printf("[gpce4] DAC_Ctrl = %04x\n", unsigned(data));
}

// MAME forwards this to the DAC device (m_dac->data_w(data)).  There is no
// sample clock on this chip: the DAC simply holds the last value written, so
// the write rate *is* the sample rate and the guest is the only thing driving
// it.  Remember the value and count the writes (see m_dac_writes).
void Gpce4::dac_cha1_data_w(uint16_t data)                             // :613
{
    m_dac_cha1 = data;
    m_dac_writes++;
}

void Gpce4::dac_cha2_data_w(uint16_t data)                             // :620
{
    if (trace_io) std::printf("[gpce4] DAC_CH2_Data = %04x\n", unsigned(data));
    m_dac_cha2 = data;
    m_dac_writes++;
}

void Gpce4::ppam_ctrl_w(uint16_t data)                                 // :625
{
    if (trace_io) std::printf("[gpce4] PPAMCtrl = %04x\n", unsigned(data));
}

uint16_t Gpce4::adc_ctrl_r()               { return 0x0000; }           // :1159
void     Gpce4::adc_ctrl_w(uint16_t)       { }                          // :1165
uint16_t Gpce4::adc_data_r()               { return 0xffff; }           // :1170
void     Gpce4::adc_data_w(uint16_t)       { }                          // :1176
uint16_t Gpce4::adc_linein_bitctrl_r()     { return 0x0000; }           // :1148
void     Gpce4::adc_linein_bitctrl_w(uint16_t) { }                      // :1154


// ===========================================================================
//  Interrupt controller - ref/generalplus_gpce4_soc.cpp:907-1015
//  A single 32-bit pending word; INT_Status exposes bits 31:16, INT2_Status
//  exposes bits 15:0, and both clear on write-one.
// ===========================================================================
uint16_t Gpce4::interrupt_ctrl_r()  { return m_interrupt_ctrl; }                // :630
void Gpce4::interrupt_ctrl_w(uint16_t data)                                     // :635
{
    m_interrupt_ctrl = data;
    update_interrupts();
}

uint16_t Gpce4::interrupt_status_r() { return uint16_t((m_interrupt_status >> 16) & 0xffff); }  // :978

void Gpce4::interrupt_status_w(uint16_t data)                                   // :984
{
    for (int i = 0; i < 16; i++)
        if (data & (1u << i))
            clear_interrupt(i + 16);
}

uint16_t Gpce4::fiq_sel_r()  { return m_fiq_sel; }                              // :994
void Gpce4::fiq_sel_w(uint16_t data) { m_fiq_sel = data; }                      // :999

uint16_t Gpce4::interrupt2_ctrl_r() { return m_interrupt2_ctrl; }               // :676
void Gpce4::interrupt2_ctrl_w(uint16_t data)                                    // :681
{
    m_interrupt2_ctrl = data;
    update_interrupts();
}

uint16_t Gpce4::interrupt2_status_r() { return uint16_t(m_interrupt_status & 0xffff); }  // :659

void Gpce4::interrupt2_status_w(uint16_t data)                                  // :665
{
    for (int i = 0; i < 16; i++)
        if (data & (1u << i))
            clear_interrupt(i);
}

uint16_t Gpce4::fiq2_sel_r() { return m_fiq2_sel; }                             // :1006
void Gpce4::fiq2_sel_w(uint16_t data) { m_fiq2_sel = data; }                    // :1011

// Interrupt bookkeeping counters.  Added 2026-11-23 to settle whether the
// 64 Hz edge is ever raised and whether the guest ever clears it: the guest can
// only retire a frame once IRQ7 delivers, and IRQ7 needs status bit 0 or bit 2.
uint32_t g_src_set[32];
uint32_t g_src_clr[32];
uint32_t g_irq7_rise = 0;
uint32_t g_irq7_fall = 0;
uint32_t g_tick_writes = 0;         // stores to [0x0850]
uint32_t g_tick_pc = 0;             // PC of the most recent store
uint32_t g_tick_val = 0;            // value of the most recent store
uint32_t g_ioa_reads = 0;           // ioa_data_r() calls

void Gpce4::request_interrupt(int which)                                        // :964
{
    if (which < 0 || which >= 32)
        return;
    if (!(m_interrupt_status & (1u << which)))
        g_src_set[which]++;
    m_interrupt_status |= (1u << which);
    update_interrupts();
}

void Gpce4::clear_interrupt(int which)                                          // :971
{
    if (which < 0 || which >= 32)
        return;
    if (m_interrupt_status & (1u << which))
        g_src_clr[which]++;
    m_interrupt_status &= ~(1u << which);
    update_interrupts();
}

void Gpce4::set_line(int line, bool state)
{
    if (line < 0 || line >= 9)
        return;
    if (m_irq_line[line] == state)
        return;
    m_irq_line[line] = state;
    if (line == UNSP_IRQ7_LINE) {
        if (state) g_irq7_rise++; else g_irq7_fall++;
    }
    if (m_cpu)
        m_cpu->execute_set_input(line, state ? 1 : 0);
}

// ref/generalplus_gpce4_soc.cpp:907  update_interrupts()
//
//   status bit31 & ctrl bit15 -> IRQ0   (timer_a)
//   status bit29 & ctrl bit13 -> IRQ2   (timer_c / game speed)
//   status bit24 & ctrl bit 8 -> IRQ3   (SPI2, 300 kHz machine timer)
//   status bit 9 & int2 bit 9 -> IRQ6   (timer_2khz)
//   status bit 0 & int2 bit 0 -> IRQ7   (timer_2hz)
//   status bit 2 & int2 bit 2 -> IRQ7   (timer_64hz)
//
// MAME writes the last two checks as if/else-if with an else that *clears*
// IRQ7, so the 2 Hz source can never reach the CPU when 64 Hz is idle.  With
// irq7_mame_quirk == true that bug is reproduced verbatim; by default the two
// sources are OR-ed, which is what the hardware does.
void Gpce4::update_interrupts()
{
    const uint32_t s = m_interrupt_status;
    const uint16_t c = m_interrupt_ctrl;
    const uint16_t c2 = m_interrupt2_ctrl;

    set_line(UNSP_IRQ0_LINE, (s & (1u << 31)) && (c & 0x8000));
    set_line(UNSP_IRQ2_LINE, (s & (1u << 29)) && (c & 0x2000));
    set_line(UNSP_IRQ3_LINE, (s & (1u << 24)) && (c & 0x0100));
    set_line(UNSP_IRQ6_LINE, (s & (1u << 9))  && (c2 & 0x0200));

    bool irq7;
    if (irq7_mame_quirk)
        irq7 = (s & (1u << 2)) && (c2 & 0x0004);
    else
        irq7 = ((s & (1u << 0)) && (c2 & 0x0001)) ||
               ((s & (1u << 2)) && (c2 & 0x0004));
    set_line(UNSP_IRQ7_LINE, irq7);
}


// ===========================================================================
//  SPI2 (LCD) - ref/generalplus_gpce4_soc.cpp:1040-1141
// ===========================================================================
uint16_t Gpce4::spi2_ctrl_r() { return m_spi2_ctrl; }                           // :1040
void Gpce4::spi2_ctrl_w(uint16_t data)                                          // :1045
{
    m_spi2_ctrl = data;
    if (data & 0x0800) {                  // SPIRST: drop the FIFO contents
        m_spi2_tx_fill = 0;
        m_spi2_txstatus = uint16_t(m_spi2_txstatus & 0xc0f0);
    }
}

// MAME returns m_spi2_txstatus (:1030) and nothing in the SoC ever sets it, so
// the game only ever saw 0 here.  The register still carries the real FIFO
// state (bits 3:0 fill level, bits 7:4 TXFLEV, bit 14 TXIEN, bit 15 SPITXIF,
// write-one-to-clear) behind the spi2_status_regs knob.
uint16_t Gpce4::spi2_txstatus_r()                                               // :1085
{
    if (!spi2_status_regs)
        return 0x0000;                    // MAME-exact
    return uint16_t((m_spi2_txstatus & 0xc0f0) | (unsigned(m_spi2_tx_fill) & 0x0f));
}

void Gpce4::spi2_txstatus_w(uint16_t data)                                      // :1091
{
    uint16_t v = uint16_t(data & 0xc0f0);
    if (data & 0x8000)
        v = uint16_t(v & ~0x8000);        // SPITXIF clear
    m_spi2_txstatus = v;
}

// ref/generalplus_gpce4_soc.cpp:1096  spi2_txdata_w(): m_spi2_out(data & 0xff)
// and ref/generalplus_gpce4.cpp:344 spi_from_soc(): command when IOB bit 0x0400
// is clear, data when it is set.
//
// MAME models no FIFO here: the byte leaves immediately, which is what the
// 48 MHz shifter does relative to a 96 MHz CPU (2 cycles per byte).  The FIFO
// counter is kept for the status register only, so it reads back empty.
void Gpce4::spi2_txdata_w(uint16_t data)                                        // :1096
{
    const uint8_t byte = uint8_t(data & 0xff);

    if (m_spi2_tx_fill < 12)             // 12 byte TX FIFO
        m_spi2_tx_fill++;

    if (!(m_iob_out & 0x0400))
        lcd.command_w(byte);
    else
        lcd.data_w(byte);

    m_spi2_tx_fill--;                    // clocked out by the shifter
    m_spi2_txstatus = uint16_t(m_spi2_txstatus | 0x8000);   // SPITXIF
}

uint16_t Gpce4::spi2_rxstatus_r() { return 0x0000; }                            // :1052
void Gpce4::spi2_rxstatus_w(uint16_t) { }                                       // :1058

uint16_t Gpce4::spi2_rxdata_r()   { return 0x0000; }                            // :1103

// MAME returns 0 (:1054).  TFE|TNF when idle behind the spi2_status_regs knob.
uint16_t Gpce4::spi2_misc_r()                                                   // :1131
{
    if (!spi2_status_regs)
        return 0x0000;                    // MAME-exact
    return m_spi2_tx_fill ? 0x0001 : 0x0003;
}

void Gpce4::spi2_misc_w(uint16_t) { }                                           // :1141


// ===========================================================================
//  SPI1 (flash) - ref/generalplus_gpce4_soc.cpp:706-901 and :733-829
// ===========================================================================
uint16_t Gpce4::spi_ctrl_r() { return uint16_t(m_spi_ctrl & ~0x0800); }        // :706
void Gpce4::spi_ctrl_w(uint16_t data)                                          // :712
{
    m_spi_ctrl = data;
    if (data & 0x0800) {
        reset_spi_fifos();
        m_spi.reset();                    // m_spi_reset(1) -> genspi->reset()
    }
}

void Gpce4::spi_txstatus_w(uint16_t data)                                      // :727
{
    if (trace_io) std::printf("[gpce4] SPI_TXStatus = %04x\n", unsigned(data));
}

void Gpce4::spi_txdata_w(uint16_t data) { write_to_tx_fifo(uint8_t(data & 0xff)); }  // :792

uint16_t Gpce4::spi_rxstatus_r() { return 0x8000; }                            // :798
void Gpce4::spi_rxstatus_w(uint16_t) { }                                       // :804
// ref/generalplus_gpce4_soc.cpp:809  spi_rxdata_r(): pops the RX FIFO, and
// prints "rx underflow!" in MAME when it is empty.
uint16_t Gpce4::spi_rxdata_r()
{
    if (m_rx_fifo_entries == 0) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            std::printf("[gpce4] spi rx underflow\n");
        }
        return 0x0000;
    }

    const uint8_t ret = m_spi_rx_fifo[0];
    for (int i = 1; i < 8; i++)
        m_spi_rx_fifo[i - 1] = m_spi_rx_fifo[i];
    m_spi_rx_fifo[7] = 0;
    m_rx_fifo_entries--;
    return ret;
}
uint16_t Gpce4::spi_misc_r() { return 0x0004; }                                // :843
void Gpce4::spi_misc_w(uint16_t) { }                                           // :722
uint16_t Gpce4::spi_man_ctrl_r() { return 0x0000; }                            // :831
void Gpce4::spi_man_ctrl_w(uint16_t) { }                                       // :837
uint16_t Gpce4::spi_auto_ctrl_r() { return m_spi_auto_ctrl; }                  // :867
void Gpce4::spi_auto_ctrl_w(uint16_t data) { m_spi_auto_ctrl = data; }         // :873
uint16_t Gpce4::spi_bank_r() { return m_spi_bank; }                            // :895
void Gpce4::spi_bank_w(uint16_t data) { m_spi_bank = data; }                   // :901


// ===========================================================================
//  SPI FIFOs - ref/generalplus_gpce4_soc.cpp:733-829
// ===========================================================================
void Gpce4::reset_spi_fifos()
{
    std::fill(m_spi_tx_fifo, m_spi_tx_fifo + 8, uint8_t(0));
    std::fill(m_spi_rx_fifo, m_spi_rx_fifo + 8, uint8_t(0));
    m_tx_fifo_entries = 0;
    m_rx_fifo_entries = 0;
    m_spi_tx_armed = false;
    m_spi_tx_next = 0;
}

// ref/generalplus_gpce4_soc.cpp:771  write_to_tx_fifo(): arms the 48 MHz
// transmit timer on the first byte, queues while there is room.
void Gpce4::write_to_tx_fifo(uint8_t data)
{
    // The SoC shifts a byte out every 2 CPU cycles (48 MHz timer), so the 8 byte
    // FIFO can never hold more than one byte while the CPU fills it.  Our CPU runs
    // in millisecond slices and cannot interleave with that timer, which made every
    // burst of writes overflow and corrupted the flash command stream.  Transfer
    // inline instead: this is the same write(out)/read() pair timer_spi_tx() does,
    // just without the two cycle queueing delay.
    m_spi.write(data);
    recieve_spi_fifo_data(m_spi.read());
}

// ref/generalplus_gpce4_soc.cpp:733  timer_spi_tx(): shift one byte out to the
// flash device and collect its answer.
void Gpce4::timer_spi_tx()
{
    if (m_tx_fifo_entries <= 0) {
        m_spi_tx_armed = false;
        return;
    }

    const uint8_t out = m_spi_tx_fifo[0];
    for (int i = 1; i < 8; i++)
        m_spi_tx_fifo[i - 1] = m_spi_tx_fifo[i];
    m_spi_tx_fifo[7] = 0;

    m_spi.write(out);                       // ref/generalplus_gpce4.cpp spi_out()
    recieve_spi_fifo_data(m_spi.read());

    m_tx_fifo_entries--;
    if (m_tx_fifo_entries == 0)
        m_spi_tx_armed = false;
    else
        m_spi_tx_next = m_now + TICKS_SPI_TX;
}

// ref/generalplus_gpce4_soc.cpp:758  recieve_spi_fifo_data()
void Gpce4::recieve_spi_fifo_data(uint8_t data)
{
    if (m_rx_fifo_entries < 8) {
        m_spi_rx_fifo[m_rx_fifo_entries++] = data;
    } else {
        std::printf("[gpce4] SPI RX FIFO overflow, byte %02x dropped\n", unsigned(data));
    }
}


// ===========================================================================
//  cycles_to_next_event() - how far the CPU may run before a timer event
//
//  tick() services the wheel in absolute time, but it can only be called from
//  between instructions.  Running the CPU for a fixed 2000 cycle slice makes
//  every event late by up to a slice, which shows up as an IRQ phase offset of
//  ~85 instructions after the first second (the 2 kHz handler nearly fills its
//  0.5 ms period, so the two streams then never re-align).  The main loop asks
//  for the distance to the next event instead and runs exactly that far.
//
//  The 300 kHz driver source (SRC_SPI2, array index 5) is deliberately
//  excluded: it only tags INT status bit 8 / IRQ3, digicolr never enables it,
//  and stepping at 320 cycle granularity would multiply the slice count by 15
//  for no observable effect on the instruction stream.
// ===========================================================================
uint32_t Gpce4::cycles_to_next_event(uint32_t cap) const
{
    uint64_t best = ~uint64_t(0);

    for (int i = 0; i < SRC_COUNT; i++)
    {
        if (i == 5)                             // 300 kHz driver source
            continue;
        if (!source_enabled(i))
            continue;
        if (m_timers[i].next < best)
            best = m_timers[i].next;
    }

    if (m_spi_tx_armed && m_spi_tx_next < best)
        best = m_spi_tx_next;

    if (best <= m_now)
        return 1;                               // event due right now

    const uint64_t delta = best - m_now;
    return delta > cap ? cap : uint32_t(delta);
}


// ===========================================================================
//  tick() - the cycle driven timer wheel
//
//  Six periodic sources plus the SPI1 transmit one-shot.  Events are serviced
//  in time order (ties broken by source index), exactly like MAME's scheduler,
//  so a byte written to the flash is clocked out at its scheduled cycle rather
//  than at the end of the slice.
// ===========================================================================
void Gpce4::tick(uint32_t cycles)
{
    if (cycles == 0)
        return;

    const uint64_t target = m_now + cycles;
    // Fast path: collapse every periodic timer straight to its last occurrence
    // at or before `target` instead of stepping through each one.
    //
    // The old loop walked the timer wheel event by event, which is what MAME
    // does because MAME has no choice: its scheduler is shared by every device.
    // Here it costs the frame real time and buys nothing.  A 300 kHz source
    // fires 5000 times in a single 16.7 ms guest frame; after the idle loop
    // collapse (the guest spins for millions of cycles waiting for a tick) one
    // tick() call can span 100 M cycles and step the wheel 300000 times.  The
    // worst single tick() measured on the device was 30 ms - a third of a
    // stalled frame - and all of it was spent re-arming timers whose only
    // observable effect is one bit in m_interrupt_status.
    //
    // Collapsing is exact for that bit: an interrupt status bit cannot record
    // how many times it was raised.  It is *not* exact for a guest that clears
    // the bit between two edges and expects to see every edge, so the slow path
    // is kept below for the sources that can matter: SPI TX carries an ordered
    // byte stream and still has to be stepped one byte at a time, and anything
    // that arrives with a period shorter than the coalescing window is left on
    // the old path.
    constexpr uint64_t kCoalesceMin = 4;
    bool any = false;
    for (int i = 0; i < SRC_COUNT; i++) {
        if (i == 5 && !timer_300k_enable)          // machine driver "timer"
            continue;
        if (!source_enabled(i))
            continue;
        TimerSource &t = m_timers[i];
        if (t.next > target)
            continue;
        if (t.period == 0 || t.period >= kCoalesceMin * 2) {
            const uint64_t n = (target - t.next) / t.period + 1;
            if (n >= kCoalesceMin) {
                t.next += n * t.period;
                if (!(m_interrupt_status & (1u << t.irq)))
                    g_src_set[t.irq]++;
                m_interrupt_status |= (1u << t.irq);
                any = true;
                continue;
            }
        }
        // Few edges (or a very short period): step them in order.
        while (t.next <= target) {
            m_now = t.next;
            t.next += t.period;
            request_interrupt(t.irq);
        }
        any = true;
    }

    if (m_spi_tx_armed) {
        while (m_spi_tx_armed && m_spi_tx_next <= target) {
            m_now = m_spi_tx_next;
            timer_spi_tx();
        }
    }

    m_now = target;
#ifdef ESP_PLATFORM
    {
        static uint64_t next_log = 0;
        if (m_now >= next_log) {
            next_log = m_now + 40000000ull;
            std::printf("[tick] now=%llu any=%d stat=%08x p64=%llu n64=%llu en=%d\n",
                        (unsigned long long)m_now, any ? 1 : 0,
                        (unsigned)m_interrupt_status,
                        (unsigned long long)m_timers[3].period,
                        (unsigned long long)m_timers[3].next,
                        source_enabled(3) ? 1 : 0);
        }
    }
#endif
    if (any)
        update_interrupts();
}
