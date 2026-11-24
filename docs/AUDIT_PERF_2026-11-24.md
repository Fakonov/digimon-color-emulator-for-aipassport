# 第一轮代码审计与优化：流畅度 / 电池 / 冗余

日期：2026-11-24
基线：v1.6.1（`11dbdc6`，`firmware/digicore-c3`）
性质：**源码审计 + 低风险改动 + 编译验证 + host 单测。没有烧机，没有升版本，没有 commit。**
设备端数字全部标〔待真机〕，本文只给机制和估算，不给承诺。

---

## 0. 结论先说

### 0.1 三个数字

| 项 | 改前 | 改后 | 差 |
|---|---|---|---|
| DRAM 占用 | 320,158 / 321,296 | **214,074 / 321,296** | **−106,084** |
| DRAM 余量 | **1,138 B**（99.65%） | **107,222 B**（66.63%） | +106,084 |
| Flash code | 780,358 | 777,422 | −2,936 |
| app 镜像 | 1,205,680 | 1,192,848 | −12,832 |

余量从 1.1 KB 涨到 105 KB 不是为了好看：它把"没有 RAM 做热区 IRAM / 预解码 token"这个前提直接推翻了。

### 0.2 一条真 bug（本轮最大单项收益）

`SparseRom` 的 overlay 区间判断**恒为真**：

```cpp
// romstore.h，改前
if (page - m_ov_lo <= m_ov_hi - m_ov_lo)   // m_ov_lo = 1, m_ov_hi = 0
```

无符号减法把"空区间"算成 `0xFFFFFFFF`，于是对**任何** page 都成立。两个后果：

1. 每一次 flash 读（`rflash` 68 万次/秒量级）都先跨 TU 调用 `read16_overlay()`，进去扫 4 个槽、再判断 `m_base`，然后才读。
2. `page_window()` 在同一条件下返回 `nullptr`，所以 **`fetch16()` 的 4 KiB 取指窗口从来没建立过**：解释器每条指令都要 `fetch_window()`（含一次 `page_window` 调用）+ 一次完整的 `read16()` 分派。

host 侧实测（`tmp/test_mmap.cpp`）：修复前 `page_window(0x230000) == nullptr`，修复后指向 `image + 0x230000`。

这一条同时打在 AOT 的数据读和解释器的取指上，估算 **5–12% 端到端吞吐**〔待真机〕，代价是两行比较。

### 0.3 三条被删掉的"永久税"

| 位置 | 干了什么 | 何时成为税 |
|---|---|---|
| `execute_run()` | 每 dispatch 判 lpc 区间、累 ISR 直方图、写 48 项 PC ring（含 `% 48`） | 2026-11-23 用这些计数结案后就没再关 |
| `aot_step()` | 每块 6 次比较 + 2 次 DRAM 读改写（分区指令统计） | 一次性的"钱花在哪"普查，结论已进 PERF_2026-11-22 |
| `write16()` | 每次 guest 写 RAM 都比一次 `addr == 0x0850` | 64 Hz tick 排查的残留 |
| `ioa_data_r()` / `ioa_buffer_r()` | 每次读 PORTA 先算 `++n % 5000`（riscv32imc **没有硬件除法**，每次一次 `__umodsi3`，约 40 周期），命中才 printf | 2026-11-23 证明"ROM 一直在读端口"之后没关 |

`aot_step` 那条尤其亏：块平均 6.6 条 guest 指令，等于**每条 guest 指令摊一次比较**。

### 0.4 电池相关的三处

1. **pacer 空转 5.5 ms/帧**。轻负载 58 fps 时，一帧 16.7 ms 里有三分之一是 `while (esp_timer_get_time() < deadline) {}`，CPU 在 160 MHz 烧电。改成 1.5 ms 窗口，其余交给 `vTaskDelay`（空闲任务进 WFI）。重负载帧没有这段等待，所以**只对"静止画面"省电**——而那正是待机时长的主体。
2. **每秒 900 字节的 STAT 行**。115200 波特下约 78 ms/秒，7–8% 的墙钟。改成默认 5 秒（`J <sec>` 可调，`J 0` 全关）。
3. 删掉上面三条热路径诊断，本身就是等比例的 CPU 时间。

