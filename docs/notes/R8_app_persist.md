# R8 应用层状态机与持久化审计

审计对象（只读，未改任何源码，未跑 idf.py，未刷机，未 git）：
`app_main.cpp`（3992 行）、`saveimg.cpp/.h`、`gueststate.cpp/.h`、`wallclock.cpp/.h`、
`dmc_link.cpp/.h`、`rom_esp.cpp/.h`、`menu.cpp/.h`、`fbsave.cpp/.h`。

取证基线：`build/digicore-c3.elf` / `.bin` 生成于 2026-11-28 16:16（v1.18.0，HEAD 741a49e），
与当前源码同版。符号尺寸用 `riscv32-esp-elf-nm -S --radix=d` 实测，非估算。
`nm` 路径：`C:\Users\DuWeisi\.espressif\tools\riscv32-esp-elf\esp-14.2.0_20251107\riscv32-esp-elf\bin\riscv32-esp-elf-nm.exe`。

## 0. 结论摘要（按优先级）

| ID | 结论 | 优先级 |
|---|---|---|
| **AP-01** | `kFont5x7` 两份 295 B 副本（menu.cpp:36 + bezel.cpp:30 各 include 一次），共 590 B 真占镜像，R7 P-05/C1 未落地。零风险可删。 | P0 |
| **AP-02** | 60 s 自动存档的触发链被 R8_BRIEF 前提写错了：**saveimg 16 KiB 根本不在 60 s 链上**（它由 `persist_save_image` 独立触发，零售 ROM 从不写 overlay）。真实链 = gueststate 12 KiB 擦 3 扇区 + fbsave 32 KiB 擦 8 扇区（仅 resumable 时）。fbsave 是最大磨损项，最坏 ~69 天/扇区。 | P1 |
| **AP-03** | lapse toast（"LAPSE 60X C=STOP"）是静态卡片，却**每帧**重推 240×22=10,560 B ≈ 2.1 ms SPI（app_main.cpp:3578 + menu.cpp:596），且它所在 y 216..238 在 blit 窗口（24..216）之外，唯一会盖它的是 30 s 一次的电池角。应事件化。 | P1 |
| **AP-04** | `crcwatch` 诊断每 ~5 s 无条件打一条 ESP_LOGI（app_main.cpp:3622-3626），每 ~0.5 s 做一次 32 KiB fb CRC（3611-3620），都不随 STAT 关闭。release 该关。 | P1 |
| **AP-05** | menu 确认卡在 menu.cpp 侧已编译掉（R7 #6），但 app_main 侧留下 5 个恒假调用点 + 2355-2384 死分支 + `s_confirm_fresh`(1 B)/`s_confirm_pad_prev`(4 B) 死状态，每帧白付 `menu_confirm_is_open()` 外部调用。 | P1 |
| **AP-06** | 5 个死导出 + 1 个死函数全为 0 B（已被 gc-sections/inline 丢弃），纯源码/头文件噪声：`dmclink_rom_ok`、`dmclink_waiting_for_rom`、`wallclock_local_sec`、`fbsave_init`、`battery_mv`、`menu_banner_present`(+`s_wide`)。改 static/删声明。 | P2 |
| **AP-07** | `wallclock_persist` 每 60 s 随 snapshot 写一次 NVS epoch（app_main.cpp:2726 → wallclock.cpp:145-165），1440 次/天 NVS commit，是除 snapshot 外的第二磨损项。可降频。 | P2 |
| **AP-08** | R7 D1-D4 的每帧纯开销（12 全局快照、每 slice 2 次 esp_timer、blit 计时、直方图）仍在，全部只喂默认关闭的 STAT。应 `#if C3_STAT` 包起。 | P2 |

---

## 1. 范围与方法

读了上面 8 个文件全部源码；跑了以下只读命令：

