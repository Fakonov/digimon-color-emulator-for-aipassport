# R8 功耗与电源路径审计

> 角色：功耗与电源路径审计员（本轮第一目标：电池续航，主攻手）
> 版本基线：v1.18.0（HEAD 741a49e），源码只读，未改任何源码/配置/生成物，未跑 idf.py，未刷机，未 git。
> 结论一律「文件:行号 + 代码摘录」，并标注 实测 / 静态推导 / 推测。

## 0. 结论摘要（5 条，按「电流节省 × 时间占比」排序）

1. **P0 · 待机时 guest 仍在 60 Hz 全速跑（最大浪费）**。`app_main.cpp:3565` 只把 `blit_frame` 挡在 `!s_standby` 之外，但 `run_guest_frame()`（`app_main.cpp:3497`）在待机分支外照跑。待机省掉的只有背光（~24 mA），CPU 仿真（~20 mA）一分没省。这是背光之外唯一可切换的大负载。
2. **P0 · 待机下仍有两条「推屏/刷面板」路径**：fps 叠加层每 1 s `display_draw_bitmap(2,2,24,12)`（`app_main.cpp:3769`→`863`）无 `!s_standby` 门控，会在 STANDBY 卡上盖数字；crcwatch 每 ~5 s 打一条 INFO 日志（`app_main.cpp:3624`）阻塞串口。两者都应加 `!s_standby` 门控。
3. **P1 · 帧末 1.5 ms spin 只能收到 ~1.0 ms**。`app_main.cpp:3694-3698` 的 spin 实际是 1.5–2.5 ms；`CONFIG_FREERTOS_HZ=1000`（`sdkconfig:1281`）下 `vTaskDelay` 粒度 1 ms，1.0 ms 是 spin 下限，再低要 tickless 或 timer（注释已证 timer 更慢）。
4. **P1 · 降频 160→80 MHz 不可单独做**。`CONFIG_PM_ENABLE` 未开（`sdkconfig:1042`），无 DFS。纯降频会使 guest 吞吐减半（pet 变慢一半）；必须配「30 fps + 每帧 2 倍 guest 周期」才保游戏时间，且重绘场景会超帧预算。需 A/B。
5. **P1 · 自动存档 60 s 的实际写放大被「脏检测被 RTC 击穿」放大了**。`snapshot_machine_state` 的 CRC 脏检测（`app_main.cpp:2690-2693`）整片算 `s_soc.ram`，但 guest 时钟每秒变 → 待机也每 60 s 擦 3 扇区；活跃时再加 fbsave 单块 32 KiB（无磨损均衡）。

---

## 1. 范围与方法

读了（按角色范围）：
- `firmware/digicore-c3/main/app_main.cpp` 全文关键段：帧循环 3090-3992、standby 1962-2060、update_sleep_backlight 2754-2830、boot 3289-3400、snapshot/persist 2561-2752、blit 1040-1188、fps overlay 825-865、常量 75-121/460-510。
- `display.c`（全文 272 行）、`buttons.c`（全文 224 行）、`battery.c`（全文 197 行）、`audio.c`（全文 514 行）、`menu.cpp`（40-159 行）、`saveimg.cpp`、`gueststate.cpp`、`gueststate.h`、`saveimg.h`、`fbsave.cpp`。
- `sdkconfig`（grep 电源/时钟/串口/日志/flash 相关项）、`sdkconfig.defaults`（全文）。
- R7 分报告 `R7_app.md`/`R7_periph.md`/`R7_build.md` 中与本范围交叉的结论。

只读命令：`grep`（sdkconfig 项、符号引用）、`read` 读源码。未整读 aot_blocks.c / *.bin / *.avi。

三本账基线（引自 R8_BRIEF/R7，实测）：CPU 固定 160 MHz（sdkconfig）、`CONFIG_PM_ENABLE` 未开、flash DIO 80 MHz、SPI 写 80 MHz 实测 39.8 Mbit/s、背光 LEDC 5 kHz/10bit 默认 80%、AUTO OFF 默认 16 s、AUTO SAVE 默认 60 s、帧末 spin 1.5 ms。

---

## 2. 发现表

