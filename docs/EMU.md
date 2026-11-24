# PC 侧 unSP 模拟器（emu/）— 第一版可玩

目标：在 PC 上完整执行 `internal_rom_221017a.bin` + `dmc_v1_base.bin`，跑通开机、进入时钟界面、
操作三键、进入数码蛋/育成画面并把存档写回 SPI flash。本目录**不是**固件，与 C3 移植无关。

结论先行（2026-11-20 实测，第二轮补完）：

- 指令级：0–20 ms / 900 ms / 5 s / 20 s 四个窗口与 MAME 逐条指令 **100%** 一致（PC 流比对），
  并且 0–20 ms 的 **682,005 条指令周期数 0 处差异**。
- 像素级：0.5 s 处的 ST7735 可见区与 MAME `-aviwrite` 帧 **9,216 / 9,216 像素**完全一致。
- 数据级：t=200 ms 的 4 KiB RAM 与 MAME 快照只差 4 个字（1 个计数器 + 3 个栈槽）。
- 行为级：能从 boot logo → 设时钟 → **数码蛋 → 孵化成育成中的数码兽**；60 s autoplay 无异常。
- 存档级：退出时写 `saves/flash.bin`，重启后游戏读回并显示「ロード しゅうりょう（读取完成）」。
- 时钟：真机没有 RTC，用 `--clock-sync` 注入本机时间；`--clock-fast` 让 1 guest 秒 = 1 游戏分钟，
  于是 60 秒就能看到破蛋。
- 设备级：同一内核已跑在 ESP32-C3（AI Passport / COM4），60 帧画面与 PC 版逐像素 0 差，
  但只有 1.2 guest fps（见 `docs/C3_PORT.md`）。

