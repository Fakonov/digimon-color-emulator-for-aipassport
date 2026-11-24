# 运行流畅度：除现有解释器 + 基本块 AOT 之外的技术路线调研

日期：2026-11-24
基线：v1.6.0（`e0b9581`，工作区 clean），ESP32-C3 / 8 MB flash / 无 PSRAM / 320×240 ST7789P3
性质：**只做调研与可行性分析，没有改固件、没有烧机、没有跑新基准**

---

## 0. 结论先说

### 0.1 现状不是“再调一调就能满帧”

当前设备端到端约 **110 个主机周期 / guest 指令**。重绘帧 154,451 条 guest 指令，折合
**17.0 M 主机周期 = 106 ms/帧**，即 8.6–9 fps；中负载界面切换 17–22 fps；静止 58 fps。
要 60 fps 需要 **17.3 周期/指令（快 6.4×）**，要 30 fps 需要 **34.5 周期/指令（快 3.2×）**。
这不是显示、blit、tick 或某个参数的问题：`blit_pct` 只有 0–1%，渲染路径已经不是瓶颈。

### 0.2 三条最重要的新判断

1. **“RAM 只剩 1.1 KB”有一半是自己占的。** `SparseRom` 里有 16 × 4 KiB = **64 KiB 页缓存**，
   但设备启动时 `esp_partition_mmap()` 成功，`read16()` 走的是 mmap 直读路径，这块缓存在
   正常运行中**一次都不会被读**。另外 `g_miss_cnt/g_miss_pc`（8 KiB 诊断）、
   `bench_exec/bench_hotblock`（16 KiB 基准 scratch）也是常驻 DRAM。先把这些腾出来，
   才有资格谈“预解码缓存”“热区进 IRAM”。这是本轮审查最有把握的一条。

2. **上次“分组超块”失败，不等于“区域化 AOT”失败。** `AOT_GROUP` 的准入条件是
   “所有静态前驱都在组内”，把循环体全部排除，覆盖率只有 12%，所以结论是负的。
   换一种结构：**每个页一个函数、函数内每个块一个 label、入口用 label 表 computed goto**，
   允许从任意块进入，就能把 1,466 个块的大多数收进少量“多入口区域函数”，
   寄存器在区域内部一直钉在 RV32 寄存器里，块间直接 `goto`，不再每块 `LOAD_REGS/SAVE_REGS`
   和 `return`。这正好打在当前 54% 的框架开销上，是 C3 上唯一有希望拿到 2× 量级的软件路线。

3. **C3 上 60 fps 不可达，30 fps 是“边缘可争”。** 按现有公开数据和本项目实测，
   软件路线现实落在 **55–75 周期/指令**，即 **1.4–1.8×**：中负载 28–40 fps、重绘 12–16 fps。
   要重绘帧 60 fps，只有换更快的核（P4 单核约 2.9×，加软件改造约 5–8×）或上 Linux 小板。

### 0.3 建议的决策顺序

| 目标 | 建议路线 | 预期 | 工期 |
|---|---|---|---|
| **留在 AI Passport（C3）**，把“明显卡”变成“基本顺” | 先腾 RAM → 区域化 AOT → HLE 增量 → 输入/时间基 | 1.4–1.8×，中负载 28–40 fps，重绘 12–16 fps | 4–8 周 |
| **可以改板，要接近满帧** | ESP32-P4 + 同一套软件改造 | 2.9×（裸换）→ 5–8×（含软件），重绘 40–60 fps | 3–6 个月 |
| **要最快出货的 2–3×** | ESP32-S3 + 软件改造 | 1.8–2.2×（裸换）→ 2.5–4×（含软件），重绘 22–30 fps | 2–3 个月 |
| **60 fps 不可谈判、且能接受 Linux** | Milk-V Duo / Pi Zero 2 W 级 SBC，直接跑 MAME `digicolr` | 100% 实时 | 2–3 个月，功耗/启动代价大 |

**不建议**：JIT、LLVM lifter、全程序 AOT 到 flash、双核分核、超频、把 QIO 当主杠杆。
理由见 §6。

---

## 1. 复核：钱到底花在哪

### 1.1 现有数字（全部来自仓库内实测记录）