背光默认 80%（`menu.cpp: kBrightIdx = 3`）没动——这是产品决策，不是代码问题，见 §5。

### 0.5 上机实测（v1.7.0，同一台设备、同一个空闲场景）

v1.6.1 → v1.7.0，都在「16 秒无按键 → ROM 写 System_Sleep → park 到内部 ROM」的同一状态，
`pc` 都落在 `0x5376` / `0x5251`，`sp=0ff4`，所以这是同场景对比，不是两个场景各取一个好看的数字：

| 指标 | v1.6.1 | v1.7.0 | 变化 |
|---|---|---|---|
| `guest_fps` | 50.5–52.6（8 个样本，均值 52.1） | **57.50 / 57.57** | **+9.5%** |
| `mi_s`（M guest instr/s） | 0.53–0.56（均值 0.55） | **0.61** | **+10.9%** |
| `guest_pct` | 90–92 | 96 | 墙钟里被 guest 吃掉的比例 |
| `[ioa]` / `[tick]` 刷屏 | 每秒数行 | **0 行** | — |

落在此前估算的 5–15% 区间。游戏画面场景（`pc=0x200343`，PRNG 块）新固件是
`guest_fps 16.5 / mi_s 6.34`，旧文档基线是 5.7 M/s〔v1.2.x，非严格 A/B，仅供参考〕。

日志：`tmp/before_v16.log`（旧）、`tmp/after_boot.log` / `tmp/after_idle.log`（新）。

---

## 1. 审计方法

- 读源码：`app_main.cpp`（2,820 行）、`emu/gpce4.cpp`、`emu/unsp_glue.cpp`、`emu/romstore.*`、`rom_esp.cpp`、`menu.cpp`、`bezel.cpp`、`audio.c`、`display.c`、`CMakeLists.txt`、`sdkconfig`。
- 读产物：`build.log` 的 size 表、`digicore-c3.map`、`riscv32-esp-elf-nm --size-sort`（找最大的 RAM 符号）、`objdump -d`（确认删掉的东西真的不在代码里）。
- 读历史判断：`AUDIT_2026-11-23.md`、`RESEARCH_PERF_ROUTES_2026-11-24.md`、`PERF_2026-11-22.md`。**不重测已经结案的硬件事实。**
- 改完的验证：IDF 构建 + 两个 host 单测 + 反汇编核对。

---

## 2. 发现清单（按"收益/成本"排序）

### F1 `SparseRom` overlay 区间恒真 —— bug

- 位置：`main/emu/romstore.h` `read16()` / `page_window()`。
- 证据：host 测试 + 代码推导（见 §0.2）。
- 修复：显式双向比较，空区间用 `m_ov_lo = 0xFFFFFFFF, m_ov_hi = 0`（lo > hi）。
- 影响面：所有 flash 读（AOT 数据读、解释器取指、SPI1 命令通路）。
- 预期：每次 flash 读省 15–25 周期；解释器取指从 ~60 周期降到 ~4。

### F2 dispatch 循环里的中断诊断 —— 永久税

- 位置：`unsp_glue.cpp` `execute_run()` 热循环开头。
- 原代价：每次 dispatch 至少 2 次范围比较 + 1 次比较 + 可能的 1 次模运算和 2 次内存写。
- 依据：`AUDIT_2026-11-23.md` §2.1 已用 `isr/rise/fall/set2/clr2` 证明中断链正常，问题结案。
- 处理：`#if C3_IRQ_DIAG`，默认 0。控制台 `S`/`O` 读到的同名全局仍在，只是不再增长。

### F3 AOT 每块的分区统计 —— 永久税（更贵）

