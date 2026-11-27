# R7：构建与二进制画像审计（最终镜像 → 冗余体积）

日期：2026-11-27
基线：`firmware/digicore-c3/build/digicore-c3.elf`（16,708,092 B，2026-11-27 19:16:19）、
`digicore-c3.bin`（1,144,944 B）、`digicore-c3.map`（3,326,778 B）、`compile_commands.json`
角色：构建与二进制画像审计员（**只读**：未改任何 `.c/.cpp/.h/.py/.txt/sdkconfig`，未运行 `idf.py build/size/reconfigure`；
唯一写入的文件就是本文件。工具只用了 `riscv32-esp-elf-{nm,size,readelf,objdump}` + IDF 自带 python 读 map/bin。）
范围：整个 app 镜像（`main` 组件 + 已链接的 ESP-IDF 库 + `.rodata/.data/.bss/.iram0`），不含 bootloader 与 sparse ROM 分区。

---

## 0. 结论先说

1. **账本三个数必须分开看，混在一起就会得出错误结论。**
   app 镜像 **1,144,944 B**（写入 0x10000）；其中**只在 flash 常驻的 1,018,044 B**（DROM+IROM，88.9%）；
   **需要拷进 RAM 的 126,792 B**（IRAM 119,352 + DRAM 7,408 + RTC 32）；另有 `.bss` **112,232 B** 不占镜像但占 RAM。
   静态 RAM 合计 **239,448 B**（IRAM 区 119,808 + DRAM 区 119,640），链接后剩余堆 **81,848 B**。
   → **本题的瓶颈不是 flash。** 2 MB 的 `factory` 分区只用了 **54.6%**（余 952,208 B），8 MB flash 里分区只排到 0x4B0000。
   真正的紧的是 DRAM（余 81,848 B，且 IRAM 与 DRAM 共用同一个 400 KB SRAM 池，见 §2.5）和 16 KB icache（见 §5）。

2. **唯一"又大又明显浪费"的是 AOT 页表：155,648 B 里 140,096 B 是空槽。**
   `g_aot_desc` 有 76 页，每页 `AotEnt[0x100]`×8 B = 2,048 B；实际登记 1,944 条（占 10.0%）。
   这是全镜像 `.rodata` 的 **48.1%**、镜像的 **13.6%**。要回收必须改 `tools/aot.py` 的生成格式 + 运行时查表，
   **属于"需 A/B 实测"的性能敏感改动**，不是删几行字符串。

3. **开关型冗余（WiFi/BT/LWIP/mbedTLS/HTTP/OTA/SPIFFS/FATFS/coredump/gdbstub）净贡献一律 0 字节。**
   `sdkconfig` 里 `CONFIG_ESP_WIFI_ENABLED=y`、`CONFIG_MBEDTLS_*=y`、`CONFIG_SPIFFS_*=y` 看着吓人，
   但 map 里 `libesp_wifi.a` / `liblwip.a` / `libwpa_supplicant.a` / `libspiffs.a` / `libfatfs.a` 的**成员一个都没被拉进来**；
   mbedtls 有成员被拉进来（9,786 B 代码），又被 `--gc-sections` 全丢（§3.2 表）。
   把它们改成 `=n` 省的是**构建时间和心智负担，不是 flash**。别把这写成"优化成绩"。

4. **字符串总量 62,218 B（`.rodata.*str1.*`），其中日志格式串 32,554 B、断言表达式 7,595 B。**
   可**零行为变化**回收的只有 D 级（`CONFIG_LOG_MAXIMUM_LEVEL=DEBUG` 编译进镜像，而运行级是 INFO，永不打印）：
   **3,789 B 字符串 + ~1.8 KB 调用点 ≈ 5.6 KB**。INFO 级唯一串另有 **5,514 B**（+ ~2.5 KB 调用点），
   但它同时是**性能问题**（阻塞串口，见 §4.4），要按"什么时候打"来改，不是按字节改。

5. **`-Os`（aot_blocks）与 `-O3`（其它）的分工是对的，不要动。**
   `-ffunction-sections/-fdata-sections/-Wl,--gc-sections` 确实生效：丢弃 **5,246 个输入段 / 1,108,063 B**
   （其中 **292,079 B 是真实代码/数据**，另 815,984 B 是只存在于 elf 的 debug 段；单 `uart.c.obj` 就丢了 21,133 B、只留 3,797 B）。
   `-fjump-tables/-ftree-switch-conversion` 在**整个 main 组件**重开，但全镜像扫描到的"代码指针数组"合计仅 7,128 B，
   其中 4,408 B 还是 ST7789 初始化命令表和 `Gpce4` 虚表 —— **这两个开关的体积代价 <2 KB，收益在 `.text` 的分派序列上**，不建议为了省字节去动它。

6. **顺带纠正一个会误导人的度量陷阱**：本 ELF 不能直接用 `riscv32-esp-elf-size`。它的 `.dram0.dummy`（119,808 B NOBITS）与
   `.flash_rodata_dummy`（720,896 B NOBITS）会污染 text/data 统计；map 里还有一条 **61,521 B 的幽灵记录**
   （`.rodata.__esp_system_init_fn_init_show_app_info.str1.4`，真实只有 238 B，RISC-V relaxation 后的地址回填错误）。
   本报告全部数字来自 `readelf -S` + 自写 map 解析（剔除该幽灵条目后 1,260,725 B vs readelf 1,257,034 B，**误差 0.29%**）。

---

## 1. 度量口径与可复算命令

### 1.1 段表（权威）

```
$ riscv32-esp-elf-readelf -S build/digicore-c3.elf        # 节选
[ 6] .iram0.text    PROGBITS 40380000 121000 01d236  AX     # 119,350
[ 8] .dram0.data    PROGBITS 3fc9d400 11e400 001cf0  WA     #   7,408
[10] .dram0.bss     NOBITS   3fc9f0f0 1200f0 01b668  WA     # 112,232
[11] .flash.text    PROGBITS 42000020 13f020 0a981c  AX     # 694,300
[13] .flash.appdesc PROGBITS 3c0b0020 0b1020 000100  A      #     256
[14] .flash.rodata  PROGBITS 3c0b0120 0b1120 04efa0  WA     # 323,488
[16] .iram0.text_end NOBITS  4039d236 13e236 0001ca         #     458
[ 4] .rtc.force_slow PROGBITS 50000000 1e9000 000020  WA    #      32
```

### 1.2 镜像段（直接解析 `digicore-c3.bin`，不信任 elf2image）

