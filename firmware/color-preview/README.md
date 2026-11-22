# color-preview — AI Passport 可见画面验证固件

## 这是什么

这是一版**显示链路 + 按键链路验证固件**，不是 unSP 模拟器，也不是最终游戏固件。

它做三件事：

1. 上电先画一张**合成静态验收图**：四角红/绿/蓝/白 + 白色箭头；红条固定在输出顶部，
   绿条固定在底部。用来判断屏幕方向、颜色顺序和“哪边是上”，不再依赖 MAME 第 60 帧
   （那一帧本身是转场，文字和彩条错位）。
2. 静态图之后回放 MAME 0.289 digicolr 录下的 96×96 参考帧，验证 ST7789P3 初始化、
   SPI2 + DMA、96×96 → 192×192 最近邻放大、zlib 解帧和 10 fps 定时。
3. 上电静态图、每次按键旋转后，把 320×240 RGB565 输出帧通过串口 RLE dump 出来；
   PC 侧解码成 PNG，可在没有摄像头的情况下检查固件实际送屏内容。

它**不执行** internal_rom_221017a.bin，**不读** dmc_v1_base.bin，没有 CPU、定时器、
中断、SPI flash、LCD 控制器、RTC、存档或压电片音频。ROM 不在固件里；真正的模拟器要等
P1/P2 接上 unSP 核心后再做，并且必须保留 NO ROM 不 step 的兜底。

## 当前产物（2026-11-20 13:2x 构建）

| 文件 | 大小 | SHA256 |
|---|---:|---|
| build/color-preview.bin | 666,000 B | E16E9552FB696881D1976B10EAEC0A2B2B415BC8BBC50FE8BE598590D75AD162 |
| color-preview-full.bin | 731,536 B | 9ADDC29B5EC83931736EFD6E3D26AADF0B6F4862C27FFCCD65B97F14A9CCB3A2 |

- app 分区 1 MB，当前 app 占 64%，剩 36%。
- 帧包 main/frames.bin：433,520 B，150 帧，10 fps，15.0 s。
  SHA256 4aa2ac9e0979a71a8f1a6b5dc045b0acb08b7b76aeb301598adf59e4d8fa6ed1。
- 帧源：项目根目录 short.avi（MAME 60 fps，1275 帧），每 6 帧取 1 帧。
- 像素格式：RGB565 大端；帧包保持 MAME 原始方向，旋转在固件运行时做。

## 静态验收图

show_static_pattern() 在 96×96 源坐标里画：

- 上左红块、上右绿块、下左蓝块、下右白块（各 16×16）；
- 白色向上箭头（源坐标），先在源坐标画，再按 s_rot 旋转、2× 放大。

输出坐标的顶部 8 px 红条、底部 8 px 绿条**不参与旋转**，所以它们永远标记物理屏幕的
上下边。当前默认 s_rot=2（180°），因此屏幕上应看到：

- 红条在上、绿条在下；
- 上左白、上右蓝、下左绿、下右红；
- 箭头向下。

按任意键，s_rot 循环 0/90/180/270，静态图重显 5 s；再按会继续转。上电静态图停留
15 s，然后进入参考帧回放，从第 0 帧开始。PC 侧期望图：

- docs/static_expected_180.png（当前默认）
- docs/static_expected_0.png
- docs/static_expected_rotations.png（0/90/180/270 四宫格）

生成工具：tools/render_static.py。

## 屏幕 dump

固件在上电静态图、每次按键旋转后各自动 dump 一次 320×240 输出帧；dump 不读 ST7789
GRAM，而是把**准备送给面板的 framebuffer**按 RLE 行发到串口。它能证明旋转、四角颜色、
红/绿条位置和箭头方向；玻璃实际显示仍需人眼/照片确认。

协议：

~~~
SHOT_BEGIN 320 240 RGB565 rot=180 why=boot-static
SHOT_ROW <y> <x> <count> <RGB565 hex>
...
SHOT_END
~~~

一行一个 run，count 是连续像素数。串口同时有日志，解码器只取 SHOT_BEGIN..SHOT_END
之间的 SHOT_ROW。

采集与解码：

~~~powershell
& 'C:\Users\DuWeisi\.espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe' tools\reset_capture.py COM4 firmware\color-preview\boot_dump.log 45
& 'C:\Users\DuWeisi\py311\python.exe' tools\shot_decode.py firmware\color-preview\boot_dump.log docs\device_shot_boot.png
~~~

reset_capture.py 会复位设备，所以先抓到 boot-static dump；要抓按键 dump，保持采集
运行并在 45 s 内按任意键即可。本次实测 boot dump 728 行 RLE，解码结果与
docs/static_expected_180.png **像素完全一致**：红条在上、绿条在下、上左白、上右蓝、
下左绿、下右红、箭头向下。样例图：docs/device_shot_boot.png。

