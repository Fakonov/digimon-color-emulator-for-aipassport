# Digimon Color 原版固件在 FoloToy AI Passport（ESP32-C3）上的移植记录

> 目录：`firmware/digicore-c3/`
> 目标板：FoloToy AI Passport —— ESP32-C3（160 MHz RV32IMC，400 KB SRAM，无 PSRAM，8 MB flash），320×240 ST7789P3 走 SPI2，3 个按键在 ADC 阶梯上。
> 目标：**让 Digimon Color 原版 ROM 在 C3 上的 unSP 解释器里跑起来，并把游戏那块 96×96 画面显示到屏上**。不追求实时。
> 状态：已上机。当前 **7.8 fps**（2.86 → 3.15 M guest instr/s），画面与 PC 版逐像素一致（0/9216）。

---

## 1. 架构

```
                 ┌──────────────────────────── app_main (main/app_main.cpp) ───────────────────────────┐
                 │  每帧: update_buttons() → run_guest_frame() → blit_frame() → 每秒一行 UART 日志        │
                 └───────────────┬───────────────────────────────┬──────────────────────────────┬────────┘
                                 │                               │                              │
                    ┌────────────▼─────────┐        ┌────────────▼───────────┐      ┌───────────▼─────────┐
                    │ unsp_20_device (CPU) │        │ Gpce4 (SoC 模型)        │      │ display.c (ST7789P3)│
                    │ main/emu/unsp_glue   │◄──────►│ main/emu/gpce4.cpp      │      │ 复用 color-preview  │
                    │ main/emu/isa/*.cpp   │  bus   │  ├ ram[4096]            │      │ SPI2 DMA + LEDC 背光│
                    │ MAME 0.289 移植      │        │  ├ 定时器 6 路 + 中断   │      └─────────────────────┘
                    └──────────────────────┘        │  ├ SPI1 flash 模型 ─────┼──► SparseRom（页面缓存）
                                                    │  └ SPI2 LCD ────────────┼──► St7735 → fb[128×128]
                                                    └─────────────────────────┘
                                                                                     ▲ blit 取 fb[y=18..113][x=0..95]
```

数据来源有两处：

| 数据 | 存放位置 | 取用方式 |
|---|---|---|
| 内部 64 KiB mask ROM（`internal_rom_221017a.bin`） | 直接编进 app 的 `.rodata`（`target_add_binary_data`，符号 `internal_rom` / `internal_rom_length`） | 直接指针访问，零拷贝 |
| 8 MiB SPI flash 镜像（`dmc_v1_base.bin`） | `roms` 数据分区里的稀疏页存储（2.4 MiB） | `SparseRom` 页面缓存 → `esp_partition_read` |

### 1.1 与 PC 版（`emu/`）的差异

`emu/` 没有被修改，`firmware/digicore-c3/main/emu/` 是拷贝后改的。改动只有四处，全部集中在"取数"和"省内存"上，CPU 与 SoC 逻辑保持原样：

1. **GPU/LCD 去掉了 256×256 的 GRAM**。`emu/st7735.cpp` 用 `std::vector<uint16_t> m_displaybuffer`（256×256 word = 128 KB）存整块 GRAM；C3 没有 PSRAM，128 KB 是致命占用。原实现里 GRAM 只用来给 fb 供数（`idx → ry=idx>>9, rx=idx&0x1ff`），而 MAME 只渲染 128×128 的 cliprect，所以**只在 `rx<128 && ry<128` 时写 `fb[128*128]`（32 KB）**。对每一个会显示出来的像素，结果逐位相同；被丢弃的只是 128×128 之外的区域。
2. **flash 后端换成 `SparseRom`**。`Gpce4SpiFlash` 原来直接 `m_rom[addr]` 访问 8 MiB 内存镜像；现在通过 `SparseRom::read8/read16`，写命令 `02h`（page program）和 `20h`（sector erase）落到 4 页 16 KiB 的 RAM overlay 上，语义仍是 NOR 的 `m_rom[addr] &= data` 和"整扇区填 0xFF"。
   **读路径有两条**：启动时用 `esp_partition_mmap()` 把整个 `roms` 分区映射进来，`SparseRom::set_mapped(base, index)` 之后 `read16()` 直接在映射窗口里按索引取字（零拷贝、走硬件 flash cache）；只有 mmap 失败才回落到 16 项 4 KiB 页缓存。实测两条路径帧率相同（0.73 M instr/s），说明瓶颈不在取数，见 §8。
3. **加了指令计数器**。`unsp_device` 里 `uint32_t m_insn_count`，在 `execute_run()` 循环体尾部自增，`take_insns()` 取走并清零——用来算日志里的 `mi_s`。
4. `compat/emu.h` / ISA 源码保持逐字拷贝，只靠编译选项 `-Wno-error=format` 放过 MAME 原有的 `printf("%x", uint32_t)`（riscv32 上 `uint32_t` 是 `long unsigned int`）。

