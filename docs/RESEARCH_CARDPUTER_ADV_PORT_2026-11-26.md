# 移植可行性研究：digimon-color-passport → M5Stack Cardputer ADV

调研日期：2026-11-26　｜　性质：**只读调研**（未改固件、未烧机、未建工程、未动 launcher）
调研对象：`C:\Users\DuWeisi\digimon-color-passport`（v1.12.0，`50fed98`）→ Cardputer ADV（ESP32-S3FN8）

标注约定：**【实测】** = 本机/本仓库测过；**【官方】** = 芯片/框架文档明确；**【推算】** = 由实测或官方数字算得；**【估算】** = 判断，未验证。

---

## 0. 先说结论

1. **可行，而且模拟器核心一行都不用改。** `emu/` + `aot_blocks.c` 是与 ISA 无关的 C/C++，PC 与 C3 两套已经证明过；要动的是 **13 个板级文件**（`app_main.cpp` 170 KB 改几何/帧循环，`menu.cpp` 32 KB + `bezel.cpp` 10 KB 重画，其余是显示/键盘/音频/电池/存储），模拟器一行不碰。
2. **性能会明显变好，量级约 2 倍（1.9–2.2×，规划按 2.0×）。** C3 现在顶到 **27–30 M guest cycles/s**，按项目自己的 48 MHz 真机标尺只有 **0.57–0.62×**；S3 能把它推到 **50–65 M**，于是 `/180` 稳 60 fps 且余量 40%+，`/120` 刚好落在真机速度（余量 10–20%）。
3. **两条硬约束，不是软件能绕的：**
   - **屏幕**：96×96 在 240×135 上只能 1:1，画面约 **10 mm**（现在 Passport 是 2× ≈ 24 mm）。1.4× 是非整数缩放，像素宽窄不一，比 1:1 更难看。
   - **ROM 必须留在 flash 并 mmap**。走 SD 会把性能优势吃光——仓库里已有现成证据：mmap 失败回落 64 KiB 页缓存时，解释器掉到 **0.7 M instr/s**（`rom_esp.cpp` 注释）。
4. **工作量 10–14 人日**（接进 BATON 当应用的口径：app 进 `ota_0`，ROM 塞进 launcher 那块**空着的 `storage` 分区**，**不用改分区表、不用刷 bootloader**）。想彻底绕开 BATON 就做独立整片固件，少 1–2 天，代价是不能和 digimon-cardputer / baton-reader 共存。

---

## 1. 被移植方的现状（数字全部来自仓库）

### 1.1 硬软件基线

| 项 | 值 | 出处 |
|---|---|---|
| 目标板 | FoloToy AI Passport，ESP32-C3（单核 RV32IMC） | `docs/C3_PORT.md:4` |
| CPU 主频 / 内存 | 160 MHz / 400 KB SRAM（DIRAM 384 KB），**无 PSRAM** | 同上 + `soc.h` |
| Flash | 8 MB，**`sdkconfig` 是 DIO @80 MHz，`sdkconfig.defaults` 却是 QIO**（第 5 轮改过、没写结论） | `sdkconfig` / `sdkconfig.defaults` 对比 |
| 指令 cache | **16 KB / 8 路 / 32 B 行，硬连线不可配** | `rom/cache.h` + `docs/RESEARCH_EXT_C3_FETCH_PATH_2026-11-26.md` |
| 屏幕 | 320×240 ST7789P3，SPI2 @80 MHz，96×96 可见区 **2× → 192×192**，居中 (64,24) | `main/display.c`、`app_main.cpp` |
| 输入 | 三键 ADC 电阻梯（A/B/C） | `main/buttons.c` |
| 音频 | ES8311 @0x18 + I2S0，**SoC 出 MCLK = 256×fs** | `main/audio.c:12-14,48-54` |
| ROM | 8 MiB 镜像 → **2,515,200 B 稀疏页存**（611/2048 页非零）→ `roms` 分区 2.5 MB，**mmap 零拷贝** | `partitions.csv`、`rom_esp.cpp` |
| 固件 | app **1,021,840 B**；Flash Code 611,764 B；DRAM **208,552 B**（余 112,744 B） | v1.12.0 里程碑 |
| AOT | **1,466 块 / 14,847 条原生长指令 / 块体 673 KB** | `main/CMakeLists.txt` 注释 |

