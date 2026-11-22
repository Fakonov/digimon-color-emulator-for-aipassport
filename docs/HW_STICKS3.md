# M5StickS3 × M5StickC Joystick HAT 兼容性评估

> 评估日期：2026-11-20
> 结论摘要：**接口不兼容，直插仅能上电不能通信；数码暴龙机模拟器在 StickS3 上可行，但必须走静态重编译，且屏幕只有 135×240。**

## 1. 两侧接口的官方定义

### 1.1 M5StickC HAT 座（8 pin，1×8，2.54 mm）

来源：M5StickC 原理图 `SCHE_StickC_page_01.png`（https://docs.m5stack.com/en/core/StickC），HAT 功能块自上而下：

| 针位 | 信号 | 性质 |
|---|---|---|
| 1 | GND | 地 |
| 2 | EXT_VDD | 3.3 V（AXP192 EXTEN 控制，可关断） |
| 3 | GPIO26 | 数据脚（Joystick HAT 作 SCL） |
| 4 | GPIO36 | 数据脚 |
| 5 | GPIO0 | 数据脚（Joystick HAT 作 SDA） |
| 6 | VBAT_Li_Ion | 电池正极 |
| 7 | VESP_3V3 | 3.3 V（常开） |
| 8 | VBUS_VIN | 5 V（USB） |

### 1.2 M5StickS3 Hat2-Bus（2×8，2.54 mm，塑高 8.5 mm）

来源：https://docs.m5stack.com/en/core/StickS3 的 PinMap 段，及同页原理图第 5 页（`K150_Stick_S3_PRJ_V0.6_..._page_05.png`，CN2 标注「2*8排列，塑高8.5，P=2.54」）。

| 左列 | 信号 | 右列 | 信号 |
|---|---|---|---|
| 1 | GND | 2 | GPIO5 |
| 3 | EXT_5V | 4 | GPIO4 |
| 5 | Boot（GPIO0） | 6 | GPIO6 |
| 7 | GPIO1 | 8 | GPIO7 |
| 9 | GPIO8 | 10 | GPIO43 |
| 11 | BAT | 12 | GPIO44 |
| 13 | 3V3_L2 | 14 | GPIO2 |
| 15 | 5V_IN | 16 | GPIO3 |

要点：**该总线没有 GPIO26**（ESP32-S3 的 GPIO26–32 被 SPI flash/PSRAM 占用），GPIO0 仅作为 Boot strap 引出。

## 2. 物理对插后的逐脚映射

Joystick HAT 是 1×8 单排公针（34.94 × 24.0 × 16.7 mm），针距 2.54 mm，可插入 Hat2-Bus 母座的左列八孔。按位置一一对应：

| HAT 针（StickC 语义） | 插入 StickS3 左列后接到 | 后果 |
|---|---|---|
| 1 GND | GND | 正常 |
| 2 EXT_VDD 3.3 V | **EXT_5V** | EXT_5V 默认高阻（输入）；一旦 `M5.Power.setExtOutput(true)`，5 V 直灌 HAT 的 3.3 V 网络 |
| 3 GPIO26 / SCL | **Boot（GPIO0）** | SCL 落到启动 strap 脚 |
| 4 GPIO36 | GPIO1 | 悬空 |
| 5 GPIO0 / SDA | **GPIO8** | SDA 落到 GPIO8 |
| 6 VBAT | BAT | 正常 |
| 7 VESP_3V3 | 3V3_L2 | 正常（HAT 由此得电） |
| 8 VBUS_VIN 5 V | 5V_IN | 正常 |

结论：**地、3.3 V、5 V、电池四路电源脚碰巧对齐，HAT 能上电；两个 I2C 信号脚错位。** 插上去不会立刻烧（EXT_5V 默认为输入），但也读不到摇杆数据。

## 3. Joystick HAT 本体参数（用于软件适配）

来源：https://docs.m5stack.com/en/hat/Hat_Joystick

- MCU：STM32F030F4P6；接口：I2C @ **0x38**；供电 3.3 V
- 寄存器：
  - `0x01` 只读 4 B：X 低/高、Y 低/高，原始值 0–4096
  - `0x02` 只读 3 B：X、Y（−127…127）+ 按键（0 = 按下，1 = 释放）
  - `0x03` 只写：0x00 正常 / 0x01 中心校准 / 0x02 最大值校准 / 0x03 保存