| ID | 位置(文件:行) | 现象/证据 | 影响机制 | 估算(标注) | 与R7关系 | 建议改动 | 风险 | 验证 | 优先级 |
|---|---|---|---|---|---|---|---|---|---|
| F1 | `app_main.cpp:3565` + `3497` | `if (s_display_ok && !s_standby && !menu_is_open()...)` 只挡 blit；`run_guest_frame()` 在菜单 else 分支（3469-3498）里，standby 不 park guest | 待机背光灭了但 CPU 仍 60 Hz 全速仿真 | 待机电流 ≈26 mA（推测，见 §4）；guest 是背光外唯一大负载 | R8_BRIEF 43-44 已点出 | 待机路径降 guest 吞吐（见 F3） | 中（改游戏时间） | 真机 STAT/`mi_s` 对比待机前后 | P1 |
| F2 | `app_main.cpp:3769`→`draw_fps_overlay` `863` | `draw_fps_overlay` 每 1 s 无条件 `display_draw_bitmap(2,2,24,12)`；3757-3771 块无 `!s_standby` 门控 | 待机时仍推 24×12 px，盖在 STANDBY 卡左上角 | ~0.06 ms SPI/s，可忽略能量；主要是「背光关了还在推屏」的代码级证据 + 产品可见常驻数字 | R7_app B6 已报「常驻叠加层」 | 3757-3771 加 `!s_standby`（建议同时 `#if C3_FPS_OVERLAY`） | 极低 | 待机抓屏无数字 | P0 |
| F3 | `app_main.cpp:3606-3655`（crcwatch） | `++s_crc_frames>=30` 每 ~0.5 s CRC 32 KiB；`if(++s_dbg%10==0) ESP_LOGI("crcwatch...")` 每 ~5 s 打 INFO，无 standby 门控 | 待机仍每 0.5 s 算 CRC、每 5 s 阻塞串口 ~4 ms | ~0.05% core + 每 5 s 一次串口 | R7 未覆盖此待机视角 | crcwatch 整块加 `!s_standby` 门控 | 极低（stall 踢腿只对可见画面有意义） | 待机串口无 crcwatch 行 | P0 |
| F4 | `app_main.cpp:3694-3698` | `if (wait>1500) vTaskDelay((wait-1500)/1000); while(esp_timer_get_time()<s_deadline_us){}` | spin 实际 1.5–2.5 ms/帧（delay 向下取整） | 1.5–2.5 ms × 60 ≈ 9–15% wall clock 忙等；转 WFI 省 ~1 mA | R7_app D6 已标 9%，标注「不动」 | `1500`→`1000`（再低需 tickless/timer） | 中（帧间隔 p95 上抬） | `J 1` 看 ft p50/p95、ov20 | P1 |
| F5 | `sdkconfig:1042` + `1072` | `# CONFIG_PM_ENABLE is not set`；`CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_160=y` | 无 DFS/无 light sleep，CPU 恒 160 MHz | 降频省 CPU 动态电流（≈线性，推测 ~10 mA @80MHz） | R8_BRIEF 29 已核实 | 仅待机路径开 DFS+80MHz，需配 30fps+2×周期 | 高（游戏时间/APB） | A/B `mi_s`/fps/游戏时间 | P1 |
| F6 | `display.c`（全文无 SLPIN）| 全文只有 `esp_lcd_panel_disp_on_off(true)`（`:186`），无 `esp_lcd_panel_disp_sleep`/SLPIN(0x10)/SLPOUT(0x11) | 暗屏后面板仍扫描 GRAM | 省 ~1–3 mA（ST7789 sleep 到 µA 级，推测） | R8_BRIEF 31 留作可行性 | 暗屏后 SLPIN，唤醒 SLPOUT+重推 | 中（唤醒 120 ms+重画） | 真机唤醒时延 + 电流 | P1 |
| F7 | `buttons.c:39-43,212-219` | `BTN_POLL_MS 5`、`BTN_ADC_SAMPLES 4`、`BTN_DEBOUNCE 1`；task 每 5 ms 采样 | 200 次/s 唤醒 ADC | ~2% duty ≈0.4 mA（推测） | R7_app 附「无死代码」 | 改 GPIO 边沿中断+10ms 退避 | 中（GPIO0 strapping、模拟梯、三键不可数字区分） | 真机按键延迟/漏按 | P2 |
| F8 | `menu.cpp:51,70` + `app_main.cpp:2808-2827` | `kBrightChoices {20,40,60,80,100}`；`s_bright_idx=3`→80%；auto-off 16 s 已灭背光 | 背光是亮屏时最大负载，已 auto-off | 80%≈24 mA（推测 30 mA 满） | R8_BRIEF 33 已核实 | 仅待机卡 3 s 降亮（收益极小） | 低但收益≈0 | — | P2 |
| F9 | `gueststate.cpp:131`、`fbsave.cpp:80,26`、`app_main.cpp:2690-2708` | 存档擦 3 扇区+32 KiB 单块；CRC 脏检测整片 RAM 被 RTC 击穿；resumable 时每次附 fbsave | 60 s 存档实际每 60 s 擦 3（待机）/7（活跃）扇区；fbsave 单块无磨损均衡 | ~300 ms/次、平均 ≈0.1 mA（推测）；磨损是主风险 | R7_periph §2.7 已列三条路径 | 脏检测排除 RTC 区；fbsave 磨损均衡/延长周期 | 中（漏存进度风险） | 计数擦除次数、A/B | P1 |
| F10 | `audio.c:12-14,307-317,341-352` | 功放无 GPIO 常开；codec 15 s 后 `codec_power_off` 关模拟轨+MCLK | 功放常开损失 ~1–3 mA（推测），固件无法关 | — | R7_periph §2.4 已核 | 无固件动作（硬件） | — | 电流表实测 | P2 |
| F11 | `sdkconfig:1106,1111` + `app_main.cpp:3405` | 双控制台 UART0+USB-Serial-JTAG；`debug_poll` 每帧 4 读 | 待机 UART/USB 空闲电流小；是 DMC1/console 数据通路 | 电池态可忽略；USB PHY 未枚举不耗电 | R7_build §3.4 | 保留（数据通路） | — | — | 不动 |

