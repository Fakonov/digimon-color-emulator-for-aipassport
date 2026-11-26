# R6B：AOT / CPU 核心热路径审计（重绘帧为什么慢）

日期：2026-11-26  
基线：v1.14.0（app 1,139,280 B，build/digicore-c3.elf 时间戳 2026-11-26 19:06:46，本轮 nm/map/size 全部取自该 ELF）  
角色：AOT / CPU 核心热路径审计员（只读分析，未改动任何 .c/.cpp/.h/.py）  
证据来源：build 目录 ELF 的 nm/objdump/size 实测、digicore-c3 源码与编译开关、dbgtest/r5_*.log 设备日志、
docs/AUDIT_SWITCH_2026-11-26_R6.md、docs/AUDIT_PERF_2026-11-26_R5.md、docs/RESEARCH_EXT_C3_FETCH_PATH_2026-11-26.md。

---

## 0. 结论先说

1. **「28 倍」是口径造成的，不是 per-dispatch 成本变了。** 重绘帧 73 ms 的分母不该是「58,477 条 guest 指令」，
   而该是「6,788 次 block dispatch」。两者放到同一分母上：重绘帧每次 dispatch 约 **1,724 host cycles**。
   而时钟稳定帧的 `mi_s=22.4` 里有 **94.8 % 是 wait-loop 折叠的记账指令**（285,611 pass/s × 74 = 21.1 M，
   占 22.25 M），真正走 AOT 的只有 226 K 指令/s（5.2 %）。所以「稳定态 7.2 host cycle/guest 指令」不是 AOT
   的成绩，是折叠循环的成绩；拿它和重绘帧的 200 比，等于拿「几乎不干活」和「真干活」比。
   **本轮该继续动的指标是 host cycles/block dispatch，不是 host cycle/guest 指令。**
2. **11.7 M host cycles 的预算能配平，60 % 落在同一个机制上：flash 里 block 体的 32 B 代码行 miss。**
   1944 个块里 **1769 个（413,020 B）在 flash**，平均 233 B/块 ≈ 7.3 行；DIO@80 MHz 一行 ≈ 304 CPU cycles，
   反推重绘帧平均 **3.4 行 miss/dispatch**（47 % miss 率）= 7.0 M cycles。只有这条链路有足够容量吃掉差额。
3. **R6 §0.3「剩下的主体是 ROM 图形数据」在算术上不成立（对切换帧）。** 一个 96×96 精灵 18 KB = 576 行
   = 175 K cycles = 1.1 ms；切换窗口实测 `blitpx=10,059`，即一帧只画一个满屏精灵，数据读在 73 ms 里只占
   **1.5–3 %**。这条假设最多解释 2 ms，解释不了 40 ms。数据读真正的危害是把**代码行挤出 16 KB 共享 cache**
   （放大第 2 条），它本身不是独立大头。
4. **`riram/rflash/rdraw` 不能用来判断代码放哪。** 三个字段按 **guest PC 区间**分桶
   （unsp_glue.cpp:1125-1127），不是按物理放置。0x207A00-0x207D00 全在 IRAM，但计入 rflash。
   拿 rflash 当「84 % 在 flash 执行」的证据是错的。物理放置只能看 nm：175 块 / 39,120 B 在 IRAM，
   1769 块 / 413,020 B 在 flash。
5. **dispatcher 本身在 IRAM（Q2(d) 不成立），但 AOT 入口表 AotEnt 在 flash（DROM 0x3C0C6xxx）。**
   这是问题清单没列、但排第二的单项：每次 dispatch 至少摸 1 条 32 B 入口行（一行覆盖 4 个 slot），
   页缓存 miss 时还要再走一遍 71 项、18 行的 DROM 二分：预算 1.0–2.1 M（9–18 %）。
6. **dispatcher 结构上确实浪费约一半指令（127 条里 ~50 条可去），但收益只有 ~4.2 %。**
   在 1,724 cycles/dispatch 的分母下，HLE 比较链合并（24→2 条）、折叠/自旋比较合并（15→4）、
   三个 64-bit 计数器移出热路径（16→2），合计 ~72 cycles/dispatch。**这一版不值得为它单独开**，
   先把 flash 行打掉。
7. **降低帧预算 / 拆帧治不了「慢动作」。** 动画墙钟时长 = 需要的 guest cycles ÷ 每秒 guest cycles，
   后者由宿主吞吐决定，与每帧切多少无关。重绘段宿主真实吞吐 = **1.6 M guest cycles / 73 ms ≈ 22 M
   guest cycles/s（原机 96 MHz 的 23 %）**；90 M 那个数字是 wait-loop 折叠的产物。

---

## 1. 三个测量陷阱（先校正口径，否则后面每条结论都是错的）

### 1.1 mi_s 里 95 % 是折叠记账，不是 AOT

