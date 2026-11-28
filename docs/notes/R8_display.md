# R8 显示与合成通路 审计

角色：显示与合成通路审计员（只读，未改任何源码/配置/生成物，未跑 idf.py，未刷机，未开串口，未 git）。
唯一写入文件：本报告 `docs/notes/R8_display.md`。

## 0. 结论摘要（带优先级）

1. **DISP-01（P0）**：`display.c:31` 的 `LCD_PCLK_HZ = 80 MHz` 与 `display.c:27-30` 注释（明确说 80 会位移、应 40）及实测链路（39.8 Mbit/s，`app_main.cpp:396-402`）三方矛盾。真相是链路实际跑在 40 MHz 档；把常量改 40 不改变今天任何画面/时序，只消除「驱动将来真正兑现 80」时的位移回归风险。**本轮唯一一条零行为变化的 P0。**
2. **DISP-02（P1）**：帧率叠加层 `draw_fps_overlay` 默认常开且**未按 standby/menu 门控**（调用点 `app_main.cpp:3769` 在 `3565` 那个 `!s_standby && !menu_is_open()` 块之外，函数本体 `824-865` 只查 `s_display_ok`），与 `3563-3564` 注释「standby 期间 no fps overlay」矛盾。待机/菜单期间每秒在左上角画 24×12 黑底数字盒。默认关（`#if C3_FPS_OVERLAY`）省 576 B DRAM + 1 Hz SPI，但**会移除可见数字**，故归 P1（产品决定），R7_app B6 已提，本轮补上「未门控」这一处新证据。
3. **DISP-03（P1，需 pixel-diff 判定）**：dirty 位图是「写」图不是「变」图。`store_pixel/fill_run/write_run` 只置位、不比较新旧值（`st7735.cpp:100/306-317/379-390`）。稳态时钟屏实测 ~455 px/帧写入（R7_core §5，来源 `dbgtest/r5_after_force.log`）→ 1–2 个 16×16 dirty tile/帧 → 约 2–4 KB/帧 SPI。这要么是真实闪烁元素，要么是 guest 重写相同像素（「改而未变」的无用推送）。若属后者，值比较可让待机时钟屏真正零推送——这是显示侧最大的续航项，但改动在 HLE 热路径（R7 刚优化过每像素周期），须 A/B。
4. **DISP-04（P2）**：整窗 pack 的 2× 展开（每像素 1 load + byteswap + or/shift + 1 次 32-bit 写，`app_main.cpp:1102-1112`）+ 每行 `memcpy` 复制（`1110`）约 0.5 ms CPU，被 14.8 ms 的 SPI DMA 主导；LUT/双字写/提前算好行只能省约 0.3 ms 的整窗帧 ~2%，不值得。阈值 `kWholeWindowTiles=21` 无场景实测、基于 tile 计数而非字节/阻塞（`app_main.cpp:415`），band 相邻行也不合并——都归 P2。
5. **DISP-07（P2）**：fbsave 每次快照整块擦 32 KiB（8 扇区）只写 24,608 B（32 B 头 + 24,576 B），触发频率 = 自动存档周期（默认 60 s，首存 10 s，仅 resumable PC 且 RAM 变化时，`app_main.cpp:2708/2735-2751`）。题目里的「28,672 B 写入」是旧 112 行窗口的过时数（现 `kPanelWindowBytes=24,576 B`，`app_main.cpp:2616`）。只存可见 96×96 需 18,432 B 压缩缓冲（DRAM 不够）且省 6 KB/分，不值得。

其余必查项（阈值/粒度/repair/菜单阻塞/dirty_clear 时机）结论见 §2 表。

---

## 1. 范围与方法（读了哪些文件、跑了哪些只读命令）

**读的报告**：`docs/notes/R8_BRIEF.md`、`docs/AUDIT_PERF_2026-11-27_R7.md`、`docs/notes/R7_app.md`、`docs/notes/R7_core.md`、`docs/notes/R7_periph.md`。

