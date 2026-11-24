# 交接：画面位置修正（2026-11-23 22:35）

给下一轮专门做"画面在面板上不居中 / 要上 B 外壳"这件事。先读本文，再动手。
其余背景读 `docs/HANDOFF_NEXT.md`；按键与极性那段的坑**不要重复**（那是刚修好的）。

---

## 1. 用户的原话与真实诉求

- 第 4 轮：「B 外壳可以，但是**游戏画面没有在整个屏幕上居中**。当前设备上运行的版本，开机后仅显示标题页面……」
- 第 4 轮另一条：「**游戏 UI 界面没有变化，还是主画面偏左的状态**。」
- 最近一句：「接下来是**画面位置修正**。」

**要解决的是观感上的"偏左"，不是"没外壳"。外壳是后一步。**

---

## 2. 当前几何（源码实测，不是回忆）

> **⚠ 本节的数字已被 §10 作废（v1.2.1）。** 保留在此只为说明"为什么当时会得出对称"：
> 那时用的窗口是 128×112 @ (0,8)，确实是 32/32 对称，但那个窗口本身取错了。
> 现行几何见 §10：96×96 @ (0,18)，2× = 192×192 落在 (64,24)。

`firmware/digicore-c3/main/app_main.cpp` 顶部：

```cpp
kWinX = 0;
kWinY = 8;                 // guest fb 的可见区从第 8 行开始
kWinW = 128;
kWinH = 112;               // 不是 120：fb 底部 8 行在面板上不可见
kScale = 2;
kDstW = 320; kDstH = 240;
kLeft = (320 - 128*2)/2 = 32;
kTop  = (240 - 112*2)/2 = 8;      // 注意：这行注释写着 "// 0"，是错的
```

推送位置（`blit_frame()` 内）：`outx = kLeft + …`、`outy = kTop + …`。

于是画面落在 **x = 32…287，y = 8…231**：

| 边 | 画面到边缘 |
|---|---|
| 左 | 32 px |
| 右 | 320 − 288 = 32 px |
| 上 | 8 px |
| 下 | 240 − 232 = 8 px |

**几何上左右严格对称、上下严格对称。** 所以"偏左"不可能来自 `kLeft` 的算术，只能来自下面三种之一。

---

## 3. "偏左"的三个候选来源（先用 G 2 区分，别猜）

### 3.1 面板可见区不在控制器坐标系的 (0,0)

`display.c` 里 `esp_lcd_panel_set_gap(s_panel, 0, 0)`，`LCD_H_RES 320 / LCD_V_RES 240`。
ST7735 320×240 是从 240×320 旋转来的（`madctl=0x00a8`，MV 置位）。**这一级别的面板常见几十像素的固定偏移，gap 填 0 不等于物理可见区从 0 开始。**

### 3.2 用户看到的是旧固件

`kTop` 的注释写 `// 0`，历史上确实跑过 `kTop = 0` 的版本（画面顶到 y=0、底部空 16 px）。**如果用户手机里那张照片是旧版本拍的，几何分析会白做。**

### 3.3 外壳/面板安装遮挡

如果机身开窗不正，右侧被挡住 20–30 px，视觉上就是"整体左移"。

### 怎么区分：`G 2`（本次新增）

```
G 2     → 直接往面板推 1 px 白框（贴 320×240 的四边）+ 四角 8×8 白块
G 1     → 只在 fb[] 里画红框+十字，经 blit 上去，用来对 blit 的缩放对齐
G 0     → 关掉
```

`G 2` 的框是**面板坐标系**的，不是画面的。**让用户拍一张 G 2 的照片**，然后量：

- 四条白边是否都完整可见？缺哪边，就是 3.1 或 3.3。
- 四角小方块到面板物理边缘的距离是否相等？左右不等 → 3.1/3.3；相等但画面偏 → 3.2。
- 白框右侧是否被机身/亚克力压住 → 3.3。

**这一步没做完之前，不要改任何 `kLeft`。**

---

## 4. B 外壳：不要把参考规格直接抄过来

`<home>\digimon-passport\docs\PROMPT-UI-carryover.md` 第 11–347 行的外壳规格是按**那台机器的 96×96 可见区**算的（canvas 192×192、rim 204×204、`view_y = 14`、底部 34 px 给 HUD）。

**本机可见区是 128×112，不是 96×96。直接套 204 会得到一个比画面还小的框。**

按本机重算：

| 项 | 值 | 说明 |
|---|---|---|
| canvas（画面） | 256×224 @ (32, 8) | 2× 整数放大，不变 |
| well | 262×230 @ (29, 5) | canvas 外扩 3，圆角 4 |
| rim | 268×236 @ (26, 2) | canvas 外扩 6，圆角 8 |
| 左右余量 | (320−268)/2 = 26 | 对称 |
| 上下余量 | (240−236)/2 = 2 | 对称，**几乎没有** |

**结论：本机上 B 外壳能塞进去，但上下只剩 2 px，HUD 横带无处可放。**

三条出路，让用户拍板，别自己替他定：

1. **放弃 HUD**：外壳按上表做，最省事，观感也最接近"整块居中"。
2. **画面降到 1.75×（224×196）**：非整数倍，1 px 的 LCD 细节会糊或抖。**不推荐**，参考文档 §3.1 明确说整数倍是硬要求。
3. **裁剪可见窗口到 96×96**：丢掉上下各 8 行和左右各 16 列，画面内容真的少了。

---

## 5. 实现要点（改之前必须知道的）

1. **`blit_frame()` 是分 band 的**：`kChunkRows = 16`，每个 band 一次 `display_draw_bitmap(outx, outy, …)`。外壳不能在整帧层面叠加，只能在**每个 band 里**把 rim/well 的像素一起算进去，否则外壳会被下一个 band 覆盖。
2. **DRAM 只剩 ~10 KB**，不能缓存整帧（320×240×2 = 150 KB）。`s_strip` 是唯一的暂存区，大小 `kSrcRowStride * kChunkRows * kScale = 512 × 16 × 2 = 16 KB`。
3. **菜单和确认卡是直推面板的**，不写 `fb[]`。外壳要么在 `menu_present()` / `menu_confirm_present()` 里也画一遍，要么接受"开菜单时外壳消失"。
4. **`panel_clear_side_margins()`** 硬编码了 `kLeft` 和 `kWinW * kScale`（左右各 32 px 黑边）。改布局必须同步改它，否则菜单关闭后会留残带。
5. **`MenuBackdrop`** 的 `out_x / out_y / scale` 是菜单采样 fb 的依据，也硬编码在调用点（`app_main.cpp` 两处，`kLeft, kTop, kScale`）。改布局要一起改。
6. **`G` 命令的解析**刚修过一次：原代码读 `line[1]`，而 `"G 1"` 的 `line[1]` 是空格，所以 `G 1` 从来没有生效过（一直返回 `ACK G 0`）。现在先跳空格再取字符。