## 2. 移植后的内存地图

### 2.1 设备侧（C3 内部 SRAM）

| 用途 | 大小 | 存放 |
|---|---|---|
| ST7735 可见帧缓冲 `fb[128×128]` | 32,768 B | `.bss` |
| SoC 内部 RAM `ram[4096]`（16 位宽） | 8,192 B | `.bss` |
| 稀疏 ROM 页缓存 16 × 4 KiB | 65,536 B | `.bss` |
| 写 overlay 4 × 4 KiB | 16,384 B | `.bss` |
| 稀疏镜像页索引 `uint32_t[2048]` | 8,192 B | `.bss` |
| 送屏条带缓冲 320×24×2（DMA 可见，4 字节对齐） | 15,360 B | `.bss` |
| **小计（本移植新增/占用）** | **≈ 146.4 KB** | |
| ESP-IDF / FreeRTOS / 驱动 | 余下 | |

`idf.py size` 实测（esp32c3，`-O2`，见 §4）：DRAM 用了 217,774 B，**还剩 103,522 B**。没有触发任何"内存不够"的降级路径，所以 GRAM 的降级方案（128×128 / 32 KB）**没有用上**，本次是直接砍掉 256×256 GRAM 而不是缩小它。

仿真循环内**不做任何动态分配**：`SparseRom` 的缓存、overlay、索引全是静态数组；`display.c` 的 DMA 缓冲是 `heap_caps_malloc`，只在 `display_init()` 里分配一次。

### 2.2 guest 侧（GPCE4 地址空间，与 MAME 模型一致）

| 地址 | 内容 |
|---|---|
| `0x0000–0x0FFF` | SoC 内部 RAM（2K word） |
| `0x3000–0x30EF` | IO 寄存器（定时器、中断、GPIO、SPI1、SPI2） |
| `0x4000–0xBFFF` | 内部 mask ROM 64 KiB |
| `0xC000–0xFFFF` | 内部 ROM 前 32 KiB 的镜像 |
| `0x200000–0x3FFFFF` | SPI flash 窗口（字地址 ×2，加 `m_spi_bank<<22`） |

复位向量：`PC = read16(0x4017)`（即 `0x4010 + REG_PC`），与 `emu/main.cpp` 完全一致。

## 3. 分区表（`firmware/digicore-c3/partitions.csv`）

```
# Name,   Type, SubType, Offset,   Size,     Flags
nvs,      data, nvs,     0x9000,   0x6000,
phy_init, data, phy,     0xf000,   0x1000,
factory,  app,  factory, 0x10000,  0x200000,
roms,     data, 0x40,    0x210000, 0x5f0000,
```

| 项 | 值 |
|---|---|
| app 分区 | 0x10000 / 2,097,152 B，实际用 325,760 B（**84% 空闲**） |
| `roms` 分区 | 0x210000 / 6,225,920 B，稀疏镜像占 2,515,200 B（**剩余 3,710,720 B**） |
| 稀疏镜像落盘偏移 | **0x210000**（与分区同址，`merge_bin` 时按此偏移拼） |
| 合计占用的 flash | 0x210000 + 2,515,200 = 4,677,888 B（≤ 8 MB） |

### 3.1 稀疏镜像格式（`tools/make_sparse_rom.py` 生成）

原镜像 8,388,608 B，其中 1,437 页全零、**611 页非零**，所以只存非零页：

| 偏移 | 内容 |
|---|---|
| `0x0000` | 4 KiB 头：magic `DMC1`、version、image_size、sha1[20]、page_size=4096、page_count=2048、bitmap_offset=0x1000、index_offset=0x1100、data_offset=0x3100、flags、present_pages |
| `0x1000` | 位图 256 B（1 = 该页存在） |
| `0x1100` | 索引 8 KiB（2048 × `uint32` 页偏移，`0xFFFFFFFF` = 全零页） |
| `0x3100` | 611 × 4 KiB 数据页 |

生成结果：`2,515,200 B`，源镜像 sha1 `ebae139f708d36151efcaf2db7c329c6614d7e68`，`--verify` 逐页回读校验通过。

## 4. 构建结果（实测）

