// Host-side equivalence test for St7735::write_run() against the byte stream.  
// Write_run() replaces two SPI2 TXDATA writes per pixel on the device, so the
// only thing that has to be true is that it lands the same pixels and leaves the
// same window state.  Build: see tools/r6b_run_test.ps1.
#include <cstdio>
#include <cstring>
#include <cstdint>
#include "../main/emu/gpce4.h"

static int fails = 0;

static void programme_window(St7735 &lcd, uint16_t madctl, int x0, int y0, int x1, int y1)
{
    lcd.command_w(0x36);
    lcd.data_w(uint8_t(madctl & 0xff));
    lcd.command_w(0x2a);
    lcd.data_w(uint8_t(x0 >> 8)); lcd.data_w(uint8_t(x0));
    lcd.data_w(uint8_t(x1 >> 8)); lcd.data_w(uint8_t(x1));
    lcd.command_w(0x2b);
    lcd.data_w(uint8_t(y0 >> 8)); lcd.data_w(uint8_t(y0));
    lcd.data_w(uint8_t(y1 >> 8)); lcd.data_w(uint8_t(y1));
    lcd.command_w(0x2c);
}

static void run_case(const char *name, uint16_t madctl, int x0, int y0, int x1, int y1, int npx)
{
    static uint16_t px[16384];   // a whole 96x96 screen plus slack
    for (int i = 0; i < npx; i++)
        px[i] = uint16_t(0x1234u + uint32_t(i) * 37u + uint32_t(i >> 3));

    St7735 a, b;
    a.reset();
    b.reset();
    programme_window(a, madctl, x0, y0, x1, y1);
    programme_window(b, madctl, x0, y0, x1, y1);

    // byte stream: high byte first, exactly like 0x005B4A does
    for (int i = 0; i < npx; i++)
    {
        a.data_w(uint8_t(px[i] >> 8));
        a.data_w(uint8_t(px[i]));
    }
    b.write_run(px, uint32_t(npx));

    const bool fb_same = std::memcmp(a.fb, b.fb, sizeof(a.fb)) == 0;
    const bool st_same = a.m_posx == b.m_posx && a.m_posy == b.m_posy &&
                         a.m_pixellatch == b.m_pixellatch &&
                         a.m_commandstep == b.m_commandstep &&
                         a.m_px_writes == b.m_px_writes &&
                         std::memcmp(a.m_dirty, b.m_dirty, sizeof(a.m_dirty)) == 0;
    std::printf("%-28s px=%5d fb=%s state=%s (pos %d,%d / %d,%d latch %04X/%04X step %d/%d)\n",
                name, npx, fb_same ? "ok" : "DIFF", st_same ? "ok" : "DIFF",
                a.m_posx, a.m_posy, b.m_posx, b.m_posy,
                unsigned(a.m_pixellatch), unsigned(b.m_pixellatch),
                a.m_commandstep, b.m_commandstep);
    if (!fb_same || !st_same)
        fails++;
}

int main()
{
    // MV set: the GRAM walk runs along x (what the guest uses).
    run_case("mv=1 row 96px", 0x0020 | 0x20, 0, 18, 95, 113, 96);
    run_case("mv=1 full 96x96", 0x0020 | 0x20, 0, 18, 95, 113, 96 * 96);
    run_case("mv=1 partial 40px", 0x0020 | 0x20, 7, 30, 46, 113, 40 * 84);
    run_case("mv=0 rotated walk", 0x0000, 0, 18, 95, 113, 300);
    run_case("mv=1 column window", 0x0020 | 0x20, 0, 18, 0, 113, 96 * 96);
    std::printf("%s\n", fails ? "FAILED" : "all cases match");
    return fails ? 1 : 0;
}