---

## 6. 验收

```powershell
# 抓当前帧（看 fb 内容与缩放是否正确）
& $py tmp\dump_fb.py COM4 tmp\s28\geom.png "8;K 4 40;5;K 4 40;6"

# 面板边框（让用户拍照，量四个方向的余量）
发送 "G 2"，等 1 s，拍照
发送 "G 0" 复位

# 画面落点（不拍照也能判断：绿框左右两边的黑边等宽就是居中）
& $py tmp\s29\align.py G 3 --noreset   →  ACK G 3
& $py tmp\s29\align.py G 0 --noreset   →  ACK G 0

# 几何自检
发送 "V"   →  VER ver=1.2.0 win=0,8 128x112 scale=2 left=32 top=8 off=0,0 home=32,8 fb=128 chunk=16
              left/top 必须等于 left_px()/top_px()，home 是几何基准（32,8）
```

**判据**：G 2 照片上四条白边到面板物理边缘的距离，左右相等、上下相等，才算"居中"。只靠 `FB_CRC` 或者抓 `fb[]` 判断不了位置——**fb 只有 128×112，它不知道自己在玻璃上的哪里**。G 3 的绿框同理，但它比 G 2 多一层好处：绿框跟着 `left_px()` 动，所以 `E` 命令调完立刻能看见结果。

---

## 7. 别踩的坑（都是这两天刚踩过的）

| 坑 | 后果 |
|---|---|
| 信 `kTop` 那句 `// 0` 注释 | 会把居中算成偏上 8 px |
| 把 `kWinH` 当 120 | 多出 8 行隐藏区，画面底部被切掉一条 |
| 用 `--press`/`K` 注入的帧数当时间 | 设备一帧 60–300 ms，30 帧 ≠ 0.5 s，而是 3–9 s |
| 改完布局只验证抓帧 | 抓帧看不到面板位置，肉眼与数据会打架 |
| 在 `blit_frame()` 之后单独画外壳 | 下一帧的 band 会盖掉它 |
| 只改 `version.txt` 就编译 | CMake 的 `file(READ)` 不会重跑，bin 里还是旧版本号；要 `-Clean` 或碰 `main/CMakeLists.txt` 时间戳 |

---

## 8. 当前状态速查

- 固件：源码 `1.2.0`（`version.txt`），已烧在设备上，`V` 回 `VER ver=1.2.0 …`；**改动尚未提交**，基线仍是 tag `v1.1.0` / commit `344aab9`
- 按键：**低有效**（A 按下 = `0xFFF7`），**不要改回高有效**
- 游戏能进：短按 A → LOAD/RESET 菜单（`[0x0C88]=1`）→ C 到宠物（`=4`）
- 长按 A 1.2 s：弹 A+C 确认卡；B 确认，C 取消
- 串口：COM4，115200，`serial.Serial(..., dtr=False, rts=False)`
- 烧写/构建命令见 `docs/HANDOFF_NEXT.md` §5

---

## 9. 2026-11-23 22:40 — 左右居中已实测确认（v1.2.0，已烧在设备上）

> **⚠ 本节的部分结论已被 §10 修正**：`kLeftHome=32`、画面 256×224 是错的窗口算出来的；
> 现行是 64 / 192×192。本节保留的价值是 `E` / `G 3` 两个工具的说明，机制不变（量程变大）。

**结论：画面在面板上左右本来就是对称的 32 / 32，实机 `G 3` 目视确认「左右一样宽」。**

第 4 轮那句「主画面偏左」不是 blit 的坐标问题。剩下的解释只有两种：看的是旧固件（`kTop=0` 那个版本），或者外壳/开窗遮挡。**不要再改 `kLeft` 的算术**——它从来没错过，从 baseline 到现在一直是 `(320-256)/2 = 32`。

这一轮把「能不能现场调」这件事补齐了，以后不管偏移来自屏、外壳还是安装，都不用重烧一遍固件去试。

### 9.1 改了什么

- **布局常量拆成「几何基准 + 运行时偏移」**：`kLeftHome = 32`、`kTopHome = 8`，新增 `s_off_x / s_off_y`（默认 0）。所有落点统一走 `left_px() / top_px()`：`blit_frame()`、`panel_clear_side_margins()`、`menu_present()` / `menu_confirm_present()` 的 `MenuBackdrop`、以及 `V` 的报告行。原来这四个地方各自硬编码 `kLeft`，一处改不全就会「游戏居中、菜单偏」——这正是它没法现场调的原因。
- **新增 `E [<left> [<top>]]`**：把画面左上角放到指定的**面板绝对坐标**（不是增量），无参数时回报当前值。执行时先 `panel_fill_black()` 再整帧重推：旧位置那几条边带没有任何代码会重画，不清就是两道残影。
- **新增 `G 3` 对齐靶**：顶部 8 px 刻度尺（每 8 一格、40 一长格）+ 绿框（当前窗口落点）+ 青色十字（窗口中心），直接推面板。与 `G 2` 的分工：`G 2` 问「玻璃到哪结束」，`G 3` 问「画面在哪」。
- `G` 的参数解析原来只认 `0..2`，`G 3` 会被当成 `G 0`；已扩到 `0..3`。
- 修掉 `kTop` 那句 `// 0` 的错注释（实际是 `(240-224)/2 = 8`）。
- `version.txt` → `1.2.0`（改完要碰 `main/CMakeLists.txt` 时间戳强制重配，否则 `file(READ)` 不重跑）。

### 9.2 实机验证记录

```
V   → VER ver=1.2.0 win=0,8 128x112 scale=2 left=32 top=8 off=0,0 home=32,8 fb=128 chunk=16
G 3 → ACK G 3            （屏上：刻度尺 + 绿框 x=32..287 y=8..231 + 十字 160,120）
目视 → 绿框到左右两边的黑边一样宽
G 0 → ACK G 0            （复位，游戏画面原样回来）
```

### 9.3 怎么用（下次量偏移，不用重烧）

```powershell
$py="<home>\.espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe"
& $py tmp\s29\align.py                # 复位 + V + 推 G 3
& $py tmp\s29\align.py G 3 --noreset  # 不打断当前游戏状态
& $py tmp\s29\align.py E 40 --noreset # 把画面左边挪到面板 x=40（往右 8 px）
& $py tmp\s29\align.py E --noreset    # 只回报当前 left/top/off
```

判据：`E <left>` 之后 `V` 里 `left` 必须等于该值（会被 `0..kMaxOffX` 夹住，v1.2.1 起是 `0..128` / `0..48`）。满意后把这个值折回 `s_off_x / s_off_y` 的初值再编一次，别指望它跨重启。

---

## 10. 2026-11-23 23:0x — 「偏左」的真根因与修复（v1.2.1，该版本已撤销归档，见 §11）

### 10.1 根因：窗口取的不是可见区

MAME 对这个机种写死了可见区（`ref/generalplus_gpce4.cpp:397`）：

