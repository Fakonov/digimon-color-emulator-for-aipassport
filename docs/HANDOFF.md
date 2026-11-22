# HANDOFF

## 最新（2026-11-20 23:3x）— 交接给新对话：走 AOT 路线

- 用户拍板（原话）：「按你建议的顺序来。但是交接给新对话执行。」→ **施工图在 `docs/PLAN_AOT.md`**，
  新对话先读它 §8 的「开场动作」，按 T1→T6 推进。
- 设备现状：COM4 已刷 `firmware/digicore-c3/build/digicore-c3.bin`（含 IRAM 热函数、1:1 正方形窗口、
  左上角 fps 浮层、`int_ctrl`/`rows` 诊断字段），静止 **58.4–58.9 guest fps**，画面正方形居中、四周纯黑。
- 唯一已知瓶颈：**换屏/按键时每帧要解释 162,000–270,000 条 guest 指令**（静止时只有 3,700 条），
  解释器约 **200 主机周期/指令** → 那一秒 fps 掉到 3，按键要 300 ms 才有反应。
- 已实测无效、**别再试**：ISA 合并 TU、jump table、取指窗口缓存、fast ops 内联、mmap、`-flto`。
  IRAM 搬九条热函数只带来换屏 9.6 → 11.3 fps 的边际收益（保留，非关键）。
- 下一步（PLAN_AOT.md 的 T1）：写 `tools/aot.py`，先覆盖 5 种指令 + 解释器回退，跑通逐指令对拍，
  再按动态频次扩到 762 个块。目标：换屏 < 20 ms、静止锁 60 fps、像素 0/9216、存档 0 字节差。

## 最新（2026-11-20 22:5x）— 设备侧锁 60 fps

- COM4 已刷 `firmware/digicore-c3/build/digicore-c3.bin`（382,992 B，sha256 前 16 位 `1BC109527C2F55E4`，
  只刷 app 区 0x10000，ROM 分区未动）。实测 **60.00 guest fps** 连续 60 秒，`blit_pct=3`。
- 速度阶梯：1.09 → 7.82（重编译 PRNG 块）→ 14.15（等待循环折叠）→ 24.5（blit 重写）→ 79.7（切片/
  定时器三项）→ 60.00（绝对截止时间节拍）。细节见 `docs/C3_PORT.md` §8.2.3。
- 设备仍有的限制：时钟靠编译时间播种（无 RTC）、音响未接（游戏不发声）、按键一次生效需 5 帧拉伸。
## 当前状态

