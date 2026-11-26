# 第五轮代码审计与优化：画面流畅性

日期：2026-11-26
基线：v1.12.0（`firmware/digicore-c3`）
本轮版本：**v1.13.0**
性质：**源码审计 + 改动 + 编译验证 + 上机实测**。用户诉求是「画面活动过程看着还是卡」，
因此判据不是平均 fps，而是**帧间隔（frame interval）和它的尾部**。

---

## 0. 结论先说

### 0.1 三个数字（同一台设备、同一场景：时钟界面 `pc=200343`，COM4）

| 指标 | v1.12.0 | v1.13.0 | 变化 |
|---|---|---|---|
| `guest_fps` | 18.48 | **56.40** | **3.05×** |
| 帧间隔 p50 | 51 ms（全部落在 ≥32 ms 桶） | **16 ms** | −69 % |
| `gmax`（单帧 guest 最长） | 51.0 ms | **14.0 ms** | −73 % |
| 游戏速度（`guest_fps × 每帧 guest cycles`） | 29.6 M cycles/s | **90.2 M cycles/s** | **3.05×**（原机 96 MHz 的 94 %） |
| `mi_s`（M guest instr/s） | 7.43 | **22.40** | 3.01× |
| `guest_pct` | 93 | 71 | −22 pt |

同一场景 park（`pc=005251/005376`）：56.5 fps、`gmax` 10.3 ms，与基线 57 fps 持平。

### 0.2 根因不是「优化不够」，是三条配置/路径错误

1. **`C3_WAIT_COLLAPSE=0`：设备跑了一条参考实现从不跑的路径。**
   ROM 的等待循环（`0x230321`）每圈 298 guest cycles、75 条指令，要经过 3 次
   block dispatch；设备构建把这个折叠关掉后，一圈要 1478 host cycles，其中 1200
   花在调度三个 1～5 条指令的外壳块上。PC 侧参考实现（`emu/unsp_glue.cpp:1146`）
   **无条件调用** `wait_loop_collapse()`，所有 golden 验证都是在开启状态产出的。
   第二轮审计（`AUDIT_PERF_2026-11-24_R2.md` §0.3）测出「开启更慢」的场景是
   `pc=5251/5376`，那里折叠函数一定 `return false`，只剩调用开销——**从未在时钟
   场景测过**。开启后：`gmax` 51 → 7 ms，`guest_pct` 93 → 31。
2. **帧预算固定 1/60 秒（`s_frame_cycles = kCpuClock/60`），把 51 ms 的执行时间
   摊在一帧里。** 每帧模拟量不变，帧率就只能跟着宿主速度走。改成 1/180 后同一
   场景 51 → 19 ms；折叠生效后再让自适应把每帧模拟量推回 1.6 M（实时速度）。
3. **STAT 日志每 5 秒一次、约 810 B，115200 波特率双控制台阻塞 ≈ 72 ms。**
   实测 `ftmx` 每 1 秒一次 79–81 ms（`J 1` 时），即「规律性顿一下」。默认改成
   `J 0`（台架用 `J 5` 打开）。

### 0.3 一句话
**这一轮把「时钟界面 18.5 fps / 每帧 51 ms」变成「56 fps / 每帧 14 ms / 游戏速度
接近实时」，主要靠恢复一个被误关的折叠开关 + 让帧预算自适应，而不是新写优化。**

---

## 1. 审计方法

- 读源码：`main/app_main.cpp`、`main/emu/unsp_glue.cpp`、`main/emu/aot.h`、
  `main/emu/gpce4.cpp`、`main/emu/st7735.cpp`、`main/display.c`、`main/menu.cpp`、
  `main/bezel.cpp`、`main/audio.c`、`main/gueststate.cpp`、`main/fbsave.cpp`、
  `main/CMakeLists.txt`、`tools/aot.py`、`tools/sync_c3_emu.py`。
- 读历史：`AUDIT_PERF_2026-11-24.md`、`AUDIT_PERF_2026-11-24_R2.md`、
  `RESEARCH_PERF_ROUTES_2026-11-24.md`、`C3_PORT.md`、`PLAN_AOT.md`、
  `HANDOFF_2026-11-22.md`。
- 四路子 agent 并行审计（AOT 热路径、显示管线、宿主每帧开销、外部资料），
  结论与本轮实测交叉核对；外部资料见 `docs/RESEARCH_EXT_C3_FETCH_PATH_2026-11-26.md`。
