# Digimon Color Emulator for AI Passport

把 Bandai **Digital Monster Color**（彩屏元祖，MAME 机种 `digicolr`）的原机固件，
跑在 **FoloToy AI Passport**（ESP32-C3，8 MB flash，320×240 ST7789P3，A/B/C 三键）上。

不是重画素材、不是重写玩法：CPU、定时器、中断、SPI flash、LCD 控制器、按键全部按原机行为实现，
ROM 用你自己的镜像，通过网页（Web Serial）写进设备。

| 项 | 值 |
|---|---|
| 目标板 | FoloToy AI Passport（ESP32-C3, 8 MB flash, 无 PSRAM） |
| 屏幕 | ST7789P3 320×240，玻璃窗口 192×192 @ (64,24)（guest 96×96 ×2） |
| 版本 | 1.15.0 |
| 许可 | GPL-2.0-or-later（核心移植自 MAME） |

---

## 一、刷机

固件**不含 ROM**：ROM 是 Bandai 的，8 MB 也塞不进 2 MB 的 app 分区。
刷完机后屏幕显示 `NO ROM`，用网页把 ROM 传进去。

### 方式 A：只烧 app 分区（推荐，不动存档和 ROM）

```bash
python -m esptool --chip esp32c3 -p COM4 -b 460800 write_flash \
    --flash_mode dio --flash_freq 80m --flash_size 8MB \
    0x10000 digicore-c3.bin
```

### 方式 B：整片镜像（第一次装 / 救砖）

```bash
python -m esptool --chip esp32c3 -p COM4 -b 460800 write_flash \
    --flash_mode dio --flash_freq 80m --flash_size 8MB \
    0x0 digicore-c3-full.bin
```

Windows 上串口名通常是 `COM4`，以设备管理器里出现的为准。

## 二、上传 ROM

1. 用 **Chrome / Edge** 桌面版打开 `dmc_web.html`（双击本文件即可；也可以用本地服务器打开）。
2. 点「连接设备」，在弹窗里选设备对应的串口。
3. 选你的 8 MiB ROM `.bin`，点「上传 ROM」。
4. 大约 20–30 秒（只传有数据的 611 页，约 2.5 MiB），设备自动重启进入游戏。

ROM 格式要求：**8 MiB、4 KiB 对齐**。非零页超过 637 页会被拒绝（分区容量）。

## 三、按键

三个键 1:1 对应原机的 A / B / C：

| 键 | 原机键 | 行为 |
|---|---|---|
| A | Select | 在上下两排图标之间循环选中 |
| B | Confirm | 确认选中的图标 / 菜单项；无选中时显示时钟 |
| C | Cancel | 取消选中 / 退出菜单；无选中时长按 2 秒开原机设置菜单 |

本机加的两个手势（原机没有）：

| 手势 | 行为 |
|---|---|
| **长按 A 1.2 秒** | 打开设置卡（8 行）：SEND A+C / SEND A+B / TIME LAPSE / AUTO OFF / BRIGHT / VOLUME / AUTO SAVE / VERSION |
| **长按 B** | 归还给游戏，不弹任何东西 |

**组合键**：原机三键在一条电阻梯上，物理上按不出 A+C / A+B，所以由设置卡代发：

| 组合 | 画面 | 功能 |
|---|---|---|
| A+C | 主画面 | 静音 / 取消静音（屏幕上没有反馈） |
| A+C | 时钟画面 | 进入改时间 |
| A+B | 墓碑（宠物死亡后） | 孵新蛋 |

## 四、存档

- **自动存档**：默认每 60 秒一次（设置卡 `AUTO SAVE` 可关）。`save` 分区里是 5 条环形记录，
  每条带 CRC；断电最多丢最后一份，开机按序列从新到旧逐条验，用第一条通过的。
- **读档**：开机自动恢复。停在卡带里的记录直接续上；停在 ROM 里的记录重播开机画面。
- 网页上也能整块导出 / 导入 128 KiB 存档（`.dmcsave`），导入后重启生效。

## 五、画面

- 玻璃窗口 **192×192 @ (64,24)**，是 guest 96×96 可见区的整数 2 倍缩放
- 外壳是 RETAIL (style C)：砖墙（128×128 A8 tile 平铺）+ 金属内框 204×204 圆角 8 + well 198×198 圆角 4 + 面板自身的 30 px 圆角遮罩
- 右下角是电量百分比（CW2017 电量计，每 30 秒读一次，只在变化时重画）
- 砖墙和电量**直推面板、不进 guest 帧缓冲**，所以串口抓帧看不到它们，只有肉眼能看

## 六、网页工具

`tools/dmc_web.html`（单文件，无依赖，Web Serial）：

- 上传 ROM（浏览器里做稀疏化，只传非零页）
- 导出 / 导入存档
- 设备状态（ROM 是否已装、存档分区）

协议（`DMC1`）写在 `docs/WEB_TOOL.md`。用 Python 做协议回归：`python tools/dmc1_test.py rom|save|hello`。

## 七、出问题怎么办

| 现象 | 处理 |
|---|---|
| 屏幕显示 `NO ROM` | 正常，还没传 ROM；按第二节上传 |
| 网页提示不支持 Web Serial | 换 Chrome / Edge；或用本地服务器打开（`python -m http.server 8000`，再访问 `http://127.0.0.1:8000/dmc_web.html`） |
| 上传到一半失败 | 重新上传即可；分区在 `END` 之前不会被认为是完整的 |
| 设备一直停在 NO ROM | 用 `digicore-c3-full.bin` 整片重烧，或重传 ROM |
| 游戏很慢 | 先确认版本：设置卡 `VERSION` 一行应显示 `1.15.0`。v1.14.0 在时钟画面约 56 fps、游戏速度约原机 94%；v1.15.0 在此基础上把「整屏重绘」那一帧的 14 ms SPI 阻塞改成与 guest 重叠。更旧的版本只有 18 fps |
| 画面偏出玻璃 | 别用调试命令 `E` 挪窗口；`E` 只用于校准 |

## 八、目录

```
firmware/digicore-c3/     固件工程（ESP-IDF 5.5.3 / esp32c3）
  main/
    app_main.cpp          主循环、blit、按键手势、控制台命令
    bezel.cpp/.h          RETAIL 外壳：砖墙 / 内框 / 圆角遮罩 / 电量
    dmc_link.cpp/.h       DMC1 传输协议：ROM 上传、存档出入、无 ROM 等待模式
    menu.cpp/.h           设置卡（直推面板，半透明 + 圆角）
    display.c             ST7789P3 初始化与 draw_bitmap
    audio.c / battery.c   方波合成器（总线所有者）/ CW2017 电量计（复用同一 I2C）
    emu/                  unSP CPU、GPCE4 SoC、ST7735 模型、AOT 块
    brick_a8.h            128×128 A8 砖纹（与 digimon-passport 同一张）
  tools/                  生成器与模拟器
  docs/                   交接、UI 推导、协议、移植记录
```

## 九、许可与来源

代码以 **GPL-2.0-or-later** 分发，因为模拟器核心是 MAME 的移植。详见 `THIRD-PARTY.md`。

- 模拟核心：**MAME 0.289**（GPL-2.0+）的 `generalplus_gpce4*`、`generic_spi_flash`、unSP 指令集
- 外壳版式与砖纹：**digimon-passport**（MIT）的 RETAIL (style C) 规格
- 硬件抽象：**FoloToy AI Passport BSP**（MIT）
- **ROM 不属于本项目**，需用你自己合法持有的镜像；仓库不包含任何 Bandai 固件、美术资源或商标授权
