# 第七轮 · 应用层审计：刷写版固件里的冗余 / 死代码

审计对象（**只读，未改任何源码，未跑 idf.py**）：

| 文件 | 行数 | 是否进固件 |
|---|---|---|
| `firmware/digicore-c3/main/app_main.cpp` | 3958 | 是，`CMakeLists.txt` SRCS 第 1 个 |
| `firmware/digicore-c3/main/menu.cpp` / `menu.h` | 897 / 142 | 是，SRCS 第 7 个 |
| `firmware/digicore-c3/main/buttons.c` | 224 | 是，SRCS 第 10 个 |

`emu/`（PC 侧）不参与刷写版，本报告任何结论都不以它为依据。

**取证基线**：`build/digicore-c3.map`、`build/digicore-c3.elf` 生成于 2026-11-27 19:16:19，
`app_main.cpp` 最后修改 19:13:29 —— 构建产物与本轮源码同一版本（`version.txt` = 1.16.2），
所以下表里的「符号字节数」是**当前源码的真实占用**，不是估算。

镜像规模（`objdump -h` / `Get-Item`）：app `.bin` 1,144,944 B；`.flash.text` 694,300 B；
`.flash.rodata` 323,488 B；`.dram0.bss` 112,232 B；`.dram0.data` 7,408 B。
**flash 宽松、DRAM 紧张**，所以 DRAM 的每 4 字节都比 flash 的每 100 字节值钱。

检索手段（本文所有「引用数」都出自这些命令，不是目测）：

```powershell
# 全工程符号检索（等价 rg -n；范围 firmware/digicore-c3 下全部 .c/.cpp/.h，排除 build/）
Get-ChildItem firmware\digicore-c3 -Recurse -Include *.c,*.cpp,*.h |
  Where-Object { $_.FullName -notmatch '\\build\\' } |
  Select-String -Pattern '\bs_b_hold_locked\b'
# 符号尺寸（只读反汇编，不重新链接）
riscv32-esp-elf-nm --print-size -S build/digicore-c3.elf
riscv32-esp-elf-objdump -d --start-address=0x4200d420 --stop-address=0x42010b5e build/digicore-c3.elf
# 判断「链接器是否已经把没人引用的段丢掉」：map 的 Discarded input sections（第 858-9123 行区间）
```

---

## 一、结论摘要（按可删收益排序）