```powershell
# 符号尺寸（死导出/重复/热函数）
& riscv32-esp-elf-nm -S --radix=d --size-sort build\digicore-c3.elf |
  Select-String 'console_exec|menu_banner_present|menu_confirm_|kFont5x7|kFont3x5|...'
# 引用普查（死导出）
Select-String -Path main\*.c,main\*.cpp,main\*.h -Pattern 'dmclink_rom_ok|menu_banner_present|...'
# STAT 格式串字节数
(Get-Content app_main.cpp)[3868..3880] | %{ [regex]::Matches($_,'"([^"]*)"') } ...
```

未读 `aot_blocks.c`/`*.bin`/`*.avi`。

---

## 2. 发现表

### 2.1 每帧固定开销表（app_main.cpp 主循环 3395-3988，按执行顺序）

| # | 位置 | 每帧无条件调用 | 估算周期/耗时 | 处置 |
|---|---|---|---|---|
| 1 | 3401 | `frame_time_note()`（32 桶直方图 + 4 比较） | ~10-20 cyc | 仅喂 STAT，默认关 → `#if C3_STAT`（R7 D4） |
| 2 | 3404 | `esp_timer_get_time()`（t_guest0） | ~40 cyc（独立函数调用） | 帧预算/guest_us，保留 |
| 3 | 3405 | `debug_poll()`（UART0+USB-JTAG 4 次 LL 读） | ~20 cyc | 唯一输入通道，保留 |
| 4 | 3421 | `selftest_step()`（`s_st_phase<0` 早退） | ~6 cyc | `C3_SELFTEST=0` 仍每帧付 → `#if` 包（R7 B7） |
| 5 | 3439 | `update_buttons()` | 多次 esp_timer + 状态机 | 功能必需 |
| 6 | 3447 | `s_soc.set_sleep_inhibit(kGuestResumePc)` | ~10 cyc | 功能必需 |
| 7 | 3454 | `menu_confirm_is_open()`（恒 false 外部调用） | ~10 cyc | 死调用，见 AP-05 |
| 8 | 3496 | `time_lapse_step()`（lapse off 早退） | ~15 cyc | 可事件化（AP 附录） |
| 9 | 3497 | `run_guest_frame()` | guest 内核（范围外） | 帧预算主体 |
| 10 | 3501 | `audio_set_buzzer(s_soc.buzzer_hz())` | ~250 cyc（`buzzer_hz()` 内含 `6000000/ticks` 软除 + timer 读） | 可事件化（R7 P-12） |
| 11 | 3505 | `sync_time_lapse()` | ~10 cyc | 可事件化 |
| 12 | 3506 | `clock_seed_step()` | ~10 cyc | 必需（直到 seeded） |
| 13 | 3507 | `clock_sync(false)`（wallclock_hms 2 次 64 位软除，250ms 死区限 ≤4/s） | ~200 cyc | 功能必需 |
| 14 | 3525 | `frame_budget_track()` | ~20 cyc | 自适应帧预算，保留 |
| 15 | 3567 | `blit_frame()` + 计时 | 视脏块 | 功能必需；计时项 D3 浪费 |
| 16 | 3578 | `menu_lapse_toast_present()`（lapse toast 时） | **~2.1 ms SPI/帧** | 静态卡每帧重画，见 AP-03 |
| 17 | 3611-3654 | crcwatch：每 30 帧 32 KiB CRC，每 300 帧 1 行 INFO | 摊销 ~0.05% + 日志 | 见 AP-04 |
| 18 | 3656 | `persist_save_image()` | ~10 cyc | 必需（overlay 脏检测） |
| 19 | 3657 | `maybe_snapshot_machine_state()` | ~10 cyc | 必需 |
| 20 | 3657 | `update_sleep_backlight()`（3× `menu_*()` 函数读 + 音量变更检测） | ~30 cyc | 可事件化（menu 变更推送） |
| 21 | 3663 | `audio_idle_poll()` | ~10 cyc | 必需 |
| 22 | 3672-3699 | 帧对齐（esp_timer ×N + vTaskDelay + ≤1.5 ms spin） | 1.5 ms/帧 | 设计选择，不动（R7 D6） |
| 23 | 3753 | `esp_timer_get_time()`（now） | ~40 cyc | 喂 STAT/fps，STAT 关时仍付 |