**读的源码（firmware/digicore-c3/main/，全部只读）**：
- `app_main.cpp`：1–459（常量/s_strip/s_frame_buf/整窗阈值）、800–1199（fps overlay/blit_frame 1002–1188/pack/band）、1855–2009（panel_edge_test/panel_align_test/standby）、2540–2809（persist_save_image/fbsave 触发/快照/睡眠背光）、3305–3354（s_frame_buf 分配）、3440–3509（菜单 present 门控）、3550–3808（帧循环/repair/CRC watchdog/fps overlay 调用/STAT）。
- `emu/st7735.cpp`（全文）、`emu/st7735.h`、`emu/gpce4.h:28-104`（St7735 结构/m_dirty/dirty_clear/dirty_all）。
- `display.c`（全文）、`bezel.cpp`（全文）、`menu.cpp`（全文）、`fbsave.cpp`（全文）。

**只读检索（grep）**：`s_frame_buf` 分配点、`fbsave_store/persist_save_image/save_panel_frame` 调用链、`menu_present/menu_standby_present/menu_lapse_toast_present` 调用点、`s_blit_repaint_all` 置位点、`draw_fps_overlay` 调用点、`m_dirty/dirty_clear/dirty_all` 定义。

**未做**：未整读 `aot_blocks.c` / `*.bin` / `*.avi` / `build/` 大 log；未跑 `idf.py build`；未 nm/objdump（本报告不依赖符号尺寸，直接引用 R7 已量数据）。

---

## 2. 发现表

> 标注约定：**实测**（源码注释自述的历史实测 / R7 引用的真机日志）｜**静态推导**（我从源码算的）｜**推测**（需进一步验证）。行号均为当前工作树真实行号。