| ID | 位置 | 类型 | 证据（摘要） | 影响 | 风险 | 建议 |
|---|---|---|---|---|---|---|
| **B1** | `menu.cpp:783-897`、`menu.h:133-142`、`app_main.cpp:254-255, 2371-2402, 3476-3485` | 被后续版本取代的旧路径 | `menu_confirm_set()` 全工程只有 `false` 调用点，无 `set(true)`；`s_confirm` 恒 false → 卡片永不出图；`nm`：`menu_confirm_present` 0x74c=**1868 B**、`set` 16 B、`is_open` 10 B | **≈1.9 KB flash** + 7 B DRAM + 每帧 3-4 次恒假判断 | 低（menu.h 自述保留理由） | 删 |
| **B2** | `app_main.cpp:1268-1754` `console_exec` | 仅调试可用 | `nm`：**0x13ca = 5066 B**，单个函数即 app 镜像的 0.44%；tools/ 实际发送过的字母只有 `V S K J t D U` | 5.1 KB flash（含字符串常量更多） | **中**（删=失去全部验收通道） | **保留**，只做 B3-B5、C4 定点裁 |
| **B8** | `app_main.cpp:1930-1966` `panel_align_test` | 仅调试可用 | `nm` 0x736=**1846 B**，唯一入口是 `G 3` / `E` 命令 | 1.8 KB flash | 中（几何标定唯一手段） | 保留或 `C3_PANELTEST` |
| **D5** | `app_main.cpp:3829-3904` STAT 巨型行 | 仅调试可用（默认关） | `s_stat_period_s=0`；格式串实测 857 B，`J 1` 打开后 ≈74 ms 阻塞/行 | ≈1.5-2.5 KB flash（估，含 858 B 字符串） | 低 | 不动；可先删恒零字段 |
| **B7** | `app_main.cpp:1768-1833` `selftest_step` | 仅调试可用 | `s_st_phase = C3_SELFTEST ? 0 : -1`，宏默认 0 → 每帧一次 load+比较；`nm` 已内联，但 `s_st_*` **6×4 B** 状态还在 | DRAM 24 B + 每帧 ~6 cycle | 低 | 整体进 `#if C3_SELFTEST` |
| **C1** | `menu.cpp:36` 与 `bezel.cpp:30` 各 `#include "font5x7.inc"` | 重复实现 | `nm`：两个 `_ZL8kFont5x7`，各 `0x127=295 B`（0x3c0c0a14 / 0x3c0c08ec） | 可省 **295 B flash** | 低 | 合并成一个 TU 定义 + `extern` |
| **B6** | `app_main.cpp:825-879` `kFont3x5`+`draw_fps_overlay` | 调试残留（常驻叠加层） | `nm`：`box` = **576 B .bss**；1 Hz 一次 576 B memset + 24×12 SPI 推送，画在 (2,2) | DRAM 576 B + 1 Hz 一次 SPI | 中（唯一「不插串口看帧率」手段） | 改编译期开关，默认关 |
| **B3** | `app_main.cpp:1728-1730` `Y` | 命令已无输出路径 | `emu/unsp_glue.cpp:961`：`C3_BLOCK_CENSUS=0` 时 `aot_dump_blocks(){}` 是空函数 → 发 `Y` 收到 0 字节 | ~30 B | 低 | 删 `Y` + 空桩 |
| **B5** | `app_main.cpp:1731-1742` `O` | 命令 3 行里 2 行恒零 | `g_wait_top/g_wait_exit/g_frame_body/g_body_alt`（`unsp_glue.cpp:1492-1495`）**只有定义、从没自增** | ~120 B 代码 + 16 B DRAM | 低 | 删 PHASE 行 + 4 个全局 |
| **B4** | `app_main.cpp:1725-1727` `M` | 命令退化成固定桩 | `C3_MISS_CENSUS=0` → `aot_dump_miss()` 只打 `MISSHEAD 0 / CENSUS end 0` | ~16 B | 低 | 删或开关化 |
| **C4** | `app_main.cpp:1279-1285` `A` | 旧路径（且已被改坏） | 它注入 `KEY0\|KEY2` 走 `s_inject_left=kPressFrames`，但 A 在 2271/2421 被剥离 → 实际先给 guest **C 六帧**，再补 **A 六帧**，从来不是和弦；与 `start_chord()`（2325-2328 注释：ROM 忽略短于数秒的和弦）重复 | ~80 B | 低 | 删（要留就改调 `start_chord`） |
| **A3** | `app_main.cpp:506-508` + `3859-3861` + `3920/3926/3927` | 真死（从不自增） | `s_hot_calls`/`s_slices`/`s_tick_us` 各只有「定义 / STAT 打印 / 清零」，**没有一处 `++`** | DRAM 12 B + STAT 三个恒零字段 | 低 | 删变量+字段 |
| **A9** | `menu.cpp:555-589` + `323` + `menu.h:105-109` | 真死（零调用） | 全工程只有定义与声明；map「Discarded input sections」明确列出 `.text` 0x338(824 B)/`.bss` 0x280(640 B)/`.rodata` 0x24 → **链接器已丢** | 现 **0 B**；误用会带回 1464 B | 极低 | 删（连 `s_wide` 与 .h 声明） |
| **D1** | `app_main.cpp:3132-3145` | 热路径纯开销 | 每帧开头 12 个全局快照，唯一读点是 3558-3570 的 LONGF（`frame_us>60 ms`） | 每帧 ≈12 次 load（~30 cycle） | 低 | 移进 LONGF 分支或 `#if` |
| **D2** | `app_main.cpp:3181, 3193` | 热路径纯开销 | 每 slice 2 次 `esp_timer_get_time`（仅喂 `s_slice_max_us`/`s_tick_max_us`）；objdump 显示 `app_main` 内共 **28 个** `esp_timer_get_time` 调用点 | ≈22-80 次多余调用/帧（每帧 11-40 slice），≈0.05-0.1% 帧预算（估） | 低 | 包进 `#if C3_STAT` |
| **D3** | `app_main.cpp:1127-1136, 1174-1181` | 热路径纯开销 | blit 每次 2 次 timer 读，只喂 `s_blit_us/s_blit_max_us/s_pack_us`（仅 STAT） | ≈3-12 次调用/帧 | 低 | 同上 |
| **D4** | `app_main.cpp:3420-3425` + `553-593` | 热路径纯开销 | 每帧一次 32 桶直方图，`frame_time_pct` 只被 STAT 的 3876-3877 调用 | ≈10-20 cycle/帧 | 低 | 同上 |
| **A4/A5/A6/A7/A8/A10/A11** | 见二-A | 真死 / 编译期死分支 / 陈旧注释 | 见二-A | 0 B（编译器/链接器已消除） | 极低 | 删（纯源码卫生） |
| **A1/A2** | `app_main.cpp:274`、`2681` | 真死（只写不读） | `s_b_hold_locked` 5 处全是定义/赋值；`s_snap_count` 定义+自增 0 读 | A1 已被 -O3 消除 = 0 B；A2 = 4 B DRAM | 低 | 删 |
| **D6** | `app_main.cpp:3657` | 每帧忙等 | `while (esp_timer_get_time() < s_deadline_us) {}`，上限 1.5 ms（3650-3652 注释自述 5.5 ms 时占 1/3 wall clock） | ≈1.5 ms/帧 ≈ **9% wall clock**（58 fps） | 中 | **不动**（帧对齐靠它） |

**可以现在就删、且不影响功能的总账（不含 B2/B8/D5/D6 这四个要裁决的）：**

- flash：B1 1.9 KB + C1 295 B + B3/B4/B5/C4 ≈250 B ≈ **2.4 KB**（占 app 0.22%）
- DRAM：A2 4 B + A3 12 B + B5 16 B + B7 24 B ≈ **56 B**；若主审同意把 B6 改成默认关，
  再加 576 B ≈ **632 B**（占 bss 0.56%）