可合并/降频/事件化项：**#4、#7、#8、#10、#11、#16、#20**，以及 D1-D4（见 AP-08）。
其中 #10（buzzer）与 #16（lapse toast）是唯二带软除/SPI 的实质开销；其余是 ~10 cyc 级。

### 2.2 flash 写放大与寿命账（AP-02）

**触发链（实测代码，纠正 R8_BRIEF 前提）：**

- 60 s 自动存档：`maybe_snapshot_machine_state()`（app_main.cpp:2731）→ `snapshot_machine_state()`
  （2673）→ `gueststate_store()`（2709）+ `save_panel_frame()`→`fbsave_store()`（2708/2619）
  + `wallclock_persist()`（2726）。**不含 saveimg**。
- `saveimg` 由 `persist_save_image()`（2561）独立触发，条件是 overlay 写计数变化 + 1 s 静置 +
  有页可导（2571-2588）。零售 ROM 从不发 PP 写（app_main.cpp:1686-1688、2595-2599 注释），
  故设备上 saveimg 实际从不触发（仅 `W` 命令能构造）。

| 层 | 区域 | 每次擦除 | 每次写 | 触发 | 最坏擦除频率(60s) | 扇区寿命(100k) |
|---|---|---|---|---|---|---|
| gueststate 环 | 0x00000..0x0F000，5×12 KiB | 12 KiB = 3 扇区（gueststate.cpp:131） | 32 B 头 + ~340 B SnapBlob + 8 KiB RAM ≈ 8.5 KiB | 每 snapshot（RAM 变时） | 每扇区 288/天 | **~347 天** |
| fbsave | 0x0F000..0x17000，1×32 KiB | 32 KiB = **8 扇区**（fbsave.cpp:80） | 32 B 头 + 24,576 B（24.6 KiB） | 仅 resumable（PC∈0x200000..0x400000，app_main.cpp:2707） | 每扇区 1440/天 | **~69 天** |
| saveimg | 0x18000..0x20000，2×16 KiB | 16 KiB = 4 扇区（saveimg.cpp:185） | 24 B 头 + ≤2 页 8 KiB | 仅 overlay 脏（零售 ROM 不触发） | ≈0 | ≈∞ |

**关键点（均已静态核实，标注：静态推导）：**
1. fbsave 的擦除是 32 KiB = **8 个 4 KiB 扇区**，不是 app_main.cpp:2703 注释写的 "four sectors"
   （该注释过期，`kRegion=0x08000`，fbsave.h:27）。
2. fbsave 已有 "resumable-only" 门控：guest 停在 ROM park 环（时钟屏/待机）时 `fb_crc=0`，
   fbsave 根本不擦（app_main.cpp:2707-2708）。所以真实磨损远低于 69 天最坏值。
3. gueststate 已有 dirty-only：`s_snap_ram_crc` 对 8 KiB RAM 做 CRC，不变即跳过整次存储
   （app_main.cpp:2690-2693），跳过后下一档仍按完整 period 排期（2749），不会重试风暴。
   fbsave 也因此被同一门控保护（CRC 判定在 fbsave 之前）。
4. 每 60 s 若 RAM 真变了（活跃游玩）：gueststate 擦 12 KiB + fbsave 擦 32 KiB，合计 44 KiB
   擦除 + ~33 KiB 写入，**阻塞帧循环**。fbsave 一次 ≈150 ms（app_main.cpp:2703 注释），
   gueststate 擦 3 扇区 + 写 8.5 KiB 估 ~100-170 ms（按 4 KiB 扇擦 ~45 ms typ 推导，未上机）。
   合计每次存档 ~250-320 ms 阻塞，即丢 15-19 帧。
5. 能量账：NOR 擦/写额外 ~15-25 mA（datasheet 级，未实测）。每次存档 ~0.3 s × 20 mA ≈ 6 mA·s，
   摊到 60 s = **~0.1 mA 平均**，可忽略。真正成本是帧循环阻塞与 flash 寿命。