如果你在设备上看到的不是这张图，问题在面板初始化/物理方向或排线，不在渲染和旋转。

## 按键：ADC 电阻梯，不是数字键

AI Passport 三键共用一个 ADC 引脚 GPIO0，外部电阻梯分压；旧版直接读
gpio_get_level(GPIO0) 是错的。本版：

- ADC1_CH0，12 dB 衰减，4 次采样平均；
- adc_cali_create_scheme_curve_fitting 校准；
- 按下阈值 <1900 mV，释放 >2100 mV；
- 初始化失败回退 GPIO 上拉数字读取；
- 三键暂时不区分，任意键只做“旋转 90°”。

阈值来自旧项目 C:\Users\DuWeisi\digimon-passport 的
components/bsp/include/bsp_pins.h:69：key0 0–150 mV、key1 150–447 mV、
key2 447–1900 mV、松开 >1900 mV。

真机空载实测 button boot mv=2944 down=0，说明 ADC 校准和“释放”判定正常；
**真实按键边沿尚未实测**（需要有人按一下）。按下后日志应出现：

~~~
I (xxx) color-preview: button edge mv=... -> rot=...
~~~

## 构建

在 firmware/color-preview 下：

~~~powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1
~~~

本机没有 pwsh 可直接执行脚本，且默认执行策略会拦截 .ps1，所以必须带
-NoProfile -ExecutionPolicy Bypass。构建使用 ESP-IDF 5.5.3、riscv32-esp-elf、
CMake/ninja 和项目自带的 Python venv。脚本会重新生成 app、合并
color-preview-full.bin，并打印实际大小与 SHA256。

构建日志里的 ESP_ROM_ELF_DIR 未定义警告来自 gdbinit 生成步骤，不影响 bin。

## 烧写（设备接上 COM4 后）

当前项目真机端口是 **COM4**；COM6 是 Cardputer ADV，不要刷错。

整片备份（已存在，不要覆盖）：

~~~
docs/backup/digimon-color-passport-COM4-8MB-20261120-1218.bin
8,388,608 B
SHA256 C74EA4FF72A82F54326892D71F7A4081C55958A0D8B029190660742004CA48F9
~~~

写合并镜像：

~~~powershell
& 'C:\Users\DuWeisi\.espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe' -m esptool --chip esp32c3 -p COM4 -b 460800 write_flash 0x0 color-preview-full.bin
~~~

合并镜像已含 bootloader（0x0）、分区表（0x8000）、app（0x10000），不需要再单独写
三个偏移。写完后按复位；串口同时从 UART0 和 C3 原生 USB-Serial/JTAG 输出。

复位抓日志可以用：

~~~powershell
& 'C:\Users\DuWeisi\.espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe' tools\reset_capture.py COM4 firmware\color-preview\device_watch.log 20
~~~

reset_capture.py 用 DTR=IO0 保持高、RTS=EN 拉低 100 ms 再释放，正常启动到 app；
不要用 esptool 的 USBJTAGSerialReset 序列（DTR=IO0 低），那会进下载模式。

## 预期日志

~~~
I (471) color-preview: button ADC ready: press < 1900 mV, release > 2100 mV
I (510) color-preview: PREVIEW_STATIC rot=180 (red top, green bottom; arrow up at rot=0)
I (510) color-preview: button boot mv=2944 down=0
I (15511) color-preview: static done; video from frame 0
I (15543) color-preview: PREVIEW_READY first_frame=0 rot=180
I (15543) color-preview: frame 0/150
...
~~~

失败时屏幕显示红蓝棋盘格、背光 60%，串口打印 PREVIEW_RESULT FAIL ...，不会静默黑屏。

## 已知限制

- 参考帧视频本身有 MAME 录制的上下错位/滚动/镜像，看起来像“画面在滚”，不是 ST7789
  扫描问题；静态验收图用于把两者分开。
- 没有 unSP CPU、内存映射、定时器、中断、SPI flash、ST7735 控制器、RTC、存档和压电片音频。
- 画面是预录参考帧，不是 ROM 执行结果；不能用来判断游戏逻辑、计时或输入。
- 没有 NO ROM 分支，因为这个固件根本不加载 ROM；后续模拟器固件必须加。
- 三键未拆分语义；只做任意键旋转。
- **没有照片/录屏**；红条是否真的在物理顶部、四角颜色/箭头方向是否与期望图一致，
  仍需肉眼或拍照确认。

## 下一步

1. 有人按键时确认串口出现 button edge，并确认静态图按 90° 旋转。
2. 拍照/录屏对照 docs/static_expected_180.png；若红条不在物理上方，先查
   display.c 的 swap_xy/mirror，不要先改帧包。
3. 接 P1 的 unSP 解释器/静态翻译块，先跑 0–17 s golden 段，再谈输入和音频。
4. 模拟器固件保持 ROM 外置、NO ROM 不 step、整片备份和版本号同源。
