// ---------------------------------------------------------------------------
//  gpce4_selftest.cpp - behavioural selftest for the GPCE4 SoC / ST7735 port
//
//  Build (from the repository root):
//    g++ -std=c++20 -O2 -I emu/compat -I emu -o emu/gpce4_selftest.exe
//        emu/gpce4_selftest.cpp emu/gpce4.cpp emu/st7735.cpp
//        emu/obj/isa_*.o emu/obj/unsp_glue.o
//
//  Every assertion prints PASS/FAIL; the process exit code is the number of
//  failures (0 = everything held).
// ---------------------------------------------------------------------------

#include "gpce4.h"
#include "st7735.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_pass = 0;
int g_fail = 0;

std::string fmt(const char *f, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    return std::string(buf);
}

void check(bool ok, const std::string &what)
{
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (ok) g_pass++;
    else    g_fail++;
}

bool load_rom(const char *name, std::vector<uint8_t> &out)
{
    const char *dirs[] = { "roms/", "../roms/", "../../roms/" };
    for (const char *dir : dirs) {
        const std::string path = std::string(dir) + name;
        FILE *f = std::fopen(path.c_str(), "rb");
        if (!f)
            continue;
        std::fseek(f, 0, SEEK_END);
        const long n = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (n > 0) {
            out.resize(size_t(n));
            const size_t got = std::fread(out.data(), 1, out.size(), f);
            std::fclose(f);
            if (got == out.size()) {
                std::printf("[selftest] loaded %s (%ld bytes)\n", path.c_str(), n);
                return true;
            }
        } else {
            std::fclose(f);
        }
    }
    return false;
}

// One SPI1 byte: write it, let the 48 MHz shifter clock it out (2 CPU cycles)
// and return whatever the flash device answered.
uint8_t spi_xfer(Gpce4 &soc, uint8_t byte)
{
    soc.write16(0x30e2, byte);
    soc.tick(2);
    return uint8_t(soc.read16(0x30e4) & 0xff);
}

void test_internal_rom(const std::vector<uint8_t> &irom)
{
    Gpce4 soc;
    soc.set_images(irom.data(), irom.size(), nullptr, 0);
    soc.reset();

    const uint16_t w0 = soc.read16(0x004000);
    const uint16_t w1 = soc.read16(0x004001);
    check(w0 == uint16_t(irom[0] | (irom[1] << 8)),
          fmt("T1.1 read16(0x004000) == irom[0] | irom[1]<<8  (got %04x, want %04x)",
              w0, unsigned(irom[0] | (irom[1] << 8))));
    check(w1 == uint16_t(irom[2] | (irom[3] << 8)),
          fmt("T1.2 read16(0x004001) == irom[2] | irom[3]<<8  (got %04x, want %04x)",
              w1, unsigned(irom[2] | (irom[3] << 8))));

    // top of the 64 KB image: word 0x00bfff is the last word of the region
    const uint16_t wtop = soc.read16(0x00bfff);
    check(wtop == uint16_t(irom[0xfffe] | (irom[0xffff] << 8)),
          fmt("T1.3 read16(0x00bfff) == last ROM word  (got %04x, want %04x)",
              wtop, unsigned(irom[0xfffe] | (irom[0xffff] << 8))));

    // 0x00c000-0x00ffff mirrors the first 32 KB
    check(soc.read16(0x00c000) == w0, "T1.4 read16(0x00c000) mirrors 0x004000");
    check(soc.read16(0x00ffff) == soc.read16(0x007fff),
          "T1.5 read16(0x00ffff) mirrors 0x007fff (second half of the mirror)");

    // writes to ROM are dropped
    soc.write16(0x004000, 0xdead);
    check(soc.read16(0x004000) == w0, "T1.6 ROM writes are ignored");
}