- 每帧：删掉 D1-D4 后 ≈0.1% 帧预算。**热路径上没有一条能靠删死代码拿到 1% 以上**；
  能上量级的只有 D6 的忙等，而那是设计选择不是残留。
  这句放在最前面，是为了避免按「瘦身 = 提速」的预期排期：本轮收益基本是 flash/DRAM 与可维护性，不是 CPU。

---

## 二、逐条证据

### A 组：真死代码

**A1 `s_b_hold_locked`（app_main.cpp:274，赋值在 2337/2350/2382/2392）**
检索 `\bs_b_hold_locked\b`，全工程 5 处：1 处定义 + 4 处 `= true`，**零读取**。
源码里自己也写着「nothing reads this any more」（271-273 行）。`nm` 里没有这个符号 ——
-O3 的死存储消除已经把它整条删掉了，**当前 0 B**。留着它唯一的后果是下一个读代码的人
以为「B 长按锁」这个机制还活着（它随 B 长按开菜单一起被砍了，2229-2235 行）。

**A2 `s_snap_count`（app_main.cpp:2681，自增在 2740）**
检索 2 处：定义 + `s_snap_count++`，零读取（STAT 也没打印它）。`nm`：`_ZL12s_snap_count`
在 `.sbss`，**4 B DRAM**。要么删，要么把它接到 STAT 的 CLK 行里 —— 现在它是纯漏出。

**A3 `s_hot_calls` / `s_slices` / `s_tick_us`（506-508；打印 3859-3861；清零 3920/3926/3927）**
三个都是「定义了、打印了、每次 STAT 清零了，就是**从来没人加**」。
`s_hot_calls` 的注释还写着「PRNG block executions since log」—— 对应的自增在某一轮被删了，
字段留在 STAT 里当常量 0。`nm` 三个都在 `.sbss`：**12 B DRAM**。
这是本轮最典型的「占着热路径（其实是占着日志行）却没产出」：它不是每帧开销，
但它是**每个读日志的人的时间和每行 STAT 的字节**。

**A4 `enum { kCmdNone = 0 };`（1210-1212）**
检索 `\bkCmdNone\b`：1 处，就是它自己。空枚举，删。

**A5 `kAcHoldDefaultUs`（222）**
检索：1 处。定义后从未引用（`s_ac_hold_us` 用字面量 1200000 初始化）。
注意别把 `s_ac_hold_us` 一起删 —— 它是 `H <ms>` 和长 A 阈值共用的活变量。

**A6 `kAutoPressAt` + 2404-2408（276）**
`static constexpr uint32_t kAutoPressAt = 0;`，使用点是 `if (kAutoPressAt != 0 && ...)`，
编译期恒假。`nm` 无相关代码 = **0 B / 0 cycle**。属于「诊断自动按键」的残留。

**A7 `rot_map()` + `kRot != 0` 分支（905、910-938、1156-1171）**
- `kRot` 是 `constexpr int kRot = 0`，所以 `if (kRot == 0)`（1156）恒真、`else`（1163-1171）不可达；
- `rot_map()` 的**唯一调用点**就在那个 `else` 里（1166），因此它没有活引用，`nm` 里也没有符号 = 0 B；
- 顺带一个真 bug：`switch (kRot & 3)`（912）配 `case 4:`（925）/`case 5:`（929）。
  `x & 3` 永远不可能等于 4 或 5，**水平/垂直镜像这两条分支就算把 kRot 改成 4/5 也永远进不去**。
  如果这是留给「屏装反了」的备用路径，那它现在是坏的；如果不要，就连 case 一起删。

**A8 `s_shot_done` / `s_shot2_done`（643-644）**
唯二使用点在 `#if C3_DEBUG_DUMP`（3690-3711）里，而 `C3_DEBUG_DUMP` 默认 0（618-620），
CMakeLists 也没定义它 → 变量无引用，`nm` 无符号 = **0 B**。
建议把这两个声明也移进 `#if` 里，别在文件头留两个孤儿状态。

**A9 `menu_banner_present()` + `s_wide`（menu.cpp:555-589、323；menu.h:105-109）**
检索 `\bmenu_banner_present\b` 全工程：**2 处 = 定义 + 声明，零调用**。
app_main 那边自己在 3624-3625 写着「The top-margin banner is gone with the standby card (v1.16.0)」。
map 的「Discarded input sections」（第 858-9123 行区间）确实列出了它：
`.text._Z19menu_banner_presentPhiPKc` **0x338 = 824 B**、`.bss._ZL6s_wide` **0x280 = 640 B**、
`.rodata...str1.4` 36 B —— 也就是说**链接器已经把它全丢了，当前 0 字节**。
所以删它不是为了省空间，是为了别让 `menu.h:105-108` 继续对外承诺一条不存在的通道。

**A10 重复声明（app_main.cpp:357/358、1221/2461）**
`void aot_dump_miss(void);` 连写两遍；`static void restore_machine_state(void);` 在 1221 和 2461
各声明一次。0 成本，但会让人以为有两个不同函数。

**A11 陈旧注释（app_main.cpp:55-57、991-1014、1759-1761）**
- 55-57：「The 5x7 face ... Its own copy of the table (295 B)」——**本文件早就没有 5x7 表了**
  （检索 `font5x7|kFont5x7`：只在 menu.cpp/bezel.cpp），电池读数搬去 bezel.cpp 时这段注释没跟着走。
