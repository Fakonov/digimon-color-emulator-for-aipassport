# PROGRESS

## 2026-11-19 — P0 可行性审计完成，停在决策点

- 阶段：P0（只做审计，未写任何固件代码）
- 报告：docs/FEASIBILITY.md（当前 25,300 B / 454 行，含第 10 节 COM4 真机复核；P0 原始版为 20,462 B / 415 行，2026-11-19 23:58 实测）
- 结论：
  - 闸门 A：有条件通过。MAME 0.289 能启动 digicolr、出 96×96 彩色画面、响应三键；无输入约 17.0 s 进入未实现的 System_Sleep，25 s 左右重新出现 logo（推测复位），47.7 s 后 LCD/SPI 花屏。
  - 闸门 B：通过。8,388,608 B 镜像中全零页 1,437/2,048，非零页 611 页，4 KiB 稀疏存储 2,502,656 B（2.39 MiB）；roms 分区 6,225,920 B（5.94 MiB），余量 3,723,264 B。无需压缩即可放下。
  - 闸门 C：不通过。实测 guest 指令率 38.13 M instr/s，官方周期 CPI 1.6508；C3 解释器估算 5.3–8.0 M instr/s，只有需求的 14%–21%，差距 5–7 倍。
- 已跑关键实验：
  - MAME 基线 30 s：gateA_30s.avi（52,871,244 B，1802 帧）
  - 45 s keep-alive：gateA_keepalive.avi（79,208,332 B，2864 帧）
  - 唤醒测试：gateA_wake.avi（70,419,532 B，2547 帧）
  - 2 s 处 10 ms unSP trace：dbgtest/g2s.tr（8,038,660 B，381,264 条）
  - 官方周期重算：dbgtest/analyze_official.py
- 待拍板（三选一）：
  1. 接受 C3 慢速（8–12 fps，慢 5–7 倍），继续 P1/P2/P3。
  2. 换硬件（ESP32-S3 + PSRAM，或 Linux SBC），保持实时目标。
  3. 停在 P0，先做静态重编译/混合方案预研。
- 在收到选择前：不动固件、不烧机、不开始 P1。

## 未做

- C3 真机 host cycle benchmark（P0 时未做；2026-11-20 已在 COM4 完成，见下文）。
- MAME sleep/wake 补丁（只做诊断）。
- 音频反推（压电片，MAME 无实现）。
- 完整 gameplay 验证（当前只到时钟/设置界面）。

## 2026-11-20 — 静态重编译 PoC v0（recomp/）

- 用户选 A 路线后，按“静态重编译先出一版”执行；当时没有动固件、没有烧机。
- 范围：只做 PC 侧、trace-guided 静态翻译，不接 LCD/SPI/中断/完整内存映射。
- 已翻译两段真实 guest 代码：
  - `0x2002F4..0x200343`：68 条指令的 32-bit 状态 hash 函数（`recomp/hot_func.c`）
  - `0x5376..0x537E`：8 条指令、循环 1000 次的延时循环（`recomp/delay_func.c`），验证条件分支 + 回跳
- 生成器：`recomp/gen_recomp.py` 读 `dbgtest/hot.asm`；`recomp/gen_delay.py` 读 `dbgtest/delay.asm`；运行时无取指/解码/dispatch。
- 验证 golden：MAME 0x289 断点抓取 `entry_ram.bin`/`exit_ram.bin`（2 s）与 `entry2_ram.bin`/`exit2_ram.bin`（10 s，另一组数据），以及 `delay_entry_ram.bin`/`delay_exit_ram.bin`；两个 harness 全 4K word memcmp + 寄存器检查，三组快照均 PASS。
- 关键寄存器结果：hash 函数 exit `r0=0FF5 r1=90F0 r2=1F30 r3=C08D r4=0601 r5=0FF3 r6=0020 r7=200343 sb=1`；延时循环 exit `r4=03E8 r5=0FF5 r6=0040 r7=537F sb=0`，`[bp]=03E8`。
- x86-64（w64devkit GCC -O2）：hash 约 35–48 host cycles/call（0.51–0.71 cycles/guest insn）；延时循环约 12.3k cycles/call（含状态重置，1.54 cycles/guest insn）。
- RISC-V（riscv32-esp-elf-gcc -O2 -march=rv32imc）：hash 静态 126 条、0 分支；延时循环 76 条、2 分支、循环体约 45 条。
- 粗略 C3 160 MHz 估算：hash 43–86 M guest insn/s（该块），延时循环 14–28 M guest insn/s（该块）；对照 48 MHz 目标 23.4–26.9 M，纸面已进区间，但整机远未证明实时。
- 重要修正：P0 报告的 38.1264 M/CPI 1.6508 是 MAME 96 MHz 观测；原机 GPCE4064B 最高 48 MHz，真实目标是 23.4–26.9 M instr/s。已在 `docs/FEASIBILITY.md` 末尾加补记。
- 当前边界（当时）：只有 RAM 映射；没有 internal ROM/SPI flash/IO/中断；没有 banked reg/MULS/BITOP/FIR_MOV；没有 C3 真机 cycle（现已在 COM4 完成）；没有完整帧。
- 下一步：C3 最小 benchmark 已在 wasm 模拟器完成（见下节），真机复核已在 COM4 完成（见下节）；然后扩到 `0x230321` 调用者（call/ret/push/pop）；加 internal ROM + SPI flash 4 MB 窗口；P1 逐帧对齐前把 MAME 时钟改 48 MHz 或按 guest cycle 对齐。