| ID | 位置(文件:行) | 现象/证据 | 影响机制 | 估算 | 与R7关系 | 建议改动 | 风险 | 验证 | 优先级 |
|---|---|---|---|---|---|---|---|---|---|
| DISP-01 | `display.c:27-31`（对照 `app_main.cpp:396-402`） | 注释「40 MHz, not 80: at 80 … horizontal displacement」，`#define LCD_PCLK_HZ (80*1000*1000)`；实测「39.8 Mbit/s … 40 MHz class」 | 常量请求 80，驱动实际钳到 40（实测）。今天无位移、画面不变；但常量与意图相反，一旦驱动/总线改动兑现 80，位移回归 | 0 画面变化；消除一次潜在回归 | R7_periph P-02（「二选一」） | 常量改 `(40*1000*1000)`，保留/微调注释（27-30 的 0.33/0.66ms 数字是旧小窗，顺手改对） | 极低（链路已在 40） | 改后 `nm`/重建 A/B 看 `ft` 不变；真机 T 图不变 | **P0** |
| DISP-02 | `app_main.cpp:824-865`（函数）、`3757-3769`（调用）、`3563-3564`（矛盾注释） | `draw_fps_overlay` 只在 `833-834` 查 `s_display_ok`；调用点在 `3565` 的 `!s_standby && !menu_is_open()` 块**之外**，故待机/菜单期间每秒仍推 24×12 盒 | 1 Hz 的 576 B SPI + 576 B memset + 每 1 s 一次 `esp_timer_get_time`（`3764-3765`）；待机/菜单壳层左上角出现黑底数字盒（壁纸区域，未被重绘清除） | 576 B DRAM（`box`，`831`）+ 1 Hz 推屏 + 待机期非零 SPI | R7_app B6（默认关），本轮补「未门控」证据 | `#if C3_FPS_OVERLAY`（默认 0，bench 置 1） | 低（移除可见调试数字属产品决定，故 P1） | bench 置 1 编译验证；默认 0 时真机确认待机壳干净 | P1 |
| DISP-03 | `emu/st7735.cpp:100,306-317,379-390`；`emu/gpce4.h:85-99` | dirty 位图只 `\|=` 置位，从不比较旧值；`m_dirty[ry>>4]` 16×16 tile 粒度 | 「写了就置脏」而非「变了才置脏」；稳态 ~455 px/帧（R7_core，r5_after_force.log）→ 1–2 tile/帧 → 每帧 ~2–4 KB SPI。若 guest 重写相同值，则属无用推送 | 时钟屏待机态约 117–233 KB/s 连续 SPI（静态推导） | R7_core R7-05 已做「写侧按行合并」，未做「值比较」 | 先 T/C 命令做 pixel-diff 判定「改而未变」是否发生；若成立，再评估 store/fill/write 加值比较（热路径） | 中（加比较 +1 cmp/px，抵消 R7-05 部分收益） | 宿主等价测试 fb/state/dirty 逐位一致 + 真机 CRC 差分 | P1 |
| DISP-04 | `app_main.cpp:1102-1112`（pack）、`415`（阈值）、`1124-1168`（band） | 整窗 pack：每像素 byteswap+or/shift+32-bit 写，每行 `memcpy` 复制（`1110`）；band 每脏 tile 行单独 `display_draw_bitmap`（`1162`），相邻行不合并 | pack CPU 在 14.8 ms DMA 前串行执行（约 0.5 ms）；阈值 21 无实测支撑 | pack ≈0.5 ms/整窗帧（静态推导，~3% 帧预算）；被 SPI 主导 | R7_periph 已证 async 整窗优于逐 band | 不动 pack（LUT/双字写省 <0.3 ms 不值得）；阈值留待 A/B | 低（改 pack 有回归面） | 若要动，`g_blit_cyc_per_px` 类反汇编 A/B | P2 |
| DISP-05 | `app_main.cpp:1014-1016,1051-1054,1063-1081` | 阈值 `kWholeWindowTiles=21`（36 tile 的 58%）；band 用 bminx/bmaxx（`1125-1130`）按行带最小宽，但**按 16 源行 tile 行**成带，相邻脏行不并成更高带 | 整窗=1 次 async 73,728 B；band=逐行同步 ≤73,728 B。阈值只看 tile 数不看字节/阻塞 | 阈值在「切换场景」边界处可能选错路径 | R7_app 记录了 bbox 更差（78%/39fps） | 保留；若要调，需按「带字节数 + 阻塞 vs async」重定阈值并真机 A/B | 低 | 真机切换帧 `ft`/`blitmax` A/B | P2 |
| DISP-06 | `app_main.cpp:1063-1081,1173-1174` | repair sweep：每 120 idle 帧推 6 band（每帧 1 band，`1064-1067`），`1173` 只成功才推进 | 静态屏唯一周期性整窗重发（安全网） | 73,728 B / ~126 帧 ≈ 33 KB/s（静态推导，可忽略） | R7_app D6 未涉及 sweep 本身 | 保留；可把 120→更大（如 600）降频，纯判断 | 低（删=失去自愈网） | 静态屏 `rows_sent` 观测 | P2 |
| DISP-07 | `fbsave.cpp:80-95`；`app_main.cpp:2616,2708,2735-2751` | 每次 `esp_partition_erase_range(kBase, 0x8000)` 擦 32 KiB，写 `32+24,576=24,608 B`；触发=自动存档（默认 60 s，首存 10 s），仅 `resumable`（PC 0x200000–0x400000）且 RAM CRC 变化 | 8 扇区擦 vs 实际只需 7 扇区（24,608 B）；题目「28,672 B」是旧 112 行窗，现 `kPanelWindowBytes=24,576 B` | ~1 次/min，每次 32 KiB 擦 + ~150 ms 阻塞（`app_main.cpp:2703-2705` 自述） | R7_periph P-16 只提了死导出 | 擦改 0x7000（7 扇区）；只存 96×96 需 18,432 B 压缩缓冲（DRAM 不够）→ 不做 | 低（分区布局已固定） | 改后 `fbsave_load` 回读校验 + 真机掉电恢复 | P2 |
| DISP-08 | `menu.cpp:718-776`（menu_present）、`bezel.cpp:164-195`（bezel_paint）、`app_main.cpp:3470-3475`（菜单关闭） | menu_present 全 240 行×320 宽 10 band=153,600 B（含卡片外 48 行纯黑 backdrop）；bezel_paint 15 band=153,600 B 阻塞；菜单关闭帧 = bezel_paint(30.9 ms) + repaint_all 整窗(pack 0.5 ms + async) | 菜单 open/key 每次 30.9 ms 阻塞；菜单 close 约 31 ms 阻塞；lapse toast 每帧 10,560 B（`app_main.cpp:3578-3579`） | 30.9 ms/次 × 菜单交互次数（事件型，非每帧） | R7_periph §4 已算 bezel 30.9 ms | 保留现算（153,600 B 放不进 DRAM，R7 已结论）；可选菜单只推卡片区 192 行省 6 ms | 中（改菜单绘制有越界前科） | 真机 `N` 出图 + 菜单往返 `ft` | P2 |
| DISP-09 | `app_main.cpp:1169-1187`（dirty_clear 时机） | `dirty_clear()` 仅在 `err==ESP_OK` 后执行（`1178`）；失败保留位、`s_have_prev=false` 下帧重试（`1180`） | 正确：失败不丢脏、成功才清；repair 推进也只在成功后（`1173`） | 0 | — | **不动**（已正确） | 无 | — | — |