- 位置：`unsp_glue.cpp` `aot_step()` 尾部。
- 原代价：6 次地址比较 + `g_ins_*` 的 2–3 次 DRAM 读改写，**每块**一次。
- 处理：`#if C3_REGION_STAT`，默认 0。
- 代价：日志里 `riram/rflash/rram/rpush/rdraw/rband` 变成恒 0。**这不是故障**，需要看时用一行编译开关重新打开（见 §6）。

### F4 每秒的巨型 STAT 行

- 位置：`app_main.cpp` 主循环尾部，一个 100+ 字段的 `ESP_LOGI`。
- 原代价：约 900 B/秒 × 115200 波特 ≈ 78 ms/秒。
- 处理：`s_stat_period_s` 默认 5，控制台 `J <sec>`（0–60）可改，`J 0` 关闭。
- 附带：面板左上角的 fps 数字**没有**跟着降频，它走自己独立的 1 Hz 窗口（一次 24×12 的 SPI 突发，约 0.06 ms）。

### F5 pacer 的 5.5 ms 自旋

- 位置：`app_main.cpp` 帧尾。
- 原代价：轻负载时约占墙钟 33% 的 160 MHz 空转。
- 处理：自旋窗口 5.5 ms → 1.5 ms，其余 `vTaskDelay`。1 tick = 1 ms（`CONFIG_FREERTOS_HZ=1000`），欠睡风险由 1 ms tick + 0.5 ms 余量兜住。
- 保留原设计：绝对 deadline、落后即重同步。esp_timer one-shot 当年测过更慢，没改回来。

### F6 `window_has_content()` 每帧扫 9,216 像素

- 位置：`app_main.cpp` 主循环。
- 用途只有一个：`if (!s_boot_ok && content)` 打印 `BOOT_OK`。一旦为真永远不会再用。
- 处理：`s_boot_ok || window_has_content()`，之后每帧短路。

### F7 64 KiB 页缓存常驻（mmap 成功时从不读）

- 位置：`romstore.h` 的 `m_cache[16][2048]`。
- 事实：`rom_esp.cpp` 里 `esp_partition_mmap()` 成功后 `read16()` 走 `m_base`，`m_cache` 一次都不碰。
- 处理：ESP 平台改成堆指针；`mount()` 先试 mmap，成功就**不分配**，失败才 `alloc_cache()`（64 KiB），`set_mapped()` 立刻 `release_cache()`。
- host 单测仍用静态数组（`#ifdef ESP_PLATFORM` 隔离），`hosttest/romstore_test.cpp` 不用改。

### F8 miss census 8 KiB

- `C3_MISS_CENSUS=0` 时 `g_miss_pc`/`g_miss_cnt` 仍然定义在 .bss。
- 处理：条件编译，`aot_dump_miss()` 留空实现，`M` 命令照样应答（空表）。

### F9 两个开机微基准 24 KiB + 一次开机 CPU

- `bench_hotblock()`（`scratch[4096]`）+ `bench_exec()`（`unsp_20_device`、`rom[4096]`、`ram[4096]`、`BenchBus::data[4096]`）。
- 结论早在 `PERF_2026-11-22.md`，代码留在产品固件里，还占 DRAM。
- 处理：`#if C3_BENCH`，默认 0。

### F10 `panel_fill_black()` 的 10 KiB 私有缓冲

- 只在开机清屏一次。`s_strip` 有 15,360 B，本来就在那儿。
- 处理：复用 `s_strip`，`static_assert` 钉住尺寸。

### F11 控制台 `W` 的 4 KiB `probe`

- 一次性测试命令，静态持有 4 KiB 一辈子。
- 处理：复用 `s_strip`。

### F12 `tick()` 里的 printf

- `#ifdef ESP_PLATFORM` 下每 40 M guest cycles 打印一行，阻塞 UART。
- 处理：`#if defined(ESP_PLATFORM) && C3_TICK_TRACE`，默认 0。

### F13 `write16()` 的 `addr == 0x0850` 探针

- 每次 guest 写 RAM 一次比较，命中时还要读 PC/SR。
- 处理：`#if C3_TICK_DIAG`，默认 0。

