# GPCE4 SoC + ST7735 port — implemented registers

Port target: MAME 0.289, machines `generalplus_gpce4` / `generalplus_gpce4_digicolr_state`
(this toy is the **digicolr** variant: D/C on IOB bit `0x0400`, 128x128 panel).

| file | mirrors |
|---|---|
| `emu/gpce4.cpp` / `emu/gpce4.h` | `ref/generalplus_gpce4_soc.cpp` (SoC), `ref/generalplus_gpce4.cpp` (machine wiring, CPU clock), `generic_spi_flash.cpp` (SPI flash device), folded into `Gpce4SpiFlash` |
| `emu/st7735.cpp` / `emu/st7735.h` | `ref/st7735_lcdc.cpp` |
| `emu/gpce4_selftest.cpp` | none — loads `roms/internal_rom_221017a.bin` + `roms/dmc_v1_base.bin` and asserts the behaviours below |

Line numbers below are **0.289 line numbers in that exact file** and were re-verified
against `ref/` with `Select-String`; the same numbers appear in the `// :NNN` comment on
every handler in `emu/gpce4.cpp`.

Terminology: every bus address is a **23-bit word address**. `read16`/`write16` move one
16-bit word, exactly like MAME's `internal_map`. Byte images (mask ROM, flash) are
therefore indexed as `image[word*2] | image[word*2+1] << 8`.

---

## 1. Memory map (`internal_map`, ref/generalplus_gpce4_soc.cpp:150-267)

| range | size | read | write | MAME |
|---|---|---|---|---|
| `0x000000-0x000fff` | 4096 words | `ram[addr]` | `ram[addr] = data` | :152 |
| `0x003000-0x0030ef` | IO block | see §3 | see §3 | :155-261 |
| `0x004000-0x00bfff` | 32768 words | `irom[ofs*2] \| irom[ofs*2+1]<<8` | ignored (logged) | :263 |
| `0x00c000-0x00ffff` | 16384 words | mirror of the **first 32 KB** of the mask ROM | ignored (logged) | :264 |
| `0x200000-0x3fffff` | 2 M words | `flash[(ofs*2) + spi_bank*0x400000]` (+1 for the high byte) | ignored (logged) | :266 |

`ofs` is always relative to the start of the window (`addr - 0x004000`, `addr - 0x00c000`,
`addr - 0x200000`). The direct flash window does **not** look at `SPI_Auto_Ctrl` — MAME's
`spi_direct_r()` (:879) ignores it, so this port does too.

Unmapped addresses (`0x001000-0x002fff`, `0x010000-0x1fffff`, `0x400000+`, and the gaps
inside the IO block) return `0xffff` and writes are dropped; both are logged once per
address into a 64-entry table so a runaway access cannot flood stdout.

`0x4000-0x400f` are the factory test vectors, `0x4010-0x401f` the user vectors,
`0x4020-0x47ff` the standard test program and `0x4800+` the custom ROM (MAME's own comment
at :263). `device_start()` sets vectorbase and boot vectorbase to `0x4010`
(:42-81); the standalone SoC has no such API, so the harness must set
`cpu.m_vectorbase = 0x4010` and start the CPU at `read16(0x4017)` itself.

---

## 2. Reset values

`Gpce4::reset()` (emu/gpce4.cpp:306) mirrors `device_reset()` (ref/…soc.cpp:103-135):
every stored register goes to `0x0000`, RAM is cleared, the LCD is reset, the SPI FIFOs are
emptied and the flash device is reset. Port *input* registers are the exception — they come
from the ioport defaults of the digicolr machine (ref/generalplus_gpce4.cpp:284-299):

| port | idle value | why |
|---|---|---|
| A (`0x3000-0x3003`) | `0xffff` | three active-low buttons released |
| B (`0x3004-0x3007`) | `0xffef` | bit 2 "Battery Charging" default `0x0004`→released; bit 4 "Charger Connected" default `0x0000`→**pressed** |
| C (`0x3008-0x300b`) | `0xffff` | nothing wired |