```cpp
void generalplus_gpce4_digicolr_state::digicolr(machine_config &config)
{
    generalplus_gpce4(config);
    m_screen->set_visarea(0, 96-1, 18, 114-1);   // x 0..95, y 18..113 → 96x96
}
```

抓帧统计也证实 ROM 只画这块：两张不同画面里非黑像素**严格**落在 x 0..95、y 18..113，矩形外全 0。

旧代码取的是 `kWinW=128 / kWinY=8 / kWinH=112`（全宽 + 更高的切片，注释理由是"底部图标被切"）。后果：

| 方向 | 旧窗口 | 真实可见区 | 多出来的部分 |
|---|---|---|---|
| 宽 | 128 列 | 96 列 | **右侧 32 列全是空** → 2× 后画面块右边多出 64 px 黑 |
| 上面 | 从行 8 起 | 从行 18 起 | 顶部 10 行空 |
| 下面 | 到行 119 | 到行 113 | 底部 6 行空 |

所以 2× 之后画面块 256 px 宽，**只有左边 192 px 有内容**，右边 64 px 空；而面板左边距只有 32 px。视觉上就是"整块偏左"。居中算术从没错，是缩放了一座空房子。

顺带解开历史谜团：注释说"96 行窗口把底部图标切一半"——那是因为当时 `kWinY` 用的是 8 而不是 18，切掉的是行 114..119 之外的部分。**改成从 18 起，96 行正好完整**。

### 10.2 修复

```cpp
kWinX = 0;  kWinW = 96;
kWinY = 18; kWinH = 96;
kLeftHome = (320-192)/2 = 64;
kTopHome  = (240-192)/2 = 24;
```

连带三处必须一起改，否则各自静默出错：

1. **`panel_clear_side_margins()` 改成分 band**（`kChunkRows` 行一次）。原来一次性清一列：32 px 宽时是 15,360 B，勉强塞进 16 KB 的 `s_strip`；64 px 宽就变 30,720 B，而 `s_strip` 现在只有 12 KB。不清就是关菜单后两道残影。
2. **`panel_window()` / `kPanelWindowBytes` 改成按整行保存**：`kWinH * kFbPanel * 2` = 24,576 B。fbsave 只认连续内存，如果按 `kWinW*kWinH` 存，它会从 96 列的行尾直接走进下一行行首，恢复出来的画面是剪切错位的。
3. **`T` dump 的注释**（56 KB→37 KB），代码本身跟着 `kWinW/kWinH` 走，不用改。

### 10.3 验证

```
V   → VER ver=1.2.1 win=0,18 96x96 scale=2 left=64 top=24 off=0,0 home=64,24 fb=128 chunk=16
T   → SHOT_BEGIN 96 96；抓帧后内容列 0..95（满宽，右侧无空白）
```

### 10.4 副作用：画面变小了，这是对的

`256×224 → 192×192`。小掉的那 64×32 本来就是空的（32 列 × 2 + 16 行 × 2）。
源是 96×96，整数倍只有 2×（3× = 288 超过 240 高），**2.5× 会让每个格子宽窄不均、糊成灰块**，这是官方文档明令禁止的。所以 192×192 是唯一可选项。

### 10.5 外壳规格自动对上了

新几何下：glass 192×192 @ (64,24)，rim `+12` = **204×204 @ (58,18)**，well `+6` = 198×198 @ (61,21)。
这正好是 `docs/HANDOFF_2026-11-23.md:202` 里用户拍板的「外壳 + 画面作为 204×204 块整体居中，左右各 58、上下各 18」。

**说明 204×204 的规格本来就是按 96×96 玻璃算的，是代码里那个 128×112 让它对不上。**

### 10.6 还没解决

- **上下方向仍无实测**：几何上 24 / 24。`G 3` 绿框现在应该是 x 64..255 / y 24..215、中心 (160,120)，让用户看一眼即可。
- **30 px 圆角仍未实现**：本机内容全部落在圆角安全区内，所以看不出来；要看出圆角必须先把外壳底纹铺满四角。详见 `docs/RESEARCH_SCREEN_AND_ROUNDING.md`。

---

## 11. 2026-11-23 23:2x — v1.2.1 的回归：长按弹卡/弹菜单后闪几下就重启（v1.2.2 已修）

### 11.1 症状

长按 A（上键）弹 A+C 确认卡、或长按 B（下键）弹设置菜单，屏幕闪烁几次后自动重启。
**v1.1.0 没有这个问题，是 §10 那次改窗口引入的。**

### 11.2 根因：`s_strip` 被缩到装不下一个菜单 band

`s_strip` 是唯一的暂存区，有两个互不相干的消费者：

| 消费者 | 需要的字节 | 来源 |
|---|---|---|
| `blit_frame()` | `kSrcRowStride * kChunkRows * kScale` = 384 × 16 × 2 = **12,288** | 跟着 `kWinW` 算 |
| `menu_present()` / `menu_confirm_present()` | `w * 2 * 24` = 320 × 2 × 24 = **15,360** | `menu.h:66` 明文契约，`menu.cpp` 里 `band_rows = 24` 硬编码两处 |

而 `s_strip` 的定义是 `kStripBytes = kSrcRowStride * kChunkRows * kScale` —— **只按 blitter 算**。

`s_strip` 具体大小历史上是 `128×2×2 × 16 × 2 = 16,384`，**刚好**盖住菜单的 15,360。
§10 把 `kWinW` 从 128 改成 96 之后，它变成 `96×2×2 × 16 × 2 = 12,288`，**短了 3,072 B**。
菜单一画，`menu_present()` 就往数组外写 3 KB，踩坏相邻静态变量，下一次间接访问就炸。

### 11.3 证据（v1.2.1 实机）

```
ACK K 1 40
I (2237) DIGICORE: long A (1414 ms) -> A+C confirm card
Guru Meditation Error: Core  0 panic'ed (Load access fault). Exception was unhandled.
  MEPC : 0x4200d5f6   RA : 0x4200d5e4   SP : 0x3fccefc0
rst:0xc (RTC_SW_CPU_RST),boot:0xa (SPI_FAST_FLASH_BOOT)     ← 软件复位
```

`Load access fault` = 读非法地址，不是除零也不是看门狗，**指针/长度被踩坏**的典型形状。
卡片日志与 panic 之间只隔一帧，正好是第一次 `menu_confirm_present()`。

### 11.4 修法

两个尺寸都具名，取最大值，并用 `static_assert` 钉死契约：

```cpp
static constexpr int kMenuBandRows = 24;                        // must match menu.cpp
static constexpr int kStripBlitBytes = kSrcRowStride * kChunkRows * kScale;
static constexpr int kStripMenuBytes = kDstW * 2 * kMenuBandRows;
static constexpr int kStripBytes =
    kStripBlitBytes > kStripMenuBytes ? kStripBlitBytes : kStripMenuBytes;
static_assert(kStripBytes >= kDstW * 2 * kMenuBandRows,
              "s_strip must hold one menu band or the menu writes past it");
```