## 2026-11-20 — C3 wasm 模拟器实测（firmware/recomp-bench）

- 用户要求完成后先用 AI Passport 模拟器测试（m01535）。
- 测试固件：firmware/recomp-bench；把 recomp/hot_func.c、delay_func.c 编进 ESP32-C3 固件；State 约 8 KiB，必须静态分配，放 app_main 栈上会踩爆 main task 默认栈。
- 构建：ESP-IDF 5.5.3，idf.py -B build -DIDF_TARGET=esp32c3 build；修复 CONFIG_ESP_TASK_WDT_EN=n 下 esp_task_wdt_deinit 未定义。
- 合并：bootloader 0x0、partition-table 0x8000、app 0x10000；recomp-bench-full.bin 273,712 B（0x42D30）。
- 模拟器：AI Passport wasm（esp32c3，160 MHz）；旧 UART0-only 镜像连续 3 次结果一致。
- 结果：HOT_PROFILE0 PASS、HOT_PROFILE1 PASS、DELAY PASS；HOT_BENCH 130 cycles/call、1.91 cycles/guest、83.7 M guest instr/s（旧镜像；最终双控制台镜像为 83.4 M，见下节）；DELAY_BENCH 46,212 cycles/call、5.77 cycles/guest、27.7 M guest instr/s；SIM_RESULT PASS failures=0。
- 判断：两个块都进入 23.4–26.9 M 目标区间；delay 只高一点，5.77 cycles/guest 主要是 C 代码膨胀（约 45 条 host 指令/8 条 guest）。这不是整机实时证明。
- 复现：powershell.exe -NoProfile -ExecutionPolicy Bypass -File firmware/recomp-bench/build.ps1 -Sim；日志 firmware/recomp-bench/sim_run.log。
- 边界：未接 internal ROM/SPI flash/LCD/IRQ；真机块级复核见下节。
- 下一步：真机复核（已完成，见下节）；扩 0x230321 调用者；接内存映射。

## 2026-11-20 — COM4 真机复核（块级 benchmark PASS）

- 用户指定本项目真机端口为 **COM4**（COM6 是 Cardputer ADV）；不再使用旧的 COM5。
- 本轮按用户指令清空重刷，没有保留整片备份；刷前 erase_flash，刷后 verify_flash 回读通过。
- 控制台：UART0 primary + C3 原生 USB-Serial/JTAG secondary，两个口都能出日志；这样 wasm 模拟器（UART0）和 COM4（USB）共用同一镜像。
- 最终镜像：`firmware/recomp-bench/recomp-bench-full.bin`，273,712 B（0x42D30），SHA256 `F04BDB757E44A85E94C6971B600C7B94709DBCC4B0F93FB9B97C968FE79BD12D`；app 208,176 B（0x32D30），SHA256 `B7983294E10D9304B37EB6C74783D4EBAEFE7FEAA9E06B1088F2583E36A964BF`。
- 设备：ESP32-C3 QFN32 rev v1.1，8 MB XMC flash，MAC `4c:11:ae:2f:3f:d4`；esptool 4.12.0，460800。
- 真机日志：`firmware/recomp-bench/device_run.log`；`HOT_PROFILE0 PASS`、`HOT_PROFILE1 PASS`、`DELAY PASS`、`SIM_RESULT PASS failures=0`。
- 真机数字（160 MHz）：
  - `HOT_BENCH n=200000 cycles=26970559 cycles_per_call=134 cycles_per_guest=1.98` → 80.68 M guest instr/s
  - `DELAY_BENCH n=2000 cycles=98745704 cycles_per_call=49372 cycles_per_guest=6.16` → 25.94 M guest instr/s
- 对照 48 MHz 原机目标 23.4–26.9 M：hot 约 3.0–3.4 倍，delay 约 96%–111%；两个块都在区间内或以上。仍是**块级**结果，不是整机实时。
- 同一镜像在 wasm 模拟器复跑仍 PASS：hot 130 cycles/call、1.91 cycles/guest、83.4 M；delay 46,212 cycles/call、5.77 cycles/guest、27.7 M；日志 `firmware/recomp-bench/sim_run.log`。
- 下一步：扩 `0x230321` 调用者（call/ret/push/pop）；接 internal ROM + SPI flash 4 MB 窗口；再做 0–17 s golden 段。整机实时仍未证明。

## 2026-11-20 07:39 — COM4 清空重刷复核

- 用户反馈屏幕黑，要求清空再刷一次。
- 执行：erase_flash（1.0 s）→ write_flash 0x0（273,712 B 压缩 108,921 B）→ verify_flash（Verifying 0x42d30，digest matched）。
- 设备仍为 ESP32-C3 QFN32 rv1.1 / 8 MB XMC / MAC `4c:11:ae:2f:3f:d4`。
- 串口 `firmware/recomp-bench/reflash_run.log`：HOT_PROFILE0/1 PASS、DELAY PASS、SIM_RESULT PASS failures=0；HOT 134 cycles/call 1.98 / 80.68 M，DELAY 49,372 cycles/call 6.16 / 25.94 M。
- 屏幕黑是预期：`recomp-bench` 只跑串口 benchmark，不初始化 LCD/背光；要出画面需接 ST7735 和背光控制。刷机本身已 verify 通过。

