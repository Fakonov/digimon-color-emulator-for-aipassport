# R8 交叉互审：display / app-persist / build-aot

互审对象：`docs/notes/R8_display.md`、`docs/notes/R8_app_persist.md`、`docs/notes/R8_build_aot.md`
（共 13 条 P0/P1）。基线：v1.18.0 HEAD `741a49e`。
只读互审：未改任何源码/配置/生成物，未跑 `idf.py`，未刷机/开串口，未 git 写操作。唯一写入本文件。

**工作树状态（必须知道）**：主审在我审的这段时间正在改，`app_main.cpp`(20:15)、`bezel.cpp`(20:17)、
`font5x7.inc`(20:17) 有未提交改动，`build/digicore-c3.elf` 已于 20:19 重编。三条 P0/P1 已经落地：
fps overlay 门控（`app_main.cpp:3787`）、lapse toast 事件化（`app_main.cpp:3585-3595`）、字体去重
（`bezel.cpp:32` + `font5x7.inc:3`）。**这三条不要再改一遍**，但落地版本有缺口，见 AP-03。

---

## 0. 结论摘要

1. **DISP-01 驳回。** "驱动实际钳到 40" 是错的：IDF 5.5.3 对 80 MHz 请求走 `clk_equ_sysclk`
   （`spi_ll.h:757-770`，`hz > 3/4×fapb`），寄存器配的就是 80 MHz，没有任何钳位代码。
   39.8 Mbit/s 是吞吐反推，2× 差额至今无解释（R6B §6.3 自己承认）。"零行为变化"不成立，
   该动的是注释，不是常量。
2. **AP-03 现象成立、方案不完整。** HEAD 每帧 10,560 B 属实；但 toast 只在**文本变化**时推，
   会被菜单卡和电池角永久覆盖且不会补画（见 §3.7）。这是本轮最值得做的一条，但要先补失效钩子。
3. **R8B-02 的 `uint8_t n` 会打坏两页。** 我复算：76 页里 `0x2022`、`0x2040` 跨度正好 **256**，
   `n` 截成 0 → 这两页 41+167=**208 块（10.7%）全 miss**。改 `base+last` 或 uint16 即可。
4. **R8B-10 的 "heap 余 87,192 B" 会误导人。** `s_frame_buf` 启动时 `heap_caps_malloc` 73,728 B
   （`app_main.cpp:3319`），真实运行时余量约 **13.4 KB**，不是 87 KB。
5. 裁决：**accept 2 / downgrade 10 / reject 1**。另有 4 条三份报告都漏的项（§5），
   其中"生成器没接进构建"会让 R8B-02/03 的 A/B 直接编到旧块体。

---

## 1. 裁决表（13 条 P0/P1）