### 1.2 性能现状（v1.8.0 之后一直沿用这套度量）

| 场景 | 帧率 | guest 占帧时间 | 说明 |
|---|---|---|---|
| 静止（宠物待机，park） | **59.3** | **62%** | 完全可玩 |
| 单次换屏（时钟界面，`pc=200343`） | 51（`/180`，`gmax` 19.9 ms） | 满 | 同样场景 `/60` 要 **51.7 ms → 18.5 fps**（`dbgtest/r5_backdio.log`） |
| 连续重绘（当前） | 由帧预算决定 | 满 | 帧预算 `/180` = 533,333 guest cycles/帧 |

- 吞吐：`/180` **27.4 M cycles/s**，`/60` **29.6 M cycles/s**（`app_main.cpp:69-85` 实测注释）。
- 真机标尺：GPCE4064B 官方最高 **48 MHz**，折算 **23.4–26.9 M guest instr/s**（CPI ≈ 1.78，`FEASIBILITY.md` §8 补记）。
- 换算：**48 M cycles/s ≈ 真机速度**；现在 27–30 M → **0.57–0.62×**。这就是玩家感觉"宠物长得慢、动作慢"的根，也是 TIME LAPSE 必须存在的根。
- **口径声明**：48 MHz 这个数来自 GeneralPlus 产品页/`ref/gpce4064a_ds.pdf` 的**推断**（项目原文："强证据推断"），而 `kCpuClock` 用的是 MAME 的 96 MHz 模型。若真机其实是 96 MHz，本文所有"相对真机倍数"减半（0.29–0.31×），但**S3 相对 C3 的收益倍数不受影响**（§5 全部按相对值算）。

### 1.3 瓶颈拆解（为什么换 S3 有用）

| 项 | 实测 | 含义 |
|---|---|---|
| 每次 AOT dispatch | **689 主机周期** | 查表+上下文 68（9.9%）、块体 532（77%） |
| 每 guest 指令（仅 AOT 路径） | **≈63 主机周期** | 块平均覆盖 11.0 条 guest 指令 |
| 块体代码总量 vs ICache | **673 KB vs 16 KB** | 移动场景"几乎每次 dispatch 都 miss"（CMakeLists 注释原话） |
| 端到端（含解释器路径） | 110 周期/guest 指令（v1.6 口径） | 92% 的时间在 guest，渲染只占 1–5% |

**结论**：这不是"渲染太慢"，是**取指 + dispatch 太贵**。所以"主频、IPC、cache、flash 带宽"四项任何一项改善都直接兑现，而"再优化 blit"没有意义。

---

## 2. 目标硬件：Cardputer ADV 实测基线

| 项 | 值 | 来源 |
|---|---|---|
| SoC | **ESP32-S3FN8**，Xtensa LX7 双核，240 MHz（默认 160） | 【实测】BSP 硬件笔记 |
| 内存 | 512 KB SRAM，**可用 DRAM ≈321 KB**（+21 KB +32 KB +7 KB RTC）；**无 PSRAM** | 【实测】heap_init |
| Flash | 8 MB，**GD，quad，当前 DIO** | 【实测】eFuse |
| Cache | **I 16/32 KB（默认 16 KB！）、D 16/32/64 KB，均可配** | 【官方】`esp32s3/Kconfig.cache` |
| 屏幕 | **240×135 ST7789V2，SPI2 @40 MHz**，控制器 240×320，gap(40,53)，invert+swap+mirror | 【实测】BSP |
| 键盘 | TCA8418，7×8 物理 → 4×14 逻辑，56 键，INT=G11 | 【实测】 |
| 音频 | ES8311 @0x18 + I2S（BCLK41/WS43/DOUT42/DIN46），**MCLK 未接**，NS4150B 常开 | 【实测】BSP |
| 电池 | ADC1_CH9（GPIO10，1/2 分压），1500–1750 mAh（两个来源不一致） | 【实测】/【官方】 |
| SD | **独立 SPI3**（CLK40/MOSI14/MISO39/CS12），与 LCD 不共享总线 | 【实测】digimon BSP |
| 分区表（板上真表） | bootflag 64 KB / factory 1 MB / **ota_0 3 MB** / **storage 3.875 MB @0x420000（launcher 未使用）** | `launcher/partitions.csv` |
| 控制台 | **必须** USB-Serial/JTAG | BSP 铁律 |