- 阶段：P0 完成；静态重编译 PoC v0 已完成（recomp/），C3 wasm 模拟器与 **COM4 真机**块级 benchmark 均 PASS（firmware/recomp-bench/）。另有 `firmware/color-preview/` 可见画面固件已构建并烧写到 COM4（15 秒串口无 panic，屏幕肉眼待确认）：它只回放 MAME 参考帧，验证 ST7789P3/背光/SPI DMA，不是模拟器。两个测试固件都不是最终模拟器；recomp-bench 只验证两个翻译块且不初始化 LCD，黑屏是预期；color-preview 才有画面，并带运行时方向测试（任意键循环 0/90/180/270；2026-11-20 14:06 用户目视确认四向旋转正常）。
- **最新（2026-11-20 21:4x）**：设备侧 **7.69 → 14.15 fps**。先在设备上量了不带探针的微基准：`hot_block_prng` 81 周期/次（68 条指令 = 1.2 周期/条）、一次 `fetch16` 49 周期、一条 `execute_one` 163 周期 → 一条解释指令 **227 周期**；而主循环 `0x230321` 是等 64 Hz 节拍的忙等，每圈 74 条指令 298 周期，其中 6 条外壳走解释器要 6×227≈1,362 周期（占一圈 82%）。于是写 `unsp_device::wait_loop_collapse()` 把**整圈原生化**（跑现成的 PRNG 块 + 手动 `pop bp` + 把 PC 置回 0x230323 + 按 `ds=0/r4=0x0850/r4=[r4]/cmp/je` 收尾），按 298 周期/圈计费，`m_sirq` 一到就交还解释器。踩坑：这台 CPU 的 `call` **只压返回地址**（不压 SR），第一次按「压 PC+SR」实现，SP 每圈漂一格，四万圈后栈被冲穿（`PC=004020 SR=8280 SP=C099`）。正确性：PC 侧 20 s / 60 s 存档与解释器路径**0 字节不同**、帧缓冲一致、周期计数完全相同；设备第 60 帧 dump 与此前参考帧 sha256 一致。app 379,456 B / SHA256 `2B38A234E353B05E2EED1097741D1B98FC916090EADA499291E6179F66C203CF`。
- **最新（2026-11-20 19:4x）**：设备侧速度 **1.09 → 7.82 fps（7.2 倍）**，`mi_s` 0.73 → 3.15。做法是先量分布：`--pc-hist` 显示 `0x2002F4..0x200343` 那 68 条指令的 PRNG 块占**全部动态指令的 87%**（每 PC 3,097,129 次），于是用 `tools/gen_hotblock.py` 把真机 golden 验证过的 `recomp/hot_func.c` 机械转换成 `emu/hotblock.h`，直接对 SoC 实时 RAM 运行，尾部 `pop bp, pc` 留给解释器，每调用计 263 周期；再加 RAM 平坦窗口、`spin_collapse()` 折叠两个倒计数循环、组件级 `-O3`。正确性：PC 侧 20 s 存档两条路径 **0 字节不同**，设备第 60 帧与 PC **0/9216 像素不同**。app 362,800 B / SHA256 `36123E7DB88F0B0F…`。要摸 60 fps 还需把 `0x230000` 主循环与 `0x5xxx` UI 的热块也静态重编译（见 `docs/C3_PORT.md` §8.2）。
- **最新（2026-11-20 18:0x）**：按键的第二个坑——**guest 要连续 4 帧按下才认**。PC 上实测：给 1/2/3 帧按下屏幕不动，4 帧才切到设置界面（游戏自己去抖是按 60 Hz 帧算的约 60~70 ms）。设备上 1 guest 帧 = 0.9 s，所以帧循环把一次轻点**拉伸成 5 帧按下 + 1 帧松开**（`kPressFrames = 5`）。代价是每次按键约 5 秒才生效；根因是整机只跑 1.09 fps（比掌机慢 ~55 倍），不是输入通路的问题。同时开机先 blit 全黑抹掉 ST7789 随机 GRAM。app 357,616 B / SHA256 `25CF70A5…4075D222`，合并镜像 4,677,888 B / SHA256 `08018E05…3B18EBD6`。
- **最新（2026-11-20 17:5x）**：`buttons.c` 改为 20 ms 轮询任务 + 按下锁存器（`buttons_state()` / `buttons_take_latched()`），修掉“帧循环里同步采样、一帧 0.9 s 所以轻点必丢”的问题；每秒日志加 `mv=`/`btn=`，按下时的阶梯电压可直接看。
- **最新（2026-11-20 17:4x）**：设备侧方向修好了。`firmware/digicore-c3/main/app_main.cpp` 的 `kRot` 从 2（180°）改成 **0**——第一版照 `color-preview` 转了 180°，用户看了一眼说“上下颠倒”，说明 `display.c` 里 `esp_lcd_panel_swap_xy(true)` + `mirror(true,false)` 已经把面板摆正，应用层再转就多转了。`rot_map()` 另加了 `kRot=4/5`（只水平镜像 / 只垂直镜像）两个档，万一镜像反了也是一行改。顺带记录：`color-preview` 当年默认 180° 的“逐像素一致”是对着同一次假设生成的期望图比的，属于自证，不能当作面板方向的证据。
- **最新（2026-11-20 17:1x）**：PC 侧现在能**真正玩到育成**：`--clock-sync` 把本机时间灌进游戏时钟（真机没 RTC），`--clock-fast` 让 1 guest 秒 = 1 游戏分钟，窗口版按 **F3** 切换。`--frames 3600 --clock-fast --press "8:150:170,20:300:320"` 60 guest 秒就**破蛋**（`docs/emu_play_sequence.png`）；`--frames 36000`（600 guest 秒 = 10 游戏小时）1.6x 实时跑完退出码 0，背景进入黄昏、场上多出野生数码兽（`docs/emu_ten_minutes.png`）。另：**对拍必须用当次二进制现生成的 trace**，`dbgtest/emu_t900.tr` 这类旧文件会把已修好的流报成 12.4%；`tools/emu_validate.py` 已改成 8-PC 前缀哈希（秒级）并放宽到 0.999。
- **最新（2026-11-20 16:xx）**：原版游戏**在 AI Passport 上跑起来了**。`firmware/digicore-c3/`（ESP32-C3 版解释器 + GPCE4 + ST7735 + 稀疏 flash 存储）已构建并刷进 **COM4**，串口 `BOOT_OK ... madctl=00a8`，稳态 `guest_fps=1.22 mi_s=0.73`；第 60 帧的 `fb` 与 PC 版 `--frames 60 --shot-raw` **0/9216 像素不同**（`docs/c3_device_frame60.png`）。速度不是实时，瓶颈是解释器每个 guest 指令约 219 个主机周期，提速正路是静态重编译（`docs/C3_PORT.md` §8）。整片备份 `docs/backup/digimon-color-passport-COM4-8MB-20261120-1218.bin` 未覆盖，回滚可用。
- **最新（2026-11-20 16:xx）**：四顶「欠账」收口——① IRQ6 相位差**不存在**，是 `tools/emu_validate.py` 的 `find_alignment()` 只搜 skip 0..63 造成的假象，`max_skip=16384` 后 900 ms / 5 s / 20 s 窗口全部 **100.000%**；② 真机**没有 RTC 芯片**，时间靠 IRQ7 + 2 Hz 软件累加，启动路径主动把 `0x0CB0` 清零所以冻结在 0:00:00（`docs/RE_RTC.md`）；③ `0x0600..0x0607` 是 128-bit PRNG 在忙等里被搅的混沌量，不能当对拍判据；④ 音频见 `docs/AUDIO.md`。另新增逐条周期对拍：682,005 条指令 **0 处周期差异**。
- **最新（2026-11-20 15:0x）**：PC 侧 unSP 模拟器第一版可玩，在 `emu/`，见 `docs/EMU.md`。`emu/digimon-player.exe` 是交互窗口（3 键、4 倍放大、F1 截图、F2 快进、Esc 退出并把存档写回 `saves/flash.bin`），`emu/digicore.exe` 是无界面对拍/回归驱动器。实测：0–20 ms 指令流与 MAME **682,006/682,006 全对**；0.5 s 画面与 MAME `-aviwrite` 帧 **9,216/9,216 像素相同**；t=200 ms RAM 只差 4 字；60 s autoplay 1.3x 实时无异常；按键能走到时钟与数码蛋育成画面，重启后显示「ロード しゅうりょう」。
- **最新（2026-11-20 13:2x）**：`firmware/color-preview` 已加“屏幕 dump”，并刷到 COM4。当前镜像 app **666,000 B** / SHA256 `E16E9552FB696881D1976B10EAEC0A2B2B415BC8BBC50FE8BE598590D75AD162`，full **731,536 B** / SHA256 `9ADDC29B5EC83931736EFD6E3D26AADF0B6F4862C27FFCCD65B97F14A9CCB3A2`。上电静态图/按键旋转后自动把 320×240 RGB565 framebuffer 以 RLE 打到串口；`boot_dump.log` 728 行，解码 `docs/device_shot_boot.png` 与 `docs/static_expected_180.png` 逐像素一致。空载 ADC 2944 mV、down=0；2026-11-20 14:06 用户目视确认四向旋转正常，key dump/屏幕照片尚未采集。下文 13:1x 段落里的 665,184/730,720 和“没有 dump”已被本段取代。
- 唯一权威报告：docs/FEASIBILITY.md
- 当前 **COM4 在线**（2026-11-20 19:4x），已刷 `firmware/digicore-c3/build/digicore-c3.bin` 到 app 区（原版游戏模拟器，**7.82 guest fps**，ROM 分区 0x210000 未动）。回滚整片备份：`docs/backup/digimon-color-passport-COM4-8MB-20261120-1218.bin`（8,388,608 B，SHA256 `C74EA4FF72A82F54326892D71F7A4081C55958A0D8B029190660742004CA48F9`）。不要刷 COM6（Cardputer ADV），也不要拿旧项目的 E0C6200 核心来套彩屏机。