| ID | 原优先级 | 裁决 | 一句理由 |
|---|---|---|---|
| DISP-01 | P0 | **reject** | 不存在"钳到 40"的机制，寄存器就是 80 MHz（`spi_ll.h:757-770`）；若线上真是 80，把常量改 40 会让整窗传输 7.37→14.75 ms，"零行为变化"不成立。只应改注释。 |
| DISP-02 | P1 | **accept** | 真实：全文无 `C3_FPS_OVERLAY` 宏（grep 0 命中），`draw_fps_overlay` 画 24×12@(2,2)（`app_main.cpp:824-865`），HEAD 调用点在 `!s_standby && !menu_is_open()` 块外；工作树已补门控（`3787`），正是正确修法。收益应按"画面污染"记，别按省电记（576 B + 1 Hz×~0.1 ms）。 |
| DISP-03 | P1 | **downgrade** | 机理真实（`st7735.cpp:99-100`、`290-317` 只置位不比较），但验证方法不成立：报告引的 `1348-1353/1625-1633` 是 `Q`/`G` 命令，`C`（fb CRC）在 `1607`；且 CRC 差分判不出"改而未变"。先加一个同值写计数器再谈值比较。降 P2。 |
| AP-01 | P0 | **downgrade** | 真实（HEAD `menu.cpp:36`+`bezel.cpp:30` 各 include 一次 `static const`），但去重省的是 **295 B 不是 590 B**（590 是两份总占用），flash 还余 963 KB，不配 P0；且工作树已落地（`bezel.cpp:32`/`font5x7.inc:3`）。降 P2。 |
| AP-02 | P1 | **downgrade** | 结论正确且确实纠正了 BRIEF 的错误前提（`persist_save_image` 由 `overlay_writes`+1 s 静置触发，`app_main.cpp:2569-2589`；60 s 链只有 gueststate+fbsave，`2708-2709`），但这条没有任何可执行改动，按 BRIEF 定义属"只报告"。降 P2。 |
| AP-03 | P1 | **downgrade** | 现象成立（HEAD `3579` 每帧 10,560 B≈2.1 ms@40MHz），但风险没写全：toast 只随文本变化推，菜单卡/电池角盖掉后不会补画（§3.7）。方案须补失效钩子才可收。 |
| AP-04 | P1 | **downgrade** | 日志行真实，但把 32 KiB CRC 当"诊断开销、release 该关"是错的：它是防冻结看门狗的比较基准（`3643-3669` 用它踢 `[0x0851]`），关掉会把 `0x5217` 停摆 bug 放回来。只能把 `ESP_LOGI` 降 DEBUG，收益 ≈0.1%。降 P2。 |
| AP-05 | P1 | **downgrade** | 真实（`menu.cpp:871` 恒 `return false`，死分支 `2355-2384`，5 B 死状态），但收益是每帧约 2 次跨 TU 调用+5 B，属死代码卫生；调用点是 6 处不是 5 处（`1431/2132/2355/2800/3454/3565`）。降 P2。 |
| R8B-01 | P1 | **downgrade** | 数字我复算一致（76×256×8=155,648，used 1,944，empty 140,096），但它本身没有改动，改动在 R8B-02；两行算同一笔成绩会把 P1 数虚增。降 P2。 |
| R8B-02 | P1 | **downgrade** | 方向对（省 64,632 B，`AotPageDesc` 的 2 B padding 确实够装 base+n），但 `uint8_t n` 装不下跨度 256 的两页（`0x2022`/`0x2040`，共 208 块）→ 必须改存 `base`+`last` 或 uint16，并在生成器里断言 span≤256。 |
| R8B-03 | P1 | **accept** | 复算一致（fully_shadowed=1097，277,946−35,104=242,842）；风险与 census 前提都写了。附加前提见 §5-M1：生成器还没接进构建。 |
| R8B-10 | P0 | **downgrade** | 纯账本复核（1,134,032 B / 963,120 B / 234,104 / 87,192），没有改动，不配 P0；而且 87,192 B 是名义值，`s_frame_buf` 已吃掉 73,728 B。降 P2 并改口径。 |
| R8B-11 | P1 | **downgrade** | 磁盘数字基本对（391.6 MB / 2.02 GB / 11.2 MB / 2.76 MB），但两处硬错：`.gitignore` 是 **UTF-8**（第 1 行中文直接可读），不是 GBK；`tmp/` 里有 `make_font.py`（4,144 B，`font5x7.inc:2` 指定它就是字体生成器）和 433 个 `.py`/54 个 `.md`，不能整目录删。 |

---

## 2. 方法与取证（可复算）

只读命令，全部在本机跑过：

```powershell
# 行号漂移核对：HEAD 对照工作树
git show HEAD:firmware/digicore-c3/main/app_main.cpp | Select-String 'menu_lapse_toast_present'   # 3579
git show HEAD:firmware/digicore-c3/main/bezel.cpp      | Select-String 'font5x7'                  # 30
# 字体符号（当前 ELF 已是去重后的版本）
riscv32-esp-elf-nm -S --radix=d build\digicore-c3.elf | Select-String 'kFont5x7|kFont3x5'        # kFont5x7 D 295 ×1
# AOT 复算（%TEMP%\r8v_spans.py / r8v_shadow.py，脚本只读 aot_blocks.c）
#   输出：pages 76  span_sum 11377  span>255: 2 (0x2022,0x2040 各 256)  max span 256
#         blocks 1944  native 18126  fully_shadowed 1097
# IDF 侧证据
Select-String "$idf\components\hal\esp32c3\include\hal\spi_ll.h" -Pattern 'clk_equ_sysclk'        # 757-770
Select-String "$idf\components\esp_lcd\spi\esp_lcd_panel_io_spi.c" -Pattern 'clock_speed_hz'      # 84，无钳位
```