`set_port(port, bits)` overwrites the whole 16-bit input word, same as an ioport field
read; bit = 1 means released, 0 means pressed.

---

## 3. IO register table (`0x3000-0x30ef`)

"Reset" is the value after `Gpce4::reset()`; `—` means the register is not stored.

### 3.1 Ports A/B/C (:296-440)

| addr | name | read | write | reset | MAME |
|---|---|---|---|---|---|
| `3000` | IOA_Data | input pins (`m_port_in[0]`) | `m_porta_out(data)` — nothing wired in digicolr | `0xffff` | :296 / :302 |
| `3001` | IOA_Buffer | **input pins** (MAME quirk, not the latch) | latch `m_ioa_buffer`, plus `m_porta_out` | `0x0000` (readback still pins) | :308 / :314 |
| `3002` | IOA_Dir | stored | stored | `0x0000` | :321 / :327 |
| `3003` | IOA_Attrib | stored | stored | `0x0000` | :333 / :339 |
| `3004` | IOB_Data | input pins (`m_port_in[1]`) | latches `m_iob_out` (this is the **D/C** bit source) | `0xffef` | :347 / :353 |
| `3005` | IOB_Buffer | latched `m_iob_buffer` | `m_iob_buffer = data`, `m_iob_out = data` | `0x0000` (readback is the latch) | :359 / :365 |
| `3006` | IOB_Dir | stored | stored | `0x0000` | :372 / :378 |
| `3007` | IOB_Attrib | stored | stored | `0x0000` | :390 / :384 |
| `3008` | IOC_Data | input pins (`m_port_in[2]`) | `m_portc_out` — nothing wired | `0xffff` | :398 / :404 |
| `3009` | IOC_Buffer | **input pins** (MAME quirk) | `m_portc_out` only, no latch | `0x0000` | :410 / :416 |
| `300a` | IOC_Dir | stored | stored | `0x0000` | :422 / :428 |
| `300b` | IOC_Attrib | stored | stored | `0x0000` | :440 / :434 |
| `300f` | IO_Ctrl | stored | stored | `0x0000` | :447 / :453 |

Note the asymmetry: A and C buffers read the pins, B's buffer reads the latch. That is
MAME's code, not a port bug; `read_data`/`set_port` users must not assume otherwise.

### 3.2 Timers (:461-498)

| addr | name | read | write | reset | MAME |
|---|---|---|---|---|---|
| `3010` | TimerA_Data | unmapped (write-only) | logged, not stored | — | :473 |
| `3011` | TimerA_CNTR | write-only | logged | — | :478 |
| `3012` | TimerB_Data | write-only | logged | — | :483 |
| `3013` | TimerB_CNTR | write-only | logged | — | :488 |
| `3014` | TimerC_Data | write-only | logged | — | :493 |
| `3015` | TimerC_CNTR | write-only | logged | — | :498 |
| `3016` | Timer_Ctrl | stored | stored | `0x0000` | :461 / :467 |

The hardware timers are not counted from these registers; the game only ever uses the
periodic sources of §4.

### 3.3 PWM, system, cache, DAC (:505-625)

| addr | name | read | write | reset | MAME |
|---|---|---|---|---|---|
| `3020` | PWM0_Ctrl | `0x0000` always | logged, not stored | — | :505 / :529 |
| `3021` | PWM1_Ctrl | `0x0000` | logged | — | :511 / :534 |
| `3022` | PWM2_Ctrl | `0x0000` | logged | — | :517 / :539 |
| `3023` | PWM3_Ctrl | `0x0000` | logged | — | :523 / :544 |
| `3030` | System_Clock | write-only | logged, ignored | — | :551 |
| `3033` | Timebase_Clear | write-only | logged, ignored | — | :556 |
| `3034` | Watchdog_Clear | write-only | logged, ignored | — | :561 |
| `3035` | Wait_Ctrl | write-only | stored in `m_wait_ctrl`, **not modelled** | `0x0000` | :567 |
| `3039` | Cache_Ctrl | `m_cache_ctrl & ~0x0002` | stored | `0x0000` | :594 / :600 |
| `3040` | DAC_Ctrl | write-only | logged, stored in `m_dac_ctrl` | — | :608 |
| `3041` | DAC_CH1_Data | write-only | stored in `m_dac_cha1`, **drives the audio model** (docs/AUDIO.md) | `0x0000` | :613 |
| `3042` | DAC_CH2_Data | write-only | logged, stored in `m_dac_cha2` (not mixed) | — | :620 |
| `3043` | PPAMCtrl | write-only | logged, stored in `m_ppam_ctrl` (mode bits recorded, not modelled) | — | :625 |

