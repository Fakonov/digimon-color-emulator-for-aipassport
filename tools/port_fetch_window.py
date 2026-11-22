#!/usr/bin/env python
"""Port the opcode fetch-window fast path from emu/ to the ESP32-C3 tree.

PC side is edited by hand; this script applies the same three insertions to
firmware/digicore-c3/main/emu/{compat/unsp.h,unsp_glue.cpp,gpce4.h,gpce4.cpp}
and rewrites every opcode fetch to go through the window cache.  Every anchor is
required: a missing pattern aborts the run so a silent no-op is impossible.

The C3 flash image is a sparse page store, so the window is one 4 KiB page
(2048 words) and is only valid when the page is present in the packed image.
"""
import io
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
C3 = os.path.join(ROOT, "firmware", "digicore-c3", "main", "emu")

HEADER_ANCHOR = """struct UnspBus
{
\tvirtual ~UnspBus() {}
\tvirtual uint16_t read16(uint32_t address) = 0;
\tvirtual void write16(uint32_t address, uint16_t data) = 0;
};"""

HEADER_NEW = """struct UnspBus
{
\tvirtual ~UnspBus() {}
\tvirtual uint16_t read16(uint32_t address) = 0;
\tvirtual void write16(uint32_t address, uint16_t data) = 0;
\t// Optional instruction-fetch window: returns a host pointer that directly
\t// backs `address` (aligned down to the window size) and reports the window
\t// base/mask.  NULL when the address has no such window (IO, unmapped page).
\tvirtual const uint16_t *fetch_window(uint32_t address, uint32_t *base, uint32_t *mask)
\t{
\t\t(void)address; (void)base; (void)mask;
\t\treturn nullptr;
\t}
};"""

CACHE_ANCHOR = """\tvoid add_lpc(const int32_t offset)
\t{
\t\tconst uint32_t new_lpc = UNSP_LPC + offset;
\t\tm_core->m_r[REG_PC] = (uint16_t)new_lpc;
\t\tm_core->m_r[REG_SR] &= 0xffc0;
\t\tm_core->m_r[REG_SR] |= (new_lpc >> 16) & 0x3f;
\t}"""

CACHE_NEW = CACHE_ANCHOR + """

\t// Instruction-fetch window cache: code lives in one or two pages at a time
\t// (internal ROM 0x4800.. / flash 0x230000..), so a single-entry cache turns
\t// every opcode fetch into "compare base, index array".
\tconst uint16_t *m_win_ptr = nullptr;
\tuint32_t m_win_base = 0;
\tuint32_t m_win_mask = 0;

\tinline uint16_t fetch16(uint32_t address)
\t{
\t\tif ((address & ~m_win_mask) == m_win_base)
\t\t\treturn m_win_ptr[address & m_win_mask];
\t\tuint32_t base = 0, mask = 0;
\t\tconst uint16_t *p = m_bus->fetch_window(address, &base, &mask);
\t\tif (p)
\t\t{
\t\t\tm_win_ptr = p;
\t\t\tm_win_base = base;
\t\t\tm_win_mask = mask;
\t\t\treturn p[address & mask];
\t\t}
\t\tm_win_ptr = nullptr;
\t\tm_win_base = 0xffffffffu;
\t\tm_win_mask = 0;
\t\treturn read16(address);
\t}"""

GPCE4_H_ANCHOR = """    uint16_t read16(uint32_t addr) override;
    void write16(uint32_t addr, uint16_t data) override;"""

GPCE4_H_NEW = GPCE4_H_ANCHOR + """
\t// Direct host window for opcode fetch (RAM / internal ROM / SPI flash page).
    const uint16_t *fetch_window(uint32_t addr, uint32_t *base, uint32_t *mask) override;"""

GPCE4_C_ANCHOR = """uint16_t Gpce4::read16(uint32_t addr)
{
\taddr &= ADDR_MASK;"""