wait_loop_collapse() 每折叠一圈就 `m_insn_count += 74ull * done`（unsp_glue.cpp:787），
execute_run 的 fast_hot 路径也 `m_insn_count += 68`（unsp_glue.cpp:1802）。实测差值
（dbgtest/r5_diag_switch.log:129→130 两次 STAT 相减）：

    frames 1068→1125 (+57)   aotb 1,406,149→1,484,826 (+78,677)   aoti 4,314,898→4,541,249 (+226,351)
    collapse=285,611 passes/s   mi_s=22.29

- 折叠记账：285,611 × 74 = **21.13 M 指令/s**
- AOT 块内指令：**0.226 M 指令/s**
- 合计 21.36 M ≈ 97.9 % 是记账；STAT 的 22.29 M 里 94.8 % 来自折叠。

**推论：mi_s 是「折叠循环转了多少圈」的代理，不是 AOT 吞吐。** 用 `160 MHz / mi_s` 得到的
「host cycle/guest 指令」只在两帧折叠比例相同时才可比。上一轮据此得出的「稳定态 7.2 vs 重绘 200」
不代表 dispatch 路径退化。

### 1.2 riram/rflash/rdraw 是 guest PC 分桶，不是物理放置

    if (lpc < 0x001000u)      g_ins_ram   += cnt;      // unsp_glue.cpp:1125
    else if (lpc < 0x010000u) g_ins_irom  += cnt;      // :1126
    else                      g_ins_flash += cnt;      // :1127

切换秒内 rdraw=88,320 统计的是 guest PC 落在 0x207A00-0x207D00 的指令，而这 70 个块 **全部在 IRAM**（见 §4）。
所以 rflash 高不能推出「代码在 flash 执行」；反过来「搬进 IRAM 后 rflash 应下降」也是错的期待。

### 1.3 dtick 与 dins 都不是 CPU 时间

- dtick（µs）是 soc.tick() 的时间，切换帧 2,644–3,721 µs（r5_blkcensus.log:100/106/114），占 73 ms 的 3.6–5.1 %。
  不是主因，但必须从预算里扣掉。
- dins 不含 HLE 电荷：hle_blit_key 每像素电 139 guest cycles、hle_push_pixels 每像素 63、hle_fill_span 每像素 54，
  都不加 m_insn_count。一帧 9,216 像素的 blit 就电 1.28 M guest cycles（= 该帧 1.6 M 预算的 80 %）。
  **「dins 少」不等于「活少」**：重绘帧是「极少指令 + 极多像素电荷」，这是 dins/dblk=8.6 看着正常、
  而帧预算已经烧完的原因。

---
## 2. Q1：一次 aot_step dispatch 的固定开销

源码：execute_run 热路径 unsp_glue.cpp:1749-1865（无 hook 分支），aot_step unsp_glue.cpp:1039-1137。
以下指令条数是 **objdump 实测**（-Os 只作用于 aot_blocks.c，这两个函数是 -O3）。

### 2.1 热循环实际走的路径（反汇编）

    loop_tail   0x403833EA  lw a3,156(s10)      ; m_icount
                0x403833EE  bltz a3, exit
                0x403833F2  lw a4,4(s11)        ; m_core
                0x403833F6  j 0x40383260
    head        0x40383260  lw s7,24(a4)        ; m_r[6] (SR)
                0x40383264  lw a2,28(a4)        ; m_r[7] (PC)
                0x40383266  lw a4,232(s11)      ; m_block_hist
                ...                        slli/and/or 组装 lpc
                0x40383274  beqz a4,+           ; 判空
                0x40383286  lbu a4,229(s11)     ; m_fasthot
                0x40383294  beq s7,0x2002F4
                0x4038329C  beq s7,0x0002F4
                0x403832A4  jal wait_loop_collapse    <- 真调用，未内联
                0x403832AC  lbu a4,228(s11)     ; m_fastspin
                0x403832B4/BC/C8  beq s7,0x5251/0x5376/0x5F35
                0x403832D0..0x4038331A  5 个 HLE 钩子比较链（每条 = lbu 标志 + lui/addi + beq）
                0x40383328  jal aot_step         <- 真调用
                0x4038374A  lw a4,100(s10)      ; m_sirq，非零才进 check_irqs（已内联）

check_irqs 已被内联进 execute_run（多处 jalr -> trigger_irq），但 wait_loop_collapse（0x40382AEC，
280 条指令 / 838 B）和 aot_step（0x40382E32，132 条 / 418 B）都是真调用。

### 2.2 逐条预算