## 环境

- 项目目录：C:\Users\DuWeisi\digimon-color-passport
- MAME：C:\Users\DuWeisi\mame\bin\mame.exe（0.289）
- romset：C:\Users\DuWeisi\mame\roms\digicolr.zip
- ESP-IDF：C:\Users\DuWeisi\idf-setup\esp-idf-5.5.3
- 工具链：C:\Users\DuWeisi\.espressif
- Python：C:\Users\DuWeisi\py311\python.exe
- w64devkit：C:\Users\DuWeisi\w64devkit
- 真机：**COM4**（COM6 是 Cardputer ADV，别刷错）
- 工具链装载：. "$env:USERPROFILE\passport-toolkit\env.ps1"

## ROM 校验

- internal_rom_221017a.bin：65,536 B，CRC32 10591b51，SHA1 78c11913e8ebd89a64b575d85bfe9b37aafb3bc0
- dmc_v1_base.bin：8,388,608 B，CRC32 23227806，SHA1 ebae139f708d36151efcaf2db7c329c6614d7e68

## MAME 跑法

    cd C:\Users\DuWeisi\mame
    .\bin\mame.exe digicolr -seconds_to_run 45 -nothrottle -skip_gameinfo -aviwrite C:\Users\DuWeisi\digimon-color-passport\gateA_keepalive.avi -autoboot_script C:\Users\DuWeisi\digimon-color-passport\tools\keepalive.lua -log