| 指标 | 值 | 出处 |
|---|---|---|
| 端到端单价 | 110 主机周期 / guest 指令 | `docs/PERF_2026-11-22.md` §2 |
| AOT 路径单价 | 63 周期 / guest 指令（11 条/块口径）；另有 195 周期/指令（1,289 周期 ÷ 6.6 条口径） | `PERF_2026-11-22.md` §2；`HANDOFF_2026-11-22.md` §7.5。**两个口径不要混用** |
| 解释器路径单价 | 约 424 周期 / guest 指令 | 同上 |
| 重绘帧 guest 指令 | 154,451 条/帧 | 同上 |
| 块平均 guest 指令 | 6.6 条（动态）/ 10.1 条（静态，生成器注释；口径待统一） | `HANDOFF_2026-11-22.md` §7.5；本轮 `aot_blocks.c` 统计 |
| 每块固定开销 | 1,289 主机周期，其中框架 699、块体 549、查表 41 | 同上 |
| AOT 代码体积 | 673 KB（`aot_blocks.c` 编译后） | `CMakeLists.txt` 注释 |
| C3 cache | 16 KB（400 KB SRAM 中 16 KB 专用于 cache） | C3 数据手册 |
| DRAM 余量 | 320,158 / 321,296 B，**剩 1,138 B（99.65%）** | v1.6.0 `build.log` |
| 设备帧率 | 中负载 17–22 fps；重绘 8.6–9 fps；guest 实时率约 27% | `AUDIT_2026-11-23.md`、`HANDOFF_2026-11-24.md` |

### 1.2 两个必须先澄清的计量问题

1. **`C3_SPLIT` 当前是 0**（`main/CMakeLists.txt:39`），所以日志里的
   `caot/cinf/cfast/cpa/clk/cfn` 全是 0 —— 不是没有负载，是根本没测。
   任何路线选择之前，先把 `-DC3_SPLIT=1` 打开跑 30 s（代价约 1.5% 帧时间），
   拿到“AOT / 解释器 / fast_hot / tick / blit”的真实占比，否则又会拿错分母做决策。
2. **`aotb/aoti` 的口径有歧义。** `aot.h` 注释说 `cnt` 是 native instructions，
   日志里 `aoti` 又像 guest 指令数；按 `aotb × 6.6` 与 `aoti` 两种算法会得到
   “AOT 覆盖 10%”和“覆盖 41%”两个互相矛盾的结论。先把 `cnt` 的定义钉死
   （建议在生成器头注释里写清楚，并在 STAT 里改名为 `aot_ginsn`），再谈覆盖率。

### 1.3 结构性约束

- **代码 673 KB，cache 16 KB。** 这是硬冲突：AOT 代码不可能常驻 cache，
  每个 dispatch 的取指都可能在 flash 上。把热块放 IRAM 是唯一解，但 DRAM 已满。
- **IRAM 与 DRAM 共享同一块 400 KB SRAM。** 腾出 DRAM 才能腾出 IRAM 代码空间；
  不能把“IRAM 还有 200 KB”当成可用余量。
- **没有 PSRAM。** 任何“把 673 KB 代码/ROM 放 PSRAM”的思路在 C3 上不存在；
  这是 S3/P4 的结构性优势。
- **单核。** 显示只占 1–5%，把显示/音频放第二个核最多省 5%，对 guest 吞吐没有帮助。

---

## 2. 已经试过、不要重做的路线

| 路线 | 结果 | 为什么 |
|---|---|---|
| 分组超块（`AOT_GROUP`） | 覆盖率 12.1%，`caot` 39.7%→40.0% | “唯一入口”条件把循环体排除；组函数变大后 `-O3` 寄存器分配变差 |
| SAVE 窄化 | `FB_CRC` 变 `2DEF5A32`，已回滚 | 语义错，根因未定位 |
| `AOT_LOCAL_MIN` 4→10 | 代码 −1.1%，`gmax` 46→56 ms | 漏算 `c->r[]` 地址计算 |
| 紧凑块表进 DRAM | `aotc=1466/1`，`guest_pct` 不变 | 查表不是瓶颈 |
| 60 个热块进 IRAM | 与基线逐位相同 | 只覆盖 60 块，且这些块不是 i-cache miss 的主因（或 body 本身贵） |
| `aot_blocks.c -Os` | 代码 −33.6%，fps +4.8% | 有收益但不足以改命 |
| QIO flash | 启动 `bad magic` | XMC flash 的 QE 位没置；不是“QIO 无效”，是没成功 |
| 页缓存 8×2 KB | `pgsw=7,600/s`，27%→66% | 与 `esp_partition_mmap` 实验冲突，结论不可信；建议重测 |
| 双缓冲整帧 DMA | 上限 1.5 ms，需 16 KB DRAM | 余量不够，且 blit 不是瓶颈 |
| blit 计费 139→8、`always_inline`、标志位无分支 | 噪声内 | 不在关键路径 |
| 改 `kCpuClock` | 只改切块粒度，不改吞吐 | 1/60→1/120 帧率升、游戏速度降 |
| 关 `C3_SPLIT` 计量 | 8.28 vs 8.64 fps，噪声内 | 诊断价值大于 1.5% |

