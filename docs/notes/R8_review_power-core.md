# R8 交叉互审：功耗（R8_power）× 核心热路径（R8_core）

> 角色：第三方固件审计团队的交叉互审员（只读）。
> 对象：`docs/notes/R8_power.md`（11 条，其中 P0/P1 共 8 条）、`docs/notes/R8_core.md`（6 条，其中 P0/P1 共 3 条）。
> 基线：v1.18.0，HEAD `741a49e`。源码只读；本轮唯一写入的文件是本文件。
> 结论一律落到「文件:行号 + 代码/反汇编摘录」，并标注 实测 / 静态推导 / 推测。

---

## 0. 结论摘要

1. **10 条 P0/P1 全部复现了机理，没有一条是该 reject 的凭空代码**；但 6 条需要降级：3 条「收益/前提站不住」（F5、F9、C2），2 条「无可执行改动或收益≈0」（F1、F2），1 条「只是另一条的复述」（C6）。
2. **R8-C1 是本轮唯一经得起反汇编复核的性能项**：`hot_prng_n` 的循环体实测 **236 条指令/pass**，其中 **13 次 `lhu` + 12 次 `sh`**；源注释「keeps the eight state words … in locals and writes memory once per pass」（`unsp_glue.cpp:631-632`）与代码不符，hoist 保持逐 pass 等价（中间状态对外不可见）。
3. **R8-C2 与 R7 的 D4 是同一件事，本轮零新证据**（`R7_aot.md:542` 逐字同结论），且「少 miss → 降占空比」不成立：1,048/1,097 个重叠块在 block census 里零命中（`R7_aot.md:353-355`），删/改它们不减少任何 miss。Q7 表里「重绘帧 20–40%」无依据。
4. **F1 的机理成立但要降级**：`run_guest_frame()`（`app_main.cpp:3497`）确实在待机分支外每帧执行，待机的 ROM 也确实在推进游戏时间；但报告给不出任何具体改动（建议改动交叉指向 F3=crcwatch，指错了），26→9 mA / 待机续航 ×3 全是同类推测，且 §0 标 P0、表里写 P1。
5. **两份报告都漏掉了「待机/性能」上真正更大的三件事**：`hot_prng_n` 的 16 位掩码脚手架（占 236 条里的 ~95 条移位指令，收益约 C1 的 2 倍）、存档 300 ms 停顿落在 `frame_us` 取样窗之外（遥测看不见）、`AUTO OFF=NEVER` 会让待机卡背光常亮 80%。

---

## 1. 范围、口径与工作树状态

**读了**：`docs/notes/R8_power.md`、`docs/notes/R8_core.md` 全文；`docs/notes/R8_BRIEF.md`；
`firmware/digicore-c3/main/app_main.cpp`（帧循环 3395-3720、standby 1962-2056、`update_sleep_backlight` 2767-2830、
`snapshot_machine_state` 2673-2752、`clock_sync` 2984-3041、`run_guest_frame` 3108-3207、`frame_budget_track` 100-115、
`draw_fps_overlay` 824-865、`persist_save_image` 2561-2590）；
`emu/unsp_glue.cpp:600-820`；`tools/aot.py:858-893`；`display.c`（grep sleep/SLPIN）；`menu.cpp:40-160`；`sdkconfig`（电源/时钟/串口）。
**历史口径对账**：`R7_app.md`（B6/D6）、`R7_aot.md`（§4.1/§4.2/D4）、`R7_periph.md`（§2.7）、`R7_core.md:141`、`R6B_aot.md`、`PERF_2026-11-22.md`。

**跑过的只读命令**：
- `riscv32-esp-elf-nm -S --radix=d build/digicore-c3.elf`：`execute_run=5142 B`、`spin_collapse=312 B`、`wait_loop_collapse=838 B` —— 与 R8_core §1 完全一致，**实测复核通过**。
- `riscv32-esp-elf-objdump -d -r …unsp_glue.cpp.obj`：16 MB 的 ELF 在本机 objdump 2.43.1 上退出码 1、零输出（工具/环境问题，未深究），改用同一 `build/` 的对象文件反汇编
  `.text._ZN11unsp_device18wait_loop_collapseEm.part.0`。**注意**：这是 TU 级代码，若最终链接另有跨 TU 内联，条数可能有 ±几条的漂移；但该函数是叶子函数，量级可信。
