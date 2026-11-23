# 研究：这台机器的屏幕规格与 30 px 圆角（2026-11-23）

> 结论先行：**30 px 圆角不是 digimon 项目的发明，是 FoloToy 官方 BSP 的显示接入层行为**，
> digimon 只是继承了它。本机（digimon-color-passport）在同一块板上漏掉了这一层。
> 但——本机的游戏画面、B 外壳、菜单卡片**全部落在圆角安全区内**，所以现在补遮罩看不出变化；
> 只有把外壳色/底纹**铺满 320×240 四角**时，圆角才会显形。
>
> 本文所有数字都有出处，标注为「源码实测」或「官方文档」，没有回忆和估算。

---

## 1. 屏幕：官方规格

来源：`C:\Users\DuWeisi\digimon-passport\firmware\ai-passport-tamago-main\docs\hardware-design\`

| 项 | 值 | 出处 |
|---|---|---|
| 面板 | ST7789P3，**240 × 320** 彩色 TFT，RGB565 | `specifications.zh_CN.md:15` |
| 形态 | 可穿戴、**透明外壳**，60 × 95 × 8.5 mm，50 g | `specifications.zh_CN.md:11-13` |
| MCU | ESP32-C3，8 MB flash，无 PSRAM | 同上 |
| 总线 | SPI2_HOST、MOSI-only、80 MHz、mode 0、8 bit 命令/参数 | `AI_HARDWARE_DEVELOPMENT_GUIDE.zh_CN.md` §5.1 |
| 反色 | `BSP_LCD_INVERT_COLOR=1`，出厂就需 INVON（参考例程末尾无条件发 0x21） | `bsp_pins.h` |
| RST | `-1`，未接 MCU，`esp_lcd_panel_reset()` 走 SWRESET | 同上 |
| gap | **(0, 0)**，mirror X/Y 都关闭（竖屏原生态） | §5.1 |
| 初始化表 | 厂商 porch/power/gamma，**不是通用 ST7789 默认值** | §5.1 |

### 与本身份核对（`firmware/digicore-c3/main/display.c`）

引脚 MOSI 9 / SCLK 8 / CS 1 / DC 20 / BL 21、80 MHz、mode 0、`invert_color(true)`、
`set_gap(0, 0)`、`reset_gpio_num = -1`、厂商 `ST7789P3_CMDS` —— **逐项与官方一致**。
唯二差异：本机 `swap_xy(true) + mirror(true,false)` 直接出横屏（官方是在 LVGL 层做
`lv_display_set_rotation(90)`），效果等价（MADCTL MV + 一个 mirror）。

---

## 2. 圆角 30：是谁定的

### 2.1 官方规定

`AI_HARDWARE_DEVELOPMENT_GUIDE.zh_CN.md` §5.2 原文：

> LVGL 最终输出的 RGB565 刷新区域会**统一套用 30 px 圆角遮罩**，因此正常刷新和页面切换期间，
> 圆角之外的四角区域都会保持纯黑。遮罩直接作用于局部绘制缓冲，不使用根 screen 的 `clip_corner`；
> 全屏圆角裁剪需要 ARGB 中间图层，在本项目无 PSRAM、LVGL 内存池仅 24 KB 的条件下可能耗尽内存。
> **该行为统一放在显示接入层，不应在各页面重复绘制四角装饰。**

### 2.2 来源链（源码实测）

| 环节 | 证据 |
|---|---|
| 上游开源项目 | `firmware/ai-passport-tamago-main/components/bsp/src/bsp_display_rounding.c`（2042 B）+ `.h` + `tests/test_bsp_display_rounding.c` |
| digimon 的 BSP 出处 | `digimon-passport/components/bsp/src/bsp_display.c:2`：`移植自 trae_card/components/platform/platform_esp32/src/disp_st7789.c` |
| 引入时间 | `git log -S "BSP_LVGL_SCREEN_RADIUS"` 只命中 `dbb04f5 初始提交` 与 `ed9f0c2 收口快照` —— 项目第一天就有 |
| 半径定义 | `digimon-passport/components/bsp/include/bsp_display.h:11`：`#define BSP_LVGL_SCREEN_RADIUS 30` |

**"digimon 首次做画面就做到了圆角"的真相：它照抄了官方 BSP，遮罩本来就在里面。**
本机从同一支抄面板初始化时，把 rounding 那一段丢了。

### 2.3 横屏的坑（上游经验）

`docs/reference/shinku-chen/landscape-rotation-and-deep-sleep-key-wake.zh_CN.md`：

- 横屏 = `lv_display_set_rotation(90)` → MADCTL **MV + 一个 mirror**，不占 CPU、不额外缓冲。
- **遮罩必须读当前逻辑分辨率**：曾写死 `240 × 320`，横屏下**静默把一条边涂黑**；
  现在向显示驱动询问逻辑尺寸，同一个 30 px 半径在横竖屏都成立。
- 旋转方向（MV+MX 还是 MV+MY）只能上机确认，别从手册推。