**结论**：这些负结果大多成立，但**“IRAM 无效”“QIO 无效”“页缓存无效”三条的实验条件不完整**，
不应作为“永远不做”的依据。尤其是 IRAM：只测了 60 个块，没有测“区域函数整体进 IRAM”。

---

## 3. 新软件路线（不换硬件）

### 3.1 R1：区域化 AOT —— 多入口区域函数 + 寄存器钉住 + 块间直跳

**这是本轮最推荐的一条。**

#### 问题

现在每个基本块是一个独立 C 函数：进入时 `AOT_LOAD_REGS`（9 次访存）、退出时
`AOT_SAVE_REGS`（9 次访存）、`c->icount -= cyc`、`return` 回调度器，再查表进下一块。
块平均只有 6.6 条 guest 指令，固定开销 1,289 周期里 699 是这套框架，占 54%。
上次的超块想省掉它，但要求“所有前驱都在组内”，循环体天然不满足，覆盖率只有 12%。

#### 做法

把 guest 地址空间按页（`lpc >> 8`）切成区域，每个区域生成**一个 C 函数**：

```c
static void region_207(Ctx *c) {
    // 进入区域时一次性 LOAD_REGS：r0..r7, sb 进局部变量
    goto *labels[c->r[7] & 0xff];        // 多入口：label 表允许从任意块进入
L_207A00:
    ...                                    // 块体，寄存器就是局部变量
    goto L_207A3E;                         // 静态后继：直接跳，不 return
L_207A3E:
    ...
    if (cond) goto L_207B10;
    // 离开区域（跨页/计算跳转/中断/icount 到期）才 SAVE_REGS + return
    AOT_SAVE_REGS(c); c->icount -= cyc; return;
}
```

关键点：

1. **多入口**：`labels[]` 是 `void *` 数组，`goto *labels[pc & 0xff]` 可以从任意块进入，
   不需要“唯一入口”。这就绕开了 `AOT_GROUP` 的结构天花板。
2. **寄存器钉住**：区域函数内部 `r0..r7/sb` 是局部变量，GCC 可以把它们分配进
   `s0..s11`/`t0..t6`；只有区域出口写回一次。框架开销从“每 6.6 条指令 699 周期”
   降到“每区域出口约 50–100 周期”。
3. **块间直跳**：静态后继用 `goto L_x`，不再 `return` + 查表。
4. **周期记账**：块内累加 `cyc`，区域出口一次 `icount -= cyc`；中断只在区域出口
   或显式 `sirq` 检查点投递（与现在“块边界投递”的语义一致，只是边界变少）。
5. **代码体积**：省掉每块的 prologue/epilogue，预计比现在小 20–30%；
   热区域（1–2 个页）可以整段放 IRAM，冷区域留 flash。
6. **验证**：现有 golden 三件套（逐指令 trace 100%、帧 hash、存档逐字节）足够；
   先在 PC 侧跑 20 s，再上机。

#### 预期

- 框架 699 → 50–100 周期/区域出口；块体 549 不变（或略降，因为局部变量生命周期更长）。
- 按 6.6 条/块、每区域 10–20 块估算：**195 → 70–100 周期/guest 指令**；
  若同时做 lazy flags / 16 位截断消除，**可到 55–75**。
- 端到端 110 → **60–80**，即 **1.4–1.8×**；重绘 8.6 → **12–16 fps**，中负载 17–22 → **28–40 fps**。
- 这是估算，必须先做 60–100 块的切片 A/B（`tools/measure_aot.py`）再上机。

#### 成本与风险

- 3–5 周；主要工作在 `tools/aot.py` 的 `emit_body` 和块表生成。
- 风险：区域边界的中断/周期语义；计算跳转落到区域中间；`icount` 到期点。
  都有现成的 trace 对拍兜底。
- **不建议一次改全量**：先做一个页（比如最热的 `0x207` 页）验证，再推广。

### 3.2 R2：热循环 trace 编译

在 R1 之上，把 3–5 个最热的**循环**（`render_band` 的扫描线循环、`0x005F8B` 乘加库、
`0x230321` 主循环、push/blit 的外层）编成“一条 trace 一个函数”，循环回边用直接 `goto`，
一次调用跑完整个循环（或跑到中断/预算边界）。

- 预期：这些循环内 **20–50 周期/guest 指令**；整机再 +30–80%。
- 成本：2–4 周；风险高（循环出口条件、内存别名、中断语义）。
- 做法：先做**影子实现**（guest 照跑，trace 并行算，逐像素/逐寄存器比对），
  全等后再摘掉 guest 路径——这是项目已经验证过的 HLE 流程。

