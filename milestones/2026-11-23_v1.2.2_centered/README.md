# v1.2.2 — 画面居中 + 长按闪退修复（2026-11-23）

**里程碑判据：实机目视确认主画面左右居中，且长按 A / 长按 B 弹卡不再重启。**

| 项 | 值 |
|---|---|
| 版本 | `1.2.2`（`version.txt`） |
| 日期 | 2026-11-23 |
| 源码 | git tag `v1.2.2` |
| 串口自检 | `VER ver=1.2.2 win=0,18 96x96 scale=2 left=64 top=24 off=0,0 home=64,24 fb=128 chunk=16` |
| 目视 | 左右黑边各 64 px、上下各 24 px，对称；长按 A/B 稳定 |

---

## 一、画面居中：窗口取错了，不是算术错

MAME 对本机种写死的可见区（`ref/generalplus_gpce4.cpp:397`）：

```cpp
m_screen->set_visarea(0, 96-1, 18, 114-1);   // x 0..95, y 18..113 → 96x96
```

抓帧统计证实 ROM 只画这块：两张不同画面里非黑像素严格落在 x 0..95、y 18..113，矩形外全 0。

旧代码取 `kWinW=128 / kWinY=8 / kWinH=112`（全宽 + 更高切片）。2× 之后画面块 256 px 宽，**只有左边 192 px 有内容**，右边 64 px 是空的，而面板左边距只有 32 px —— 这就是"偏左"。

```cpp
kWinX = 0;  kWinW = 96;
kWinY = 18; kWinH = 96;
kLeftHome = (320-192)/2 = 64;
kTopHome  = (240-192)/2 = 24;
```

顺带解开历史谜团：早期"96 行窗口把底部图标切一半"的报告，是因为当时 `kWinY` 用了 8；从 18 起，96 行正好完整。

**副作用（正向）**：画面 256×224 → 192×192。小掉的 64×32 本来就是空的。源是 96×96，整数倍只有 2×（3× = 288 超过 240 高），非整数倍会糊，所以 192×192 是唯一可选项。

**外壳规格自动对上**：glass 192×192 @ (64,24)，rim +12 = **204×204 @ (58,18)**，well 198×198 @ (61,21)。这正是 `docs/HANDOFF_2026-11-23.md:202` 里拍的方案——它本来就是按 96×96 算的。

## 二、长按闪退：共享缓冲区被缩水

修完窗口之后出现的回归，v1.1.0 没有。

`s_strip` 是唯一的暂存区，有两个消费者：

| 消费者 | 需要 | 依据 |
|---|---|---|
| `blit_frame()` | 384 × 16 × 2 = **12,288 B** | 跟着 `kWinW` 算 |
| `menu_present()` / `menu_confirm_present()` | 320 × 2 × 24 = **15,360 B** | `menu.h:66` 明文契约，`menu.cpp` 里 `band_rows = 24` |

而 `s_strip` 的定义只按 blitter 算。当 `kWinW` 还是 128 时它是 16,384 B，**刚好**盖住菜单的 15,360；改成 96 之后变成 12,288，短了 3,072 B。卡片一画就写穿数组，踩坏相邻静态变量。

实机证据：

```
ACK K 1 40
I (2237) DIGICORE: long A (1414 ms) -> A+C confirm card
Guru Meditation Error: Core  0 panic'ed (Load access fault). Exception was unhandled.
  MEPC : 0x4200d5f6   RA : 0x4200d5e4   SP : 0x3fccefc0
rst:0xc (RTC_SW_CPU_RST)
```

`Load access fault` = 读非法地址，指针被踩坏的典型形状；卡片日志与 panic 只隔一帧，正好是第一次 `menu_confirm_present()`。

修法：两个尺寸具名、取最大、加 `static_assert` 钉死契约。