## 2026-11-20 — color-preview 可见画面验证固件（已上 COM4）

- 用户要求“做出游戏画面”，机器暂时不在；本轮交付 `firmware/color-preview`，目标是先把 ST7789P3、背光、SPI DMA、96×96→320×240 合成链路跑通，屏幕上看到原机 boot logo 和时钟/设置 UI。
- 这不是模拟器：不执行 `internal_rom_221017a.bin`，不读 `dmc_v1_base.bin`，没有 unSP CPU、定时器、中断、SPI flash、LCD 控制器、按键、RTC、存档、音频；ROM 不在固件里。
- 当前产物（2026-11-20 08:19 构建，命令实测）：
  - `build/color-preview.bin` 651,680 B，SHA256 `87EFB2D9E5C59FBCA7F36ABA00D3DEA519B260AFCEFCB65D8625ABEB480E46E8`
  - `color-preview-full.bin` 717,216 B，SHA256 `CBCA3DF60D83AB08F27A152FCB06770CBBFBD811A39E4A63E37F0025549390CB`
  - 1 MB app 分区剩 38%。
- 帧包 `main/frames.bin`：433,520 B，150 帧，10 fps，15.0 s；源 `short.avi`（1275 帧/60 fps，每 6 帧取 1），RGB565 大端，zlib；SHA256 `4aa2ac9e0979a71a8f1a6b5dc045b0acb08b7b76aeb301598adf59e4d8fa6ed1`。帧包保持 raw 方向，旋转在固件运行时做。
- 屏幕布局：96×96 最近邻放大 2 倍到 192×192，居中于 320×240，左 64、上 24，四周黑边；每 16 行一笔 SPI DMA，首帧后开背光 80%，150 帧后从第 20 帧循环。
- 方向：默认 180°（`s_rot=2`）。上电先显示第 60 帧 + 顶部红条/底部绿条 2.5 s；任意键循环 0/90/180/270，每次按键重显 1.4 s。默认 180° 的依据是原始帧里 `SET` 的 T 横条在下方、以及同系列 Color 手册（Godzilla/Pendulum）的“时间在上、SET 在下”布局；没有取到 Digital Monster Color 实机照片，所以保留按键循环。
- 关键源码：`main/display.c`（ST7789P3/SPI/LEDC/回调）、`main/preview_main.c`（DCPV 校验、tinfl 解帧、放大、播放）、`build.ps1`（idf.py + merge_bin）；入口文档 `firmware/color-preview/README.md`。
- 构建坑：本机没有 `pwsh`，且默认执行策略拦截脚本；必须用 `powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1`。
- 验证状态：已接 COM4、已整片备份、已烧写并 verify；15 秒串口无 panic，`PREVIEW_ROT 180`、`PREVIEW_READY`、`frame 0/150` 到 `frame 120/150`。屏幕红条/文字方向尚未肉眼确认，没有照片/录屏。
- 下一步：按任意键把测试帧调到正立 → 确认方向和画面 → 再接 unSP 核心、ROM 分区、`NO ROM` 兜底。

## 2026-11-20 12:2x — COM4 首刷与栈溢出修复

- 设备：ESP32-C3 QFN32 v1.1，8 MB XMC，USB-Serial/JTAG，MAC `4c:11:ae:2f:3f:d4`；端口 **COM4**。
- 整片备份：`docs/backup/digimon-color-passport-COM4-8MB-20261120-1218.bin`，8,388,608 B，SHA256 `C74EA4FF72A82F54326892D71F7A4081C55958A0D8B029190660742004CA48F9`；`verify_flash 0x0` digest matched。
- 首刷 `CBCA3D...` 镜像后启动：`PREVIEW_ROT 180` 后立即 `Guru Meditation Error ... Stack protection fault`，`MEPC 0x40038aaa`、`RA 0x42008d54` 对应 `main/preview_main.c:68`（`tinfl_decompress_mem_to_mem` 返回处）。
- 根因：`tinfl` 的内存解压状态约 11 KB，`preview` 任务只给了 8 KB 栈；不是帧包损坏。
- 修复：`xTaskCreate(preview_task, "preview", 32768, ...)`；重新构建后 app 651,680 B / SHA256 `87EFB2D9E5C59FBCA7F36ABA00D3DEA519B260AFCEFCB65D8625ABEB480E46E8`，full 717,216 B / SHA256 `CBCA3DF60D83AB08F27A152FCB06770CBBFBD811A39E4A63E37F0025549390CB`。
- 复刷：`write_flash 0x0` + `verify_flash 0x0` 均通过；15 秒日志 `firmware/color-preview/device_run_15s.log` 3,654 B，SHA256 `A7C01DA1D06E0FE146EEF266F89FC1A9FCC62E174196804F89D45BE20A419187`，无 panic，帧号到 120。
- 仍待肉眼确认：红条是否在物理上方、`SET` 是否正立、颜色是否正确；未拍照。
## 2026-11-20 13:1x — 静态验收帧 + ADC 按键（COM4 已刷）