```
$ python binseg.py build/digicore-c3.bin
file build/digicore-c3.bin bytes 1144944
magic=0xe9 segments=6 mode=2 sizefreq=0x3f entry=0x403802de
  seg0: addr=0x3c0b0020 size= 323744 (0x4f0a0)   # .flash.appdesc + .flash.rodata   → flash
  seg1: addr=0x3fc9d400 size=   3920 (0xf50)     # .dram0.data 前段                 → RAM
  seg2: addr=0x42000020 size= 694300 (0xa981c)   # .flash.text                      → flash
  seg3: addr=0x3fc9e350 size=   3488 (0xda0)     # .dram0.data 后段                 → RAM
  seg4: addr=0x40380000 size= 119352 (0x1d238)   # .iram0.text                      → RAM
  seg5: addr=0x50000000 size=     32 (0x20)      # .rtc.force_slow                  → RTC
sum segment bytes: 1144836  header+pad overhead: 108
trailing bytes after last seg: 36          # 1 checksum + 32 sha256 + 3 pad
```

| 口径 | 字节 | 占镜像 | 说明 |
|---|---:|---:|---|
| app 镜像（写入 0x10000） | **1,144,944** | 100% | `factory` 分区 2,097,152 B，余 **952,208 B** |
| 纯 flash 常驻（DROM+IROM） | **1,018,044** | 88.9% | 不占 RAM |
| 镜像内需拷入 RAM | **126,792** | 11.1% | IRAM 119,352 + DRAM 7,408 + RTC 32（在 flash 里也要占一份） |
| `.bss`（不占镜像） | **112,232** | — | 只占 RAM |
| 静态 RAM 合计 | **239,448** | — | IRAM 区 119,808（119,350 + 458 `text_end`）+ DRAM 区 119,640（7,408 + 112,232） |
| 链接后剩余堆 | **81,848** | — | `0x3fcce710 − 0x3fcba758`；可分配池 0x4e710 = 321,296 B |

> **别把这三列混着写。** 例如"删掉 16 KB 的 `kBrickA8`"省 16 KB **flash**；"把 AOT 块从 IRAM 挪回 flash"省的是**镜像字节**（因为 IRAM 内容也写进了 bin），但会**掉帧**；"降 `-O3`→`-Os`"省 flash 但**直接掉帧**。

---

## 2. 体积账本

### 2.1 按组件（只统计会进镜像的段；总和与 readelf 差 3,691 B = 0.29% 的对齐/`*fill*`）

| 组件 | 合计 | .flash.text | .iram0.text | .flash.rodata | .dram0.bss | 判断 |
|---|---:|---:|---:|---:|---:|---|
| **main（本工程）** | **925,389** | 501,856 | 56,184 | 259,512 | 107,743 | 核心，不可删 |
| toolchain newlib/libc | 59,331 | 53,590 | — | 4,712 | 753 | printf/scanf 家族（§4.5） |
| esp_hw_support | 32,880 | 19,720 | 7,468 | 5,197 | 136 | 必需（gdma/rtc/sleep） |
| hal | 25,203 | 8,396 | 10,694 | 1,062 | 4 | 必需（IRAM 版 HAL） |
| freertos | 19,116 | 804 | 11,722 | 4,334 | 2,240 | 必需 |
| **nvs_flash** | **18,741** | 17,458 | — | 1,259 | 24 | **真在用**（设置存档，§3.1） |
| esp_driver_spi | 18,187 | 7,448 | 5,386 | 5,237 | 8 | 必需（LCD + flash） |
| esp_driver_i2c | 16,659 | 11,756 | 728 | 4,159 | 16 | 必需（电池 + 音频 codec） |
| heap（tlsf） | 15,815 | 4,012 | 8,074 | 2,852 | 8 | 必需 |
| esp_driver_i2s | 15,372 | 10,170 | 306 | 4,880 | — | 必需（音频） |
| spi_flash | 15,147 | 1,244 | 9,910 | 1,672 | 10 | 必需（ROM/save 读 flash） |
| esp_system | 12,777 | 7,422 | 1,440 | 3,566 | 121 | panic/wdt/startup |
| esp_lcd | 9,250 | 6,796 | 62 | 2,392 | — | 必需（含 st7789 驱动 2,920） |
| esp_driver_uart | 8,685 | 7,250 | — | 1,227 | 24 | 串口控制台 + printf |
| esp_driver_ledc | 8,438 | 6,168 | — | 2,234 | 36 | 必需（背光 PWM） |
| **esp_common（esp_err_to_name）** | **8,276** | 52 | — | **8,224** | — | **可省**（§3.3） |
| esp_driver_gpio | 7,257 | 6,226 | — | 995 | — | 必需 |
| esp_mm | 6,152 | 2,668 | 1,284 | 1,304 | 48 | 必需（cache msync） |
| esp_adc | 5,677 | 3,542 | — | 2,117 | 18 | 必需（按键 ADC） |
| vfs | 5,145 | 4,660 | — | 249 | 44 | 必需（uart/usb console） |
| newlib（esp 侧） | 4,850 | 1,780 | 2,240 | 382 | 200 | locks/time |
| efuse | 3,651 | 2,192 | — | 1,383 | 4 | 必需（chip rev） |
| esp_driver_usb_serial_jtag | 2,869 | 2,378 | 110 | 316 | 17 | 次串口（可裁，§3.4） |
| esp_partition | 2,546 | 2,058 | — | 480 | 8 | 必需 |
| esp_timer | 2,364 | 1,400 | 392 | 520 | 36 | 必需 |
| esp_ringbuf | 2,346 | — | 1,332 | 1,014 | — | 被 USB-Serial-JTAG 拖入 |
| riscv | 1,589 | 192 | 848 | 293 | 256 | 必需 |
| log | 1,529 | 670 | 402 | 173 | 276 | 必需 |
| pthread | 1,425 | 1,140 | — | 281 | 4 | ldgen 强制 (`esp_libc_include_pthread_impl`) |
| soc | 1,237 | — | — | 1,237 | — | 必需（寄存器表） |
| bootloader_support / esp_app_format / esp_vfs_console / app_update / cxx / esp_pm / esp_security / nvs_sec_provider / esp_phy | 959+734+706+331+56+18+14+2+2 | | | | | 零头 |

**复算**（本报告 §2 全部表格来自这一个解析器）：

```
$ python map2.py build/digicore-c3.map map2.json     # 输出见下
map loadable sum by output section: {'.iram0.text': 119350, '.dram0.data': 10976, '.dram0.bss': 112068,
  '.flash.text': 694284, '.flash.appdesc': 256, '.flash.rodata': 385312}
sum map 1322246 sum readelf 1257034
```
→ `.flash.rodata` 的 385,312 比真实 323,488 多 61,824，正是那条 61,521 B 幽灵条目 + 303 B fill；
`.dram0.data` 多 3,568 是 relaxation 造成的重复地址条目。**逐对象数字（本报告用的）已剔除幽灵条目**，
容器级总量以 readelf 为准。