- `git status` / `git diff`（只读）：确认工作树里 `app_main.cpp` 有 **3 处未提交改动**（lapse toast 单次推送 +16 行、spin `1500→1000`、fps 叠加层加 `!s_standby && !menu_is_open()`）。

**行号漂移说明（重要，不据此罚分）**：两份报告的行号对应 HEAD `741a49e`，我逐条核对过，**在 HEAD 上是准确的**。
主审的两处改动使 `app_main.cpp` 出现了 +16 行（toast 块）与 +2 行（spin 注释）的位移，因此我下文一律用「审计时行号 → 当前行号」双写，并按代码内容定位。

**已落地的两条**（我在工作树里看到、报告仍按「建议」写）：
- F2：`app_main.cpp:3787` 现在是 `if (dt && !s_standby && !menu_is_open())`（HEAD 是 `if (dt)`）。
- F4：`app_main.cpp:3713-3715` 现在是 `if (wait > 1000) vTaskDelay(pdMS_TO_TICKS((wait - 1000) / 1000));`，注释里还写着「(R8 audit, F4)」。

---

## 2. 裁决表（10 条 P0/P1）

| ID | 位置（审计时 → 当前） | 优先级 | 裁决 | 一句理由 |
|---|---|---|---|---|
| F1 | `app_main.cpp:3565`+`3497` | P1（§0 误标 P0） | **downgrade** | 机理成立：待机没有早退、`run_guest_frame()` 在菜单 else 分支里照跑；但报告拿不出改动（「见 F3」指到了 crcwatch），26→9 mA / ×3 续航全是同类推测，且 §0 与表内优先级自相矛盾——只能作为 P1 观察项，且必须先把「停 guest 会停掉宠物成长」的风险写全。 |
| F2 | `app_main.cpp:3769`→`3787` | P0 | **downgrade** | 代码确实缺门控（主审已补），但叠加层画在 (2,2)、窗口在 (64,24)，**根本盖不到 STANDBY 卡**；报告自估 0.06 ms SPI/s≈0 能量，实质是卫生项，P0 名不副实。 |
| F3 | `app_main.cpp:3606-3655`→`3622-3671` | P0 | **accept** | 待机确实每 ~0.5 s 对 32 KiB framebuffer 算 CRC、每 ~5 s 打一条 INFO，全块无 `s_standby` 门控；收益小（0.05% core）但改动零风险，P0 站得住。 |
| F4 | `app_main.cpp:3694-3698`→`3712-3717` | P1 | **accept** | 1 ms tick 粒度把 spin 压到 1.0–1.99 ms 属实，省 ~0.5 ms/帧 = **3% wall clock ≈0.5 mA**（报告的「~1 mA」高估约 2×）；与 R7 D6「不动」不冲突（D6 反对删结构，F4 只挪边界）。已在工作树落地，等真机 `J 1` 验收。 |
| F5 | `sdkconfig:1042`/`1071-1073` | P1 | **downgrade** | 无 DFS 属实，但「仅待机开 DFS 省 8 mA」不成立：DFS 只在 idle 窗口切频，而待机仍有 ~14 ms/帧 guest 工作（报告自己 Q1 算的 84–90% 占空比），没有可切的窗口；要么强制 80 MHz（吞吐减半、改游戏时间），要么先降 guest 工作。报告 §5.3 也倾向 P2。 |
| F6 | `display.c:186`（全文无 SLPIN） | P1 | **accept** | 面板确实从不进 sleep，暗屏后照样扫描 GRAM；1–3 mA 标了推测、风险（120 ms 唤醒）写了、验证可做。补一句：SLPIN 保留 GRAM，所以「重推整窗 ~30 ms」不必需，真实代价只有 SLPOUT 120 ms + 重新 DISPON。 |
| F9 | `app_main.cpp:2690-2693`/`2707-2708` | P1 | **downgrade** | 机理真（CRC 覆盖整片 `s_soc.ram`，含时钟 `0x0CAD..0x0CAF`，每秒变 → 脏检测失效），但擦写账的前提写反了：`resumable` 的判据正是 `PC ∈ [0x200000,0x400000)`（`app_main.cpp:2707`），而 R8_core C6 说待机停在 `0x230321`（正在该区间）→ 待机**也**会写 fbsave 4 扇区。两份报告互相矛盾，磨损若按 C6 算就被低估一半。 |
| R8-C1 | `unsp_glue.cpp:655-708` | P0 | **accept** | 实打实：源码 11 读 8 写 = 19 次；反汇编每 pass **13 `lhu` + 12 `sh`**（~18 次落在 `m[0x600..0x607]`），注释「writes memory once per pass」是错的。hoist 保持逐 pass 等价：collapse 期间 CPU 由本线程持有、中断只在 `:782` 判一次，中间状态对外不可见。 |
| R8-C2 | `tools/aot.py:876` + `aot_blocks.c` | P1 | **downgrade** | 与 `R7_aot.md:542` 的 D4 逐字同一件事（277,946→~35 KB、243 KB、不损命中、风险=AOT_GROUP），本轮**零新证据**；且「少 miss→降占空比」不成立（1,048/1,097 个重叠块 census 零命中），Q7 的「重绘帧 20–40%」没有依据。 |
| R8-C6 | `unsp_glue.cpp:722-820` | P1 | **downgrade** | 只是 C1 的复述：无独立改动、无独立验证（判据就是 C1 的 guest_pct）；「LFSR 不能跳过」过强——循环体全是移位/XOR，GF(2) 跳步是精确的，它自己的 action 列也写了跳跃表，自相矛盾。 |