**能否 dirty-only/变更触发/合并写入：**
- dirty-only **已实现**（gueststate RAM CRC），fbsave 由它连带保护。
- 变更触发（挂钩 guest RAM 写置脏）能省掉每 60 s 一次 8 KiB CRC，但该 CRC 成本 ~8 KiB @160 MHz
  ≈ 25 µs/分钟，不划算，**不建议改**。
- 合并写入：gueststate 与 fbsave 分区偏移/擦除粒度不同，且 fbsave 是可选（resumable-only），
  无法合并。saveimg 已在独立链上，无需并入。

### 2.3 NVS 写入频率（AP-07）

`nvs_commit` 全工程 3 处：
- `menu.cpp:89` `persist_u8`——仅在 menu 内 B 键**改变**某个值时调用（lapse/auto/bright/vol/save，
  191-212、247）。**没有每按键都 commit 的路径**（menu_press_b 只在 menu 打开时、边沿触发）。
- `menu.cpp:126` `menu_load`——仅 fresh_build（volbuild 时间戳变化）时一次。
- `wallclock.cpp:164` `wallclock_persist`——被 3 处调用：`wallclock_set`（console `t`/`DMC1 HELLO`）、
  `wallclock_adopt_local`（玩家在游戏时钟编辑器提交、或退出 lapse）、以及 **每次成功 snapshot**
  （app_main.cpp:2726）。

结论：**无每按键 commit**。唯一的高频 NVS 写是 `wallclock_persist` 每 60 s 随 snapshot 写一次
epoch（epoch 每 60 s 推进，nvs_set_i64 每次都写新值 → NVS 页重写）。1440 次/天，一年 ~52.5 万次，
摊到 24 KiB（6 扇区）NVS 上是 ~8.7 万擦/扇区，**接近 100k 寿命**（仅当宠物常年活跃、snapshot 从不跳过时）。
可降频：时钟锚点只需"最近一次快照时刻"，与 60 s 粒度本就一致，改成每 5 min 或每 5 次 snapshot 写一次
（断电时钟最多再旧 4 min，wallclock.h:106-108 已声明"断电恢复即最后一次快照时刻"是尽力而为）。

### 2.4 time_lapse 每帧注入与 flash 写（范围第 4 问）

- `time_lapse_step()`（app_main.cpp:777-798）每帧跑：lapse off 时 3 次赋值早退；on 时 1 次
  `s_lapse_frames++` + 比较 + 每 1/4 帧写一次 `s_soc.ram[0x0CB1]=2`（15X）或每帧（60X/300X）。
  纯 RAM 写，无 flash。
- `sync_time_lapse()`（721-767）每帧跑，内部只在与 menu 值不同步或 force 时写 RAM 0x0D03，已变更门控。
- **快进不加速 flash 写频率**：snapshot 排期用 `esp_timer_get_time()`（真实墙钟）+ period（2740-2749），
  与 lapse 乘数无关，仍 60 真实秒一次。但 lapse 期间 guest RAM 变化更快，dirty-only CRC 更易命中，
  snapshot 更少跳过 → 有效存储率逼近 60s 最坏值。这是正确行为，不是 bug。
- lapse 期间 `clock_sync` 走 `wallclock_track_local`（3005-3012），**不写 NVS**；NVS 仍只在 snapshot 写。

### 2.5 STAT/日志（范围第 5 问）

- 默认 `s_stat_period_s = 0`（app_main.cpp:497），STAT 关闭。✓
- STAT 一行格式串实测 **828 B**（12 个字面量，app_main.cpp:3869-3881；比 R7 的 857 B 少了 3 个
  hot/slices/tick_pct 字段）。828 B @115200 ≈ 71.9 ms 阻塞，含 60 个参数求值约 74-78 ms/行。