`3041` is the only register MAME hands to a device (`DAC_16BIT_R2R_TWOS_COMPLEMENT`);
`emu/gpce4.cpp` now integrates the held code into a 48 kHz ring - see
[docs/AUDIO.md](AUDIO.md) for the measurements and for the fact that this game never
writes any of the four registers.

### 3.4 Interrupt controller (:630-1015) — see §4 for bit meanings

| addr | name | read | write | reset | MAME |
|---|---|---|---|---|---|
| `3050` | INT_Ctrl | stored | stored, then `update_interrupts()` | `0x0000` | :630 / :635 |
| `3051` | INT_Status | `m_interrupt_status >> 16` | write-one clears source bits **16-31** | `0x0000` | :978 / :984 |
| `3052` | FIQ_SEL | stored | stored | `0x0000` | :994 / :999 |
| `3053` | INT2_Ctrl | stored | stored, then `update_interrupts()` | `0x0000` | :676 / :681 |
| `3054` | INT2_Status | `m_interrupt_status & 0xffff` | write-one clears source bits **0-15** | `0x0000` | :659 / :665 |
| `3055` | FIQ2_SEL | stored | stored | `0x0000` | :1006 / :1011 |

### 3.5 ADC (:1148-1176)

| addr | name | read | write | reset | MAME |
|---|---|---|---|---|---|
| `3070` | ADC_Ctrl | `0x0000` constant | logged | — | :1159 / :1165 |
| `3071` | ADC_Data | `0xffff` constant | logged | — | :1170 / :1176 |
| `3072` | ADC_LineIn_BitCtrl | `0x0000` constant | logged | — | :1148 / :1154 |

The toy has a battery ADC; MAME returns a fixed "no reading", so the charge state is
faked through IOB bit 4 instead (§2).

### 3.6 SPI2 / LCD (:1040-1141)

| addr | name | read | write | reset | MAME |
|---|---|---|---|---|---|
| `3090` | SPI2_Ctrl | stored | stored; bit `0x0800` empties the TX counter and clears TXFLEV/TXIF | `0x0000` | :1040 / :1045 |
| `3091` | SPI2_TXStatus | `0x0000` (MAME) — with `spi2_status_regs`: bits 3:0 fill, 7:4 TXFLEV, 14 TXIEN, 15 TXIF | write-one clears bit 15 | `0x0000` | :1085 / :1091 |
| `3092` | SPI2_TXData | write-only | `lcd.command_w(data & 0xff)` when IOB `0x0400` is clear, else `lcd.data_w(...)` | — | :1096 |
| `3093` | SPI2_RXStatus | `0x0000` constant | logged | — | :1052 / :1058 |
| `3094` | SPI2_RXData | `0x0000` constant (no panel read path wired) | — | — | :1103 |
| `3095` | SPI2_Misc | `0x0000` (MAME) — with `spi2_status_regs`: `0x0003` idle / `0x0001` busy | logged | — | :1131 / :1141 |

### 3.7 SPI1 / flash (:706-901)