两个立刻能用上的事实：
1. **`storage` 那 3.875 MB 是空的**（launcher 源码里没有任何 SPIFFS 调用），正好装 2.5 MB 的稀疏 ROM，**不用改表、不用刷 bootloader**（见 §4A）。
2. **S3 的 32 KB ICache 不是默认值**，默认还是 16 KB（`default ESP32S3_INSTRUCTION_CACHE_16KB`）。不显式打开等于白换芯片。

---

## 3. 可行性：逐模块清单

| 模块 | 现实现 | Cardputer 上要做什么 | 工作量 | 风险 |
|---|---|---|---|---|
| `emu/gpce4.*`、`emu/isa/*`、`emu/unsp_glue.cpp`、`emu/aot_blocks.c` | GPCE4 SoC + unSP + AOT | **零改动**（ISA 无关；端序同为小端；不依赖 C3 特性） | 0 | 低 |
| `emu/st7735.cpp` | 128×128 guest 帧缓冲 | 不动（窗口仍是 96×96） | 0 | 低 |
| `display.c` | ST7789P3 320×240 / SPI2 / 80 MHz | 改 240×135、40 MHz、gap(40,53)、同一套 invert/swap/mirror | 0.5–1 d | 低 |
| blit 几何 | 96×96→192×192 @2×，(64,24) | **96×96 @1×**，左 72 / 上 19；脏行照旧 | 0.5 d | 低 |
| `bezel.cpp` + `brick_a8.h` | 320×240 砖墙 + 30 px 圆角遮罩 + 电量 | **重画 240×135**（侧栏放钟/电量/lapse），圆角遮罩作废 | 2–3 d | 中 |
| `menu.cpp` | 280×84 卡片 / 8 项 | 8 行全屏列表（8 px 字体，行距 11）+ 键盘上下/确认 | 1–2 d | 低 |
| `buttons.c` | ADC 三键 + 长按 | TCA8418 → Q/W/E（与 digimon-cardputer 同键位）+ 长按 | 0.5 d | 低 |
| `audio.c` | ES8311 + MCLK 12.288 MHz | **无 MCLK**：改 BCLK 派生时钟的寄存器序列（BSP 的 esp_codec_dev 通路已验证） | 1 d | 中 |
| `battery.c` | CW2017 电量计 | ADC 线性映射（BSP 现成） | 0.2 d | 低 |
| `wallclock.cpp` | NVS 纪元 + 主机对时 | 不变 | 0 | 低 |
| `saveimg/gueststate/fbsave` | 128 KB `save` 分区（0x41） | 改 **SD 文件**（原子 rename，digimon 已验证）或分区 | 1 d | 低 |
| `rom_esp.cpp` | `roms` 分区 + mmap | 分区换位置（0x420000）或独立固件自带表；mmap 原样 | 0.5 d | 中 |
| `dmc_link.cpp` | UART0 + USB-JTAG 双通路 | 只用 USB-JTAG；或改成"从 SD 供料" | 0.5 d | 低 |
| 构建 | target=esp32c3 | target=esp32s3 + BSP 组件 + sdkconfig（240 MHz / QIO / 32 KB IC / 关 WiFi / USB-JTAG console / 8 MB） | 0.5 d | 低 |
| BATON 集成（可选） | — | `PROJECT_VER`、镜像头、3 MB 槽校验、打包脚本（**不改表**，ROM 进 `storage`） | 1–2 d | 低 |

**一句话**：模拟器是"拖过去就能跑"的部分；真正的工作量在**屏幕重新排版**和**存储路线选择**。

---

## 4. ROM 存储：三条路线，只有一条不亏性能

### A. 直接占用 launcher 的 `storage` 分区（**不改分区表**）【推荐】

- 现表尾部：`storage, data, spiffs, 0x420000, 0x3E0000`（3.875 MB，**launcher 里没有任何 spiffs 调用，全空**）。
- 做法：**原样保留这张表**，把 2.5 MB 稀疏 ROM 裸写进 0x420000，然后用
  `esp_partition_find_first(DATA, DATA_SPIFFS, "storage")` + `esp_partition_mmap()` 映射。
  mmap 只认 flash 区间，不认 subtype；偏移 0x420000 与长度 0x280000 都是 64 KB 对齐，条件满足。