- 地址 0x38 与 StickS3 板内 I2C 设备（BMI270 0x68、M5PM1 0x6e、ES8311 0x18）不冲突，但板内总线（G47/G48）未引到 Hat2-Bus。

## 4. 三条可用路线

### 路线 A：飞线接 Hat2-Bus（保留这块 HAT）

不插排针，只接 4 根线：

| HAT | 接到 StickS3 |
|---|---|
| 3.3 V | Hat2-Bus 13 脚（3V3_L2） |
| GND | Hat2-Bus 1 脚 |
| SDA | 任一空闲 GPIO，例如 9 脚（GPIO8） |
| SCL | 任一空闲 GPIO，例如 10 脚（GPIO43） |

软件侧 `Wire.begin(SDA, SCL)` 自建总线即可。**不要用 GPIO0 当 SCL**（strap 脚）。此路线绕开了全部错位问题。

### 路线 B：换 Grove 摇杆 Unit Joystick2

来源：https://docs.m5stack.com/en/unit/Unit_Joystick2

- STM32G030F6P6，I2C @ **0x63**，霍尔摇杆，X/Y 16 位（0–65535）+ Z 键
- Grove HY2.0-4P：黑 GND / 红 5 V / 黄 SDA / 白 SCL
- StickS3 的 PORT.A（G9/G10）在 M5Unified 中被登记为外部 I2C（SCL=GPIO10、SDA=GPIO9），**即插即用**
- 注意：StickS3 默认 `EXT_5V_EN` 关闭，Grove 无 5 V 输出，必须先 `M5.Power.setExtOutput(true)` 才能给该单元供电

### 路线 C（推荐）：不用摇杆，走 3 键

数码暴龙机的输入语义只有 3 个键（本项目实测 B1=0x0008、B2=0x0010、B3=0x0020）。StickS3 自带 KEY1=GPIO11、KEY2=GPIO12，再从 Hat2-Bus 引 1 个 GPIO 加一个轻触键即可凑齐 3 键；也可把 3 键全部外挂到 Hat2-Bus，避开外壳开孔。

## 5. M5StickS3 硬件规格（决策相关）

来源：https://docs.m5stack.com/en/core/StickS3

| 项 | 参数 |
|---|---|
| SoC | ESP32-S3-PICO-1-N8R8，双核 Xtensa LX7 @ 240 MHz |
| 内存 | 8 MB Flash + 8 MB Octal PSRAM |
| 屏幕 | ST7789P3，135 × 240，SPI（MOSI=G39、SCK=G40、RS=G45、CS=G41、RST=G21、BL=G38） |
| IMU | BMI270（0x68），与 M5PM1 共用 I2C（SCL=G48、SDA=G47） |
| 音频 | ES8311 codec（0x18）+ I2S（MCK=G18、BCK=G17、WS=G15、DOUT=G14、DIN=G16）+ AW8737 + 8 Ω/1 W 2011 腔体喇叭 + MEMS 麦克风 |
| 按键 | KEY1=G11、KEY2=G12 |
| 电源 | M5PM1 PMIC（0x6e），250 mAh，满载 519 mA |
| 扩展 | Hat2-Bus（2×8，2.54）+ Grove PORT.A（G9/G10，5 V）+ IR 收发 |
| 尺寸 / 重量 | 48 × 24 × 15 mm / 20 g |

官方声明的**结构不兼容** HAT 列表：Hat Mini JoyC（U156）、Hat Mini EncoderC（U157）、Hat 18650C（U080）。Joystick HAT 未列入，但这只是结构口径，不代表电气兼容。

## 6. 模拟器可行性核算

### 6.1 算力

| 路线 | C3 @160 MHz 实测 | StickS3 @240 MHz 推算 |
|---|---|---|
| 解释器（现状） | 219 cycles/guest instr → 1.22 fps | ≈ 1.1 M guest instr/s，约 1.8 fps，**仍不达标** |
| 静态重编译（recomp） | 1.98 cycles/guest（热块）、6.16（delay 块）→ 80.68 M / 25.94 M guest instr/s | 同 cycles 假设下 ≈ 121 M / 39 M guest instr/s |