标注约定：**实测**＝仓库日志/报告复算；**静态**＝我从源码与 IDF 源码推出；**推测**＝需上机。

---

## 3. 特别核对项（用户点名 6 项）

### 3.1 DISP-01「SPI 实际 40、常量写 80」→ 驳回

- 常量：`display.c:31` `#define LCD_PCLK_HZ (80*1000*1000)`；`display.c:146` `.pclk_hz = LCD_PCLK_HZ`。
- IDF 5.5.3 实测路径（静态，已读安装目录源码）：`spi_bus_add_device` 取 `SPI_CLK_SRC_DEFAULT`
  = APB（`clk_tree_defs.h:277`，CPU 160→APB 80，`sdkconfig` `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=160`）→
  `spi_ll_master_cal_clock(80M, 80M)` 命中 `hz > (fapb/4)*3` 分支 → `clk_equ_sysclk=1`，
  `eff_clk = fapb = 80 MHz`（`spi_ll.h:757-770`）。`esp_lcd_panel_io_spi.c:84` 直传 `pclk_hz`，**全链路无钳位**。
- 实测侧：`app_main.cpp:396-402` 自述 12,288 B 带 2.2–2.7 ms、整窗 14.4–14.8 ms（≈40 MHz 档）；
  R6B §6.2 的 `spi40.log:41`（20,480 B / 4,157 µs = 39.4 Mbit/s）也同档。
- **冲突没解**：R6B §6.3 明确写"若外设真的按 80 MHz 移位，DMA 一定 1.23 ms 发完，与实测 2.47 ms 矛盾"，
  并把机理标成推测、要求示波器定案。报告把这条未决项写成"驱动实际钳到 40（实测）"，是把推测升格成事实。
- 后果：若线上真是 80，改常量后整窗 73,728 B 从 7.37 ms 变 14.75 ms，异步重叠的预算被吃掉——
  正是"必须 reject"的情形；若线上真是 40，改常量零收益（今天的时序已经是对的）。
  两种情况下都不该动常量。
- **建议**：改 `display.c:27-30` 注释（现在的 "0.33 → 0.66 ms per frame" 是旧小窗时代的数，对 192×192
  的 73,728 B 完全不成立），并留一行"待示波器定案"。P2。

### 3.2 DISP-02 / F2「fps overlay 待机未门控」→ 成立，已修

- 全工程 `grep C3_FPS_OVERLAY` = 0 命中：叠加层**没有编译开关**，默认就是每秒画。
- `draw_fps_overlay`（`app_main.cpp:824-865`）只查 `s_display_ok`，`box[24*12*2]=576 B` 静态，
  画在 `(2,2)`——在 192×192 窗口（`(64,24)`）之外，所以 blit 永远不会覆盖它，待机卡/菜单卡一盖就永久缺失/残留。
- HEAD 调用点在 `app_main.cpp:3769`，确实在 `3565` 的 `!s_standby && !menu_is_open()` 块外；
  工作树已改成 `if (dt && !s_standby && !menu_is_open())`（`3787`），**修法正确且已落地**。
- 收益校正：576 B bss + 50 B 字体 + 1 Hz×576 B SPI ≈ 0.06 ms/s。真正的问题是"待机/菜单上多一个黑底数字盒"，
  不是省电。P1 保留可以，但别写成续航项。

### 3.3 DISP-03 → 机理成立，验证手段要换

- `st7735.cpp:99-100`：`fb[...] = pixel; m_dirty[...] |= bit;` 无值比较；`290-317` 是 R7-05 的行内 OR 合并，
  仍然只置位。机理没问题。
- 但报告写的验证"用 `T`/`C` 命令（`1348-1353/1625-1633`）做 pixel-diff"对不上：那两段是 `Q`（poke RAM）
  和 `G`（pattern）。`T` 在 `1330`，`C`（整帧 fb CRC）在 `1607`。而且**帧 CRC 差分判不出"写相同值"**——
  它只能告诉你画面变没变，不能告诉你 guest 写了几次相同值。
- 可执行的替代：在 `store_pixel`/`write_run` 里加 `g_same_value_writes`（`fb[i]==v` 计数）挂到 STAT，
  跑一次稳态时钟屏即可定案；值比较的代价（+1 cmp/px，抵消 R7-05 一部分）也一起量。