**计数：accept 4（F3、F4、F6、R8-C1）／downgrade 6（F1、F2、F5、F9、R8-C2、R8-C6）／reject 0。**
（F7/F8/F10/F11 是 P2/不动，R8-C3/C4/C5 是 P2，按任务范围不入裁决表。）

---

## 3. 特别核对

### 3.1 R8-C1「每 pass 19 次 DRAM 访问」——属实，且机器码证实注释是错的

**源码复核**（`unsp_glue.cpp:655-708`，逐语句计数）：

| 序号 | 语句 | 访问 |
|---|---|---|
| 657 | `a = (m[0x0603]<<16)` 或 `m[0x0602]` | 2 读 |
| 663 | `w0 = m[0x0600], w1 = m[0x0601]` | 2 读 |
| 664-665 | `m[0x0600]=m[0x0604]; m[0x0601]=m[0x0605]` | 2 读 + 2 写 |
| 666-667 | `m[0x0604]=m[0x0606]; m[0x0605]=m[0x0607]` | 2 读 + 2 写 |
| 668-669 | `m[0x0606]=w0; m[0x0607]=w1` | 2 写 |
| 697 | `m[0x0601]` | 1 读 |
| 701-702 | `m[0x0600]`、`m[0x0601]` | 2 读 |
| 706-707 | `m[0x0600]=r1; m[0x0601]=r3` | 2 写 |

合计 **11 读 + 8 写 = 19 次**，与报告一致。注释（`:631-632`）说「keeps the eight state words … in locals and writes memory once per pass」——**与实际 8 次写、11 次读不符**：真正被 hoist 的只有移位寄存器三件（`ylo/yhi/r4_rot`），8 个状态字一个没 hoist。所以 §5.2 问的「注释作废还是代码漏写」，答案是**第三种**：作者只 hoist 了一半，注释描述的是未完成的目标。

**机器码复核**（`wait_loop_collapse.part.0`，循环体 `.L12`@0xb4 → 回边@0x3dc）：

```
236 条指令/pass：slli 56, or 42, srli 39, and 19, seqz 16, lhu 13, sh 12,
                 xor 10, andi 9, addi 8, add 5, zext.b 3, sub 2, bne 1, xori 1
```

其中 13 次 `lhu` + 12 次 `sh` = **25 次访存**；按立即数 `-1024..-1010(a3)`（`a3 = ram + 0x1000`，即 `m[0x0600..0x0607]`）
外加 2 条经掩码地址的 `0(t0)/0(a7)`，落在状态字上的约 **18–20 次**，剩下 5–7 次是 `[sp]`/`[bp+0..2]` 栈暂存。
→ **GCC 没有替他把这 19 次消掉**，C1 的现象与机理都成立；报告「hoist 后循环内≈0 次 DRAM」的目标可达。

**逐 pass 等价性（能不能 hoist）**：可以，理由是**中间状态对外不可见**：