结果 `kStripBytes = 15,360`（菜单需求）。`static_assert` 保证以后谁再动 `kWinW`、`kDstW` 或菜单 band 行数，编译期就报错，而不是上机闪几下。

### 11.5 验证（v1.2.2 实机）

```
长按 A → I (3451) long A (1221 ms) -> A+C confirm card          无重启
长按 B → I (2349) long B (1525 ms) -> settings menu
         I (2452) menu: pushed 10 band(s), 320x240              无重启
按 C 关菜单 → 抓帧 96x96 游戏画面完整，左右边带无残影
```

复现脚本：`tmp/s31/repro.py <mask> <frames> <seconds> [--reset]`，会在日志里找
`ESP-ROM` / `rst:` / `Guru` / `wdt` / `canary` 这些标记，命中即判重启。

### 11.6 教训

免费内存是挪出来的，不是白送的：把缓冲区大小绑在某个几何常量上，等于给所有消费者发了一张它没同意签的支票。
**共享缓冲区必须按"最大消费者"定尺寸，并把每个消费者的语义写进同一个文件。**

### 11.7 版本归档

1.2.1 那一版带这个 bug，**未做归档**（tag 与里程碑都已撤销）。里程碑是 **v1.2.2 = 画面居中 + 闪退修复**，
见 `milestones/2026-11-23_v1.2.2_centered/`。

---

## 12. 2026-11-23 23:3x — 设置界面改到长按 A，第一项发 A+C（v1.3.0）

### 12.1 需求与原因

原设计：长按 A 弹 A+C 确认卡、长按 B 呼出设置菜单。
问题：**长按 B 的 1.5 s 先被喂给了 ROM**（一帧 60–300 ms，按住就持续喂），游戏收到长按 B 后切到时钟画面，用户"既开了菜单也换了画面"。

新设计：

| 手势 | 行为 |
|---|---|
| 长按 A（1.2 s，`H <ms>` 可调） | 呼出设置菜单，光标默认落在第 0 行 |
| 菜单第 0 行 | **SEND A+C** —— 按 B 发送 3.2 s A+C，并关闭菜单 |
| 菜单第 1–7 行 | AUTO OFF / BRIGHT / VOLUME / AUTO SAVE / SAVE OUT / SAVE IN / VERSION，行为不变 |
| 长按 B | **归还给游戏**，不再呼出任何东西 |

A+C 确认卡（`menu_confirm_*`）不再被触发，代码保留并标注 superseded——它仍是唯一能画 modal 的路径。

### 12.2 改动清单

| 文件 | 改动 |
|---|---|
| `menu.h` | `MENU_ITEMS` 7→8；新增 `MENU_IDX_CHORD = 0`，其余 1..7；新增 `MENU_ACTION_CHORD` |
| `menu.cpp` | 行高 24→20、行距 26→22（8 行：45 + 7×22 + 20 = 219 < 228 卡片底）；`name_for/label_for` 加 CHORD；`menu_press_b` 加 case；`menu_open` 光标默认 CHORD；`menu_present` 加渲染/总耗时日志 |
| `app_main.cpp` | 长按 A → `menu_open()`；删除长按 B 的菜单触发；A 只喂 `kPressFrames` 帧脉冲（长按不再灌进 ROM）；`MENU_ACTION_CHORD` → 置 `s_ac_active` 3.2 s 并关菜单；`kMenuHoldUs` 删除 |
| `font5x7.inc` | 由 `tmp/make_font.py` 重新生成，补了 `+` 字形（原来 43 号是空的，SEND A+C 显示成 "SEND A C"） |

### 12.3 两个当场踩到的坑

1. **时间戳倒挂让改动编不进去**：`edit` 写入的 mtime 比上一次编译产物的 mtime 旧（并行执行时构建先读了文件），ninja 判定 obj 更新、跳过编译，于是连着两次烧的固件行为一模一样。症状是"改了代码但设备表情不变"。**改完源码先 touch 再 build**，或者直接 `-Clean`。
2. **长按 A 打开菜单后，同一个 A 又把光标挪走一格**：`s_menu_pad_prev` 原本用 `instant | latched` 种子，而 K 注入是在长按判定**之后**才覆盖 `instant` 的，所以种子是 0，下一帧 A 读回来就是"新边沿"，`menu_press_a()` 把光标从第 0 行推到第 1 行（用户按 B 改的是 AUTO OFF）。改成用 `pressed`（含 `s_inject_mask`）种子。

### 12.4 性能评估：半透明要不要改成不透明

`menu_present()` 现在会打印自己的耗时：

```
menu: pushed 10 band(s), 320x240: render 79 ms, total 100 ms
```

| 指标 | 值 | 含义 |
|---|---|---|
| render | **79–81 ms** | 240 行 × 320 px 的合成：客人画面采样 + 12 px 阴影 + 90 % 卡片混合 + 文字 |
| total | 98–100 ms | 含 10 个 band 的 SPI 推送 |
| 频次 | 只有 `s_dirty` 时才跑 | 打开菜单、每次按键各一次；菜单期间 guest 已 park，静止时开销为 **0** |

**结论：不改不透明。** 理由不是"还能忍"，是**改不透明解决不了主要成本**：

- 卡片**外面**必须继续显示游戏画面（否则四周全黑），所以 `backdrop_px()` 的逐像素采样一次都省不掉；
- 不透明只能省掉 `blend565()`（每像素 3 次乘法），估计只回收 10–15 ms 里的一部分；
- 换来的是失去半透明质感和投影——那是照 digimon 参考面板做的观感，代价明确、收益可疑。

真正的成本在**每次按键都重绘整张卡**。正确的优化是**只重推变化的行**：

| 方案 | 打开菜单 | 每次按键 | 观感 |
|---|---|---|---|
| 现状 | 79 ms | 79 ms | 半透明 ✓ |
| 改不透明 | ~65 ms | ~65 ms | 失去质感，收益小 |
| **局部重绘（建议）** | 79 ms | **~2 ms** | 半透明 ✓ |
| 局部重绘 + backdrop 移位代替除法 | ~55 ms | ~2 ms | 半透明 ✓ |

（`scale` 固定为 2，`backdrop_px()` 里的两次整数除法可以换成移位，约占 79 ms 的三分之一。）

要不要做局部重绘，等拍板。改动集中在 `menu_present()` 的 band 循环：把"整卡 dirty"换成行区间 dirty。

### 12.5 验证

```
长按 A → menu: open / long A (1575 ms) -> settings menu
         menu: pushed 10 band(s) ... render 79 ms, total 100 ms
按 B   → menu: A+C chord requested → menu: close
         DIGICORE: menu row 0 -> A+C sent (3200 ms)
         port=ffd7（A+C，A 0x08 + C 0x20）→ 3.2 s 后 port=ffff
长按 B → 只有 port=ffef（B 到 guest），没有 menu: open
```