- 991-1014：整整 24 行在描述四个「已经不在这里」的画法（`brick_px`/`panel_brick_band`/
  `panel_draw_battery`/`panel_draw_bezel`）。信息有价值，但更该进 C3_PORT.md 而不是留在热文件里。
- 1759-1761：「The UART's RX line is not wired on this carrier board, so the host cannot send
  commands in」——与 1835-1864 的 `debug_poll()` 直接矛盾（它明确从 USB-Serial/JTAG 收命令，
  tools/c3_key.py 也证明能收）。这是最危险的一类陈旧注释：它会让下一个审计员得出
  「console_exec 根本进不来，所以整个命令表都是死代码」的错误结论。

### B 组：仅调试可用

**B1 A+C 确认卡（menu.cpp:783-897 + app_main 三处 + menu.h:133-142）**
证据链完整：
1. `menu_confirm_set(bool)` 全工程 4 处：`app_main 2381`、`app_main 2391`（都是 `false`）、
   定义、声明。**没有任何 `menu_confirm_set(true)`**。
2. 于是 `s_confirm` 从初始化起恒为 `false`，`menu_confirm_is_open()` 恒假（855-858）。
3. 于是 `menu_confirm_present()` 第一行 `if (!s_confirm || !s_confirm_dirty) return;`（862）永不放行，
   `confirm_line()`（801-847）不可达。
4. 于是 `app_main 2371-2402`（整段按键处理）、`3476-3485`（每帧绘制）、`2150/2822/3587` 里的
   `!menu_confirm_is_open()` 全部恒真/恒假。
5. `nm`：`menu_confirm_present` **1868 B** 在 `.flash.text`，另 `set` 16 B、`is_open` 10 B。
   DRAM：`s_confirm`(1) + `s_confirm_dirty`(1) + `s_confirm_fresh`(1) + `s_confirm_pad_prev`(4)。
6. `s_confirm_fresh`（254-255）**只在 2375/2378 被读/写**，也就是只在这段死代码里 —— 一并归零。

menu.h:133-139 自己承认：「Superseded by row 0 of the settings menu ... nothing raises this modal
any more. It is kept because ...」。**删掉它能省 1.9 KB flash，而且不可能破坏任何现有功能**
（唯一「风险」是产品决定：万一以后要在菜单里再加一次确认步骤）。
若主审想保留这个可能性，正确做法是 `#if C3_CONFIRM_CARD`（默认 0），而不是编译着 1.9 KB 的死画法。

**B2 `console_exec` 命令表（1268-1754）**
`nm`：`console_exec` **0x13ca = 5066 B**，是 app_main.cpp 里仅次于 `app_main` 本体（0x373e = 14,142 B）
的第二大函数，也是唯一一个「一行命令换 5 KB」的地方。
逐条核对「谁还在发这个字母」——检索 `tools/*.py|*.ps1` 里的写串：
实际有工具在用的只有 `V`（c3_autotest.py）、`S`（autotest / c3_clock_audit / c3_setclock）、
`K`（autotest / c3_key / c3_clock_audit / r5_keyseq）、`J`（r5_keyseq 示例）、
`t`（c3_setclock / c3_clock_audit）、`D`、`U`（c3_clock_audit）。
其余 `A H k N T X Q Z a p R y j i f F B C L G E P W M Y O` 没有任何脚本在用，
但它们是**手工 bench 的通道**（T 是 tools/c3_shot.py 的唯一数据来源，
N 是唯一能看菜单出图的手段，G/E 是几何标定的唯一手段）。
结论：**整表保留**；要瘦身只能做 B3/B4/B5/C4 这四处定点，共 ≈250 B。

**B3 `Y`（1728-1730）**：`aot_dump_blocks()` 在 `unsp_glue.cpp:961` 是 `{}`（`C3_BLOCK_CENSUS=0`）。
发 `Y` 会得到零字节回复 —— 这是全表里**唯一一条真正的空命令**。

**B4 `M`（1725-1727）**：`C3_MISS_CENSUS=0` 时 `aot_dump_miss()` 是 993-996 的桩，
固定打印 `MISSHEAD 0` + `CENSUS end 0`（后者读 `g_aot_uniq`，而 census 关着时它也恒 0）。

**B5 `O`（1731-1742）**：`g_wait_top`/`g_wait_exit`/`g_frame_body`/`g_body_alt` 在
`unsp_glue.cpp:1492-1495` 只有定义，**四个都从没被 `++`**（检索 `\bg_wait_top\b` 等：3 处 =
定义/extern/打印）。所以 `PHASE wait=0 exit=0 body=0 alt=0 ioa=...` 里唯一有意义的是 `ioa`。

**B6 帧率叠加层（825-879）**
`kFont3x5` 50 B + `box` 576 B `.bss`；每 1 s 一次 `memset(box,0,576)` + 三位数字各 3×5 个 2×2
点阵块填充 + 一次 `display_draw_bitmap(2,2,24,12)`。它画在边框里，是**产品可见**的常驻数字。
判断：这是「不接串口也能读帧率」的唯一手段，属于**有价值的调试件**，但它默认常开、
占 576 B DRAM、每帧还要付一次 `esp_timer_get_time()`（3718-3731 那段）。
建议：`#if C3_FPS_OVERLAY`（默认 0，bench 设 1）。**不建议直接删** —— 光靠串口 log 判断
「面板到底刷了多少帧」在 handset 上不可行。