- 按键 active-low：PORTA 的 P1 Button 1/2/3 = bit 0x08/0x10/0x20，Lua 里 set_value(0) 是按下，set_value(1) 是松开。
- 无输入约 17.0 s 会睡眠；17 s 后要每 6 s 注入一次按键，否则复位/黑屏。
- 约 47.7 s 会出现 LCD/SPI 花屏，不能作为 golden。
- DIP "Charger Connected" 用 Lua set_value 无效（ioport.cpp:772-778 只改 m_digital_value，不改 live value）；-cfg_directory 写 DIP 也会被启动时重写。当前不阻塞 Gate A。

## 调试器 trace

    .\bin\mame.exe digicolr -debug -debugger windows -debugscript <cmd> -video none -sound none

脚本示例：

    gtime 2000
    trace C:\Users\DuWeisi\digimon-color-passport\dbgtest\g2s.tr,0,noloop
    gtime 10
    trace off,0
    quit

- 必须用 -debugger windows；-debugger none 会忽略脚本。
- 5 s 处长 trace 会因无 DRC 极慢，120 s 超时；g2s.tr 是 2 s 处 10 ms 的可用样本。

## 关键数字

- guest 指令率：38.1264 M instr/s（381,264 条 / 10 ms）
- 官方周期重算：629,394 cycles / 10 ms，CPI 1.6508
- 96 MHz 占用 65.6%；48 MHz 为 131%
- C3 解释器估算：5.33–8.0 M instr/s（20–30 host cycles/instr），含缓存惩罚 4.27–6.4 M
- 实时预算：≤4.20 host cycles/guest instr（对 38.13 M），软件解释器做不到
- 代码局部性：6 个 4 KiB 页；0x200 页 91.69%，0x200+0x230 99.79%
- 稀疏镜像：611 个非零页，2,502,656 B；分区 6,225,920 B
- 写路径未验证：若游戏运行期写 SPI flash，需要 dirty-page overlay（写过的 4 KiB 页落 NVS）；P1 统计 SPI 0x02/0x20/0xD8 命令

## 下一步

- 选 A（C3 慢速）：P1 先写 PC 侧 unSP 解释器，用 g2s.tr 做指令级回归；再 P2 固件骨架。
- 选 B（换硬件）：先定板型和屏幕接口，再复用 P1 解释器。
- 选 C（静态重编译）：已做 PoC v0，见下节；下一步扩基本块和内存映射。

