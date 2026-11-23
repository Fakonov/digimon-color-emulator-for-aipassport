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
#include "audio_dump.h"     // WAV capture for the headless tools (docs/AUDIO.md)

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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

// --- temporary diagnosis ---------------------------------------------------
// GPCE4_BUSLOG=1 prints every bus access made from the ROM's idle/sleep loop so
// it is possible to see what that loop is actually waiting on.  Off by default;
// the checks are two loads and a compare on the hot path.
bool buslog_on()
{
    static int on = -1;
    if (on < 0) on = std::getenv("GPCE4_BUSLOG") ? 1 : 0;
    return on != 0;
}
int buslog_left = 400;
bool buslog_pc(uint32_t pc)
{
    return (pc >= 0x5240 && pc <= 0x5262) || (pc >= 0x5370 && pc <= 0x5382);
}

} // anonymous namespace


// ===========================================================================
//  Gpce4SpiFlash - port of MAME's generic_spi_flash_device
// ===========================================================================
void Gpce4SpiFlash::set_image(uint8_t *rom, size_t size)
{
    m_rom = rom;
    m_size = size;
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
    if (!m_rom || m_size == 0)
        return 0xff;
    if ((m_size & (m_size - 1)) == 0)          // power of two: MAME's mask
        return m_rom[addr & uint32_t(m_size - 1)];
    return m_rom[addr % m_size];
}