| 可见性通道 | 结论 | 证据 |
|---|---|---|
| 其它任务 | 无。全工程只有 `audio_task`（`audio.c:402`）与 `button_task`（`buttons.c:119`），都不碰 `s_soc.ram` | grep `xTaskCreate` |
| 中断/异常 | collapse 期间不服务；中断标志只在进入前判一次 | `unsp_glue.cpp:782` `done = (tick == 0 && !m_sirq) ? passes : 1` |
| 同一任务的重入 | 无。`hot_prng_n` 唯一调用点是 `:803`，`wait_loop_collapse` 由 `execute_run` 串行调用 | grep `hot_prng_n` |
| 别名 | **唯一风险**：`m[sp_in&0xfff]`、`m[sp0+1..3]`（`:651,711-713`）若落进 `0x0600..0x0607`，写回顺序才有意义 | 建议落地时加一句运行期断言 |

**收益口径要修正**：报告把「19 次访问 ≈38 cyc/pass」当成全部收益，但循环体是 236 条指令/pass，
DRAM 部分只占 ~16%。方向对、量级偏窄（见 §5 的 M1：真正的另一半是 16 位掩码脚手架）。

### 3.2 R8-C2 与 R7 的 D4 —— 同一件事，零新证据

`R7_aot.md:542` 原文：
> **D4** 把「重叠块」改成「入口桩」（`goto L_<pc>` 进主块）… 理论上 277,946 → ~35 KB，省 **243 KB**，且**不损失任何命中** … 需要块内标签 + 局部化形式一致（`AOT_GROUP` 的 `groupify` 已有现成实现，只是准入条件要放宽到「入口在任何块内」）

`R8_core.md:65` 的 R8-C2 与上述逐项对应（同样的 277,946→~35 KB、同样的 243 KB、同样的「不损命中」、同样的 AOT_GROUP 风险）。
本轮所谓「新证据」=「R6B (a) 块体 miss 60%」与「重叠占 61.4%」同源——这是**推断**，不是新测量；R7 的 D4 行里已经把两者并列过，且 R8_core 自己在 §Q5 承认「重绘帧的 census 目前缺失」。

更关键的是**机制不成立**：
- `R7_aot.md:353-355`：重叠块 1,097 个中，**929 个 force+census 零命中、120 个非 force+census 零命中**，合计 1,048 个从不执行。
- 从不执行的块**不会产生 I-cache/TLB 行 miss**，删掉或改成桩只省 flash 字节，**不省周期**。
- 因此 `R8_core.md:128` Q7 表里「R8-C2 … 可达 20–40%」与 §0.2 的「少 miss → 降占空比」都没有依据；R7 自己对 D1 的估算是 **+1~5% 帧**（负收益），D4 只承诺「不损命中」，从没承诺提速。

**裁决：降级到 P2**。保留「省 243 KB flash」这一句（flash 不紧，见 BRIEF §2，所以连这一句也不该排在 P1），删掉所有帧/占空比收益；
等「重绘帧 block census × 行数」拿到之后再谈 I-cache 常驻（`R8_core.md:111-112` 自己也这么写）。

### 3.3 F1「待机 guest 仍 60 Hz」的机理与游戏时间

- **执行位置属实**：帧循环没有任何 `s_standby` 早退；`update_buttons()` 在待机里直接 return（`2055`），
  但随后 `time_lapse_step(); run_guest_frame();`（`3496-3497`）无条件执行；`s_standby` 只挡 blit（`3565`）与 fps 叠加层（`3787`）。
- **待机确实在推进游戏时间**：guest 的 64 Hz tick 由 guest 自己的 ISR 抬（`wait_loop_collapse` 的 `tick` 语义，`:755-758`），
  时钟 `0x0CAD..0x0CAF` 随 guest 时间前进；主机只做「每 250 ms 与墙钟比对、偏差 ≥2 s 才写回」（`clock_sync`，`:3013-3034`）。
  → **「停 guest」会停止宠物成长/进化计数**，而 `clock_sync` 仍会把时钟拉回墙钟，所以**表盘时间不会错，错的是成长**。
  报告的风险栏只写「中（改游戏时间）」，§5.2 提到一点，但没把这条「时钟不错、成长停」的差异说清——这是本轮该补的一句话。
