# 提示词：把 digimon-color-passport v1.16.1 的改动搬到 Cardputer ADV 移植版

> 用法：整段交给 Cardputer ADV 侧的 agent。它看不到这篇对话，所以本文自带全部前提。
> 基准仓库：`C:\Users\DuWeisi\digimon-color-passport`，HEAD `dd8e9a5`，固件 **v1.16.1**。

---

## 0. 你的角色与边界

你是 Cardputer ADV（ESP32-S3FN8）移植版的执行者。你手上已经有一份移植调研：
`docs/RESEARCH_CARDPUTER_ADV_PORT_2026-11-26.md`（路线、工作量、风险都在里面，**先读它**）。
本文只补充它写完之后、原仓库又改掉的四件事（v1.13 → v1.16.1），以及这些东西**哪些能照搬、哪些必须重算**。

硬边界：

1. **只读**基准仓库。不要修改 `digimon-color-passport` 里任何文件；所有改动落在 Cardputer 侧工程。
2. `emu/gpce4.*`、`emu/isa/*`、`emu/unsp_glue.cpp`、`emu/st7735.cpp`、`emu/aot.h`、`emu/compat/unsp.h` 是**与 CPU/板子无关**的：
   直接按 v1.16.1 的版本拿过去，**不要退回 v1.12 的副本**（调研文档写于 v1.12，说这堆“零改动”，现在它们内部有改动）。
3. 不要重跑 `tools/sync_c3_emu.py`：它会用 PC 侧副本覆盖设备侧 `unsp_glue.cpp`。

---

## 1. 先读这四个文件（按顺序）

| # | 文件 | 你要拿走的东西 |
|---|---|---|
| 1 | `docs/RESEARCH_CARDPUTER_ADV_PORT_2026-11-26.md` | 目标硬件事实、路线 A（ROM 进 `storage`）、S0–S4 阶段、风险清单 |
| 2 | `docs/AUDIT_PERF_2026-11-26_R6B.md` | v1.15.0 的整窗异步推送、HLE 重构、IRAM 纠错，以及**三处口径纠正** |
| 3 | `docs/STANDBY_LAPSE_2026-11-27.md` | v1.16.0/1.16.1 的待机卡与快进键改动，含 1.16.1 修正段 |
| 4 | `docs/notes/R6B_{aot,display,guest}.md` | 三份分报告：AOT 预算表、IDF 源码逐行结论、guest 侧 4 帧切换的实测分解 |

读完后先回答自己一个问题：**这次移植的性能目标是什么？** 答案是「把 `/120` 拉进 60 fps」
（`/120` = 800,000 guest cycles/帧，60 fps 时 = 48 M guest cycles/s ≈ 真机速度），
而**不是**「帧率更高」——pacer 写死 60 Hz，超过没有意义。

---

## 2. 目标平台事实（决定所有数字重算）

| 项 | C3（原） | S3 / Cardputer ADV（你） | 影响 |
|---|---|---|---|
| CPU | 单核 RV32IMC 160 MHz | 双核 LX7 240 MHz | 主频 1.5×、整数吞吐 1.87× |
| ICache | **16 KB 硬连线** | **16/32 KB 可配，默认 16** | 必须显式 `CONFIG_ESP32S3_INSTRUCTION_CACHE_32KB=y`，否则白换芯片 |
| 可用 DRAM | 393 KB | **321 KB（比 C3 少）** | 别照抄 73,728 B 缓冲这类大手笔 |
| 画面 | 96×96 → **2×** = 192×192 @ (64,24) | 96×96 → **1:1** = 96×96 @ (72,19) | 每帧字节数 **1/4**；字要重新定倍数 |
| SPI | 实测 **39.8 Mbit/s**（配置写 80，板子上是 GPIO matrix 40 MHz 档） | BSP 就是 40 MHz | 整窗推送 **18,432 B ≈ 3.7 ms**（不是 14.4 ms） |
| PSRAM | 无 | 无 | 同上 |
| 输入 | 三键 ADC 电阻梯 | TCA8418 键盘 | 按键改动要重新映射（调研建议 Q/W/E） |

> 一个容易踩的类比：原仓库把「80 MHz 是假的」写进了结论，那是 **GPIO matrix 40 MHz 上限**，S3 同样受这条约束。
> 想在 S3 上跑 80 MHz 必须走 IOMUX 引脚并实测，别默认它能跑。

---

## 3. 必须移植的五项（按优先级；每项都给了移植点、平台差异、判据）