目标：原机 GPCE4064B 最高 48 MHz，折算 23.4–26.9 M guest instr/s。

判定：**换 S3 不会让解释器变实时**（只从 1.22 fps 到约 1.8 fps），必须把 recomp 路线做完。recomp 在 S3 上预计有 2.3–4.5 倍余量（取决于 PSRAM 命中率与代码放 SRAM 的比例）。

### 6.2 内存

- ROM 镜像 8 MB，稀疏后非零段 2,502,656 B（2.39 MiB）→ 8 MB PSRAM 可整段常驻，不再需要页缓存/overlay。
- 8 MB Flash 与新板容量一致，分区表沿用即可（app 分区可放大）。
- 帧缓冲 135 × 240 × 2 B = 64,800 B，内部 SRAM 512 KB 足够，另可留 DMA 双缓冲。

### 6.3 屏幕（唯一硬伤）

- 游戏内容 96 × 96。
- 135 × 240 竖屏：1× 居中 → 96 × 96；最大等比放大 1.4×（134 px，非整数缩放，像素不均匀）。
- 现状 320 × 240 可做 2×（192 × 192）。
- 换到 StickS3，画面物理尺寸明显变小；但 1.14 英寸本身更接近原机观感，是否算缺陷取决于目标是「还原手感」还是「大屏爽玩」。
- 好消息：驱动 IC 与现板同为 ST7789P3，显示代码只需改分辨率、偏移与旋转。

### 6.4 音频

现板音频未实现（压电片未反推）。StickS3 自带 ES8311 + I2S + 1 W 喇叭，M5Unified 已封装 `M5.Speaker`（I2S0，默认 22.05 kHz 立体声），音效从「做不了」变成「能做」。

### 6.5 输入

原生 2 键 + Hat2-Bus 外挂 1 键 = 3 键，正好覆盖 B1/B2/B3；无需 ADC 电阻梯。

### 6.6 电源

250 mAh / 满载 519 mA → 纯满载不足半小时。模拟器双核跑满 + 屏幕 + 音频，实际续航约 40–60 分钟，长时间玩需外接电源。

### 6.7 移植工作量

| 模块 | 工作 |
|---|---|
| 工具链 | ESP-IDF 换 target 到 esp32s3，开 Octal PSRAM，改分区表（8 MB） |
| 显示 | ST7789P3 驱动改 135×240（偏移/旋转/刷新区域） |
| 输入 | 去掉 ADC 电阻梯，改 GPIO 直读 3 键 |
| 电源 | 接 M5PM1 初始化（L2 域、EXT_5V 默认关闭） |
| 音频 | 新增 ES8311 + I2S 通路（可复用 M5Unified） |
| 模拟器 | `emu/` 核心与架构无关，交叉编译；recomp 输出需重编到 Xtensa |
| 双核 | core0 跑 guest CPU，core1 跑 LCD 刷屏 + 音频 |

## 7. 最终判定

1. **M5StickS3 × M5StickC Joystick HAT：不兼容。** 结构上能插（针距 2.54、单排 8 针正好插左列），电源脚碰巧对齐，但 SDA/SCL 分别落到 GPIO8 与 Boot(GPIO0)，直插读不到数据；且 EXT_5V 与 HAT 的 3.3 V 同网，开 5 V 输出即烧。
2. **要用摇杆**：飞 4 根线到 Hat2-Bus（3V3_L2 / GND / GPIO8 / GPIO43），或直接用 Grove 版 Unit Joystick2（0x63，注意先开 EXT_5V）。
3. **做数码暴龙机**：摇杆不是必需品，3 键即可；StickS3 的算力、PSRAM、音频都够，屏幕是唯一妥协项。
4. **换板不能替代 recomp**：解释器在 240 MHz 上仍然只有约 1.8 fps。

## 8. 出处