- 收益：**bootloader / 分区表 / factory / ota_0 全都不动**——没有变砖路径，只写一块数据区；性能与今天完全一致；SD 的 SPI3 腾出来（想外接 320×240 副屏时正好用得上）。
- 代价：这块分区名义上叫 `storage`/spiffs，实际是裸数据，**必须在 README 和代码注释里钉死"别在这块上挂 SPIFFS"**；存档要么在同一分区尾部切 128 KB，要么放 SD。
- 可选洁癖版：把表里的 `storage` 改名为 `roms`（data/0x40）、再切 128 KB `save`（data/0x41）。偏移不变、现有分区不受影响，但要重刷分区表（0x8000 一扇区），风险从这里才开始。**第一版不要做。**

### B. ROM 放 SD + 64 KB 页缓存【否决】

- 页缓存 16 项 × 4 KiB；**193 KiB 的 guest 代码**会把它冲穿，仓库里实测解释器掉到 **0.7 M instr/s**（`rom_esp.cpp` 注释）。
- AOT 之后代码取指大部分不走 ROM 了，但**重绘帧的数据读（图块/位图）仍然走**，而重绘帧正是唯一还慢的场景——同一份工作集，同一个坑。
- 量化：4 KiB 页一次 SD 读（SPI3 @20 MHz）≈ **1.6 ms + 开销**；一帧 16.7 ms 只容得下 2–3 次 miss。要靠 64–110 KB RAM 做到 99.9% 命中，**没有把握，且 S3 的可用 DRAM 反而比 C3 少（321 vs 393 KB）**。
- 结论：省下的工作量会被性能吃掉，不做。

### C. 独立整片固件（自带分区表，不接 BATON）【备选：绕开 launcher 的最省事验证版】

- 自己的表：app 2 MB + `roms` 2.625 MB + `save` 128 KB + NVS，整片 8 MB 刷 `0x0`。
- 优点：不碰 launcher、不受 3 MB 槽限制、ROM 供料可以走 `esptool` 一次写死；回滚就是刷回 BATON 整片镜像。
- 缺点：不能和 baton-reader / digimon-cardputer 共存，切应用要重刷。

| 路线 | 性能 | 工作量 | 风险 | 回滚 |
|---|---|---|---|---|
| A 占用 `storage`（不改表） | **100%** | 低 | 低（只写数据区） | 重写该分区即可 |
| A' 改表成 `roms`+`save` | 100% | 中 | 中高（刷表/砖） | 重刷整片 |
| B SD 页缓存 | **30–60%（估）** | 中 | 高 | 容易 |
| C 独立固件 | 100% | 低 | 低 | 重刷整片 |

---

## 5. 性能：到底会不会更快

### 5.1 先给天花板

C3 的重绘帧成本已经算到底：`19.9 ms @ /180`、`51.7 ms @ /60`，且 92% 时间在 guest。**换硬件是唯一还能拿到数量级改善的方向**（`PERF_2026-11-22.md` 已经这么判过，AOT 之后结论不变，只是绝对值变好了）。

### 5.2 增益来源拆开算

| 来源 | 依据 | 倍数 | 可信度 |
|---|---|---|---|
| 主频 160 → 240 MHz | 硬指标 | **1.50×** | 【实测】 |
| IPC / 整数吞吐 | WLED 整数基准 C3 **127** vs S3 **238** MOPS；CoreMark 483 vs 1,330 | **1.87× / 2.75×** | 【项目内引用】/【第三方】 |
| Flash 取指 QIO | 官方："QIO/QOUT 相比 DIO **almost double**"；**但 C3 第 5 轮试过，日志里分不出差异**（见下） | 取指段 ×2，端到端 **0–20%〔未证实〕** | 【官方】+【实测存疑】 |
| ICache 16 → 32 KB | S3 可配（**默认 16 KB，必须显式改**） | miss 数减半 → 端到端 **5–15%〔估算〕** | 【官方】+【推算】 |
| 双核 | guest 在 core0，blit/音频/键盘在 core1 | +1–5%，**主要治抖动** | 【推算】 |
| 显示几何 | 192×192 → 96×96，每帧字节数 **1/4**，全窗 18.4 KB @40 MHz ≈ 3.7 ms | blit 从 1–5% 降到 ~1% | 【推算】 |

