# 第二轮代码审计与优化：流畅度 / 电池 / 冗余

日期：2026-11-24
基线：v1.7.0（`8e9fecb`，`firmware/digicore-c3`）
本轮版本：**v1.8.0**（`version.txt`，已烧机验证）
性质：**源码审计 + 改动 + 编译验证 + 上机实测**。设备端数字全部来自本机
（COM4，同一台 AI Passport，同一 park 场景）。

---

## 0. 结论先说

### 0.1 三个数字

同场景：16 秒无按键后 ROM park，`pc` 落在 `0x5251`/`0x5376`，`sp=0ff4`。

| 指标 | v1.7.0 | v1.8.0 | 变化 |
|---|---|---|---|
| `guest_fps` | 57.5 | **59.3** | +3.1% |
| `mi_s` | 0.61 | **0.63** | +3.3% |
| **`guest_pct`（CPU 被 guest 吃掉的墙钟占比）** | 96 | **62** | **−34 pt** |
| Flash code | 777,266（陈旧 .o，见 §4.4） | **598,078** | −23% |
| app 镜像 | 1,192,640 | **1,005,792** | −186,848 |
| DRAM | 214,066 | **206,032** | −8,034 |
| DRAM 余量 | 107,230 | **115,264** | +8,034 |

**`guest_pct` 96 → 62 是本轮最重要的一行**。fps 被 pacer 锁在 60，差 0.7 ms
看不出来；但 park 场景每帧空出来的 5.5 ms 会进 `vTaskDelay`（WFI），
那是 160 MHz 满载电流与静态电流的差别。

### 0.2 最大的单项：`aot_blocks.c` 的 `-O3` 是错的

`main/CMakeLists.txt` 第 53 行用 `-O3` 编 3.8 MB 的生成代码。实测换 `-Os`：

| | `-O3` | `-Os` |
|---|---|---|
| Flash code | 870,812 | **598,078**（−31%） |
| app 镜像 | 1,289,840 | **1,005,792** |
| DRAM | 217,404 | **206,032** |
| park `guest_pct` | 91 | **64** |
| park `guest_fps` | 59.28 | 59.33 |

代码小 31%，CPU 时间少 27 pt，fps 不掉。在 16 KiB 指令 cache 上，**块体大小
就是命中率**：`-O3` 多出来的那几条指令救不回一次 flash miss。

第一轮 CMakeLists 的注释已经写了这个判断（"if the miss penalty dominates,
smaller wins. Measured A/B"），但代码里是 `-O3`。本轮改回并实测。

### 0.3 一条被"探针"留下的死路径 —— 但恢复它反而更慢

`wait_loop_collapse()` 在 2026-11-23 被写成：

```cpp
if (true)
    return false;      // TEMP PROBE: ... Disable the collapse to test that theory.
```

那项调查最后在别处结案（`SparseRom` overlay 区间，第一轮 F1），但这段代码留在
热循环里：每个 dispatch 一次跨 TU 调用、永远返回 false。

本轮先把它恢复启用（`C3_WAIT_COLLAPSE=1`）并实测，**结果是负的**：

| 配置（均为 `-Os`） | `guest_pct` |
|---|---|
| collapse 关 | **64** |
| collapse 开 | 66 |

在 `-Os` 之后，`0x230321` 循环走 AOT 块已经不比 `hot_prng_n` 慢多少，而
collapse 每个切片要做一次状态搬运 + 记账，反而贵 2 个百分点。

**处理**：`C3_WAIT_COLLAPSE` 保留为编译开关，**默认 0**。那条 `if (true) return false;`
换成一个开关，热路径上零成本；要复现 2026-11-20 的 14.15 fps 结论时再打开。

结论记在这里：**这不是"恢复一个被误关的优化"，而是"确认一个优化已经过期"。**

### 0.4 电池相关的两处

1. **I2S/ES8311 开机常开 → 15 秒无 beep 关断**。这块板唯一的声源是按键 beep；
   除此之外 MCLK 12.288 MHz、codec 模拟轨、I2S DMA 空转从开机跑到关机。
   `audio_idle_poll()` 由帧循环调用，超时后先关模拟轨再停 MCLK（避免爆音），
   下次 beep 由 audio task 自己唤醒（约 2 ms I2C，不占主循环）。
   上机：`ACK K 1 6` → beep → `audio: codec + I2S off after 15 s without a beep`。
2. **park 场景 CPU 占用 96% → 62%**（§0.2 + §0.5）。

背光默认 80% 未动，那是产品决策。

### 0.5 另外两处热路径税