### 3.1 帧归因（先做这个，因为它决定后面所有判断）

- **移植点**：`main/emu/unsp_glue.cpp` 的 `C3_HLEPROF` 块（`g_h_band/blit/push/fill/mul` + `HLE_T0/HLE_ADD` 宏，五个钩子调用点各包一层）；
  `main/app_main.cpp` 的 `LONGF` 行（`hp=band/blit/push/fill/mul`、`ppx=records/blitpx/pushpx`）与 STAT 的 `win=`/`pk=`。
- **平台差异**：无。`esp_cpu_get_cycle_count()` 两平台都有。开销每钩子一次读计数器（钩子按行/按精灵进入，不是按像素）。
- **判据**：`J 1` 打开 STAT 后，长帧（`LONGF`）能给出该帧 host cycles 的五项分解；`win=`/`pk=` 有数。

### 3.2 HLE 的 per-pixel 改造（纯赚，先上）

- **移植点**：
  - `main/emu/st7735.cpp` 的 `St7735::write_run()`（新的 span 写入，等价于逐字节 `data_w()`）；
  - `main/emu/gpce4.h` 的 `lcd_push_pixels()` + `Gpce4::spi2_txstatus` 收尾；
  - `main/emu/compat/unsp.h` 的 `m_lcd_push` 回调；
  - `main/emu/unsp_glue.cpp` 的 `hle_push_pixels()`（整段落地）、`hle_blit_key()` / `hle_render_band()`（源数据走页窗口 + 行缓冲走扁平 RAM）；
  - `main/app_main.cpp` 的 `lcd_push_hook()` 与 `guest_init()` 里的挂接。
- **为什么值得搬**：原实现每像素两次 `write16(0x3092)`，每次都要过 I/O 地址解码 + SPI2 FIFO 记账 + 命令 switch。
  改成「一行 96 像素一次调用」后这一段几乎没有宿主开销。S3 上相对收益比 C3 小，但它**不占预算、不改行为**。
- **必须做的验证**：`firmware/digicore-c3/hosttest/st7735_run_test.cpp` 是纯宿主测试（不需要板子）。
  同一串像素分别走「两次 `data_w()`」和 `write_run()`，比对 `fb[]`、`dirty[]`、`posx/posy`、`pixellatch`、`commandstep`、`px_writes`。
  5 个用例（MV=1 / MV=0 旋转 / 96×96 整屏 / 裁剪 / 列窗口）必须**逐位相同**。搬过去先跑它。

### 3.3 整窗一次异步推送（原仓库的主菜，你这边要重算尺寸）

- **背景结论（与板子无关，是 IDF 行为）**：`esp_lcd_panel_io_tx_color()` 的第一条有效语句是
  `spi_device_acquire_bus()`，而 bus lock 要求**本设备在飞事务清空**才授予（IDF 5.5.3
  `esp_lcd_panel_io_spi.c:323`、`spi_bus_lock.c:102-108/723-748`）。因此**带级双缓冲注定为 0**：
  第二条带永远等第一条发完。唯一能重叠的形状是**一帧只发一次 `tx_color`**——io 层会按 32 KiB 自己切块、
  非末笔置 `CS_KEEP_ACTIVE`、只在末笔回调。
- **移植点**：`display.c/.h` 的 `display_push()` / `display_draw_bitmap()` / `display_draw_bitmap_async()` /
  `display_flush()` 四层拆分（含二值信号量「取完再补一次零超时 take」的陈旧 give 丢弃），
  以及 `app_main.cpp` 里 `blit_frame()` 的整窗分支（`kWholeWindowTiles` 阈值 + 打包 + 异步推送 + `win=/pk=` 计数）。
- **平台差异（关键）**：
  - 整窗只有 **18,432 B = 3.7 ms**（C3 是 73,728 B / 14.4 ms）。缓冲按 **96×96×2 = 18,432 B** 申请，不要照抄 73,728。
  - 在 `/120` 的 8.3 ms 帧预算里 3.7 ms 仍占 **45%**，所以异步依然值得；但**先把同步版跑通再上异步**，别一次改两件事。
  - S3 是双核：调研 §5.2 建议把刷屏整个提到 core1。**先按原仓库的单核异步做**（改动小、可对照），
    把「blitter 独立任务」留作 S3 阶段的第二选择，两者不要同时上。
  - 局部帧继续走原来的逐带/逐块路径；`kWholeWindowTiles` 阈值按脏块占比自行重标（原值 21/36，即「超过一半」）。
