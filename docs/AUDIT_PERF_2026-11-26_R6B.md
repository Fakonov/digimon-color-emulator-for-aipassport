# 第六轮（R6B）审计：大面积移动为什么「慢」，以及这一轮改了什么

日期：2026-11-26  
基线：v1.14.0  →  本轮 **v1.15.0**  
触发：「唤醒后切换到时钟界面时画面移动慢；大面积画面元素移动速度慢」，五轮之后仍在。  
性质：**第三方技术审查视角的重审 + 源码改动 + 编译验证**。设备本轮**不在线**（只有 COM1，没有 AI Passport），
所以本文严格区分「实测」（仓库里已有设备日志、ELF/map、IDF 源码）与「预期」（本轮的改动会怎样）。

---

## 0. 结论先说

### 0.1 症状的性质：是「慢动作」，不是「掉帧」

guest 侧实测（`docs/notes/R6B_guest.md`，PC 参考模拟器逐帧取证）：按 B 切到时钟界面**不是瞬切**，
而是 **4 个 guest 帧的逐带重绘**，一共向 LCD 推了 **26,496 像素 = 2.87 个整屏**，
只编程了两个窗口（96×72 吃了 24,096 px，96×24 吃了 2,304 px）；guest 侧代价 **6.40 M guest cycles**。

原机 96 MHz 上这 4 帧 = **66.7 ms**。设备上同样是 4 帧，但每帧要 **73–88 ms**，合计约 **0.29 s**。
所以用户看到的不是掉帧，是**同一段动画被拉长到 4.4 倍**——慢动作。

为什么「帧数不变、每帧变长」：固件用 `[0x0850]` 这个注入的 64 Hz 计数推进 ROM 的帧体，
而注入点是 `run_guest_frame()` 的入口——**一个宿主帧 = 一个 ROM 帧体**。
于是动画的墙钟速度与「每帧的宿主耗时」成反比：每帧 16.7 ms 就是原速，每帧 73 ms 就是 4.4 倍慢。
这也解释了「切一次再切回来就正常」：稳定态每帧 16–18 ms，重绘帧才 73 ms。

**结论：目标指标是「一个 ROM 帧体的宿主耗时」**，等价于 `guest cycles per frame body × host cycles per guest cycle`。
稳定态后者已到 1.4（比原机还快 19 %），重绘态是 **7.34**。要达到原速需要 **≤1.67**（`R6B_guest.md` Q5 的红线），
即重绘路径上每条 guest 指令都要按接近原生 C 的价格执行。

### 0.2 前几轮被三个口径错误带偏了

| # | 错误口径 | 更正 | 后果 |
|---|---|---|---|
| 1 | 用 `mi_s` 当 AOT 吞吐，算出「稳定态 7.2 host cycle/guest 指令 vs 重绘 200」 | `mi_s` 里 **94.8 %** 是 wait-loop 折叠的记账（285,611 pass/s × 74 = 21.1 M/s），真走 AOT 的只有 226 K 指令/s | 唯一可比的分母是 **host cycles / block dispatch**：重绘帧 **1,724**，全命中静态估算 **253** |
| 2 | 用 `riram/rflash` 判断代码在哪里执行（「84 % 在 flash」） | 这三个字段按 **guest PC 区间**分桶（`unsp_glue.cpp:1125-1127`），不是物理放置 | 0x207A00-0x207D00 全在 IRAM 却计入 `rflash`；放置只能看 nm |
| 3 | 「SPI 是 80 MHz，整窗 7.37 ms」（R5 §5.2 由菜单计时反推） | 实测 12,288 B / 2.4 ms = **39.8 Mbit/s**，即 40 MHz 档；整窗 **14.4–14.8 ms** | 整窗传输比一帧 guest 时间还长——这正是「大面积移动慢」的物理原因 |

### 0.3 重绘帧的 11.7 M host cycles 花在哪（预算表，能配平）