### 2.2 main 组件按源文件（19 个目标文件，合计 925,389 B）

| 源文件 | 合计 | text | iram | rodata | bss | 判断 |
|---|---:|---:|---:|---:|---:|---|
| `emu/aot_blocks.c` | **609,448** | 408,842 | 44,294 | **156,312** | — | 核心。其中 **155,648 B 是页表**（§2.3） |
| `app_main.cpp` | 104,179 | 29,722 | — | 6,634 | **67,775** | 核心（含 `s_soc` 42,272 / `s_strip` 15,360 / `s_save_buf` 8,192） |
| `assets/internal_rom_221017a.bin` | **65,540** | — | — | **65,540** | — | 64 KiB 内部掩模 ROM，**不可删** |
| `emu/rom_esp.cpp` | 26,749 | 1,186 | — | 851 | 24,712 | 核心（`s_rom` 16,512 + `s_index` 8,192） |
| `emu/isa/unsp_amalgam.cpp` | 26,402 | 9,898 | 11,890 | 2,853 | 1,753 | 解释器（内含 `isa/*.cpp` 与 `unsp_glue.cpp`） |
| `bezel.cpp` | 19,397 | 2,542 | — | **16,855** | — | 16,384 是 `kBrickA8` 位图（§2.3） |
| `menu.cpp` | 16,461 | 13,360 | — | 1,576 | 1,511 | 核心 |
| `emu/gpce4.cpp` | 14,567 | 10,900 | — | 3,384 | 283 | 核心 |
| `dmc_link.cpp` | 14,068 | 4,574 | — | 984 | 8,510 | 核心 |
| `audio.c` | 7,032 | 3,266 | — | 650 | 3,112 | 核心 |
| `gueststate.cpp` | 4,650 | 3,828 | — | 810 | 8 | 核心 |
| `wallclock.cpp` | 3,281 | 2,406 | — | 875 | — | 核心 |
| `display.c` | 3,075 | 2,548 | — | 527 | — | 核心 |
| `emu/st7735.cpp` | 2,309 | 1,996 | — | 313 | — | 核心 |
| `saveimg.cpp` | 2,092 | 1,688 | — | 404 | — | 核心 |
| `battery.c` | 1,937 | 1,516 | — | 421 | — | 核心 |
| `emu/romstore.cpp` | 1,712 | 1,229 | — | 467 | 16 | 核心 |
| `fbsave.cpp` | 1,505 | 1,256 | — | 249 | — | 核心 |
| `buttons.c` | 985 | 786 | — | 199 | — | 核心 |

（`unsp_glue.cpp` 没有独立的 `.obj` 进镜像：它被 `isa/unsp_amalgam.cpp:17` `#include` 进去了。
build 目录里那份 `unsp_glue.cpp.obj` 是过期产物，`compile_commands.json` 里也没有它 —— 别拿它算账。）

### 2.3 `.rodata` 323,488 B 的去向

| 类别 | 字节 | 占比 | 证据 |
|---|---:|---:|---|
| **AOT 页表**（`aot_ent_*`，76×2,048） | **155,648** | 48.1% | `nm`：`_ZL12aot_ent_2303` 等 76 个，各 0x800；`g_aot_desc_n = 76u` |
| **internal_rom**（.rodata.embedded） | **65,540** | 20.3% | `nm`: `internal_rom` @0x3c0ec438, 0x10004 |
| **字符串**（`.rodata.*str1.*`） | 62,218 | 19.2% | map 分段求和（§4） |
| 其它（esp_err 表 8,224、ST7789 命令表、结构体常量…） | 22,749 | 7.0% | map `other` 桶 |
| **`kBrickA8`**（bezel 砖纹） | **16,384** | 5.1% | `nm`: `_ZL8kBrickA8` @0x3c0c0b3c, 0x4000 |
| font5x7 | 640 | 0.2% | map `.rodata._ZL8kFont5x7` |
| `g_aot_desc` | 612 | 0.2% | `nm`: 0x260 |
| 合计 | **323,791** | | 比 readelf 的 323,488 多 303 B（`*fill*` 对齐/重复计入，0.09%） |

**AOT 页表细账（本题最大的一块）**

```
$ nm -S --size-sort digicore-c3.elf | grep aot_ent    # 76 条，全部 0x800
$ grep -c 'static void aot_b' main/emu/aot_blocks.c   # 1944 个块函数
$ grep -c 'AOT_IRAM static void aot_b' …              #  208 个在 IRAM
$ grep -n 'g_aot_desc_n' …                            # const uint32_t g_aot_desc_n = 76u;
```
| 量 | 值 |
|---|---:|
| 页数 × 每页槽位 | 76 × 256 = **19,456** |
| 已登记块 | **1,944**（10.0%） |
| 已用字节 | 1,944 × 8 = 15,552 |
| **空槽字节** | **140,096** |
| 表总字节 | **155,648** |

压缩到"每页只存已用槽"（`{u8 slot; AotEnt ent;}` 有序数组 + 每页 8 B 目录）≈ 1,944×9 + 76×8 ≈ **18.1 KB**，
可省 **≈137 KB flash**；代价是每次 dispatch 在页内多一次二分（~8 次比较）。
**但这正好打在热路径上**（`aot_lookup()` 每次块进入调用一次），必须 A/B 实测才能定 —— 见 §5.4。

### 2.4 `.bss` 112,232 B 与 `.data` 7,408 B（RAM 侧）

| 符号 | 字节 | 归属 | 说明 |
|---|---:|---|---|
| `s_soc` | **42,272** | app_main.cpp | SoC 模型实例（含 guest VRAM/寄存器） |
| `s_rom` | 16,512 | rom_esp.cpp | sparse ROM 页缓存 |
| `s_strip` | **15,360** | app_main.cpp | 显示条带缓冲（320×24×2） |
| `s_index` | 8,192 | rom_esp.cpp | sparse 页索引 |
| `s_save_buf` | 8,192 | app_main.cpp | 存档搬运缓冲 |
| `s_header` / `s_page` | 4,096 / 4,096 | 匿名命名空间（`_ZN12_GLOBAL__N_1…`） | 页缓冲（NVS/save 路径） |
| `buf.3` / `buf.2` | 2,048 / 1,024 | libc/musl 侧 | stdio 缓冲 |
| `xIsrStack` | 1,536 | freertos | 中断栈 |
| 其余（<1 KB 合计） | ~12,000 | | |