构建命令：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File firmware\digicore-c3\build.ps1
#   -Clean     全量重建（删 build/）
#   -NoSparse  跳过稀疏镜像重生成
```

产物（一次 `-Clean` 全量构建的实测值）：

| 文件 | 字节 | SHA256 |
|---|---|---|
| `build/digicore-c3.bin`（app） | 357,616 | `25CF70A5024C5F2552167BC6148AF07599B0F1B93ECE804EF713AC2C4075D222` |
| `digicore-c3-full.bin`（合并，偏移 0） | 4,677,888 | `08018E057406C84C46516AD6AE60F495CB696AAE4E64919C475925703B18EBD6` |
| `sparse/digicore-sparse.bin`（落到 0x210000） | 2,515,200 | `C6FCAA69329D682965DFF82D0B186BD34A28BA6D0DA6973C50BA8BB59F749A6F` |

> app/合并镜像的哈希每次都变：`app_main()` 里打了 `__DATE__ __TIME__`，二进制里带编译时间。稀疏镜像与源码无关，哈希稳定。

bootloader 0x51E0（20,960 B，分区余量 36%）；app 占 app 分区 0x4F880 / 0x200000，**84% 空闲**。

`idf.py size`（esp32c3，Totals）：

| Memory Type/Section | Used [B] | Used [%] | Remain [B] | Total [B] |
|---|---|---|---|---|
| DRAM | 217,774 | 67.78 | **103,522** | 321,296 |
| 　.bss | 152,048 | 47.32 | | |
| 　.text（IRAM） | 58,670 | 18.26 | | |
| 　.data | 7,056 | 2.20 | | |
| Flash Code（.text） | 140,720 | | | |
| Flash Data（.rodata） | 118,908 | | | |
| 　.appdesc | 256 | | | |
| RTC SLOW | 56 | 0.68 | 8,136 | 8,192 |
| Total image size | 325,652（.bin 因对齐到 325,760） | | | |

C3 把 IRAM 和 DRAM 放在同一块 SRAM 里报告，所以只有一行 DRAM；**内部 RAM 余量 103,522 B**（约 100 KB），`malloc` 大块仍可用。

## 5. 模拟了什么 / 桩了什么

**完整模拟（与 MAME 0.289 一致）**

- unSP 2.0 全部指令：`emu/isa/` 五个源文件逐字移植（`unsp_fxxx` / `unsp_exxx` / `unsp_extended` / `unsp_jumps` / `unsp_other`），含 DIVQ、MULS、SECBANK、FIR_MOV、中断压栈。
- GPCE4 总线与内存地图、`io_r/io_w` 全部已实现寄存器。
- 6 路定时器（1 kHz / 20 kHz / 2 Hz / 64 Hz / 2 kHz / 300 kHz SPI2）与 `cycles_to_next_event` 精确事件调度。
- 中断控制器：IRQ0/2/3/6/7 的 status/ctrl/int2 映射，vector base `0x4010`。
- GPIO PORTA 输入（ADC 阶梯按键）、SPI1 flash 命令集（含 `0x02` 页编程、`0x20` 扇区擦除、`0x9F` RDID 等）、SPI2 → ST7735（CASET/RASET/RAMWR/MADCTL、显示开关）。
- 游戏自身的渲染路径：ROM 里的代码把像素通过 SPI2 打进 ST7735，模型再落到 fb。

**桩 / 未建模**

| 项 | 现状 |
|---|---|
| LCD 睡眠/唤醒（`10h`/`11h`） | 模型只置 `sleep` 标志，不黑屏。真机到 ~17 s guest 时间游戏会发 SLPIN——此为 MAME 模型既有缺陷，PC 版同样如此 |
| 音频 | GPCE4 在本模型里没有音频通路，未实现（AI Passport 的喇叭不会响） |
| guest 存档持久化 | 游戏往 `0x7FD000` / `0x7FE000` 写 8 字节；写入落在 16 KiB RAM overlay，**掉电即丢**。要持久化需把 4 个 overlay 页 flush 到 NVS/fatfs——本次未做，明确记录在此 |
| 未映射地址读 | 打印一次告警后返回 `0xFFFF`（原模型行为） |
| SPI2 时序 / DMA | 不建模，`spi2_txdata_w` 立即送进 LCD 模型 |
| CPU 主频 | 模型用 96 MHz（MAME 遗产）；真实 GPCE4064B 是 48 MHz。所以"guest fps"是按 96 MHz 虚拟时钟算的 |
| 128×128 之外的 fb 区域 | 不显示（只有 96×96 窗口上屏） |

## 6. 渲染与输入

- 取 `fb` 的 `x 0..95, y 18..113`（游戏自己的 96×96 窗口），**2 倍缩放**成 192×192，居中放到 320×240 面板（`LEFT=64, TOP=24`），其余区域填黑。
- 旋转：`kRot = 0`（不旋转）。第一版照 `color-preview` 用了 180°，上机后用户反馈**画面上下颠倒**，改为 0°（`display.c` 里 `esp_lcd_panel_swap_xy(true)` + `mirror(true,false)` 已经把面板摆好，应用层不能再转）。要改就改 `main/app_main.cpp` 的 `kRot`（0/1/2/3），一次编译一行。
- 每个 guest 帧都送屏（未做跳帧）。送屏走 `display_draw_bitmap()`，按 24 行一条带、共 10 次 SPI 传输，单次 ≤ 320×24×2 = 15,360 B（`display.c` 的 `max_transfer_sz` 是 51,200 B）。整帧 153,600 B，80 MHz 下约 15 ms 纯总线时间。
- 按键：`buttons.c` 读 GPIO0 的 ADC 阶梯（key0 <150 mV，key1 <447 mV，key2 <1900 mV，松手 >1900 mV，同一套曲线拟合校准），映射到 **active-low** 的 PORTA 位 `0x0008 / 0x0010 / 0x0020`：`soc.set_port(0, 0xFFFF & ~mask)`。只在电平变化时下发。
- **按键必须异步采样 + 拉伸**（2026-11-20 修）：两个独立的坑。
  1. 采样率：guest 每帧才看一次 PORTA，而一帧是 0.9 s；原写法在帧循环里调 `buttons_scan()` 且要连续两次同值才算，等于要求**按住约 2 秒**。现在 `buttons_init()` 起一个 20 ms 的轮询任务，`buttons_state()` 给实时电平，`buttons_take_latched()` 给“上次取用以来按下过的键”。
  2. 保持长度：**PC 侧实测 guest 需要连续 4 帧按下才动作**（按 1/2/3 帧屏幕不动，4 帧才切换），因为游戏自己的去抖是按 60 Hz 帧算的 60~70 ms。所以帧循环把一次轻点**拉伸成 5 个 guest 帧按下 + 1 帧松开**（`kPressFrames = 5`）。代价：1.1 fps 下每次按键要等 ~5 秒才生效——这是速度问题，不是输入问题。

## 7. 串口日志

UART0，115200（`CONFIG_ESP_CONSOLE_UART_DEFAULT`），tag `DIGICORE`：

```
I (312) DIGICORE: Digimon Color on FoloToy AI Passport - esp32c3 rev 0.3, 1 core(s)
I (312) DIGICORE: build Nov 20 2025 ..., reset cause 1, heap 2xxxxx B (largest block 1xxxxx B)
I (320) ROMSTORE: partition `roms` data/0x40 at 0x210000, 6080 KiB
I (325) ROMSTORE: sparse v1: image 8388608 B, 2048 pages (611 present), data at +0x3100
I (330) ROMSTORE: image sha1 ebae139f708d36151efcaf2db7c329c6614d7e68
I (340) DIGICORE: internal ROM 65536 B embedded, reset vector -> PC=xxxxxx
I (nnn) DIGICORE: BOOT_OK first non-blank guest frame at N frames, pc=..., display_on=1 sleep=0 madctl=...
I (nnn) DIGICORE: frames=12 guest_fps=2.85 mi_s=5.61 host_fps=2.85 pc=04a2 sr=... sp=... insns=1796 K cyc=19200 K
```

- `frames` guest 帧累计；`guest_fps` 实测 guest 帧率；`mi_s` 每秒 guest 指令数（M instr/s）；`host_fps` 实际送屏帧率；`pc/sr/sp` 当前寄存器；`insns` 累计指令（K）；`cyc` 累计 guest 周期（K）。
- 每秒一次；`BOOT_OK` 只在第一帧非空画面出现一次。
- 中断进入不打日志（不刷屏）。`rom pages loaded/overlay/writes` 是 `ESP_LOGD`，需要 `CONFIG_LOG_MAXIMUM_LEVEL_DEBUG` 才能看到。
- 定点数（`%u.%02u`）是故意的：newlib-nano 的 printf 不支持 `%f`。

## 8. 实测速度（2026-11-20 上机，COM4）

在 AI Passport（ESP32-C3 @160 MHz）上刷入并跑通，串口 115200 日志（`dbgtest/c3_serial4.log`）：

```
I (16678) DIGICORE: BOOT_OK first non-blank guest frame at 20 frames, pc=007ca6, display_on=1 sleep=0 madctl=00a8
I (55927) DIGICORE: frames=60 guest_fps=0.39 mi_s=0.15 ...   <- 打帧 dump 期间被串口拖慢
```

稳态（未打 dump 时）：`guest_fps=1.22`、`mi_s=0.73`、`host_fps=1.22`。

- **每 guest 指令约 219 个主机周期**（160 MHz ÷ 0.73 M/s）。这个数字远高于原先 5.3–8.0 M instr/s 的估算，
  说明瓶颈不是 flash 页缓存（换成整段 `esp_partition_mmap` 零拷贝之后 fps 一点没变），而是解释器本身：
  每条指令有 1–3 次 `UnspBus::read16` 虚调用、`execute_one()` 的大 switch、`check_irqs()` 与 `m_icount` 记账。
- 因此 **1.2 guest fps**，能开机、能出画面、能按键，但不是实时。
- 真正的提速路线是 `firmware/recomp-bench` 已经验证过的**静态重编译**（块级 80.68 M guest instr/s，
  约 10 倍余量 → 理论 30+ fps），不是继续磨解释器。
- 另外 `dump_frame_hex()` 打 36 KB 十六进制时会把帧率压到 0.39，属预期（UART 阻塞）。

### 8.1 设备侧时钟播种（2026-11-20 17:2x）

设备没有 RTC，原版固件上电后停在 0:00 的设置界面等人按键。固件在第 120 帧用**编译时间**
（`__TIME__`）播种 guest 时钟：`0x0CB0=1`、`0x0CAF/0x0CAE/0x0CAD=时/分/秒`、`0x0CB2=1`。

```
I (111135) DIGICORE: clock seeded 17:19:35 from build time
I (158753) DIGICORE: frames=170 guest_fps=1.10 mi_s=0.43 host_fps=1.10 pc=0002fc sr=0220 sp=0ff2
```

第 130 帧再打一次完整帧（`tools/c3_shot_all.py` 可把一段串口日志里的所有 `SHOT_BEGIN` 块
都还原成 PNG）；此时画面仍是 boot logo，因为**logo 要按键才走**——时间是灌进 RAM 的，
不按 B1 游戏不会离开 logo 页。串口抓取脚本：`tools/c3_serial_capture.py`、
`tools/c3_serial_reset_capture.py`（带 DTR/RTS 复位，需用带 pyserial 的 ESP-IDF Python）。

### 8.2 提速到 7.8 fps（2026-11-20 19:4x）

先量分布再动手。10 秒的 PC 直方图（`--pc-hist`，242,116,283 条指令、5,310 个不同 PC）暴露了真凶：
`0x2002F4..0x200343` 那 **68 条指令的函数被调用 310 万次**（每 PC 各 3,097,129 次），占全部动态指令的 **约 87%**。
它就是主循环里的 PRNG/hash 忙等（`0x230321: call 2002f4` 反复调）。原先以为的 `0x5F35` 三行循环只占 4.8%。

于是按「静态重编译」路线处理这一个块：

| 手段 | 实现 | 收益 |
|---|---|---|
| 复用已验证的重编译块 | `tools/gen_hotblock.py` 把 `recomp/hot_func.c`（golden 逐字比对 PASS 的那份）机械转换成 `emu/hotblock.h`，RAM 改成指向 SoC 实时数组的指针 | 核心 7 倍 |
| 尾部留给解释器 | 只跑 `0x2002F4..0x200342`（65 条），`0x200343 pop bp, pc` 仍由解释器执行，返回路径语义零变化 | 正确性 |
| 精确计费 | 该区间各指令众数周期之和 = **263 周期/调用**（用 `dbgtest/cyc900.txt` 统计；平均 45.5 是中断归因污染的假值） | 时序一致 |
| RAM 平坦窗口 | `unsp_device::m_ram/m_ram_limit`，`read16/write16` 命中 `<=0x0fff` 直接数组访问，绕过 `UnspBus` 虚调用 | 少量 |
| 自旋折叠 | `spin_collapse()` 把 `0x5F35`（8 周期/圈）与 `0x5251`/`0x5376`（33 周期/圈）的倒计数循环一次性收缩，留一圈给解释器 | 少量 |
| O3 | 组件级 `-O3` | +11% |

设备实测（`dbgtest/o3_serial.log`）：

```
I (29363) DIGICORE: frames=178 guest_fps=7.82 mi_s=3.15 host_fps=7.82 pc=200343 ... insns=63906 K
```

**1.09 → 7.82 fps（7.2 倍）**，guest 指令率 0.73 → 3.15 M/s；PC 侧同一改动 1.7x → 13.8x。

正确性验证：

- PC 侧 20 秒跑完，`--save-flash` 的整片存档在「解释器」与「热块」两条路径下 **0 字节不同**；周期计数只差 135,603/1,920,000,000（0.007%，来自中断只能在块边界投递）。
- 设备第 60 帧与 PC 参考帧 **0/9216 像素不同**，逐像素一致。
- `-O3` 后再测仍 0/9216。

剩余瓶颈：PRNG 块拿掉后，剩下 13% 的指令由解释器承担，约 51 主机周期/条（160 MHz ÷ 3.15 M/s）。
要摸到 60 fps 需要约 27 M instruction/s，也就是解释器再快 8.5 倍——继续磨解释器不够，
得把 `0x230000` 段主循环与 `0x5xxx` UI 的热块也做成静态重编译（下一批 20–30 个块，需要可复现的代码生成器，不能再像这次手工搬）。

### 8.2.1 又试了四种微优化，全部实测后回滚（2026-11-20 21:xx）

热块拿下 87% 之后，剩下的 10% 解释指令吃掉约 80% 的时间。逐个试过、逐个上机量过，
结论是**解释器已经是这套架构的上限**，这四项都不能动 fps：

| 尝试 | 想法 | 设备实测 | 处置 |
|---|---|---|---|
| 取指窗口缓存 | `fetch16()` 单条目 4 KiB 窗口，把每条指令的虚函数取指换成「比较基址 + 数组索引」 | 7.73 fps（原 7.82） | 回滚 |
| 解释器放 IRAM | 给 27 个 `unsp_device::` 定义加 `IRAM_ATTR`，绕开 flash 取指的 cache 抖动 | 6.78 fps | 回滚（并移除 27 处属性） |
| 打开 jump table | IDF 默认 `-fno-jump-tables -fno-tree-switch-conversion`，改成打开 | 6.78 fps | 保留（无收益也无害） |
| 单指令快速处理 | `fast_step()`：把 `rX,imm6` / `rX,[bp+imm]` / 条件跳转三组最高频形式直接译码，跳过 `execute_one` 的三层派发 | 6.60 fps（解释指令从 273k/s 降到 196k/s，但总时间变长） | 代码保留、默认关闭 |

为什么快速处理反而更慢：`fast_step()` 是个独立的非内联函数，每条指令一次调用 + 一次重复译码，
在 RV32 上没有分支预测可依赖，函数序言/尾声的成本超过了省下的派发。要它有用，得把处理逻辑
**内联进循环体**（让寄存器常驻），那是把解释器重写成「巨型 switch」的工作量，收益仍受限于
剩下这 10% 的指令数。

设备侧真实时间分布（探针实测，`prof_*` 计数器）：

| 项 | 占比 |
|---|---|
| 解释器（每指令 **403 主机周期**） | 约 78% |
| SPI 条带 blit（10 条 320×24） | 约 18% |
| `soc.tick()` 定时器/中断 | 约 5% |

403 周期/条这个数字本身就是结论：`execute_one → execute_remaining → do_basic_alu_ops`（引用参数）
在无 LTO 的 IDF 构建里跨不了编译单元，每条指令 3–6 次函数调用。IDF 5.5.3 没有
`CONFIG_COMPILER_LTO`，`-flto` 加进去 configure 直接失败，所以「靠编译器解决」这条路堵死。

最终交付配置：**7.69 fps**（`interp=0`、无取指窗口、无 IRAM、fast ops 关闭），像素仍 0/9216。
再往上只有一条路：**整程序 AOT 静态重编译**。覆盖率数据支持这个判断——`ind 0`、`smc 0`，
762 个基本块全部静态可定，没有间接跳转也没有自修改代码，编译产物可以直接内联成 C 函数。

### 8.2.2 把等待循环整段折叠：7.69 → 14.15 fps（2026-11-20 21:4x）

上一节把时间归到了「13% 的解释指令吃掉大半时间」，但那个比例是用周期计数器
（`esp_cpu_get_cycle_count()`，两次调用约 300 周期）测出来的，本身不可信。于是先在
设备上加了一个**不含探针的微基准**：

| 项目 | 结果 |
|---|---|
| `hot_block_prng`（68 条指令的 PRNG 体） | 100,000 次 = 50,725 µs → **81 周期/次调用（1.2 周期/指令）** |
| `fetch16` 一次（走虚函数 + 范围判断） | **49 周期** |
| `execute_one(0x9c00)` 一条 `r4 += imm6` | **163 周期** |
| 两者合起来（一次完整「取指 + 译码执行」） | **227 周期** |

结论变了：**一条解释指令 227 周期，其中 163 是译码分派、49 是取指**。IDF 的 `-O2`
把 `execute_one → execute_remaining → do_basic_alu_ops` 拆在三个编译单元里，每次都是一次
跨 flash 的远调用加 i-cache 未命中——这个开销靠改微结构消不掉。

但同一张表也指出了另一条路：既然一条解释指令要 227 周期，那就**别让它执行**。主循环
`0x230321` 是「等 64 Hz 节拍」的忙等：

```
230321  f060 02f4   call 2002f4        ; 68 条指令的 PRNG（已经是原生代码）
230323  fe00        ds = 00
230324  990c 0850   r4 = 0850
230326  98e4        r4 = ds:[r4]
230327  4840        cmp r4, 00
230328  5e48        je 230321
```

每圈 74 条指令、**298 周期**（PRNG 体 273 + 外壳 25，均为逐条众数周期实测）。PRNG 那 68 条
已经原生，可剩下 6 条外壳走解释器要 6×227 ≈ 1,362 周期——**一圈的开销 82% 花在 6 条指令上**。

`unsp_device::wait_loop_collapse()` 把整圈原生化：直接跑 `hot_block_prng`、手动做
`pop bp`、把 PC 置成返回地址 0x230323、再按 `ds=0 / r4=0x0850 / r4=[r4] / cmp / je` 的语义
收尾，按 298 周期/圈计费，`m_sirq` 一到或预算耗尽就交还解释器。

第一次实现**跑飞**了（`PC=004020 SR=8280 SP=C099`），原因是把 `call` 当成「压 PC + 压 SR」，
而这条 CPU 的 `call` 只压返回地址；多压的那个字让 SP 每圈往下漂一格，42,000 圈后把栈冲穿。
改成「call/ret 相互抵消、整圈不进栈，只保留被调函数的 `push bp` / `pop bp` 配平」之后：

| 配置 | 设备 fps | 说明 |
|---|---|---|
| 折叠前 | 7.69 | `mi_s=3.09` |
| 折叠后 | **14.15** | `mi_s=1.37`、`insns` 从 320k/帧降到 92k/帧 |

正确性：PC 侧 20 s 跑完 `--save-flash` 与解释器路径**逐字节相同（0 字节差）**、帧缓冲完全一致、
周期计数 `1920138455` 与解释器路径**完全相同**；设备侧第 60 帧 dump 与此前验证过的参考帧
sha256 一致（`38a42031…`）。

剩下 70 ms/帧仍然主要是解释指令（227 周期/条），要再上台阶只有 AOT 一条路。

### 8.2.3 到 60 fps：先拆 blit，再削定时器（2026-11-20 22:xx）

目标是 60 fps。14.15 fps 时用探针（`esp_timer_get_time()` 包住 `run_guest_frame()` 与 `blit_frame()`）
量出的分布是 **guest 58% / blit 40%**——上一节「skipped blit 无变化」的结论只在 7.6 fps 的配置下成立，作废。

**第一步：重写 blit（`tools/patch_blit.py`）→ 24.5 fps**

| 改动 | 原因 |
|---|---|
| 只发 192 宽的窗口（`kLeft..kLeft+kWinW*kScale`） | 原来每帧发 320×240 全屏 153,600 B，其中 40% 是永不变化的黑边 |
| 脏行跳过（`s_prev[]` + 逐行比较） | 时钟/菜单每帧只改几行，没变的条带整条不发 |
| 32 位存储（`pair = sw \| sw<<16`） | `kRot == 0` 是恒等映射 + 水平 2 倍，一次写两个像素，省掉 `rot_map()` 与两次字节写 |

设备实测 `guest_pct=97 blit_pct=1`，帧时间从 69 ms 降到 41 ms。

**第二步：削掉 guest 侧的四块固定开销 → 42 → 79.7 fps**

| # | 改动 | 之前 → 之后 |
|---|---|---|
| 1 | collapse 每圈只读一次总线：把「切片内 tick 计数器不会变」「有待处理中断就先退出」提到圈外，圈内只剩 `hs.r[4]=0; hot_block_prng(&hs); SP+=1; BP=bp_saved;` | 1,260 → **245 主机周期/圈**，24.5 → 27.3 fps |
| 2 | `kSliceCycles` 2,000 → **48,000**（一个 20 kHz 周期；调度仍按事件精确落点，只是切片变粗） | 27.3 → 42.0 fps |
| 3 | 300 kHz SPI2 驱动源默认不排事件（`timer_300k_enable = false`）：它只置 INT bit8，digicolr 从不使能，但 320 周期的粒度让 `tick()` 每秒吃掉 42 M 周期 | 42.0 → 51.2 fps，`tick()` 42 M → 9.3 M 周期/秒 |
| 4 | 被屏蔽的定时器不再排事件（`source_enabled(i)`：`timer_a` 的使能位是 `INT_Ctrl` bit15，全程为 0） | 51.2 → **79.7 fps**，`tick()` 再降到 3.0 M |

第 4 条是「中断源被 CPU 屏蔽时，事件不可能改变 guest 状态，只会置一个没人读的 pending 位」——不是
近似，是等价变换：`INT_Status` 的对应位不再被置起，而游戏既不读也不使能它。

**第三步：节拍锁到 60.00 fps**

79.7 fps 相当于让 guest 以 1.33 倍速变老（时钟、育成计时都跟着 guest 走），所以必须限速。
第一次用 `vTaskDelay(pdMS_TO_TICKS(kFrameUs - spent))` 把**微秒当毫秒**传进去，`pdMS_TO_TICKS()`
把它当成「毫秒数」，于是每帧睡 3.1 秒，设备变成 0.16 fps——一晚上最蠢的一行代码。修正单位后仍只有
57.3 fps（`vTaskDelay` 向上取整到 1 ms tick，每帧多吃 0.7 ms）。

最终做法是**绝对截止时间**（`tools/patch_pacer_deadline.py`）：截止时间每帧加 16.667 ms，先
`vTaskDelay` 睡掉 `wait - 2 ms`，最后 2 ms 忙等；落后一整帧才重新对齐，避免 tick 量化累积。

```
I (24887) DIGICORE: frames=981 guest_fps=60.00 mi_s=0.22 host_fps=60.00 pc=200343 sr=0020 sp=0ff5
         guest_pct=68 blit_pct=3 ... hotins=145724 collapse=324344 collcalls=2173 stage=336/109793/1384