- 上机：COM4，AI Passport（ESP32-C3 rev 1.1，8 MB flash，JEDEC 0x204017），
  只写 app 分区（`0x10000`），ROM / NVS / 存档分区未动。
- 新增测量工具 `tools/r5_probe.py`（发命令 + 采日志 + 打印 STAT），以及固件内的
  **帧间隔直方图**（见 §3 F1）。

---

## 2. 现场证据（改动前）

时钟界面，`J 1`，12 秒窗口：

```
frames=418 guest_fps=18.18 mi_s=7.32 pc=200343 guest_pct=93 blit_pct=0
gmax=78102 smax=336 tmax=2011 fd=0
aotb=6530177 aoti=15644571 aotfb=18973 aotmiss=4529
```

- `gmax` 51–78 ms，`fd=0`（帧预算 200 ms 从未触发），`blit_pct` 0–1 %。
- 帧间隔直方图：**全部 32 ms 以上**，`ftmx` 79–113 ms。
- park 场景（`pc=005376`）：57 fps、`gmax` 9.5 ms、`guest_pct` 51。

**推论**：卡顿不是丢帧策略、不是 blit、不是 pacer；是「guest 一帧要 51 ms」。
`guest_pct 93` 说明 CPU 里 93 % 是 guest 执行。

---

## 3. 改动清单

### F1 帧间隔直方图（测量能力）

`app_main.cpp`：每帧循环入口记录与上一帧的间隔，1 ms 一桶到 31 ms，末桶 ≥32 ms；
STAT 增 `ft=p50/p95/p99/p100 ftmx= ov20= ov33= ov50= ov100=`。
这不是优化，但它把「感觉卡」变成可比较的数字——本轮所有结论都由它判读。

### F2 恢复 `C3_WAIT_COLLAPSE=1`（本轮最大单项）

- `main/CMakeLists.txt`：`-DC3_WAIT_COLLAPSE=1`。
- `main/emu/unsp_glue.cpp`：折叠路径补 `m_insn_count += 74ull * done`，让 `mi_s`
  与 AOT 路径可比（68 条 PRNG + 6 条外壳）；否则 `mi_s` 会从 7.5 掉到 0.1，被误读成回归。
- 语义依据：周期计费 `CYCLES_PER_PASS=298` 与 AOT 路径逐块相加（9 + 263 + 10 + 16）
  相等；`C3_PORT.md:339` 记录过折叠开启后 `--save-flash` 逐字节相同、周期计数完全相同。
- 实测（F 180 时）：`collapse=94965/10 s`，`guest_fps 18.5 → 56.9`，`gmax 51 → 7 ms`，
  `guest_pct 93 → 31`。

### F3 帧预算从固定 1/60 改成自适应（目标 14 ms）

- 默认 `s_frame_cycles = kCpuClock/180`。
- 新增 `frame_budget_track()`：按 8 帧滑动平均，<12.5 ms 时 +6 %，>15.5 ms 时 −6 %，
  钳在 `[kCpuClock/240, kCpuClock/60]`。`F <hz>` 关闭自动（`auto=0`）。
- 理由：折叠让时钟界面每 guest cycle 只花 2.1 host cycles（/180 只需 7 ms），
  而重绘场景没有折叠、/60 一帧要 20–40 ms。固定预算必然对其中一个场景错误。
  实测收敛到 `fc=1600000`（即 1/60、实时速度），帧间隔 16 ms。

### F4 STAT 默认关闭

`s_stat_period_s = 0`。台架 `J 5` / `J 1` 打开。依据见 §0.2 第 3 条与 §5。

### F5 空闲修复扫描摊平（显示侧唯一周期尖峰）

`blit_frame()`：原来每 120 个空闲帧在**一帧内**推 6 条全宽带（6 × 12,288 B =
11–15 ms 阻塞）；改成 6 帧各推 1 条。画面本来就是静止的（否则不会进这条分支），
语义不变，单帧峰值 ≤2.5 ms。

### F6 QIO 尝试（负面结论）

sdkconfig 原本是 DIO，而 `sdkconfig.defaults` 的注释在论证 QIO 的好处——配置与
注释自相矛盾。本轮按 QIO 全片构建并烧写 `0x0`：

```
mode:QIO, clock div:1
load:0x3fcd5820,len:0x16b4
ets_loader.c 78
rst:0x7 (TG0WDT_SYS_RST)
```