- 用户指令：按计划 C 执行，COM4 一直连着，可自行观察和测试；按键实现可参照 `C:\Users\DuWeisi\digimon-passport`。
- 代码改动（`firmware/color-preview/main/preview_main.c`）：
  - 去掉“第 60 帧 + 红/绿条”的旧校准；改为合成静态验收图：黑底、四角红/绿/蓝/白、白色向上箭头；红条固定在输出坐标顶部、绿条固定在底部。默认 `s_rot=2`（180°），所以 180° 下箭头向下、上左白/上右蓝/下左绿/下右红。
  - 启动先显示静态图 15 s，再进入 10 fps 参考帧回放；按键边沿旋转 90°，重显静态图 5 s，然后从当前帧继续。静态期间 20 ms 轮询。
  - 按键从“GPIO0 数字电平”改为 ADC 电阻梯：ADC1_CH0、12 dB、4 次平均、curve fitting 校准；按下阈值 <1900 mV，释放 >2100 mV。初始化失败回退 GPIO 上拉数字读取。阈值来自旧项目 BSP：key0 0–150 mV、key1 150–447 mV、key2 447–1900 mV、释放 >1900 mV。
  - 新增 `button boot mv=... down=...` 日志；本次真机空载实测 **2944 mV / down=0**，说明 ADC 校准和“释放”判定生效。
- 构建（命令实测）：
  - `powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1`
  - 历史构建（已被 13:2x 的 666,000 B 取代）：`build/color-preview.bin` **665,184 B**，SHA256 `9D38DBDD1F18FB551CC28C54A1D8AB32B8C4E2857498A906E210FD131EC306CF`
  - 历史构建（已被 13:2x 的 731,536 B 取代）：`color-preview-full.bin` **730,720 B**，SHA256 `79E749C881B27AE835FFDB71C09B06098A8472A0EE8F479B83F20F3F15FA83F8`
  - 1 MB app 分区剩余 37%。
- 真机：COM4 已 `write_flash 0x0`，esptool 输出 `Hash of data verified`；设备 ESP32-C3 QFN32 rv1.1、8 MB XMC、MAC `4c:11:ae:2f:3f:d4`。
- 启动日志（`firmware/color-preview/device_watch.log`，180 s 抓取已完成，无按键边沿）：
  - `button ADC ready: press < 1900 mV, release > 2100 mV`
  - `PREVIEW_STATIC rot=180 (red top, green bottom; arrow up at rot=0)`
  - `button boot mv=2944 down=0`
  - 15 s 后 `static done; video from frame 0`、`PREVIEW_READY first_frame=0 rot=180`，无 panic。
- PC 侧期望图：`docs/static_expected_180.png`（320×240，当前默认 180°）、`docs/static_expected_0.png`、`docs/static_expected_rotations.png`（0/90/180/270 四宫格）；工具 `tools/render_static.py`。
- 新增串口复位/抓取工具 `tools/reset_capture.py`：用 DTR=IO0 高、RTS=EN 低脉冲做正常启动（不是下载模式），避免 USB-Serial/JTAG 打开串口后错过 boot 日志。
- `firmware/color-preview/README.md` 13:1x 时为 **6,808 B / 153 行**；13:2x 加入屏幕 dump 后为 **8,401 B / 187 行**，含静态图、ADC 阈值、dump 协议、构建/烧写命令、预期日志与已知限制。
- 仍未完成/未证实：
  - **没有用户按键的实测记录**。空载 ADC 已读到 2944 mV；需要用户按任意键，日志应出现 `button edge mv=... -> rot=...`。181 s 后台抓取已完成，日志到 `177543`，仍无 `button edge`；说明无人按键，不是代码报错。
  - **没有照片/录屏**，红条是否真的在物理顶部、四角颜色/箭头方向是否与期望图一致，仍需肉眼或拍照确认。
  - 参考帧视频本身仍有 MAME 的上下错位/滚动/镜像；静态图只用于把“屏/方向”与“源视频问题”分开，不修复源视频。
  - 现有整片备份仍是 `docs/backup/digimon-color-passport-COM4-8MB-20261120-1218.bin`（8,388,608 B，SHA256 `C74EA4FF72A82F54326892D71F7A4081C55958A0D8B029190660742004CA48F9`），未覆盖；需要回滚时用它。

## 2026-11-20 13:2x — 屏幕 dump（COM4 已刷）

- 用户指令：红条一直在上方；给测试固件加屏幕 dump，由助手侧观察。
- 实现（`firmware/color-preview/main/preview_main.c`）：
  - `dump_screen(why)` 打印 `SHOT_BEGIN 320 240 RGB565 rot=... why=...`，每 16 行调用 `render_chunk`，覆盖顶部红条 0xF800 / 底部绿条 0x07E0，逐行 RLE 打印 `SHOT_ROW <y> <x> <count> <hex>`，最后 `SHOT_END`；每块 `fflush` + `vTaskDelay(1)`。
  - dump 的是送给面板的 framebuffer，不读 ST7789 GRAM；上电静态图后自动 `why=boot-static`，按键旋转后自动 `why=key`。
  - 修正 `static_until` 声明顺序，boot/key 两条路径都可赋值。