---

## 3. 必答（每条给数字或明确假设）

### Q1 待机（STANDBY + 背光已灭）CPU 占空比 / 可切换负载 / guest 60 Hz 是否最大浪费

**是，guest 60 Hz 全速是最大浪费。** 证据：`app_main.cpp:3565` 只把 blit 挡掉，`run_guest_frame()`（`:3497`）在待机分支外照跑；`standby_enter`（`:1985-2002`）注释「Enter leaves the guest running… behind the dark screen all along」。

- CPU 占空比（静态推导）：`frame_budget_track`（`:100-115`）把 guest 工作自适应到 ~14 ms/帧（12.5–15.5 ms 目标），加 spin 1.5–2.5 ms + 固定开销 ~0.5 ms ≈ 16 ms/16.7 ms ≈ **84–90% 占空比**。待机只少了 blit 的间歇 SPI，占空比几乎不变。
- 可切换负载（推测，按数据手册量级）：背光 80%≈24 mA（已灭，省掉）、CPU 仿真 ≈20 mA（未省）、面板扫描 ≈2 mA、功放常开 ≈2 mA、flash ≈1 mA、其余 ≈1 mA。
- 待机电流 ≈ **26 mA**（推测）；若把 guest 在待机停掉只喂时钟，CPU 落到 WFI ≈3 mA，待机降到 ≈9 mA，**待机续航约 ×3**。

### Q2 降频 160→80 MHz 可行性

- 前提不成立：`CONFIG_PM_ENABLE` 未开（`sdkconfig:1042`），无 DFS；要降频必须开 PM+DFS（本身是构建+配置改动，需 A/B）。
- 时钟约束：flash 80 MHz 是独立 MSPI（`sdkconfig:460`），不随 CPU 降；SPI2 实测 39.8 Mbit/s（`app_main.cpp:404-406`），C3 上 80 MHz CPU 时 APB 仍 80 MHz，LCD SPI 不受影响 → **只 CPU 执行吞吐减半**（静态推导）。
- 吞吐：guest 工作以 host cycles 计，80 MHz 下 host cycles/s 减半。`frame_budget_track` 仍把每帧 guest 工作压到 ~14 ms → 每帧 guest 周期减半 → **pet 时间变慢 ~2×，不可接受**。
- 「80 MHz + 每帧跑 2 倍 guest 周期」（=30 fps 面板 + 1,066,666 guest 周期/帧）的游戏时间账：guest 推进 = 30 × 1.067 M = **32 M guest 周期/s，与 60×533k 持平，游戏时间保真**；但单帧仿真 1.067 M 周期，在折叠场景（~2.1 host cyc/guest）≈2.24 M host cyc = 28 ms（<33.3 ms 可行），重绘场景（~4.2）≈4.5 M = 56 ms（超帧预算 → 丢帧/快进）。**只在折叠场景成立**。
- 恢复延迟：DFS 切频毫秒级，但唤醒需从 80→160 立即拉回；失败回退=释放 esp_pm lock 回 160 MHz。**P1，需真机 A/B。**

