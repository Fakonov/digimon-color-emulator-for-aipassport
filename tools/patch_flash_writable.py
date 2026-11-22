"""Make Gpce4SpiFlash a writable NOR part instead of a read-only image.

MAME's generic_spi_flash_device never writes (the Digimon Color game writes its
save block at flash 0x7fe000); this patch models page program (AND semantics)
and 4 KB sector erase, gated by Gpce4SpiFlash::writable.
"""
import sys

p = 'emu/gpce4.cpp'
s = open(p, encoding='utf-8').read()

subs = [
    ("void Gpce4SpiFlash::set_image(const uint8_t *rom, size_t size)",
     "void Gpce4SpiFlash::set_image(uint8_t *rom, size_t size)"),
    ("void Gpce4::set_images(const uint8_t *irom, size_t irom_size, const uint8_t *flash, size_t flash_size)",
     "void Gpce4::set_images(const uint8_t *irom, size_t irom_size, uint8_t *flash, size_t flash_size)"),
]

old_pp = """        } else if (m_statusreg & 0x02) {
            // The image is read-only in this port: report and drop.
            std::printf("[spi_flash] page program %06x = %02x ignored (read-only image)\\n",
                        m_addr, unsigned(data));
            m_addr++;
        }"""
new_pp = """        } else if (m_statusreg & 0x02) {
            if (writable && m_rom && m_addr < m_size) {
                // A NOR page program can only clear bits.
                m_rom[m_addr] &= data;
            } else if (!writable) {
                std::printf("[spi_flash] page program %06x = %02x ignored (read-only image)\\n",
                            m_addr, unsigned(data));
            }
            m_addr++;
        }"""
subs.append((old_pp, new_pp))

old_se = """            m_addr = (m_addr & 0xffff00) | data;
            std::printf("[spi_flash] sector erase %06x ignored (read-only image)\\n", m_addr);
            m_state = READY_FOR_COMMAND;"""
new_se = """            m_addr = (m_addr & 0xffff00) | data;
            if (writable && m_rom) {
                const uint32_t base = m_addr & ~uint32_t(0xfffu);
                const uint32_t end = (uint32_t)m_size < base + 0x1000u ? (uint32_t)m_size : base + 0x1000u;
                for (uint32_t a = base; a < end; a++) m_rom[a] = 0xff;
            } else {
                std::printf("[spi_flash] sector erase %06x ignored (read-only image)\\n", m_addr);
            }
            m_state = READY_FOR_COMMAND;"""
subs.append((old_se, new_se))

for old, new in subs:
    if old not in s:
        sys.exit('pattern not found: %r' % old[:60])
    s = s.replace(old, new)

open(p, 'w', encoding='utf-8', newline='\n').write(s)
print('patched', p)