### 3.3 R3：预解码 token + computed goto 的解释器

不是 JIT，是“把 guest 指令预解码成 RAM 里的 token 数组”，分派用 `goto *handler[token.op]`：

- 省掉每次 `fetch16` + 大 switch 解码 + `aot_rd` 边界检查；
- token 可以带预解码的操作数、预计算的 RAM 指针、下一条 token 的索引；
- 热 8,000–10,000 条 guest 指令 × 8 B = **64–80 KB**，正好是 §3.4 腾出来的量级；
- 预期解释器路径 **424 → 90–140 周期/指令**（2.5–4×），对 AOT 未覆盖的部分收益最大；
- 成本 2–3 周；风险中等（无自修改代码，构建一次即可；只需处理 RAM/ROM 边界）。

这条和 R1 不冲突：R1 打热区，R3 打冷区；也可以先用 R3 替代“解释器回退”，
再把 R1 只做最热的 1–2 个页。

### 3.4 R4：先腾 RAM（所有 RAM 型方案的前置条件）

从 v1.6.0 的 map 和源码核对，下列缓冲在正常运行中是**死重或可裁剪**：

| 项 | 大小 | 依据 | 处理 |
|---|---|---|---|
| `SparseRom::m_cache`（16×4 KiB 页缓存） | **64 KiB** | 设备 `esp_partition_mmap()` 成功后 `read16()` 走 `m_base` 直读；`m_cache` 只在 mmap 失败时用 | 用 `#ifdef` 或运行时 `heap_caps_malloc` 只在 fallback 分配 |
| `g_miss_cnt` / `g_miss_pc`（miss census） | 8 KiB | 诊断用，`C3_MISS_CENSUS=0` 时仍占 .bss | 编译开关裁掉 |
| `bench_exec()` 的 `bus` / `bench_hotblock()` 的 `scratch` | 16 KiB | 只在 boot 跑一次 | 改栈上/静态复用 `s_strip` |
| `panel_fill_black()` 的 `fill` | 10 KiB | 一次性清屏 | 复用 `s_strip` 或分块 |
| 128×128 帧缓冲 → 96×96 可见区 | 最多 14 KiB | guest 可见区是 (0,18)–(95,113)，其余像素不可见；需确认 guest 不读回 | 先做影子验证，再裁 |

合计**可安全腾出约 88–98 KiB**（不含帧缓冲那一项需要影子验证的 14 KiB）。这不是“性能优化”，但它把
“没有 RAM 做 decode cache / 热区 IRAM”这个前提直接推翻。
腾出后建议的分配：**32–64 KiB 给热区 IRAM 代码**，或 **64 KiB 给 R3 的 token 数组**。

> 注意：IRAM 和 DRAM 是同一块 SRAM 的两个别名；腾 DRAM 才能在链接时把更多
> `.iram0.text` 放进去。改完必须看 map 的 `iram0_0_seg` / `dram0_0_seg` 和
> `_iram_end`/`_bss_end` 断言，不能只看 `idf.py size` 的百分比。

### 3.5 R5：HLE 增量（已有计划，继续做）

- `0x005F8B` 定点乘加库：占 39.8% 指令、约 15.6% 主机周期；HLE 预期省 10–16%。
- `render_band` 整函数：占 46.2% 指令、17.3% 周期；HLE 预期省 17–25%。
- 两者都做完，重绘帧大约 **1.2–1.4×**（8.6 → 10–12 fps），单独不够，但和 R1/R2 乘起来有意义。
- 风险：`render_band` 有裁剪/翻转/色键三条路径，翻转路径当前 trace 执行率为 0，
  必须先影子实现，不能直接替换。

### 3.6 R6：编译/缓存小项（便宜，但别指望）

| 项 | 预期 | 备注 |
|---|---|---|
| `-flto`（跨 TU 内联） | 0–5% | 现在 `aot_blocks.c` 是 3.8 MB 单 TU，`-O3` 内联预算已经被撑爆；分片 TU 可能比 LTO 更有效 |
| AOT 代码按页拆成多个 TU | 5–15%〔估算〕 | 每 TU 的内联预算独立，块体可能更紧 |
| 热区域 IRAM（R1 之后） | 10–30%〔估算〕 | 只有“会 miss 的代码”才受益；现有 60 块实验不能代表区域函数 |
| QIO 80 MHz（正确置 QE） | 0–15%〔估算〕 | 先把 XMC 的 QE 位写对，再 A/B；不要再拿 `bad magic` 当结论 |
| 192 MHz 超频 | 10–20%〔风险〕 | C3 官方最高 160 MHz；PLL/ flash 时序不稳，不建议产品化 |
| `-msave-restore` | 通常 −体积、可能 −性能 | 官方文档明说是省体积，不是提速 |
| `__attribute__((hot))` | 0–3% | 只在同 section 内调布局，跨 flash 页无效 |