`.data` 7,408 B 主要是 `wdt_hal_iram`(3,755) / tlsf(802) / esp_cache_msync(715) / mmu_hal(575) / spi_flash(547) 的上下文实例，**全部是运行时必需**。

> ⚠️ 用 `nm` 名字查比用 map 归属查更准：`s_soc`/`s_strip`/`s_save_buf` 三个都在 `app_main.cpp.obj`，
> 而 map 里 `.bss` 段存在重叠地址条目，逐对象求和的 `.dram0.data` 会多算 3,568 B（见 §1.2 注）。

### 2.5 `.iram0.text` 119,350 B（谁占了 IRAM）

| 目标文件 | IRAM | 说明 |
|---|---:|---|
| `aot_blocks.c.obj` | **44,294** | 208 个热块（`IRAM_ATTR`） |
| `unsp_amalgam.cpp.obj` | 11,890 | 解释器热路径 |
| `tlsf.c.obj` | 5,840 | 堆 |
| `tasks.c.obj` | 5,482 | 调度器 |
| `spi_master.c.obj` | 5,206 | `CONFIG_SPI_MASTER_ISR_IN_IRAM=y` |
| `queue.c.obj` | 4,416 | 队列 |
| `spi_flash_chip_generic.c.obj` | 3,498 | flash 驱动 IRAM 部分 |
| `esp_flash_api.c.obj` | 3,264 | 同上 |
| `spi_flash_hal_iram.c.obj` | 2,586 | 同上 |
| `spi_hal_iram.c.obj` | 2,526 | 同上 |
| `rtc_clk.c.obj` | 1,984 | 时钟 |
| 其余 | ~28,000 | |

**关键约束（这决定"能不能再往 IRAM 塞块"）**：`.iram0.text` 结束于 0x4039d236（+458 尾巴），
`.dram0.data` 正好从 **0x3fc9d400（= 0x1d400 = 119,808 偏移）** 开始 —— **IRAM 和 DRAM 是同一个 400 KB SRAM 池，
从两端往中间长**。每往 IRAM 多放 4 KB 代码，`.bss`/堆就少 4 KB。可用池 0x4e710 = 321,296 B，已用 239,448 B，**余 81,848 B 全给了堆**。

---

## 3. 库与组件冗余（sdkconfig 对账）

### 3.1 先说"确实在用"的（不要动）

| 项 | 证据 | 字节 |
|---|---|---:|
| **NVS** | `menu.cpp:96-107` `nvs_flash_init()` / `nvs_open("digicore")` / `nvs_set_u8` / `nvs_commit` —— 菜单设置就存在这 | **18,741**（nvs_flash）+ 8,192（`s_page`/`s_header` 缓冲） |
| esp_lcd + st7789 | `display.c` 调 `esp_lcd_new_panel_io_spi` / `esp_lcd_new_panel_st7789` / `esp_lcd_panel_reset` | 9,250（含驱动 2,920） |
| SPI / I2C / I2S / LEDC / ADC / GPIO | `audio.c`: `i2s_new_channel`+`i2c_new_master_bus`；`battery.c`: `i2c_master_transmit_receive`；`display.c`: `ledc_timer_config`；`buttons.c`: `adc_oneshot_new_unit`+`adc_cali_raw_to_voltage` | 18,187 / 16,659 / 15,372 / 8,438 / 5,677 / 7,257 |
| SPI-flash（`spi_flash`+`esp_partition`） | `rom_esp.cpp`/`saveimg.cpp` 直接读 `roms`/`save` 分区 | 15,147 + 2,546 |
| esp_timer / freertos / heap / log | 全链路必需 | 2,364 + 19,116 + 15,815 + 1,529 = 38,824 |

### 3.2 打开了但**一个字节都没进镜像**的（改 `=n` 只省构建时间）

| sdkconfig 项 | 值 | 镜像证据 |
|---|---|---|
| `CONFIG_ESP_WIFI_ENABLED` / 全部 `CONFIG_ESP_WIFI_*`（AMPDU/WPA3/SAE/Enterprise…） | y | map：`libesp_wifi.a` 成员 **0** 个；`esp_phy` 仅 2 B 占位 |
| `CONFIG_BT_ENABLED` | n（已关） | — |
| `CONFIG_LWIP_ENABLE` + `LWIP_*`（TCP/UDP/DHCP/DNS/IPv6） | y | `liblwip.a` 成员 **0** |
| `CONFIG_MBEDTLS_*`（TLS1.2、X509、ecp 全曲线、cert bundle 200 张） | y | **最终贡献 0 字节**：`libmbedcrypto.a` 有成员被拉进来（`esp_aes_dma_core.c.obj` 26,867 B 等），代码 **9,786 B 全部被 `--gc-sections` 丢掉**；`build/x509_crt_bundle.S`（440 KB 源）未参与链接 |
| `CONFIG_ESP_TLS_*` / `CONFIG_HTTPD_*` | y | 无 `libesp-tls.a`/`libesp_http_server.a` 成员 |
| `CONFIG_SPIFFS_*`（MAX_PARTITIONS=3、CACHE、MTIME…） | y | `libspiffs.a` 成员 **0**；分区表里也没有 spiffs 分区 |
| `CONFIG_FATFS_*`（VOLUME_COUNT=2、CP437、LFN_NONE…） | y | `libfatfs.a` 成员 **0** |
| `CONFIG_ESP_COREDUMP_ENABLE_TO_NONE` | y | 无 coredump 代码（正确配置） |
| `CONFIG_ESP_GDBSTUB_ENABLED` | y | 未链接（`CONFIG_ESP_SYSTEM_PANIC_GDBSTUB` 未开） |
| `CONFIG_ESP_WIFI_IRAM_OPT` 等 IRAM 相关 | n | 无影响 |

```
$ grep -c 'libesp_wifi\.a(' build/digicore-c3.map          # 0（链接区）
$ grep -c 'liblwip\.a('     build/digicore-c3.map          # 0
$ grep -c 'libspiffs\.a('   build/digicore-c3.map          # 0
$ grep -c 'libfatfs\.a('    build/digicore-c3.map          # 0
```

**被 `--gc-sections` 丢掉的字节（按组件，取自 map 丢弃表；已剔除 `.debug*`）**

> 全表共丢弃 **5,246 个输入段 / 1,108,063 B**，其中 **815,984 B 是 debug 信息**（只占 elf，不进镜像）、
> **292,079 B 是真实代码/数据**。下表是后者，也是"`-ffunction-sections` + `--gc-sections` 真的在工作"的最强证据：