- 构建：`powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1`
  - app `firmware/color-preview/build/color-preview.bin` **666,000 B**，SHA256 `E16E9552FB696881D1976B10EAEC0A2B2B415BC8BBC50FE8BE598590D75AD162`
  - full `firmware/color-preview/color-preview-full.bin` **731,536 B**，SHA256 `9ADDC29B5EC83931736EFD6E3D26AADF0B6F4862C27FFCCD65B97F14A9CCB3A2`
  - 1 MB app 分区剩 36%。
- 刷机：COM4 `write_flash 0x0`，esptool `Hash of data verified`；ESP32-C3 QFN32 rv1.1 / 8 MB XMC / MAC `4c:11:ae:2f:3f:d4`。
- 抓取：`tools/reset_capture.py COM4 firmware/color-preview/boot_dump.log 45`。日志 23,415 B；`SHOT_BEGIN ... why=boot-static` 在第 133 行，`SHOT_END` 在第 1591 行，`SHOT_ROW` 共 728 行；无 panic。
- 解码：新增 `tools/shot_decode.py`，输出 `docs/device_shot_boot.png`（320×240，755 B）。与 `docs/static_expected_180.png` 逐像素比较：`bbox=None`、`same_pixels=True`。
- 采样颜色：上条 (255,0,0)、下条 (0,255,0)、上左 (255,255,255)、上右 (0,0,255)、下左 (0,255,0)、下右 (255,0,0)、箭头 (255,255,255)、背景 (0,0,0)。即 180° 下红上绿下、上左白上右蓝、下左绿下右红、箭头向下。
- 结论：固件实际送屏内容与期望图一致；用户看到“红条一直在上方”与设计一致，红/绿条固定，按键只旋转内容。**dump 不是摄像头**，玻璃物理显示仍需照片/人眼。
- 文档：`firmware/color-preview/README.md` 已加“屏幕 dump”章节并改新构建数字；本节写入 PROGRESS/HANDOFF。
- 2026-11-20 14:06 用户反馈四向旋转“好像没问题”（人工目视确认；未抓 key dump）。
- 未完成：key dump、屏幕照片；均不阻塞显示链路验收。

## 2026-11-20 15:0x — PC 侧 unSP 模拟器第一版可玩（emu/）

- 目标：先交「60 秒 trace 覆盖率报告」（已交 `docs/COVERAGE.md`），再自主推进到「第一版可以完整测试游戏的」；
  本轮完成后者，**没有改固件、没有烧机**（COM4 保持 color-preview，COM6 未动）。
- 新增目录 `emu/`：GPCE4 SoC + ST7735 + MAME unSP 核心（0.289 移植）+ 两个前端。细节见 `docs/EMU.md`。
- 验证数字（全部本轮实测）：
  - 指令级：0–20 ms 窗口 `matched 682006 / 682006 (100.000%)`（对 `dbgtest/cov/A/000_t00000.tr`）。
  - 像素级：0.5 s 的 ST7735 可见区 vs MAME `dbgtest/mame12.avi` 第 30 帧 = **9,216 / 9,216 像素相同**
    （AVI 是 24-bit BI_RGB，必须按 BGR 解；先前「R/B 互换」的结论是我自己解码错了）。
  - 数据级：t=200 ms 的 4 KiB RAM 与 MAME 快照只差 4 个 word（`0850` 计数器差 1、`0FE7/0FE9/0FF1` 栈槽）。
  - 行为级：`--autoplay` 跑 60 s（3600 帧 / 5,760,000,000 cycles）1.3x 实时、exit 0、无 panic。
  - 交互级：窗口里按 J 从 boot logo 进时钟界面；J/K/L 序列依次得到时钟 0:00 → 1:00 → 1:01 → 1:02 → 数码蛋育成画面
    （`dbgtest/tour_sheet.png`、`docs/emu_tour_sheet.png`）。
  - 存档级：退出时写 `saves/flash.bin`（8,388,608 B），与 `roms/dmc_v1_base.bin` 只差 8 字节
    （0x7FD002/0x7FD00A/0x7FD05A/0x7FD05B、0x7FE002/0x7FE00A/0x7FE05A/0x7FE05B）；重启后按 J 显示
    「ロード しゅうりょう（读取完成）」，说明游戏真的把存档读回去了。
- 关键修复（按发现顺序）：
  1. SPI flash 同步化：MAME 的 48 MHz TX 定时器在按片执行的 CPU 下无法交织，突发写溢出 FIFO，
     阈值表（flash 0x223000 → RAM 0x0C9F..0x0CA4）读成 0，软浮点比较 0x5965 结论反了。改成就地 write/read。
  2. 切片细化：1 ms 切片把同毫秒内两次 2 kHz 事件合并，IRQ 速率掉到 1 kHz；改 `SLICE_CYCLES=2000`。
  3. flash 可写：page program（NOR AND 语义）+ 4 KiB 扇区擦除 + 整片持久化。
  4. 交互前端：Win32/GDI 4 倍放大 96×96 可见区、60 fps、键位轮询（避免失焦卡键）、F1 截图、F2 快进。
