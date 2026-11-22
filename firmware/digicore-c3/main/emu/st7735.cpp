// ---------------------------------------------------------------------------
//  st7735.cpp - Sitronix ST7735 LCD controller model
//
//  Port of MAME 0.289 src/devices/video/st7735_lcdc.cpp (ref/st7735_lcdc.cpp).
//  The MAME model keeps a 256x256 word GRAM and renders it through a cliprect;
//  this port keeps the identical window/rotation maths but stores only the
//  128x128 clipped view (St7735::fb, 32 KiB), because the ESP32-C3 has no PSRAM
//  and the 128 KiB GRAM would not fit next to the SPI flash page cache.  Every
//  pixel the panel can show lands in the same fb[] slot it would have had.
//
//  Everything MAME does with command parameters is reproduced bit for bit;
//  registers MAME only logs are still stored here (never interpreted), and the
//  commands MAME does not know at all are answered the way the real chip does
//  (window reset on SWRESET, DISPOFF, inversion, ...) *and* logged once.
//  See the "deviations" section of docs/SOC_PORTS.md and docs/C3_PORT.md.
// ---------------------------------------------------------------------------

#include "st7735.h"

#include <algorithm>
#include <cstdio>

namespace {

// ref/st7735_lcdc.cpp: m_displaybuffer is 256*256 and render_to_bitmap() reads
// it as (y*0x200 + x), i.e. 512 words per row with only the first 128 visible.
constexpr int GRAM_W    = 0x200;
constexpr int PANEL     = 128;

} // anonymous namespace

// ---------------------------------------------------------------------------
//  reset - ref/st7735_lcdc.cpp device_start(): every register cleared, the
//  panel left asleep (m_sleep = 1) and the display off, MADCTL 0.
// ---------------------------------------------------------------------------
void St7735::reset()
{
    std::fill(fb, fb + PANEL * PANEL, uint16_t(0));

    m_posx = m_posy = 0;
    m_posminx = m_posmaxx = m_posminy = m_posmaxy = 0;
    m_command = 0;
    m_commandstep = 0;
    m_madctl = 0;
    m_colmod = 0;
    m_pixellatch = 0;
    std::fill(m_params, m_params + sizeof(m_params), uint8_t(0));
    std::fill(m_unknown_seen, m_unknown_seen + sizeof(m_unknown_seen), uint8_t(0));

    display_on = false;
    sleep = true;                 // ref/st7735_lcdc.cpp: m_sleep = 1 in device_start
}

// 0x01 SWRESET: the registers a real ST7735 clears, without touching GRAM.
// (MAME logs this as an unknown command and does nothing - see docs.)
void St7735::soft_reset_regs()
{
    m_posx = m_posy = 0;
    m_posminx = m_posmaxx = m_posminy = m_posmaxy = 0;
    m_commandstep = 0;
    m_madctl = 0;
    m_colmod = 0;
    m_pixellatch = 0;
    std::fill(m_params, m_params + sizeof(m_params), uint8_t(0));
    display_on = false;
    sleep = true;
}

void St7735::log_unknown(uint8_t cmd)
{
    const uint8_t bit = uint8_t(1u << (cmd & 7u));
    uint8_t &slot = m_unknown_seen[cmd >> 3];
    if (slot & bit)
        return;
    slot = uint8_t(slot | bit);
    std::printf("[st7735] unknown command %02x ignored\n", unsigned(cmd));
}

// ---------------------------------------------------------------------------
//  store_pixel - the 0x2c write path, ref/st7735_lcdc.cpp (m_command == 0x2c)
//
//    MV (MADCTL bit 5) set :  idx = posx + posy*0x200
//    MV clear               :  idx = posy + (129-posx)*0x200
//
//  The second form rotates the panel by 90 degrees; it is what MAME does, so it
//  is what this port does.  fb[] receives the 128x128 clipped view.
// ---------------------------------------------------------------------------
void St7735::store_pixel(uint16_t pixel)
{
    m_px_writes++;
    const int idx = ((m_madctl & 0x0020) ? (m_posx + m_posy * GRAM_W)
                                         : (m_posy + (129 - m_posx) * GRAM_W)) & 0xffff;

    const unsigned ry = unsigned(idx) >> 9;
    const unsigned rx = unsigned(idx) & 0x1ffu;
    if (rx < unsigned(PANEL) && ry < unsigned(PANEL))
        fb[ry * PANEL + rx] = pixel;
}