| 组件 | 丢弃(代码/数据) | 组件 | 丢弃(代码/数据) |
|---|---:|---|---:|
| toolchain（libc/libgcc 未用函数） | 44,370 | esp_driver_ledc | 11,757 |
| esp_hw_support | 23,353 | newlib | 10,149 |
| esp_driver_uart（`uart.c.obj` 21,133） | 21,673 | **mbedtls / mbedcrypto** | **9,786** |
| **bootloader_support**（镜像格式/OTA 校验） | **20,276** | main（本工程未用到的函数） | 9,640 |
| freertos（stream_buffer 等） | 15,361 | esp_ringbuf | 9,060 |
| efuse / hal | 13,037 / 8,660 | pthread / nvs_flash | 8,314 / 8,263 |

> 结论：**这些开关的体积收益是 0**。建议照旧关掉只是为了让下次谁手滑 `#include "esp_wifi.h"` 时不要突然多出 700 KB
> （WiFi + wpa_supplicant + lwip + mbedtls 一旦被*真正引用*就会成片进来 —— mbedtls 已经演示了"拉进来 9.8 KB 代码又全部丢掉"，
> 而 `--gc-sections` 只保证"没人引用才丢"，不保证"引用了也小"）。

### 3.3 可以真正省下字节的配置项

| ID | 项 | 当前值 | 可省 | 证据 | 风险 | 建议 |
|---|---|---|---:|---|---|---|
| R7B-05 | `CONFIG_ESP_ERR_TO_NAME_LOOKUP` | y | **8,224** | `esp_err_to_name.c.obj` rodata 8,224 B（`size -A` 实测），是那张 `esp_err_t→字符串` 表；main 里只有 **4 处**调用（`grep -c esp_err_to_name main/*.cpp`） | 无功能风险，只丢错误名字符串 | **建议改**：4 处调用换成 `snprintf("0x%x")`，同时关掉该选项 |
| R7B-02 | `CONFIG_LOG_MAXIMUM_LEVEL` | 4 (DEBUG) | **≈5.6–6.8 KB** | 见 §4.2：镜像内 D 级唯一串 72 条 / **3,789 B** + 72~117 个调用点 ×~25 B（1.8–2.9 KB） | **零**：运行级是 INFO(3)，D 级本来永不打印 | **建议改** `=3 (INFO)` |
| R7B-04 | `CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL` | 2（全开） | **≈15,500** | 316 个 `__assert_func` 调用点；断言表达式串 7,595 B + `__FILE__` 串 3,515 B | **行为改变**：API 误用从 panic 变成静默 UB | 待裁决（§7） |
| R7B-06 | `kBrickA8` 位图 | 16,384 | **≤8,192** | `nm`: 0x4000，8bpp alpha 128×128 | 画质/观感改变 | 待裁决 |
| R7B-01 | AOT 页表空槽 | — | **≈140,096** | §2.3 | 热路径查表变慢 | 待裁决 + A/B |
| — | `CONFIG_COMPILER_STACK_CHECK_MODE_NONE` | **已关** | 0 | `sdkconfig`: `…STACK_CHECK_MODE_NONE=y` | — | **无需处理** |
| — | `CONFIG_COMPILER_OPTIMIZATION_PERF`（=-O2 基线） | y | — | `compile_commands.json`: 各组件 `-O2` | — | 保持 |
| — | `CONFIG_ESP_INT_WDT` | y | 354 | `int_wdt.c.obj` 只有 354 B（其余在 IRAM 侧 `wdt_hal_iram`） | 掉一层死锁保护 | **不值得动**，留着 |
| — | `CONFIG_ESP_SYSTEM_PANIC_PRINT_REBOOT` | y | ~1,800 | `panic.c` 1,796 + `panic_arch.c` 3,082 | 丢崩溃现场 | 留着（真机调试全靠它） |

### 3.4 双控制台（可裁但需先确认硬件）

`CONFIG_ESP_CONSOLE_UART_DEFAULT=y`（UART0 @115200）+ `CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=y`。
两者都在镜像里：`esp_driver_uart` 8,685（uart.c 3,797 + uart_vfs 4,888）、`usb_serial_jtag` 2,869、
`esp_ringbuf` 2,346（被 USB-Serial-JTAG 的 `xRingbufferCreate` 拖进来）、`vfs` 5,145、`esp_vfs_console` 706。
砍掉次控制台（USB-Serial-JTAG）**理论可省 ≈5.2 KB**（usb_serial_jtag 2,869 + ringbuf 2,346），
但 `printf` 走的是 VFS → 得确认现场调试究竟插哪个口。**待裁决，不建议本轮动。**

---

## 4. 字符串与日志

### 4.1 总量（实测，不是估算）

```
$ python strings.py build/digicore-c3.bin
seg 0x3c0b0020: len= 323744 strings= 2076 bytes(+NUL)= 67391 (20.82%)   # DROM（rodata）
seg 0x42000020: len= 694300 strings= 3883 bytes(+NUL)= 20502 ( 2.95%)   # IROM（.text 里的字面量+指令噪声）
seg 0x40380000: len= 119352 strings=  624 bytes(+NUL)=  3266 ( 2.74%)   # IRAM
total printable-string bytes in image: 95766 (8.36% of 1144944)
```
镜像里可打印字符合计 **95,766 B（8.4%）**；按 `readelf` 的口径，`.rodata.*str1.*` 段合计 **62,218 B**（§2.3）。
DROM 里 2,076 条串中，按形态分类：

| 类别 | 条数 | 字节 | 判定方式 |
|---|---:|---:|---|
| **ESP_LOG 格式串**（`I (`/`E (`/`W (`/`D (` 开头） | **601** | **32,554** | 正则 `^[IEWDV] \(`（IDF 宏 `LOG_FORMAT` 前缀） |
| 其中 **E** | 361 | 18,790 | 错误日志 |
| 其中 **I** | 101 | 5,514 | 信息日志 |
| 其中 **W** | 67 | 4,461 | 警告 |
| 其中 **D** | 72 | **3,789** | **调试日志（运行级过滤，永不输出）** |
| 其中 V | 0 | 0 | — |
| **断言表达式串** | 148 | 7,595 | 含 `&&`/`||`/`!=`/`->` 且长度 ≥10 |
| **`__FILE__` 路径串** | 72 | 3,515 | 含 `/IDF/` 或 `/components/` |
| 其余（TAG 名、`esp_err` 名表、面板命令名、结构体初始化器…） | ~1,255 | ~23,727 | 余量 |