| 位置 | 每次都做什么 | 处理 |
|---|---|---|
| `aot_rd` / `aot_wr` | 每个 guest 访存一次 DRAM 读改写（`C3_AOTACC=1`） | 默认 0 |
| `spin_collapse` 调用点 | 每次 dispatch 一次跨 TU 调用，用来判断"是不是那三个地址" | 先内联比较三个 PC，命中才调用 |
| `spin_collapse` 探针 | 每次 dispatch 多次 DRAM 读改写 | `C3_SPIN_DIAG=0` |

前两条合计贡献了 64 → 62 的那两个百分点。

---

## 1. 审计方法

- 读源码：`app_main.cpp`、`emu/unsp_glue.cpp`、`emu/aot.h`、`emu/gpce4.cpp`、
  `emu/st7735.cpp`、`audio.c`、`display.c`、`CMakeLists.txt`。
- 读历史：`AUDIT_PERF_2026-11-24.md`（第一轮）、`RESEARCH_PERF_ROUTES_2026-11-24.md`、
  `C3_PORT.md` §8.2（等待循环折叠的原始记录）、`HANDOFF_2026-11-24.md`。
- **不重测已结案的硬件事实**（96×96 可见区、面板只写不读、ROM 无音频、
  三键电阻梯、CW2017/ES8311 共用 I2C0）。
- 上机：`tmp/probe.py` 读串口；只写 app 分区（`0x10000`），未动 bootloader /
  分区表 / `roms` / `save`。
- 每一处改动都是**单独构建 + 单独烧机 + 单独量**，不叠加结论。

---

## 2. 发现清单（按收益/成本排序）

### F1 `aot_blocks.c` 用 `-O3` 编 —— 成本最高的一处配置

- 位置：`main/CMakeLists.txt:53`。
- 证据：`-Os` 实测 flash −272,734 B、`guest_pct` 91 → 64、fps 不掉。
- 处理：改 `-Os`。
- 影响面：1,466 个块 / 14,847 条原生长指令。

### F2 `C3_AOTACC=1` —— 每个 guest 访存一次 DRAM 读改写

- 位置：`main/CMakeLists.txt:42` → `aot.h` 的 `AOT_ACC_*`。
- 和第一轮删掉的 `C3_REGION_STAT` 是同一类东西，只是藏在 `aot_rd/aot_wr` 里。
- 处理：默认 0。STAT 行仍打印 `rdf/rds/wrf/wrs`（恒 0），日志格式不变。

### F3 `C3_PROF=1` —— 每个调度切片 4 次 `esp_cpu_get_cycle_count()`

- 位置：`run_guest_frame()`，每帧约 336 个切片。
- 处理：默认 0；`stage=` 字段保留（0/0/0）。

### F4 `spin_collapse` 的调用形态

- 原样：每个 dispatch 一次跨 TU 调用（`m_fastspin && spin_collapse(lpc)`），
  函数内部再做三次比较；97% 的 dispatch 是为了得到 `false`。
- 处理：调用点先内联比较 `0x005251 / 0x005376 / 0x005F35`，命中才进函数。
- 附带：把探针计数收进 `C3_SPIN_DIAG`（默认 0）。

### F5 `wait_loop_collapse()` 的 `if (true) return false`

- 处理：`C3_WAIT_COLLAPSE`，**默认 0**（实测开启更慢，见 §0.3）；
  函数体与调用点都进 `#if`，关掉时零成本。

### F6 `fast_step()` 与 `g_verify` 死分支

- `m_fastops` 默认 false（`C3_PORT.md` §8.2.1：RV32 上实测更慢），但函数体、
  `++g_fast_calls`（每次调用一次 RMW）和 `g_verify` 影子执行路径全部编译着。
- 处理：`C3_FASTOPS` / `C3_VERIFY`，均默认 0。

### F7 `blit_frame()` 每帧哈希 9,216 个像素

- 位置：`app_main.cpp`，36 个 16×16 tile × 256 像素的 FNV-1a，每 host 帧一次，
  **不管画面有没有动**。约 50k 主机周期/帧。
- 事实：guest 写 GRAM 只有两条路——`St7735::store_pixel()`（逐像素）和
  `fill_run()`（常量色整段）。两者都能在写入点精确标记。
- 处理：`St7735::m_dirty[8]`（128×128 的 8×8 tile 位图），`blit_frame()` 只读位图。
  窗口 tile `(ty,tx)` 对应 GRAM tile 行 `ty+1`/`ty+2`、列 `tx`（`kWinY=18` 的偏移）；
  推送成功后 `dirty_clear()`，传输失败保留位，下帧重试。
- 直接写 `fb[]` 的两处（`paint_pattern`、`fbsave_load`）显式 `dirty_all()`。