---

## 3. 每帧 SPI 字节/时间账 + CPU pack 账（必查 1/3/8 汇总）

链路实测 **39.8 Mbit/s ≈ 4.975 MB/s**（`app_main.cpp:396-402`，源码自述三版实测）。所有时间按此折算，非 80 MHz。

| 场景 | 字节 | @39.8 Mbit/s | 频率 | 性质 |
|---|---|---|---|---|
| 整窗重绘（192×192×2） | 73,728 B | 14.82 ms | 重绘帧（≥21 tile） | async：DMA 与下帧 guest 重叠，下帧首次 push 的 `display_wait_idle()` 才 drain |
| 满宽 band（6 tile × 16 源行） | 12,288 B | 2.47 ms | band 路径 | 阻塞（`display_draw_bitmap` 同步） |
| 单 dirty tile（16×16→32×32） | 2,048 B | 0.41 ms | 局部动画 | 阻塞 |
| 时钟屏稳态（~455 px/帧） | ~2–4 KB | 0.4–0.8 ms | 每帧 | 静态推导；DISP-03 |
| 真·静态（guest 零写，前 119 帧） | 0 B | 0 | 每帧 | `blit_frame:1069` 早退 |
| repair sweep | 12,288×6 B | 2.47×6 ms | ~每 126 帧 | 安全网，分 6 帧 |
| fps overlay（24×12） | 576 B | 0.116 ms | 1 Hz | 未门控（DISP-02） |
| bezel_paint（320×240） | 153,600 B | 30.9 ms | 事件 3–5 次/上电 | 阻塞（R7_periph §4 同） |
| menu_present（320×240） | 153,600 B | 30.9 ms | 菜单脏时 | 阻塞 |
| standby card（192×192） | 73,728 B | 14.8 ms | 进入待机 1 次 | 阻塞 |
| lapse toast（240×22） | 10,560 B | 2.1 ms | 显示期每帧 | 阻塞 |

**CPU pack 账（整窗帧，160 MHz，静态推导）**：9,216 px × (1 load + byteswap ≈4 op + or/shift + 1×32-bit store) ≈ 9,216×8 ≈ 74K + 96 次 `memcpy(384 B)`（≈9,216 字拷贝）≈ 15K ≈ **约 0.5 ms**。落在 14.8 ms DMA 之前（`1100-1116` 串行），故整窗帧 CPU 成本 ~0.5 ms 计，SPI 是绝对瓶颈。