### Q3 降帧（待机 30 Hz + 每帧 2 倍周期，不切频）能省多少

- 游戏时间：保真（同上 32 M cyc/s 不变）。
- 省的固定开销（静态推导）：`run_guest_frame` 外的每帧固定调用（`debug_poll/update_buttons/set_sleep_inhibit/audio_set_buzzer/sync_time_lapse/clock_seed_step/clock_sync/frame_budget_track/window_has_content/crcwatch/persist_save_image/maybe_snapshot/update_sleep_backlight/audio_idle_poll`）从 60 次/s 降到 30 次/s；spin 从 ~1.5-2.5 ms×60≈90-150 ms/s 降到 45-75 ms/s。
- WFI 占比：60 Hz 时 work~14 ms+spin~2 ms ≈ 16 ms/16.7 ms（WFI 极小）；30 Hz 时 work~28 ms+spin~2 ms+固定~0.5 ms ≈ 30.5 ms/33.3 ms → **WFI ≈ 8%**。省的主要是固定开销+spin+更少唤醒，约 **1–2 mA**。
- 唤醒影响：`update_buttons` 在 30 Hz 排空 latch → 唤醒延迟 +16 ms；guest 时间不受影响。面板流畅度：guest 只按脏块重绘，静态屏无感，动画减半。

### Q4 帧末 1.5 ms spin 能否降到 0.2–0.5 ms

**不能。** `app_main.cpp:3694-3698` 的 spin 实际是 **1.5–2.5 ms**（`vTaskDelay((wait-1500)/1000)` 向下取整，spin = wait − delay_ms×1000）。`CONFIG_FREERTOS_HZ=1000`（`sdkconfig:18`）下 `vTaskDelay` 粒度 1 ms，注释（`:3684-3689`）已说明「one tick round-off is 1 ms… spin only has to cover that plus scheduling jitter」→ **1.0 ms 是下限**；0.2–0.5 ms 要么上 tickless idle、要么 esp_timer one-shot（注释已测：task 分发 +0.4 ms，更慢）。
- 可落地：`1500`→`1000`，省 ~0.5–1 ms/帧 ≈ 3–6% wall clock（~1 mA）。**验收标准**：`J 1` 下 `ft` p50≤17 ms、p95≤18 ms、`ov20=0`、`guest_fps` 56±1 不变。

### Q5 按钮 5 ms ADC → GPIO 边沿中断 + 10 ms 退避

- 收益（推测）：5 ms 轮询 ≈ 200 次/s 唤醒，~2% duty ≈ 0.4 mA，省掉。
- 风险：① 电阻梯是模拟量，三键电压 0-150/150-447/447-1900 mV（`buttons.c:5-8`），数字阈值下**三键都读 LOW**，边沿中断只能知「有按下」不能知「哪个」，仍需 ADC 分类 → 中断仅替代轮询当唤醒触发器；② GPIO0 被点名是 strapping 脚（若 boot 采样需上机确认时序，当前 `button_fallback` 已把它配成 input+pullup `:69-77`）；③ 10 ms 退避比当前 5 ms 延迟 +5 ms（相对 ~16.7 ms 主机帧可忽略，但模拟边沿噪声/去抖更复杂）。**P2，本轮不做。**

### Q6 ST7789 暗屏后是否仍扫描 / SLPIN 收益 / 唤醒代价

- 现状：`display.c` 全文无 SLPIN/SLPOUT，仅 `esp_lcd_panel_disp_on_off(true)`（`:186`）；**暗屏后面板仍扫描 GRAM**。
- SLPIN 收益：ST7789 sleep 电流 µA 级（推测），省 ~1–3 mA。
- 唤醒代价：SLPOUT 需 120 ms（数据手册）+ 重推整窗 ~30 ms（`standby_leave` 已置 `s_blit_repaint_all=true`，`:2017`）≈ **150 ms「按键→可见」**；当前是背光即时恢复。**是否可接受取决于产品，P1，需 A/B + 用户验收。** 若与 Q1（冻结 guest）配对，GRAM 不脏，可省重推。