**B7 `selftest_step()`（1768-1833）**
`s_st_phase = C3_SELFTEST ? 0 : -1`，宏默认 0（1768-1770）。函数每帧被调一次（3443），
第一句 `if (s_st_phase < 0) return;` —— `nm` 显示它已被内联进 `app_main`，
但 `s_st_phase/s_st_timer/s_st_idle0/s_st_press0/s_st_guard0/s_st_taps` **6 个 4 B 状态仍在**
（`.data`/`.bss`）。改法：`#if C3_SELFTEST` 把函数+状态+调用点整块包起来，省 24 B DRAM
和每帧一次分支。**不建议删源码**：这是唯一一条「无人操作也能自证显示链路」的自动验收序列。

**B8 `panel_edge_test()` / `panel_align_test()`（1892-1915 / 1930-1966）**
`nm`：220 B + 1846 B。入口是 `G 2`/`G 3`/`E` 和主循环 3592-3595。
`panel_align_test` 是 1.8 KB 里最大的一笔「纯画图」代码，但它回答的是
「图到底偏了还是玻璃偏了」这个已经反复返工过的问题（docs/HANDOFF_UI_POSITION.md）。
**保留**，或 `C3_PANELTEST` 开关化。

**B9/B10 `bench_hotblock`/`bench_exec`（3238-3309）、`AUTOWALK`（3449-3460）**
`C3_BENCH`、`C3_AUTOWALK` 都默认 0 且 CMakeLists 未定义 → 编译期整块消失，**0 B**。
已经做对了，保持现状即可（CMakeLists 注释里「a ~24 KiB of static scratch」的说法现在也确实不成立）。

### C 组：重复实现

**C1 字体表两份（menu.cpp:36、bezel.cpp:30 都 include font5x7.inc）**
`font5x7.inc` 里是 `static const uint8_t kFont5x7[]`，每个 TU 一份。
`nm` 实锤：`0x3c0c08ec` 和 `0x3c0c0a14` **两个 295 B 副本**，共 590 B。
改法：新建 `font5x7.h`（`inline constexpr uint8_t kFont5x7[]`，C++17 保证单实例）
或在一个 .cpp 定义 + `extern`。省 295 B，零功能风险。**这是本轮唯一一条零风险的重复实现**。

**C2 四个 band 推送循环（menu.cpp 717-775 / 860-897 / 595-647 / 654-715）**
四个函数都是「24 行一条带 → `display_draw_bitmap`」，但尺寸、是否采样背景、是否缩放各不相同。
删掉 B1 之后剩三个。**建议不动**：把四份合成一份的抽象成本高于 3 份各自 40 行的重复，
而且这条路径已经在 1.16.0 出过一次越界（app_main 373-395 的注释记着那次 3 KB 溢出）。

**C3 `T` / `L` / `C` 三条帧缓冲读取命令（1348-1353 / 1634-1644 / 1625-1633）**
都是读 `s_soc.lcd.fb`：`T` 整窗 96×96 hex（喂 c3_shot.py）、`L` 单行 nibble、`C` 全帧 CRC。
功能重叠但用途不同（图像 / 局部 / 变化检测），**不动**。

**C4 `A` 命令 vs 菜单 SEND A+C 行（1279-1285 vs 2323-2343）**
这是本轮发现里唯一一条「旧路径不但没删，而且已经被后续版本改坏」的：
- 命令自称「inject the A+C chord, like a held A」；
- 它设 `s_inject_mask = BTN_KEY0|BTN_KEY2`、`s_inject_left = kPressFrames`（6 帧）；
- 但 2026-11-27 的「A 扣留」改造之后，A 在 `pad = instant & ~BTN_KEY0`（2271）和
  `mask = ... (instant & ~BTN_KEY0)`（2421）两处都被剥掉，A 只能通过「释放时补脉冲」
  （`!a_down && s_a_withheld` → `s_hold_mask = BTN_KEY0`，2184-2196）到达 guest；
- 于是 `A` 的实际效果是：**guest 先收到 6 帧 C，再收到 6 帧 A**，两次单键，中间还隔着
  一个释放边沿。它既不是和弦，也永远不可能被 ROM 认成 A+C。
- 而真正的和弦通道（菜单 row 0 → `start_chord(KEY0|KEY2, 700 ms + 3200 ms)`）是**按住** 3.2 s，
  2330-2335 的注释明确说「the cartridge ignores anything shorter than a couple of seconds」。
结论：`A` 是这条逻辑的旧实现，语义已被取代且当前是错的。删（≈80 B），
或改成 `start_chord(BTN_KEY0|BTN_KEY2, kPadBlankUs + kAcChordUs)`。**别留着让人误以为它可用**。

### D 组：热路径上的纯开销

先说结论：**这一组的量级都很小**（合计 ≈0.1-0.2% 帧预算）。真正每帧烧掉可观时间的是
D6 的忙等（设计如此），不是这些计数器。之所以仍然列出来，是因为它们**只在 STAT 打开时才有产出**，
而 STAT 默认是关的（`s_stat_period_s = 0`，500-505 行的注释解释过原因：每行 74-78 ms 阻塞）。