### 3.4 AP-01「kFont5x7 两份 295 B」→ 真实，但省 295 B 不是 590 B

- HEAD：`menu.cpp:36` 与 `bezel.cpp:30` 各 `#include "font5x7.inc"`，而 `font5x7.inc` 里是
  `static const uint8_t kFont5x7[]` → 两个内部链接副本，各 295 B（0x127）。
- 去重后只留一份 → **省 295 B**；报告 §0 的"共 590 B 真占镜像"是两份总占用，容易被读成收益。
- 工作树已改：`bezel.cpp:32` 改 `extern const uint8_t kFont5x7[];`、`font5x7.inc:3` 改
  `extern const uint8_t kFont5x7[] = {`（C++ 里 extern+初始化仍是定义，合法）。当前 ELF 只剩
  `kFont5x7 D 295`（nm 实测）——**已经生效，别再动**。
- 唯一残留风险（没写进报告）：这给 menu.cpp 造了一个跨 TU 定义依赖，哪天 menu.cpp 被裁出构建
  就是链接错误。可接受。

### 3.5 AP-02「saveimg 不在 60 s 链上」→ 对，确实纠正了 BRIEF

- `persist_save_image()`（`app_main.cpp:2561`）：`overlay_writes()` 变化 → 记脏 + 1 s 静置 → `saveimg_store`
  （`2569-2589`），与帧循环每帧调用、与 snapshot 完全独立。
- 60 s 链：`maybe_snapshot_machine_state`（`2731`）→ `snapshot_machine_state`（`2673`）→
  `save_panel_frame`（`2708`）+ `gueststate_store`（`2709`）+ `wallclock_persist`（`2726`）。**不含 saveimg** ✓。
- 零售 ROM 不写 overlay 的依据在 `2595-2599`（注释）+ `1686-1688`，与报告一致。
- 磨损账我复核过：fbsave 每次擦 8 扇区 → 每扇区 1440 次/天 → 100k/1440 ≈ 69 天（最坏，resumable-only
  门控会大幅降低）；gueststate 5 条环 → 288 次/天/扇区 → ~347 天。数字成立。

### 3.6 DISP-07「fbsave 擦 0x8000 只需 0x7000」→ 安全

- `fbsave.cpp:27` `kRegion = 0x08000`，`fbsave.cpp:80` `esp_partition_erase_range(s_part, kBase, kRegion)`；
  写入 32 B 头 + 24,576 B（`app_main.cpp:2616` `kPanelWindowBytes = 96*128*2`）= **24,608 B**。
  24,608 跨 7 个 4 KiB 扇区（6 满 + 32 B）→ 擦 `0x7000` 刚好够。
- 读取侧只按 `h.bytes` 流式读 24,576 B（`fbsave.cpp:124-147`），不校验尾部是否 0xFF；`kRegion` 只用于
  边界检查与 `init` 的分区大小检查。分区偏移 `0x0F000` 与 `0x7000` 都是 4 KiB 对齐，不动分区表。
- 代价收益：省 1/8 擦除时间（≈19 ms/次）与 12.5% fbsave 磨损，触发频率 = 自动存档（默认 60 s，且仅
  resumable）。P2 合理，报告结论正确。

### 3.7 AP-03「lapse toast 每帧 10,560 B」→ 现象成立，落地版本有缺口（重点）

**成立**：HEAD `app_main.cpp:3579` 每帧无条件调 `menu_lapse_toast_present`（`menu.cpp:596-648`），
推 `240×22×2 = 10,560 B`；`display_draw_bitmap` 是同步的（`display.c:250-258` → `display_wait_idle`），
@40 MHz ≈ 2.1 ms/帧（@80 MHz ≈ 1.05 ms）。lapse 期间全程如此，属实。

**内容是静态的**：卡片文本只有 `"LAPSE %s  C=STOP"`（`menu.cpp:608`），`%s` 来自 `s_lapse_toast_text`，
只在 `app_main.cpp:746`（进 lapse）和 `3481`（菜单关后补提示）写入 `"60X"/"120X"/…`。也就是说
**同一段 lapse 里每帧画的 5,280 px 完全一样**——"事件化"在语义上站得住，不是把动态内容冻住。