- **判据**：STAT 的 `blitmax` 从毫秒级掉到百微秒级（异步只计排队时间）、`win=` 递增、`pk=` 在几百微秒；
  整窗帧的帧间隔比同步版短。

### 3.4 待机卡 STANDBY 与快进键（行为照搬，几何与键位重算）

**行为契约（必须一模一样）**：

1. ROM 在 16 s 无输入时请求 System_Sleep；帧循环继续拒绝（`set_sleep_inhibit`），由宿主**画出**这个状态：
   **只覆盖游戏窗口**（边框/侧栏/电量区域不动），窗口内黑底 + 居中白字 `STANDBY`。
2. **任意键唤醒，唤醒那一下不转发给 guest**（否则会顺带按到游戏菜单）。唤醒只重推窗口，不重画外壳。
3. 背光：`AUTO OFF` 仍然管背光，但**不早于「卡片出现 + 3 s」**（`max(AUTO OFF, 16+3)`），
   否则默认 `AUTO OFF = 16S` 会让卡片和熄屏落在同一帧、永远看不见。`NEVER` 一直亮到按键。
4. 快进：**提示条上 C = 停止快进**，按键被吞掉；B 不做任何事（原仓库 v1.16.0 曾让 B 只收提示条，用户明确否掉）。
   卡片关闭后由提示条接管；提示条出现时先「等手松开」（`s_lapse_hold_off`）才吃键。

**移植点**：`main/menu.cpp` 的 `menu_standby_present(strip, x, y, w, h, text)`（1x 位图 + 放大扫描 + 24 行一条带推送）、
`main/app_main.cpp` 的 `s_standby` / `standby_enter()` / `standby_leave()` / `cancel_time_lapse()` / `drain_injector()`，
以及 `update_buttons()` 里三个模态分支的先后顺序（**待机 → 提示条 → 设置卡**）。

**几何重算（照抄会溢出）**：原卡片在 192 px 窗口里用 4 倍字模（168×28）。你的窗口只有 **96 px**，`STANDBY` 1x 宽 42 px：
`scale = floor(96 / 42) = 2` → **84×14**，居中。把这个倍数写成「由窗口宽算出来」而不是硬编码 4。

**键位重算**：`BTN_KEY2`（C）在 Cardputer 上是调研建议的 Q/W/E 之一，映射到你选定的「取消」键；模态分支顺序不能变。

### 3.5 IRAM 名单（**不要照抄**，只搬规则）

原仓库 v1.15.0 有一版把 67 个块从 IRAM 删掉，理由是「HLE 钩子已经吸收了这些循环」——**实测被打回**：

- 钩子在**每个 slice 边界都会 bail**（预算不够一次计费就返回 false），实测 band 钩子 **1,022 次/秒**；
  bail 之后 guest 自己跑这些循环体，于是它们必须留在 IRAM。
- 删除后：重绘帧 host cycles/dispatch **1,650 → 2,200**，最坏单帧 **153 ms**；装回去后 **1,107**、最坏 **59–78 ms**。

**规则**：`0x207CA3-0x207CC4`（键控 blit 循环体）、`0x005B1E-0x005B2E`（填充）、`0x005B4A-0x005B5C`（推送）、
`0x005F8B-0x005F9A`（乘法）、`0x207A3E-0x207D0F`（render_band 记录体）**必须常驻 IRAM**；
其余按**重绘帧 block census** 决定（控制台 `R`…`Y` 那条链路，或用本项目 `tools/r6b/` 的取证脚本）。
**不要**按 guest cycles 排优先级（`AotEnt.cyc` 是死字段），也不要拿 `riram/rflash` 判断物理放置（那三个字段按 guest PC 分桶）。

---

## 4. 三条已经踩过的口径纠错（别再踩）

1. **`mi_s` 不是 AOT 吞吐。** 它 94.8% 来自 wait-loop 折叠的记账（285,611 pass/s × 74）。
   可比指标是 **host cycles / block dispatch**（重绘帧 1,724 → 修好后 1,107；全命中理论 253）。
2. **`riram/rflash/rdraw` 按 guest PC 区间分桶，不是物理放置。** 判物理位置只能看 `nm`。
3. **帧预算/拆帧治不了「慢动作」。** 动画墙钟时长 = 需要的 guest cycles ÷ 每秒 guest cycles；
   吞吐才是唯一杠杆。而且固件用 `[0x0850]` 注入 64 Hz tick、**一个宿主帧 = 一个 ROM 帧体**，
   所以「每帧宿主耗时」直接决定动画速度——这就是为什么 `/120` 进 60 fps 才是目标。