### 3.7 R7：输入延迟与时间基（不改吞吐，但“手感”提升最大）

这是最容易被“帧率”掩盖的一条。

- 现状：`update_buttons()` **每个主机帧调用一次**；`kPressFrames = 6`，
  一次轻点被拉长成 6 个 guest 帧。17 fps 下 6 帧 = **353 ms**。
- 更硬的约束：ROM 自己要求**连续 4 帧按下**才认键（1/2/3 帧实测无效）。
  所以即使把采样移到切片边界、消抖改成时间，**输入延迟的地板仍是 4 个 guest 帧**：
  17 fps 下 235 ms，60 fps 下 67 ms。
- 结论：**输入延迟的主因是 guest 帧率，不是采样。** 要真做到 <100 ms，只有两条路：
  1. 把 guest 跑到 ≥40 fps（R1/R2 或换硬件）；
  2. **patch ROM 的消抖常数 4 → 1–2**（ROM 在仓库里、可反汇编、可回验），
     宿主侧做 5–10 ms 时间去抖。这条最便宜，但属于“改游戏行为”，要先做 A/B。
- 另外两条低成本改动：
  - 采样移到 `run_guest_frame()` 的每个 slice 边界（0.5 ms），去掉“整帧量化”；
  - 轻点注入的 `kPressFrames` 从 6 降到 4（ROM 的最低要求），少 118 ms 的“粘键”感。
- **时间基**：设备没有 RTC，guest 时钟按 2 Hz 中断走；27% 实时率下，宠物时钟和成长
  都慢 3.7×。可以在宿主每秒把 `0x0CAF/0x0CAE/0x0CAD` 同步成真实时间（类似 PC 侧
  `--clock-sync`），让“时间”至少是对的。成长是否跟着加速取决于 ROM 用哪个计时，
  需要实测，**标为推测**。

### 3.8 R8：显示侧“看起来更顺”（实验性，0 吞吐收益）

- 现在 60 Hz 面板在 17 fps 下重复显示同一帧；观感是“跳”，不是“拖”。
- **帧插值**：在相邻两个 guest 帧之间做 50% alpha 混合，60 Hz 输出。成本约
  9,216 px × 2 读 + 1 写 ≈ 0.1–0.3 ms/帧，完全可承受。缺点是像素 ART 会糊/重影，
  对宠物小游戏可能反而更差；建议只做可选开关，实测观感。
- **按 guest 完成即显示**，不要为了“60 fps”在轻负载时硬等 16.7 ms；现在 pacer
  在轻负载会睡/自旋，重负载会自动落后，问题不大。
- **不要**用“跳帧/拉长帧预算/降低 kCpuClock”伪造 fps；那会让游戏时间变慢。

---

## 4. 硬件路线

### 4.1 候选表

| 候选 | CPU / 主频 | SRAM / 代码空间 | Cache | 相对 C3 单核 | 现有代码裸换 | + 软件改造 | 改板工作量 | 结论 |
|---|---|---|---|---|---|---|---|---|
| **ESP32-C3（现状）** | RV32IMC 单核 160 MHz | 400 KB（16 KB 给 cache） | 16 KB 只读 cache | 1.0× | 1.0× | 1.4–1.8× | — | 天花板 |
| **ESP32-S3** | Xtensa LX7 双核 240 MHz | 512 KB SRAM + 模组 PSRAM | I 16/32 KB、D 16/32/64 KB 可配 | 1.8–2.2×〔估算/基准〕 | 1.8–2.2× | 2.5–4× | 2–3 个月（QFN56，非 drop-in） | **最快出货的 2×** |
| **ESP32-P4** | RV32 双核 400 MHz + LP | 768 KB SRAM + 外部 PSRAM | L1 I 16 KB / D 64 KB + L2 128–512 KB | **2.87×**（CoreMark 折算） | 3.5–4.5×〔估算，含 L2〕 | **5–8×** | 3–6 个月；无射频，需 C6 | **唯一现实的 60 fps** |
| STM32H750/H743 | Cortex-M7 480 MHz | 1 MB RAM（含 TCM） | 16 KB I/D + TCM | 3–4×〔估算〕 | 3–4× | 5–7× | 3–4 个月（工具链/驱动全换） | 性能够，生态冷 |
| NXP i.MX RT1062 | Cortex-M7 600 MHz | 1 MB OCRAM + 512 KB TCM | 32 KB I/D + TCM | 4–5×〔估算〕 | 4–5× | 6–8× | 3–4 个月 | 性能最好，成本/生态差 |
| Milk-V Duo / Pi Zero 2 W | 1 GHz 级 + Linux | 64 MB–512 MB | 大 | 6–10× | **直接跑 MAME** | 100% 实时 | 2–3 个月；功耗/启动/体积 | 60 fps 的“核选项” |
| FPGA 软核 | — | — | — | — | — | — | 6 个月+ | 不做 |