### F8 I2S/ES8311 开机常开

- 见 §0.4。`codec_power_on()` 重放完整寄存器序列而不是"部分恢复"：哪几个
  寄存器能挺过深睡取决于关断深度，猜错就是无声且无错误码。

### F9 `kBlitEvery` 编译期常量

- 改成 `s_blit_every` + 控制台 `k <n>`，默认 **1**（面板每帧跟随 guest）。
  以前面板刷新率恒为 guest 帧率的一半，58 fps 的场景面板只有 29 Hz。

### F10 三个实验残留文件

- `main/app_main.cpp.pregion`、`main/emu/unsp_glue.cpp.pregion`、
  `main/emu/aot.h.compact.bak`：区域化 AOT 实验的整份副本，不在 `SRCS` 里，
  不参与编译，只是源码目录噪音。已删除（git 历史保留）。

### F11 STAT 缺 collapse 诊断

- 原有 `collapse/collcalls` 只统计 `wait_loop_collapse`，`spin_collapse` 的成功
  次数从未可见，导致"collapse=0"被误读成"没有 collapse"。
- 处理：加 `colltot`（wait 累计调用）、`spin=calls/seen/big/short`、
  `spinv=value/bp/icount/ok`（后者在 `C3_SPIN_DIAG=0` 时读 0）。

---

## 3. 改了哪些文件

| 文件 | 改动 |
|---|---|
| `main/CMakeLists.txt` | `C3_AOTACC` 1→0、`C3_PROF` 1→0；`aot_blocks.c` `-O3`→`-Os` |
| `main/emu/unsp_glue.cpp` | `C3_WAIT_COLLAPSE`/`C3_FASTOPS`/`C3_VERIFY`/`C3_SPIN_DIAG` 开关；`wait_loop_collapse` 死代码收进开关；`t_inf` 条件编译；spin 调用点预比较；`aot_dump_blocks` 改 `ESP_LOG` |
| `main/emu/gpce4.h` | `St7735::m_dirty[8]` + `dirty_mark/dirty_all/dirty_clear` |
| `main/emu/st7735.cpp` | `store_pixel`/`fill_run`/`reset` 标脏 |
| `main/app_main.cpp` | `blit_frame` 读脏位代替哈希；`kBlitEvery` 运行时化 + `k` 命令；`audio_idle_poll()`；STAT 新字段；census dump 周期化 |
| `main/audio.c` / `audio.h` | codec 电源门控、`audio_idle_poll()`、`es8311_init` 日志只打一次 |
| `version.txt` | 1.7.0 → **1.8.0** |
| `main/app_main.cpp.pregion` 等 | 删除 |

**没碰**：`tools/dmc_web.html`（用户自己的未提交改动）、`emu/`（PC 侧参考实现）、
`roms`/`save` 分区、`sdkconfig`、`partitions.csv`、菜单/砖墙/电量绘制逻辑。

---

## 4. 上机验证记录

全部在同一台设备（COM4），只写 `0x10000`。

### 4.1 版本与几何

```
VER ver=1.8.0 win=0,18 96x96 scale=2 left=64 top=24 off=0,0 home=64,24 fb=128 chunk=16
```

### 4.2 park 场景

| 构建 | `guest_fps` | `mi_s` | `guest_pct` | `blit_pct` |
|---|---|---|---|---|
| v1.7.0 | 57.5 | 0.61 | 96 | 0–1 |
| 本轮 `-O3`，collapse 开 | 59.28 | 0.63 | 91 | 0–1 |
| 本轮 `-Os`，collapse 开 | 59.31 | 0.63 | 66 | 0–1 |
| 本轮 `-Os`，collapse 关 | 59.33 | 0.63 | 64 | 0–1 |
| **本轮 `-Os`，collapse 关，探针关** | **59.3** | **0.63** | **62** | 0–1 |

55 秒 soak：59.22–59.37，无 `STUCK`、无 panic、无 `E (`。

### 4.3 音频门控

```
I (91276)  DIGICORE: frames=4722 guest_fps=58.77 ...      ← 注入 K 1 6
ACK K 1 6
I (120278) audio: codec + I2S off after 15 s without a beep
```

按键 → beep → codec 上电；15 秒静音 → 关断。开机后第一次关断也在 `I (15851)`，
说明 boot 阶段没有一直占着 codec。

### 4.4 代码体积

| 配置 | Flash code | DRAM | app 镜像 |
|---|---|---|---|
| v1.7.0（记录值，陈旧 .o） | 777,266 | 214,066 | 1,192,640 |
| 本轮 `-O3` | 870,812 | 217,404 | 1,289,840 |
| 本轮 `-Os` | 598,078 | 206,032 | 1,005,792 |