void test_ram()
{
    Gpce4 soc;
    soc.reset();

    soc.write16(0x000100, 0xbeef);
    check(soc.read16(0x000100) == 0xbeef, "T2.1 ram read/write 0x000100 == 0xbeef");
    check(soc.ram[0x100] == 0xbeef,
          fmt("T2.2 value lands in ram[0x100]  (got %04x)", unsigned(soc.ram[0x100])));

    soc.write16(0x000fff, 0x1234);
    check(soc.read16(0x000fff) == 0x1234 && soc.ram[0xfff] == 0x1234,
          "T2.3 last RAM word 0x000fff");
    check(soc.read16(0x001000) == 0xffff, "T2.4 0x001000 is unmapped -> 0xffff");
}

void test_flash_window(const std::vector<uint8_t> &flash)
{
    Gpce4 soc;
    soc.set_images(nullptr, 0, flash.data(), flash.size());
    soc.reset();

    // SPI_Auto_Ctrl = READ4 style value; MAME's spi_direct_r() never decodes it,
    // it is only stored, so the window works with or without it.
    soc.write16(0x30e7, 0x0b);
    check(soc.read16(0x30e7) == 0x0b, "T3.1 SPI_Auto_Ctrl (0x30e7) is read/write");

    soc.write16(0x30e9, 0);
    check(soc.read16(0x30e9) == 0x0000, "T3.2 SPI_BANK reads back 0");
    const uint16_t lo = soc.read16(0x200010);
    check(lo == uint16_t(flash[0x20] | (flash[0x21] << 8)),
          fmt("T3.3 bank 0: read16(0x200010) == flash[0x20]|flash[0x21]<<8  (got %04x, want %04x)",
              lo, unsigned(flash[0x20] | (flash[0x21] << 8))));

    soc.write16(0x30e9, 1);
    const uint16_t hi = soc.read16(0x200010);
    check(hi == uint16_t(flash[0x400020] | (flash[0x400021] << 8)),
          fmt("T3.4 bank 1: read16(0x200010) == flash[0x400020]|flash[0x400021]<<8  (got %04x, want %04x)",
              hi, unsigned(flash[0x400020] | (flash[0x400021] << 8))));

    soc.write16(0x30e9, 0);
    check(soc.read16(0x3fffff) == uint16_t(flash[0x3ffffe] | (flash[0x3fffff] << 8)),
          "T3.5 last word of the direct window 0x3fffff");
}