| addr | name | read | write | reset | MAME |
|---|---|---|---|---|---|
| `30e0` | SPI_Ctrl | `m_spi_ctrl & ~0x0800` | stored; bit `0x0800` empties both FIFOs **and** resets the flash device | `0x0000` | :706 / :712 |
| `30e1` | SPI_TXStatus | unmapped (write-only register in MAME) | logged | — | :727 |
| `30e2` | SPI_TXData | write-only | enqueue `data & 0xff` into the 8-byte TX FIFO | — | :792 |
| `30e3` | SPI_RXStatus | `0x8000` constant | logged | — | :798 / :804 |
| `30e4` | SPI_RXData | pops the RX FIFO, `0x0000` + one warning on underflow | — | — | :809 |
| `30e5` | SPI_Misc | `0x0004` constant | logged | — | :843 / :722 |
| `30e6` | SPI_Man_Ctrl | `0x0000` constant | logged | — | :831 / :837 |
| `30e7` | SPI_Auto_Ctrl | stored | stored (`READ4` mode is accepted, never decoded) | `0x0000` | :867 / :873 |
| `30e9` | SPI_BANK | stored | stored — selects `+0x400000` in the direct window | `0x0000` | :895 / :901 |

---

## 4. Interrupt controller and CPU lines

One 32-bit pending word `m_interrupt_status` in the SoC. `request_interrupt(n)` sets bit
`n`, `clear_interrupt(n)` clears it, and both call `update_interrupts()`
(:964 / :971 / :907). `INT_Status` exposes bits 31:16, `INT2_Status` bits 15:0, and both
clear on **write one to a bit** (not write zero), i.e. the handler loops
`if (data & (1 << i)) clear_interrupt(i + 16)` / `clear_interrupt(i)`.

Sources raised by this port (bit index used by `request_interrupt`, and how it appears when
the guest reads `INT_Status` = bit-16):

| source bit | readback bit | enabled by | CPU line | rate | set by |
|---|---|---|---|---|---|
| 31 | `INT_Status` bit 15 | `INT_Ctrl` bit 15 | `UNSP_IRQ0_LINE` | 20 kHz | `timer_a` callback (:274) |
| 29 | `INT_Status` bit 13 | `INT_Ctrl` bit 13 | `UNSP_IRQ2_LINE` | 1 kHz | `timer_c` callback (:269) |
| 24 | `INT_Status` bit 8 | `INT_Ctrl` bit 8 | `UNSP_IRQ3_LINE` | 300 kHz | machine driver "timer" (ref/generalplus_gpce4.cpp:322-325, :387) |
| 9 | `INT2_Status` bit 9 | `INT2_Ctrl` bit 9 | `UNSP_IRQ6_LINE` | 2 kHz | `timer_2khz` callback (:289) |
| 2 | `INT2_Status` bit 2 | `INT2_Ctrl` bit 2 | `UNSP_IRQ7_LINE` | 64 Hz | `timer_64hz` callback (:284) |
| 0 | `INT2_Status` bit 0 | `INT2_Ctrl` bit 0 | `UNSP_IRQ7_LINE` | 2 Hz | `timer_2hz` callback (:279) |

`update_interrupts()` (:907-962) is a plain AND of pending and enable per line; the line
state is pushed through `cpu->execute_set_input(line, 0/1)` only on change.

**Known MAME bug, reproduced behind a switch.** MAME's `update_interrupts()` handles the
two IRQ7 sources with `if (…) … else …` twice, and the second `else` *clears* the line
again, so a pending 2 Hz interrupt alone can never reach the CPU. Default here is the
OR of both sources (`irq7_mame_quirk = false`); set `irq7_mame_quirk = true` for bit-exact
MAME behaviour when comparing traces.

---

## 5. Cycle-driven time base

The harness calls `Gpce4::tick(cycles)` between CPU slices; nothing is wall-clock based.
Periods are in 96 MHz CPU cycles (`ref/generalplus_gpce4.cpp:370`):

| source | period (cycles) | callback | remark |
|---|---|---|---|
| `timer_c` | 96000 | `timer_c_cb` (:269) | 1 kHz "game speed" |
| `timer_a` | 4800 | `timer_a_cb` (:274) | 20 kHz |
| `timer_2hz` | 48000000 | `timer_2hz_cb` (:279) | |
| `timer_64hz` | 1500000 | `timer_64hz_cb` (:284) | |
| `timer_2khz` | 48000 | `timer_2khz_cb` (:289) | |
| 300 kHz machine timer | 320 | driver timer → bit 24 | on by default, `timer_300k_enable = false` turns it off |
| `timer_spi_tx` | 0 then 2 | `timer_spi_tx` (:733) | one-shot per SPI1 byte |

