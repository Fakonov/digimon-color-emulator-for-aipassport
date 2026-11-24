# 第三方组件与素材

本工程的代码以 **GPL-2.0-or-later** 分发（见 `LICENSE`）。它不是从零写的：
模拟器核心是 MAME 的移植，外壳版式来自姊妹工程。各自的条件如下。

## 代码来源

| 来源 | 用途 | 许可 | 说明 |
|---|---|---|---|
| [MAME](https://github.com/mamedev/mame) | `main/emu/gpce4.cpp`：GPCE4 SoC（内存映射、IO、定时器、IRQ、SPI2）；`main/emu/isa/unsp_*.cpp`：unSP 2.0 指令集（**未修改副本**）；`st7735.cpp`：LCD 控制器模型；`generic_spi_flash` 行为 | **GPL-2.0-or-later** | 移植基于 MAME 0.289 的 `generalplus_gpce4_soc.cpp` / `generalplus_gpce4.cpp`。这是本项目必须 GPL 的原因 |
| [MAME](https://github.com/mamedev/mame) | `main/assets/internal_rom_221017a.bin`：GPCE4 SoC 的 64 KiB 内部 mask ROM | GPL-2.0-or-later | 随 MAME 的 `generalplus` 驱动分发，不是游戏卡带内容 |
| [digimon-passport](https://github.com/)（姊妹工程） | `main/brick_a8.h` 与 `tools/gen_brick_a8.py`：128×128 A8 砖纹及其生成算法；RETAIL (style C) 的外壳几何与配色规格 | MIT | 同一块面板、同一套外壳观感，像素数据逐字节沿用 |
| [FoloToy AI Passport](https://github.com/FoloToy) | 硬件抽象：屏幕初始化表、按键电阻梯、ES8311 音频、CW2017 电量计 | MIT | 厂商 BSP 与例程 |
| Espressif ESP-IDF | 构建系统与驱动 | Apache-2.0 | 作为依赖引入，未修改 |

GPL-2.0-or-later 与上表中的 MIT 组件兼容；分发本工程或其衍生作品时，整体按 GPL-2.0-or-later 处理。

## 不随仓库分发的素材

| 内容 | 原因 |
|---|---|
| `roms/*.bin`（8 MiB 卡带镜像） | Bandai 版权作品。用你自己的镜像，见 `roms/README.md` |
| `firmware/*/sparse/*.bin`（稀疏页表） | 同上，它就是卡带镜像的页表化形式 |
| 原机说明书扫描件、商品图 | 版权归原出版方 / Bandai |
| 数码兽立绘、名称、商标 | 归 Bandai / 东映所有 |

## 关于逆向研究数据

`docs/` 里的分析（RAM 地址、UI 状态机、定时器行为）是从**用户自己的镜像**跑出来的**观测数据**，
不含原镜像字节。记录的是"这个程序怎么行为"，不是程序本身。

## 商标

"Digital Monster"、"デジタルモンスター"、"Digimon"、"数码宝贝" 等名称与形象是 Bandai 的商标。
本项目与 Bandai、FoloToy 均无隶属关系，也未获其背书。