void test_interrupts()
{
    std::vector<uint8_t> irom(65536, 0);
    Gpce4 soc;
    soc.set_images(irom.data(), irom.size(), nullptr, 0);
    soc.reset();

    unsp_device cpu;
    soc.set_cpu(&cpu);

    soc.tick(96000);   // exactly one 1 kHz tick
    check((soc.read16(0x3051) & 0x2000) != 0,
          fmt("T4.1 1 kHz source latches INT_Status bit13 with INT_Ctrl disabled  (status %04x)",
              unsigned(soc.read16(0x3051))));
    check((cpu.m_core->m_sirq & (1u << UNSP_IRQ2_LINE)) == 0,
          "T4.2 IRQ2 stays released while INT_Ctrl bit13 is clear");

    soc.write16(0x3050, 0x2000);   // INT_Ctrl: IRQ2_TMC enable
    check((soc.read16(0x3050) & 0x2000) != 0, "T4.3 INT_Ctrl bit13 reads back");
    check((soc.read16(0x3051) & 0x2000) != 0, "T4.4 enabling does not clear the pending bit");
    check((cpu.m_core->m_sirq & (1u << UNSP_IRQ2_LINE)) != 0,
          "T4.5 pending + enabled asserts UNSP_IRQ2_LINE on the CPU");

    soc.write16(0x3051, 0x2000);   // write one to clear
    check((soc.read16(0x3051) & 0x2000) == 0, "T4.6 INT_Status bit13 cleared by write-one");
    check((cpu.m_core->m_sirq & (1u << UNSP_IRQ2_LINE)) == 0,
          "T4.7 IRQ2 released after the handler clears the source");

    soc.tick(95999);
    check((soc.read16(0x3051) & 0x2000) == 0, "T4.8 no tick one cycle early");
    soc.tick(1);
    check((soc.read16(0x3051) & 0x2000) != 0, "T4.9 exactly one tick per 96000 cycles");

    soc.write16(0x3051, 0x0001);   // unrelated bit
    check((soc.read16(0x3051) & 0x2000) != 0, "T4.10 write-one-to-clear is per bit");
    soc.write16(0x3051, 0x2000);
    check((soc.read16(0x3051) & 0x2000) == 0, "T4.11 cleared again");

    // exact pending word with the 300 kHz machine source switched off: the
    // 1 kHz timer (status bit 29) plus 20 ticks of the 20 kHz timer (bit 31).
    Gpce4 quiet;
    quiet.set_images(irom.data(), irom.size(), nullptr, 0);
    quiet.timer_300k_enable = false;
    quiet.reset();
    quiet.write16(0x3050, 0x2000);
    quiet.tick(96000);
    check(quiet.read16(0x3051) == 0xa000,
          fmt("T4.12 INT_Status == 0xa000 (timer_c bit13 + timer_a bit15, 20 ticks)  (got %04x)",
              unsigned(quiet.read16(0x3051))));

    // the 300 kHz machine timer raises status bit 24 -> INT_Ctrl bit 8 -> IRQ3
    Gpce4 spdt;
    spdt.set_images(irom.data(), irom.size(), nullptr, 0);
    spdt.reset();
    spdt.write16(0x3050, 0x0100);            // INT_Ctrl: IRQ3_SPI2
    spdt.tick(320 * 3);
    check((spdt.read16(0x3051) & 0x0100) != 0,
          "T4.13 300 kHz source sets INT_Status bit8 after 3 periods");
    check(spdt.read16(0x3051) != 0, "T4.14 SPI2 interrupt pending");
}

