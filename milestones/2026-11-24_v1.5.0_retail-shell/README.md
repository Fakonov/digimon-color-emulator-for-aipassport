# v1.5.0 — 壳换成 RETAIL (style C)（2026-11-24）

**里程碑判据：整屏是砖墙 + 一圈金属内框，玻璃画面嵌在框里，四角按面板的 30 px 圆角遮罩切黑；右下角电量百分比与设置卡照常工作。**

| 项 | 值 |
|---|---|
| 版本 | `1.5.0`（`firmware/digicore-c3/version.txt`） |
| 日期 | 2026-11-24 |
| 源码 | git tag `v1.5.0` |
| 串口自检 | `VER ver=1.5.0 win=0,18 96x96 scale=2 left=64 top=24 off=0,0 home=64,24 fb=128 chunk=16` |
| 电量 | `batt: CW2017 v0F ready, 4103 mV, SOC 92%` |
| app 镜像 | 1,199,792 B（分区 0x200000，占 57%，余 0xdb150 = 43%） |

---

## 一、为什么要换

v1.4.2 的壳是程序化生成的 28×11 规则砖网格，砖墙直接怼着窗口边，没有框、没有圆角。真机在屏幕和墙之间还有一圈金属框，屏幕本身是圆角面板。

用户给了 digimon-passport 的 RETAIL（style C）规格底版与它的生成器 `tools/preview_retail_blank.py`，要求**照那个项目的实现方法**重做，砖块密度原样抄，颜色选 DMC 真机的红色。

## 二、参照图的实现方法（照搬的部分）

那个生成器不画示意图，它读固件里同一批烘焙头文件，按 `build_ui()` / `render_lcd()` / `blit_slot()` 的规则逐像素复现，所以预览可以直接当断言用。层序：

```
body 底色
  → 128x128 A8 砖墙平铺 + recolor
  → rim     (204x204) r8 + 1 px 高光
  → well    (198x198) r4
  → glass   玻璃内容
  → HUD     右下电量（-16/-4）
  → 全局 30 px 圆角遮罩（四角涂黑）
```

本机没有 LVGL，四层改成逐像素解算后按 16 行一带推面板，几何与颜色只写在一处（`bezel.cpp`）。

## 三、三处不能照抄

| 项 | 参考图 | 本机 | 理由 |
|---|---|---|---|
| 玻璃 | 192×183 @ (64,22) | **192×192 @ (64,24)** | 183 = `28+15+96+16+28`，是 32×16 点阵按 pitch 6 算的；本机 96×96 彩屏 ×2，183 会变成 0.953 纵向缩放 |
| rim | 204×195 @ (58,16) | **204×204 @ (58,18)** | 跟玻璃长 9 px，与 `HANDOFF_UI_POSITION.md` §10.5 的推导一致 |
| 玻璃里 | backdrop + 6% 网格 + 8 槽图标 | **模拟器原画面** | 网格是给点阵 ROM 分格的，彩屏像素画叠网格就毁了 |

## 四、砖：照抄，密度未改

`tools/gen_brick_a8.py` 把 `digimon-passport/.../dg_brick.h` 的 **16,384 字节 A8 数据原样**抄成
`firmware/digicore-c3/main/brick_a8.h`，只丢掉 LVGL 的 image descriptor。

- 128×128，`ROWS=3`，石块约 128×43，缝 5 px，平铺原点 (0,0)
- 砖缝占 2,495 px（15%），其余是砖面
- 左右墙各 58 px，与 digimon-passport 的 rim x=58 完全相同

中途按真机照片量出"砖块约 40×18"另生成过一张 96×48 的 tile，被用户否决：两台机器是同一块面板，
那个项目的成品观感是调过多轮的，改密度就不是它了。旧图留在 `tmp/s33/A_dmc_dmc.png` 作反面参照。

## 五、浮雕：painter 补，不加素材

style C 那边砖面是平的（壳色可换，tile 只有 alpha）。本机是单一红壳，平面砖读成印刷马赛克。`bezel.cpp` 的
`stone_px()` 查 A8 的上下邻居：**砖块上缘 +14%、下缘 −16%**。两行判断，flash 零成本。

## 六、颜色（用户选 A：DMC 真机）

