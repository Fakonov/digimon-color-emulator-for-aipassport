# v1.4.2 — 砖墙边框 + 电量百分比（2026-11-24）

**里程碑判据：主画面四周是砖墙（真机 Original Brown 配色），右下角显示白色电量百分比，长按 A 的设置卡与两条组合键照常工作。**

| 项 | 值 |
|---|---|
| 版本 | `1.4.2`（`version.txt`） |
| 日期 | 2026-11-24 |
| 源码 | git tag `v1.4.2` |
| 串口自检 | `VER ver=1.4.2 win=0,18 96x96 scale=2 left=64 top=24 off=0,0 home=64,24 fb=128 chunk=16` |
| 电量 | `batt: CW2017 v0F ready, 4103 mV, SOC 92%` |

---

## 一、砖墙边框

### 配色从商品图采样，不是凭印象

`tmp/dmc_photos/mynavi_001.jpg`（Ver.1 Original Brown 官方商品图）取平均色：

| 位置 | 24-bit | RGB565 | 常量 |
|---|---|---|---|
| 受光面 | `#A25D59` | `0xA2EB` | `kBrickLit` |
| 中间调 | `#974E4A` | `0x9269` | `kBrickFace` |
| 背光面 | `#84342B` | `0x81A5` | `kBrickBase` |
| 砖缝 | `#723631` | `0x71A6` | `kBrickMortar` |

**这不是** digimon-passport `SHELLS[]` 里的 `BROWN 0x8A5A3C`（那个偏棕黄）；彩屏元祖的 "Original Brown" 是偏红的砖红。

### 实现与那边不同，因为约束不同

| | digimon-passport | 本机 |
|---|---|---|
| 载体 | 预生成 128×128 **A8 alpha 蒙版**（`dg_brick.h`，16 KB flash） | **程序化生成 565**，flash 零字节 |
| 平铺 | LVGL `lv_image_tiled` | 逐像素算，分 band 推 |
| 染色 | 运行时染色（8 种配色共用一张 tile） | 直接出最终颜色 |
| 原因 | 有色号菜单，A8 省 48 KB | 无 LVGL、无配色菜单，代码比 tile 更省 |

砖块 **28×11 + 2 px 砖缝**，奇数行偏移半块，每块砖用 `col*2654435761 ^ course*2246822519` 取稳定色差（1/4 偏暗），最后一行压暗当阴影。不加这三样，整面墙会像印刷网格。

### 绘制时机

开机填空黑之后、关设置卡之后、`E` 改位置之后、`G 0` 退出诊断之后。四条带都按**绝对面板坐标**生成，石头跨带相连。

成本：窗口外 39,936 像素，一次性约 5 ms + SPI，**不进每帧路径**。

## 二、电量百分比

### 硬件：和音频 codec 共用一条 I2C

| 项 | 值 |
|---|---|
| 芯片 | CW2017，7 位地址 `0x63` |
| 总线 | I2C0，SDA 10 / SCL 7，与 ES8311（`0x18`）同一对引脚 |
| 读法 | `0x04` 高字节 = 整数百分比；`0x02/0x03` = 14 bit 电压，312.5 µV/LSB |

`audio.c` 已经开好这条总线，所以 `battery.c` **不另开总线**（同一对引脚两个 master 是这块板承受不了的）。为此 `audio.h` 加了 `void *audio_i2c_bus(void)`，用 `void*` 避免把驱动头文件渗进 `audio.h`。

顺序约束：`battery_init()` 必须在 `audio_init()` 之后。

### 第一版读到 0%，原因写清楚

v1.4.1 只有"restart → active → 读 SOC"，实测 `CW2017 v0F ready` 但 `battery 0%`，等 100 秒不变。

补上上游完整流程（profile 比对 → 必要时睡眠态写 80 字节并校验、置 `UPDATE_FLAG` → 重启进 active）后读出 `4103 mV, SOC 92%`。