**静态零推送的代码路径证明（必查 8）**：guest 像素写 → `st7735.cpp` 置 `m_dirty`（`100/314/387`）→ `blit_frame` 读 `rowmask`（`1039-1044`）→ `any=false` 且 `s_repair_ty<0` → `++s_idle_frames<120 → return`（`1069`）→ 零 SPI。**机制成立**；但稳态 guest 仍 ~455 px/帧写入（非零），故「真零推送」仅当 guest 完全不写时成立。真正的「每帧无用合成」候选 = 若这 455 px 是重写相同值（DISP-03 待判）；其余每帧固定开销（D1–D4 计数器、CRC watchdog 30 帧一次 32 KiB CRC `app_main.cpp:3611-3620`、`audio_set_buzzer` 每帧）量级均 <0.1% 帧预算，R7 已列。

---

## 3. 需要主审裁决的冲突/不确定项

1. **DISP-03「改而未变」是否成立**：需用 `T`/`C` 命令（`app_main.cpp:1348-1353/1625-1633`）在稳态时钟屏做 CRC/pixel-diff，判定 ~455 px/帧是真实闪烁还是同值重写。这决定显示侧最大续航项（值比较）是否值得做、以及是否抵消 R7-05 的每像素收益。
2. **DISP-02 fps overlay 去留**：R7_app B6 建议默认关；本轮补「未按 standby/menu 门控」这条与注释矛盾的新证据。删/关会移除可见数字（产品决定），或至少补 `!s_standby && !menu_is_open()` 门控让待机/菜单期间不推。两条路不同，请主审定。
3. **DISP-01 常量 80→40**：我判断改常量（而非删注释）是对的——注释是正确意图、常量是错。但请主审确认「驱动确实钳 40」这条前提（R7_periph P-02 已给实测 39.8 Mbit/s 证据），避免改完反被某版驱动真正跑 80 而变慢（其实变快，但注释说 80 会位移）。
4. **repair sweep 周期 120 是否加长**：纯安全网，成本 ~33 KB/s 可忽略；是否从 120 拉到更大属策略，我建议不动（P2）。

---

## 4. 明确不动的清单（防误删）

| 对象 | 理由 |
|---|---|
| `blit_frame` 的 dirty-tile 逻辑与 `dirty_clear` 成功后才清（`app_main.cpp:1169-1187`） | 失败不丢脏、成功才清、repair 成功才推进——已正确，动它即引入丢帧/漏推 |
| `s_strip`=15,360 B 取两消费者 max、`s_frame_buf`=73,728 B 堆且分配失败退回 band 路径（`app_main.cpp:365-388,1108,3319`） | 刻意设计，R7_app §3-5 已结论；4 字节对齐已满足 uint32_t 写 |
| 整窗 async 推（`app_main.cpp:1094-1122` + `display.c:264-267`） | R7_periph §5 实测 blitmax 1980-2462→290 µs；退成逐 band 会回退 |
| `display_wait_idle` 丢弃陈旧信号量（`display.c:225-227`） | 二进制信号量语义必需，删了把上次完成误判成本次 |
| `bezel_paint` 现算几何/配色（`bezel.cpp:14-65,164-195`） | 对零售机照片采样，153,600 B 放不进 DRAM（R7_periph §4 已结论） |
| `fbsave_load` 两遍流式读 + CRC（`fbsave.cpp:124-147`） | 掉电撕裂检测唯一手段，DRAM 无第二块缓冲（R7_periph §2.7） |
| `panel_edge_test`/`panel_align_test`（`app_main.cpp:1874-1948`） | 几何标定唯一手段（R7_app B8 已结论） |
| 每帧末 ≤1.5 ms 忙等（`app_main.cpp:3690-3698`） | 帧对齐，R7_app D6 已结论不动 |

---

*报告完毕。本轮未修改任何源码/配置/生成物，未运行 idf.py，未刷机，未开串口，未 git 操作；唯一写入文件是本报告。*