```

连续 60 秒 soak（`dbgtest/final60.log`）：3,116 帧、每秒都是 **60.00**、无 panic / 无 watchdog、
无 `STUCK`；第 60 帧 dump 与 PC 参考帧 **0/9216 像素不同**。

**整机速度阶梯（全部上机实测）**

| 阶段 | guest fps | 帧时间 |
|---|---|---|
| 朴素解释器 | 1.09 | 917 ms |
| + 重编译 PRNG 块 / O3 / 自旋折叠 | 7.82 | 128 ms |
| + 等待循环整段折叠 | 14.15 | 71 ms |
| + blit 重写 | 24.5 | 41 ms |
| + 切片/定时器三项 | 79.7（无节拍） | 12.5 ms |
| + 截止时间节拍 | **60.00**（锁定，输出与 1.33 倍速时逐像素相同） | 16.7 ms |

### 8.3 逐像素验证（设备 vs PC）

固件在第 60 帧（guest 1 s）把 `fb[18..113][0..95]` 以 RGB565 十六进制打到串口，
`tools/c3_shot.py` 还原成 `docs/c3_device_frame60.png`；PC 侧 `digicore.exe --frames 60 --shot-raw`
取同一窗口比较：

```
pixels differing (raw 565): 0 / 9216
```

**0 差**。同一份解释器 + SoC 模型在 C3 上和 PC 上逐像素一致，说明移植没有引入语义偏差。

## 9. 刷机（由设备所有者执行）

合并镜像已经在偏移 0 处包含 bootloader + 分区表 + app + 稀疏 ROM 分区，**一条命令写完**：

```
esptool --chip esp32c3 -p COMx -b 460800 --before default_reset --after hard_reset \
        write_flash --flash_mode dio --flash_freq 80m --flash_size 8MB \
        0x0 digicore-c3-full.bin