**但日志里没有 "writing the profile"**，说明 `UPDATE_FLAG` 和 80 字节本来就在、比对通过，profile 没被重写。所以 0% 更可能是**复位后芯片还没算完就被读**，而 v1.4.2 多出来的那串操作（读 80 字节约 80 ms、必要时再 restart→active）刚好给了它时间。**不要记成"必须写 profile 才能工作"。**

profile 的 80 字节是这块板子的电池（万锂新能源 500 mAh，profile2 / XIL40X / 2026-10-20），逐字节抄自 `ai-passport-tamago/components/bsp/src/bsp_battery.c`；留着是为了芯片被换过或 profile 被清掉时能自愈。

### 显示

| 项 | 值 |
|---|---|
| 位置 | 右下角，`bx = 320 − 5 − tw`，`by = 240 − 4 − 14`（"92%" 时 x 281..314，y 222..235） |
| 字体 | `font5x7.inc` 的 5×7 面，2 倍（10×14，步进 12），和设置卡同一套 |
| 颜色 | 白色 `0xFFFF` |
| 背景 | 字形没盖住处**露出砖墙**，不是黑框 |

画在墙上而不是窗口里是刻意的：窗口内是模拟器输出的原机画面，一个 UI 像素都不该占用。

刷新：每 30 s 读一次，**只有数字变了才重画那个角**；菜单打开时不读（卡片盖满面板，关菜单时 `panel_draw_bezel()` 连电量一起重画）。

## 三、这一串版本一起归档

| 版本 | 内容 |
|---|---|
| v1.4.0 | 砖墙边框 |
| v1.4.1 | 电量显示第一版（SOC 恒 0） |
| **v1.4.2** | 电量显示修好（profile 比对 + 电压诊断） |

三个版本连续、中间没有可用的独立发布点，所以只在这一版打里程碑。

## 四、验证

```
VER ver=1.4.2 win=0,18 96x96 scale=2 left=64 top=24
batt: CW2017 v0F ready, 4103 mV, SOC 92%
DIGICORE: battery 92%
```

电压 4103 mV 与 92% 自洽——**这两个数必须一起看**，只看 SOC 分不清"电池没电"和"芯片没算完"；看到 `0 mV` 才是真的没接电池。

砖墙和电量都直推面板、不进 `fb[]`，串口抓帧看不到。`tmp/s31/bezel_preview.py` 用**同一套 `brick_px()` 和同一份字体表**离线渲染 320×240，预览应与面板一致；实际观感仍需目视。

## 五、素材

| 文件 | 大小 | 说明 |
|---|---|---|
| `digicore-c3-v1.4.2.bin` | 1,182,768 B | **app 分区镜像**（0x10000 起），不含 ROM |
| `sha256.txt` | — | 归档件 + 两个可重建产物的指纹 |

整片镜像不入库：`digicore-c3-full.bin`（4,677,888 B）含 sparse ROM 分区。

## 六、重建与烧写

```powershell
cd firmware\digicore-c3
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1

$py="C:\Users\DuWeisi\.espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe"
& $py -m esptool --chip esp32c3 -p COM4 -b 460800 write_flash --flash_mode dio `
     --flash_freq 80m --flash_size 8MB 0x0 digicore-c3-full.bin
```

## 七、没做

- **A+B 的真实验收**：`port=ffe7` 只到 PORTA，墓碑画面才能验。
- **主画面 A+C 静音**：本机没有 ROM 音频可静音，永远无反馈。
- **音频模拟**：要先找到 ROM 驱动压电片的那条通路（`dac=0 pwm=0`，不在已建模的寄存器里）。
- **金属内框**：真机在屏幕和砖墙之间还有一圈框 + 钥匙孔造型，现在是墙直接怼着窗口边。
- **guest 实时率 ~27%**、16 s 空闲 park 点，见 `docs/HANDOFF_NEXT.md` §4。