## 静态重编译 PoC v0（2026-11-20）

- 位置：`C:\Users\DuWeisi\digimon-color-passport\recomp`
- 入口文档：`recomp/README.md`
- 一键复现：`powershell.exe -NoProfile -ExecutionPolicy Bypass -File recomp/build.ps1`
- 已翻译：
  - `0x2002F4..0x200343`，68 条指令的 32-bit hash 函数 → `recomp/hot_func.c`
  - `0x5376..0x537E`，8 条指令、1000 次循环的延时循环 → `recomp/delay_func.c`
- golden：
  - `dbgtest/entry_ram.bin` / `dbgtest/exit_ram.bin`（2 s）
  - `dbgtest/entry2_ram.bin` / `dbgtest/exit2_ram.bin`（10 s，入口寄存器相同、RAM 不同）
  - `dbgtest/delay_entry_ram.bin` / `dbgtest/delay_exit_ram.bin`
  - 生成脚本：`dbgtest/bp_func.cmd`、`dbgtest/bp_func2.cmd`、`dbgtest/bp_delay.cmd`
- 验证：两个 harness 的 RAM 全 4K word memcmp 与寄存器检查均 PASS；hash 函数两组快照（2 s/10 s）都过。
- x86-64（GCC -O2）：hash 约 35–48 cycles/call、0.51–0.71 cycles/guest insn；延时循环约 12.3k cycles/call、1.54 cycles/guest insn。
- RISC-V：hash 静态 126 条、0 分支；延时循环 76 条、2 分支；C3 真机 cycle 已测（COM4，见下节），wasm 模拟器也已测。
- 当前只映射 RAM（`0x0000..0x0FFF`）；没有 internal ROM、SPI flash、IO、中断、banked reg、MULS/BITOP/FIR_MOV。
- 重要：P0 报告的 38.1264 M/CPI 1.6508 是 MAME 96 MHz 观测；原机 GPCE4064B 最高 48 MHz，真实目标 23.4–26.9 M instr/s。P1 逐帧对齐前必须把 MAME 改 48 MHz 或按 guest cycle 对齐。
- 下一步：真机块级复核已在 COM4 完成（见下节）；扩 `0x230321` 调用者（call/ret/push/pop）；接 internal ROM + SPI flash 4 MB 窗口；再做 0–17 s golden 段。

## C3 wasm + COM4 实测（firmware/recomp-bench，2026-11-20）

- 入口：`firmware/recomp-bench/README.md`；一键 `build.ps1 -Sim`。
- 测试固件把 `recomp/hot_func.c` 与 `recomp/delay_func.c` 编进 ESP32-C3，先做全 4K word memcmp + 寄存器检查，再跑 benchmark。
- 关键坑：`State` 约 8 KiB，必须静态分配；放 `app_main` 栈上会踩爆 main task 默认栈，表现为打印 start 后无输出。
- 关键坑：`CONFIG_ESP_TASK_WDT_EN=n` 时 `esp_task_wdt_deinit` 未编译，链接会 undefined reference；测试固件已删该调用。
- 构建：ESP-IDF 5.5.3，`idf.py -B build -DIDF_TARGET=esp32c3 build`；最终合并镜像 `recomp-bench-full.bin` **273,712 B（0x42D30）**，SHA256 `F04BDB757E44A85E94C6971B600C7B94709DBCC4B0F93FB9B97C968FE79BD12D`；app 208,176 B（0x32D30），SHA256 `B7983294E10D9304B37EB6C74783D4EBAEFE7FEAA9E06B1088F2583E36A964BF`。
- 控制台：UART0 primary + C3 原生 USB-Serial/JTAG secondary；wasm 模拟器（UART0）和 COM4（USB）共用同一镜像。日志里会看到 `GPIO 20 and 21 are used as console UART I/O pins`。
- 模拟器：AI Passport wasm（esp32c3，160 MHz），同一最终镜像连续复跑：
  - `HOT_PROFILE0 PASS`、`HOT_PROFILE1 PASS`、`DELAY PASS`
  - `HOT_BENCH` 130 cycles/call、1.91 cycles/guest、83.4 M guest instr/s
  - `DELAY_BENCH` 46,212 cycles/call、5.77 cycles/guest、27.7 M guest instr/s
  - `SIM_RESULT PASS failures=0`