```

- 把 `COMx` 换成 AI Passport 自己的串口（**不是别的板子的口**）。
- 镜像在 `firmware/digicore-c3/digicore-c3-full.bin`，4,677,888 B。
- 想要分开写（例如只更 app，不动 2.4 MB 的 ROM 分区）：

```
esptool --chip esp32c3 -p COMx -b 460800 write_flash --flash_mode dio --flash_freq 80m --flash_size 8MB \
        0x0 build/bootloader/bootloader.bin 0x8000 build/partition_table/partition-table.bin \
        0x10000 build/digicore-c3.bin 0x210000 sparse/digicore-sparse.bin
```

- 上电后串口 115200 8N1，看 `BOOT_OK` 与每秒状态行。屏幕应显示 192×192 的游戏画面（居中等比放大），背景黑。

## 10. 已知缺口

1. **已上机验证（2026-11-20）**：COM4 刷入后 `BOOT_OK`、60 帧 dump 与 PC 版逐像素 0 差；**方向已修**（kRot 2→0，用户反馈 180° 时画面上下颠倒）；按键电平仍未肉眼确认。
2. **guest 存档不持久**。见 §5。
3. **~17 s（guest 时间）后游戏发 SLPIN**，真机行为是黑屏；模型不模拟睡眠，所以屏上继续有画面——与 PC 版一致，属模型缺陷而非移植缺陷。
4. **~47.7 s（guest 时间）画面开始出现乱码/花屏**，`docs/FEASIBILITY.md` 已记录，PC 版同样复现，推测是渲染路径里某个未建模的寄存器（**未证实**）。
5. `-Wno-error=format`：`main/emu/gpce4.cpp` 有 2 处 `printf("%x", uint32_t)` 警告。保留是为了下次从 `emu/` 再同步拷贝时不用二次修改；真要干净就把这两处改成 `unsigned(...)` 强转。
6. **没有跳帧开关**。若要提高 `host_fps` 观感，可在 `app_main` 里每 N 帧才 `blit_frame()`。
7. **只有解释器**。`recomp-bench` 证明静态重编译在 C3 上能到 80 M instr/s（约 10 倍），是后续提速的正路。
8. `main/emu/` 是 `emu/` 的拷贝，上游改动需要手工再同步（四处改动见 §1.1）。

## 11. 主机侧验证（`hosttest/`）

`main/emu/romstore.h` 的页缓存与写 overlay 无法在设备上做单元测试，因此在 PC 上对着原始 8 MiB 镜像做了等价性验证：

```powershell
cd firmware\digicore-c3
& 'C:\Users\DuWeisi\w64devkit\bin\g++.exe' -std=c++20 -O2 -Wno-sign-compare `
    -o romstore_test.exe hosttest/romstore_test.cpp main/emu/romstore.cpp
.\romstore_test.exe sparse\digicore-sparse.bin ..\..\roms\dmc_v1_base.bin
```

实测输出：

```
sparse   2515200 B, image 8388608 B, page 4096 B, index +0x1100, data +0x3100
pass 1   4194304 words, 2048 page loads
pass 2   4194304 words, 2048 page loads
read8    4096 bytes at 0x230000 ok
save      sector 0x7fd000 erased + 8 bytes programmed ok
overlay  survives 32 page evictions ok (loads 4130, overlay hits 2, writes 9)
neighbour page 0x7fe000 untouched ok
ALL OK
```

覆盖：全镜像 419 万字的 `read16` 两遍（第二遍打乱页序强制换出）、`read8` 奇偶偏移、擦除 + 页编程的 NOR 语义、overlay 在 32 次换页后仍然有效、相邻扇区未被误擦。