### F14 PORTA 读探针里的软件除法（烧机前抓基线时才补上）

- 位置：`gpce4.cpp` `ioa_data_r()` / `ioa_buffer_r()`。
- 形状：每次读端口先 `++n % 5000u == 1`。riscv32imc 没有除法指令，`% 5000` 走 `__umodsi3`，数十个周期。
- 为什么比 F12 严重：tick 的 printf 每 40 M guest 周期一次，这一条是**每次按键读取**。v1.6.1 的实测串口日志（`tmp/before_v16.log`）里 `[ioa] reads=` 每秒好几行，读端口次数在每秒几十万量级。
- 处理：`#if C3_IOA_TRACE`，默认 0。`g_ioa_reads++` 保留（一次 DRAM 读改写；`O` 命令还靠它判断 ROM 读没读端口）。
- 记在这里的原因：审计阶段静态 grep 被 `#ifdef ESP_PLATFORM` 挡住没看见，是抓基线时被串口刷屏暴露的。

---

## 3. 改了哪些文件

| 文件 | 改动 |
|---|---|
| `main/emu/romstore.h` | overlay 区间判断修复（`read16` / `page_window`）；空区间 `0xFFFFFFFF/0`；ESP 分支 `m_cache` 改堆指针 + `alloc_cache()`/`release_cache()`；`read16` 空缓存防御 |
| `main/emu/romstore.cpp` | `clear()` 条件 memset；`read16_miss`/`read16_overlay` 空缓存返回 `0xffff`；`program_byte`/`erase_sector`/`overlay_import` 的缓存写加 `&& m_cache`；区间初始化 |
| `main/rom_esp.cpp` | `mount()` 改为「先 mmap，成功不分配页缓存；失败才 `alloc_cache()`」，失败则报错返回而不是带病运行 |
| `main/emu/unsp_glue.cpp` | 新增 `C3_IRQ_DIAG` / `C3_REGION_STAT`；`execute_run()` 中断诊断块、`aot_step()` 分区统计块条件编译；`g_miss_*` 8 KiB 条件编译 + `aot_dump_miss()` 空实现 |
| `main/emu/gpce4.cpp` | 新增 `C3_TICK_DIAG` / `C3_TICK_TRACE` / `C3_IOA_TRACE`；`write16()` 的 0x0850 探针、`tick()` 的周期 printf、`ioa_data_r()`/`ioa_buffer_r()` 的取模 printf 全部条件编译 |
| `main/app_main.cpp` | 新增 `C3_BENCH`；`bench_*` 条件编译；`panel_fill_black()` 与 `W` 的 probe 复用 `s_strip`；`window_has_content()` 短路；pacer 自旋 5.5 → 1.5 ms；`s_stat_period_s` + 控制台 `J <sec>`；面板 fps 数字独立 1 Hz |
| `tmp/test_mmap.cpp`、`tmp/stub/esp_heap_caps.h` | 新增：在 PC 上跑 ESP 分支的 mmap/cache 两条路径（见 §4） |
| `docs/AUDIT_PERF_2026-11-24.md` | 本文件 |

**没碰**：`tools/dmc_web.html`（用户自己的未提交改动）、`emu/`（PC 侧参考实现，按 `C3_PORT.md` 的约定不碰）、`sdkconfig`、`partitions.csv`、ROM/存档分区。

---

## 4. 验证记录