---

## 5. 不要做的事

- 不要为「半透明 STANDBY 遮罩」写逐帧混合：原仓库实测菜单那套 `blend565()`（每像素三乘 + 掩码）
  渲染 320×240 要 **78 ms**；若要做，只能用移位（α=50% 时 `(c >> 1) & 0x7BEF`，36,864 像素约 1.4 ms）
  或 32 项 LUT，并且**只做一次性遮罩**（稳态零代价）。这条用户已明确「先不做」。
- 不要照抄 73,728 B 的整窗缓冲、4 倍字模、`21/36` 脏块阈值——这三个数都是按 192 px 窗口推出来的。
- 不要把 `C3_BLIT_CYC=139` 当成要重新标定的东西：它是 **guest 侧计费**（每像素虚拟周期），与宿主平台无关，保持 139。
  已知它比实测均值高 11%（键色像素只需 38），但方向是让 guest 时钟偏快、动画偏短，与「慢」的抱怨相反，别顺手改。
- 不要试图用 SD 供 ROM（调研 §4B 已否决：页缓存会被 193 KiB 代码冲穿，掉到 0.7 M instr/s）。
- 不要一上来就动 pacer（写死 `kFrameUs = 16667`）和 60 fps 上限。

---

## 6. 验收协议（照抄原仓库的口径，不新造指标）

上机后按这个顺序，**每一行都要留下日志**：

```
V                    -> 版本、窗口几何（应显示 96x96 scale=1 左/上坐标）
J 1                  -> STAT，1 秒一行
# 1) 基线：同场景（时钟界面）跑 12 s，记录 guest_fps / gmax / ft p50,p95 / blit_pct
# 2) 整窗：构造一次整屏重绘，看 win= 是否 +1、pk= 是否几百微秒、blitmax 是否掉到百微秒级
# 3) 长帧归因：任何 LONGF 行给出 hp=band/blit/push/fill/mul，五项之和 + dtick 应能对上 us
# 4) 待机：静置 16 s -> 'standby up ... (card 96x96 at ...)'；按键 -> 'standby off after ... ms'，且该帧不出长帧
# 5) 快进：卡片里设倍速 -> C 退出（卡片必须关、倍速保持）-> 提示条出现 -> 再按 C 才停
P 0 / P 1            -> push 钩子现场 A/B（同一画面比较 hp=push）
X 0 / X 1            -> render_band 钩子现场 A/B（比较 hp=band 与 ft）
```

**通过判据**：

| 判据 | 期望 |
|---|---|
| `/120` 重绘场景帧间隔 p95 | ≤ 16.7 ms（这是整次移植的性能目标） |
| 稳定态 | `ft` p50/p95 = 16/16，`gmax` < 12 ms |
| `win=`/`pk=` | 整窗路径确实被走到，`pk` 与 18,432 B 的量级相符 |
| 待机 | 卡片只覆盖窗口；唤醒帧不出现 `LONGF` |
| 快进 | 卡片里 C 只关卡片；提示条上 C 才停；B 无动作 |

---

## 7. 交付物与汇报格式

1. **代码**：上述移植点在你工程里的 diff（不要散在多个分支）。
2. **宿主测试**：`st7735_run_test` 的 5 个用例输出（`all cases match`）。
3. **真机日志**：§6 每一行的原始抓取，文件名带日期与版本。
4. **报告**：一页纸，分「实测 / 预期 / 未验证」三栏写，**数字必须标来源**（本仓库实测 / 你的实测 / 推算）。
   明确写出你对 `RESEARCH_CARDPUTER_ADV_PORT_2026-11-26.md` 里 S0 那条「1.9× 还是 2.2×」的实测答案。
5. **不要**改基准仓库，也不要把它当工作区；需要引用代码就用路径 + commit id（当前 `dd8e9a5` / v1.16.1）。

---

## 8. 一句话摘要（给排期的人看）

这次搬的是三样东西：**① 别让 SPI 阻塞和 guest 抢同一段时间**（整窗一次异步推，IDF 的 bus lock 决定了这是唯一可行形状）；
**② HLE 每像素的记账要按段收敛**（push 走 span、源数据走页窗口，附宿主等价测试）；
**③ 模态层不许抢下层的键**（待机吞键、提示条等手松开、卡片里 C 只关卡片）。
IRAM 名单与所有尺寸常量都要按 S3/240×135/96×96 重算，不要照抄。