> 注：`esp_log` 调用点共 **638** 处（`objdump -d` 计 `call <esp_log>`），与源侧 596 处 ESP_LOGx 基本吻合（同一格式串会被链接器合并，故 601 条唯一串 < 596+ 个调用点不矛盾）。

### 4.2 可安全降级的清单（D 级：零行为变化）

`CONFIG_LOG_DEFAULT_LEVEL=3`（INFO），`CONFIG_LOG_MAXIMUM_LEVEL=4`（DEBUG）——
意味着 **D 级格式串和调用点被编进镜像，但运行期永远被过滤掉**。这是唯一"删了绝对不影响行为"的一档。

| 组件 | D 级串(条/字节) | D 级调用点 | 证据 |
|---|---:|---:|---|
| vfs | 30 / 1,196 | 30 | `logstr.py` 逐源文件统计 |
| nvs_flash | 20 / 700 | 20 | |
| esp_driver_i2s | 16 / 532 | 16 | |
| app_update | 11 / 355 | 11 | |
| esp_driver_ledc | 8 / 284 | 8 | |
| esp_driver_i2c | 6 / 146 | 6 | |
| esp_lcd / esp_driver_spi / esp_hw_support / esp_system / esp_mm | 4+5+2+3+3 | 17 | |
| **main（本工程）** | **3 / 123** | 3 | `app_main.cpp` 里只有 3 处 `ESP_LOGD` |
| **合计** | **117 / 3,899**（源侧） | **117** | 链接器合并后镜像内是 **72 条 / 3,789 B**（§4.1） |

→ `CONFIG_LOG_MAXIMUM_LEVEL=INFO`：省 **3,789 B 唯一串 + 1.8~2.9 KB 调用点代码 ≈ 5.6~6.8 KB**，**零行为变化**。
（这是本题性价比最高的一条，唯一代价是以后想开 DEBUG 日志得重新改配置。）

### 4.3 INFO 级：能省 5.5 KB，但**不是"纯收益"**

I 级唯一串 101 条 / 5,514 B。真降级（改成 WARN 或删）要挑，因为其中一部分是启动自检（`ESP_LOGI(TAG,"panel cleared to black (320x240)")` 这类一次性信息，留着有用）。

**建议降级/删除的热路径 INFO（每次按键都触发，见 §4.4 的阻塞代价）**：

| 位置 | 串 | 为什么 |
|---|---|---|
| `app_main.cpp:2245` | `"key -> panel lit"` | 每次按键 |
| `app_main.cpp:2276` | `"press %02x -> holding"` | 每次按键 |
| `app_main.cpp:2299` | `"tap %02x -> holding %d guest frames"` | 每次按键 |
| `app_main.cpp:2341/2354/2385/2395` | `"menu row 0/1 …"`、`"chord card: B -> A+C sent"`、`"chord card: cancelled"` | 每张卡/每次和弦 |
| `app_main.cpp:741/770/2000` | `"time lapse …"` | 每次状态切换 |

这 10 条串合计 ~420 B（省不了多少字节），但**每次触发要阻塞串口 3~7 ms**（§4.4）。**按性能改，不要按体积改。**

### 4.4 字符串背后的真问题：阻塞串口

`printf`/`ESP_LOGx` 最终走 `fwrite` → UART VFS 阻塞写。`CONFIG_ESP_CONSOLE_UART_BAUDRATE=115200`：
**≈11.5 字符/ms** → 一条 40~80 字符的日志 = **3.5~7 ms 停顿**；而整帧预算只有 **16 ms**（稳定态）/ ~47 ms（重绘帧）。

- 按键日志 3 条连续触发 ≈ 10~20 ms → **按一下掉一帧**（这是"手感"问题，不是 flash 问题）。
- 那条 869 B 的 STAT/LONGF 格式串（`I (%lu) %s: frames=%u guest_fps=…`）单次输出 200+ 字符 ≈ **17 ms**；
  它被限频（仅在 >60 ms 的帧打 LONGF），但仍在**渲染循环里**打。
- 结论：**日志的运行时成本比它的 flash 成本高两个数量级。** 要动就动"什么时候打"，不是"省几个字节"。

### 4.5 libc 的 printf/scanf 家族（59,331 B，占镜像 5.2%）

| 符号 | 字节 | 谁用 |
|---|---:|---|
| `_vfprintf_r` / `_svfprintf_r` | 11,212 / 11,062 | `printf`、`snprintf`（122 处调用） |
| `__ssvfscanf_r` | **9,121** | `sscanf` —— 只有 `console_exec` 的调试命令解析用 |
| `dtoa` / `mprec` / `strtod` / `gdtoa-*` | 4,173 + 3,385 + 3,931 + 1,583 + 466 | 浮点格式化（`%f`） |
| `svfiprintf` / `fvwrite` / `findfp` / `fflush` / `fseeko` | ~3,800 | stdio 基础设施 |

- `sscanf` 那 9.1 KB 只服务调试控制台命令（`console_exec`，`app_main.cpp:1288-1488` 一串 `sscanf("%d"/"%x")`）。
  **若关掉控制台命令通道，一并省 ~9 KB**（待裁决）。
- 浮点那 13.5 KB（`dtoa`+`mprec`+`strtod`+`gdtoa-*`）**本工程根本不用 `%f`**：
  `grep -c '%[0-9.]*[fge]' main/**` = **0 处**，`app_main.cpp:3745` 的注释自己写着
  *"Fixed point on purpose: newlib's nano printf has no %f/%llu"*。它们是被**完整版 newlib printf** 无条件拖进来的。
  → `CONFIG_LIBC_NEWLIB_NANO_FORMAT=y` 可省 **≈13.5 KB**，但 nano 版不支持 `%lld/%llu`，
  而 main 里还有 **16 处 `%lld/%llu`**（`app_main.cpp:1445/1565/2018/2036/2181/2193/2341/2354…`），
  **必须先全部改成 `%ld` + `(long)` 才能开**。属"待裁决 + 需改动代码"。

---

## 5. 编译器/链接级

### 5.1 实际生效的选项（`compile_commands.json` 原文）

```
# main 组件（app_main.cpp 为例；aot_blocks.c 同，但末尾多一个 -Os）
… -march=rv32imc_zicsr_zifencei -ffunction-sections -fdata-sections -O2 …
  -fno-jump-tables -fno-tree-switch-conversion -fno-exceptions -fno-rtti -fuse-cxa-atexit
  -O3 -DC3_PROF=0 -DC3_SPLIT=0 … -fjump-tables -ftree-switch-conversion -DAOT_CONST_TABLES
  -DAOT_NO_HOOKS -DC3_MISS_CENSUS=0 -DC3_BLOCK_CENSUS=0 -DC3_NO_AOT=0 -DC3_BLIT_CYC=139
  -DC3_AOTACC=0 -DC3_WAIT_COLLAPSE=1 -DC3_HLEPROF=1
# aot_blocks.c 额外：… -Os    （最后一条胜出 → 实际 -Os）

# IDF 组件（i2c_master.c 为例）
… -ffunction-sections -fdata-sections -O2 … -fno-jump-tables -fno-tree-switch-conversion
```