| 段 | 指令 | cycles | 反汇编依据 |
|---|---|---|---|
| lpc 组装（lw,lw,slli,and,or） | 5 | 6 | 0x40383260-0x40383274 |
| m_block_hist 判空 | 2 | 3 | 0x40383266 / 0x40383274 |
| m_fasthot + 两个 PC 比较 | 7 | 8 | 0x40383286-0x4038329C |
| wait_loop_collapse 调用 + callee 头（m_fastwait、lpc!=0x230321） | 4+8 | 16 | 0x403832A0-0x403832A8、0x40382AEC |
| m_fastspin + 三个 PC 比较 | 11 | 13 | 0x403832AC-0x403832CC |
| 5 个 HLE 钩子比较链 | 24 | 27 | 0x403832D0-0x4038331A |
| m_aot + aot_step 调用 + 结果判定 | 6 | 9 | 0x4038331E-0x4038332C |
| m_sirq 判定 | 2 | 3 | 0x4038374A-0x4038374E |
| 循环尾（icount、重取 m_core、j） | 4 | 6 | 0x403833EA-0x403833F6 |
| **execute_run 小计** | **65** | **≈91** | |
| aot_step 序言（g_aot_ctx_ready + 存 s0/ra） | 7 | 9 | 0x40382E32-0x40382E42 |
| 页缓存探测 + 入口项解引用 | 18 | 22 | 0x40382E92-0x40382F3C |
| 双 64-bit 计数器 + ctx 写回（sb/icount） | 19 | 24 | 0x40382F42-0x40382F7E |
| 间接调用 + 收尾（aot_blocks 64-bit + 写回 m_core） | 18 | 22 | 0x40382F82-0x40382FB8 |
| **aot_step 小计** | **62** | **≈77** | |
| **合计（全命中、不含块体）** | **127** | **≈168** | |

块体本身：flash 块平均 233 B，按 RVC 平均 3.2 B/条 ≈ 73 条主机指令 ≈ **85 cycles**（IRAM 命中）。
所以「全命中」的一次 dispatch ≈ **253 cycles**；实测重绘帧 **1,724 cycles**，差 1,471。

| 口径 | 数值 | 说明 |
|---|---|---|
| 稳定态 mi_s=22.4 → 160 MHz/22.4 M | 7.1 host cycles/guest 指令 | 95 % 是折叠记账，**不可用作 dispatch 成本** |
| 稳定态 AOT 段 aoti=226 K/s | 5.2 % 的 mi_s | 折叠之外的真实 AOT 部分 |
| 重绘帧 11.7 M / 58,477 guest 指令 | **200 host cycles/guest 指令** | 含 HLE 像素、tick、dispatch、flash miss |
| 重绘帧 11.7 M / 6,788 dispatch | **1,724 host cycles/dispatch** | **本轮唯一可比的分母** |
| 静态估算（全命中） | 253 cycles/dispatch | 差 6.8× ⇒ 差额必须由 flash 行 miss 解释 |

**一句话：固定开销 168 cycles 是可控的，1,724 里 85 % 不是 dispatcher。**

---

## 3. Q2：28 倍/6.8 倍的机制预算（哪个能凑够 11.7 M）

### 3.1 预算表（按「能提供多少 cycles」排序）

分母：一帧 6,788 dispatch、58,477 块内指令、73.2 ms = 11.7 M host cycles。

| # | 机制 | 单次成本 | 帧内量 | 预算 | 占比 |
|---|---|---|---|---|---|
| a | **flash 侧 block 体代码行 miss**（1769 块 / 413,020 B，233 B ≈ 7.3 行） | 304 cycles/行 × 3.4 行（反推） | 6,788 | **7.0 M** | **60 %** |
| d' | **AotEnt 入口行在 DROM**（8 B/项，4 项/行）+ 页缓存 miss 时的 71 项二分（aot.h:364 记 36 % dispatch 跨页） | 1 行=304（摊 0.5–1） | 6,788 | 1.0–2.1 M | 9–18 % |
| e1 | **固定 dispatch 开销**（127 条指令，§2） | 168 | 6,788 | 1.14 M | 10 % |
| e2 | **HLE 像素路径**：blit 9,216 px × ~25 + push 9,087 px × ~90（两次虚调用 write16(0x3092)，unsp_glue.cpp:1334-1335） | — | — | 1.05 M | 9 % |
| e3 | **soc.tick()** | dtick 3.2–3.7 ms | 1 | 0.42–0.52 M | 4 % |
| b | **精灵数据从 flash 窗口读**（read16 走 mmap，romstore.h:109-117） | 576 行/个 96×96 精灵 = 175 K | 1–2 精灵 | 0.18–0.35 M | 1.5–3 % |
| c | **数据读冲掉代码行** | 与 b 同量级，不独立 | — | 计入 a（抬高 a 的 miss 率） | — |
| d | **dispatcher 不在 IRAM** | — | — | **0**（实测在 IRAM） | 0 |

合计 ≈ 11.6 M，与 11.7 M 配平。

### 3.2 逐条说明

**(a) 唯一有足够容量的机制。** 1769/1944 个块体在 flash（§4 的 nm 实测），块内顺序执行，
每条 233 B 的块体天然横跨 7.3 条 32 B 行。512 行的 16 KB cache 面对 12,907 行（413 KB / 32）的块体集合，
命中率只能来自「同一块重复执行」（循环体）和「相邻块共享边界行」。反推的 3.4 行/dispatch（47 % miss）
意味着约一半行命中——与「每扫描线重走同一批设置块」的访问模式一致。**这是本轮唯一值得开一版的优化目标。**