// ---------------------------------------------------------------------------
//  command_w - ref/st7735_lcdc.cpp lcdc_command_w()
// ---------------------------------------------------------------------------
void St7735::command_w(uint8_t d)
{
    m_command = d;
    m_commandstep = 0;

    switch (d) {
    case 0x00:                                  // NOP
        break;

    case 0x01:                                  // SWRESET (MAME: unknown)
        soft_reset_regs();
        break;

    case 0x10:                                  // SLPIN
        sleep = true;
        break;

    case 0x11:                                  // SLPOUT
        sleep = false;
        break;

    case 0x12:                                  // NORON   - normal display mode on
    case 0x13:                                  // IDMOFF  - inversion off (partial mode)
        break;

    case 0x20:                                  // INVOFF
        break;

    case 0x21:                                  // INVON
        break;

    case 0x28:                                  // DISPOFF (MAME: unknown)
        display_on = false;
        break;

    case 0x29:                                  // DISPON
        display_on = true;
        break;

    case 0x2a:                                  // CASET - column address set
    case 0x2b:                                  // RASET - row address set
    case 0x2c:                                  // RAMWR - memory write
        break;

    case 0x36:                                  // MADCTL
    case 0x3a:                                  // COLMOD
    case 0x2d:                                  // RAMRD / ST7735S colour LUT
    case 0xb1: case 0xb2: case 0xb3: case 0xb4: // frame rate / porch
    case 0xc0: case 0xc1: case 0xc2: case 0xc3: // power control
    case 0xc4: case 0xc5:
    case 0xe0: case 0xe1:                       // gamma tables (16 bytes each)
        break;

    default:
        // 0x30-0x3x power control, 0x50-0x63 window/gamma, 0xb5-0xbe, 0xc6-0xc8,
        // 0xd0-0xd6 read commands - parameter carrying, never interpreted here.
        if ((d >= 0x30 && d <= 0x35) || (d >= 0x37 && d <= 0x39) || (d >= 0x3b && d <= 0x3f) ||
            (d >= 0x50 && d <= 0x63) ||
            (d >= 0xb5 && d <= 0xbe) ||
            (d >= 0xc6 && d <= 0xc8) ||
            (d >= 0xd0 && d <= 0xd6))
            break;

        log_unknown(d);
        break;
    }

    if (trace)
        std::printf("[st7735] command %02x\n", unsigned(d));
}