卡片外观用 `tmp/s31/menu_shot.py` 从串口抓回（`N 10` 通道，菜单不写 fb[]）：

```
SETTINGS                              1.3.0
[ SEND A+C ]                       ← 第 0 行，黄色选择条
  AUTO OFF                  FOLLOW 16S
  BRIGHT                          80%
  VOLUME                          20%
  AUTO SAVE                       60S
  SAVE OUT                   SAVE NOW
  SAVE IN                   LOAD LAST
  VERSION                       1.3.0
```

---

## 13. 2026-11-23 23:4x — 「SEND A+C 没有作用」：两条真 bug，和一个不成立的前提

### 13.1 修掉的两条（都实测过）

**① 长按 A 会把游戏推离当前画面。**

A 按下后先喂了 guest 6 帧（`kPressFrames`）才进入长按计时。在标题画面（`[0x0C88] = 0`）实测：

```
冷启动              0x0C88 = 0000
长按 A（修前）      0x0C88 = 0001   ← A 的脉冲把游戏推进了 LOAD/RESET 菜单
```

于是菜单里那条 SEND A+C 落在一个不认它的画面上——这就是"SEND A+C 没作用"的直接原因。

修法：**A 按下时扣住不投递，松手时才决定**。短按 → 补发一个 `kPressFrames` 脉冲；长按 → 只开菜单，一个字节都不进 ROM。四处要一起堵：

| 位置 | 原来 | 现在 |
|---|---|---|
| `if (instant != 0) { ... s_hold_left = kPressFrames; }` | A 在按下时就被喂 | 用 `instant & ~BTN_KEY0`，A 不进这条路 |
| `else if (latched != 0)` | 锁存的 A 从侧门进来 | 同样剥掉 `BTN_KEY0` |
| `int mask = ... : instant;` | **A 从这里再次进入** | `instant & ~BTN_KEY0` |
| 松开检测 | 无 | `s_a_withheld` → 投递一次脉冲 |

修后实测：长按 A `0x0C88` 保持 `0000`；短按 A（325 ms）→ `0001`，短按手感正常。

代价：短按 A 从"按下即响应"变成"松手才响应"，延迟就是一个手指的按击时长（100–300 ms）。标题画面对纯 A 本来就不响应（见下），所以最要紧的那个画面上看不出差别。

**② 菜单发 chord 前少了 700 ms 静默期。**

旧确认卡是 `B 确认 → 700 ms 全松开（kPadBlankUs）→ A+C`；菜单版是 `B 选中 → 下一帧直接 A+C`。B 还按着就跳到 A+C，ROM 那次跳变直接丢掉。已补：

```cpp
s_ac_until_us = now + kPadBlankUs + kAcChordUs;
s_b_hold_locked = true;
s_pad_blank_us = now + kPadBlankUs;
```

实测 `port=ffd7` 现在从按键后 +752 ms 才开始，3.2 s 后释放——时序和旧卡片一致。

### 13.2 前提是成立的，但注入方式不对

**先更正 13.2 的初版结论**：A+C 不是"不存在"的组合键。官方说明书写着三条组合键（见 `docs/RESEARCH_DMC_MANUAL.md`）：

| 组合 | 界面 | 功能 |
|---|---|---|
| **A+C** | 主画面 | **静音 / 取消静音**（屏幕上没有任何反馈） |
| **A+C** | 时钟画面（B 显示时钟后） | **进入改时间** |
| **A+B** | 墓碑 | 孵新蛋 |

所以 13.1 修好的两条仍然必要，但"A+C 无效"另有原因。实测（`K 5 <n>` 直接注入 `0xFFD7`）：

| 画面 | 输入 | 结果 |
|---|---|---|
| 标题 `[0x0C88]=0` | A+C 6 / 20 / 30 帧 | 无变化 |
| LOAD/RESET `1` | A+C | 无变化 |
| 宠物 + 时钟叠加 `4` | A+C，以及「先 A 4 帧、再 A+C 24 帧」分阶段 | 画面无变化 |

`port=ffd7` 都完整发出并保持到位，是 ROM 不接。

**最可能的原因**：真机三键在一条电阻梯上，物理上不可能同时产生两个接触，组合键是靠**扫描时序**拼出来的（ROM 轮换扫描线，A+C 在两轮之间被识别），不是靠一个稳定的 `0xFFD7` 电平。我们现在给的是一个稳定电平，ROM 的解码里大概没有这个分支。

**因此**：`SEND A+C` 在有可见反馈的场合（时钟画面）仍需验证；主画面那条即使成功也只是静音，屏幕上看不出来——**这很可能就是"1.2.2 有作用 / 1.3.0 没作用"的观感来源**：1.2.2 那次可能是在有反馈的画面，或者听到了静音，而 1.3.0 测试时画面没对上。

### 13.3 待决

`SEND A+C` 值得再验一次，验法要按手册来：

1. 主画面无图标选中时**短按 B** 显示时钟；
2. 在时钟画面上按菜单第 0 行；
3. 看时间数字是否变成可编辑（闪烁 / 可选）。

如果时钟画面也不认，就说明稳定电平注入这条路走不通，那时再决定：
- 换成分阶段注入（A 先落、C 后加，带间隔）；
- 或者把这个入口换成真的能用的命令（回标题 / 立即存档）。

在结论出来之前不动代码。

---

## 14. 2026-11-24 00:0x — 菜单加入 A+B（v1.3.1）

### 14.1 为什么加

`docs/RESEARCH_DMC_MANUAL.md` §2 抄了官方说明书的第三条组合键：

> **A+B on a Grave.** This will allow you to hatch a new Digimon.

真机三键在一条电阻梯上，A+B 和 A+C 一样物理上按不出来，所以它和 A+C 一样只能由模拟器代发。

### 14.2 布局：8 行放不下第 9 行

卡片 `kCardY=16 / kCardH=212`，内容底边 228。原来 8 行用 20 px 行高 + 22 px 间距：

```
45 + 7*22 + 20 = 219   ✓  8 行刚好
45 + 8*22 + 20 = 241   ✗  9 行溢出 13 px
```

改成 **18 px 行高 + 20 px 间距**：

```
45 + 8*20 + 18 = 223   ✓  距底边 5 px，14 px 字形上下各留 2 px
```

卡片尺寸没动（加高会顶到屏幕底 240）。

### 14.3 实现：两个 chord 共用一个槽位

原来的 `s_ac_active / s_ac_until_us` 换成一组通用的：

```cpp
static bool s_chord_active = false;
static int  s_chord_keys = 0;        // BTN_KEY0|BTN_KEY2 = A+C, BTN_KEY0|BTN_KEY1 = A+B
static int64_t s_chord_until_us = 0;
static void start_chord(int keys, int64_t hold_us);
```