**几何**（我核过，报告的"唯一覆盖者是电池角"不完整）：
- toast：`x0=(320-240)/2=40`，宽 240 → **x 40..279**；`y0=240-22-2=216` → **y 216..237**（`menu.cpp:598-605`）。
- 电池角：`bx=320-16-tw`（`bezel.cpp:210`），`tw=4*12-2=46`（"100%"）或 34（"88%"）→ **bx 258 或 270**，
  `by=240-4-14=222` → **y 222..235**（`kHudPadX=16`/`kHudPx=2`，`bezel.cpp:69-72`）。
  → 与 toast 右侧**重叠 10–22 px × 14 行**，30 s 一次（值变化时）盖上去。
- 菜单卡：`menu_present` 推满 **240 行**（含卡外 48 行黑 backdrop，见 DISP-08 自己的账），开菜单就盖住 toast 整条。

**缺口**（工作树已落地的 push-once 版本，`3585-3595`）：
1. 只按 `strcmp(s_toast_up, s_lapse_toast_text)` 判断，**没有任何外部失效钩子**。
2. 菜单开→关：`3470-3485` 只在 `s_lapse_x>1 && !s_lapse_toast` 时重建 toast——可 `s_lapse_toast` 一直是 true，
   分支不进；`bezel_paint`（`3473`）又把整屏重画成外壳 → **toast 永久消失**，直到 lapse 被取消/重开。
   （HEAD 每帧重推没有这个问题，所以这是"用 2.1 ms/帧换一个功能回归"。）
3. 电池角每次变化都会在 toast 右缘留一个**不会修复的缺口**（比 HEAD 的"每帧被 toast 盖回去"更丑）。

**收的条件**：push-once 保留，但必须在这三处把 `s_toast_up` 清空/置无效：菜单关闭路径（`3470-3475`）、
`bezel_paint` 之后（`3473`/`3491`）、电池角重画之后（`3606`）；或者更简单——把 toast 的 22 行改成由
`s_blit_repaint_all` 那条重绘链统一带一遍。

---

## 4. 其余 P1 的关键取证（R8B-02/03/11）

### 4.1 R8B-02：`uint8_t n` 装不下 256

复算（`%TEMP%\r8v_spans.py`，与报告 §A 同口径）：76 页、tight span 合计 11,377 槽 → 91,016 B、
省 64,632 B，这部分**完全对**。但对每页单独看：

```
page 2022  used 41   min 0  max 255   span 256   ← n 溢出
page 2040  used 167  min 0  max 255   span 256   ← n 溢出
page 2000  used 47   min 0  max 254   span 255   ← 刚好
```

`aot.h:395` 现热路径是 `&d->ent[lpc & 0xffu]`；改成 `(unsigned)(slot - d->base) >= d->n` 后，
`n=256` 被截成 0 → `>= 0` 恒真 → 这两页 208 个块全部 `return nullptr`（占 1,944 的 10.7%，且都在
0x20xxxx 这段 guest ROM 主循环里）。改法二选一：存 `last=max` 用 `slot <= last` 判（0..255 正好装得下），
或 `n_minus_1`。生成器里顺手加 `assert(span <= 256)`。
另：报告说"`AotEnt` 32 B cache line 覆盖 4 槽"——`AotEnt` 是 8 B（`aot.h:324-330`），4 槽 = 32 B ✓。

### 4.2 R8B-03：数字复核一致

`fully_shadowed = 1097`（我按 §A 同算法重跑）、桩 32 B×1097 = 35,104、277,946−35,104 = **242,842** ✓。
前提（929 个 force 块要先跑一次重绘帧 census）报告写清楚了，这里只补一条：**生成器目前不在构建链上**（§5-M1）。

### 4.3 R8B-11：`.gitignore` 是 UTF-8，`tmp/` 不能整删

- `.gitignore` 前 9 字节 `35 32 2D 2D 2D 2D 20 E6 9E`（`# ---- 构`），全文中文正常可读 → **UTF-8**。
  报告"GBK、读出来全是乱码、建议转 UTF-8"是错的；照做反而有把文件改坏的风险。删掉这条建议。
- `tmp/make_font.py`（4,144 B）就是 `font5x7.inc:2` 写明的字体生成器；`tmp/` 里另有 433 个 `.py`、
  54 个 `.md` 调研稿。"该删 tmp/"必须先分类搬运，否则删掉生成器和调研记录。