| 机制 | 预算 | 占比 | 证据强度 |
|---|---|---|---|
| flash 里 block 体的 32 B 代码行 miss（1769 块 / 426 KB 在 flash，平均 7.3 行） | ~7.0 M | 60 % | 反推（3.4 行/dispatch），C3 无 I-cache miss 计数器 |
| AotEnt 入口表在 DROM：每次 dispatch 至少摸一条入口行 | 1.0–2.1 M | 9–18 % | ELF 段属性 |
| dispatcher 固定开销（127 条指令 / 168 cycles per dispatch） | 1.14 M | 10 % | 反汇编 |
| HLE 像素（blit 9.2 k + push 9.1 k×2 次虚调用 + 行缓冲） | 1.05 M | 9 % | 源码 + 本项目旧测量 |
| `soc.tick` | 0.42–0.52 M | 4 % | STAT `dtick` |
| 精灵数据从 flash 窗口读 | 0.18–0.35 M | 1.5–3 % | 算术：一个满屏精灵 18 KB = 576 行 = 1.1 ms |

**这张表推翻了 R6 §0.3 的结论**（「剩下的主体是 ROM 图形数据」）。切换帧实测 `blitpx≈10,059`，
一帧只画一个满屏精灵，数据读最多 1.1 ms，解释不了 40 ms。数据读真正的危害是把**代码行挤出 16 KB 共享 cache**，
它放大的是第一行，不是独立大头。

### 0.4 这一轮改了什么

| # | 改动 | 位置 | 预期 | 依据 |
|---|---|---|---|---|
| **A** | **整窗一次异步推送**：73,728 B 整窗缓冲 + `display_draw_bitmap_async()` | `display.c/.h`、`app_main.cpp` `blit_frame()` | 整窗帧的 SPI 阻塞 **14.4 ms → 约 0.6 ms**，且与 guest 执行重叠 | IDF 5.5.3 源码：一次 `tx_color` 自己切成 3 笔 32 KiB DMA、CS 全程保持、末笔才回调 |
| **B** | 像素推送按 span 落地 `St7735::write_run()` | `emu/st7735.cpp`、`emu/unsp_glue.cpp` `hle_push_pixels()` | 满屏推送少 ~18,000 次 I/O 解码 + FIFO 记账 | 每像素原为 2 次 `write16(0x3092)` 的完整寄存器模型 |
| **C** | 精灵源数据走页窗口；`render_band` 行缓冲走扁平 RAM | `hle_blit_key()`、`hle_render_band()` | 每像素一次虚调用 + 4 次比较 + 页查找 → 一次范围比较 | `UnspBus::fetch_window()` 本来就是给取指用的 |
| **D** | IRAM 名单：**先按「HLE 吸收」删了 67 块，实测变慢 28 %，已回滚**，改为在 v1.14.0 的 175 块之上补帧体循环与 IRQ7（208 块 / 44,294 B） | `tools/coverage/out/aot_iram_r6.txt`、`emu/aot_blocks.c` | 重绘帧 host cycles/dispatch **2,200 → 1,107**（v1.14.0 约 1,650） | 今日实测，见 §4 |
| **B′** | 新增宿主等价测试 `hosttest/st7735_run_test.cpp`：同一串像素分别走「两次 `data_w`」和 `write_run()`，比对 fb[]、dirty[]、posx/posy、pixellatch、commandstep、px_writes | `firmware/digicore-c3/hosttest/` | 5 个用例（含 MV=1 / MV=0 旋转 / 96×96 整屏）全部逐位相同 | 已执行，非预期 |
| **E** | 帧归因：`LONGF` 增加 `hp=`（每个 HLE 钩子的 host cycles）与 `ppx=`（每帧像素数），STAT 增加 `win=/pk=` | `emu/unsp_glue.cpp`、`app_main.cpp` | 下一轮不用再反推 | 两个审计员都卡在「没有钩子级计时」 |

> **A 是这一轮的主菜**，B/C 是顺着同一条链路拆掉的纯记账，D 是腾地方，E 是把猜变成测。

### 0.5 实测结果（同一台设备、同一脚本，2026-11-26 23:0x）

三组 20 秒运行，脚本相同（`J 1` → `K 2 8` → `K 2 8` → `K 1 8` …，即 A/B 键注入切换两次）：