| 用途 | 24-bit | RGB565 |
|---|---|---|
| 砖面 | `#974E4A` | `0x9269` |
| 砖面上缘 | +14% | `0xAACA` |
| 砖面下缘 | −16% | `0x7A07` |
| 砖缝 | `#6B302C` | `0x6985` |
| 框体 | `#7A7470` | `0x7BAE` |
| 框高光 | `#B7B1AF` | `0xB595` |
| well | `#4A4341` | `0x4A08` |
| 电量墨色 | `#EDEDE6` | `0xEF7C` |

全部采自 `tmp/dmc_photos/zoom_device.png`。另一版灰壳（style C 原配色）预览在 `tmp/s33/B_greyfish_dg.png`。

## 七、代码

| 文件 | 变化 |
|---|---|
| `main/bezel.cpp` / `bezel.h` | **新增**：壳的四个层 + 30 px 遮罩 + 电量角，`bezel_paint/strip` 接口 |
| `main/brick_a8.h` | **新增**：16 KB A8 tile（从 dg 抄） |
| `tools/gen_brick_a8.py` | **新增**：可重生成的抄写入口 |
| `main/app_main.cpp` | 删 `brick_px` / `panel_brick_band` / `panel_draw_battery` / `panel_draw_bezel`；四处调用改调 bezel；加两个 `static_assert` 钉几何 |
| `main/CMakeLists.txt` | 加 `bezel.cpp` |

## 八、三个连带点

1. **关设置卡后的重画链**：`blit_frame()` 只推脏 tile，窗口外没人管。`bezel_paint()` **故意把玻璃写黑**，
   调用方必须同时置 `s_blit_repaint_all`（四处调用点都带着）。
2. **`s_strip` 上限**：按 16 行一带渲染，最大 10,240 B，在 15,360 B 内；圆角按行求 span，不会要 78 KB。
3. **`E` 挪窗口**：壳固定、玻璃洞口固定；`E` 之后画面会偏出洞口盖在壳上，这是调试行为，不修。

## 九、验证

```
VER ver=1.5.0 win=0,18 96x96 scale=2 left=64 top=24 off=0,0 home=64,24 fb=128 chunk=16
batt: CW2017 v0F ready, 4103 mV, SOC 92%

H 300 / K 1 60 -> menu: open / long A (345 ms) -> settings menu / pushed 10 band(s), render 78 ms
K 4 6          -> menu: close, STAT errs=0, blits 10 -> 20, blitpx=5009
```

壳与电量直推面板，`T` 抓帧看不到。`tmp/s33/style_c_preview.py` 用同一批常量离线渲染 320×240
（`tmp/s33/A_dmc_dg.png`），预览应与面板一致；**实际观感仍需目视**。

## 十、素材

| 文件 | 大小 | 说明 |
|---|---|---|
| `digicore-c3-v1.5.0.bin` | 1,199,792 B | app 分区镜像（0x10000 起），不含 ROM |
| `sha256.txt` | — | 归档件 + 两个可重建产物的指纹 |

整片镜像不入库：`digicore-c3-full.bin`（4,677,888 B）含 sparse ROM 分区。

## 十一、重建与烧写

```powershell
cd firmware\digicore-c3
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1

# 只烧 app（不动 bootloader / ROM / 存档）
$py="C:\Users\DuWeisi\.espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe"
& $py -m esptool --chip esp32c3 -p COM4 -b 460800 write_flash --flash_mode dio `
     --flash_freq 80m --flash_size 8MB 0x10000 build\digicore-c3.bin
```

**版本号在 `firmware/digicore-c3/version.txt`**，不是仓库根目录。改完要碰一下
`main/CMakeLists.txt` 才会重新 configure，否则 `-- App version` 还是旧的（这一轮踩过）。

## 十二、没做

- **钥匙孔造型**：真机屏幕右侧有一枚锁孔；本机右墙只有 64 px 宽，20×26 的锁会挤在框和砖之间，
  而且 style C 的版式就是"整面墙 + 一圈框"，加锁孔就不属于这套。
- **左下 HUD**：参考图那里是游戏时长 `0:00`，本机没有对应数据；固件版本在设置卡 `VERSION` 行。
- **A+B 墓碑验收**、**A+C 时钟复验**、**音频通路**、**guest 实时率**：见 `docs/HANDOFF_2026-11-24.md` §6。