- **动作缺失**：F1 的「建议改动」写「待机路径降 guest 吞吐（见 F3）」，而 F3 是 crcwatch 门控，与吞吐无关；
  真正的动作在 F5/Q3（降帧+2×周期或降频），F1 本身没有可执行内容。
- **优先级自相矛盾**：§0 第 1 条标 **P0**，§2 表里 F1 标 **P1**。按 BRIEF §5 的定义（P1=需 A/B 或上机）应为 P1。

→ **downgrade**：保留机理与「背光之外唯一可切换大负载」的判断，删掉 P0 标签与 ×3 续航口号，补「成长停/时钟不错」，把动作指向 F5/Q3。

### 3.4 F4「spin 1.5→1.0 ms」与 R7 D6「不动」是否冲突

- R7_app D6（`R7_app.md:59`）：`≈1.5 ms/帧 ≈ 9% wall clock（58 fps）`，结论「**不动**（帧对齐靠它）」。
- R7 的「不动」反对的是**删掉/重构**这个 spin（改成 tickless 或 esp_timer，注释 `:3702-3705` 说实测更慢），
  不是说 1500 µs 这个边界值神圣。F4 保留结构、只把覆盖窗从 1.5 ms 收到 1.0 ms，并给出新论据（`CONFIG_FREERTOS_HZ=1000` 下 1 tick = 1 ms，
  `vTaskDelay` 向下取整 → 残余天然 ≥1000 µs）。**不与 D6 冲突，但要写明「这是 D6 的边界值修正，不是推翻」。**
- 收益修正：residual 从 [1500,2500) 变 [1000,2000)，**只省 0.5 ms/帧 = 3% wall clock**；
  按报告自己的电流表（活跃 20 mA / WFI 3 mA）折算 ≈ **0.5 mA**，不是 ~1 mA。
- 代价：余量归零后，调度抖动会让个别帧晚 ~1 ms 收尾；绝对 `s_deadline_us` + 落后者重同步（`:3696-3698`）保证不累积，`ft` p95 上抬是可接受风险——报告的风险栏写得对。
- **现状**：该改动已在工作树落地（`3713-3715` 已是 1000，注释还引用了 F4），所以本条的实际动作只剩真机验收。

---

## 4. 其余条目的复核要点

| ID | 复核结果 |
|---|---|
| F2 | 现象真（HEAD `3769`/`if (dt)`）；但 `draw_fps_overlay` 画在 `(2,2)`、24×12（`app_main.cpp:863`），而 STANDBY 卡是 `menu_standby_present(..., left_px()=64, top_px()=24, 192,192, ...)`（`:1999`）——**数字落在左上角黑墙/边框上，不在卡片上**。风险栏「极低」写得对，收益「0.06 ms SPI/s」也对，但由此得出 P0 不成立；且与 R7_app B6（同一函数、编译开关）部分重复。 |
| F3 | 现象真（`3627` `++s_crc_frames>=30`、`3640` `s_dbg%10`、全块无 `s_standby`）。风险栏漏一句：加门控后 `s_fb_crc`/`s_stall` 会在待机期间变陈旧，回屏第一帧必然判「变了」→ 只丢一次 kick，无害，写明白即可。 |
| F5 | 事实对（`sdkconfig:1042` 未开 PM、`1071-1073` 160 MHz）。但 DFS≠强制降频：`esp_pm` 只在 idle 进 hook 切频，而待机帧里 guest 占 ~14/16.7 ms（报告 Q1 自述 84–90%），可切窗口 <10% → 「省 ~8 mA」拿不到。**降级 P2/研究项**。 |
| F6 | 事实对（`display.c` 全文无 `esp_lcd_panel_disp_sleep`/SLPIN，只有 `:186` `disp_on_off(true)`）。补充：ST7789 SLPIN 后 GRAM 保持，SLPOUT+DISPON 即恢复，故唤醒代价是 120 ms 而非 150 ms；风险反而比报告写的更小。 |
| F9 | 机理对（`2690-2693` 整片 CRC；`0x0CAD..0x0CAF` 由 guest 每秒推进）；`fbsave` 单块 `0x0F000` 无磨损均衡也对。但擦写账的「待机 3 / 活跃 7 扇区」建立在「待机非 resumable」上，而 `2707` 的判据是 `PC ∈ ROM 区间`；R8_core C6 又说待机停在 `0x230321`。二者不能同真 → **降级，先把待机 PC 拼写定死**（RAM 副本 `0x000321` vs flash `0x230321`，见 `unsp_glue.cpp:730-736` 的注释）。 |
| R8-C6 | 现象真（collapse 在跑、`m_insn_count += 74*done`），但它是 C1 的「判据化」，没有独立改动/验证；「LFSR 不能跳过」与自己的「预计算跳 N 步多项式」互斥（GF(2) 跳步是精确等价的）。**降级并入 C1**。 |