**(b) R6 §0.3 的判断偏大。** 算术：一个 96×96 RGB565 精灵 = 18,432 B = 576 行；一帧只 blit 一次
（切换窗口 blitpx=10,059，即 1.09 个满屏）⇒ 576 × 304 = 175 K cycles = 1.1 ms。即使一帧 4 个精灵也只有 4.4 ms。
**「30–50 ms 花在数据读」在当前像素量下站不住**；它成立的前提是「每帧几十个精灵」，实测没有。

**(c) 不是独立项。** 数据读的价值在于把代码行挤出 cache：每扫描线读 6 条数据行、一个精灵 576 条；
16 KB = 512 行，所以画一个精灵就足以把整个 cache 换一遍。这正是 (a) 的 miss 率偏高（47 % 而非 10 %）的原因，
但它自己的成本已经算在 (b) 里，**不能重复计一次**。

**(d) 不成立，实测。** execute_run 0x40382FD4（4,738 B）、aot_step 0x40382E32、wait_loop_collapse 0x40382AEC、
spin_collapse 0x403829B4 全在 .iram0.text（0x40380000-0x4039BB48）。dispatcher 自身不产生 miss。
**但入口表不是：** aot_ent_048 等 71 个数组各 0x800 B，落在 0x3C0C63C0-0x3C0EC3C0（DROM = flash 数据窗），
g_aot_desc 608 B 也在 0x3C0C6160。这是「dispatcher 相关但仍付 flash 代价」的部分 (d')。

**(e) 其他：** fallback 244 次 × ~400 = 0.1 M（1 %）；3 个 64-bit 计数器 0.07 M（0.6 %）。

### 3.3 排序与判断

1. **(a) 7.0 M（60 %）** —— 唯一能凑够的机制；修它只能动「块体放哪」或「一行多贵」。
2. **(d') 1.0–2.1 M（9–18 %）** —— 第二，且改动最小（608 B 的 g_aot_desc 搬 DRAM、入口项压到 4 B、
   或给热页做 DRAM 影子表）。
3. **(e1) 1.14 M（10 %）** —— dispatcher 固定开销，最多能砍一半（§5）。
4. **(e2) 1.05 M（9 %）** —— push 每像素两次虚调用，可合并成一次。
5. (e3)/(b) 共 0.6–0.9 M，量级小，先不动。

---

## 4. Q3：IRAM 里到底有多少、还剩多少、还差哪些块

### 4.1 实测（nm --print-size + size -A + map）

| 项 | 数值 | 依据 |
|---|---|---|
| AOT 块总数 | 1,944 | nm 里 aot_b* 符号数；aot_blocks.c 头注释「1944 registered」 |
| 块体总字节 | **452,140 B**（平均 232.6） | nm 求和（R6 记 448,704，差 3.4 KB，不影响结论） |
| IRAM 里的块 | **175 块 / 39,120 B**（平均 223.5） | 地址 0x40384256-0x4038DB26 |
| flash 里的块 | **1,769 块 / 413,020 B**（平均 233.5） | 地址 0x4201F352-0x420841F0 |
| .iram0.text 总量 | 113,480 B（0x40380000-0x4039BB48） | size -A；AOT 块占 34.5 % |
| .flash.text 总量 | 694,606 B | size -A；**AOT 块体占 59 %** |
| 静态占用 | 232,864 B | 113,480 + data 7,416 + bss 111,960 |
| heap 起点 | 0x3FCB8E60（map:24033 _heap_start） | DIRAM 顶 0x3FCE0000 ⇒ **160,160 B heap** |
| 运行时 heap | 166,844 B（最大块 114,688 B） | r5_diag_switch.log:60 |

**IRAM 余量：** 不是 R6 说的 88,440 B（那是 linker 的 region 记账），而是**由 heap 决定**：
heap 起点 0x3FCB8E60 到 0x3FCE0000 共 **160,160 B**，固件自报 166,844 B 可用（含 7 KB RTCRAM）。
**判据：IRAM 代码增量 ≤ 48 KB，保住 ≥ 96 KB heap**（两条 DMA 条带 15,360 B×2 + FreeRTOS + 菜单/音频缓冲，
WiFi 未初始化不占）。

### 4.2 各区间放置现状（「重绘帧执行的块有没有进 IRAM」的答案）

| guest 区间 | 块数 | IRAM | flash | flash 字节 | 备注 |
|---|---|---|---|---|---|
| 0x004800-0x004900（IRQ7 体） | 21 | 4 | **17** | 2,556 | 64 Hz ISR，每 tick 都跑 |
| 0x005000-0x005600（辅助） | 175 | 7 | **168** | 35,688 | 含 0x5251/0x5376 自旋块 |
| **0x005A00-0x005B60（push/fill）** | 48 | **48** | 0 | 0 | 全在 IRAM（OK） |
| 0x005F00-0x006000（乘加） | 51 | 21 | **30** | 3,290 | 0x5F8B 在 IRAM |
| 0x200000-0x201000（基础） | 249 | 5 | **244** | 69,014 | |
| 0x200300-0x200343（PRNG 尾） | 1 | 0 | 1 | 1,022 | fast_hot 优先拦，无人用 |
| 0x203000-0x206000 | 389 | 0 | **389** | 94,384 | 卡片/图标渲染 |
| **0x207000-0x207A00（render_band 设置）** | 223 | 0 | **223** | **69,466** | **最大单一缺口** |
| **0x207A00-0x207D00（逐记录体）** | 70 | **70** | 0 | 0 | 全在 IRAM（OK） |
| 0x207D00-0x208000（尾） | 4 | 3 | 1 | 88 | |
| 0x230000-0x230340（主循环外壳） | 48 | 13 | **35** | 5,514 | 0x230321/0x230323 已在 IRAM |

**读法：** R6 把「像素推送 + 逐记录体」搬进 IRAM 是对的（0x5A00-0x5B60、0x207A00-0x207D00 100 % 命中），
但**重绘帧的块数主体不在那两个区间**：render_band 的「每行/每条记录设置」在 0x207000-0x207A00，
223 块 / 69,466 B，**一块都没进 IRAM**；0x203000-0x206000 的 389 块（94,384 B）同样全在 flash。
这两处合计 163,850 B，比整个 IRAM 预算还大，不可能全搬。

### 4.3 可执行候选清单（按「行数 × 执行次数」排序）

数据源：tools/coverage/block_rank.json（PC 侧 harness 20 s 自动跑，1729 个块带 execs；
**不是设备侧重绘帧 census，必须标明**）+ nm 尺寸。0x207000-0x207A00 里最胖的块：

| PC | 字节 | 32 B 行数 | profile execs | 说明 |
|---|---|---|---|---|
| 0x2070D5 | 596 | 20 | 72 | 直线型 bp 相对拷贝（26 条 guest 指令，9 guest cycles） |
| 0x2070B9 | 548 | 19 | 72 | 同上 |
| 0x207051 | 532 | 18 | 72 | 同上 |
| 0x20706B | 532 | 18 | 72 | 同上 |
| 0x207085 | 532 | 18 | 72 | 同上 |
| 0x20709F | 532 | 18 | 72 | 同上 |
| 0x2070F1 | 532 | 18 | 72 | 同上 |
| 0x20710B | 532 | 18 | 72 | 同上 |
| 0x207125 | 532 | 18 | 72 | 同上 |
| 0x207039 | 480 | 16 | 72 | 同上 |
| 0x20713F | 430 | 15 | 72 | |
| 0x207029 | 288 | 10 | 72 | |

- **病理样本 0x2070D5：** 26 条 guest 指令、guest 成本 9 cycles、编译后 596 B = 20 条行。
  一次 dispatch 最坏 20 × 304 = **6,080 host cycles**，是它 guest 成本的 675 倍。
  **guest cycles（AotEnt.cyc 里的 9）与 host 成本完全脱钩**，任何按 guest cycles 排优先级的调度都是错的，
  必须按「行数 × 执行次数」排。
- **候选集分三档（按字节预算切）：**
  1. 档 A（6,400 B）：上表前 12 块，可抹掉 12 × 18 × 304 ≈ 65 K cycles/帧（0.6 %）——**太小，性价比低**。
  2. 档 B（24,664 B）：0x207000-0x207A00 里按 execs×行数排前 69 块 = 24.7 KB。
  3. 档 C（69,466 B）：整个 0x207000-0x207A00，吃掉 43 % 的 IRAM 余量。
- **结论：静态 profile 不足以决定搬哪些。** 必须先做一次**设备侧重绘帧 block census**
  （-DC3_BLOCK_CENSUS=1，按 B 切换，控制台发 Y，见 unsp_glue.cpp:912-934），拿到切换帧 top-200 块入口，
  再 × nm 尺寸 = 真实「行数×次数」排序。**这是 Q3 唯一还缺的证据，也是最便宜的一步（一次构建 + 一次按键）。**

---

## 5. Q4：dispatcher 与 aot_step 的结构性浪费（逐项 cycles）

| # | 浪费点 | 位置 | 现状 | 改法 | 节省 |
|---|---|---|---|---|---|
| W1 | 5 个 HLE 钩子各一条「lbu 标志 + lui/addi + beq」比较链 | unsp_glue.cpp:1831-1850 / 0x403832D0-0x4038331A | 24 条指令 + 5 分支 | AotEnt.pad（偏移 7，当前空闲）存 hook_id；aot_step 返回 0/1/2，命中即跳钩子表 | **~20 条 / 22 cycles** |
| W2 | wait_loop_collapse 每 dispatch 一次**真调用** | 0x403832A4 jal 0x40382AEC | 4 条 + callee 8 条 | 折叠点只有 3 个（0x230321/0x321/0x2F4）；先 lpc>>16 判 0x23/0x00/0x20，再精确比较 | **~10 条 / 11 cycles** |
| W3 | m_fastspin + 三个 PC 比较 | 0x403832AC-0x403832CC | 11 条 | 并入 W1 的 hook_id 表（自旋块也是特殊入口） | ~9 条 / 10 cycles |
| W4 | m_block_hist 判空（生产构建永远是 null） | 0x40383266/0x40383274 | 2 条 + 一次 DRAM 读 | 提到循环外（execute_run 入口已做过同样判断，可复制该模式） | ~2 条 / 3 cycles |
| W5 | m_fasthot + 两个 PC 比较 | 0x40383286-0x4038329C | 7 条 | 同上，提到循环外 | ~5 条 / 6 cycles |
| W6 | 3 个 64-bit 计数器（m_aot_insns/m_insn_count/m_aot_blocks）每次 dispatch 两次 RMW | 0x40382F42-0x40382F76、0x40382F88-0x40382FAC | 19+8 条 | 换成 slice 内 32-bit 累加，slice 结束一次并入 64-bit | **~16 条 / 18 cycles** |
| W7 | 每块 18 次寄存器搬运（AOT_LOAD_REGS/AOT_SAVE_REGS，aot.h:83-93） | 块体首尾 | 18 次 DRAM 访问（无 D-cache，~2 cycles/次） | RV32 只有 15 个可用寄存器，做不到常驻；只能「只保存被改写的寄存器」（生成器分析写集） | 最多 ~6 次 / 12 cycles，**收益不确定** |
| W8 | aot_set_pc 在块尾 RMW c->r[6]/r[7] | 每块 | ~6 条 | 与 AOT_SAVE_REGS 合并（现在 r6/r7 被写两次） | ~3 条 / 4 cycles |
| W9 | check_irqs 已内联（每块一次 lw+beqz） | 0x4038374A | 2 条 | 不动 | 0 |

**W1-W6 合计 ≈ 55 条指令 / 72 cycles ≈ 4.2 %（对 1,724 cycles/dispatch）。**
**判断：dispatcher 是「结构性浪费但已不是瓶颈」。** 127 条指令里 50 条可去，即使全去掉也只把重绘帧
从 73 ms 降到 70 ms。R5 §6 表里「HLE 检查合并 −8 %」「aot_step 内联 −13 %」是拿 430–560 cycles 的
dispatch-only 分母算的，**在 1,724 的真实分母下不成立**；请以本表绝对 cycles 为准。

**另一个必须写清的浪费：AotEnt.cyc 是死字段且数值错误。** R5 §5.1 已记，本轮复核仍在：
aot_lookup(lpc, &cnt, nullptr) 传 cyc=nullptr，lbu a2,6(a5) 只取 cnt（aot.h:395-400）；0x2070D5 的
26 条指令记「9 cycles」，而它的 host 成本是 6,080。任何「按块周期预扣预算」的改动先修它。

---

## 6. Q5：按收益/风险排序的改动清单（本轮未动手）

### P1. QIO：把 miss 从 304 降到 ~165 cycles/行
- **改哪里**：sdkconfig CONFIG_ESPTOOLPY_FLASHMODE_QIO=y；整片镜像重出（bootloader 头也要 qio）。
- **为什么第一**：它按比例作用在**所有** flash 行上（本轮 75 % 的帧预算），不需要选块、不需要 census、
  不牺牲 heap。
- **预期**：(a)+(d') 的 8.0 M 按 304→165 缩到 4.3 M ⇒ 重绘帧 73 ms → **~45–50 ms**；时钟帧 14 → ~9 ms。
  这是唯一能把「22 M guest cycles/s」抬到 30 M+ 的单点改动。
- **验证**：LONGF us= 与 gmax，同场景三次切换取中位。若 <10 % 改善，说明 miss 假设错，立刻停手回到 (b) 方向。
- **回滚**：R5 留了 tmp/digicore-c3-full-dio.bin（4,677,888 B）；esptool --before default-reset 可强下载。
  风险集中在 QE 位（R5 §F6 试过失败，需先写 status register 的 QE 位）。
- **风险**：中（起不来）。**收益/风险比最高。**

### P2. 把入口表 + 页描述符从 DROM 挪走（(d')）
- **改哪里**：① g_aot_desc（608 B，0x3C0C6160）启动时拷进 DRAM 数组，aot_lookup 读副本（aot.h:383）；
  ② AotEnt 从 8 B 压到 4 B（fn 指针单独一张表，cnt 进冷数组；32 B 行覆盖 8 个 slot 而不是 4 个）；
  ③ 可选：给 8 个最热页做 DRAM 影子 fn[256]（8 × 1 KB = 8 KB）。
- **预期**：每 dispatch 省 0.5–1 条 flash 行 ⇒ **150–300 cycles/dispatch（1.0–2.0 M/帧，9–17 %）**。
- **验证**：STAT 的 pgsw（页切换数）与 gmax；A/B 同场景 LONGF。
- **回滚**：纯 C 改动，git revert + 重编。**风险：低。** ①单独做就值一版。

### P3. 重绘帧 block census -> 定向 IRAM 提升（(a)）
- **先做**：-DC3_BLOCK_CENSUS=1 构建，按 B 切换，控制台 Y 取 top-200（设备当前没插 USB，需要主审安排台架）。
- **再改**：按「行数 × 次数」排序，把前 N 块写进 tools/coverage/out/aot_iram_r6.txt（格式「PC n」），
  AOT_IRAM_LIST= 重新生成，**增量 ≤ 48 KB**。
- **预期**：每搬一块省 (行数 × 304) × 该块帧内次数。若切换帧 top-100（约 25 KB）占 40 % dispatch，
  则省 0.4 × 3.4 × 304 = 413 cycles/dispatch ⇒ **73 → 55 ms**。
- **验证**：LONGF us= + .iram0.text 增量 + 运行时 heap ≥ 96 KB；面板截图/帧 CRC 比对（IRAM 化不改语义）。
- **回滚**：删掉 r6 列表重生成即可（aot_blocks.c 是生成物）。
- **风险**：低-中（IRAM 与 heap 共享同一块 SRAM，吃多了 DMA 条带分配会失败——用 heap_caps_get_free_size 守）。

### P4. push 钩子每像素两次虚调用（(e2)）
- **改哪里**：hle_push_pixels（unsp_glue.cpp:1334-1335）每像素两次 d->write16(0x3092,...)。
  仿照 m_lcd_fill 增加 m_lcd_push_word(opaque, pixel, n)，一次推 n 个像素
  （spi2_txdata_w 的语义就是 lcd.data_w(byte)，两字节顺序 = 高字节先）。
- **预期**：9,087 px × ~35 cycles = **0.32 M/帧（2.7 %）**，顺带减少 SPI 寄存器写次数。
- **验证**：面板逐像素截图比对 + pushpx 计数不变 + LONGF。
- **回滚**：控制台 P 0 开关已在（app_main.cpp:1608），或 revert。**风险：低**（触及面板时序，必须截图比对）。

### P5. dispatcher 去冗余（W1-W6，见 §5）
- **预期**：**72 cycles/dispatch ≈ 4.2 %**（重绘帧 73 → 70 ms；时钟帧 14 → 13.5 ms）。
- **验证**：aotb/aoti/aotmiss 计数必须与改动前逐位相同（纯调度改动不改语义），再比 LONGF。
- **风险：低，收益也低 —— 排在 P1-P4 之后，或作为顺手清理并进某一版。**

### P6. ICache prelock（ROM API，把 16 KB 里的若干行钉死）
- **改哪里**：Cache_Start_ICache_Preload / Cache_Lock_ICache_Items（esp_rom/esp32c3/rom/cache.h）。
  候选：AotEnt 热页的行、0x2070xx 的十几条大块。
- **预期**：等价「16 KB 软 IRAM」，不占 DRAM；上限 = 被钉住行对应的 miss 总量。
- **风险：高**（ROM 头文件明写 Please do not call this function in your SDK application；cache error 直接 panic，
  IDF 无兼容路径）。**只在 P1-P4 做完、还想再挤 10 % 时碰。**

### 明确不要做的两件
1. **不要用「降帧预算/拆帧」治慢动作。** 见 §0.7 的算术：切帧不改吞吐。
2. **不要再按 guest cycles（AotEnt.cyc）排优先级。** 0x2070D5：9 guest cycles / 6,080 host cycles。

---

## 7. 我没验证的部分

1. **重绘帧的实际 miss 行数。** 3.4 行/dispatch 是**由预算反推**的，不是测出来的。C3 没有可用的
   I-cache miss 计数器（PMU 不含），只能靠「A/B 搬运」或「QIO 前后帧时间比」间接验证。
   **这是本报告最关键的未验证项，也是 P1 的判据。**
2. **每行 miss 的真实延迟。** 304 cycles 来自 docs/RESEARCH_EXT_C3_FETCH_PATH_2026-11-26.md §1.3 的
   带宽推算（152 SPI clocks @80 MHz），官方未公开。若 C3 实际做了 critical-word-first，真实停顿可能只有
   100–150 cycles，那时 (a) 的容量就不够解释 11.7 M，得重新找机制。**推测：** 未做 critical-word 优化
   （esptool 文档只说「32 bytes is read per command」）。
3. **hot_prng_n 的每 pass 成本。** 从「时钟帧 14 ms、折叠 5,073 pass/帧」反推约 396 cycles/pass（89 % 帧时间）。
   我没单独测它，也没拆到指令级。若这个数成立，**时钟帧的真正瓶颈是 hotblock.h 生成的 C 代码，而不是 AOT**，
   这与「继续优化 fps 没用」的观察一致，但需要一次 A/B（把 hot_prng_n 换回解释器看帧时间涨多少）确认。
4. **切换窗口 3 个 LONGF 是否同一帧。** r5_blkcensus.log:100-115 里连续出现 88.6/102/84 ms 与 52/78/70 ms 两组，
   我按「一次按键=一次重绘」解释，但没有帧号对齐证据。
5. **像素数归属。** blitpx/pushpx/fillpx 是 1 s 窗口累计值，只能把窗口内 LONGF 帧数（3 个）当分母，误差 ±30 %。
   要精确到帧，需要把 g_blit_px_win 等也打进 LONGF 行（一行 6 字段的改动，属于源码修改，本轮未做）。
6. **IRAM 提升后的 heap 安全性。** 160,160 B 由 map 算出，运行时 166,844 B 含 RTC/retention 区；
   我没验证「吃掉 48 KB 后 DMA 条带分配仍成功」。
7. **应用层是否还有别的同步阻塞在重绘帧里。** blit_frame()、快照、菜单渲染都在 run_guest_frame() 之外，
   LONGF 看不到；本轮只对 guest 侧负责。

---

## 8. 复现命令

    # 符号与放置
    riscv32-esp-elf-nm --print-size --defined-only build/digicore-c3.elf | grep aot_b
    riscv32-esp-elf-size -A build/digicore-c3.elf
    # dispatch 反汇编（本报告用到的三段）
    riscv32-esp-elf-objdump -d --start-address=0x40382e32 --stop-address=0x40382fd4 <elf>   # aot_step
    riscv32-esp-elf-objdump -d --start-address=0x40383260 --stop-address=0x40383340 <elf>   # 热循环头
    riscv32-esp-elf-objdump -d --start-address=0x40383848 --stop-address=0x40383900 <elf>   # push 钩子内联体
    # 行数/预算换算
    #   块行数 = ceil((size + 31) / 32)；一行成本 = 304 cycles（DIO@80 MHz 推算）
    #   预算 = 行数 × 304 × 该块帧内执行次数

本报告全部数字可由上述命令 + dbgtest/r5_*.log 复算；未改动任何 .c/.cpp/.h/.py。

---

## 9. 附：P3 的实验已经有一半现成（补 §4.3 / §6-P3）

1. **控制台有 R = 强制整屏重绘**（app_main.cpp:1448-1451，`case 'R': s_force_full = true;`），
   配合 **Y = 打印并清零 block census**（app_main.cpp:1705-1706 → aot_dump_blocks）与
   **-DC3_BLOCK_CENSUS=1**，就能拿到「一次确定性整屏重绘」的块分布，不用靠按键时机。
   **这是 P3 的推荐做法：Y（清零）→ R（整屏重绘）→ Y（取分布）**，比按 B 可复现得多。
2. **已有旧抓取，但不能直接用：** tmp/census_redraw.log（2026-11-22，v1.11 时代）+ tmp/census_redraw.py
   就是这个思路。该文件只有一个 BLKHEAD、747 个块，合计 301,108 次，其中 0x230321/0x200343/0x00485D
   各 23,288 次（稳态自旋）与 0x207A35/0x207D10 各 4,416 次（96 行 × 46 记录）混在一个窗口里，
   **不是干净的单帧重绘**。另外它比现在的 aot_blocks.c 早 4 天（1466 块时代），
   所以只能用来看「哪些 PC 会跑」，不能用来定字节预算。
3. **有用的旁证：** 该窗口里 0x5F8B（32×32 乘法）执行 **27,022 次**，是除稳态自旋外最大的单块；
   它已在 IRAM（0x403846CE），这也是「HLE + IRAM 组合确实管用」的正面样本。


---

## 10. 与并行工作的接口 / 数据时效

1. **本报告的实测数字全部来自 build/digicore-c3.elf（2026-11-26 19:06:46 构建）**。工作树里
   `unsp_glue.cpp`、`app_main.cpp`、`aot_blocks.c` 等仍是未提交状态（`git status` 显示 M），
   `aot_blocks.c` 的 mtime 是 21:38（晚于 ELF）。我核对过一致性：当前 aot_blocks.c 生成的 1944 个块
   与 ELF 里的 1944 个 `aot_b*` 符号一一对应、175 个 AOT_IRAM 标记与 IRAM 地址一一对应，
   `.flash.text` 694,606 B 也与 R6 记录一致 ⇒ **分析对象就是 v1.14.0 的块集**。但重编一次后
   个别块的字节数会变（编译器版本/顺序敏感），§4.1 的总字节数需重算。
2. **`tools/coverage/out/aot_iram_r6.txt` 已经存在（141 条）**，不是我这一轮建的。§4.2/§4.3 的放置分析
   基于 `aot_iram_r5.txt`（175 条）。如果 r6 列表已经/将要生效，`aot_blocks.c` 必须重新生成，
   §4.1 的 175/39,120 B 与 1769/413,020 B 要按新列表重测；**判定标准不变：行数 × 执行次数**。
3. **`tools/r6b/`（probe.cpp 等）是另一位并行审计员的现场工具**，我没有使用、也不改动它。
   如果它的 probe 能给出重绘帧的**每块 host cycles 分布**，那正好可以替我把 §7.1（miss 行数）从
   「反推」变成「实测」：把 probe 的块级 host cycles ÷ 304 就是该块的行 miss 数。**这是我最想要的交叉验证。**