数据来源与说明：

- C3：官方数据手册，CoreMark **483.27 @160 MHz = 3.02/MHz**；400 KB SRAM 中 16 KB 专用于 cache。
- P4：官方/Zephyr，双核 400 MHz、768 KB SRAM、L1+L2 cache、PPA、MIPI-DSI；CoreMark
  **6.92/MHz（双核）→ 单核 3.46/MHz**，换算 **400×3.46 ÷ (160×3.02) ≈ 2.87×**。
- S3：240/160 = 1.5× 只是主频；WLED 整数基准 C3 127 vs S3 238 MOPS ≈ **1.87×**；
  加上 32 KB I-cache 和 512 KB SRAM，取 **1.8–2.2×**。M5 Cardputer ADV 是 **ESP32-S3FN8，
  8 MB flash，无 PSRAM**，不能拿“PSRAM 整段放 ROM”当默认前提。
- H7/RT1062：只有主频和 TCM 的估算，没有本项目的实测；标〔估算〕。

### 4.2 为什么 P4 是“唯一现实的 60 fps”

- 单核 2.87× 是官方 CoreMark 折算，已把 IPC 算进去；不是纸面 400/160。
- **L2 128–256 KB 可以放 AOT 热代码**，把 673 KB 代码的 flash miss 从“几乎每次”
  压到“偶尔”；L1 I-cache 仍只有 16 KB，所以**必须把最热的 1–2 个区域钉进 L2/SRAM**，
  不能把 673 KB 全丢给 L2。
- 768 KB SRAM 中，除 L2 外还有 512 KB 左右可做代码/数据；比 C3 的“1.1 KB 余量”是数量级差异。
- 代价：P4 没有 Wi-Fi/BT，产品要加 ESP32-C6（SDIO）；显示从 SPI 换 MIPI-DSI/RGB；
  电源/PCB 全重做。**这是产品级决策，不是软件优化。**

### 4.3 S3 的定位

- 优点：IDF 生态、双核、32 KB I-cache、512 KB SRAM、模组便宜、Cardputer ADV 现成；
  代码是 C/C++，AOT 生成器输出 C，移植主要是板级驱动。
- 缺点：1.8–2.2× 裸换不足以 60 fps；无 PSRAM 的模组不能整段放 ROM；
  QFN56 与 C3 QFN32 不兼容，必须改板。
- 结论：**如果 2–3 个月内要“明显更顺”且不想赌 P4，S3 是保底；要 60 fps 不要选它。**

### 4.4 Linux 小板

- 直接跑 MAME 的 `digicolr`（现有 `ref/generalplus_gpce4.cpp` 就是 MAME 驱动），
  不需要移植模拟器，100% 实时、音频/存档/RTC 全有。
- 代价：启动 5–20 s、待机功耗、电池、外壳、成本；对“AI Passport”这种玩具形态是降维，
  但对“先让用户看到 60 fps”是最短路径。
- 若要保留 MCU 形态，P4 是折中；若要最快验证“游戏本身在 60 fps 下是什么样”，
  先用 Linux 小板做基线，再决定要不要为 P4 改板。

---

## 5. 组合预测（线性外推，实际受 cache/内存影响）

以“重绘帧 154k 指令、端到端 110 周期/指令、8.6 fps”为基线。表里的倍数是
**相对 C3 现状的端到端吞吐**，不是把“周期/指令”和“主频”混在一起乘：

| 方案 | 相对基线 | 重绘帧 fps | 中负载 fps | guest 实时率 | 备注 |
|---|---|---|---|---|---|
| C3 现状 | 1.0× | 8.6 | 17–22 | 27% | 基线 |
| C3 + R1/R2/R5 | 1.4–1.8× | 12–16 | 28–40 | 45–65% | 软件上限附近 |
| C3 + 极致 codegen | 2.0× | 17–18 | 35–45 | 60–75% | 需要 lazy flags/截断消除，风险高 |
| S3 + 同软件 | 2.5–4× | 22–30 | 45–60 | 75–100% | 1.8–2.2×（硬件）× 1.4–1.8×（软件） |
| P4 + 同软件 | 5–8× | 40–60 | 60 | 100% | 2.87×（硬件）× 1.4–1.8×（软件）+ L2 |
| H7/RT1062 + 同软件 | 5–8× | 45–60 | 60 | 100% | 性能够，生态冷 |
| Linux + MAME | 10×+ | 60 | 60 | 100% | 基线验证 |