一个槽位够用：chord 只从菜单行触发，而菜单在触发的同时就关闭了，两个不可能重叠。
`mask = s_chord_active ? s_chord_keys : (instant & ~BTN_KEY0);` —— 仍然是"物理 pad 不含 A"。

菜单：

| 行 | 内容 | 动作 |
|---|---|---|
| 0 | SEND A+C | `start_chord(A|C, kPadBlankUs + kAcChordUs)` |
| 1 | SEND A+B | `start_chord(A|B, kPadBlankUs + kAcChordUs)` |
| 2–8 | AUTO OFF / BRIGHT / VOLUME / AUTO SAVE / SAVE OUT / SAVE IN / VERSION | 不变 |

两条 chord 都先 blank 700 ms 再按住 3.2 s，和旧确认卡一致。

### 14.4 验证

```
长按 A → menu: open（光标第 0 行）
按 A   → 光标移到第 1 行（A 的边沿由菜单消费，不喂 guest）
按 B   → menu: A+B chord requested → menu: close
         menu row 1 -> A+B in 700 ms, held 3200 ms
         port=ffe7（A 0x08 + B 0x10）→ 3.2 s 后 port=ffff
```

卡片渲染（`tmp/s31/menu_shot.py` 从串口抓回）：9 行全在框内，`SEND A+B` 在第 2 行，版本号 1.3.1。

### 14.5 还没验的

**A+B 在墓碑画面到底能不能孵出蛋**，现在验证不了——需要宠物死亡才会出现墓碑，从当前存档走不到。等真的死一只再说。

`port=ffe7` 只证明组合送到了 PORTA，和 A+C 那次一样，ROM 认不认是另一回事（A+C 是在时钟画面才确认有效的）。

---

## 15. 2026-11-24 00:1x — 关掉设置卡后上下残留（v1.3.2）

### 15.1 症状

关掉设置卡之后，游戏主画面上方和下方各留一条设置卡颜色的横带。

### 15.2 根因：卡片上下伸出了游戏窗口

| 对象 | 位置与尺寸 |
|---|---|
| 设置卡 | x 10..309，y 16..227（`menu.cpp` kCardX/Y/W/H = 10/16/300/212） |
| 游戏窗口 | x 64..255，y 24..215（96×96 × 2 @ (64,24)） |

卡片比窗口**上下各多出一截**：

- 上方 y 16..23 —— 8 行，192 px 宽
- 下方 y 216..227 —— 12 行，192 px 宽

左右两截（x 10..63 与 256..309）旧代码本来就清，所以只有上下看得见。

**为什么以前没问题**：窗口还是 256×224 @ (32,8) 的时候，卡片 y 16..227 完全落在窗口 y 8..231 之内，关菜单后下一帧 blit 重推整个窗口就把它盖掉了。§10 把窗口缩到 192×192 @ (64,24) 之后，这条"反正会被 blit 覆盖"的假设就不成立了——blit 只管窗口内，窗口外没有任何人重画。

这是 §10 那次窗口修正的第三笔连带账（前两笔：`s_strip` 缩水导致闪退、`panel_window()` 连续内存假设）。

### 15.3 修法：清"窗口之外"而不是"左右两条"

新增 `panel_fill_rect(x, y, w, h, color)`：按 `kChunkRows` 行一块，用 `s_strip` 填任意矩形，自动裁到面板内，565 值做大端交换。

`panel_clear_side_margins()` 改成四条带：

```cpp
panel_fill_rect(0, 0, lx, kDstH, 0);                       // 左
panel_fill_rect(lx + rw, 0, kDstW - (lx + rw), kDstH, 0);  // 右
panel_fill_rect(lx, 0, rw, ty, 0);                         // 上
panel_fill_rect(lx, ty + rh, rw, kDstH - (ty + rh), 0);    // 下
```

`lx/ty/rw/rh` 都取自 `left_px()/top_px()/kWinW*kScale/kWinH*kScale`，所以用 `E` 命令挪过画面之后这几条带仍然贴着窗口——不会出现"清错位置"。

### 15.4 验证

```
长按 A → menu: open，menu: pushed 10 band(s)（render 78 ms）
按 C   → menu: close，无 E ( 错误，guest 恢复运行（frames 35 → 49 → 65 → 81）
T      → 96x96 抓帧，宠物/海景/图标完好
```

面板上那两条带是否真的黑掉了**只能目视**——ST7789 的 SPI 是单向的，读不回像素。逻辑上四条带覆盖了窗口外全部区域（左 64 列、右 64 列、上 24 行、下 24 行，窗口 192×192），合起来就是整块 320×240。

（v1.4.0 起这四条带不再是黑色，改成了砖墙，见 §16。）

---

## 16. 2026-11-24 00:4x — 游戏画面加砖墙边框（v1.4.0）

### 16.1 配色：从真机商品图采样，不是拍脑袋

用 `tmp/dmc_photos/mynavi_001.jpg`（Ver.1 Original Brown 官方商品图）取九宫格平均色：

| 采样位置 | 24-bit | RGB565 |
|---|---|---|
| 机身左上（受光面） | `#A25D59` | `0xA2EB` |
| 机身左下（中间调） | `#974E4A` | `0x9269` |
| 机身右侧（背光面） | `#84342B` | `0x81A5` |
| 砖缝 | `#723631` | `0x71A6` |

注意：这不是 digimon-passport `SHELLS[]` 里的 `BROWN 0x8A5A3C`（那个偏棕黄），彩屏元祖的"Original Brown"实际是**偏红的砖红色**。

### 16.2 实现：直接生成 565，不用贴图

digimon-passport 的做法（`main/dg_brick.h`）是：`tools/gen_brick_tile.py` 预生成一张 **128×128 的 A8 alpha 蒙版**（砖缝 250、砖面 0），LVGL 用 `lv_image_tiled` 平铺，再按用户选的配色染成单色——因为那边有色号菜单，同一张 tile 要服务 8 种外壳色，A8 比 ARGB8888 省 48 KB。

本机没有 LVGL，也没有配色菜单，所以**直接程序化生成 RGB565**：

```cpp
static uint16_t brick_px(int x, int y);   // 28x11 砖，2 px 缝，交错半砖
static void panel_brick_band(int x, int y, int w, int h);
static void panel_draw_bezel(void);        // 窗口外的四条带
```

好处：**flash 里零字节**，没有 tint 过程，只要一行 `s_strip` 暂存。代价是每次重排都要重算——但那是一次的，不是每帧。

砖块参数：28×11 面板像素 + 2 px 砖缝，奇数行偏移半块；每块砖用 `col*2654435761 ^ course*2246822519` 取一个稳定的色差，**1/4 的砖偏暗**，避免整面墙看起来像印刷网格；每块砖最后一行压暗成 `kBrickBase` 当阴影。

### 16.3 什么时候画