- M5StickS3 文档（规格 / PinMap / 兼容性说明 / 原理图）：https://docs.m5stack.com/en/core/StickS3
- M5StickC 文档（含原理图 `SCHE_StickC_page_01.png`）：https://docs.m5stack.com/en/core/StickC
- Hat Joystick 文档（I2C 0x38、寄存器、PinMap）：https://docs.m5stack.com/en/hat/Hat_Joystick
- Unit Joystick2 文档（I2C 0x63、Grove 接线）：https://docs.m5stack.com/en/unit/Unit_Joystick2
- M5Unified 板级引脚表 `src/M5Unified.inl`（StickS3 板定义、ex_i2c=G10/G9、按键 G11/G12、ES8311 I2S 引脚）：https://github.com/m5stack/M5Unified
- 产品页（受 429 限制未能读取正文，规格以文档站为准）：https://shop.m5stack.com/products/m5sticks3-esp32s3-mini-iot-dev-kit 、https://shop.m5stack.com/products/m5stickc-joystick-hat
- M5Stack Cardputer-Adv 文档（规格 / PinMap / EXT 14P 引脚）：https://docs.m5stack.com/en/core/Cardputer-Adv
- M5Unified 板级表 `board_M5CardputerADV` 条目（内部 I2C SCL=G9/SDA=G8；外部 I2C SCL=G1/SDA=G2）：https://github.com/m5stack/M5Unified

## 9. 追加评估：M5StickS3 ↔ Cardputer-Adv 外部接线

### 9.1 Cardputer-Adv 关键规格（决定能不能当协处理器）

| 项 | 参数 |
|---|---|
| SoC | **ESP32-S3FN8**，双核 LX7 @ 240 MHz |
| 内存 | 8 MB Flash，**无 PSRAM**（型号 FN8 即无 PSRAM 版本） |
| 屏幕 | ST7789V2，**1.14"，240 × 135**（SPI：BL=G38、RST=G33、RS=G34、DAT=G35、SCK=G36、CS=G37） |
| 键盘 | 56 键（4×14），TCA8418RTWR 键盘控制器（I2C，SDA=G8、SCL=G9、INT=G11） |
| 音频 | ES8311（SDA=G8、SCL=G9），NS4150B + 8 Ω/1 W 喇叭，3.5 mm 耳机口 |
| IMU | BMI270（G8/G9） |
| 存储 | microSD（CS=G12、MOSI=G14、CLK=G40、MISO=G39） |
| 供电 | 1500 mAh，工作电流 120.2 mA |
| 扩展 | Grove HY2.0-4P（GND / 5V / G2 / G1）+ **EXT 2.54-14P** |

### 9.2 Cardputer-Adv EXT 2.54-14P 引脚（互联用的就是这排）

| 针 | 左列 | 针 | 右列 |
|---|---|---|---|
| 1 | RESET (G3) | 2 | 5VIN |
| 3 | INT (G4) | 4 | GND |
| 5 | BUSY (G6) | 6 | 5VOUT |
| 7 | SCK (G40) | 8 | I2C_SDA (G8) |
| 9 | MOSI (G14) | 10 | I2C_SCL (G9) |
| 11 | MISO (G39) | 12 | G13 |
| 13 | CS (G5) | 14 | G15 (UART_RX) |

即：**SPI、I2C、UART 全部引出**，另带 5VIN / 5VOUT / GND / RESET / INT / BUSY 握手线。电平 3.3 V，与 StickS3 一致。

### 9.3 三种物理链路与其上限

| 链路 | 引脚（StickS3 侧 ←→ Cardputer 侧） | 实测可用速率 | 能否传帧 |
|---|---|---|---|
| SPI（S3 作 master，Cardputer 作 slave） | SCK/MOSI/MISO/CS 各一，如 G1/G2/G3/G4 ←→ G40/G14/G39/G5 | 20–30 MHz，约 1–2.5 MB/s | 96×96@30 fps（553 KB/s）可以；240×135@30 fps（1.94 MB/s）勉强，需 RLE 压缩 |
| UART + 流控 | G43(TX)/G44(RX) ←→ G13/G15（RTS/CTS 再占两脚） | 921600–2 Mbps，90–250 KB/s | 只够传指令/事件 |
| I2C | G8/G9 ←→ G8/G9 | 400 kHz，约 40 KB/s | 只够传按键 |

注意：StickS3 的 UART0（G43/G44）默认是日志口，但 M5StickS3 的 PlatformIO 配置带 `ARDUINO_USB_CDC_ON_BOOT=1`，console 走 USB，UART0 可让给板间通信。

### 9.4 为什么「提高处理性能」不成立