〔推断〕：上表是“按相对吞吐缩放”，没有计入 cache 命中率变化；P4 的 L2 和 S3 的
32 KB I-cache 会让实际值好于线性，C3 的 IRAM 不足会让实际值差于线性。
**不要拿这张表当承诺，拿它当排序。**

---

## 6. 不建议的路线

| 路线 | 为什么不 |
|---|---|
| **JIT / dynarec** | 当前只有 1.1 KB DRAM 余量；即使按 §3.4 腾出 ~90 KB，24–32 KB 代码缓存也会和“热区 IRAM / token 数组”抢同一块 SRAM。还要关 `CONFIG_ESP_SYSTEM_MEMPROT` 放弃 W^X。同族公开实测（ESP32-S3 68000 dynarec）**78 周期/指令 vs 解释器 46**，翻译码 70–80 B/guest 指令挤不过 32 KB I-cache。C3 更差。 |
| **LLVM lifter / unSP→LLVM IR** | 与手写 C 发射器同量级（±20%），6–12 周；LLVM 在 16 位截断/延迟标志上不占优。参考实现（N64Recomp、llvm-mctoll）也都是自建发射器。 |
| **全程序 AOT 到 flash** | 673 KB → 1.5 MB+，I-cache 更差；没有 PSRAM 做 backing。正确做法是“只把最热 30–50 KB 做成区域/trace，其余留解释器”。 |
| **双核分核** | 显示只占 1–5%；分核最多省 5%，不值得为速度做。S3/P4 的双核价值在“把音频/显示移走、减少抖动”，不是吞吐。 |
| **超频 192 MHz** | 官方上限 160 MHz；PLL/flash 时序不稳，产品不可接受；收益 10–20%，不如 R1 的一个零头。 |
| **QIO 当主杠杆** | 先正确置 QE 位再 A/B；即使成功，flash 带宽也不是 110 周期/指令的主因。 |
| **跳帧/降 kCpuClock/拉长帧预算** | 伪造 fps，游戏时间变慢；项目已经用 `F` 旋钮证伪。 |
| **只优化 blit/显示** | `blit_pct` 0–1%，已经到极限。 |

---

## 7. 建议的执行顺序（若决定继续在 C3 上推进）

1. **P0（1 天）**：打开 `C3_SPLIT=1`，跑 30 s 中负载 + 重绘场景，拿到
   `caot/cinf/cfast/pd/pp` 真实占比；同时把 `aoti` 的口径钉死。
2. **P1（3–5 天）**：腾 RAM——页缓存 64 KB、census 8 KB、bench 16 KB；
   用 map 验证 IRAM/DRAM 段边界；目标 DRAM 余量 ≥ 64 KB。
3. **P2（1–2 周）**：R1 区域化 AOT，先只做 `0x207` 页 + `0x000` 页的 20–30 个块；
   PC 侧 20 s trace 100%、帧 hash、存档逐字节；再上机 A/B。
4. **P3（1 周）**：把最热区域函数放 IRAM；量 `caot/cfn` 和 fps。
5. **P4（1–2 周）**：R5 HLE 乘加库 + `render_band` 影子实现。
6. **P5（3–5 天）**：R7 输入采样/时间去抖/轻点注入 6→4；RTC 时间同步实验。
7. **P6（可选，2–3 周）**：R3 token 解释器，打 AOT 未覆盖的冷区。
8. **P7（可选，1 周）**：R8 帧插值，纯观感实验。

验收口径不变：逐指令 trace 100%、`FB_CRC`/帧 hash 一致、存档逐字节一致、
周期计数差 <0.01%；每次改动先在 PC 侧验证，再烧机。

---

## 8. 风险与不确定项

1. **R1 的区域边界语义**：中断只能在区域出口投递，可能改变 IRQ 相位；需要用
   “同一帧 IRQ 响应 PC 序列”对拍，不能只看最终帧。
2. **R2 trace 的内存别名**：guest 可能通过计算地址写到自己正在执行的代码页
   （本作 `smc 0`，但 RAM 里的代码副本存在）；trace 必须能在写命中时退出。