---

## 3. 圆角几何算法（可直接搬）

`bsp_display_rounding.c`，纯函数，无状态：

```
radius = min(radius, min(w, h) / 2)                       /* 先夹紧 */
中段行  y ∈ [radius, h-radius)   → 整行 [0, w-1]
边角行  edge_y = y < radius ? radius - y : y - (h-1-radius)
        inset  = max{ i : (i+1)² + edge_y² ≤ radius² }     /* 整数循环，≤ radius 次 */
        span   = [radius - inset, w - radius + inset - 1]
```

逐像素判定（host 断言用）：

```
dx = 距最近竖向圆心的水平距离；dy = 距最近横向圆心的垂直距离
不在圆角带内 → 可见；dx² + dy² > radius² → 被切
```

r=30、320×240 的实测表（`test_bsp_display_rounding.c` 就是这么断言的）：

| 行 y | edge_y | inset | 可见区间 | 宽 |
|---|---|---|---|---|
| 0 | 30 | 0 | [30, 289] | 260 |
| 29 | 1 | 29 | [1, 318] | 318 |
| 30…209 | — | — | [0, 319] | 320 |
| 239 | 30 | 0 | [30, 289] | 260 |

角点断言：`(29,0)` 切 / `(30,0)` 见；`(8,8)` 切 / `(9,9)` 见；竖屏 240×320 与横屏 320×240 两种朝向都要过。

---

## 4. 本机现状验算（源码 + 30 px 半径逐点算）

| 元素 | 位置与尺寸 | 是否侵入圆角区 |
|---|---|---|
| 游戏画面 256×224 | (32, 8) | 否，全在 `[30,289]` 内 |
| B 外壳 rim 268×236 | (26, 2) | 否 —— 角点 dx=4, dy=28 → 16+784=800 ≤ 900 |
| well 262×230 | (29, 5) | 否 |
| 菜单卡片 300×212 | (10, 16) | 否 —— dx=20, dy=14 → 400+196=596 ≤ 900 |
| 菜单卡片自带圆角 | `menu.cpp kCardRound[8] = {8,4,3,2,1,1,0,0}` | 已有 8 px，与 30 px 遮罩无关 |

**结论：本机现在加 30 px 遮罩，肉眼看不到任何变化。** 四角本来就是黑的（`panel_fill_black()` 开机填黑），
没有任何有色像素画到 x<30 或 y<30 的角区。

对比证据：`digimon-passport/docs/ui-retail-check/retail_main.png` —— 320×240 整屏铺砖纹 +
四角切成圆弧，圆角轮廓才显形。

---

## 5. 三条路（等用户拍板，别自己选）

| 选项 | 内容 | 是否需要 30 px 遮罩 |
|---|---|---|
| A | 保持黑底 + 画面居中 | 不需要（遮了也看不见） |
| B | 仿 digimon：外壳底纹**铺满 320×240** + 四角切圆 + 中间玻璃窗 | **必须**，这就是遮罩存在的理由 |
| C | 保持黑底，只给画面块/卡片画自己的圆角 | 不需要 30 px 遮罩，画 8/4 px 即可 |

落地要点（走 B 时）：

1. 遮罩必须落在**逐像素打包那一层**（本机 = `blit_frame()` 的 band 循环），
   不能整帧刷完再盖一层，下一个 band 会把它涂掉。
2. 写 `0` 而不是"黑色值"：0 在 RGB565 与 RGB565_SWAPPED 两个字节序下都是黑，
   不用判格式。（`AUDIT-R3.md:157` 就在这条上翻过车：换格式后格式判断写死导致遮罩静默失效。）
3. 菜单/确认卡是直推面板的，遮罩要在 `menu_present()` / `menu_confirm_present()` 里同样套一遍，
   否则"开菜单时四角变方"。
4. 半径用 30，分辨率用**当前逻辑尺寸**（本机 320×240），不要写死竖屏 240×320。

---

## 6. 参考文件索引

| 文件 | 用途 |
|---|---|
| `digimon-passport/docs/PROMPT-UI-centering-and-corners.md`（188 行） | 尺寸推导 → 居中 → 三层 → 圆角 → 扫行交集 → 三个别做 → 验收顺序 |
| `digimon-passport/docs/PROMPT-UI-carryover.md`（415 行） | §2 视觉规格、L266–302 二十条陷阱、L293 HUD 16 px |
| `digimon-passport/docs/HANDOFF.md:866` | 「显示接入层对每次 flush 都套圆角，四角 30 px 内会被切」 |
| `digimon-passport/docs/AUDIT-R3.md:157` | 字节序格式判断导致遮罩静默失效 |
| `digimon-passport/tests/test_bsp_display_rounding.c`（56 行） | 可整段搬的 host 断言 |
| `ai-passport-tamago-main/docs/reference/shinku-chen/landscape-rotation-and-deep-sleep-key-wake.zh_CN.md` | 横屏与遮罩分辨率 |