void test_lcd()
{
    St7735 lcd;
    lcd.reset();
    check(!lcd.display_on, "T5.1 reset leaves the display off");
    check(lcd.sleep, "T5.2 reset leaves the panel asleep (MAME: m_sleep = 1)");

    lcd.command_w(0x11);                       // SLPOUT
    check(!lcd.sleep, "T5.3 SLPOUT wakes the panel");
    lcd.command_w(0x29);                       // DISPON
    check(lcd.display_on, "T5.4 DISPON turns the display on");

    // MADCTL MV=1 gives the straight posx/posy -> fb mapping.
    lcd.command_w(0x36);
    lcd.data_w(0x20);

    lcd.command_w(0x2a);                       // CASET 0..4
    lcd.data_w(0x00); lcd.data_w(0x00); lcd.data_w(0x00); lcd.data_w(0x04);
    lcd.command_w(0x2b);                       // RASET 0..3
    lcd.data_w(0x00); lcd.data_w(0x00); lcd.data_w(0x00); lcd.data_w(0x03);
    lcd.command_w(0x2c);                       // RAMWR

    const uint16_t px[5] = { 0xf800, 0x07e0, 0x001f, 0xffff, 0x1234 };
    for (int i = 0; i < 5; i++) {
        lcd.data_w(uint8_t(px[i] >> 8));
        lcd.data_w(uint8_t(px[i] & 0xff));
    }

    bool fb_ok = true;
    for (int i = 0; i < 5; i++)
        if (lcd.fb[i] != px[i])
            fb_ok = false;
    check(fb_ok, fmt("T5.5 five RGB565 pixels land at fb[0..4]  (fb0=%04x fb1=%04x fb2=%04x fb3=%04x fb4=%04x)",
                     unsigned(lcd.fb[0]), unsigned(lcd.fb[1]), unsigned(lcd.fb[2]),
                     unsigned(lcd.fb[3]), unsigned(lcd.fb[4])));
    check(lcd.m_displaybuffer[2] == 0x001f, "T5.6 GRAM index 2 holds the third pixel");
    check(lcd.fb[5] == 0x0000, "T5.7 pixel after the burst is untouched");

    // One more write wraps posx back to posminx and advances posy.
    lcd.data_w(0xab);
    lcd.data_w(0xcd);
    check(lcd.fb[128 + 0] == 0xabcd, "T5.8 window wrap advances to the next row");

    // MADCTL MV=0 is MAME's rotated mapping: idx = posy + (129-posx)*0x200.
    St7735 rot;
    rot.reset();
    rot.command_w(0x2a);
    rot.data_w(0x00); rot.data_w(0x02); rot.data_w(0x00); rot.data_w(0x02);  // CASET 2..2
    rot.command_w(0x2b);
    rot.data_w(0x00); rot.data_w(0x03); rot.data_w(0x00); rot.data_w(0x04);  // RASET 3..4
    rot.command_w(0x2c);
    rot.data_w(0xab);
    rot.data_w(0xcd);
    check(rot.fb[127 * 128 + 3] == 0xabcd,
          fmt("T5.9 MV=0: pixel(2,3) lands at fb[127*128+3]  (got %04x)",
              unsigned(rot.fb[127 * 128 + 3])));

    rot.data_w(0x0f);
    rot.data_w(0x0f);
    check(rot.fb[127 * 128 + 4] == 0x0f0f,
          fmt("T5.10 MV=0: next pixel advances posy to row 4  (got %04x)",
              unsigned(rot.fb[127 * 128 + 4])));

    // DISPOFF / SLPIN
    rot.command_w(0x28);
    check(!rot.display_on, "T5.11 DISPOFF turns the display off");
    rot.command_w(0x10);
    check(rot.sleep, "T5.12 SLPIN puts the panel back to sleep");

    // 0x01 SWRESET clears the addressing window but keeps GRAM.
    rot.command_w(0x01);
    check(rot.m_posminx == 0 && rot.m_posmaxx == 0, "T5.13 SWRESET clears the column window");
    check(rot.m_displaybuffer[65027] == 0xabcd && rot.m_displaybuffer[65028] == 0x0f0f,
          fmt("T5.14 SWRESET keeps GRAM contents  (gram[65027]=%04x gram[65028]=%04x)",
              unsigned(rot.m_displaybuffer[65027]), unsigned(rot.m_displaybuffer[65028])));
}

void test_ports()
{
    Gpce4 soc;
    soc.reset();

    soc.set_port(0, 0xfff7);              // BUTTON1 (bit 3) pressed, active low
    check(soc.read16(0x3000) == 0xfff7, "T6.1 IOA_Data returns the input pins");
    check(soc.read16(0x3001) == 0xfff7, "T6.2 IOA_Buffer also returns the input pins (MAME)");

    soc.write16(0x3002, 0x00ff);
    check(soc.read16(0x3002) == 0x00ff, "T6.3 IOA_Dir read/write");
    soc.write16(0x3003, 0xaaaa);
    check(soc.read16(0x3003) == 0xaaaa, "T6.4 IOA_Attrib read/write");

    check(soc.read16(0x3004) == 0xffef, "T6.5 IOB_Data returns the idle pins (charger unplugged)");
    soc.write16(0x3005, 0x0400);          // D/C = data
    check(soc.read16(0x3005) == 0x0400, "T6.6 IOB_Buffer latches the written value");
    check(soc.read16(0x3004) == 0xffef, "T6.7 IOB_Data still reads the pins");
    soc.write16(0x3004, 0x0000);          // output latch only
    check(soc.read16(0x3005) == 0x0400, "T6.8 IOB_Data write does not disturb IOB_Buffer");

    soc.set_port(1, 0xffff);
    check(soc.read16(0x3004) == 0xffff, "T6.9 set_port updates IOB reads");

    check(soc.read16(0x3008) == 0xffff, "T6.10 IOC_Data returns the input pins");
    check(soc.read16(0x3009) == 0xffff, "T6.11 IOC_Buffer returns the input pins (MAME)");
    check(soc.read16(0x300c) == 0xffff, "T6.12 hole in the IO map reads 0xffff");
}