// ---------------------------------------------------------------------------
//  data_w - ref/st7735_lcdc.cpp lcdc_data_w()
//  MAME increments m_commandstep for *every* data byte, whatever the command.
// ---------------------------------------------------------------------------
void St7735::data_w(uint8_t d)
{
    switch (m_command) {
    case 0x2b:                                  // RASET: 4 parameter bytes
        switch (m_commandstep) {
        case 0: m_posminy = (int(d) << 8) | (m_posminy & 0xff); break;
        case 1: m_posminy = (m_posminy & 0xff00) | d;           break;
        case 2: m_posmaxy = (int(d) << 8) | (m_posmaxy & 0xff); break;
        case 3: m_posmaxy = (m_posmaxy & 0xff00) | d;           break;
        default: break;
        }
        m_posy = m_posminy;                     // MAME: every byte resets posy
        if (trace)
            std::printf("[st7735] RASET step %d byte %02x -> min %d max %d\n",
                        m_commandstep, unsigned(d), m_posminy, m_posmaxy);
        break;

    case 0x2a:                                  // CASET: 4 parameter bytes
        switch (m_commandstep) {
        case 0: m_posminx = (int(d) << 8) | (m_posminx & 0xff); break;
        case 1: m_posminx = (m_posminx & 0xff00) | d;           break;
        case 2: m_posmaxx = (int(d) << 8) | (m_posmaxx & 0xff); break;
        case 3: m_posmaxx = (m_posmaxx & 0xff00) | d;           break;
        default: break;
        }
        m_posx = m_posminx;                     // MAME: every byte resets posx
        if (trace)
            std::printf("[st7735] CASET step %d byte %02x -> min %d max %d\n",
                        m_commandstep, unsigned(d), m_posminx, m_posmaxx);
        break;

    case 0x2c:                                  // RAMWR: 16-bit RGB565 pixels
        m_pixellatch = uint16_t((m_pixellatch << 8) | d);
        if (m_commandstep & 1) {
            store_pixel(m_pixellatch);
            m_posx++;
            if (m_posx > m_posmaxx) {
                m_posx = m_posminx;
                m_posy++;
                if (m_posy > m_posmaxy)
                    m_posy = m_posminy;
            }
        }
        break;

    case 0x36:                                  // MADCTL
        if (m_commandstep == 0)
            m_madctl = d;
        break;

    case 0x3a:                                  // COLMOD
        if (m_commandstep == 0)
            m_colmod = d;
        break;

    case 0xe0:
    case 0xe1:                                  // gamma tables, 16 bytes
        if (m_commandstep < int(sizeof(m_params)))
            m_params[m_commandstep] = d;
        if (trace)
            std::printf("[st7735] gamma %02x step %d byte %02x\n",
                        unsigned(m_command), m_commandstep, unsigned(d));
        break;

    default:
        // Parameter sink for every other parameter-carrying command (0x2d,
        // 0x30-0x3x, 0x50-0x63, 0xb1-0xbe, 0xc0-0xc8, 0xd0-0xd6).  Nothing in
        // the MAME model looks at these.
        if (m_commandstep < int(sizeof(m_params)))
            m_params[m_commandstep] = d;
        break;
    }

    m_commandstep++;
}

// ---------------------------------------------------------------------------
//  read_data - ref/st7735_lcdc.cpp lcdc_data_r() returns 0 unconditionally.
//  (The panel's GRAM read-out is not modelled by MAME either.)
// ---------------------------------------------------------------------------
uint8_t St7735::read_data()
{
    return 0;
}

// ---------------------------------------------------------------------------
//  fill_run - the same pixels as `data_w()` called twice per pixel, without the
//  register plumbing in between.
//
//  The guest's constant-colour loop (0x005B1E) never changes the value it
//  pushes, so a whole span is one pixel repeated.  Going through data_w() means
//  two register writes per pixel, each one decoding 0x3092, bumping the SPI2
//  FIFO counter, setting TXIF and calling back here - about 90 host cycles of
//  bookkeeping for 6 cycles of actual work.  This does the work directly.
//
//  Correctness rests on copying store_pixel()'s index maths verbatim (including
//  the 129-posx rotation when MADCTL.MV is clear) and on advancing posx/posy
//  with exactly the wrap rule data_w() uses.  m_pixellatch ends up holding the
//  last pixel, and m_commandstep advances by two per pixel, so a follow-up
//  data_w() sees the same state a real byte stream would have left.
// ---------------------------------------------------------------------------
void St7735::fill_run(uint16_t pixel, uint32_t count)
{
    if (m_command != 0x2c || count == 0u)
        return;

    m_px_writes += count;
    const bool mv = (m_madctl & 0x0020) != 0;
    for (uint32_t i = 0; i < count; i++)
    {
        const int idx = (mv ? (m_posx + m_posy * GRAM_W)
                            : (m_posy + (129 - m_posx) * GRAM_W)) & 0xffff;
        const unsigned ry = unsigned(idx) >> 9;
        const unsigned rx = unsigned(idx) & 0x1ffu;
        if (rx < unsigned(PANEL) && ry < unsigned(PANEL))
            fb[ry * PANEL + rx] = pixel;

        m_posx++;
        if (m_posx > m_posmaxx)
        {
            m_posx = m_posminx;
            m_posy++;
            if (m_posy > m_posmaxy)
                m_posy = m_posminy;
        }
    }

    m_pixellatch = pixel;
    m_commandstep += int(count * 2u);
}