- **真机 COM4（2026-11-20）**：erase_flash + write_flash + verify_flash（digest matched）；设备 ESP32-C3 QFN32 rv1.1、8 MB XMC、MAC `4c:11:ae:2f:3f:d4`。日志 `firmware/recomp-bench/device_run.log`：
  - `HOT_PROFILE0 PASS`、`HOT_PROFILE1 PASS`、`DELAY PASS`、`SIM_RESULT PASS failures=0`
  - `HOT_BENCH n=200000 cycles=26970559 cycles_per_call=134 cycles_per_guest=1.98` → **80.68 M guest instr/s**
  - `DELAY_BENCH n=2000 cycles=98745704 cycles_per_call=49372 cycles_per_guest=6.16` → **25.94 M guest instr/s**
- 2026-11-20 07:39 重刷复核：erase 1.0 s → write/verify digest matched，日志 `firmware/recomp-bench/reflash_run.log`；数值同 `device_run.log`（134/1.98/80.68 M、49,372/6.16/25.94 M），仍 PASS。
- 解释：对照 48 MHz 原机 23.4–26.9 M，hot 约 3.0–3.4 倍，delay 约 96%–111%；两个块都在区间内或以上。5.77/6.16 cycles/guest 主要是 C 代码膨胀（约 45 条 host 指令对应 8 条 guest）。**这是块级结果，不是整机实时证明**。
- 日志：`firmware/recomp-bench/sim_run.log`（模拟器）、`firmware/recomp-bench/device_run.log`（COM4 首轮）、`firmware/recomp-bench/reflash_run.log`（2026-11-20 07:39 重刷复核）。
- 未做：internal ROM、SPI flash、LCD、IRQ、完整帧、整机解释器；真机只跑了块级 benchmark。

## color-preview 可见画面固件（2026-11-20，已上 COM4）

- 位置：`firmware/color-preview/`；入口文档：`firmware/color-preview/README.md`。
- 当前状态：已烧写到 COM4 并启动；`PREVIEW_ROT 180`、`PREVIEW_READY first_frame=0 rot=180`、`frame 0/150` 到 `frame 120/150` 均出现，15 秒无 panic。屏幕上的红条/文字方向待用户肉眼确认。
- 产物：
  - app `build/color-preview.bin` 651,680 B，SHA256 `87EFB2D9E5C59FBCA7F36ABA00D3DEA519B260AFCEFCB65D8625ABEB480E46E8`
  - 合并镜像 `color-preview-full.bin` 717,216 B，SHA256 `CBCA3DF60D83AB08F27A152FCB06770CBBFBD811A39E4A63E37F0025549390CB`