```cpp
static constexpr int kMenuBandRows = 24;                        // must match menu.cpp
static constexpr int kStripBlitBytes = kSrcRowStride * kChunkRows * kScale;
static constexpr int kStripMenuBytes = kDstW * 2 * kMenuBandRows;
static constexpr int kStripBytes =
    kStripBlitBytes > kStripMenuBytes ? kStripBlitBytes : kStripMenuBytes;
static_assert(kStripBytes >= kDstW * 2 * kMenuBandRows,
              "s_strip must hold one menu band or the menu writes past it");
```

结果 `kStripBytes = 15,360`。以后谁再动 `kWinW`/`kDstW`/菜单 band 行数，**编译期报错**，不用等上机闪。

## 三、连带必须改的四处

| 位置 | 为什么 |
|---|---|
| `panel_clear_side_margins()` | 边带 32→64 px，一次性清要 30,720 B，`s_strip` 只有 12 KB；改成按 `kChunkRows` 分 band |
| `panel_window()` / `kPanelWindowBytes` | fbsave 只认连续内存；按 `kWinW*kWinH` 存会从 96 列的行尾走进下一行行首，恢复出来是剪切错位。改成整行存 24,576 B |
| `s_strip` 尺寸 | 见上一节 |
| `T` dump 注释 | 56 KB → 37 KB |

## 四、验证（v1.2.2 实机）

| 动作 | 结果 |
|---|---|
| `VER` | `ver=1.2.2 win=0,18 96x96 left=64 top=24` |
| 抓帧 | 96×96，内容列 0..95 无空白 |
| 目视 | 左右各 64 px、上下各 24 px，居中 |
| 长按 A | `long A (1221 ms) -> A+C confirm card`，**无重启** |
| 长按 B | `long B (1525 ms) -> settings menu` + `menu: pushed 10 band(s), 320x240`，**无重启** |
| 按 C 关菜单 | 抓帧游戏画面完整，左右边带无残影 |

复现脚本 `tmp/s31/repro.py <mask> <frames> <seconds> [--reset]`，在日志里找 `ESP-ROM`/`rst:`/`Guru`/`wdt`/`canary` 标记判重启。

## 五、同批的诊断能力（v1.2.0 起）

| 命令 | 作用 |
|---|---|
| `E [<left> [<top>]]` | 画面落点在面板上的绝对坐标，现场校准不重烧 |
| `G 3` | 面板级对齐靶：刻度尺 + 绿框（当前落点）+ 中心十字 |
| `G 2` | 面板四边白框 + 四角方块，量玻璃物理边界 |
| `V` | 几何自证，含 `left/top/off/home` |

把画面块和面板坐标系分开画出来之后，"偏左"从"感觉"变成了"哪 32 列是空的"。

## 六、素材

| 文件 | 大小 | 说明 |
|---|---|---|
| `digicore-c3-v1.2.2.bin` | 1,178,176 B | **app 分区镜像**（0x10000 起），不含 ROM |
| `sha256.txt` | — | 归档件 + 两个可重建产物的指纹 |

**整片镜像不入库**：`digicore-c3-full.bin`（4,677,888 B）含 sparse ROM 分区，ROM 不进版本库。

## 七、重建与烧写

```powershell
cd firmware\digicore-c3
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1     # 重新合并 sparse ROM + app

$py="C:\Users\DuWeisi\.espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe"
& $py -m esptool --chip esp32c3 -p COM4 -b 460800 write_flash --flash_mode dio `
     --flash_freq 80m --flash_size 8MB 0x0 digicore-c3-full.bin
```

## 八、没做

- **30 px 圆角**：本机内容全在圆角安全区内，补了看不出变化；要显形必须先把外壳底纹铺满四角。详见 `docs/RESEARCH_SCREEN_AND_ROUNDING.md`。
- **B 外壳**：几何已对齐（rim 204×204 @ (58,18) / well 198×198 @ (61,21)），代码未做。
- 16 s 空闲 park 点、guest 实时率 ~27%，见 `docs/HANDOFF_NEXT.md` §4。