v1.7.0 那一行的 777,266 来自第一轮的 `build.log`，当时 `aot_blocks.c` 的 `.o`
是更早构建留下的（那一轮只重编了 `gpce4.cpp`）。本轮 `aot.h` 变了，整个
3.8 MB 的生成文件重编，`-O3` 的真实体积才暴露出来：**870,812**。
所以 `-Os` 相对"v1.7.0 实际跑的代码"是 −31%，相对上一轮记下的数字是 −23%。

### 4.5 菜单与画面

- 长按 A：`long A (1216 ms) -> settings menu`，`menu: pushed 10 band(s), 320x240:
  render 79 ms, total 99 ms`。
- 按 C 关闭：`menu: close`，随后 `guest_pct=93 / blit_pct=3 / rows=2688` 的全屏
  重绘一次，然后回到 `guest_pct=65`、`blit_pct=0`。
- 说明脏位路径与"关闭菜单强制重推"都正常。

---

## 5. 看见了但这一轮没动

| # | 项 | 预期 | 为什么没做 |
|---|---|---|---|
| 1 | 热块进 IRAM | 每块省 2–4 周期 | census 已取到（§6），但 `aot_blocks.c` 是生成文件，手工加 `IRAM_ATTR` 会被下次生成覆盖；要先给 `tools/aot.py` 加热表。按 `-Os` 之后的每块成本算，收益在 1% 以内 |
| 2 | 区域化 AOT（R1） | 1.4–1.8× | 4–8 周工作量，独立立项 |
| 3 | `lcd.fb` 128→96 列 | −14 KiB | 余量 115 KiB 已够，重构索引的风险不值 |
| 4 | `dmc_link` 两个 4 KiB | −8 KiB | 只在传输时用，收益小 |
| 5 | 背光默认 80% → 60% | 5–15 mA | 产品决策 |
| 6 | `CONFIG_PM_ENABLE` + DFS | 空闲降频 | 与 USB-Serial-JTAG console、1 kHz tick 语义冲突；本轮把空闲转成 WFI 已经拿到主要收益 |
| 7 | 每 60 s 的帧缓冲快照 | 少量 flash 写电流 | RAM CRC 未变时已经跳过；真正要动的是"画面在动时每 60 s 写 24 KB"，属于存档策略 |
| 8 | park 点按键失效 | 行为问题 | `HANDOFF_2026-11-24.md` 待办第 5 条，独立处理 |

---

## 6. 本轮取到的 census 数据（下一轮的输入）

`C3_BLOCK_CENSUS=1` 构建（collapse 关、`-O3`），frame 1200 起每 900 帧 dump，park 场景：

```
BLKHEAD 299
BLK 200343 2407527
BLK 230323 2407527
BLK 230321 2407526
BLK 005255 151186
BLK 005251 151186
BLK 00537A 151177
BLK 005376 151177
BLK 005898 34996
BLK 005322 17001   （之后是一批 17001 次的 0x005xxx）
```

`-Os` 之后这份分布已经变了，下一轮要重新 census 一次再决定 IRAM 热表。

---

## 7. 需要上机确认的清单

1. `VER` —— 应显示 `ver=1.8.0`，几何字段与 §4.1 一致。
2. **目视画面**：静态画面的时钟/宠物动画、菜单开关、设置卡上下边带是否仍然
   正常重绘（这是本轮唯一改了显示路径的地方）。
3. 长按 A 开卡/关卡拉一遍，确认卡片与砖墙没有被 `s_strip` 复用影响。
4. 按键听一声 beep（codec 已经关断过），确认唤醒后仍有声音。
5. 进游戏画面（重绘帧）量 `guest_fps` / `blit_pct`，并在同一画面下用 `k 1` /
   `k 2` 对比，决定 `kBlitEvery` 的最终默认值。
6. 静止 10 分钟测电池掉电，与 v1.7.0 基线对比。
7. `SAVE IN`（`U`）拉一次存档，确认 overlay 路径未受影响。

**回滚**：`git checkout v1.7.0 -- firmware/digicore-c3`，重编，只烧 `0x10000`。
ROM 与存档分区不在任何回滚路径上。

---

## 8. 一句话给下一轮

`-Os` 这一条把 park 场景从"CPU 满载"拉到三分之二负载，也顺带说明了这个项目
最容易踩的坑：**生成代码的编译选项和热路径上的"临时探针"都要定期复核**——
前者藏 27 个百分点，后者藏两个。下一轮该在**重绘帧**上做同样的实测，那里才是
用户看得见的卡；热块 IRAM 要等 `tools/aot.py` 支持热表之后再来一次。