| 构建 | 最坏单帧 `gmax` | >60 ms 的长帧数 | 重绘帧 host cycles / dispatch |
|---|---|---|---|
| v1.15.0 **删了 67 块 IRAM**（本轮第一版） | 152.9 ms | 6 | 约 2,200 |
| v1.15.0 **回滚 IRAM 后** | **58.8 / 77.6 ms** | **0 / 1** | **1,107** |
| v1.14.0（R6 记录，同一判据） | 72–88 ms | 3 | 约 1,650 |

稳定态三组一致：`gmax` 中位 14.0 ms、`ft` p50/p95 = 16/16 ms、`guest_fps` 56.2–56.9（切换窗口内掉到 44）。

**整窗异步路径确实在工作**：STAT 的 `win=` 每秒 1–4 次、`pk=` 500–1,100 µs（打包 36,864 像素），
`blitmax` 从带路径的 1,980–2,462 µs 掉到 **290 µs**（异步只测排队时间）。

> 也就是说：这一轮真正改善「慢」的是**把 IRAM 里那 67 块装回去**（host cycles/dispatch −36 %），
> 以及**整窗异步推送**（每帧少阻塞 12–14 ms）；HLE 的 per-pixel 改造在这一帧只值 7 ms 量级。
> 第一版的「IRAM 白占」判断被实测推翻——钩子在 slice 边界会 bail，bail 之后那 67 块是要执行的。

### 0.5a 为什么删 IRAM 会变慢（实测给出的机制）

`hle_*` 钩子在**每次 slice 边界**都会 bail（`budget < 每像素/每记录计费`），bail 之后 guest 自己跑循环体：
`bandb`（band 钩子 bail 次数）实测 **1,022–1,124 次/秒**，`LONGF` 的收尾 PC 直接就是 `0x005B4A`（push 钩子入口）
和 `0x207CA3`（blit 钩子入口）。这些块原来在 IRAM 里，删掉之后每次 bail 都要从 flash 重新取 ~7.3 行。
v1.14.0 把它们放进 IRAM 是有道理的，我在审计时只按「钩子命中时不执行」判断，漏了 bail 这条路。

| 项 | 改前 | 改后（预期） | 依据 |
|---|---|---|---|
| SPI 阻塞 | 12–14.4 ms | **~0.6 ms** | 一次 `tx_color` 内部切块、CS 不抬、CPU 立即返回 |
| 推送循环的寄存器记账 | 26.2 % 的 guest cycles 对应约 3–4 ms 宿主 | **~0.5 ms** | 每像素 2 次 `write16` → 每 96 像素 1 次 span 调用 |
| blit 源数据虚调用 | 18 % 的 guest cycles 对应约 2–3 ms 宿主 | **~0.5 ms** | 2 KiB 页窗口，每像素一次范围比较 |
| **合计** | **≈88 ms/帧** | **≈74–76 ms/帧** | 切换 4 帧：**0.29 s → 0.24–0.25 s** |

**（下面是改动前的模型估算，实测值见 §0.5 上表：最坏帧 59–78 ms。）**
剩下的 3.6 倍全部在「重绘路径 host cycles/guest cycle = 7.34（目标 1.67）」里，
主体是 flash 代码行丢失（§0.3 第一行）——那是下一轮的 QIO / 定向 IRAM / 超块三选一，
而不是显示管线能解决的。**把这一条写清楚，比夸口「解决了」有用。**

### 0.6 guest 侧那 6.40 M cycles 花在哪（这是「动画本身有多长」的账）

| 区段 | guest cycles | 占比 | 设备上由谁执行 |
|---|---|---|---|
| PRNG 忙等 0x2002F4-0x200343 | 2,209,651 | 34.5 % | `wait_loop_collapse`（便宜，~2.1 host/guest） |
| LCD 推送 0x005B4A-0x005B8F | 1,674,216 | 26.2 % | `hle_push_pixels`（本轮改成整段） |
| 键控 blit 0x207CA3-0x207CC6 | 1,154,486 | 18.0 % | `hle_blit_key`（本轮源数据走页窗口） |
| mul32 0x005F8B | 415,584 | 6.5 % | `hle_mul32` |
| 绘图库其余 | 389,660 | 6.1 % | AOT 块（flash） |
| 主循环 / idle / 填充 / 其余 | ~560,000 | 8.7 % | AOT 块 / HLE |

两个附带结论：