ROM 能按 QIO 读 bootloader 头，随后 WDT 复位循环，**不能启动**。flash 型号
JEDEC `0x204017`（8 MB）在 DIO 下正常。最可能的原因是 QE 位未置位（esptool 不改它），
但本轮没有继续验证。已回滚 DIO，并把结论写进 `sdkconfig.defaults` 注释。

### F7 版本与工具

- `version.txt` 1.12.0 → **1.13.0**。
- 新增 `tools/r5_probe.py`（`R5_BOOT_WAIT` 控制「打开端口复位后等多久再发命令」）。
- 文档：本报告、`docs/RESEARCH_EXT_C3_FETCH_PATH_2026-11-26.md`（外部资料）。

---

## 4. 上机验证记录（v1.13.0）

全部同一台设备（COM4），只写 `0x10000`。

### 4.1 时钟界面（`pc=200343`，`J 1`）

| 配置 | `guest_fps` | `gmax` | `mi_s` | `guest_pct` | `ft` p50/p95 |
|---|---|---|---|---|---|
| v1.12.0，F 60 | 18.48 | 51.0 ms | 7.43 | 93 | ≥32/≥32 |
| v1.12.0，F 180 | 51.4 | 19.0 ms | 6.90 | 92 | 17/19 |
| v1.13.0，F 180 + collapse | 56.9 | 7.0 ms | 7.55 | 31 | 16/16 |
| **v1.13.0，自适应（fc=1.6 M）** | **56.4** | **14.0 ms** | **22.40** | **71** | **16/16** |

注：`J 1` 时 STAT 每 1 秒造成一次 79–81 ms 的长帧（`ftmx`），因此 `guest_fps` 比
`J 0` 时低。`ft` p50/p95 = 16/16 ms 说明绝大多数帧就是 60 Hz。

### 4.2 park（`pc=005251/005376`）

`guest_fps 56.5–56.6`、`gmax 10.3 ms`、`mi_s 0.60`、`guest_pct 52–54`。
与 v1.12.0 的 57 fps 持平——park 本来就被 pacer 限制，改进体现在 `gmax` 不再抖动。

### 4.3 存档 / 时钟

- `snapshot seq=2038 -> record 3 (280 + 8192 B)`、`snapshot pc=005255 frame skipped
  (power-on replay)`：帧块跳过逻辑正常（R4-F2）。
- 没有 `CLK` 纠偏行，说明 `clock_sync()` 认为时钟在死区内——折叠没有让客人时钟漂移。
- 没有 panic / `STUCK` / `E (`。

### 4.4 构建

app `1,023,456 B`（`228EB119…`）、full `4,677,888 B`（`5750E6E9…`）。
DRAM 余量与 R2 同级（未新增静态缓冲）。

---

## 5. 子 agent 结论摘要（与实测交叉核对）

### 5.1 AOT 热路径（已采纳 F2；其余为下一轮清单）

- 一次 dispatch ≈430–560 host cycles，其中固定开销 180–200（dispatcher 循环 59 条 +
  `aot_step` 61 条 + 查表），块体 250–350（块均 2.6 条指令 → 134 周期/条，只能是 flash 取指）。
- 时钟场景一圈：4 次 dispatcher 迭代、1478 host cycles，其中 1200 在调度三个外壳块。
- 可继续做：`0x230323` 加进 IRAM 热表（当前在 flash，178 B）；HLE 检查合并（−8 %）；
  `aot_step` `always_inline` + 去掉 `cnt` 统计（−13 %）；尾调/回边超块（−30 %）。
- 两个地雷：`AotEnt.cyc` 是死字段且数值错误（`0x2002F4` 记 0、实际 263）；
  `aot_b2002F4` 是死代码（`fast_hot` 永远先拦）。任何「预扣周期 / 尾调进 PRNG 块」
  的改动必须先修 `cyc`。
- `cycles_to_next_event()` / `tick()` 没有线性扫描，**不需要优化**（≈34 k cycles/帧 = 0.4 %）。

### 5.2 显示管线（已采纳 F5）

- 显示侧不是 18.8 fps 的来源：`blit_pct` 0–1 %，优化到 0 也只 +0.1 fps。
- 唯一周期尖峰是 120 帧全窗重绘（11–15 ms），已摊平。
- 双缓冲**收益为 0**：`esp_lcd` 的带命令调用会 drain 在飞事务，只要还按 band 发
  CASET，下一带必然等上一带。真异步要「整窗一次设窗 + `tx_color(-1)` 续传」，
  押 73 KB DRAM 与一条未验证的硬件行为（RAMWR 跨 CS 续传），本轮不做。