> **QIO 的实测存疑**：`dbgtest/r5_*.log` 里，同一时钟界面在 `/60` 下的窗口 `gmax` 是 51.1 / 51.7 / 52.9 / 60.5 ms 一簇，`r5_backdio`（名字就是"退回 DIO"）与其它几份没有可分辨的差别；`/180` 的两份都是 19.9–20.0 ms。也就是说 **C3 上 QIO 没有留下能证明自己的数据**——可能是镜像头/ QE 位没真正切过去（研究文档里"`bad magic`"那条），也可能是取指瓶颈不只在带宽。S3 上仍然要显式开 QIO 并核对镜像头，但**别把它当成 1.5× 之外的额外大头**。

**关键点**：从 63 周期/guest 指令与"673 KB 代码 vs 16 KB cache"看，取指仍是主要成本；但 C3 的 QIO 实验**没能证明带宽就是瓶颈**，所以 S3 的额外收益按 **5–20% 保守估**，不叠成 3×。"

### 5.3 估算结果

单帧成本模型（由 `19.9 ms @/180` 与 `51.7 ms @/60` 两个实测点解出）：**guest 侧 ≈30 ns/cycle，固定开销 ≈4 ms**（blit/tick/pacer；S3 上按 2 ms 估）。

| 场景 | C3 实测 | S3 ×1.9〔估算〕 | S3 ×2.2〔估算〕 | 判据（16.7 ms） |
|---|---|---|---|---|
| park 静止 | 59.3 fps / 62% | 60 fps / **33%** | 60 fps / 28% | 富余一倍 |
| 重绘 `/180`（533k） | **19.9 ms**（51 fps） | **10.4 ms** | **9.3 ms** | 稳 60，余 38–44% |
| 重绘 `/120`（800k） | 28 ms（36 fps） | **14.0 ms** | **12.9 ms** | **刚好 60** / 余 23% |
| 重绘 `/60`（1.6M） | **51.7 ms**（18.5 fps） | 26 ms（38 fps） | 23.8 ms（42 fps） | 仍到不了 60，但**游戏已超真机速度** |

**游戏速度**（= 60 fps × 帧预算，受真机 48 M cycles/s 约束）：

| 设置 | C3 | S3〔估算〕 |
|---|---|---|
| `/180` @60 fps | 32 M cycles/s = **0.67×** | 32 M = 0.67×（被 pacer 限住，再快也没用） |
| `/120` @60 fps | 做不到（36 fps）→ 29 M = 0.61× | **48 M = 1.00×** ← 目标 |
| `/60` @40 fps | 29 M = 0.61× | 64 M = 1.33×（过快，别用） |

**结论**：S3 不是"帧率更高"（60 fps 已封顶），而是**把 `/120` 拉进 60 fps**——那时候宠物成长/动画才回到真机速度，`clock_sync()` 每 250 ms 硬纠也可以退化成兜底。`/180` 在 S3 上只是"很省电的 0.67×"，别拿它当性能指标。

### 5.4 不会变快的部分（别指望）

- **60 fps 上限**：pacer 写死 `kFrameUs = 16667`；超过没有意义（guest 会变速）。
- **AOT dispatch 固定开销**：689 周期里 68 是查表+上下文，只吃主频，占比不到 10%。
- **按键延迟**：guest 要连续 4 帧才动作，延迟 = 4 × 单 guest 帧时间；换芯片不加分，而且**把预算放到 `/120` 会让它从 22 ms 涨到 33 ms**（仍 < 100 ms，但这是 `/120` 的代价之一）。
- **guest 逻辑本身**：ROM 里跑多少条还是多少条，S3 只是把每条变便宜。

### 5.5 功耗/发热〔估算〕

240 MHz 满载电流约比 160 MHz 高 30–50%，但 `/180` 下每帧能睡 6–8 ms（WFI），**平均电流大概率与今天持平**；`/120` 会明显更费。背光仍是第一大户。

---

## 6. 风险与坑（按疼度排序）