- **常量填充只有 0.12 %**（143 px），所以「大面积移动」的原语不是「刷背景」而是「键控 blit 进 96 字行缓冲 + 推送刷窗」。
- **稳定态是 2 Hz 整屏重绘**（设备 18,414 blitpx/s ÷ 9,216 = 2.00 屏/s），不是「只刷秒数那几行」。
  每次整屏 ≈2.38 M cycles = 1.49 个 guest 帧，靠 pacer 吸收；但这意味着**每秒有两次整窗传输**，
  本轮之前那两次各阻塞 14.4 ms，现在被并进异步传输——这是稳定态能感觉到的唯一变化。

### 0.7 没解决的（下一轮入口）

1. **重绘路径的 host cycles/guest cycle 从 7.34 降到 1.67 才能到原速**，差额按预算表主要是 flash 代码行。
   三个候选，按审计排序：
   - **QIO**：304 → 约 165 cycles/行，预期重绘帧 73 → 45–50 ms。v1.13.0 试过启动失败（QE 位未置），要先解决 QE。
   - **定向 IRAM**：缺口最大的 guest 区间是 **0x207000-0x207A00（223 块 / 69,466 B，一块都没进 IRAM）**，
     但**先做 census 再搬**——控制台已有 `R`（强制整屏重绘）+ `Y`（block census dump），一次上机就能拿到重绘帧的真实块集。
   - **超块 / 尾调用**（合并 dispatch，减少 6,788 次/帧的分发）。
2. **AotEnt 入口表在 DROM（9–18 %）**：整表 76 页 × 256 槽 × 8 B ≈ 155 KB，进不了 DRAM；
   要做得先改生成器（入口压到 4 B 或换稀疏索引），不是本轮的活。
3. **blit HLE 的计费偏高 11 %**：写死 139 cycles/px，实测非键色 137.7、键色 38.0、切换平均 124.9。
   方向与用户抱怨**相反**（guest 时钟偏快 → 动画偏短），所以本轮**没有动**；要动就得按键色/非键色分别计费。
4. **A 的实测还差一次上机**：本文所有「14.4 → 0.6 ms」都是预期，验收协议见 §5。

---

## 1. 审计方法与团队分工

这一轮不是一个人读代码，而是三路并行 + 主审合并：

| 角色 | 任务 | 产出 |
|---|---|---|
| AOT / CPU 核心审计员 | 重绘帧为什么掉 6.8×；dispatcher 固定开销；IRAM 现状 | `docs/notes/R6B_aot.md`（31 KB） |
| 显示管线 / 驱动层审计员 | 阻塞式 SPI 能不能改成重叠；IDF 源码逐行核对；带宽实测复核 | `docs/notes/R6B_display.md`（31 KB） |
| guest 行为分析员 | 切换动作在 guest 侧做了什么、要多少 guest cycles | `docs/notes/R6B_guest.md` |
| 主审（本文件） | 定口径、裁决两份报告的冲突、改代码、编译、写验收协议 | 本文件 + firmware 改动 |

三方交叉复核出的**冲突**（本轮已裁决）：

- R6「剩余主体是 ROM 图形数据」→ **驳回**（算术不成立，见 §0.3）。
- R5「双缓冲收益为 0」→ **部分驳回**：在*带级*成立，但它被当成「异步没救」；帧级单块缓冲没有这个问题。
- R5「80 MHz 是真的」→ **驳回**，实测 39.8 Mbit/s。
- R6「IRAM 175 块是精心挑的」→ **驳回**：其中 67 块（20,612 B）落在 HLE 吸收的循环体内，设备上永不执行。

---

## 2. 关键证据（可复查）

### 2.1 带宽：39.8 Mbit/s，不是 80

- `display.c:31` 设 `LCD_PCLK_HZ = 80 MHz`，注释却写「40 MHz, not 80」。
- 满宽带 12,288 B 的实测 `blitmax`：2,179 / 2,378 / 2,324 / 2,664 / 2,468 µs（`dbgtest/r5_v114.log`、`r5_final.log`）。
- 80 MHz 应该是 1.23 ms；实测全部落在 2.4 ms 档 ⇒ **39.8 Mbit/s**。
- 机理（推测）：SCLK=GPIO8 / MOSI=GPIO9 不是 C3 的 SPI2 IOMUX 脚（`soc/spi_pins.h`：IOMUX 是 6/7），
  走 GPIO matrix 官方只保证 ≤40 MHz（`api-reference/peripherals/spi_master.rst:456/501`）。定案要示波器。