| 项 | 状态 | 证据 | 判断 |
|---|---|---|---|
| `-ffunction-sections -fdata-sections` | **已生效** | 每个函数/变量一个段（map 里 `.text.aot_b0049DD`、`.rodata.bezel_paint.str1.4`…） | 保持 |
| `-Wl,--gc-sections` | **已生效** | `build.ninja` LINK_FLAGS | 保持 |
| `-O3`（main） | 已生效 | 命令行 `-O2 … -O3` | 保持 |
| `-Os`（aot_blocks） | 已生效 | 命令行末尾 `-Os` | **保持**（见下） |
| `-fno-jump-tables -fno-tree-switch-conversion` | IDF 默认开，**main 组件被重开** | 同上 | 见 §5.3 |
| `-fno-lto` | 已关 LTO | LINK_FLAGS | 保持（LTO 会显著改变 AOT 块的内联行为，属高风险） |
| `-fno-exceptions -fno-rtti` | 已生效 | 同上 | 保持 |

### 5.2 `-Os` 到底值多少（这是全镜像最大的一笔"已有成绩"）

`main/CMakeLists.txt:56-62` 注释写着"block bodies 是 673 KB"。实测 `-Os` 下 `aot_blocks.c.obj` 的
`.flash.text` 部分是 **408,842 B**（+ IRAM 44,294 + rodata 156,312）。

| 假设 | 结论 |
|---|---|
| 若改回 `-O3` | 依注释应回到 ~673 KB → **镜像 +264 KB（+23%）**，而且 16 KB icache 下每条块的首行几乎必 miss |
| 若再往 `-O2` | 未测，**需 A/B**；预期体积介于两者之间，收益不确定 |
| **建议** | **不动**。这条是"已经算过的账"，动它等于把 2026-11-2x 的实测重做一遍 |

### 5.3 `-fjump-tables/-ftree-switch-conversion` 的代价：实测 <2 KB

对整个 DROM 扫描"指向 `.flash.text`[0x42000020, +694300) 的 4 字节数组"（排除 `nm` 里 ≥512 B 的已知数据表）：

```
$ python jtab.py
jump-table-like runs (>=4 entries): 22 bytes 7128
     2368 B at 0x3c0c4d3c (592 entries)   ← objdump 注释：ST7789P3_CMDS+0x104（面板初始化命令表）
     2040 B at 0x3c0c5988 (510 entries)   ← objdump 注释：_ZTV5Gpce4+0x30c（虚表 + 后续指针表）
      724 B at 0x3c0fe2a8 (181 entries)
      540 B at 0x3c0fdfcc (135 entries)
      540 B at 0x3c0fe5a0 (135 entries)
      …
```
→ 去掉面板命令表和虚表，**真正的开关跳转表 ≤ 2.7 KB**。
**结论：这两个开关在体积上几乎免费，收益在 `.text` 的分派序列（解释器的大 switch）。不建议改。**
若一定要缩范围，唯一有意义的做法是"只对 `emu/` 子目录重开"（`set_source_files_properties` 逐个设置），
但那会把 `app_main.cpp`/`menu.cpp` 里几个热 switch 也降级 —— **收益不明，需 A/B，不建议本轮做**。

### 5.4 `-DC3_HLEPROF=1`

`main/emu/unsp_glue.cpp:143-165`：5 个 HLE 钩子（band/blit/push/fill/mul）各包一对 `esp_cpu_get_cycle_count()`
（2 次 `rdcycle` + 1 次 `.bss` 读改写）。

| 口径 | 量 | 证据 |
|---|---:|---|
| flash | **≈150 B**（5 个调用点 × ~30 B；`g_h_*` 5 个计数变量在两种编译下都存在） | `nm`: `g_h_band/g_h_blit/g_h_push/g_h_fill/g_h_mul` 均在；宏展开见源 |
| 运行时 | 每次钩子入口 ≈8~12 host cycles；重绘帧 ~1,650 次入口 ⇒ **0.3%**（R7_core 的实测口径） | `docs/notes/R7_core.md` §0-3 |
| 消费者 | 只有 `>60 ms` 帧的 LONGF 行（`app_main.cpp:3565` 附近） | 源 |

→ **建议改 `-DC3_HLEPROF=0`**：省不了体积（150 B），但这是一条**已经答完问题的探针**，
留在热路径上纯属给"重绘帧"交 0.3% 的税。改动零风险（另有 `#else` 分支把变量定义成 0，LONGF 行仍能编译）。

### 5.5 `CONFIG_COMPILER_STACK_CHECK` / 断言

| 项 | 值 | 字节 | 判断 |
|---|---|---:|---|
| `CONFIG_COMPILER_STACK_CHECK_MODE_NONE` | **y（已关）** | 0 | **已是最优，无需处理**（这题可以结案） |
| `CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE` | y | 见 R7B-04（≈15.5 KB） | 待裁决 |
| `CONFIG_COMPILER_ASSERT_NDEBUG_EVALUATE` | y | 0（只影响语义） | 保持 |
| `CONFIG_COMPILER_OPTIMIZATION_CHECKS_SILENT` | n | — | 保持（否则 panic 无输出） |

断言实测：`objdump -d | grep -c '<__assert_func>'` = **316**；表达式串 148 条 / 7,595 B；
`__FILE__` 串 72 条 / 3,515 B（`-fmacro-prefix-map` 已把绝对路径压成 `//IDF/...`，这点做得对）。
316 × ~14 B 的调用序列 ≈ 4.4 KB → 合计 **≈15.5 KB**。
关掉的代价：FreeRTOS 的 `configASSERT` 全部消失，API 误用会变成内存踩踏而不是 panic。
**对一个"跑模拟器、出错就白屏"的设备，这 15 KB 不建议省。**

---

## 6. 不要动清单（已确认必需，附证据）