### Q7 背光 80% 能量占比 / 无感降亮

- 占比：背光 80% ≈ 24 mA，约占活跃电流 50 mA 的 **~48%**（推测），是亮屏时最大单项。
- 已 auto-off（16 s，`app_main.cpp:2808-2812`）。
- 不改默认的前提下「按场景降亮」：唯一无感场景是 STANDBY 卡那 3 s（`kStandbyGraceMs=3000`，`:483`）——从 80% 降到 20% 只省 ~18 mA×3 s ≈ 54 mAs/次，**收益≈0**。亮屏主动场景背光就是感知亮度，无「无感」降法。**P2，无意义，除非改默认或缩短 auto-off。**

### Q8 flash 写放大（60 s 自动存档）

- 已有脏检测但被击穿：`snapshot_machine_state` 用整片 `s_soc.ram` CRC（`app_main.cpp:2690-2693`），guest 时钟（`0x0CAD/CAE/CAF`，`clock_sync` 每帧写，`:3507`）每秒变 → **待机也每 60 s 触发存档**。
- 实际擦写（静态推导）：待机（PC 在 ROM，非 resumable）→ gueststate 擦 3 扇区（`gueststate.cpp:131`，12 KiB）+ 写 blob+8 KiB；活跃（resumable）→ 再加 fbsave 擦 32 KiB（`fbsave.cpp:80`，4 扇区）+ 写 24,576 B。合计 **3（待机）/7（活跃）扇区 + ~45 KiB 写 / 次**。
- 时间/电流：fbsave ~150 ms（`app_main.cpp:2704` 注释）+ gueststate 3 扇区 ~110 ms + 写 ~50 ms ≈ **300 ms/次**；flash 擦写电流 ~15–20 mA 额外（推测）→ 平均 ≈0.1 mA（能量小）。
- 真风险是磨损：fbsave 是单块固定 0x0F000（`fbsave.cpp:26`），**无磨损均衡**，活跃时 1440 擦/天（推测 ~69 天到 100k 次）。
- 改进：脏检测排除 RTC/时钟区（防待机空写，P1）；fbsave 磨损均衡或延长周期（菜单已给 120/300/OFF，`menu.cpp:57`，P1/P2）。

### Q9 日志/UART/USB console/crcwatch/STAT 常驻成本

- STAT：默认关（`s_stat_period_s=0`，`app_main.cpp:497`）；LONGF 仅 >60 ms 帧。无周期输出。
- crcwatch：每 ~5 s 一条 INFO 阻塞串口（`app_main.cpp:3624`）——**唯一 release 该关的常驻项**（F3，P0 加 `!s_standby` 门控）。
- UART0 115200 + USB-Serial-JTAG 双控制台：电池态空闲电流可忽略（USB PHY 未枚举不耗电），且是 console/DMC1 数据通路，**不能关**。
- `debug_poll` 每帧 4 读、`audio_set_buzzer` 每帧软除+timer 读：可忽略（~0.03–0.1%）。

### Q10 「背光关了但仍在推屏/刷面板/跑全帧」的路径（代码级证据）

有，两条：
1. **fps 叠加层**：`app_main.cpp:3769` 每 1 s 无条件调 `draw_fps_overlay`，函数内 `display_draw_bitmap(2,2,24,12)`（`:863`）真推屏；3757-3771 块无 `!s_standby` 门控 → 待机在 STANDBY 卡上盖 3 位数字。
2. **crcwatch**：`app_main.cpp:3606-3655` 待机仍每 0.5 s CRC + 每 5 s 串口日志。
3. （硬件）面板本身：无 SLPIN，暗屏后仍扫描（F6）。
- 主窗口 blit 已正确门控（`:3565` `!s_standby`），`run_guest_frame` 仍全速（属 F1）。

---

## 4. 续航估算公式与假设

```
T(h) = C_usable / ( I_active·d + I_standby·(1−d) )
```

假设（**全部为推测**，无电流表实测，按 ESP32-C3 / ST7789 / ES8311 / 板级拓扑量级）：