**D1 每帧 12 个全局快照（3132-3145）**
`run_guest_frame()` 一进来就无条件读 `m_aot_blocks/m_aot_insns/m_aot_miss/m_aot_fallback` +
`g_h_band/g_h_blit/g_h_push/g_h_fill/g_h_mul` + `g_hle_band_recs/g_blit_px_total/g_hle_push_px_total`，
存进 `s_frame_*`/`s_hp_*`/`s_px_*`。这些值的**唯一消费者**是 3551-3571 的 LONGF 分支
（`if (frame_us > 60000)`）——即每秒最多几次、每次一行。
改法：把这 12 个 load 移进 LONGF 分支，并接受「delta 是上一次打印至今」的语义（本来也是估算用），
或用 `#if C3_STAT`。

**D2 每 slice 两次 `esp_timer_get_time`（3181 `ts0`、3193 `tt0`）**
`kSliceCycles = 48000`、默认帧 533,333 cycles → 每帧至少 11 个 slice，遇到 timer 事件更多。
其中 3158 那次是**功能必需**（帧预算守卫），`ts0`/`tt0` 只喂 `s_slice_max_us`/`s_tick_max_us`
（→ STAT 的 `smax`/`tmax`）。objdump 统计 `app_main` 内 `esp_timer_get_time` 调用点共 **28 个**，
且它是**独立函数**（`0x40381584`，36 B），不是内联的寄存器读 —— 每次都有调用开销。
按 40 cycle/次、11-40 slice 估：每帧 ≈900-3200 cycle，约 0.04-0.15% 的 14 ms 帧。
量小，但它是纯粹的「为了日志」。

**D3 blit 计时（1127-1136 整窗路径、1174-1181 带路径）**
整窗路径 1 次 `t_pack` + 1 次 `t_blit` + 1 次收尾 = 3 次/帧；带路径每带 2 次（`t_blit`+收尾），
6 带最坏 = 12 次/帧。全部只喂 `s_blit_us/s_blit_max_us/s_pack_us/s_rows_sent/s_blits` → 只有 STAT 打印。

**D4 帧间隔直方图（3420-3425 调用、553-593 实现）**
每帧：1 次 `esp_timer_get_time` + 1 次桶号除法 + 1 次 `s_ft_hist[]++` + 4 次比较 + 1 次 max 比较。
产出只有 STAT 的 `ft/ftmx/ov20/ov33/ov50/ov100`。默认关 → 每帧 ≈10-20 cycle 白烧。

**D5 STAT 巨型行（3829-3904）**
一行 **857 字符**的格式串（把 3829-3841 的 12 个字面量逐个量过，合计 857 B，全进 `.rodata`）
+ 60 个字段的取值代码，全在 `if (s_stat_period_s <= 0) {...} else {...}` 的 else 分支里 ——
**关着的时候不执行，但代码和字符串仍然占 flash**（估 1.5-2.5 KB 含 858 B 字符串；代码部分
无法从符号表里单独切出来，故标注为估）。857 B @115200 ≈ 74 ms 阻塞，与源码注释的「~78 ms」吻合。
更要紧的是**内容质量**：这行里 **36 个字段**是结构性恒零（逐个数过）：
- `rdf/rds/wrf/wrs`（C3_AOTACC=0，注释已声明有意保留）
- `caot/cinf/cfast/daot/dinf/cpa/cpi/clk/cfn/pd/pp`（C3_SPLIT=0）
- `riram/rflash/rram/rpush/rdraw`（C3_REGION_STAT=0）
- `stage=slice/exec/tick`（C3_PROF=0，注释已声明有意保留）
- `spin=…/spinv=…` 共 8 个（C3_SPIN_DIAG=0 → `SPIN_PROBE` 是空宏，8 个全局恒 0）
- `aotuniq`（C3_MISS_CENSUS=0）
- **`pg`（`g_aot_pg_switch`，全工程从没自增）**
- **`hot`（`s_hot_calls`）、`slices`（`s_slices`）、`tick_pct`（`s_tick_us`）**

前六组是「宏关掉所以是 0，字段保留给 log parser 兼容」（CMakeLists 的注释明确选择了这条路），
**最后四个是纯粹没有实现的字段**，删掉不违反任何兼容承诺。

**D6 忙等 spin（3632-3659，重点是 3657）**
`while (esp_timer_get_time() < s_deadline_us) {}` —— 每帧最多 1.5 ms，每次循环一个函数调用。
3650-3652 的注释自己算过账：「5.5 ms of spin per frame at 58 fps was a third of the wall clock」，
所以已经收到 1.5 ms。1.5 ms / 16.6 ms ≈ **9%，是整套固件里最大的一笔「纯 host 时间」**。
但它**有产出**（帧对齐），不属于死代码 → 放进第三节「不建议动」，只提示一个可选微优化：
把 1.5 ms 的窗口再收窄，或用 `esp_cpu_get_cycle_count()` 直接自旋（省掉那次 36 B 的调用）。

### E 组：宏开关与「永久税」核对（特别检查项 5）