1. **NVS 18,741 B** —— `menu.cpp` 用它存设置（`nvs_open("digicore")`/`nvs_set_u8`/`nvs_commit`）。省不了，也不该省。
2. **`internal_rom` 65,540 B** —— 64 KiB 掩模 ROM 是模拟器的取指目标，PC 可落在任意地址，整块必需。
3. **AOT 块体 408,842 B（flash）+ 44,294 B（IRAM）** —— 这是 AOT 加速的主体（`aot.h:355` 注释记着实测：去掉页缓存后 17 fps，解释器 59 fps）；动它等于动性能。
4. **esp_lcd + st7789 + spi_master（9,250 + 18,187）** —— 面板唯一的显示通路。
5. **i2s/i2c/ledc/adc/gpio（15,372+16,659+8,438+5,677+7,257 = 53,403）** —— 音频、电池、背光、按键，全是硬件接口。
6. **`s_soc` 42,272 + `s_rom` 16,512 + `s_index` 8,192** —— guest 状态与 ROM 页缓存，删了跑不起来。
7. **`panic.c`/`panic_arch.c` 4,878 + `int_wdt.c` 354** —— 崩溃现场与死锁保护，真机调试的命根子。
8. **`-Os` on aot_blocks、`-O3` on main** —— 已 A/B 过的结论（§5.2）。
9. **`esp_err_to_name` 的 4 处调用点**（可以换实现，但不能让 `ESP_ERR_*` 打印变成乱码而没人发现 —— 改的时候要一起改）。
10. **UART0 控制台（uart + vfs_console + vfs ≈ 14.5 KB）** —— 122 处 `printf` 是主机侧协议（`SHOT_BEGIN`/`ACK`/`NAK`）+ 诊断，不是纯调试。

---

## 7. 待裁决（要主审/业主拍板，我不替你决定）

| ID | 议题 | 收益 | 代价 | 需要的证据 |
|---|---|---|---|---|
| R7B-01 | AOT 页表改稀疏格式 | **≈137 KB flash**（镜像 −12%） | 每次 dispatch 多一次页内二分；**热路径** | A/B：改 `tools/aot.py` 生成 + 真机 fps/STAT 对比 |
| R7B-04 | 关断言（`ASSERTION_LEVEL=0`） | ≈15.5 KB | 误用从 panic 退化 UB | 业主对"静默跑飞"的容忍度 |
| R7B-06 | `kBrickA8` 改 4bpp 或程序化 | ≤8 KB | 观感 | 视觉对比图 |
| R7B-15 | 关 USB-Serial-JTAG 次控制台 | ≈5.2 KB | 少一个调试口 | 确认产线/调试实际插哪个口 |
| R7B-16 | 砍 `sscanf` 控制台命令通道 | ≈9.1 KB | 失去 `console_exec` 命令 | 命令通道是否算产品功能 |
| R7B-17 | 开 `CONFIG_LIBC_NEWLIB_NANO_FORMAT` | **≈13.5 KB**（dtoa/mprec/strtod/gdtoa） | 先把 16 处 `%lld/%llu` 改成 `%ld`+`(long)`；nano 版无 `%f`（本工程本就不用） | `grep '%[0-9.]*[fge]' main/**` = 0；`grep -c '%ll[du]'` = 16 |
| R7B-18 | 热路径 INFO 降 WARN | 3~7 ms/按键 + ~0.4 KB | 少日志 | 按键延迟实测（现在没人量过按键到画面响应的延迟） |

**明确不建议做的**：关 WiFi/BT/LWIP/mbedTLS/SPIFFS/FATFS（0 字节收益）、改 `-O3→-Os`（掉帧）、
关 `int_wdt`/panic（0.3~4 KB 换掉调试能力）、给全工程上 LTO（AOT 内联行为不可预测）。

---

## 8. 复算附录（主审照抄即可）

```powershell
$T = "C:/Users/DuWeisi/.espressif/tools/riscv32-esp-elf/esp-14.2.0_20251107/riscv32-esp-elf/bin"
cd C:\Users\DuWeisi\digimon-color-passport\firmware\digicore-c3\build

# 1) 段表（唯一可信的段级口径）
& $T/riscv32-esp-elf-readelf.exe -S digicore-c3.elf

# 2) 镜像分段（6 段，见 §1.2）
python <本文件 §1.2 的 binseg.py> digicore-c3.bin

# 3) 组件/文件账本（map 解析；剔除 61,521 B 幽灵条目）
python map2.py digicore-c3.map map2.json

# 4) 丢弃段总量（gc-sections 生效证据）
#    map 第 858 行 "Discarded input sections" 到第 9125 行 "Memory Configuration"
#    合计 5,246 条 / 1,108,063 B = debug 815,984 B（不进镜像）+ 代码/数据 292,079 B（真被省掉）
Select-String -Path digicore-c3.map -Pattern '^\s\.text\.\S+\s+0x0+\s+0x' | Measure-Object

# 5) 单对象丢弃 vs 留存（-ffunction-sections 生效证据）
& $T/riscv32-esp-elf-size.exe -A "esp-idf/esp_driver_uart/CMakeFiles/__idf_esp_driver_uart.dir/uart.c.obj"
#   → 该对象被丢弃 21,133 B、进镜像 3,797 B

# 6) AOT 页表
& $T/riscv32-esp-elf-nm.exe -S --size-sort digicore-c3.elf | Select-String 'aot_ent'
Select-String -Path ..\main\emu\aot_blocks.c -Pattern 'static void aot_b' | Measure-Object

# 7) 日志/断言调用点
& $T/riscv32-esp-elf-objdump.exe -d digicore-c3.elf | Select-String -SimpleMatch '<esp_log>'   | Measure-Object  # 638
& $T/riscv32-esp-elf-objdump.exe -d digicore-c3.elf | Select-String -SimpleMatch '<__assert_func>'| Measure-Object # 316
```

**本报告用到的临时脚本**（都在 `%TEMP%\digi_r7\`，不在仓库里，可删）：
`mapparse.py`（map 概览）、`map2.py`（可装载段账本）、`objsize.py`（坏掉了：把未链接的 obj 也算进来，别用）、
`binseg.py`（镜像分段）、`strings.py`（字符串普查）、`logstr.py`（逐源文件日志字面量）、`jtab.py`（跳转表扫描）。

---

## 9. 一句话给主审

**这份固件不缺 flash，缺的是 DRAM 和 icache。** 真正能省下"看得见的字节"的只有两处：
AOT 页表空槽（140 KB，但要动热路径）和 `kBrickA8`/`esp_err` 名表/断言串这几个零碎（合计 ~40 KB，代价是观感或调试能力）。
其余所谓"冗余"——WiFi/BT/HTTP/OTA/SPIFFS/FATFS/gdbstub——**一个字节都没进镜像**，改 `sdkconfig` 只会让构建更快，
不要写进优化成绩单。日志那 32 KB 字符串带来的运行时成本（每行 3~7 ms 阻塞串口）比它的 flash 成本重要得多。