| 时机 | 原因 |
|---|---|
| 开机 `panel_fill_black()` 之后 | 面板上电 GRAM 是随机噪声，先铺墙再 blit 窗口 |
| 关设置卡之后 | 卡片覆盖全屏，关闭后四条带要恢复（原 `panel_clear_side_margins()` 的活） |
| `E <left> [<top>]` 之后 | 窗口位置变了，墙要跟着边界重算 |
| `G 0` 之后 | `G 2`/`G 3` 会盖满整屏，退出诊断要恢复 |

四条带都按**绝对面板坐标**生成，所以石头在带与带之间是连着的，不会出现接缝错位。

成本：窗口外共 39,936 像素（320×240 − 192×192），每像素十几次运算，一次性约 5 ms 加 SPI 传输。**不进每帧路径**。

### 16.4 预览与验收

砖墙直推面板、不进 `fb[]`，所以 `T` 抓帧看不到它。`tmp/s31/bezel_preview.py` 用**同一套参数和哈希**离线渲染出 320×240，把真抓的游戏画面贴进窗口位置——预览和面板应当一致。

实机验收：`VER ver=1.4.0`，开机日志无 `brick band failed`，窗口内容正常。**观感（颜色深浅、砖块大小）只能目视**。

### 16.5 还可以再进一步

真机在屏幕与砖墙之间还有一圈**金属框 + 钥匙孔造型**。现在墙是直接怼着窗口边的，没有过渡。要做得更像，下一步是在窗口外沿加 2–3 px 的浅色内框（`kBrickLit` 提亮或中性灰），把画面"嵌"进墙里。

---

## 17. 2026-11-24 00:5x — 右下角电量百分比（v1.4.2）

### 17.1 硬件：电量计和音频 codec 共用一条 I2C

| 项 | 值 |
|---|---|
| 芯片 | CW2017，7 位地址 `0x63` |
| 总线 | I2C0，**SDA 10 / SCL 7**，与 ES8311（`0x18`）**同一对引脚** |
| 读法 | `0x04` 高字节 = 整数百分比；`0x02/0x03` = 14 bit 电压（312.5 µV/LSB） |

`audio.c` 已经在 `audio_init()` 里开好了这条总线并持有句柄，所以 `battery.c` **不另开总线**——同一对引脚上挂两个 master 是这块板子承受不了的。为此在 `audio.h` 加了一个访问器：

```c
void *audio_i2c_bus(void);   // NULL until audio_init() succeeds
```

用 `void*` 是为了不让 `driver/i2c_master.h` 渗进 `audio.h`。

顺序约束：`battery_init()` 必须在 `audio_init()` 之后，否则拿不到总线（电量功能整体失效，游戏照跑）。

### 17.2 第一次读到 0 %，以及为什么

v1.4.1 第一版只有"restart → active → 读 SOC"，实测：

```
batt: CW2017 v0F ready
DIGICORE: battery 0%
```

芯片在线、版本号读得到，偏偏 SOC 是 0，而且等了 100 秒没变。

补上 `ai-passport-tamago` 那套完整流程后（profile 比对 → 必要时睡眠态写 80 字节并校验、置 `UPDATE_FLAG` → 重启进 active），同一块板子读出：

```
batt: CW2017 v0F ready, 4103 mV, SOC 92%
DIGICORE: battery 92%
```

**要说清楚的一点**：日志里**没有**"writing the 500 mAh pack profile"，说明 `UPDATE_FLAG` 和 80 字节都已经在、比对通过，profile 并没有被重写。所以 0 % 更可能是**复位后芯片还没算完就被读**，而 v1.4.2 多出来的那串操作（读 80 字节约 80 ms、必要时再 `restart → active`）刚好给了它时间。结论写成这样，别记成"必须写 profile 才能工作"。

profile 的 80 个字节是这块板子的电池（万锂新能源 500 mAh，profile2 / XIL40X / 2026-10-20），逐字节抄自上游 `bsp_battery.c`。留着它是为了芯片被换过或 profile 被清掉时能自愈。

### 17.3 显示

| 项 | 值 |
|---|---|
| 位置 | 右下角，`bx = 320 − 5 − tw`，`by = 240 − 4 − 14`（"92%" 时 x 281..314，y 222..235） |
| 字体 | `font5x7.inc` 的 5×7 面，**2 倍**（10×14，步进 12），和设置卡同一套字形 |
| 颜色 | 白色 `0xFFFF` |
| 背景 | 文字没盖住的地方**露出砖墙**（逐像素先算 `brick_px()` 再叠字），不是黑框 |

画在墙上是刻意的：窗口内是模拟器输出的原机画面，一个像素都不该被 UI 占用；窗口外本来就已经归砖墙所有。

刷新：主循环里每 30 s 读一次，**只有数字变了才重画那个角**。菜单打开时不读（卡片会盖住整个面板，关菜单时 `panel_draw_bezel()` 会连电量一起重画）。

`font5x7.inc` 被 `app_main.cpp` 也 include 了一份（static 数组，295 B），没有去共享 `menu.cpp` 里那份——两个编译单元各一份，代价 295 B flash，省掉一次跨文件耦合。

### 17.4 实测

```
batt: CW2017 v0F ready, 4103 mV, SOC 92%
DIGICORE: battery 92%
```

电压 4103 mV 对应满电附近，和 92 % 自洽——这两个数一起看才知道读数是不是真的，只看 SOC 分不清"电池没电"和"芯片没算完"。

离线预览（`tmp/s31/bezel_preview.py`，同一套 `brick_px()` + 同一份字体表解析）能画出 "92%" 在右下角的样子，用来确认位置和对比度；面板上的实际效果仍需目视。

### 13.4 测试通道的副作用

`K 5 <n>`（注入 A+C）现在不会生效：`K` 注入走的是同一个 `mask` 表达式，A 的位被一并剥掉了，日志里只会看到 `ffdf`（只剩 C）。这不是 bug，是"A 一律不投递"的直接后果；要测组合键得另外加一条能绕过的命令。
- **「偏左」的观感来源仍未定位到物证**：要么是旧固件，要么是外壳。下次让用户确认当时装没装外壳、看到的是哪个版本，别再从代码里找。`G 2`（四边白框 + 四角方块）就是为这个留的。

---

## 18. 2026-11-24 01:0x — 壳换成 RETAIL (style C)（v1.5.0）

### 18.1 需求与参照

用户给了 digimon-passport 的 `docs/ui-retail-blank.png`（RETAIL / style C 主画面规格底版）和它的生成器
`tools/preview_retail_blank.py`，要求"**照那个项目的实现方法**做，砖块密度按原样抄，颜色选 A（DMC 红壳）"。

那个生成器的做法值得照搬，因为它把"预览"变成了断言：

```python
# 读固件同一批烘焙头文件，按 build_ui()/render_lcd()/blit_slot() 的规则逐像素复现
brick  = dg_brick.h                  # 128x128 A8
# 层序：body fill → tiled A8 recolour → rim r8 + 1 px hi → well r4 → glass → HUD → 30 px mask
```