## 构图

    emu/
      build.ps1          构建脚本（w64devkit g++，产出 digicore.exe 与 digimon-player.exe）
      main.cpp           无界面驱动器：trace/断点/RAM 快照/截图/flash 存读，用于对拍与回归
      player.cpp         Win32 + GDI 交互窗口：4 倍放大、60 fps、键盘三键、F1 截图
      gpce4.cpp/.h       GPCE4 SoC 模型（移植自 MAME 0.289 的 generalplus_gpce4_soc + digicolr 机型）
      st7735.cpp/.h      ST7735 面板模型（GRAM 256×256、MADCTL、CASET/RASET、RGB565 像素流）
      unsp_glue.cpp      MAME unSP 核心与本项目总线/中断之间的粘合层
      isa/               MAME unSP 指令实现的副本（构建时从 ref/unsp/*.cpp 复制，保证 include 解析）
      compat/            兼容层头文件（unsp.h 等），让 MAME 源码在 w64devkit 下编译
      gpce4_selftest.cpp 83 条断言的 SoC/LCD/flash 自测
      obj/               目标文件

## 时序模型

CPU 时钟 96 MHz（MAME 的 `digicolr` 配置），一帧 = 1,600,000 cycles = 1/60 s。
主循环把每帧切成 **2,000 cycles** 的片（`SLICE_CYCLES`），每片执行
`cpu.m_core->m_icount = slice - 1; cpu.execute_run(); soc.tick(slice);`。

2,000 这个数字不是随手取的：2 kHz 源周期 48,000 cycles，2,000 cycles 让每个定时器源在一帧内
至少被量化 24 次，`tick()` 的事件表不会把同一毫秒内的两次事件合并。早期按 1 ms（96,000 cycles）
切片时 IRQ6 实际速率掉到 1 kHz，就是这个原因。

现在的循环更进一步：每片只跑到**下一个定时器事件**为止。

    slice = soc.cycles_to_next_event(min(remain, SLICE_CYCLES));
    cpu.m_core->m_icount = slice - 1; cpu.execute_run();
    consumed = (slice - 1) - m_icount; soc.tick(consumed); cycle += consumed;

`cycles_to_next_event()` 取 `m_timers[0..5]` 中最小的 `next - m_now`（跳过 300 kHz 的 SPI2 驱动源，
它只影响 INT bit8/IRQ3，digicolr 从不使能），所以中断总是在「事件时刻 + 一条指令的 overshoot」
处被取走，与 MAME 的调度点同一个量级。`SLICE_CYCLES` 现在只是上限。

定时器源（`emu/gpce4.cpp` 顶部常量）：`1 kHz=96,000`、`20 kHz=4,800`、`2 Hz=48,000,000`、
`64 Hz=1,500,000`、`2 kHz=48,000`、`300 kHz=320`、`SPI TX=2` cycles；源位分别是
`29 (timer_c) / 31 (timer_a) / 0 (2 Hz) / 2 (64 Hz) / 9 (2 kHz) / 24 (SPI2)`。

## 与 MAME 的偏差（都是有意的）

1. **SPI flash 同步化**：MAME 用 48 MHz 移位定时器把 TX FIFO 里的字节一个个吐出去；我们的 CPU
   是按片跑的，没法与定时器交织，突发写必然溢出 8 字节 FIFO（旧版 1 秒 363 条 overflow，
   flash 命令流被打断，阈值表读到 0）。现在 `write_to_tx_fifo()` 就地完成一次 write/read 往返。
2. **SPI flash 可写**：MAME 的 `generic_spi_flash_device` 只解码地址不落盘；游戏在 0x7fe000
   写存档，所以本模型实现 page program（按 NOR 语义取 AND）与 4 KiB 扇区擦除，并把整片镜像
   交给调用方持久化。
3. **IRQ7 走硬件 OR**：MAME 的 `update_interrupts()` 是 if/else，2 Hz 源永远到不了 CPU；
   这里默认两路 OR（`irq7_mame_quirk = false`），要复现 MAME 行为把开关打开即可。
4. **未实现的寄存器**：300c-300e、3031/3032、3036、303a/303b、3073/3074、309a-309f、
   30b0-30b3、30d0-30d7、30e8、30ea-30ef，以及 0x1000-0x2fff、0x10000-0x1fffff、0x400000+
   （读 0xffff）。MAME 自己也只注释掉或缺失，见 `docs/SOC_PORTS.md` §9。
5. **没有 RTC 硬件**：见下节——真机也没有 RTC 芯片，时间本来就靠 2 Hz 中断在软件里累加。
   音频寄存器按 MAME 语义建模并可按需导出 WAV（`docs/AUDIO.md`），但游戏在测过的场景里
   根本不发声（0 次写 `0x3040..0x3043`），所以设备端没有接扬声器。

## 时钟：把真机时间接进来

真机的时钟只有在「设过时间」之后才走（`0x0CB0=1`），启动路径会把它清零，所以原版固件上电后
永远停在 `0:00`。模拟器给了两个开关把宿主时间灌进去（`emu/digicore.exe` 与窗口版都支持）：

| 开关 | 作用 |
|---|---|
| `--clock-sync` | 第 120 帧（2 s）把 `0x0CAF/0x0CAE/0x0CAD` 设成本机时间、`0x0CB0=1`、`0x0CB2=1` 强制刷新 |
| `--clock-fast` | 同时把 `0x0D03`（快进标志）置 1：游戏自己的 2 Hz 处理每拍额外加 `0x3C` 秒，**1 guest 秒 = 1 游戏分钟** |
| 窗口版 F3 | 运行时切换上面的快进标志 |

时钟界面按 **12 小时制**显示，实测对齐：种子 `10:45` → 屏上 `10:45`，`19:45` → `7:45`，
`09:45` → `9:45`，`20:45` → `8:45`，`16:44` → `4:44`。

开了快进之后游戏能真正往前走：按键序列「B1 进设置 → B3 确认」进入育成画面，`--clock-fast` 跑
60 guest 秒（=1 游戏小时）就**破蛋成长期**（`docs/emu_play_sequence.png`：设时钟 → 蛋 → 孵化后的数码兽）。

## 与 MAME 的对拍结果（全部 100%）

| 窗口 | 指令数 | 结果 | 文件 |
|---|---|---|---|
| 0–20 ms | 682,005 | **100.000%** | `dbgtest/cov/A/000_t00000.tr` |
| 900 ms–904 ms | 95,260 | **100.000%** | `dbgtest/cov/A/045_t00900.tr` |
| 5 s–5.004 s | 95,251 | **100.000%** | `dbgtest/cov/A/052_t05000.tr` |
| 20 s–20.004 s | 93,343 | **100.000%** | `dbgtest/cov/A/067_t20000.tr` |
| 0–20 ms 周期数 | 682,005 | **0 处差异** | `dbgtest/mame_cyc.tr` vs `--trace-cycles` |

**对拍必须用当次二进制现生成的 trace**：`dbgtest/emu_t900.tr` 这类旧文件是改动前的产物，
拿它们比会把已经修好的流报成 12.4%（本轮就踩过）。三条现生成的对拍分别是 skip 72 / 84 / 106。

`tools/emu_validate.py` 的 `find_alignment()` 必须把 `max_skip` 放到 16384，并且用 8-PC 前缀
哈希 + 全长度打分：调试器的 `gtime` 会落在几百个周期之外（CPI≈4，即 70–110 条指令），搜索窗
只开 64 会把**完全一致的流**报成 12.4%。上一次「IRQ6 有 85 条指令相位差」的结论就是这么来的，
现已撤销——相位没有差，差的是量具。

周期级对拍的做法：MAME 侧 `trace ...,{tracelog "%06X %d\n",pc,totalcycles}`（`dbgtest/probe_cyc.cmd`），
本模拟器 `--trace-cycles`，两边按 `charge[i] = cyc[i+1] - cyc[i]` 比。MAME 的 trace 动作在**执行前**触发，
本模拟器的 `m_cycle_hook` 在**执行后**，这一点容易搞反。

## 已知差异（都不影响玩）

- **ST7735 只实现 RGB565**（COLMOD 记录但不切换），`fb[]` 在 display off/sleep 时照写，
  绘制时要自己 gate `display_on && !sleep`。
- **LCD 睡眠不建模**：约 17 s（guest 时间）后游戏发 SLPIN，真机黑屏，这里继续画。
- **约 47.7 s 花屏**：`docs/FEASIBILITY.md` 有记录，原因未证实（**推测**是某个未建模的渲染寄存器）。

## 用法

构建：

    powershell -NoProfile -ExecutionPolicy Bypass -File .\emu\build.ps1

交互窗口（在项目根目录运行，需要 `roms/`）：

    .\emu\digimon-player.exe

| 键 | 作用 |
|---|---|
| J / Z / 左 Ctrl | 按键 1（PORTA 0x0008，active low） |
| K / X | 按键 2（PORTA 0x0010） |
| L / C | 按键 3（PORTA 0x0020） |
| F1 | 存 BMP 截图到 `saves/shot_NNN.bmp` |
| F2 | 快进开关（不按实时节奏跑） |
| F3 | 时间快进开关（游戏内 60× 走时，见上文「时钟」） |
| Esc | 退出（退出时把 flash 写回 `saves/flash.bin`） |

标题栏显示帧数、guest 秒数、实测 fps 与当前 PC。窗口在启动后第 120 帧会把本机时间灌进游戏时钟。

无界面驱动器（对拍/回归用）：

    .\emu\digicore.exe --seconds 1.0 --trace dbgtest\emu_t0.tr --trace-seconds 0.02
    .\emu\digicore.exe --seconds 0.25 --ram-at 0.2 --ram-dump dbgtest\emu_ram_t00200.bin
    .\emu\digicore.exe --seconds 4.0 --press "8:60:66" --shot dbgtest\emu_clock.bmp
    .\emu\digicore.exe --frames 3600 --clock-fast --press "8:150:170,20:300:320" --shot-dir dbgtest\grow2 --shot-every 300
    .\emu\digicore.exe --seconds 60.0 --autoplay --wav dbgtest\out.wav --shot-raw dbgtest\fb60.bin
    .\emu\digicore.exe --seconds 20.0 --load-flash saves\flash.bin --save-flash saves\flash.bin

选项一览：`--seconds` `--frames` `--trace` `--trace-start` `--trace-seconds` `--shot` `--shot-raw`
`--shot-dir` `--shot-every` `--press` `--autoplay` `--bp` `--bp-dump` `--ram-at` `--ram-dump`
`--load-flash` `--save-flash` `--clock-sync` `--clock-fast` `--wav` `--trace-cycles` `--irq-log`。

## 验证怎么复现

1. 指令级：MAME 采样窗 `dbgtest/cov/A/000_t00000.tr`（0–20 ms）与本模拟器 `--trace` 输出送
   `tools/emu_validate.py`，期望 `matched 682006 / 682006 (100.000%)`。
2. 像素级：MAME `-aviwrite dbgtest/mame12.avi` 抽第 30 帧（注意 24-bit BI_RGB 是 BGR 顺序），
   与本模拟器 `--shot-raw` 的 `fb` 裁 `(0,18)-(96,114)` 逐像素比，期望 9,216 / 9,216。
3. 交互：`tools/player_tour.ps1` 驱动窗口按 J/K/L 并逐步截图到 `dbgtest/tour/`，
   `dbgtest/tour_sheet.png` 是 8 帧总览（时钟 0:00/1:00/1:01/1:02 → 数码蛋）。
4. 存档：退出 player 后 `saves/flash.bin` 与 `roms/dmc_v1_base.bin` 应只差 8 个字节
   （0x7FD002/0x7FD00A/0x7FD05A/0x7FD05B 与 0x7FE002/0x7FE00A/0x7FE05A/0x7FE05B）。
5. 时钟与育成：`--frames 3600 --clock-fast --press "8:150:170,20:300:320"`，第 120 帧会打印
   `[clock] seeded HH:MM:SS from host`；`--shot-every 300` 出的图里能看到设时钟 → 蛋 → 孵化
   （`docs/emu_play_sequence.png`）。

## 设备侧（ESP32-C3 / AI Passport）

同一份解释器与 SoC 模型已经移植到 FoloToy AI Passport 并在 COM4 上跑通：
`BOOT_OK`、第 60 帧的 `fb` 与 PC 版**逐像素 0 差（9,216/9,216）**，但只有 **1.2 guest fps**
（`mi_s=0.73`，每个 guest 指令约 219 个主机周期）。细节、内存地图、稀疏 flash 存储与缺口见
`docs/C3_PORT.md`；刷机用 `firmware/digicore-c3/digicore-c3-full.bin`。设备端没有 RTC，
上电后要先在设置界面按键设时间——和真机一致。

## 下一步

1. **速度**：C3 上解释器只有 1.2 fps，正路是把 `recomp/` 的静态重编译铺到全部 762 个基本块
   （`firmware/recomp-bench` 实测块级 80.68 M guest instr/s，约 10 倍余量）。
2. **覆盖率复核**：用本模拟器跑 60 s 的 PC 流，与 `docs/COVERAGE.md` 的 4,788 唯一 PC 对照，
   确认解释器走的是同一批代码（预计只多不少）。
3. **47.7 s 花屏**：找一个 guest 时间的窗口，把 MAME 与 PC 版的 `fb` 并排比，定位未建模寄存器。
4. **音频**：模型与 WAV 导出已就绪（`docs/AUDIO.md`），等找到会写 `0x3041` 的场景再接扬声器。