- 新增工具：`tools/emu_validate.py`（PC 流对拍）、`tools/capture_window.ps1`、`tools/press_key.ps1`
  （keybd_event 长按）、`tools/player_tour.ps1`（按键序列 + 截图）、`tools/patch_flash_writable.py`。
- 已知差异（不阻塞玩）：0.5 s 后 IRQ6 相位差约 85 条指令（同速率不同相位，900 ms 窗口 12.4% 对齐）；
  RAM `0x0600-0x0607` 八个字与 MAME 不同（像随机数/计数器）；无 RTC、无音频。
  **注：这三条后来全部被推翻或重分类，见下一节。**

## 2026-11-20 16:xx — 四项欠账收口 + 原版游戏在 AI Passport 上跑通（COM4 已刷）

用户指令（16:56）：把上一轮的四个欠账一次做完，并尽量产出能在 AI Passport 上跑起来的成果。

### 1. IRQ6 相位差——不存在，是量具坏了
- 主循环改成**事件驱动切片**：`cycles_to_next_event()` 取六个定时器源里最早的 `next - m_now`
  （跳过 300 kHz 那条只影响 IRQ3 的驱动源），每片只跑到下一个事件点。帧缓冲回归与旧 golden 逐字节一致。
- 逐条周期对拍：MAME `tracelog "%06X %d\n",pc,totalcycles` vs 本模拟器 `--trace-cycles`，
  **682,005 条指令 0 处周期差异**（`dbgtest/mame_cyc.tr`，末行 `005F36 1920001`）。
- `tools/emu_validate.py` 的 `find_alignment()` 原来只搜 skip 0..63，而调试器 `gtime` 的量化误差是
  72–106 条指令，于是把**完全一致**的流报成 12.4%。`max_skip` 放到 16384 后：
  **900 ms 95,260/95,260、5 s 95,251/95,251、20 s 93,343/93,343，全部 100.000%**。

### 2. RTC——真机也没有 RTC 芯片（结论：不需要建模）
- `docs/RE_RTC.md`（33,514 B）：时间变量 `0x0CAF/0x0CAE/0x0CAD`=时分秒、`0x0CB0`=使能、`0x0CB1`=半秒、
  `0x0CB2`=秒进位；唯一时间源是 IRQ7（0x486B）里 `[0x3054]&1 → [0x0CB1]++`，2 Hz 两次进一秒（0x5BE2）。
- 启动路径 `0x2041C5` 把六个字清零、`0x0CB0=0`，界面就停在 `0:00:00` —— **真机没设时间时同样如此**。
- 补丁实验：把 flash 副本 `0x2041D7` 从 `0x9640` 改成 `0x9641`（清时间函数里 `0x0CB0: 0→1`），
  6 秒后 `0x0CAD=5`，时钟自己走。
- 顺手纠错：上一轮截图里 `0:00 → 1:00 → 1:01 → 1:02` 是**设置时钟界面**（读暂存 `0x085C/0x085D`，
  秒位硬编码 "00"），不是真时间在跑。

### 3. 0x0600..0x0607——128-bit PRNG 的忙等产物，不是 bug
- 种子 `0x2002CC`、步进 `0x2002F4`；主循环 `0x230321: call 2002f4; r4=[0x0850]; je 0x230321` 忙等里
  20 ms 搅 6,412 次（每 64 Hz 周期约 5,000 次），值取决于「这一拍挤进多少迭代」，是混沌量。
- 逐位实证：断点拿到的 `45EA DCA3 …` 与公式算的 `W0'=0xDCA345EA` 完全一致。
- `0x0C86/0x0C9A/0x0CB1` 的 ±1 差值是快照落在 tick 边界两侧，不是功能差异。**对拍判据里应排除这 8 个字。**

### 4. 音频
- 见 `docs/AUDIO.md`（PPAM/DAC 0x3040..0x3043 的寄存器语义与宿主侧发声方案）。

### 5. AI Passport（ESP32-C3）上机跑通
- `firmware/digicore-c3/` 构建通过，整片镜像刷 COM4（`Hash of data verified`）。
- 串口实测：`BOOT_OK first non-blank guest frame at 20 frames, pc=007ca6, display_on=1 madctl=00a8`；
  稳态 `guest_fps=1.22 mi_s=0.73 host_fps=1.22`。
- **逐像素验证**：固件在第 60 帧把 `fb[18..113][0..95]` 以 RGB565 hex 打到串口
  （`dump_frame_hex()`，`tools/c3_shot.py` 还原成 `docs/c3_device_frame60.png`），与 PC 版
  `--frames 60 --shot-raw` 同窗口比较 **0 / 9216 像素不同**。
- 速度结论：每 guest 指令约 219 个主机周期，瓶颈是解释器本身（换 `esp_partition_mmap` 零拷贝后
  fps 一点没变）。要实时必须上静态重编译（recomp 块级 80.68 M instr/s ≈ 10 倍余量）。

## 2026-11-20 17:1x — 宿主时钟接入 + 育成真跑起来 + 对拍文件过期教训