- 日志上限 R7 已降到 INFO；但 release 仍常开的 INFO 有三类该降：
  1. **crcwatch**（app_main.cpp:3624）每 ~5 s 无条件 1 行 —— 该 `ESP_LOGD`（AP-04）。
  2. **LONGF**（3529-3548）仅 >60 ms 帧触发，属有用诊断，可留或 `#if C3_STAT`。
  3. STAT 巨型行本身虽默认关，但 828 B 格式串 + 取值代码仍占镜像（估 1.5-2.5 KB，含 828 B 字符串，
     代码部分无法从符号单独切出，标注为估）。
- release 建议关闭清单：crcwatch（LOG→DEBUG）、selftest、fps overlay（`#if C3_FPS_OVERLAY` 默认 0）。

### 2.6 未用函数/字段/分支/常量（R7 之后剩下的，范围第 6 问）

**A 类：真占镜像（nm 实测字节数）**

| 项 | 位置 | 证据 | 字节 |
|---|---|---|---|
| `kFont5x7` 重复两份 | menu.cpp:36、bezel.cpp:30 include font5x7.inc:3（`static const`） | nm：`_ZL8kFont5x7` ×2，各 0x127 | **590 B**（295×2） |
| fps overlay：`kFont3x5` + `box` | app_main.cpp:811-822、831 | nm：`_ZL8kFont3x5` 0x32=50 B；`box` 0x240=576 B bss | **626 B** |
| `console_exec`（含死命令 Y/M/O/A） | app_main.cpp:1250-1736 | nm：`console_exec.constprop.0` 0x13ca | **5066 B**（整体保留，见 §3） |
| STAT 格式串 | app_main.cpp:3869-3881 | 实测 828 B | **828 B**（+取值代码） |
| selftest 状态 `s_st_*` | app_main.cpp:1758-1763 | nm：6×4 B（`.data` 1 + `.bss` 5） | **24 B** |
| 确认卡死状态 `s_confirm_fresh`/`s_confirm_pad_prev` | app_main.cpp:253-254 | nm：1 B + 4 B bss | **5 B** |
| `s_image_size`（只写不读） | rom_esp.cpp:40、109 | nm：`s_image_size` 4 B bss | **4 B** |

**B 类：浪费阅读、0 字节占镜像（nm 均无符号 = 已被 gc-sections/inline 丢弃）**

| 项 | 位置 | 状态 |
|---|---|---|
| `dmclink_rom_ok()` / `dmclink_waiting_for_rom()` | dmc_link.cpp:386/391、dmc_link.h:43/52 | 死导出，0 调用 |
| `menu_banner_present()` + `s_wide[320]` | menu.cpp:556-590/324、menu.h:112 | 死函数+死 640 B bss，0 调用 |
| `wallclock_local_sec()` | wallclock.cpp:202、wallclock.h:65 | 死导出，仅被 wallclock_hms 内联 |
| `fbsave_init()` | fbsave.cpp:47、fbsave.h:40 | 死导出，仅 3 次内部自调（内联） |
| `battery_mv()` | battery.c:190、battery.h:24 | 死导出，仅 battery_init 日志内联 |
| `aot_dump_miss` 双重声明 | app_main.cpp:349-350 | 纯源码噪声（R7 A10 仍在） |
| `restore_machine_state` 双重声明 | app_main.cpp:1203、2441 | 同上 |
| `s_rom_ok = s_rom_ok;` 自赋值 | dmc_link.cpp:320 | 废句（R7 P-19 仍在） |
| `#include "saveimg.h"` | dmc_link.cpp:36 | 死 include（R7 P-20 仍在） |

### 2.7 菜单/待机/提示条状态机（范围第 7 问）

- **menu_present**（menu.cpp:718）：`if (!s_dirty) return` 正确门控，只在 open/press_a/press_b/
  set_time_lapse_idx 重画。**无重复绘制**。微浪费：app_main.cpp:3460-3466 每帧无条件构造
  `MenuBackdrop bg`（10 字段），即便 menu_present 立即返回（菜单稳态每帧 10 次 store）。
- **menu_confirm_present**（menu.cpp:874）：`C3_CONFIRM_CARD=0` 时是 2 B 空桩；但 app_main 仍每帧
  进入 `if (menu_confirm_is_open())` 分支（3454）并构造 bg + 调空桩（3462）。**永不触发**（AP-05）。