1. **构建**：`build.ps1 -NoSparse` 通过，无新增警告，size 表见 §0.1。
2. **`hosttest/romstore_test.cpp`（未改动的通用逻辑）**：全 8 MiB、4,194,304 词 ×2 遍 + `read8` + 擦除/编程 + overlay 抗驱逐 + 邻页不变 —— `ALL OK`。
3. **新增 `tmp/test_mmap.cpp`（ESP 分支）**：
   - mmap 路径（**页缓存从不分配**）：4,194,304 词逐字比对，`0 bad`，`page loads=0`；
   - 页缓存路径（`alloc_cache()`，模拟 mmap 失败）：乱序跨页访问强制驱逐，`0 bad`，`page loads=2048`；
   - `page_window(0x230000)` 指向 `image + 0x230000`（**修复前是 null**）；
   - overlay：擦除 + 8 字节编程后逐字节正确，邻页未受污染。
   
   复现：
   ```powershell
   cd firmware\digicore-c3
   & C:\Users\DuWeisi\w64devkit\bin\g++.exe -std=c++20 -O2 -DESP_PLATFORM -Itmp/stub `
       -o tmp\test_mmap.exe tmp\test_mmap.cpp main\emu\romstore.cpp
   .\tmp\test_mmap.exe ..\..\roms\dmc_v1_base.bin
   ```
4. **反汇编核对**：`objdump -d --disassemble=_ZN11unsp_device11execute_runEv` 里已经没有 `g_pc_ring` / `g_isr7_hits` / `g_isr_hist` 的引用——诊断确实被编译掉了，不是"还在但没触发"。
5. **上机**（v1.7.0，只写 app 分区 `0x10000`）：
   - `VER ver=1.7.0` 确认版本；启动、时钟播种、16 s 自动关背光都正常。
   - 同场景吞吐对比见 §0.5：`guest_fps` 52.1 → 57.5（+9.5%），`mi_s` 0.55 → 0.61（+10.9%）。
   - `[ioa]` / `[tick]` 刷屏消失，STAT 行 5 秒一条。
   - **分区完整性**：`roms` 头部 4 KiB 刷前刷后 SHA256 一致（`C244169F…`）；
     `save` 分区 128 KiB 刷前刷后差 10 个字节，全部落在 slot B 的页 24/25/28，
     内容是 DGSV 记录头序列号 `0x42/0x43 → 0x44/0x45` 加 2 字节游戏数据——
     **那是新固件启动后游戏自己存了两次档，不是刷写越界**：app 镜像写到 `0x1332C0` 就结束了，
     离 `roms` 的 `0x210000` 还有 0xDDD40 的空间；slot A 的前 64 KiB 逐字节未变。

---

## 5. 看见了但这一轮没动（按性价比排序）

| # | 项 | 预期 | 为什么没做 |
|---|---|---|---|
| 1 | `kBlitEvery` 2 → 1 | 面板更新频率翻倍（8.6–17 fps 的 guest → 面板同样频率；现在被砍半到 4–8 Hz） | 直接改观感，也直接加 SPI 阻塞时间。**要先 A/B**：`blit_pct` 现在 0–1%，多加一次预期 1–2%，但重绘帧的帧预算已经很紧。一行常量，值得单独烧一次机对比 |
| 2 | 192×192 的 `lcd.fb` 只用到 96×96 可见区（128 行 stride） | −14 KiB DRAM | 需要重构 `st7735` 的索引与 `panel_window()`；要先证明 guest 不读回 GRAM。现在余量够，不急 |
| 3 | `dmc_link` 的 `s_page` + `s_header` 各 4 KiB | −8 KiB | 上传协议专用，正常运行闲置；可复用 `s_strip`，但属于"改一个能跑的协议"，收益小 |
| 4 | **I2S/ES8311 开机常开** | 静止时省 MCLK/BCLK + codec 偏置（数量级 5–15 mA，需实测） | `i2s_channel_enable()` 在 `audio_init()` 里就开着，`auto_clear` 一直喂静音。关掉要验证 ES8311 在 MCLK 停后能否恢复锁定，**必须上机**，不能在 PC 上拍板 |
| 5 | 背光默认 80% | 每降 20% 通常省 5–15 mA | 产品决策，不是代码问题。`kBrightChoices` 里 20/40/60/80/100 都有，改 `s_bright_idx` 一行即可 |
| 6 | `CONFIG_PM_ENABLE` + 动态调频 | 空闲降频 | 与 `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG` 和 64 Hz 定时器语义冲突；主循环本来满载，收益集中在 §0.4 那段等待，而那段已经交给 WFI 了 |
| 7 | `CONFIG_LOG_DEFAULT_LEVEL` INFO → WARN | 省 flash 和启动噪声 | 会一并吃掉 `BOOT_OK`、`battery`、手势等诊断行；`J 0` 已经能关掉最贵的那一条 |
| 8 | 热区 IRAM（60–100 KB） | 10–30%〔估算〕 | 以前 DRAM 只剩 1.1 KB，做不了；**现在有 107 KB 了**，这是第二轮的正式入口。先按 `dbgtest/iram_hot.txt` 选块，量 `caot/cfn` |
| 9 | 区域化 AOT（多入口区域函数 + 寄存器钉住） | 1.4–1.8× | 4–8 周工作量，`RESEARCH_PERF_ROUTES_2026-11-24.md` §3.1 已论证；本轮只负责把前置条件（RAM）腾出来 |
| 10 | guest 16 s 空闲 park 点（`0x5230` 那条） | 修"按了没反应" | 行为问题，不是性能问题，独立处理 |

---

## 6. 怎么恢复被关掉的诊断

全部是编译开关，改 `main/CMakeLists.txt` 第 39–42 行的 `target_compile_options`：

| 想恢复 | 加什么 |
|---|---|
| 中断链计数（`S`/`O` 命令的 ISR 直方图、PC ring） | `-DC3_IRQ_DIAG=1` |
| 分区指令统计（日志里的 `riram/rflash/...`） | `-DC3_REGION_STAT=1` |
| 帧节拍写入探针（`O` 命令的 `tickw/tickpc`） | `-DC3_TICK_DIAG=1` |
| `tick()` 的周期 printf | `-DC3_TICK_TRACE=1` |
| PORTA 读探针（`[ioa] reads=…`，注意它每秒几十次 printf） | `-DC3_IOA_TRACE=1` |
| 两个开机微基准 | `-DC3_BENCH=1` |

改完记得**先碰 mtime 再 build**（`HANDOFF_2026-11-24.md` §4 第一条坑）。

运行时旋钮：
- `J 0` / `J 1` / `J 5`：STAT 行周期（0 = 关，1 = 旧行为）。
- `S` / `V` / `T` / `O` 等控制台命令完全没变，不依赖上面的开关。

---

## 7. 需要上机确认的清单

按顺序烧一次 app（`0x10000`，不动 bootloader / ROM / 存档），然后：

1. `VER` —— 几何没变：`win=0,18 96x96 scale=2 left=64 top=24`。
2. 等 10 s，看 STAT 行是否 **5 秒一条**（不是 1 秒），字段格式不变。
3. `J 1` → 回到 1 秒；`J 0` → 完全安静；`J 5` → 复原。
4. 看 `mi_s` 和 `guest_fps`。**这是本轮唯一能直接证伪的指标**：F1/F2/F3 全删之后 `mi_s` 应该上升（估算 5–15%）。如果没动，说明瓶颈在别处，第二轮直接走 IRAM/区域化 AOT。
5. 长按 A 开卡/关卡拉一遍，确认 `s_strip` 复用没有把砖墙或卡片画花（`panel_fill_black` 和 `W` 是开机/命令路径，理论上碰不到；但要眼看一遍）。
6. 静止画面放 10 分钟，看电池掉电速度（对比 1.6.1 的基线，需要两次测量才有结论）。
7. 菜单里 `SAVE IN`（`U`）拉一次存档，确认 `SparseRom` 的新堆路径下 overlay 导入仍然生效。

**如果出现异常**，回滚只要 `git checkout -- firmware/digicore-c3/main`，重编即可；ROM 和存档分区不在改动范围内。

---

## 8. 一句话给下一轮

瓶径没有变，只是现在终于有地方放代码了：**DRAM 余量 107 KB → 先把最热的 1–2 个页做成区域函数钉进 IRAM，再谈区域化 AOT 和 token 解释器。** 在动这两条之前，先把 §7 第 4 条（`mi_s` 有没有涨）测出来——它是判断本轮是否真的赚到了的唯一硬指标。