void test_spi2_lcd()
{
    Gpce4 soc;
    soc.reset();

    // D/C is IOB bit 0x0400 for this machine (ref/generalplus_gpce4.cpp:344)
    soc.write16(0x3005, 0x0000);          // command mode
    soc.write16(0x3092, 0x11);            // SLPOUT
    check(!soc.lcd.sleep, "T7.1 SPI2 byte goes to command_w while IOB 0x0400 is clear");

    soc.write16(0x3005, 0x0400);          // data mode
    soc.write16(0x3092, 0x29);
    check(!soc.lcd.display_on, "T7.2 SPI2 byte goes to data_w while IOB 0x0400 is set");

    soc.write16(0x3005, 0x0000);
    soc.write16(0x3092, 0x29);            // DISPON, this time as a command
    check(soc.lcd.display_on, "T7.3 DISPON through the real command path");

    soc.write16(0x3092, 0x2a);            // CASET
    soc.write16(0x3005, 0x0400);
    soc.write16(0x3092, 0x00);
    soc.write16(0x3092, 0x00);
    soc.write16(0x3092, 0x00);
    soc.write16(0x3092, 0x00);
    soc.write16(0x3092, 0x2c);            // RAMWR (command mode again)
    soc.write16(0x3005, 0x0000);
    check(true, "T7.4 command/data interleaving did not assert");

    // MAME answers every SPI2 status read with a constant: TXStatus 0 (:1030,
    // m_spi2_txstatus is never set), RXStatus 0 (:1042), MISC 0 (:1054).  That
    // is what the mask ROM was written against, so it is the default here.
    check(soc.read16(0x3091) == 0x0000, "T7.5 SPI2_TXStatus reads 0 by default (MAME-exact)");
    check(soc.read16(0x3093) == 0x0000, "T7.6 SPI2_RXStatus reads 0 by default (MAME-exact)");
    check(soc.read16(0x3095) == 0x0000, "T7.7 SPI2_Misc reads 0 by default (MAME-exact)");

    // The real FIFO bookkeeping is still kept; the knob exposes it.
    soc.spi2_status_regs = true;
    soc.write16(0x3005, 0x0400);          // data mode, bytes fall into GRAM
    for (int i = 0; i < 16; i++)          // more than the 12 byte FIFO
        soc.write16(0x3092, 0x00);
    uint16_t st = soc.read16(0x3091);
    check((st & 0x000f) == 0 && (st & 0x8000) != 0,
          fmt("T7.8 16 byte burst keeps the FIFO bounded and raises SPITXIF (got %04x)",
              unsigned(st)));
    soc.write16(0x3091, 0x8000);          // clear SPITXIF
    check(soc.read16(0x3091) == 0x0000, "T7.9 SPITXIF is write-one-to-clear");
    check((soc.read16(0x3095) & 0x0003) == 0x0003, "T7.10 SPI2_Misc reports TFE|TNF when idle");
}