---

## 5. missed：两份报告都漏掉的续航/性能项（5 条，均带文件:行号）

| # | 位置 | 漏了什么 | 量级 / 证据 | 性质 |
|---|---|---|---|---|
| M1 | `firmware/digicore-c3/main/emu/unsp_glue.cpp:653-708` | `hot_prng_n` 的 **16 位掩码/位切片脚手架**：循环体实测 236 条/pass，其中 **56 `slli` + 39 `srli` + 19 `and` + 16 `seqz` ≈ 130 条**是 `uint16_t` 局部量在模拟 rotate/位宽；R8-C1 只盯 19 次 DRAM（~38 cyc）。改用 32 位中间量、循环尾一次 `&0xFFFF` 写回，保守可压到 ~150 条/pass | 保守 −70 条/pass ≈ **C1 的 2 倍**（285,609 pass/s × 70 ≈ 20 M cyc/s ≈ 12% CPU，推测） | 性能+续航，两边都漏 |
| M2 | `app_main.cpp:3404`（`t_guest0`）、`:3525`（`frame_us`）、`:3672-3673`（`persist_save_image`/`maybe_snapshot`） | 存档的 ~300 ms 同步擦写发生在 **`frame_us` 取样窗之外**：`frame_us` 在 3525 就取完，persist/snapshot 在 3672 才跑 → `LONGF`/`guest_max`/`ov20` 都看不到这 300 ms，只有 `frame_time_note`（`:3399-3401`）的直方图能看到 | 每 60 s 一次 ~300 ms 不推屏（≈18 帧）；F9 的「300 ms/次」因此无法用 STAT 复核 | 性能/可观测性，两边都漏 |
| M3 | `app_main.cpp:1991-1994`（`standby_enter` 主动点亮背光）+ `:2812`（`dark = auto_off > 0 && …`）+ `menu.cpp:47` | **`AUTO OFF = NEVER`（`kAutoOffChoices` 最后一档 0）时，待机卡会以默认 80% 背光永久常亮**：`standby_enter` 里 `if (!s_lit) display_backlight_set(s_brightness_pct)`，而 `dark` 在 `auto_off==0` 时恒 false | 按报告自己 §4 的表：背光 24 mA，待机 26 → **~50 mA**，且永不进暗屏。F8 只算了「待机卡 3 s 降亮收益≈0」，没算这一档 | 续航，两边都漏 |
| M4 | `app_main.cpp:3496`（`time_lapse_step()` 无 `s_standby` 门控）+ `:3005-3011`（lapse 期间 `clock_sync` 改为跟随 guest）+ `:2724-2725`（存档把快时钟写进 NVS） | 用户开了 15×/60×/300× 时移后走开，**待机期间照样每帧注入 1 秒**（`time_lapse_step` 只判 `s_lapse_x>1` 与 `0x0CB0`），背光黑着把宠物快进几小时，下一次存档还会把这个快进后的时钟落盘 | 待机 300× 下 1 小时 ≈ 12.5 游戏天；F1/Q3 的待机讨论完全没提这条 | 续航/行为，两边都漏 |
| M5 | `emu/unsp_glue.cpp:766-788` | `wait_loop_collapse` **每次调用**都 `memcpy` 32 B `HotState` + 回写 8 个寄存器 + 重算 NZSC/PC；真机 `collcalls=19,154/s`（`R7_core.md:141`）≈ 319 次/帧。C1 只算循环体，两边都没算这个固定税 | ~40–55 条/调用 → ≈0.9 M cyc/s ≈ **0.5%**（小，但纯白给：collapse 期间 `core->m_r` 无人读，可省掉这份镜像） | 性能，量级小 |