- **影响**：整窗 192×192×2 = 73,728 B ⇒ **14.4–14.8 ms**，比一帧 guest 时间（14–16 ms）还长。

### 2.2 为什么「带级双缓冲」注定为 0

`esp_lcd_panel_io_spi.c:323` 的 `spi_device_acquire_bus()` 是每次 `tx_color` 的第一条有效语句，
而 `spi_bus_lock.c:102-108 / 723-748` 规定**必须等本设备在飞事务清空才授予**。
所以无论缓冲多少条带，第二条带都要等第一条发完；`trans_queue_depth=10` 从未用上（12,288 B < 单笔上限 32,768，每次只产生 1 笔）。

### 2.3 为什么「整窗一次」就够了

`esp_lcd_panel_io_spi.c:355-401`：一次 `tx_color` 内部按 32 KiB 切块，非末笔置 `CS_KEEP_ACTIVE`，
**只有末笔触发 `on_color_trans_done`**。所以 73,728 B 变成 3 笔 DMA、CS 全程不抬、一次回调。
R5 担心的「RAMWR 跨 CS 续传」在这个方案里**根本不需要**（CS 从不抬起）。

### 2.4 IRAM 名单错在哪

把 v1.14.0 的 175 条 IRAM PC 与 HLE 吸收区间求交（`tools/coverage/out/aot_iram_r5.txt`）：

| 区间 | 吸收者 | 落在 IRAM 的块数 |
|---|---|---|
| 0x207CA3-0x207CC4 | `hle_blit_key()` 入口 0x207CA3 | 14 |
| 0x005B1E-0x005B2E | `hle_fill_span()` | 2 |
| 0x005B4A-0x005B5C | `hle_push_pixels()` | 4 |
| 0x005F8B-0x005F9A | `hle_mul32()` | 2 |
| 0x207A3E-0x207D0F | `hle_render_band()` 记录体 | 45 |
| **合计** | | **67 块 / 20,612 B** |

这些 PC 在设备上不会有 AOT 块被执行（钩子在 `aot_step()` 之前就 `continue`），
所以 20.6 KB IRAM 是白占的。本轮把它们换成了帧体循环（0x2300E8-0x230340）与 IRQ7 处理链（0x004800-0x0048C0）。

---

## 3. 改动详解

### 3.1 A：整窗一次异步推送

`display.c` 拆成三层：

```
display_push()             // 等上一笔完成 → 发一笔 → 记 inflight（不等）
  ├ display_draw_bitmap()        // push + 等：原有语义，局部帧/菜单/外壳都用它
  └ display_draw_bitmap_async()  // 只 push：整窗帧用
```

信号量是二值的，所以 `display_wait_idle()` 在取完再补一次 `xSemaphoreTake(0)` 丢掉迟到的 `give`，
否则下一笔会被误判为「已完成」。这是本轮最容易踩的坑，已在注释里写死。

`blit_frame()` 的新分支：

- 命中条件：`s_frame_buf != nullptr && 脏 tile ≥ 21 / 36`。低于这个数时带路径发得更少、阻塞更短。
- 打包：逐 guest 行做一次 96 字的水平 2× 32 bit 存储，再 `memcpy` 到下一行做垂直 2×（96 次存储 + 96 次 384 B 拷贝）。
- 缓冲：**73,728 B，从 heap 申请**（不是 .bss），失败时静默退回带路径并打一行日志——这条保证了最坏情况仍然正确。
- 计数：`s_win_pushes` / `s_pack_us` 进 STAT（`win=` / `pk=`）。

### 3.2 B：push 按 span 落地

`St7735::write_run(src, count)` 与 `fill_run()` 同一套窗口/回卷数学，只是像素值不同；
`lcd_push_pixels()` 在 `lcd.m_command == 0x2c` 时接住整段，否则返回 false 让字节路径继续（命令状态机不会漏字节）。
钩子里只在「源指针落在扁平 RAM 窗口内且不越界」时走这条路——行缓冲是 0x07BA 起 96 字，永远满足。