| 项 | 数值 | 性质 |
|---|---|---|
| C_usable（Wanli 500 mAh 有效） | 425 mAh | 推测（×0.85） |
| CPU 160 MHz 活跃 | 20 mA | 推测 |
| CPU WFI 空闲 | 3 mA | 推测 |
| flash DIO | 1 mA | 推测 |
| 背光 @80% | 24 mA（满 30） | 推测 |
| 面板扫描 ST7789 | 2 mA | 推测 |
| 功放常开 | 2 mA | 推测 |
| 其余（CW2017/I2C/LEDC） | 1 mA | 推测 |
| I_active（活跃，guest ~85% duty） | ≈50 mA | 推测 |
| I_standby（待机，guest 仍全速） | ≈26 mA | 推测 |
| I_standby'（冻结 guest，WFI） | ≈9 mA | 推测 |

续航（推测）：
- 纯活跃：425/50 ≈ **8.5 h**
- 纯待机（现状）：425/26 ≈ **16 h**
- 纯待机（冻结 guest，Q1 落地）：425/9 ≈ **47 h**（≈×3）
- 纯待机（+SLPIN）：425/7 ≈ **60 h**

排序「电流节省 × 时间占比」：① guest 待机冻结（26→9 mA，省 17 mA）＞ ② 背光（已 auto-off，24 mA，已落地）＞ ③ 80 MHz 待机 DFS（省 ~8 mA）＞ ④ SLPIN（~2 mA）＞ ⑤ spin→WFI（~1 mA）＞ ⑥ 功放/按键（<1 mA，硬件/风险）。

---

## 5. 需要主审裁决的冲突/不确定项

1. **P0 的 fps 叠加层去留**：R7_app B6 已建议 `#if C3_FPS_OVERLAY`（默认 0）。与本轮「待机推屏」结论一致：建议至少加 `!s_standby` 门控，是否整块编译掉请主审拍板（它是不接串口读帧率的唯一手段，bench 时仍要）。
2. **guest 待机冻结（F1/Q1）的游戏时间语义**：冻结 guest 会停掉 pet 的时钟与 Z 动画（`app_main.cpp:3594-3605` 注释「guest clock keeps running」是现状）。若要保游戏时间，需「冻结期间只喂 wallclock/快进补偿」或改「降频不冻结」。这是产品行为决策，非技术可单方定。
3. **80 MHz 是否值得为「待机省 8 mA」开 DFS**：DFS/`CONFIG_PM_ENABLE` 会引入功耗管理与时钟切换回调，改动面远超本轮 P0；且重绘场景 2×周期超预算。倾向 P2（只报告），除非主审明确要待机激进省电。
4. **SLPIN 的 120 ms 唤醒延迟**是否可接受，需产品验收，无法由审计单方裁。
5. **存档 CRC 排除 RTC 区**：排除哪些字节才算「进度无变化」需要定义（排除 `0x0CAD-0x0CB0` 时钟区即可覆盖本发现），但若 ROM 用该区存别的状态会漏存——需与模拟器核心审计员对账后再动。

---

## 6. 明确不动的清单（防误删）

1. `app_main.cpp:3565` 的 `!s_standby` blit 门控本身——正确，保留。
2. 帧对齐 spin（`app_main.cpp:3672-3699`）整体结构——只允许 `1500→1000` 这一档微调，勿删（删了掉帧）。
3. `audio.c:307-317` codec 15 s 掉电序列与 `audio_idle_poll` 的 `s_last_beep_us` 竞态保护（`audio.c:341-352`）——已实测，勿动。
4. `battery.c` CW2017 profile（80 B）与 sleep/active 顺序（`battery.c:81-135`）——厂商模型，勿动。
5. 三条 save 路径的掉电撕裂保护（`fbsave` 两遍流式读、`gueststate/saveimg` 只回读头部校验）——DRAM 无第二块缓冲，删即失去撕裂检测（R7_periph §2.7 已核）。
6. `display.c` ST7789P3_CMDS 厂商序列（`:49-65`）——勿精简。
7. 双控制台 UART0+USB-Serial-JTAG（`sdkconfig:1101-1109`）——DMC1/console 数据通路，勿关。
8. `s_pace_timer/s_pace_sem`（`app_main.cpp:592-599,3365-3377`）——仅 create 未 start，零功耗，属冗余审计范围，本报告不列为功耗项。

---

*报告完毕。本轮未改任何源码/配置/生成物，未跑 idf.py，未刷机，未 git；唯一写入文件为本报告。*