GPCE4_C_NEW = """// Opcode fetch fast path: hand the CPU a host pointer for the 4 KiB page that
// holds `addr`.  The sparse flash store keeps each present page contiguous, so
// only pages that are actually in the image get a window; anything else (IO,
// absent pages) falls back to read16().
const uint16_t *Gpce4::fetch_window(uint32_t addr, uint32_t *base, uint32_t *mask)
{
    constexpr uint32_t WINDOW_MASK = 0x7ff;                 // 2048 words = 4 KiB
\taddr &= ADDR_MASK;

\tif (addr < 0x001000) {                                  // word-addressed RAM
\t\t*base = addr & ~WINDOW_MASK;
\t\t*mask = WINDOW_MASK;
\t\treturn ram + *base;
\t}

\tif (addr >= 0x004000 && addr <= 0x00bfff && m_irom) {   // 64 KB internal mask ROM
\t\t*base = addr & ~WINDOW_MASK;
\t\t*mask = WINDOW_MASK;
\t\treturn reinterpret_cast<const uint16_t *>(m_irom + (*base - 0x004000) * 2);
\t}

\tif (addr >= 0x200000 && addr <= 0x3fffff && m_flash) {  // SPI flash page store
\t\tconst uint32_t off = (addr - 0x200000) * 2;
\t\tconst uint16_t *page = m_flash->page_window(off);
\t\tif (page) {
\t\t\t*base = addr & ~WINDOW_MASK;
\t\t\t*mask = WINDOW_MASK;
\t\t\treturn page;
\t\t}
\t}

\t*base = 0xffffffffu;
\t*mask = 0;
\treturn nullptr;
}

uint16_t Gpce4::read16(uint32_t addr)
{
\taddr &= ADDR_MASK;"""

ROMSTORE_ANCHOR = """\t// Single byte view for the SPI1 command path (generic_spi_flash model)."""

ROMSTORE_NEW = """\t// Host pointer for the 4 KiB page holding byte offset `off`, or null when the
\t// page is absent from the packed image.  Used by the CPU's opcode window.
\tconst uint16_t *page_window(uint32_t off)
\t{
\t\tconst uint32_t page = off >> PAGE_BITS;
\t\tif (page - m_ov_lo <= m_ov_hi - m_ov_lo)
\t\t\treturn nullptr;                 // overlay pages are patched copies
\t\tif (!m_base)
\t\t\treturn nullptr;
\t\tconst uint32_t po = m_index[page];
\t\tif (po == INDEX_NONE)
\t\t\treturn nullptr;
\t\treturn reinterpret_cast<const uint16_t *>(m_base + po);
\t}

\t// Single byte view for the SPI1 command path (generic_spi_flash model)."""


def patch(path, pairs, optional=False):
    text = io.open(path, encoding="utf-8").read()
    changed = 0
    for anchor, new in pairs:
        if new in text:
            continue
        if anchor not in text:
            if optional:
                continue
            sys.exit("anchor missing in %s:\n%s" % (path, anchor[:120]))
        text = text.replace(anchor, new, 1)
        changed += 1
    io.open(path, "w", encoding="utf-8", newline="\n").write(text)
    print("%-46s %d edit(s)" % (os.path.relpath(path, ROOT), changed))


def main():
    patch(os.path.join(C3, "compat", "unsp.h"),
          [(HEADER_ANCHOR, HEADER_NEW), (CACHE_ANCHOR, CACHE_NEW)])
    patch(os.path.join(C3, "gpce4.h"), [(GPCE4_H_ANCHOR, GPCE4_H_NEW)])
    patch(os.path.join(C3, "gpce4.cpp"), [(GPCE4_C_ANCHOR, GPCE4_C_NEW)])
    patch(os.path.join(C3, "romstore.h"), [(ROMSTORE_ANCHOR, ROMSTORE_NEW)])

    glue = os.path.join(C3, "unsp_glue.cpp")
    text = io.open(glue, encoding="utf-8").read()
    n16 = text.count("const uint16_t op = read16(lpc);")
    n32 = text.count("const uint32_t op = read16(lpc);")
    text = text.replace("const uint16_t op = read16(lpc);", "const uint16_t op = fetch16(lpc);")
    text = text.replace("const uint32_t op = read16(lpc);", "const uint32_t op = fetch16(lpc);")
    io.open(glue, "w", encoding="utf-8", newline="\n").write(text)
    print("%-46s %d fetch site(s)" % (os.path.relpath(glue, ROOT), n16 + n32))
    return 0


if __name__ == "__main__":
    sys.exit(main())