1. **没有共享内存**。两颗 S3 之间只有串行链路，模拟器的 guest CPU 执行是紧耦合单线程（一个 basic block 内全是本地 SRAM 访问），拆不开也没法远程执行。
2. **Cardputer-Adv 更弱**。FN8 = 无 PSRAM，ROM 稀疏镜像 2.39 MiB 只能放 flash / SD 卡，随机访问延迟远高于 StickS3 的 Octal PSRAM。
3. **能并行的部分已经被单芯片吃掉了**。渲染、刷屏、音频本来就在 S3 的 core1 上跑，不需要第二颗芯片。
4. 跨芯片同步（SPI 中断 + 双缓冲）引入的延迟与抖动，只会让 30 fps 目标的时序更难过。

### 9.5 为什么「扩展屏幕」也不成立

- Cardputer-Adv：1.14"，**240 × 135**
- M5StickS3：1.14"，**135 × 240**

同一块尺寸的屏，只是长宽对调。96×96 的内容在 240×135 上最大等比仍是 1.4×（135/96），和 StickS3 上一模一样，等于没扩。

真正扩屏应外接 SPI TFT（2.8"/3.5"，320×240，ILI9341/ST7789），从 StickS3 的 Hat2-Bus 引 SPI + 3V3 + GND，成本几十元，视野翻倍。

### 9.6 这条线真正值钱的地方

如果确实要连，收益不在算力，而在 Cardputer-Adv 的**外围**：

| 收益 | 说明 |
|---|---|
| 电力 | 1500 mAh vs StickS3 的 250 mAh，EXT 5VOUT → StickS3 的 5V_IN，续航从 40 分钟级拉到数小时 |
| 输入 | 56 键 TCA8418 键盘，替暴龙机的 3 键输入（过剩但省事） |
| 存储 | microSD 放 ROM / 存档，StickS3 没有卡槽 |
| 音频 | 3.5 mm 耳机口，NS4150B 功放 |
| 副屏 | 只在「双屏分工」场景有意义（如一边跑模拟器一边看调试信息） |

### 9.7 若要实施：推荐拓扑

StickS3 = 主机（跑 recomp 模拟器，core0 执行 guest，core1 刷本地屏并打包帧）；Cardputer-Adv = 终端（收帧、显示、把键盘事件回传）。

连线（StickS3 Hat2-Bus → Cardputer EXT 14P）：

| 信号 | StickS3 Hat2-Bus 脚 | Cardputer EXT 脚 |
|---|---|---|
| GND | 1 | 4 |
| 5V（Cardputer 供电给 StickS3） | 15（5V_IN） | 6（5VOUT） |
| SCK | 7（GPIO1） | 7（G40） |
| MOSI | 14（GPIO2） | 9（G14） |
| MISO | 16（GPIO3） | 11（G39） |
| CS | 4（GPIO4） | 13（G5） |
| 握手 IRQ | 2（GPIO5） | 3（INT / G4） |

注意事项：
1. 先共地，再接信号，最后接 5V。
2. StickS3 的 `EXT_5V` 默认关闭，不要依赖它给外部供电；此处是由 Cardputer 的 5VOUT 反向喂 StickS3 的 5V_IN。
3. 帧数据用 RGB565 + 行内 RLE，96×96 一帧可从 18 KB 压到 3–6 KB。
4. Cardputer 侧 SPI 作 slave 时不要与 microSD（同用 G40/G14/G39）抢总线，二选一。

### 9.8 结论

| 诉求 | 判定 |
|---|---|
| 电气可连 | **可以**，SPI/UART/I2C 全部现成，电平一致 |
| 提高处理性能 | **不成立**，无共享内存、Cardputer 无 PSRAM，算力不可叠加 |
| 扩展屏幕 | **不成立**，两块屏同为 1.14" 240×135 / 135×240 |
| 真正的升级路径 | 单芯片做 recomp + 复用 8 MB PSRAM；若要更强算力，换 ESP32-P4（400 MHz 双核 RISC-V，M5 有 Tab5 / CoreP4X）或 Linux SBC |
| 合理用法 | Cardputer-Adv 当「电池 + 键盘 + SD 卡 + 副屏」终端，StickS3 专心跑模拟器 |