3. **R3 的 token 缓存一致性**：ROM 只读、RAM 代码副本可写；按页失效即可，
   但要有写屏障。
4. **P4 的 L2 配置**：L2 若被配成 PSRAM backing，收益全无；必须用内部 SRAM 做 L2，
   热代码钉在 L2/SRAM。P4 rev<v3 不支持 512 KB L2。
5. **S3 的 PSRAM 型号**：Cardputer ADV 是 S3FN8 无 PSRAM；要 PSRAM 得选别的模组/板。
6. **输入延迟的 ROM 消抖**：patch 4→1 会改变游戏手感，必须先做 A/B 和回归；
   不建议默认开启。
7. **所有“估算”数字**：本文标〔估算〕的倍数是按周期线性外推，不是实测；
   上线前必须用 60–100 块切片或整机 A/B 验证。

---

## 9. 参考来源

**芯片/官方**
- ESP32-C3 数据手册（CoreMark 483.27 @160 MHz、400 KB SRAM/16 KB cache）：
  <https://documentation.espressif.com/esp32-c3_datasheet_cn.html>
- ESP32-S3 数据手册：<https://documentation.espressif.com/esp32-s3_datasheet_cn.html>
- ESP32-P4 特性（双核 400 MHz、768 KB SRAM、L1+L2、PPA、MIPI-DSI）：
  <https://docs.zephyrproject.org/latest/boards/espressif/common/soc-esp32p4-features.html>
- ESP32-P4-Function-EV-Board（$55、7 寸屏、C6 协处理器）：
  <https://www.cnx-software.com/2024/08/07/esp32-p4-function-ev-board-development-board-launched-for-55-with-7-inch-display-and-camera-module/>
- ESP-IDF Memory Types（IRAM/flash/cache 语义）：
  <https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/api-guides/memory-types.html>
- M5 Cardputer ADV（ESP32-S3FN8、8 MB flash、无 PSRAM）：
  <https://docs.m5stack.com/zh_CN/core/Cardputer-Adv>

**同行实测**
- ESP32 Emu Turbo — Optimizations（IRAM 只帮会 miss 的代码；热区 32 KB；
  68000 dynarec 78 vs 解释器 46）：
  <https://pjcau.github.io/esp32-emu-turbo/docs/software/optimizations>
- ESP32 Emu Turbo — Emulators（S3 上各机种帧率、SNESticle 的 jump-table/IRAM 做法）：
  <https://pjcau.github.io/esp32-emu-turbo/docs/software/emulators>
- ESP32 Emu Turbo — JIT plan（代码缓存 24–32 KB 的实测门槛）：
  <https://pjcau.github.io/esp32-emu-turbo/docs/next-steps/jit-plan>

**本项目文档**
- `docs/PERF_2026-11-22.md`：110/63/424 周期、场景帧率、判负表
- `docs/ROUTE_2026-11-22.md`：A/B/C 线、按键延迟、S3/JIT 的旧判断
- `docs/HANDOFF_2026-11-22.md` §6/§7：13 条负结果、分组超块失败原因
- `docs/RESEARCH_UNSP_CODEGEN.md`：21–23 条 RV32/guest 指令、寄存器钉住/lazy flags 估算
- `docs/AUDIT_2026-11-23.md`：27% 实时率、AOT 命中率、park 点
- `docs/HANDOFF_2026-11-24.md`：v1.5.0 现状、硬件事实、坑表

---

## 10. 本轮核对清单（只读，没有改动固件）

| 项 | 实测/核对结果 |
|---|---|
| v1.6.0 `build.log` | DRAM 320,158 / 321,296（99.65%），剩 1,138 B |
| `digicore-c3.map` | `.iram0.text` 0x180ee = 98,542 B；`s_rom` 0x1407c = 82,044 B；`g_miss_cnt/g_miss_pc` 各 4 KiB；`bench_*` 共 16 KiB；`panel_fill_black` 10 KiB |
| `romstore.h` | `SLOTS=16`，`m_cache` 64 KiB；`m_base` 非空时 `read16()` 不读 `m_cache` |
| `rom_esp.cpp` | `esp_partition_mmap()` 成功后 `set_mapped()`；失败才回退页缓存 |
| `aot_blocks.c` | 1,466 块；块大小 1–122 条，平均 10.1；1–2 条的短块 354 个（24%） |
| `CMakeLists.txt` | `-DC3_SPLIT=0`（所以 `caot/cinf/cfn` 全 0）；`-fjump-tables` 已开；`AOT_NO_HOOKS` 已开 |
| `app_main.cpp` | `kPressFrames=6`；`update_buttons()` 每主机帧一次；`kFrameBudgetUs=200 ms` |