> 未列入（已确认不成立，避免误导）：所谓「待机 guest 被自适应超频」——`frame_budget_track`（`:100-115`）的上限 `kCpuClock/60 = 1.6 M` 正好是**一整 guest 帧**，
> 实测 285,609 pass/s × 298 ≈ 85 M guest cyc/s ≈ 实机 96 MHz 的 89%，是**正常实时**而非超频，不能当浪费报。

---

## 6. 需要主审裁决的冲突（按影响排序）

1. **待机 PC 到底是 `0x000321`（RAM 副本）还是 `0x230321`（flash）** —— 直接决定 F9 的擦写次数（3 扇区 vs 7 扇区）与「每 60 s 磨一次 fbsave」。
   `unsp_glue.cpp:730-736` 的注释说游戏在本场景跑 RAM 副本；R8_core C6 说停在 `0x230321`。请用一次真机 `STAT` 或快照日志的 `snapshot pc=` 定案。
2. **F1 的产品语义**：待机停 guest → 表盘时间仍对（`clock_sync` 每帧在跑），但成长/进化停。这条不是技术选择，得产品拍板。
3. **F5 是否要开 `CONFIG_PM_ENABLE`**：开了也只是给 idle 窗口切频（<10%），拿不到 8 mA；真要待机省电只能降 guest 工作（M1 + F1/Q3），建议 F5 直接落 P2。
4. **C1 的落地形态**：hoist 六字 + 种子两字（报告写法）只吃掉 16% 的循环体；建议与 M1（32 位化）合并成一次改动、一次 A/B，别分两轮各改一遍同一个函数。
5. **F2 去留**：主审已加 `!s_standby`（并顺手加了 `!menu_is_open()`，比报告建议更严）。是否再叠 R7_app B6 的编译开关，属产品决定（它是 bench 时唯一不插串口的看帧率手段）。

---

## 7. 明确不动的清单（给主裁防误删）

| 对象 | 理由 |
|---|---|
| `app_main.cpp:3565` 的 `!s_standby` blit 门控 | 正确；待机不许推主窗口 |
| `wait_loop_collapse` / `spin_collapse` 的判定与预筛（`unsp_glue.cpp:722-820`、`377-454`） | 折叠是活路径（285,609 pass/s）；本轮只允许改 `hot_prng_n` 内部 |
| `app_main.cpp:3688-3718` 的「绝对 deadline + vTaskDelay + 短 spin」结构 | 1.5→1.0 是边界修正，结构本身别动（tickless/timer 实测更慢，注释 `:3702-3705`） |
| `display.c` 的 ST7789P3_CMDS 厂商序列 | 加 SLPIN 时只追加 0x10/0x11 与 DISPOFF/ON，不要重排既有序列 |
| `clock_sync` 的 250 ms 采样与 ±2 s 死区 | 待机/唤醒都靠它保表盘正确；改它会同时动 F1 的风险面 |
| 三条 save 路径的撕裂保护（`fbsave` 两遍流式读、`gueststate/saveimg` 只回读头部） | R7_periph §2.7 已核；F9 只许改「脏检测的比对范围」，不许删校验 |
| `menu.cpp:47/51/57` 的档位表 | M3 只是「把 NEVER 的代价写进文档/菜单提示」，不是删档位 |
| `frame_budget_track` 的 `hi = kCpuClock/60` | 它等于「一整 guest 帧」，是实时正确性的锚点，不是浪费 |

---

## 8. 本轮未做 / 做不到

- 未刷机、未开串口、未跑 `idf.py build`（BRIEF §7）：所有 `ft`/`guest_pct`/`aotmiss` 的「验收」都只是**可执行的方法**，不是实测值。
- 反汇编用的是 `build/…/unsp_glue.cpp.obj`（ELF 的 objdump 在本机失败），条数可能有 ±几条的链接期内联漂移。
- 全部电流数字沿用 R8_power §4 的推测表（无电流表）；本报告只做**相对比较**，不背书任何绝对毫安数。
- 未审 R8-C3/C4/C5 与 F7/F8/F10/F11（P2/不动，超出本次 P0/P1 范围）。

---

*本文件是本轮唯一写入。未改任何源码/配置/生成物，未跑构建、未刷机、未 git 操作。*