CMakeLists 里那批 `-D` 与源码里的 `#ifndef` 默认值，逐个核对**是否真的清零**：

| 开关 | CMakeLists | 源码默认 | 实际生效 | 结论 |
|---|---|---|---|---|
| `C3_PROF` | 0 | —（无 `#ifndef`） | 0 | ✅ 清。objdump 在 `app_main` 全函数（0x4200d420-0x42010b5e）里数到 **0 条 `csrr …,0x7e2`**（C3 周期计数器 CSR 0x7E2），而 `execute_run()` 里有 **16 条** —— 检测方法有效，app_main 侧确实清零 |
| `C3_AOTACC` | 0 | — | 0 | ✅ 清。STAT 的 `rdf/rds/wrf/wrs` 恒 0（有意保留） |
| `C3_SPLIT` | 0 | **1**（unsp_glue.cpp:57） | 0（CMake 覆盖） | ✅ 清，但**源码默认值是 1**；一旦有人手工去掉 `-DC3_SPLIT=0`，会把 hot path 的 SPLIT 计时全打开。建议源码默认改 0 |
| `C3_MISS_CENSUS` | 0 | 0 | 0 | ✅ 清 → `M` 退化成桩（B4），`aotuniq` 恒 0 |
| `C3_BLOCK_CENSUS` | 0 | 0 | 0 | ✅ 清 → `Y` 变成空命令（B3） |
| `C3_HLEPROF` | **1** | 0 | **1** | ❌ **没清**。`HLE_T0/HLE_ADD` 活着（unsp_glue.cpp:153-165、五个钩子 1922-1968 各一对），objdump 在 `execute_run()` 里数到 **16 条 `csrr …,0x7e2`**；唯一消费者是 STAT（默认关）和 LONGF（>60 ms）→ 见第四节 |
| `C3_NO_AOT` | 0 | — | 0 | ✅ |
| `C3_SKIP_BLIT` | 0 | 0（app_main:40-42） | 0 | ✅，但这个宏在 app_main 里**定义后没有任何使用点**（检索 `C3_SKIP_BLIT` = 2 处：`#ifndef` + `#define`）。死宏，可删 |
| `C3_WAIT_COLLAPSE` | 1 | 0（unsp_glue:90） | 1 | ✅ 功能开关，保持 |
| `C3_SELFTEST` | 未定义 | 0 | 0 | 见 B7：代码还在，每帧一次判断 |
| `C3_AUTOWALK` | 未定义 | 0 | 0 | ✅ 整块消失 |
| `C3_BENCH` | 未定义 | 0（app_main:628-630） | 0 | ✅ 整块消失 |
| `C3_DEBUG_DUMP` | 未定义 | 0（app_main:618-620） | 0 | ✅ 整块消失，但留下两个孤儿变量（A8） |
| `C3_IRQ_DIAG` | 未定义 | 0 | 0 | ✅（emu 侧，前几轮已处理） |
| `C3_REGION_STAT` | 未定义 | 0 | 0 | ✅ |
| `C3_SPIN_DIAG` | 未定义 | 0 | 0 | ✅ 但 STAT 仍打印 8 个恒零字段（D5） |
| `C3_TICK_DIAG` / `C3_TICK_TRACE` / `C3_IOA_TRACE` / `C3_VERIFY` / `C3_FASTOPS` | 未定义 | 0 | 0 | ✅ |

**一句话回答「中国式注释里写的永久税是否真的清零」**：
`C3_PROF`、`C3_AOTACC`、`C3_SPLIT`、两个 census 确实清零了，且反汇编能证；
**唯一没清的是 `C3_HLEPROF=1`**，而它的产出（每帧 `hp=` 归因）只在 STAT 打开或长帧时可见。

---

## 三、不建议动的（附理由）

1. **`console_exec` 整表（B2）** —— 5.1 KB flash 看着肉疼，但它是唯一的验收/取证通道：
   `T`→c3_shot.py、`N`→菜单出图、`S`/`V`→autotest、`t`→对时、`D`/`U`/`i`→快照回放、
   `K`→按键注入。删掉它省 0.4% flash，换来「所有后续轮次只能靠串口日志猜」。
2. **`T` / `N` / `G` / `E` / `panel_edge_test` / `panel_align_test`** —— 几何与出图的唯一手段，
   已经因为窗口从 128×112 改到 96×96 返工过一轮（app_main 124-141、373-395 的注释）。
3. **D6 忙等 spin** —— 1.5 ms/帧确实是 9% wall clock，但它保证「面板每帧都是新的」。
   上一轮已从 5.5 ms 收到 1.5 ms，再收就得动帧对齐策略，风险远大于收益。
4. **四个 band 推送循环合并（C2）** —— 抽象成本 > 3×40 行重复；这条路已经在 1.16.0 出过
   越界复位（`s_strip` 的两个消费者尺寸不同），合并时最容易把缓冲区契约搞错。
5. **`s_strip`（15,360 B）与 `s_frame_buf`（73,728 B heap）** —— 已经是共享/可选：
   `s_strip` 取两个消费者的 max（390-396），`s_frame_buf` 分配失败会退回带路径（1108）。
   这是刻意设计，不是冗余。