### 1. 时钟接进游戏（新增开关，PC 与窗口版都有）
- `--clock-sync`：第 120 帧把本机时间写进 `0x0CAF/0x0CAE/0x0CAD`，置 `0x0CB0=1`、`0x0CB2=1`。
- `--clock-fast`：再置 `0x0D03=1`，游戏自己的 2 Hz 处理每拍加 `0x3C` 秒 → **1 guest 秒 = 1 游戏分钟**。
- 窗口版新增 **F3** 运行时切换上面这个快进标志；启动第 120 帧自动灌本机时间。
- 时钟界面确认是 **12 小时制**：种子 `10:45`→屏上 `10:45`，`19:45`→`7:45`，`09:45`→`9:45`，
  `20:45`→`8:45`，`16:44`→`4:44`（用 `TZ` 偏移逐个验过）。

### 2. 育成流程真的跑通了
- 按键路径：B1（0x0008）进设置 → B2（0x0010）调分 → B3（0x0020）确认进育成画面。
- `--frames 3600 --clock-fast --press "8:150:170,20:300:320"` 跑 60 guest 秒（=1 游戏小时）：
  蛋 → **孵化成数码兽**，见 `docs/emu_play_sequence.png`（设时钟 / 蛋 / 孵化两姿态）。
- 长跑：`--frames 36000`（600 guest 秒 = 10 游戏小时）用时 378 s、1.6x 实时、退出码 0；
  终帧背景变成黄昏、场上多出三只数码兽（`docs/emu_ten_minutes.png`）→ 昼夜循环与野外个体
  都随游戏时钟推进，说明游戏主体逻辑（不只 UI）在模拟器里正常运转。

### 3. 教训：对拍必须用当次二进制现生成的 trace
- `dbgtest/emu_t900.tr` 等旧文件是修复前的产物，用它比会把已修好的流报成 12.4%。
  现生成三条：900 ms skip 72、5 s skip 84、20 s skip 106，全 100.000%。
- `tools/emu_validate.py` 的 `find_alignment()` 改成 **8-PC 前缀哈希 + 全长度打分**：
  原来每个 skip 都 `bytes.find` 一遍，682k 指令的流要跑好几分钟；哈希版秒级完成，
  全长度打分也顺手修掉了「多个候选打满窗口上限就分不出高下」的 tie-break 问题。
- 阈值放宽到 `same >= 0.999 * n`：trace 在窗口尾部被截断属于正常现象，不该报 FAIL。

### 4. 音频收口
- `docs/AUDIO.md`（14,255 B）：游戏在 boot/时钟/菜单/20 s/60 s/30 s 密集按键六个场景里
  **0 次写 `0x3040..0x3043`**，`INT_Ctrl=0000`（20 kHz timer_a 全程屏蔽）→ 这些场景本来就无声。
  DAC 模型（零阶保持 + 2000-cycle 时间加权重采样，48 kHz 整数网格）与 WAV 导出已就绪，
  `digicore.exe --wav out.wav` 即可录；模型净开销 ≤0.08% 墙钟。




## 2026-11-20 19:4x — 设备侧 1.09 fps -> 7.82 fps（重编译热块）

目标：解决运行速度。做法是「先量分布，再动手」。

1. 量分布：新增 `--pc-hist FILE`（insn hook 里按 PC 累加），10 秒 242,116,283 条指令、5,310 个 PC。
   真凶是 `0x2002F4..0x200343` 的 68 条指令 PRNG 块：每个 PC 各执行 3,097,129 次，占动态指令 **87%**；
   原先怀疑的 `0x5F35` 三行循环只占 4.8%。
2. 复用已验证重编译块：`tools/gen_hotblock.py` 把 `recomp/hot_func.c`（真机 golden 逐字比对通过的那份）
   机械转换成 `emu/hotblock.h`，RAM 改为指向 SoC 实时数组；只覆盖 `0x2002F4..0x200342`，
   尾部 `pop bp, pc` 仍交给解释器，返回路径语义不变；每调用固定计费 263 周期
   （各指令众数周期之和；平均 45.5 是被中断归因污染的假值）。
3. 其他：RAM 平坦窗口（绕过 UnspBus 虚调用）、`spin_collapse()` 折叠 `0x5F35`（8 周期/圈）
   与 `0x5251`/`0x5376`（33 周期/圈）、组件级 `-O3`。
4. `tools/sync_c3_emu.py` 把 `emu/` 的共享源同步进 `main/emu/`，并重新插入 C3 专有的指令计数器
   （三个 `execute_one` 调用点），避免两边手工漂移。

实测：
- 设备 `guest_fps=7.82`、`mi_s=3.15`（原 1.09 / 0.73），**7.2 倍**；PC 侧同改动 1.7x -> 13.8x。
- 正确性：PC 侧 20 秒 `--save-flash` 存档在「解释器」与「热块」两条路径下 **0 字节不同**；
  设备第 60 帧与 PC 参考帧 **0/9216 像素不同**；周期计数差 0.007%（中断只能在块边界投递）。
- 按键：guest 需要连续 4 帧按下才动作（1/2/3 帧实测无效），固件把一次轻点拉伸成 5 帧；
  7.8 fps 下等待从 5 秒降到 0.7 秒。