// generic_spi_flash.cpp: write(u8 data) - the only entry point; one byte per
// clock edge, driven by the SoC's transmit timer.
void Gpce4SpiFlash::write(uint8_t data)
{
    switch (m_state) {
    case READY_FOR_COMMAND:
        switch (data) {
        case 0x01: m_state = COMMAND_01_WRSR;        m_state_step = 0; break;
        case 0x02: m_state = COMMAND_02_PP;          m_state_step = 0; break;
        case 0x03: m_state = COMMAND_03_READ;        m_state_step = 0; break;
        case 0x04: m_statusreg = uint8_t(m_statusreg & ~0x02); break;   // WRDI
        case 0x05: m_state = COMMAND_05_RDSR;        m_state_step = 0; break;
        case 0x06: m_statusreg = uint8_t(m_statusreg | 0x02); break;    // WREN
        case 0x0b: m_state = COMMAND_0B_FAST_READ;   m_state_step = 0; break;
        case 0x11: m_state = COMMAND_11_UNKNOWN;     m_state_step = 0; break;
        case 0x15: m_state = COMMAND_15_RDCR;        m_state_step = 0; break;
        case 0x20: m_state = COMMAND_20_SE;          m_state_step = 0; break;
        case 0x31: m_state = COMMAND_31_UNKNOWN;     m_state_step = 0; break;
        case 0x35: m_state = COMMAND_35_RDSR2;       m_state_step = 0; break;
        case 0x66: break;                                            // enable reset
        case 0x90: m_state = COMMAND_90_REMS;        m_state_step = 0; break;
        case 0x99: break;                                            // reset
        case 0x9f: m_state = COMMAND_9F_RDID;        m_state_step = 0; break;
        case 0xab: break;                                            // release DP
        case 0xb9: break;                                            // deep power down
        case 0xeb: m_state = COMMAND_EB_4READ;       m_state_step = 0; break;
        case 0xec: m_state = COMMAND_EC_UNKNOWN;     m_state_step = 0; break;
        case 0xff: break;                                            // CRMR
        default:
            // MAME: fatalerror("SPI set to unknown/unhandled command %02x")
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
            if (writable && m_rom && m_addr < m_size) {
                // A NOR page program can only clear bits.
                m_rom[m_addr] &= data;
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
                const uint32_t base = m_addr & ~uint32_t(0xfffu);
                const uint32_t end = (uint32_t)m_size < base + 0x1000u ? (uint32_t)m_size : base + 0x1000u;
                for (uint32_t a = base; a < end; a++) m_rom[a] = 0xff;
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

// The WAV file (GPCE4_WAV) and the register statistics (GPCE4_AUDIO_STATS) are
// finished here because main() owns its Gpce4 on the stack: by the time an
// atexit handler would run, the object is already gone.  See docs/AUDIO.md.
Gpce4::~Gpce4()
{
    // Statistics first: they report whether a WAV was open, so closing it before
    // the dump would make every run look as if it never recorded anything.
    if (m_audio_stats_on)
        audio_stats_dump(m_audio_stats_path);
    if (m_audio_wav) {
        audio_wav_close();
        m_audio_wav = false;
    }
}

void Gpce4::set_images(const uint8_t *irom, size_t irom_size, uint8_t *flash, size_t flash_size)
{
    m_irom = irom;
    m_irom_size = irom_size;
    m_flash = flash;
    m_flash_size = flash_size;
    m_spi.set_image(flash, flash_size);
}

void Gpce4::set_cpu(unsp_device *cpu)
{
    m_cpu = cpu;
    for (int i = 0; i < 9; i++) m_irq_line[i] = false;   // lines start released
    // Hand the CPU a flat view of RAM so its hot path skips the bus vtable.
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

    audio_reset();      // after m_now = 0: frame edges are aligned to cycle 0
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
    if (base + 1 >= m_flash_size) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            std::printf("[gpce4] flash window read past end (%08x, bank %d) -> ffff\n",
                        unsigned(base), int(m_spi_bank));
        }
        return 0xffff;
    }
    return uint16_t(m_flash[base] | (m_flash[base + 1] << 8));
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

// Opcode fetch fast path: hand the CPU a host pointer for the 2 KiB (1024 word)
// page holding `addr`, so the interpreter fetches with one compare and one array
// read instead of a virtual call plus the full decode chain.  IO space and
// unmapped holes return nullptr and keep the slow path.
const uint16_t *Gpce4::fetch_window(uint32_t addr, uint32_t *base, uint32_t *mask)
{
    constexpr uint32_t WINDOW_MASK = 0x3ff;                // 1024 words = 2 KiB
    addr &= ADDR_MASK;

    if (addr < 0x001000) {                                 // word-addressed RAM
        *base = addr & ~WINDOW_MASK;
        *mask = WINDOW_MASK;
        return ram + *base;
    }

    if (addr >= 0x004000 && addr <= 0x00bfff && m_irom) {  // 64 KB internal mask ROM
        *base = addr & ~WINDOW_MASK;
        *mask = WINDOW_MASK;
        return reinterpret_cast<const uint16_t *>(m_irom + (*base - 0x004000) * 2);
    }

    if (addr >= 0x200000 && addr <= 0x3fffff && m_flash) { // SPI flash direct window
        const uint32_t off = addr - 0x200000;
        const uint32_t wbase = off & ~WINDOW_MASK;
        if ((wbase + WINDOW_MASK + 1) * 2 <= m_flash_size) {
            *base = addr & ~WINDOW_MASK;
            *mask = WINDOW_MASK;
            return reinterpret_cast<const uint16_t *>(m_flash + wbase * 2);
        }
    }

    *base = 0xffffffffu;
    *mask = 0;
    return nullptr;
}

uint16_t Gpce4::read16(uint32_t addr)
{
    addr &= ADDR_MASK;

    if (buslog_on() && buslog_left > 0 && m_cpu)
    {
        const uint32_t pc = m_cpu->m_core->m_r[7] & 0xffffffu;
        if (buslog_pc(pc))
        {
            std::printf("[bus] r pc=%06X addr=%06X\n", unsigned(pc), unsigned(addr));
            --buslog_left;
        }
    }

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

void Gpce4::write16(uint32_t addr, uint16_t data)
{
    addr &= ADDR_MASK;

    if (buslog_on() && buslog_left > 0 && m_cpu)
    {
        const uint32_t pc = m_cpu->m_core->m_r[7] & 0xffffffu;
        if (buslog_pc(pc))
        {
            std::printf("[bus] w pc=%06X addr=%06X = %04X\n", unsigned(pc), unsigned(addr), unsigned(data));
            --buslog_left;
        }
    }

    if (addr < 0x001000) {
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
    // Proof-of-silence counter (docs/AUDIO.md §1): one increment per IO write,
    // so the report can show every offset the game touched in a run - the DAC
    // block included, at zero.  Off by default; costs nothing unless
    // GPCE4_AUDIO_STATS is requested.
    if (m_audio_stats_on)
    {
        m_io_hist[ofs & 0xff]++;
        m_io_last[ofs & 0xff] = data;
    }

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
// ===========================================================================
static unsigned long g_ioa_reads = 0;
uint16_t Gpce4::ioa_data_r()     { ++g_ioa_reads; return m_port_in[0]; }
unsigned long gpce4_ioa_reads()  { return g_ioa_reads; }                       // :296
void     Gpce4::ioa_data_w(uint16_t) { /* m_porta_out(data): nothing wired */ } // :302

uint16_t Gpce4::ioa_buffer_r()   { return m_port_in[0]; }                       // :308
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
void Gpce4::pwm0_ctrl_w(uint16_t) { }                                           // :529
void Gpce4::pwm1_ctrl_w(uint16_t) { }                                           // :534
void Gpce4::pwm2_ctrl_w(uint16_t) { }                                           // :539
void Gpce4::pwm3_ctrl_w(uint16_t) { }                                           // :544


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
    m_asleep = true;
    // On hardware this write stalls the CPU clock; execution resumes at the
    // instruction after it when a wake-up condition appears.  Leaving the core
    // running turns the ROM's wait loop into an endless spin that never reads
    // the key ladder again, so the halt has to be modelled.
    m_halted = true;
    if (m_cpu)
        std::printf("[sleep] halt at pc=%06X sr=%04X sp=%04X data=%04X\n",
                    unsigned(m_cpu->m_core->m_r[7] & 0xffffffu),
                    unsigned(m_cpu->m_core->m_r[6] & 0xffffu),
                    unsigned(m_cpu->m_core->m_r[0] & 0xffffu), unsigned(data));
    if (trace_io) std::printf("[gpce4] System_Sleep = %04x (asleep)\n", unsigned(data));
}

// Release the System_Sleep stall.  The ROM parks itself inside a two-instruction
// wait loop that writes 0x3036 over and over and never looks at the key ladder,
// so simply resuming at the following instruction lands right back in the same
// loop.  On the handset the wake-up interrupt retires the loop, which means the
// only faithful-enough model is to step over it: land on the first instruction
// after the loop, which is where the ROM continues its idle-housekeeping.
void Gpce4::wake_from_sleep()
{
    m_asleep = false;
    if (!m_halted)
        return;
    m_halted = false;
    if (!m_cpu)
        return;

    const uint32_t pc = ((m_cpu->m_core->m_r[6] & 0x3fu) << 16) | (m_cpu->m_core->m_r[7] & 0xffffu);
    uint32_t npc = 0;
    if (pc >= 0x5251 && pc <= 0x525a) npc = 0x525b;
    else if (pc >= 0x5376 && pc <= 0x537f) npc = 0x5380;
    if (!npc)
        return;
    m_cpu->m_core->m_r[7] = npc & 0xffffu;
    m_cpu->m_core->m_r[6] = (m_cpu->m_core->m_r[6] & ~0x3fu) | ((npc >> 16) & 0x3fu);
    std::printf("[wake] skip wait loop %06X -> %06X\n", unsigned(pc), unsigned(npc));
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
    m_dac_ctrl = data;
    audio_stat_note(0, data);
    if (trace_io) std::printf("[gpce4] DAC_Ctrl = %04x\n", unsigned(data));
}

// MAME forwards this to the DAC device (m_dac->data_w(data), a
// DAC_16BIT_R2R_TWOS_COMPLEMENT: the value *is* the sample, sign included) and
// then to the speaker, which the machine never mixes.  Here it also drives the
// audio model: the code is latched and held until the next write (see
// audio_advance()).
void Gpce4::dac_cha1_data_w(uint16_t data)                             // :613
{
    audio_stat_note(1, data);
    m_dac_cha1 = data;
    audio_sink_code(data);
}

void Gpce4::dac_cha2_data_w(uint16_t data)                             // :620
{
    m_dac_cha2 = data;
    audio_stat_note(2, data);
    if (trace_io) std::printf("[gpce4] DAC_CH2_Data = %04x\n", unsigned(data));
}

void Gpce4::ppam_ctrl_w(uint16_t data)                                 // :625
{
    m_ppam_ctrl = data;
    audio_stat_note(3, data);
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

void Gpce4::request_interrupt(int which)                                        // :964
{
    if (which < 0 || which >= 32)
        return;
    m_interrupt_status |= (1u << which);
    update_interrupts();
}

void Gpce4::clear_interrupt(int which)                                          // :971
{
    if (which < 0 || which >= 32)
        return;
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

    const uint64_t audio_from = m_now;      // DAC code is constant during a tick

    const uint64_t target = m_now + cycles;
    uint32_t processed = 0;

    for (;;) {
        int which = -1;
        uint64_t best = ~uint64_t(0);

        for (int i = 0; i < SRC_COUNT; i++) {
            if (i == 5 && !timer_300k_enable)      // machine driver "timer"
                continue;
            if (!source_enabled(i))
                continue;
            if (m_timers[i].next < best) {
                best = m_timers[i].next;
                which = i;
            }
        }
        if (m_spi_tx_armed && m_spi_tx_next < best) {
            best = m_spi_tx_next;
            which = SRC_SPI_TX;
        }

        if (which < 0 || best > target)
            break;

        m_now = best;

        if (which == SRC_SPI_TX) {
            timer_spi_tx();
        } else {
            m_timers[which].next = best + m_timers[which].period;
            request_interrupt(m_timers[which].irq);
        }

        // Guard against a pathological slice (e.g. tick(96'000'000) from a
        // harness that never steps the CPU): drop the events that piled up.
        if (++processed > 4000000u) {
            std::printf("[gpce4] tick(): event budget hit, timers resynchronised\n");
            for (int i = 0; i < SRC_COUNT; i++)
                if (m_timers[i].next <= target)
                    m_timers[i].next = target + m_timers[i].period;
            break;
        }
    }

    m_now = target;
    audio_advance(audio_from, target);       // emit the frame edges we just crossed
    update_interrupts();
}


// ===========================================================================
//  AUDIO - DAC / PPAM model  (docs/AUDIO.md)
//
//  What MAME does: 0x3040 DAC_Ctrl and 0x3043 PPAMCtrl are log-only;
//  0x3041 DAC_CH1_Data calls m_dac->data_w() on a 16-bit R-2R twos-complement
//  DAC and the result goes to a speaker that digicolr never adds to the mixer.
//  So upstream is silent by construction, and so is every port of it.
//
//  What this section adds, with no changes to the machine wiring:
//    * the 0x3041 code is latched and held (zero order hold, like the DAC);
//    * tick() integrates that held code over the cycles it just advanced and
//      emits one 48 kHz frame per 2000 cycles (96 MHz / 48 kHz = 2000 exactly);
//    * frames land in a 65536-frame ring that the host drains with audio_read().
//  The cost is one multiply-add per tick() call plus one division per emitted
//  frame (~48k/s), independent of how often the game writes the DAC.
// ===========================================================================
void Gpce4::audio_reset()
{
    if (m_audio_ring.size() != AUDIO_RING_FRAMES)
        m_audio_ring.assign(AUDIO_RING_FRAMES, 0);
    else
        std::fill(m_audio_ring.begin(), m_audio_ring.end(), int16_t(0));

    m_audio_wr = 0;
    m_audio_rd = 0;
    m_audio_next = AUDIO_CYCLES_PER_FRM;
    m_audio_acc = 0;
    m_audio_acc_cycles = 0;
    m_audio_level = 0;
    m_audio_frames = 0;
    m_audio_overruns = 0;
    m_audio_underruns = 0;
    m_audio_peak = 0;
    m_audio_sumsq = 0;
    m_audio_nonzero = 0;
    std::memset(m_audio_sec_writes, 0, sizeof(m_audio_sec_writes));
    std::memset(m_io_hist, 0, sizeof(m_io_hist));
    std::memset(m_io_last, 0, sizeof(m_io_last));
    m_audio_last_write_cycle = 0;
    m_dac_ctrl = 0;
    m_ppam_ctrl = 0;
    m_dac_cha2 = 0;

    static const char *const names[AUDIO_REG_COUNT] = {
        "0x3040 DAC_Ctrl", "0x3041 DAC_CH1_Data", "0x3042 DAC_CH2_Data", "0x3043 PPAMCtrl"
    };
    for (int i = 0; i < AUDIO_REG_COUNT; i++) {
        m_audio_stat[i] = AudioRegStats{};
        m_audio_stat[i].name = names[i];
    }

    // Capture is opt-in through the environment: the normal headless runs must
    // not touch the disk or print anything extra.
    const char *sp = std::getenv("GPCE4_AUDIO_STATS");
    m_audio_stats_on = (sp != nullptr && *sp != '\0');
    if (m_audio_stats_on)
        std::snprintf(m_audio_stats_path, sizeof(m_audio_stats_path), "%s", sp);
    else
        m_audio_stats_path[0] = '\0';

    const char *wp = std::getenv("GPCE4_WAV");
    if (wp != nullptr && *wp != '\0') {
        m_audio_wav = audio_wav_open(wp, AUDIO_RATE);
    } else {
        if (m_audio_wav) audio_wav_close();   // reset() after an earlier open
        m_audio_wav = false;
    }

    // A/B switch for the throughput measurements in docs/AUDIO.md §4.
    const char *off = std::getenv("GPCE4_AUDIO_OFF");
    audio_enabled = !(off != nullptr && *off != '\0' && *off != '0');
}

// 0x3041 write: DAC_16BIT_R2R_TWOS_COMPLEMENT, so the raw code is the sample.
void Gpce4::audio_sink_code(uint16_t data)
{
    m_audio_level = int32_t(int16_t(data));
    const unsigned sec = unsigned(m_now / CPU_CLOCK);       // 1 s activity buckets
    if (sec < 64) m_audio_sec_writes[sec]++;
}

void Gpce4::audio_push(int16_t frame)
{
    if (m_audio_wr - m_audio_rd >= AUDIO_RING_FRAMES) {   // producer outran the reader
        m_audio_rd++;
        m_audio_overruns++;
    }
    m_audio_ring[m_audio_wr & (AUDIO_RING_FRAMES - 1)] = frame;
    m_audio_wr++;
    m_audio_frames++;

    const uint32_t mag = uint32_t(frame < 0 ? -int32_t(frame) : int32_t(frame));
    if (mag > m_audio_peak) m_audio_peak = mag;
    m_audio_sumsq += uint64_t(mag) * uint64_t(mag);
    if (mag != 0) m_audio_nonzero++;

    if (m_audio_wav) audio_wav_write(&frame, 1);
}

// Integrate the held code over [from, to) and emit every 2000-cycle frame edge.
void Gpce4::audio_advance(uint64_t from, uint64_t to)
{
    if (!audio_enabled || m_audio_ring.empty())
    {
        // Keep the frame phase glued to the clock so re-enabling does not
        // replay a burst of stale frames.
        m_audio_acc = 0;
        m_audio_acc_cycles = 0;
        m_audio_next = ((to / AUDIO_CYCLES_PER_FRM) + 1) * AUDIO_CYCLES_PER_FRM;
        return;
    }

    if (m_audio_next <= from)            // never expected; keeps the loop monotonic
        m_audio_next = ((from / AUDIO_CYCLES_PER_FRM) + 1) * AUDIO_CYCLES_PER_FRM;

    while (from < to)
    {
        const uint64_t stop = (to < m_audio_next) ? to : m_audio_next;
        const uint32_t span = uint32_t(stop - from);
        m_audio_acc += m_audio_level * int32_t(span);
        m_audio_acc_cycles += span;
        from = stop;

        if (from >= m_audio_next)
        {
            int32_t v = m_audio_acc_cycles
                      ? m_audio_acc / int32_t(m_audio_acc_cycles)
                      : m_audio_level;
            if (v > 32767)  v = 32767;
            if (v < -32768) v = -32768;
            audio_push(int16_t(v));
            m_audio_acc = 0;
            m_audio_acc_cycles = 0;
            m_audio_next += AUDIO_CYCLES_PER_FRM;
        }
    }
}

size_t Gpce4::audio_read(int16_t *dst, size_t frames)
{
    if (!dst || frames == 0 || m_audio_ring.empty())
        return 0;

    const size_t avail = size_t(m_audio_wr - m_audio_rd);
    const size_t n = (frames < avail) ? frames : avail;
    for (size_t i = 0; i < n; i++)
        dst[i] = m_audio_ring[m_audio_rd++ & (AUDIO_RING_FRAMES - 1)];

    if (n < frames) {                    // starved: pad with silence
        std::memset(dst + n, 0, (frames - n) * sizeof(int16_t));
        m_audio_underruns += uint64_t(frames - n);
    }
    return n;
}

// A few integer ops on the write path; unconditional so a run without the
// statistics file still reports the same numbers as one with it.
void Gpce4::audio_stat_note(int idx, uint16_t data)
{
    if (idx < 0 || idx >= AUDIO_REG_COUNT)
        return;

    AudioRegStats &s = m_audio_stat[idx];
    s.writes++;
    if (data < s.min_value) s.min_value = data;
    if (data > s.max_value) s.max_value = data;

    for (int i = 0; i < 8; i++) {
        if (s.hist[i] && s.hist_value[i] == data) { s.hist[i]++; return; }
        if (!s.hist[i]) { s.hist_value[i] = data; s.hist[i] = 1; return; }
    }
    // All eight slots taken: the value is outside the reported top eight.

    if (idx != 1)
        return;

    if (s.writes > 1) {
        const uint64_t dt = m_now - m_audio_last_write_cycle;
        s.deltas++;
        s.delta_sum += dt;
        if (dt < s.delta_min) s.delta_min = dt;
        if (dt > s.delta_max) s.delta_max = dt;
        uint64_t bucket = dt / 480;                   // 480 cycles = 10 us
        if (bucket > 33) bucket = 33;
        s.delta_hist[bucket]++;
    }
    m_audio_last_write_cycle = m_now;
}

static const char *audio_io_name(uint16_t ofs)
{
    switch (ofs) {
    case 0x00: return "IOA_Data";        case 0x01: return "IOA_Buffer";
    case 0x02: return "IOA_Direction";   case 0x03: return "IOA_Attribute";
    case 0x04: return "IOB_Data";        case 0x05: return "IOB_Buffer";
    case 0x06: return "IOB_Direction";   case 0x07: return "IOB_Attribute";
    case 0x08: return "IOC_Data";        case 0x09: return "IOC_Buffer";
    case 0x0a: return "IOC_Direction";   case 0x0b: return "IOC_Attribute";
    case 0x0f: return "IO_Ctrl";
    case 0x10: return "TimerA_Data";     case 0x11: return "TimerA_Counter";
    case 0x12: return "TimerB_Data";     case 0x13: return "TimerB_Counter";
    case 0x14: return "TimerC_Data";     case 0x15: return "TimerC_Counter";
    case 0x16: return "Timer_Ctrl";
    case 0x20: return "PWM0_Ctrl";       case 0x21: return "PWM1_Ctrl";
    case 0x22: return "PWM2_Ctrl";       case 0x23: return "PWM3_Ctrl";
    case 0x30: return "System_Clock";    case 0x33: return "Timebase_Clear";
    case 0x34: return "Watchdog_Clear";  case 0x35: return "Wait_Ctrl";
    case 0x39: return "Cache_Ctrl";
    case 0x40: return "DAC_Ctrl";        case 0x41: return "DAC_CH1_Data";
    case 0x42: return "DAC_CH2_Data";    case 0x43: return "PPAMCtrl";
    case 0x50: return "INT_Ctrl";        case 0x51: return "INT_Status";
    case 0x52: return "FIQ_Sel";         case 0x53: return "INT2_Ctrl";
    case 0x54: return "INT2_Status";     case 0x55: return "FIQ2_Sel";
    case 0x70: return "ADC_Ctrl";        case 0x71: return "ADC_Data";
    case 0x72: return "ADC_LineIn";      case 0x90: return "SPI2_Ctrl";
    case 0x91: return "SPI2_TXStatus";   case 0x92: return "SPI2_TXData";
    case 0x93: return "SPI2_RXStatus";   case 0x95: return "SPI2_MISC";
    case 0xe0: return "SPI_Ctrl";        case 0xe1: return "SPI_TXStatus";
    case 0xe2: return "SPI_TXData";      case 0xe3: return "SPI_RXStatus";
    case 0xe5: return "SPI_MISC";        case 0xe6: return "SPI_Man_Ctrl";
    case 0xe7: return "SPI_Auto_Ctrl";   case 0xe9: return "SPI_Bank";
    default:   return "(unmapped/logged)";
    }
}

void Gpce4::audio_stats_dump(const char *path)
{
    if (!path || !*path)
        return;

    // "1" / "-" mean stdout (handy when there is no writable directory).
    const bool to_stdout = (std::strcmp(path, "1") == 0 || std::strcmp(path, "-") == 0);
    FILE *out = to_stdout ? stdout : std::fopen(path, "wb");
    if (!out)
        out = stdout;

    const double secs = double(m_now) / double(CPU_CLOCK);
    std::fprintf(out, "# GPCE4 DAC / PPAM write statistics (docs/AUDIO.md)\n");
    std::fprintf(out, "emulated_cycles = %llu\n", (unsigned long long)m_now);
    std::fprintf(out, "emulated_seconds = %.3f\n\n", secs);

    for (int i = 0; i < AUDIO_REG_COUNT; i++) {
        const AudioRegStats &s = m_audio_stat[i];
        std::fprintf(out, "%s\n", s.name);
        std::fprintf(out, "  writes      = %llu\n", (unsigned long long)s.writes);
        if (secs > 0.0)
            std::fprintf(out, "  writes_per_s = %.1f\n", double(s.writes) / secs);
        if (s.writes) {
            std::fprintf(out, "  value_range = 0x%04x..0x%04x (%u..%u)\n",
                         unsigned(s.min_value), unsigned(s.max_value),
                         unsigned(s.min_value), unsigned(s.max_value));
            std::fprintf(out, "  top_values  = ");
            for (int k = 0; k < 8 && s.hist[k]; k++)
                std::fprintf(out, "0x%04x x%llu%s", unsigned(s.hist_value[k]),
                             (unsigned long long)s.hist[k], (k < 7 && s.hist[k + 1]) ? ", " : "");
            std::fprintf(out, "\n");
        }
        std::fprintf(out, "\n");
    }

    const AudioRegStats &d = m_audio_stat[1];
    std::fprintf(out, "0x3041 inter-write spacing (CPU cycles, 96 MHz)\n");
    if (d.deltas) {
        std::fprintf(out, "  samples     = %llu\n", (unsigned long long)d.deltas);
        std::fprintf(out, "  mean        = %.1f cycles (%.2f us, %.1f Hz)\n",
                     double(d.delta_sum) / double(d.deltas),
                     double(d.delta_sum) / double(d.deltas) / 96.0,
                     96000000.0 * double(d.deltas) / double(d.delta_sum));
        std::fprintf(out, "  min / max   = %llu / %llu cycles (%.2f / %.2f us)\n",
                     (unsigned long long)d.delta_min, (unsigned long long)d.delta_max,
                     double(d.delta_min) / 96.0, double(d.delta_max) / 96.0);
        std::fprintf(out, "  histogram   (bucket = 480 cycles = 10 us; 20 kHz timer_a = 4800 cycles = bucket 10)\n");
        for (int b = 0; b < 34; b++) {
            if (!d.delta_hist[b]) continue;
            if (b == 33)
                std::fprintf(out, "    >= 33      : %llu\n", (unsigned long long)d.delta_hist[b]);
            else
                std::fprintf(out, "    %2d0-%2d0 us : %llu\n", b, b + 1,
                             (unsigned long long)d.delta_hist[b]);
        }
    } else {
        std::fprintf(out, "  samples     = 0 (fewer than two writes)\n");
    }
    std::fprintf(out, "\n");

    std::fprintf(out, "0x3041 writes per emulated second\n");
    int last = 0;
    for (int s = 0; s < 64; s++) if (m_audio_sec_writes[s]) last = s + 1;
    for (int s = 0; s < last; s++)
        std::fprintf(out, "  t=%2d s : %u\n", s, m_audio_sec_writes[s]);
    if (!last) std::fprintf(out, "  (none)\n");
    std::fprintf(out, "\n");

    // Every IO write the game made, so "the DAC is never touched" can be read
    // straight off the profile instead of being taken on trust.
    {
        int order[256];
        int n = 0;
        uint64_t total = 0;
        for (int i = 0; i < 256; i++) {
            if (!m_io_hist[i]) continue;
            order[n++] = i;
            total += m_io_hist[i];
        }
        for (int i = 1; i < n; i++) {                 // insertion sort, n <= 256
            const int v = order[i];
            int j = i - 1;
            while (j >= 0 && m_io_hist[order[j]] < m_io_hist[v]) { order[j + 1] = order[j]; j--; }
            order[j + 1] = v;
        }
        std::fprintf(out, "IO writes by offset (0x3000+ofs), %llu total, %d distinct\n",
                     (unsigned long long)total, n);
        std::fprintf(out, "  ofs  register               writes    last value\n");
        for (int i = 0; i < n && i < 48; i++) {
            const int ofs = order[i];
            const char *nm = audio_io_name(uint16_t(ofs));
            std::fprintf(out, "  %04x %-22s x%-8u %s%04x\n", unsigned(ofs), nm,
                         unsigned(m_io_hist[ofs]),
                         (ofs >= 0x40 && ofs <= 0x43) ? "<- " : "   ",
                         unsigned(m_io_last[ofs]));
        }
        if (n > 48) std::fprintf(out, "  ... %d more\n", n - 48);
        std::fprintf(out, "\n");
    }

    std::fprintf(out, "interrupt state at end of run\n");
    std::fprintf(out, "  INT_Ctrl    = %04x (bit15 timer_a/20 kHz %s, bit13 timer_c %s)\n",
                 unsigned(m_interrupt_ctrl),
                 (m_interrupt_ctrl & 0x8000) ? "ENABLED" : "masked",
                 (m_interrupt_ctrl & 0x2000) ? "ENABLED" : "masked");
    std::fprintf(out, "  INT2_Ctrl   = %04x\n", unsigned(m_interrupt2_ctrl));
    std::fprintf(out, "  INT_Status  = %08x\n", unsigned(m_interrupt_status));
    std::fprintf(out, "  unmapped writes = %d distinct address(es)\n", m_unmapped_write_count);
    std::fprintf(out, "\n");

    const double rms = m_audio_frames ? std::sqrt(double(m_audio_sumsq) / double(m_audio_frames)) : 0.0;
    std::fprintf(out, "audio model\n");
    std::fprintf(out, "  frames      = %llu (%.3f s at 48 kHz)\n",
                 (unsigned long long)m_audio_frames, double(m_audio_frames) / 48000.0);
    std::fprintf(out, "  nonzero     = %llu (%.1f%% of frames)\n",
                 (unsigned long long)m_audio_nonzero,
                 m_audio_frames ? 100.0 * double(m_audio_nonzero) / double(m_audio_frames) : 0.0);
    std::fprintf(out, "  peak        = %llu (%.2f%% of full scale)\n",
                 (unsigned long long)m_audio_peak, double(m_audio_peak) * 100.0 / 32768.0);
    std::fprintf(out, "  rms         = %.1f (%.2f%% of full scale)\n",
                 rms, rms * 100.0 / 32768.0);
    std::fprintf(out, "  overruns    = %llu, underruns = %llu\n",
                 (unsigned long long)m_audio_overruns, (unsigned long long)m_audio_underruns);
    std::fprintf(out, "  wav_open    = %s\n", m_audio_wav ? "yes" : "no");

    if (out != stdout) std::fclose(out);
}