| # | 坑 | 后果 | 规避 |
|---|---|---|---|
| 1 | **画面只有 ~10 mm** | 产品体验直接掉一档 | 先实物比对；不能接受就别移植（见 §8） |
| 2 | 把 `storage` 当裸数据区用 | 以后谁在这块挂 SPIFFS，ROM 与文件系统互相踩 | 代码注释 + README 钉死；第一版**不改表**，就别碰 bootloader |
| 3 | ROM 走 SD | 重绘帧掉到 0.7 M instr/s 量级 | ROM 留 flash + mmap；SD 只放存档 |
| 4 | QIO 的 QE 位 / 镜像头 | C3 第 5 轮没测出差异，很可能头没真正切过去（研究文档记过 `bad magic`） | S3 上先 `esptool flash-id` 确认 GD 片，bootloader 与 app 头统一 QIO，A/B 时核对 `esptool.py image_info`；保留 DIO 整片回滚 |
| 5 | **ICache 默认 16 KB** | 白换芯片，收益缩水一半 | sdkconfig 显式 `CONFIG_ESP32S3_INSTRUCTION_CACHE_32KB=y` |
| 6 | Xtensa 代码密度未知 | 673 KB 可能变 750 KB+，miss 反而变多 | 先量重编后 `Flash Code`；必要时 `-Os` + 超块（见 `PLAN_AOT.md`） |
| 7 | ES8311 无 MCLK | 无声（且不报错） | 抄 BSP/esp_codec_dev 的无 MCLK 序列，上机先放一声 beep |
| 8 | RAM 没变多 | 321 KB vs 393 KB，双核栈/DMA 另算 | 别开 64 KB DCache；页缓存方案本来就被否 |
| 9 | flash 擦写停 CPU | 存档瞬间 30–100 ms 卡顿（研究文档怀疑过这点） | 存档放 SD，走独立 SPI3 + 后台任务 |
| 10 | app 与 ROM 同槽 | 装一次应用就把 ROM 冲了 | app 与 ROM 必须分属不同分区 |

---

## 7. 工作量与推进顺序

| 阶段 | 内容 | 人日 |
|---|---|---|
| **S0 先验证倍数** | 只把当前固件 target 改 `esp32s3`，显示/按键打桩，跑同一个时钟界面场景，量 `guest_pct` / `guest_fps` | **1** |
| S1 bring-up | BATON 应用（app 进 `ota_0`，ROM 进 `storage`）：显示 1:1、键盘 Q/W/E、音频 beep、ADC 电量、ROM mmap、存档上 SD | 3–4 |
| S2 UI | 240×135 侧栏 + 砖墙重画 + 8 行菜单 + lapse 提示条 + 预览工具 | 3–5 |
| S3 性能收口 | QIO、32 KB IC、blit 32 位、双核（core1 刷屏）、`/120` 判定、pacer | 2–3 |
| S4 回归 | AOT golden（562,126 条 trace SHA256、20 s 存档 SHA256、帧缓冲 0/9216）、60 s soak、按键手感 | 2 |
| **合计** | BATON 应用口径 | **10–14** |

**S0 是必须先做的一步**：一天的成本换掉"1.9× 还是 2.2×"这个最大不确定项。判据：同场景 `guest_pct` 若只降 20–25%（≈1.3×），说明瓶颈不在主频/取指，S3 不值这个工作量。

---

## 8. 三条替代路线（同一笔钱/同一份时间）

| 路线 | 性能 | 画面 | 工作量 | 评价 |
|---|---|---|---|---|
| **Cardputer ADV**（本文） | 1.9–2.2×，`/120` 真机速度 | **10 mm，硬伤** | 10–14 人日 | 能玩、手感最好（键盘+电池+SD），但宠物变小 |
| **320×240 的 S3 板**（T-Display S3 / 2.8" 模块） | 同上 | 保留 2× ≈ 24 mm | **6–8 人日**（`bezel.cpp`/几何几乎不动） | 性价比最高，代价是换硬件 |
| **C3 原地优化**（QIO + 超块 + IRAM 热块） | 1.1–1.3×（QIO 收益未证实）→ 0.65–0.8× 真机 | 不变 | 2–4 人日 | 便宜，但**到不了真机速度** |
| P4 / Tab5 | 5–8× | 大屏 | 3–6 个月 | 唯一"真 60 fps + 余量"，不是这一版该碰的 |

**建议**：
1. 想先确认"值不值"→ 做 S0（1 天）。
2. 只要性能不要这个形态 → 买块 320×240 的 S3 板，省一半工作量和全部屏幕妥协。
3. 一定要 Cardputer → **BATON 应用 + ROM 写进 `storage` 分区**（不改表、不刷 bootloader），UI 用"左侧竖栏 + 中间 96×96 画面 + 顶部状态条"的排版，把 144 px 的横向空白用掉；存档放 SD。
4. 只在 C3 上抠 → 先把 QIO 这条钉死（1 天，顺手解决 `sdkconfig` 与 `sdkconfig.defaults` 打架的状态），再考虑超块；别碰 JIT。