欠账：要摸到 60 fps，还得把 `0x230000` 主循环与 `0x5xxx` UI 的热块也静态重编译（约 20-30 个块，
需要一个可复现的块级生成器，不能手工搬）。

## 2026-11-20 21:4x - 设备侧 7.69 -> 14.15 fps（等待循环整段折叠）

- 在设备上量了三个不含探针的微基准：hot_block_prng 81 周期/次（68 条指令 = 1.2 周期/条）、
  一次 etch16 49 周期、一条 execute_one 163 周期 → 一条解释指令 227 周期。
- 结论不是「解释器结构可以优化」，而是「别让它执行」：主循环 0x230321 是等 64 Hz 节拍的忙等，
  每圈 74 条指令 298 周期，其中 6 条外壳指令走解释器要 1,362 周期（占一圈的 82%）。
- 新增 unsp_device::wait_loop_collapse()：整圈原生化（跑 PRNG 块 + 手动 pop bp + 尾部落 PC），
  按 298 周期/圈计费，m_sirq 一到就交还解释器。第一次实现把 call 当成压 PC+SR，
  多压一个字导致 SP 每圈漂一格、四万圈后栈被冲穿（PC=004020）；改成 call/ret 相互抵消后正确。
- 实测：PC 侧 12.5x -> 30.3x；设备 7.69 -> **14.15 fps**。存档逐字节相同、帧缓冲一致、
  周期计数完全相同、设备第 60 帧 sha256 与此前参考帧一致。
- 工具：	ools/add_wait_loop.py（PC 侧安装）、	ools/port_wait_loop.py（同步到 C3 树）、
  pp_main.cpp 里的 ench_exec() 微基准（boot 时打一行）。


## 2026-11-20 22:5x — 设备侧 24.5 → 60.00 fps（锁定 60 帧）

- 先量：`guest_pct=58 / blit_pct=40`（14.15 fps 时）。上一轮「skip blit 无变化」的结论只在 7.6 fps
  配置下成立，作废。
- blit 重写（`tools/patch_blit.py`）：只发 192 宽窗口（省掉 40% 永不变化的黑边）、脏行跳过
  （`s_prev[]` 逐行比较）、32 位双像素写入（`pair = sw | sw<<16`）→ **24.5 fps**、blit 降到 3%。
- 削 guest 侧固定开销四步：collapse 每圈只读一次总线（1,260 → 245 主机周期/圈）、
  `kSliceCycles` 2,000 → 48,000、300 kHz SPI2 驱动源默认不排事件、被屏蔽的 `timer_a`
  不再排事件（`source_enabled()`）→ **79.7 fps**（帧 12.5 ms）。`tick()` 从 42 M 周期/秒降到 3.0 M。
- 限速：79.7 fps 会让 guest 以 1.33 倍速变老。第一版把微秒当毫秒传给 `pdMS_TO_TICKS()`
  → 每帧睡 3.1 秒、设备 0.16 fps；修单位后 57.3 fps（tick 向上取整吃掉 0.7 ms/帧）。
  最终用绝对截止时间（`tools/patch_pacer_deadline.py`：睡 `wait-2 ms`、最后 2 ms 忙等、
  落后整帧才重同步）→ **60.00 fps 稳定**。
- 验证：连续 60 s soak（`dbgtest/final60.log`）3,116 帧全部 60.00、无 panic/STUCK；
  第 60 帧 dump 与 PC 参考帧 **0/9216 像素不同**；app 382,992 B（sha256 1BC10952…）。
- 新增工具：`tools/patch_blit.py`、`patch_wait_collapse.py`、`patch_collapse_hoist.py`、
  `patch_masked_timer.py`、`patch_pacer_deadline.py`、`patch_stuck_watchdog.py`、
  `patch_slice_knob.py`、`port_wait_loop.py`、`add_collapse_stats.py`。

## 2026-11-20 23:3x — 交接：AOT 施工图落地

- 这一轮补齐的画面工作：`blit_frame()` 只发 192×192 窗口导致面板其余区域一直是 ST7789 上电随机 GRAM
  （用户看到的「一大片花屏中间一小块画面」）→ `display_init()` 后先分 10 条清黑；窗口从「水平 2 倍、
  垂直不缩放」改成 `kScale` 两轴统一，最终定为 **1:1（96×96 居中）**；左上角加三位实测帧率浮层。
- 诊断：静止每帧只解释 3,700 条指令（60 fps 稳），换屏那一秒变成 162,000–270,000 条/帧（fps 3.14）。
  `--block-hist` 显示 `0x207CA3..0x207CC4`（带透明色键 `0x07E0` 的位图拷贝）占换屏指令 23%，
  `0x230321` 等待循环 8.7%，PRNG 块 30%，其余散在 6,043 个 PC 上。
- 结论：解释器约 200 主机周期/条，已无微观优化空间（六种手段实测 0 收益），唯一出路是整程序 AOT
  （覆盖率报告证明 762 个块静态可定、无间接跳转、无自修改代码）。
- 交接：`docs/PLAN_AOT.md` = 目标与验收、架构决策（unSP→C、一块一函数、块边界投递中断、回退保底）、
  指令发射器清单（按动态频次前 30 种覆盖 99%）、T1–T6 分步任务、命令级验证手法、雷区表、开场动作。