`tick()` services the pending events **in time order** (seven-way minimum, ties broken by
source index), advances an internal `m_now`, and finally pushes the interrupt lines, so a
byte queued for the flash is clocked out at its scheduled cycle instead of the end of the
slice. `reset()` restarts every source at `m_now = 0`, `next = period`, which is why
`tick(96000)` produces exactly one 1 kHz tick and `tick(95999)` produces none.

`Wait_Ctrl` (`0x3035`) is recorded but not modelled: the unSP core in this project has no
HALT/WAIT state, and MAME does not model one either (:567 only logs).

---

## 6. SPI1 (flash) — `generic_spi_flash.cpp`

`Gpce4SpiFlash` is a straight port of MAME's `generic_spi_flash_device`, driven only through
`write(u8)` / `read()`. Machine wiring (ref/generalplus_gpce4.cpp:301-320):
`set_rom_ptr` + `set_rom_size`, `set_single_byte_status_read()` and, for digicolr,
`set_single_byte_status_writes()`.

Timing: an 8-byte TX FIFO and an 8-byte RX FIFO. `write_to_tx_fifo()` (:771) arms the
48 MHz shifter when the FIFO was empty — `adjust(attotime::zero, 0, from_hz(48 MHz))` means
the **first byte leaves immediately** and every following byte 2 CPU cycles later
(`TICKS_SPI_TX = 2`). `timer_spi_tx()` (:733) pops one byte, calls `m_spi.write()`, then
`recieve_spi_fifo_data(m_spi.read())` (:758). Overflow of either FIFO is reported and the
byte dropped (MAME `fatalerror`s; a toy emulator must not abort).

**MISO skew:** `get_command()` only changes state and zeroes `m_state_step` — it does not
load `m_latch`. The answer to a command byte therefore appears on the *next* clock. That is
why `spi_xfer()` in the selftest always costs one extra exchange, and why reading
`SPI_RXData` right after writing the command byte returns the stale latch.

Command set implemented (state machine, address phase is MSB first:
`addr = (addr & 0x00ffff) | data<<16`, then `(addr & 0xff00ff) | data<<8`, then
`(addr & 0xffff00) | data`):

| opcode | name | behaviour |
|---|---|---|
| `0x01` | WRSR | config byte (and status byte in multi-byte mode), then idle |
| `0x02` | PP | 3 address bytes; programs only when WEL is set — image is read-only here, the byte is logged and dropped, address still increments |
| `0x03` | READ | 3 address bytes, then `latch = rom[addr++]` (mod image size) |
| `0x04` | WRDI | clears WEL |
| `0x05` | RDSR | `latch = statusreg`; single-byte mode returns to idle (digicolr sets both single-byte flags) |
| `0x06` | WREN | sets WEL |
| `0x0b` | FAST_READ | READ + 1 dummy byte |
| `0x11` | (unknown in MAME) | swallows bytes, stays in that state |
| `0x15` | RDCR | `latch = configreg`, idle |
| `0x20` | SE | 3 address bytes only; **does not erase** (MAME logs and forgets) |
| `0x31` | (unknown) | swallows bytes |
| `0x35` | RDSR2 | `latch = statusreg`, idle |
| `0x66` | enable reset | accepted, ignored |
| `0x90` | REMS | 5 dummy bytes, idle |
| `0x99` | reset | accepted, ignored |
| `0x9f` | RDID | `fe fe 00`, then repeats the last id byte forever (MAME never returns to idle — reset the device before the next command) |
| `0xab` | RDP | accepted, ignored |
| `0xb9` | DP | accepted, ignored |
| `0xeb` | 4READ | READ + 3 dummy bytes |
| `0xec` | (unknown) | swallows bytes |
| `0xff` | CRMR | accepted, ignored |
| other | — | logged once as unhandled and ignored (MAME: `fatalerror`) |