---

## 9. 验证判据（照抄项目现有口径，不新造指标）

| 项 | 判据 |
|---|---|
| 语义 | `emu_validate.py`：682,005/682,005、95,260/95,260、95,251/95,251、93,343/93,343 全 PASS |
| 逐字节 | 1 帧 trace SHA256 `C3A980DD…`；20 s autoplay 存档 `6872AEBE…`；shot `B22302E9…` |
| 像素 | 第 60 帧与 PC 参考 **0/9216** 不同 |
| 吞吐 | `guest_pct`、`guest_fps`、每帧 guest 指令数、`gmax`（帧时间最大值） |
| 卡顿 | `/120` 下 60 s soak 的 **p99 帧时间**（现在只有均值，看不见尖峰） |
| 手感 | 按键→画面反馈 < 100 ms（`PERF_2026-11-22.md` §7 的口径） |

---

## 10. 出处

**项目内文档（本地）**
- `PROGRESS.md`、`docs/C3_PORT.md`、`docs/FEASIBILITY.md`（§8 48 MHz 补记）
- `docs/AUDIT_PERF_2026-11-24_R2.md`（`-Os`、62%、块分布）
- `docs/PERF_2026-11-22.md`（63 周期/指令、110 周期端到端、帧预算 A/B）
- `docs/RESEARCH_PERF_ROUTES_2026-11-24.md`（S3 1.8–2.2×、WLED 127 vs 238、P4 对比）
- `docs/RESEARCH_EXT_C3_FETCH_PATH_2026-11-26.md`（C3 ICache 固定、QIO −45%、miss 推算）
- `docs/HW_STICKS3.md`（同尺寸屏的 1.4× 结论、Cardputer ADV 规格）
- `docs/HANDOVER_AOT.md`（AOT 架构与验收）
- `dbgtest/r5_*.log`（第 5 轮真机：`/60` 时钟界面 **51.7 ms / 18.5 fps**、`/180` **19.9 ms / 51 fps**、park **57 fps / 22–23%**；`r5_backdio` 这个名字说明 QIO 最后被退回 DIO）

**本机实测**
- `C:\Users\DuWeisi\Tools\cardputer-adv-bsp\docs\Cardputer-ADV-硬件笔记.md`（S3FN8 / 321 KB / 240×135@40 MHz / TCA8418 / ES8311 无 MCLK / GD quad DIO）
- `launcher/partitions.csv`（`storage` 3.875 MB 空置）
- `digimon/components/bsp/src/bsp_sd.c`（SD 独立 SPI3）

**外部**
- ESP-IDF · Speed Optimization (ESP32-S3)：<https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-guides/performance/speed.html>
  —— "Set CONFIG_ESPTOOLPY_FLASHMODE to QIO or QOUT … **almost double** the speed … compared to the default DIO"；S3 的 I/D cache 尺寸可配。
- ESP-IDF `esp_system/port/soc/esp32s3/Kconfig.cache`（本地 5.5.3 树）：I 16/32 KB、D 16/32/64 KB、行 16/32/64 B，**ICache 默认 16 KB**。
- ESP-IDF `esp_rom/esp32c3/.../rom/cache.h`：`MIN/MAX_ICACHE_SIZE = 16384`、8 路、32 B 行 —— C3 不可配。
- CoreMark 483（C3）/ 1,330（S3）：<https://www.schematik.io/blog/esp32-c3-vs-s3>
- M5Stack Cardputer-Adv 规格（1.14" 240×135、ES8311、BMI270、TCA8418）：<https://docs.m5stack.com/en/core/Cardputer-Adv>

---

## 11. 一句话给下一轮

**"能不能移"不是问题，"值不值"才是**：模拟器搬过去几乎免费、性能还能翻倍，代价是 96×96 只剩 10 mm（外加把 launcher 那块空着的 `storage` 征用成 ROM 区）。先花一天把 S3 的 `guest_pct` 量出来，再决定是给 Cardputer 让路，还是换一块 320×240 的 S3 板。