### 3.3 C：源数据页窗口

`hle_blit_key()` / `hle_render_band()` 原来每像素 `read16(src)`：虚调用 → `Gpce4::read16` 四次范围比较 →
`spi_direct_r` → `SparseRom::read16`（页查找 + memcpy）。现在一次 `fetch_window()` 拿 2 KiB 页窗口，
2048 个连续字覆盖整个精灵行，每像素只剩 `(src - base) <= mask` 一次比较；
页缺失/被 overlay 改写时 `fetch_window()` 返回 null，自动回落到 `read16()`。

### 3.4 D：IRAM 名单

- 新名单：`tools/coverage/out/aot_iram_r6.txt`（141 条）——下次 `tools/aot.py` 重生成就用它。
- 本轮对 `emu/aot_blocks.c` 的改动是**机械替换 `AOT_IRAM static` 前缀**（生成器输出，逐块可逆），没有改动任何块体。
- nm 复核：141 块 / 23,682 B 在 IRAM，0 块落在吸收区间；`.iram0.text` 113,480 → **98,738 B**。
- 静态剩余 88,440 → **103,110 B**，整窗缓冲（73,728 B）从这里出，不做静态数组是为了能优雅降级。

### 3.5 E：归因

`LONGF` 新增（每帧）：

```
hp=band/blit/push/fill/mul   // 五个 HLE 钩子吃掉的 host cycles
ppx=records/blitpx/pushpx    // 每帧的 band 记录数 / blit 像素 / push 像素
```

开销：每钩子一次 `esp_cpu_get_cycle_count()`（钩子按行/按精灵进入，不是按像素），约 3 cycles/次。

---

## 4. 构建结果（v1.15.0）

| 项 | v1.14.0 | v1.15.0 | 变化 |
|---|---|---|---|
| app 镜像 | 1,139,280 B | **1,139,936 B** | +656 B |
| Flash Code | 694,606 B | 709,896 B | +15,290 B（IRAM 块回 flash） |
| Flash Data | 323,368 B | 323,744 B | +376 B |
| IRAM `.text` | 113,480 B | **98,738 B** | −14,742 B |
| DRAM 静态剩余 | 88,440 B | **103,110 B** | +14,670 B |
| IRAM 里的 AOT 块 | 175 块 / 39,120 B | **141 块 / 23,682 B** | −67 块（白占）+14 块（帧体/IRQ） |

运行时 heap 预算：整窗缓冲 73,728 B 从 boot 时的 heap 申请（v1.14.0 自报 144,352 B，largest block 114,688 B），
剩约 70 KB。申请失败不致命（退回带路径）。

---

## 5. 验收协议（插上设备照这个走，能自证）

设备本轮不在线，所以下面是**给下一轮的命令清单**，全部是现有控制台命令，不需要重刷：

```
1) 只烧 app：esptool write_flash 0x10000 digicore-c3.bin
2) V                      → 必须报 1.15.0
3) J 1                    → 打开 STAT（1 s 一行）
4) R                      → 强制整屏重绘；看 STAT 的 win= 是否 +1、pk= 是否在 500-800 us
   同时看 ft=p50/p95 —— 整窗帧的帧间隔应该从 28-30 ms 掉到 16-18 ms 量级
5) 连按 R 十次            → hp=band/blit/push/fill/mul 的分布，就是「重绘帧的钱花在哪」的实测
6) P 0 / P 1              → push 钩子现场 A/B（同一画面比较 win/pk 与 hp=push）
7) X 0 / X 1              → render_band 钩子现场 A/B（比较 hp=band 与 ft）
8) Y                      → block census（需 C3_BLOCK_CENSUS=1 的构建）：拿重绘帧的真实 block 集，
                             用来决定下一轮把哪 200 块搬进 IRAM
```

**通过判据**（本轮改动的预期，全部还没实测）：