`device_reset()` clears address/state/latch but keeps status and config, matching MAME.

---

## 7. SPI2 (LCD)

`spi2_txdata_w()` (:1096) is synchronous in MAME: `m_spi2_out(data & 0xff)` runs inside the
write, and the machine's `spi_from_soc()` (ref/generalplus_gpce4.cpp:344-354) picks the
destination from the latched port-B output:

```cpp
if (!(m_iob & 0x0400)) m_lcdc->lcdc_command_w(data);
else                   m_lcdc->lcdc_data_w(data);
```

This port keeps that timing (a byte reaches the panel in the same CPU instruction, so D/C
can never drift), but still maintains a 12-byte TX fill counter and the TXFLEV/TXIF bits so
the status register can be modelled. Because MAME answers `TXStatus`, `RXStatus`, `RXData`
and `Misc` with a hard `0` and the mask ROM polls exactly those registers (`0x3091` ×5,
`0x3093` ×5, `0x3095` ×6 static hits), the MAME answer is the default: **all SPI2 status
reads return 0**. `spi2_status_regs = true` exposes the real model
(`fill | TXFLEV<<4 | TXIF`, write-one-to-clear on bit 15, `Misc` = `0x0003` idle /
`0x0001` busy). A ROM that waits for `TXStatus & 0x8000` would deadlock with the default,
so keep the default unless a specific ROM is known to need the richer model.

`SPI2_Ctrl` bit `0x0800` empties the counter and clears TXFLEV/TXIF.

---

## 8. ST7735 (`ref/st7735_lcdc.cpp`)

State: a 256x256 word GRAM (`m_displaybuffer`, 9 bits of X), window registers
`posmin/posmax x/y`, `m_command`, `m_commandstep`, `m_madctl`, `m_pixellatch`. `reset()`
mirrors `device_start()` (:371) — everything zero **except `sleep = true`**. MAME's
`device_reset()` (:402) is empty, so in MAME the panel is only initialised once at machine
start; here `Gpce4::reset()` calls `lcd.reset()` so a harness reset gives a clean panel
(deviation 7 in §10).

`command_w()` (:62) sets the command and zeroes the parameter step; `data_w()` (:161)
consumes parameters and ends with `m_commandstep++` for *every* command.

| command | handling |
|---|---|
| `0x00` NOP | nothing |
| `0x01` SWRESET | window/registers cleared, `display_on = false`, `sleep = true`, **GRAM kept** (MAME logs "unknown" and ignores it — see §10) |
| `0x10` SLPIN / `0x11` SLPOUT | `sleep = true` / `false` |
| `0x12`, `0x13`, `0x20`, `0x21` | accepted (normal/idle mode, inversion on/off), no visual effect — the framebuffer is always RGB565 |
| `0x28` DISPOFF | `display_on = false` (MAME: unknown) |
| `0x29` DISPON | `display_on = true` |
| `0x2a` CASET | 4 parameter bytes → `posminx`, `posmaxx`; **`posx = posminx` after every byte** |
| `0x2b` RASET | 4 parameter bytes → `posminy`, `posmaxy`; **`posy = posminy` after every byte** |
| `0x2c` RAMWR | `m_pixellatch = (m_pixellatch << 8) \| d`; on every **odd** step the word is stored, `posx++`, wrap to `posminx`/`posy++`, wrap `posy` to `posminy` |
| `0x36` MADCTL | first parameter byte stored in `m_madctl` |
| `0x3a` COLMOD | first parameter byte recorded only (always 16-bit pixels) |
| `0x2d`, `0x30-0x35`, `0x37-0x39`, `0x3b-0x3f`, `0x50-0x63`, `0xb1-0xbe`, `0xc0-0xc8`, `0xd0-0xd6`, `0xe0/0xe1` | parameters swallowed into a 16-byte buffer (`m_params`); never interpreted |
| other | logged once per command byte |