- 其余数字对得上：根 avi/png 391.6 MB（14 文件，报告 410 MB 略高）、`tmp/` 2,016 MB、
  `dist/` 11.2 MB、`build.log` 2.76 MB、`sdkconfig.bak-wifion` 66.6 KB（报告 ~85 KB）。

---

## 5. missed（三份都漏，按价值排序）

| # | 位置(文件:行) | 现象 | 影响 | 建议 |
|---|---|---|---|---|
| M1 | `main/CMakeLists.txt:26` + `build.ps1:87-89` + `.gitignore:29` | 只编译 `emu/aot_blocks.c`，构建链里**没有任何 `tools/aot.py` 调用**，而该文件又被 gitignore | R8B-02/03 改生成器后不清缓存就编到旧块体 → A/B 结果不可信、甚至"改了没效果" | 在 `build.ps1`（或 CMake custom command）里加"源 `emu/aot_blocks.c` 或 `tools/aot.py` 比产物新就重生成"，并断言页数/块数 |
| M2 | `display.c:239` + `app_main.cpp:863`、`bezel.cpp:233` | fps overlay / 电池角都走同步 `display_draw_bitmap` → `display_wait_idle()` | 在刚发过 73,728 B 异步整窗的帧上，1 Hz 叠加层要额外等最多一整笔传输（14.8 ms@40 / 7.4 ms@80）→ 每秒一次帧尖峰；DISP-02 只记了 576 B | overlay 改画进 `s_frame_buf` 再随整窗发，或 `s_inflight` 时跳过本次 |
| M3 | `display.c:138` | `max_transfer_sz = 320*80*2 = 51,200`，而 SPI 驱动把单笔夹到 32,768 | 描述符按 51,200 分配 13 片×2 = 312 B，15,360 只需 96 B；DRAM 最紧时白占 ~216 B，且误导"能一次发 50 KB" | 改 `320*24*2`（或 32,768），只表达真实上限 |
| M4 | `app_main.cpp:3319`（对照 R8B-10 §2.3） | `s_frame_buf = heap_caps_malloc(73,728, INTERNAL)` 在启动时执行 | "heap 余 87,192 B" 是**静态账**；扣掉它真实剩余 ≈ **13.4 KB**，任何新增 DRAM 用户（`.bss` 不算，但运行期 malloc/队列）都在这 13 KB 里 | R8B-10 的口径改成"启动后可用 ≈13 KB"，后续所有 DRAM 提案按这个审 |

---

## 6. 给主审的动作清单（按顺序）

1. **DISP-01**：只改 `display.c:27-30` 注释，常量保持 80；补一条示波器 TODO。**不要**改常量。
2. **AP-03**：保留 push-once，补失效钩子（菜单关闭 / `bezel_paint` 后 / 电池角重画后）。当前工作树版本会丢 toast。
3. **R8B-02**：`base`+`last`（或 uint16）替换 `uint8_t n`，加 span 断言；先做这条低风险裁剪。
4. **M1**：把 `tools/aot.py` 接进构建，再谈 R8B-03 的 A/B。
5. AP-04：只把 `ESP_LOGI` 降 DEBUG，CRC 与踢位逻辑一行不动。
6. R8B-11：删磁盘可以，先把 `tmp/*.py`、`tmp/*.md` 分类搬出；`.gitignore` 编码这条删掉。
7. AP-01/AP-02/AP-05/R8B-01/R8B-10：已经落地或只是口径，别再排期。

---

## 7. 未做 / 边界

- 未上机、未跑构建、未读 `*.bin`/`*.avi`；AOT 数字来自 `aot_blocks.c` 文本复算 + 当前 ELF 的 nm/readelf。
- 当前 `build/` 里的 ELF 是 20:19（改动后）重编的，**不能**当作被审基线；AP-01 的"两份副本"是用 HEAD 源码
  + include 机制确认的，不是用这份 ELF 量的。
- "线上到底是 40 还是 80 MHz" 我给不出定案，只能给"寄存器是 80、吞吐是 40、两者矛盾未解"。
  这也是 DISP-01 必须驳回而不是降级的原因：**证据不足以支撑一个零风险改动的定性**。

*唯一写入文件：本文件。*