- 构建：`powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1`；本机没有 `pwsh`，默认执行策略会拦截 `.ps1`。
- 烧写：设备接 **COM4**；已执行 `read_flash 0x0 0x800000` 到 `docs/backup/digimon-color-passport-COM4-8MB-20261120-1218.bin`（8,388,608 B，SHA256 `C74EA4FF72A82F54326892D71F7A4081C55958A0D8B029190660742004CA48F9`），`verify_flash` digest matched；随后 `write_flash 0x0 color-preview-full.bin` 并再次 verify。不要刷 COM6。
- 2026-11-20 真机首刷踩坑：`tinfl_decompress_mem_to_mem` 在 8 KB 任务栈上触发 `Stack protection fault`，`MEPC 0x40038aaa`、`RA 0x42008d54`（`preview_main.c:68`）。已把 `preview` 任务栈从 8,192 提到 32,768，复刷后 15 秒稳定。日志：`device_run_15s.log`（3,654 B，SHA256 `A7C01DA1D06E0FE146EEF266F89FC1A9FCC62E174196804F89D45BE20A419187`）。
- 画面：上电先做 2.5 秒方向测试（第 60 帧 + 顶部红条/底部绿条），任意键循环 0/90/180/270；之后 96×96 参考帧 ×2 居中（192×192，left 64 / top 24），黑边；前 2 秒 boot logo，随后时钟/设置 UI；10 fps，150 帧，15 秒后从第 20 帧循环。数字不会真的走时。
- 帧包：`main/frames.bin` 433,520 B，SHA256 `4aa2ac9e0979a71a8f1a6b5dc045b0acb08b7b76aeb301598adf59e4d8fa6ed1`；RGB565 大端，zlib，DCPV 头 + 偏移表。
- 方向：帧包保持 raw 方向，默认运行时旋转 180°（`s_rot=2`）。依据是 raw 帧里 `SET` 的 T 横条在下方、以及同系列 Color 手册（Godzilla/Pendulum）“时间在上、SET 在下”的布局；没有 Digital Monster Color 实机照片，因此保留任意键循环。红色上边条始终画在输出坐标顶部，用来判断“上”。
- 串口：UART0 primary + USB-Serial/JTAG secondary；正常日志 `PREVIEW_ROT 180`、`PREVIEW_READY first_frame=0 rot=180`、`frame 0/150`；失败会打 `PREVIEW_RESULT FAIL` 并显示红蓝棋盘格。
- 边界：不是模拟器；不执行 internal ROM / SPI flash 镜像；没有 CPU、定时器、中断、SPI flash、按键、RTC、存档、音频。后续接 unSP 时必须保留 ROM 外置和 `NO ROM` 不 step。
## 2026-11-20 13:1x — 静态验收图与 ADC 按键（COM4）

- 目标：把“屏/方向问题”和“MAME 参考视频自带的滚动/镜像”分开；同时修正按键读取（旧版把 GPIO0 当数字键，实际是 ADC 电阻梯）。
- 静态验收图（`firmware/color-preview/main/preview_main.c` 的 `show_static_pattern()`）：
  - 黑底 96×96，四角 16×16 色块：上左红、上右绿、下左蓝、下右白；中间白色向上箭头（源坐标）。
  - 先按 `s_rot` 旋转、再 2× 放大到 192×192 居中；红条固定输出顶部 8 px，绿条固定底部 8 px。
  - 当前默认 `s_rot=2`（180°）：屏幕应为上左白、上右蓝、下左绿、下右红，箭头向下。
  - 启动显示 15 s；任意键边沿 `s_rot=(s_rot+1)&3`，重显 5 s 后从当前帧继续；静态轮询 20 ms。
  - PC 期望图：`docs/static_expected_180.png`、`docs/static_expected_0.png`、`docs/static_expected_rotations.png`（0/90/180/270）。
- ADC 按键：
  - ADC1_CH0（GPIO0），12 dB，4 次平均，`adc_cali_create_scheme_curve_fitting`；初始化失败回退 GPIO 上拉数字读取。
  - 按下 <1900 mV，释放 >2100 mV；阈值来自旧项目 `bsp_button.c` 的 `BSP_BTN_MV_TABLE`：key0 0–150、key1 150–447、key2 447–1900、释放 >1900 mV。
  - 真机空载：`button boot mv=2944 down=0`；ADC 校准/释放判定有效。
  - **未实测真实按键边沿**：`firmware/color-preview/device_watch.log` 已抓 181 s（日志到 177543），没有出现 `button edge mv=... -> rot=...`；说明期间无人按键，不是代码报错。
  - `firmware/color-preview/README.md` 13:1x 时为 6,808 B / 153 行；13:2x 加入 dump 后为 8,401 B / 187 行。