Pixel placement (`store_pixel`, emu/st7735.cpp:95) is MAME's two-form mapping:

```
MV = MADCTL bit 5
MV set   : idx = posx + posy * 0x200
MV clear : idx = posy + (129 - posx) * 0x200      // 90 degree rotated panel
m_displaybuffer[idx & 0xffff] = pixel;
```

`fb[]` is the 128x128 clipped view of the same write (`rx = idx & 0x1ff`, `ry = idx >> 9`,
written only inside 0..127). It is updated **whether or not** the panel is on, so the harness
renders `fb[]` and gates on `lcd.display_on && !lcd.sleep`; MAME instead gates its
`render_to_bitmap()` (:30). Pixels outside the window are still written to GRAM, exactly as
MAME does — only the wrap arithmetic differs from the datasheet.

12-bit, 18-bit and 24-bit pixel formats are **not** implemented: `ref/st7735_lcdc.cpp` only
ever handles 16-bit words, and inventing a second format would diverge from the reference
that the ROM was validated against. `COLMOD` values other than `0x05` are recorded and
otherwise ignored, and 18-bit colour (`0x06`) is reported once, not fatal.

---

## 9. Deliberately not implemented

Every one of these is either absent from `internal_map` or present only as a comment in
MAME, so a standalone port cannot have behaviour to copy.

| address | name | reason |
|---|---|---|
| `300c-300e` | IOA/IOB/IOC_WakeUp_Mask | commented out at :170-172; wake-up is a sleep-mode feature |
| `3031`, `3032` | System_Reset, Reset_LVD_Ctrl | commented out at :189-190; a self-reset has no meaning without the LVD block |
| `3036` | System_Sleep | commented out at :194; the unSP core has no sleep state |
| `303a`, `303b` | Cache_Hit_Rate, Stack_INT_Level | commented out at :197-198 (debug readouts) |
| `3073`, `3074` | ADC_PGA_Ctrl, ADC_FIFO_Ctrl | commented out at :215-216; the ADC itself returns constants |
| `309a-309f` | SPI2 DMA block | commented out at :225-230; no DMA engine is modelled |
| `30b0-30b3` | BUF_ACTIVE / LINK_BUF* | commented out at :232-235 (ICE/link buffers) |
| `30d0-30d7` | CTS_* timer/capture block | commented out at :237-244 |
| `30e8` | SPI_PRGM_BC | commented out at :254 |
| `30ea-30ef` | SPI DMA block | commented out at :256-261 |
| `0x001000-0x002fff`, `0x010000-0x1fffff`, `0x400000+` | — | not mapped by MAME; read `0xffff`, writes dropped |
| PWM outputs, DAC audio, ADC input, watchdog reset, cache, sleep/wake | — | MAME logs and forgets them too; nothing the ROM needs to boot or draw |

---

## 10. Where this port deviates from MAME, and why

1. **SPI2 status registers** default to MAME's constant `0`; the real FIFO model is behind
   `spi2_status_regs` (§7). Chosen because the ROM polls them and was only ever validated
   against the constant-zero model.
2. **IRQ7 double else-clear** is fixed by default (`irq7_mame_quirk` restores it) — MAME's
   own comment-free bug would silently drop every 2 Hz interrupt (§4).
3. **`0x28` DISPOFF / `0x01` SWRESET / `0x21` INVON** are implemented; MAME falls into its
   "unknown command" log for them. A real panel has them, and they cost nothing.
4. **`fb[]` is written regardless of display state** (§8). MAME's `render_to_bitmap()`
   gates the picture instead; a standalone emulator needs the framebuffer available even
   before the ROM enables the display.
5. **FIFO overflow** is logged and dropped instead of `fatalerror` (`write_to_tx_fifo`,
   `recieve_spi_fifo_data`, unknown SPI command) — aborting is not an option in a toy.