本机没有 LVGL、没有 canvas，所以"层序"改成逐像素解算后按带推面板，但几何常量和颜色仍然只写在一处。

### 18.2 三处不能照抄，四处照抄

| 项 | 参考图 | 本机 | 理由 |
|---|---|---|---|
| 玻璃 | 192×183 @ (64,22) | **192×192 @ (64,24)** | 183 = `28+15+96+16+28`，是 32×16 点阵按 pitch 6 算的。本机是 96×96 彩屏 ×2，183 意味着纵向 0.953 缩放 |
| rim | 204×195 @ (58,16) | **204×204 @ (58,18)** | 跟着玻璃长 9 px；与 §10.5 早就算出的 204×204 对上 |
| 玻璃里面 | backdrop + 6% 网格 + 8 槽图标 | **模拟器原画面** | 网格是给点阵 ROM 分格用的；DMC 是彩屏像素画，叠网格=毁画 |
| 砖墙 | A8 平铺 + recolor | 照搬 | 同一块面板、同一张调好的 tile |
| rim + well | r8 + 1 px 高光 / r4 | 照搬 | 三层结构 |
| 30 px 遮罩 | BSP 显示层 | 照搬 | 本机以前没做，四角是砖 |
| HUD | 右下、-16/-4、`#EDEDE6` | 照搬（只留电量） | 左下时间槽本机没有对应数据 |

### 18.3 砖：照抄，不重算密度

第一次尝试按 `zoom_device.png` 量真机砖块（约 40×18 px）另生成了一张 96×48 的 tile——**用户否决**：
"砖块密度按 digimon 项目的原样抄过来，都是在 AI Passport 上运行，那个项目的最终呈现已经是最优了。"

于是 `tools/gen_brick_a8.py` 把 `dg_brick.h` 的 **16384 字节像素数据原样** 抄成 `main/brick_a8.h`，
只丢掉 LVGL 的 `lv_image_dsc_t`（本机没有 LVGL）。128×128、`ROWS=3`、石块 ~128×43、5 px 缝，一个像素没改，
平铺原点 (0,0)。那个 tile 里 15% 是砖缝（2495 px），其余是砖面。

左右墙各 58 px，与 digimon-passport 的 rim x=58 完全相同——**同一个壳、同一块屏，砖的落位本来就该一致**。

### 18.4 浮雕由 painter 补，不加素材

style C 那边砖面是平的（壳色可换，tile 只有 alpha）。DMC 是单一红壳，平面砖会读成印刷马赛克。所以
`bezel.cpp` 的 `stone_px()` 查 A8 的上下邻居：

```
当前像素是缝            -> kMortar
上一个像素是缝=砖块上缘 -> 亮 14%
下一个像素是缝=砖块下缘 -> 暗 16%
否则                    -> 砖面
```

两行判断，flash 零成本，`tmp/s33/style_c_preview.py` 里是同一段逻辑（预览才敢当断言用）。

### 18.5 颜色（选 A：DMC 真机）

| 用途 | 24-bit | RGB565 | 来源 |
|---|---|---|---|
| 砖面 | `#974E4A` | `0x9269` | `zoom_device.png` 采样 |
| 砖面高光 | +14% | `0xAACA` | 计算 |
| 砖面阴影 | −16% | `0x7A07` | 计算 |
| 砖缝 | `#6B302C` | `0x6985` | 采样后加深一档（红底需要更多对比） |
| 框体 | `#7A7470` | `0x7BAE` | 采样（金属中间调） |
| 框高光 | `#B7B1AF` | `0xB595` | 采样（金属受光面） |
| well | `#4A4341` | `0x4A08` | 采样（暗面） |
| 电量墨色 | `#EDEDE6` | `0xEF7C` | 与 style C 的 HUD 同色 |

灰壳（style C 原配色 `#3C4143` / `#808080`）也出了预览 `tmp/s33/B_greyfish_dg.png`，**用户选 A**。

### 18.6 代码落点：新模块 bezel.cpp

`app_main.cpp` 已经 2,700 行，这轮要加的是"另一个渲染器"，塞进去没法维护。新建：

| 文件 | 内容 |
|---|---|
| `main/bezel.cpp` / `bezel.h` | `bezel_paint()` / `bezel_draw_battery()` / `bezel_visible()`，几何与颜色常量都在这里 |
| `main/brick_a8.h` | 16,384 B A8 tile（从 dg 抄） |
| `tools/gen_brick_a8.py` | 抄 tile 的可重生成入口 |

`app_main.cpp` 删掉四段：`brick_px()`、`panel_brick_band()`、`panel_draw_battery()`、`panel_draw_bezel()`，
四处调用点改调 `bezel_paint()` / `bezel_draw_battery()`；`s_batt_pct` / `s_batt_next_us` 留在 app_main（刷新循环在那儿）；
加了两个 `static_assert` 把 `BEZEL_GLASS_*` 与 `kWinW/kScale/kLeftHome/kTopHome` 钉在一起。

### 18.7 三个连带点（每个都会咬人）

1. **关设置卡后的重画链**：`blit_frame()` 只推脏 tile，窗口外没人管。`bezel_paint()` 现在负责整个壳，
   且**故意把玻璃写黑**——调用方必须同时置 `s_blit_repaint_all`，否则窗口黑着不动。四处调用点都带着这个标志。
2. **`s_strip` 上限**：按 16 行一带渲染，最大 `320×16×2 = 10,240 B`，在 15,360 B 之内；rim 的圆角按行求 span，
   不会一次要 204×204×2 = 78 KB。
3. **`E` 挪窗口**：壳是固定的，玻璃洞口也在 (64,24)。`E` 之后画面会偏移到洞口外、盖在壳上——这是调试行为，
   文档标注，不修。

### 18.8 验证

```
VER ver=1.5.0 win=0,18 96x96 scale=2 left=64 top=24 off=0,0 home=64,24 fb=128 chunk=16
batt: CW2017 v0F ready, 4103 mV, SOC 92%

H 300 / K 1 60  -> menu: open / long A (345 ms) -> settings menu / pushed 10 band(s), render 78 ms
K 4 6           -> menu: close，STAT errs=0，blits 10 -> 20，blitpx=5009（全窗重推）
```

壳和电量都直推面板，`T` 抓帧看不到；`tmp/s33/style_c_preview.py` 用同一批常量离线渲染，预览应与面板一致，
**实际观感仍需目视**。

### 18.9 没做

- **钥匙孔造型**：真机屏幕右侧有一枚锁孔。本机右墙只有 64 px 宽，20×26 的锁会挤在框和砖之间；
  而且 style C 的版式就是"整面墙 + 一圈框"，加锁孔就不属于这套了。要的话另起一条分支。
- **左下 HUD**：参考图那里是游戏时长 `0:00`，本机没有对应数据；固件版本在设置卡 `VERSION` 行。