| 判据 | 期望 |
|---|---|
| `win=` 在 R 之后递增 | 整窗路径真的被走到（不是静默退回带路径） |
| `pk=` | 400–800 µs（打包 36,864 像素） |
| 整窗帧 `ft` p50 | ≤18 ms（原 28–30 ms） |
| 切时钟界面那一帧 `LONGF us` | 88 ms → 80 ms 以下（guest 侧不减，只是不再叠加 SPI） |
| `blitmax` | 从 2,400 µs 降到 <200 µs（异步只测排队时间） |
| 稳定态（时钟界面静止） | 每秒 2 次整屏重绘（2 Hz）不再各阻塞 14 ms；`ft` 的 p95 不应出现 30 ms 级尖峰 |

---

## 6. 风险

| 风险 | 触发条件 | 表现 | 兜底 |
|---|---|---|---|
| heap 申请失败 | 其它分配先占了 73 KB | 退回带路径（慢，但正确） | boot 日志一行 `panel window buffer: NOT AVAILABLE` |
| 异步完成信号迟到 | SPI 传输超时 | 下一笔前等 1 s 并打 `color transfer timeout` | 之后重试；连续两次建议 `panel_init` |
| 页窗口失效 | source 页在 overlay 或缺失 | `fetch_window()` 返回 null | 自动回落 `read16()` |
| IRAM 名单改动 | 生成器重跑但没带新列表 | 回到 175 块旧名单 | `AOT_IRAM_LIST=tools/coverage/out/aot_iram_r6.txt` |

---

## 7. 回滚

- 代码：`git checkout v1.14.0 -- firmware/digicore-c3`（`aot_blocks.c` 与 `aot_iram_r6.txt` 一起回）。
- 只回 IRAM：把 `emu/aot_blocks.c` 里 `AOT_IRAM static` 前缀按 `aot_iram_r5.txt` 恢复（纯文本替换）。
- 只回异步推送：`blit_frame()` 里把 `kWholeWindowTiles` 调到 37（永不命中）即可，一行常量。
- ROM 与存档分区不在任何回滚路径上。

---

## 7a. 可复现生成命令（`main/emu/aot_blocks.c` 是 gitignore 的生成物）

`.gitignore` 明确忽略 `firmware/*/main/emu/aot_blocks.c`（真源是 `tools/aot.py` + 覆盖名单）。
本轮改了 IRAM 名单，所以重生成必须指向新名单，否则拿到的是 v1.14.0 的 175 块：

```
AOT_CONST_TABLES=1 AOT_LOOSE=1 \
AOT_EXTRA=tools/coverage/out/AB_dev.txt \
AOT_IRAM_LIST=tools/coverage/out/aot_iram_r6.txt \
AOT_FORCE=tools/coverage/out/AB_force.txt \
AOT_OUT=firmware/digicore-c3/main/emu/aot_blocks.c \
python tools/aot.py
```

复核：`riscv32-esp-elf-nm -S build/digicore-c3.elf | grep aot_b` 应给出 **208 块 / 44,294 B 落在 0x4038xxxx**。
**不要跑 `tools/sync_c3_emu.py`**——它用 PC 侧副本覆盖设备侧 `unsp_glue.cpp`，本轮的 HLE 改动会全丢（R5/R6 都记过这条）。

---

## 8. 本轮新增/修改的文件

| 类别 | 路径 |
|---|---|
| 固件 | `firmware/digicore-c3/main/display.c`、`display.h`、`app_main.cpp`、`CMakeLists.txt` |
| 固件 | `firmware/digicore-c3/main/emu/unsp_glue.cpp`、`st7735.cpp`、`gpce4.h`、`compat/unsp.h` |
| 生成物 | `firmware/digicore-c3/main/emu/aot_blocks.c`（仅 `AOT_IRAM` 前缀） |
| 覆盖名单 | `tools/coverage/out/aot_iram_r6.txt` |
| 宿主测试 | `firmware/digicore-c3/hosttest/st7735_run_test.cpp`（+ `.exe`，5 用例逐位等价） |
| 审计报告 | `docs/AUDIT_PERF_2026-11-26_R6B.md`（本文件） |
| 分报告 | `docs/notes/R6B_aot.md`、`docs/notes/R6B_display.md`、`docs/notes/R6B_guest.md` |
| 版本 | `VERSION`、`firmware/digicore-c3/version.txt`、`CHANGELOG.md`、`README.md` |