- **menu_standby_present**（menu.cpp:655）：仅 `standby_enter` 调一次（app_main.cpp:1999），
  `if (s_standby) return` 防重入。**无重复**。
- **menu_lapse_toast_present**（menu.cpp:596）：**每帧重画，无 dirty 门控**（AP-03）。
  y0=216..238 在 guest 窗口（24..216）之外，blit 不覆盖；唯一覆盖者是电池角（y222..235，
  R7 P-07），且电池角 30 s 才变一次。→ 每帧 2.1 ms SPI 是 95% 浪费。
- **永不触发的分支**：`rot_map` 的 `case 4/5`（app_main.cpp:911-917）——`switch(kRot & 3)` 配
  `case 4/5` 恒不可达（R7 A7）；`s_pattern==2/3` 的 panel_edge/align 每帧重画仅 console 激活时。

---

## 3. 需要主审裁决的冲突/不确定项

1. **AP-02 与 R8_BRIEF 前提冲突**：BRIEF §3 把 saveimg 16 KiB 与 gueststate/fbsave 并列成
   "60 s 自动存档"三件套。代码证明 saveimg 不在该链上。请主审确认口径，避免把 saveimg 的
   磨损算进 60 s 账（它会高估 4 扇区/次的擦除）。
2. **fbsave "four sectors" 注释 vs 实际 8 扇区**（app_main.cpp:2703）：要么改注释，要么澄清
   是否是按 8 KiB 扇区口径（ESP32-C3 是 4 KiB）。不影响代码，但影响寿命账。
3. **AP-03 lapse toast 每帧重画**：menu.h:114-119 注释自己承认"每帧重画，cheap"。
   5.3k px / 37k px ≈ 14%，约 2.1 ms/帧。是否接受？（建议事件化，但需确认电池角重叠是否真是
   每帧重画的唯一理由。）
4. **AP-07 NVS 降频**：wallclock epoch 每 60 s 写一次是否必要？降频会让断电恢复时钟更旧。
   这是产品取舍（断电时间本就不可知，见 wallclock.h:106-108）。
5. **`console_exec` 整体保留**（5066 B）：R7 B2 已定保留（`T/N/G/E/V/S/K/J/D/U/t` 是唯一验收通道）。
   本轮不推翻，只建议定点删 `Y`（空命令，`aot_dump_blocks()` 是空函数）、`M`（`aot_dump_miss()`
   桩）、`O` 的 3 个恒零字段、`A`（已坏的旧和弦路径），共 ~250 B。这些是 R7 B3/B4/B5/C4 未落地项。

---

## 4. 明确不动的清单（防误删）

| 对象 | 理由 |
|---|---|
| `save` 分区三段偏移（0x0/0xF000/0x18000）+ gueststate ring=5 + fbsave 1 块 + saveimg 2 槽 | 三模块互相引用的硬约定（gueststate.h:36-39、fbsave.h:18-22），动一个全崩 |
| gueststate 的 header-only 回读校验 / fbsave 两遍流式读 | 掉电撕裂检测的唯一手段，DRAM 无第二块缓冲（gueststate.cpp:147-154、fbsave.cpp:124-147） |
| `s_snap_ram_crc` dirty-only 与 fbsave resumable-only 门控 | 已是正确的磨损抑制，勿"优化"成变更触发（§2.2） |
| 每帧 `audio_set_buzzer(buzzer_hz())` | 快照恢复后唯一补发通道（app_main.cpp:1551 注释），删需在恢复路径补显式发布 |
| `console_exec` 整表 | 验收/取证唯一通道（R7 B2） |
| 帧末 ≤1.5 ms 忙等 | 帧对齐靠它（R7 D6） |
| `s_strip`（15,360 B）与 `s_frame_buf`（heap 73,728 B） | 共享/可选缓冲，已按 max 尺寸化（app_main.cpp:381-388） |

---

*报告完毕。本轮未修改任何源码，未运行 idf.py；唯一写入的文件是本报告。*