- 构建与烧写：
  - `powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1`
  - 历史构建（已被 13:2x 取代）：app `build/color-preview.bin` 665,184 B / SHA256 `9D38DBDD1F18FB551CC28C54A1D8AB32B8C4E2857498A906E210FD131EC306CF`
  - 历史构建（已被 13:2x 取代）：full `color-preview-full.bin` 730,720 B / SHA256 `79E749C881B27AE835FFDB71C09B06098A8472A0EE8F479B83F20F3F15FA83F8`
  - COM4 `write_flash 0x0`，esptool `Hash of data verified`；设备 ESP32-C3 QFN32 rv1.1 / 8 MB XMC / MAC `4c:11:ae:2f:3f:d4`。
  - 整片备份仍为 `docs/backup/digimon-color-passport-COM4-8MB-20261120-1218.bin`（8,388,608 B，SHA256 `C74EA4FF72A82F54326892D71F7A4081C55958A0D8B029190660742004CA48F9`），未覆盖。
- 串口工具 `tools/reset_capture.py`：打开 COM4 后 DTR=IO0 保持高、RTS=EN 拉低 100 ms 再释放，正常启动到 app；随后按秒抓日志。注意不要用 esptool 的 USBJTAGSerialReset 序列（DTR=IO0 低），会进下载模式。
- 启动日志关键行：
  - `button ADC ready: press < 1900 mV, release > 2100 mV`
  - `PREVIEW_STATIC rot=180 (red top, green bottom; arrow up at rot=0)`
  - `button boot mv=2944 down=0`
  - `static done; video from frame 0` / `PREVIEW_READY first_frame=0 rot=180`，无 panic。
- 未完成：真实按键边沿、照片/录屏、红条物理位置与四角颜色肉眼确认；参考帧视频仍有 MAME 自身滚动/错位，未被本固件修复。

## 2026-11-20 13:2x — 屏幕 dump（COM4）

- 用户指令：红条一直在上方；给测试固件加屏幕 dump，由助手侧观察。
- 实现（`firmware/color-preview/main/preview_main.c`）：新增 `dump_screen(why)`，打印 `SHOT_BEGIN 320 240 RGB565 rot=... why=...`，每 16 行 `render_chunk` 后用 `dump_rle_row` 输出 `SHOT_ROW <y> <x> <count> <hex>`，顶部 8 行红条 0xF800、底部 8 行绿条 0x07E0，最后 `SHOT_END`。每块 `fflush(stdout)` + `vTaskDelay(1)`。上电静态图后自动 `why=boot-static`；按键旋转后 `why=key`。dump 的是**送面板的 framebuffer**，不读 ST7789 GRAM。
- 修正 `static_until` 声明顺序，使 boot/key 两条路径都能赋值。
- 构建：`powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1`
  - app `firmware/color-preview/build/color-preview.bin` **666,000 B** / SHA256 `E16E9552FB696881D1976B10EAEC0A2B2B415BC8BBC50FE8BE598590D75AD162`
  - full `firmware/color-preview/color-preview-full.bin` **731,536 B** / SHA256 `9ADDC29B5EC83931736EFD6E3D26AADF0B6F4862C27FFCCD65B97F14A9CCB3A2`
  - 1 MB app 分区剩 36%。
- 刷机：COM4 `write_flash 0x0`，esptool `Hash of data verified`。
- 抓取：`tools/reset_capture.py COM4 firmware/color-preview/boot_dump.log 45`。日志 23,415 B；`SHOT_BEGIN ... why=boot-static` 第 133 行，`SHOT_END` 第 1591 行，`SHOT_ROW` 共 728 行；`button boot mv=2944 down=0`；`static done; video from frame 0`；无 panic。
- 解码：`C:\Users\DuWeisi\py311\python.exe tools\shot_decode.py firmware\color-preview\boot_dump.log docs\device_shot_boot.png`，输出 320×240、755 B。与 `docs/static_expected_180.png` 逐像素比较 `bbox=None` / `same_pixels=True`。采样色：上条红 (255,0,0)、下条绿 (0,255,0)、上左白、上右蓝、下左绿、下右红、箭头白、背景黑。
- 结论：默认 180° 的送屏内容正确；红/绿条固定是设计，按键只旋转内容。dump 不能替代摄像头，玻璃显示仍需照片/人眼。
- 2026-11-20 14:06 用户反馈四向旋转“好像没问题”（人工目视确认；未抓 key dump）。
- 未完成：key dump、屏幕照片；参考帧视频的 MAME 滚动/错位未修。均不阻塞显示链路验收。