6. **`debug_poll()` 每帧 4 次寄存器读** —— 唯一输入通道（UART0 + USB-JTAG 双前端），成本可忽略。
7. **`audio_set_buzzer(s_soc.buzzer_hz())` 每帧** —— 功能（buzzer 状态同步），不是日志。
8. **`s_stat_period_s` 默认 0 这个决定** —— 500-505 的解释成立（每行 74-78 ms 阻塞 = 每秒丢 4-5 帧），
   `J` 能现场打开就够了。

---

## 四、不确定、需要主审裁决

1. **B1 确认卡：删，还是 `#if`？** 我倾向删（1.9 KB，且它会诱导下一个读者以为「按 A 会弹确认卡」）。
   但 menu.h 明确写了「保留因为和弦可能再需要一次确认」—— 这是产品决定，不是技术决定。
2. **STAT 的恒零字段**：CMakeLists 的注释说「the STAT line keeps printing rdf/rds/wrf/wrs as zeros
   so log parsers do not have to change」——即**故意保持字段形状**。那么 `hot/slices/tick_pct/pg`
   这四个（A3/D5）是跟着一起保留形状，还是按「从来没实现过」删掉？我建议只删这四个，
   不动宏控的那些。
3. **`Y` / `M` 命令删不删**（B3/B4）：删了不影响任何工具（没有脚本发它们），
   但 census 重开时只能改 CMake/源码。命令本身不是开关，若主审接受「重开 census 时顺手加回命令」，就删。
4. **`C3_HLEPROF=1`**：本轮唯一没清零的「永久税」，但**加税点在 `emu/unsp_glue.cpp`**
   （不在我的三个文件里）。实测：`execute_run()` 的 IRAM 体里有 16 条 `csrr …,0x7e2`，
   app_main 全函数 0 条 —— 这 16 条就是 HLEPROF 的 5 对 `HLE_T0/HLE_ADD`（外加余量）。
   好消息是它们**不在每 dispatch 路径上**：都写在 `if (m_hle_blit && lpc == AOT_HLE_BLIT_ENTRY)`
   这类判断**之后**，只在钩子真正命中时付（每帧约 96 行 × 若干钩子 ≈ 数百次），
   估 ≈0.1-0.2% 帧预算，与 unsp_glue.cpp:149 注释「~3 host cycles per call - noise」一致。
   建议 CMakeLists 改 `-DC3_HLEPROF=0` —— 需 emu 侧审计员确认没有别的读者。
5. **`A` 命令（C4）是删还是修**：它现在是错的（先给 C 再给 A，不是和弦）。
   如果保留它作为「手工和弦」入口，应改调 `start_chord()`；如果认为菜单 row 0 已覆盖，直接删。
   **不管哪种，都不该保持现状。**
6. **`rot_map` 的 `case 4/5` 与 `kRot & 3` 矛盾**（A7）：原意像是「4=水平镜像、5=垂直镜像」
   作为屏装反的补救路径，但掩码让它们永远不可达。删掉等于承认镜像路径不存在；
   修好等于承诺一条没人验证过的渲染路径（还要同步改 1108 行只支持 `kRot == 0` 的全窗口 pack）。
   我建议**删**，并在注释里写明这一点。
7. **`s_snap_count`（A2）**：是「准备接到 STAT 但忘了接」，还是「留给以后」？
   它连注释都没有，我无法判断。默认建议：删。
8. **`s_b_hold_locked`（A1）**：源码注释说「kept because the card code path still sets it and may
   come back」—— 但那个 card 本身（B1）已经死了。两条一起删才自洽；只删一条会留下更费解的状态。
9. **`buttons.c` 没有死代码**（见附），但两处文档/实现不一致值得顺手修：
   `buttons.h:23-27` 与 `buttons.c:16` 都写「20 ms 轮询」，实际 `BTN_POLL_MS = 5`（buttons.c:43）；
   `buttons.c:22-23` 的 `#include <stdlib.h>`/`<string.h>` 在本文件里没有任何使用点。
   不影响任何字节，属文档卫生，是否纳入本轮清理请主审定。

---

## 附：buttons.c / menu.h 的结论（范围内但没有可删项）

- `buttons.c`：`buttons_scan/state/take_latched/last_mv` 全部有活调用点
  （`buttons_scan` 只被 `button_task` 调，但它是对外 API，保留）；`button_fallback()` 只在
  ADC 初始化失败路径用；`s_adc/s_cali/s_raw/s_same/s_state/s_q*` 都在用。
  **无真死代码、无热路径纯开销**（ADC 采样在独立 task，5 ms 一次，与主循环无关）。
  唯一可议的是两个未使用的 include 和过时的「20 ms」注释。
- `menu.h`：除 A9（`menu_banner_present`）与 B1（`menu_confirm_*`）那两段声明外，
  其余 API 全部有调用点（`menu_take_action / menu_load / open / close / is_open / press_a|b|c /
  auto_off_sec / bright_pct / volume_pct / autosave_sec / time_lapse_x / set_time_lapse_idx /
  time_lapse_idx / present / lapse_toast_present / standby_present / set_capture`）。

---

*报告完毕。本轮未修改任何源码，未运行 idf.py；唯一写入的文件是本报告。*