void test_spi_flash(const std::vector<uint8_t> &flash)
{
    Gpce4 soc;
    soc.set_images(nullptr, 0, flash.data(), flash.size());
    soc.reset();

    // MAME's generic_spi_flash answers on the clock *after* the command byte:
    // get_command() only switches state and clears m_state_step, so read()
    // right after the command returns the previous latch.  The SoC/driver
    // sample the device once per byte, so this skew is part of the machine's
    // observable behaviour and the tests below include it.
    auto flash_reset = [&soc]() { soc.write16(0x30e0, 0x0800); };  // SPI_Ctrl reset bit

    check(spi_xfer(soc, 0x9f) == 0x00, "T8.1 RDID command byte returns the stale latch");
    check(spi_xfer(soc, 0x00) == 0xfe, "T8.2 RDID id[0] == 0xfe");
    check(spi_xfer(soc, 0x00) == 0xfe, "T8.3 RDID id[1] == 0xfe");
    check(spi_xfer(soc, 0x00) == 0x00, "T8.4 RDID id[2] == 0x00");
    check(spi_xfer(soc, 0x00) == 0x00, "T8.5 RDID keeps repeating id[2] (MAME loops here)");

    // write enable / read status (single byte status mode, as the driver sets)
    flash_reset();
    spi_xfer(soc, 0x06);                                  // WREN
    spi_xfer(soc, 0x05);                                  // RDSR command byte
    check(spi_xfer(soc, 0x00) == 0x02, "T8.6 RDSR reports WEL after WREN");

    flash_reset();
    spi_xfer(soc, 0x04);                                  // WRDI
    spi_xfer(soc, 0x05);
    check(spi_xfer(soc, 0x00) == 0x00, "T8.7 RDSR reports WEL cleared after WRDI");

    // 0x03 read with a 24-bit address; the data starts one clock later
    flash_reset();
    spi_xfer(soc, 0x03);
    spi_xfer(soc, 0x00);
    spi_xfer(soc, 0x00);
    spi_xfer(soc, 0x10);
    const uint8_t rd0 = spi_xfer(soc, 0x00);
    const uint8_t rd1 = spi_xfer(soc, 0x00);
    check(rd0 == flash[0x10],
          fmt("T8.8 READ 0x000010 returns flash[0x10]  (got %02x, want %02x)",
              unsigned(rd0), unsigned(flash[0x10])));
    check(rd1 == flash[0x11],
          fmt("T8.9 READ auto-increments the address  (got %02x, want %02x)",
              unsigned(rd1), unsigned(flash[0x11])));

    // SPI_Ctrl bit 0x0800 resets the FIFOs and the flash state machine, and
    // must be invisible on read-back.
    soc.write16(0x30e0, 0x0800);
    check((soc.read16(0x30e0) & 0x0800) == 0, "T8.10 SPI_Ctrl masks out the reset bit on read");
    spi_xfer(soc, 0x06);                                  // accepted again == state was reset
    spi_xfer(soc, 0x05);
    check(spi_xfer(soc, 0x00) == 0x02,
          "T8.11 the flash state machine was reset (WREN accepted after a READ burst)");
    check(soc.read16(0x30e3) == 0x8000, "T8.12 SPI_RXStatus reads 0x8000");
    check(soc.read16(0x30e5) == 0x0004, "T8.13 SPI_Misc reads 0x0004");
}

void test_unmapped()
{
    Gpce4 soc;
    soc.reset();
    check(soc.read16(0x003100) == 0xffff, "T9.1 unmapped IO address reads 0xffff");
    check(soc.read16(0x080000) == 0xffff, "T9.2 unmapped high address reads 0xffff");
    soc.write16(0x003100, 0x1234);         // must not crash
    check(soc.read16(0x003100) == 0xffff, "T9.3 unmapped write is dropped");
}

} // anonymous namespace

int main()
{
    std::vector<uint8_t> irom;
    std::vector<uint8_t> flash;

    std::printf("=== GPCE4 / ST7735 selftest (MAME 0.289 port) ===\n");

    if (!load_rom("internal_rom_221017a.bin", irom)) {
        std::printf("FAIL: roms/internal_rom_221017a.bin not found (run from the repo root)\n");
        return 99;
    }
    if (!load_rom("dmc_v1_base.bin", flash)) {
        std::printf("FAIL: roms/dmc_v1_base.bin not found (run from the repo root)\n");
        return 99;
    }
    check(irom.size() == 65536, fmt("T0.1 internal ROM is 64 KB (%zu bytes)", irom.size()));
    check(flash.size() == 8388608, fmt("T0.2 SPI flash is 8 MB (%zu bytes)", flash.size()));

    test_internal_rom(irom);
    test_ram();
    test_flash_window(flash);
    test_interrupts();
    test_lcd();
    test_ports();
    test_spi2_lcd();
    test_spi_flash(flash);
    test_unmapped();

    std::printf("\n=== %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail > 125 ? 125 : g_fail;
}