- 80 MHz 是真的（用菜单 `render 79 ms / total 100 ms` 反推）；`display.c:27` 的
  「40 MHz, not 80」是过时注释，**待清理**。
- `max_transfer_sz = 320*80*2 = 51,200` 超过 C3 单笔 DMA 上限 32 KB，白占描述符池；
  建议改 `320*24*2 = 15,360`。

### 5.3 宿主每帧开销（已采纳 F4）

- `gmax` 的采样点在 `clock_sync()` 之后、blit/快照/STAT 之前，**结构性地看不见
  后面那些同步阻塞**。修法：用新的帧间隔直方图（F1），它覆盖整个循环。
- 每 5 秒 ≈72 ms：UART0 无驱动、逐字节忙等 FIFO（810 B × 86.8 µs），再串行叠
  USB-Serial-JTAG ≈13 ms。**这是「规律性顿一下」的来源，已靠默认 `J 0` 消除。**
- 每 60 秒 ≈140–250 ms：`save` 分区擦 3 扇区 + 32 KiB 帧块（8 扇区）+ 33 KB 编程；
  擦除期间 ICache 关闭、AOT 块停摆。建议下一轮改成「只在暗屏/菜单窗口写」。
- 任务优先级不是瓶颈（main=1 / audio=3 / btn=6）；按键 ADC 每 5 ms 忙等 150 µs。

### 5.4 外部资料（`docs/RESEARCH_EXT_C3_FETCH_PATH_2026-11-26.md`）

- C3 的 ICache 硬连 16 KB / 8 路 / 32 B 行，**没有任何 Kconfig 可调**。
- miss penalty 官方没给；按带宽推算 DIO ≈1.9 µs/行、QIO ≈1.0 µs/行。
- 全屏 320×240×16bpp @80 MHz 单线 = 15.36 ms，占 60 Hz 预算 92 %——「全屏刷 + 60 fps」
  在算术上不成立。
- 同类项目（Anemoia NES）的 60 fps 不可比：NES 1.79 MHz / 240 MHz 双核是 134:1，
  本项目 96 MHz / 160 MHz 单核是 0.6:1。

---

## 6. 看见了但这一轮没动

| # | 项 | 预期 | 为什么没做 |
|---|---|---|---|
| 1 | 快照挑暗屏/菜单窗口 | 消掉 140–250 ms/60 s | 需要改存档时序策略，单独验证 |
| 2 | 菜单卡渲染 77 ms + 推送 22 ms | 一次性手感 | 分帧渲染要状态机；先记录 |
| 3 | `0x230323` 进 IRAM + 热页 AotEnt 进 DRAM | −10~15 %（待证） | 要重跑 block census + 重新生成 |
| 4 | HLE 检查合并 / `aot_step` 内联 | −20 % | 需要 A/B + 帧 CRC 比对 |
| 5 | 尾调 / 回边超块 | −30 % | 生成器改动，1 天 |
| 6 | QIO（QE 位） | 取指 −45 % | 本轮启动失败，需要 QE 写入实验 |
| 7 | WiFi/coex 关闭 | 未知 | `CONFIG_ESP_WIFI_ENABLED=y` 但代码从不初始化，未实测 |
| 8 | 整窗异步 blit | 全窗帧 −10 ms | 押 73 KB + 未验证的 RAMWR 续传 |

---

## 7. 下一轮的入口

1. **重绘场景**（用户真正看得见的活动画面）：现在时钟界面已经是 56 fps / 14 ms，
   但换屏/动画没有折叠加持。请求：`J 1` + 游戏内切页，量 `gmax` / `ft` / `rows`。
2. **快照避让**：`maybe_snapshot_machine_state()` 加 `quiet` 判据（暗屏 / 菜单 /
   30 s 超期），把 140–250 ms 挪到没人看的时候。
3. **AOT 微优化三连**（每个单独 A/B）：`0x230323` IRAM、HLE 合并、`aot_step` 内联。
   验收看 `gmax` 与 `mi_s`，不看平均 fps。
4. **文档债**：`display.c:27` 的 40 MHz 注释、`app_main.cpp` 的 `kGuardRows` 死常量、
   `max_transfer_sz` 超 32 KB。

---

## 8. 回滚

- 固件：`git checkout v1.12.0 -- firmware/digicore-c3`，重编，只烧 `0x10000`。
- 本轮 DIO 全片镜像备份：`tmp/digicore-c3-full-dio.bin`（4,677,888 B）。
- ROM 与存档分区不在任何回滚路径上。