6. **Unmapped accesses** are logged once per address, capped at 64 entries.
7. **`Gpce4::reset()` also resets the panel** (`lcd.reset()`). MAME's lcdc `device_reset()`
   is empty, so there the panel keeps its state across a SoC reset; resetting both keeps the
   two objects consistent for a harness that resets the machine.

Judgement calls where the MAME source is ambiguous (stated, not hidden):

- `Wait_Ctrl` (`0x3035`): MAME only logs it. Stored for inspection, no CPU stall. If the ROM
  ever relies on WFI, the unSP core would have to grow a HALT state first.
- The 300 kHz machine timer is a *driver* timer (`generalplus_gpce4.cpp:322`), not part of
  the SoC. It lives in `Gpce4::tick()` because the standalone build has no driver layer;
  `timer_300k_enable` switches it off.
- `spi_direct_r()` uses `spi_bank * 0x400000` on the **byte** index while the window offset
  is a word index (`offset * 2`). That is MAME's arithmetic verbatim; the selftest pins it
  with bank 0 → `flash[0x20]` and bank 1 → `flash[0x400020]`.
- `INT_Status`/`INT2_Status` write-only-to-clear is implemented per bit; writing `0` leaves
  the bit alone (MAME's loop only acts on set bits).
- ROM writes are logged and dropped; MAME's `rom()` region is read-only as well.
- **`SPI_Auto_Ctrl` "READ4" has no bit encoding in MAME.** The register comment
  (:849-865) defines `ADDR_LEN` bit 15, `ADDR_1BIT` bit 14, … and `AUTO_CMD` bits 3:0, and
  `spi_direct_r()` (:879-893) is documented to require READ4 mode (:881-884) but never tests
  the register. This port therefore only stores it, and the selftest writes `0x000b`
  (`AUTO_CMD = 0x0b`, i.e. the FAST_READ opcode) as the most plausible READ4 setting. The
  direct window works with or without it, so no ROM can tell the difference.

---

## 11. Build and run

```
powershell -NoProfile -ExecutionPolicy Bypass -File .\emu\build.ps1     # emu/digicore.exe
```

The selftest links the CPU core objects by hand. PowerShell does **not** expand
`emu/obj/isa_*.o` for a native command and `emu/obj` also holds `gpce4.o`, `st7735.o` and
`main.o`, which would clash with the sources we compile here, so build the object list
explicitly:

```powershell
$objs = Get-ChildItem emu\obj\*.o |
        Where-Object { $_.Name -like 'isa_*' -or $_.Name -eq 'unsp_glue.o' } |
        ForEach-Object { $_.FullName }
& C:\Users\DuWeisi\w64devkit\bin\g++.exe -std=c++20 -O2 -Wall -Wextra -fpermissive -I emu/compat -I emu `
    -o emu/gpce4_selftest.exe emu/gpce4_selftest.cpp emu/gpce4.cpp emu/st7735.cpp emu/audio_dump.cpp @objs
.\emu\gpce4_selftest.exe
```

Two things this command needs and the older text did not mention:

* `emu/audio_dump.cpp` — `gpce4.cpp` now includes it for the WAV capture path
  (docs/AUDIO.md), so every link that pulls in `gpce4.cpp` needs it too, including
  `emu/link_test.exe` if you build that by hand.
* `-fpermissive` — `gpce4_selftest.cpp:129` and `:374` pass the `const` result of
  `std::vector<uint8_t>::data()` to `set_images(..., uint8_t *flash, ...)`; with
  g++ 16 that is a hard error (`invalid conversion from 'const unsigned char*' to 'uint8_t*'`).
  The clean fix is declaring those two buffers non-const, the one-line workaround is
  `-fpermissive`.

In cmd.exe the single-line form from the task description works verbatim
(`... gpce4_selftest.cpp emu/gpce4.cpp emu/st7735.cpp emu/obj/isa_*.o emu/obj/unsp_glue.o`).

The selftest loads `roms/internal_rom_221017a.bin` (65536 B) and `roms/dmc_v1_base.bin`
(8388608 B), prints one `PASS:`/`FAIL:` line per assertion and returns the number of
failures as its exit code.
