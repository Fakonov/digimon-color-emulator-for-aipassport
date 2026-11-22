# 阶段 0 可行性审计报告

- 项目：digimon-color-passport
- 目标机：FoloToy AI Passport（ESP32-C3，160 MHz，单核 RV32IMC，400 KB SRAM，8 MB flash，无 PSRAM）
- 目标 ROM：Bandai Digital Monster Color，MAME romset `digicolr`
- 审计日期：2026-11-19
- 审计范围：只做可行性，不改固件代码
- 后续更新：2026-11-20 已补记 48 MHz 修正与静态重编译 C3 实测，见第 8、9 节；P0 原始结论保留不改。

---

## 0. 结论摘要

| 闸门 | 结论 | 关键数据 |
|---|---|---|
| A：MAME 能否跑出画面 | **有条件通过** | 能启动、能出 96×96 彩色画面、能响应三键；无输入约 17.0 s 进入未实现的 System_Sleep，25 s 左右疑似复位重启；长跑约 47.7 s 出现 LCD/SPI 花屏。NOT_WORKING 的根因是 sleep/wake 与音频未实现，不是 CPU/LCD 核心完全不工作。 |
| B：8 MB 镜像能否塞进本机 | **通过（只读容量）** | 8,388,608 B 中全零页 1,437/2,048；非零页 611 页，稀疏存储 2,502,656 B（2.39 MiB），roms 分区 6,225,920 B（5.94 MiB），余量 3,723,264 B（3.55 MiB）。不需要压缩即可放下。 |
| C：C3 能否实时模拟 48–96 MHz GPCE4 | **不通过** | 实测指令流 38.13 M guest instr/s；官方 unSP-2.0 周期重算 CPI=1.6508，折合 62.94 M guest cycles/s（96 MHz 的 65.6%）。C3 解释器估算 5.3–8.0 M instr/s，只有需求的 14%–21%；要实时，host 预算必须 ≤4.2 cycles/guest instr，软件解释器做不到。 |

**总判定：按“加载原机 ROM + 全硬件行为 + 实时”的原始目标，ESP32-C3 这条路不成立。**

不是“再优化一下”能补上的差距：即使把解释器压到 10 host cycles/guest instr，也只有 16.0 M instr/s，仍不到 38.13 M 的 42%。要实时，必须换硬件，或改用静态重编译/动态翻译，或接受 4–7 倍慢速。

---

## 1. 闸门 A：MAME 侧到底能不能跑出画面

### 1.1 运行环境与命令

- MAME：0.289（mame0289），实际可执行文件 `C:/Users/DuWeisi/mame/bin/mame.exe`
- romset：`C:/Users/DuWeisi/mame/roms/digicolr.zip`（231,778 B）
- 基准命令：

```powershell
cd C:/Users/DuWeisi/mame
./bin/mame.exe digicolr -seconds_to_run 30 -nothrottle -skip_gameinfo -aviwrite gateA_30s.avi -log
```

- 按键注入用 Lua（`tools/shortpress.lua`、`tools/keepalive.lua`），active-low：
  PORTA 的 P1 Button 1/2/3 对应 bit 0x08/0x10/0x20，`set_value(0)` 为按下，`set_value(1)` 为松开。

### 1.2 实测结果

| 时间 | 现象 | 证据 |
|---|---|---|
| 0.0–0.5 s | 黑屏初始化，SPI flash 读取、ST7789 初始化命令 | error.log 开头 |
| 0.5 s | 出现 576 色 Digimon COLOR 启动 logo | gateA_frames/f0030.png，帧 30 |
| 2.0 s | 注入 B1 短按 3 帧后，进入彩色时钟/设置界面（大号 0:00、SET、环形图标） | short_frames/f0300.png |
| 2.0–17.0 s | 界面持续动画，CPU 在 0x2002F8–0x200343 主循环 | dbgtest/g2s.tr |
| 17.0 s | 游戏主动发 LCD SLEEP IN（命令 0x10），屏幕转黑 | 帧 1022，gateA_30s.avi |
| 17.0 s 后 | CPU 在 0x527C 附近反复写未实现的 0x300C–0x300E、0x3036 | error.log |
| 2.0–47.7 s（keep-alive 测试） | 每 6 s 注入一次 B1，UI 持续，时钟从 1:00 走到 3:00 | gateA_keepalive.avi，帧 600/1200/1800/2400 |
| 17–22 s（wake 测试） | 屏幕保持黑；20 s 注入 B1 后未立即恢复 | gateA_wake_frames/f1200.png、f1300.png |
| 25–30 s（wake 测试） | 屏幕重新出现 logo，随后回到彩色 UI；是否属于复位重启尚无直接证据（推测：sleep 路径未实现导致复位） | gateA_wake_frames/f1500.png、f1800.png |
| 47.7 s | 出现竖条彩色噪声，LCD/SPI 链路失稳 | gateA_keepalive_f2863.png |

MAME 运行速度：104.92%–115.82%（`-nothrottle` 下的相对值，不是 C3 性能）。

### 1.3 卡点与日志

无输入时，日志末尾四类写各重复 18,413 次：

```text
[:maincpu] ':maincpu' (00527C): unmapped program memory write to 00300C = FFC7 & FFFF
[:maincpu] ':maincpu' (005282): unmapped program memory write to 00300D = FFFF & FFFF
[:maincpu] ':maincpu' (005288): unmapped program memory write to 00300E = FFFF & FFFF
[:maincpu] ':maincpu' (005312): unmapped program memory write to 003036 = 5555 & FFFF
[:maincpu] Exceeded pending input line event queue on device ':maincpu'!
```

对应 `ref/generalplus_gpce4_soc.cpp` 的内部映射：

- 0x300C–0x300E：只注释为 IOA/IOB/IOC_WakeUp_Mask，未映射。
- 0x3036：只注释为 System_Sleep，未映射。
- 0x3030 System_Clock、0x3033 Timebase_Clear、0x3034 Watchdog_Clear、0x3035 Wait_Ctrl 已映射，但 sleep 路径没有完整实现。

结论：**不是 CPU 核跑不动，也不是 LCD 不工作；是 SoC 的 sleep/wake 没有实现，导致游戏进入省电后出不来。** 25 s 重新出现 logo 的原因，现有证据只能支持“复位重启”这一推测，尚未采样复位标志，报告中按推测标注。

### 1.4 闸门 A 能提供什么、不能提供什么

能提供：

- 启动 logo、时钟/设置界面的逐帧参考。
- 三键输入、LCD 命令/数据、SPI flash 读取的真实行为。
- unSP-2.0 指令流的动态 trace（dbgtest/g2s.tr，381,264 条/10 ms）。
- 0–17 s 的稳定 golden 段；17–40 s 在每 6 s 注入输入的前提下可延长，但 47.7 s 后花屏。

不能提供：

- 完整的游戏主循环/战斗/养育流程参考。当前只验证到时钟/设置界面，**未验证进入实际游戏玩法**；这一项标“无法判定”。
- 音频参考。压电片在 MAME 里完全未实现。
- 可信的周期级 timing。MAME 的 unSP 周期模型缺项严重（见闸门 C），且 gtime 在调试器下会因未扣周期而偏快。

### 1.5 闸门 A 判定

**有条件通过。** MAME 可以作为“启动 + 菜单 + 指令流”的参考，但不能作为“完整一局游戏”的 golden reference。要继续用 MAME 做 P1 逐帧比对，需要先给 SoC 补 0x3036/0x300C–0x300E 的 sleep/wake，或者保持 17 s 内注入输入、只截取稳定窗口。

---

## 2. 闸门 B：8 MB SPI flash 镜像能不能塞进 AI Passport

### 2.1 镜像实测

roms/dmc_v1_base.bin：

- 大小：8,388,608 B（8.00 MiB）
- SHA1：ebae139f708d36151efcaf2db7c329c6614d7e68（与 MAME -listroms 一致）
- CRC32：23227806

字节分布：

| 项 | 字节数 | 占比 |
|---|---:|---:|
| 0x00 | 6,074,696 | 72.42% |
| 0xFF | 62,139 | 0.74% |
| 其它 | 2,251,773 | 26.84% |
| 整体 Shannon 熵 | — | 2.5353 bit/byte |

4 KiB 页（共 2,048 页）：

| 项 | 数值 |
|---|---:|
| 全零页 | 1,437 |
| 非零页 | 611（29.83%） |
| 页熵 min / median / mean / max | 0 / 0 / 1.022 / 6.80 |
| 页熵 p90 / p99 | 3.93 / 5.63 |
| 熵 ≥ 7.0 的页 | 0 |

压缩实测：

| 方式 | 大小 | 相对原镜像 | 备注 |
|---|---:|---:|---|
| zlib-9 整体 | 224,824 B | 2.68% | 需要 8 MB 连续 RAM 解压，C3 不可行 |
| lzma-6 整体 | 171,920 B | 2.05% | 同上，且解压更慢 |
| bz2-9 整体 | 218,164 B | 2.60% | 同上 |
| 4 KiB 页独立 zlib | 313,299 B | 3.73% | 可随机解压，但每页解压有开销 |
| 4 KiB 页稀疏直存 | 2,502,656 B | 29.83% | **推荐** |

### 2.2 分区与预算

上一项目 firmware/digimon-passport/partitions.csv：

```text
nvs,      data, nvs,     0x9000,   0x6000,
phy_init, data, phy,     0xf000,   0x1000,
factory,  app,  factory, 0x10000,  0x200000,
roms,     data, 0x40,    0x210000, 0x5f0000,
```

- app 分区：0x200000 = 2,097,152 B（2.00 MiB）
- roms 分区：0x5F0000 = 6,225,920 B（5.94 MiB）
- 当前 app 二进制：753,920 B（dist/digimon-passport2.1.5.bin），app 余量约 1.34 MiB

若把 8 MiB 原样放进去，需要压缩到 ≤ 6,225,920 / 8,388,608 = **74.22%**，即至少省 25.78%。但按页稀疏存储只需要 2,502,656 B，占 roms 分区的 40.2%，**不需要压缩**。

### 2.3 推荐的存储布局

```text
[4 KiB 头]
  magic "DMC1", version, image_size=0x800000,
  sha1[20], page_size=4096, page_count=2048,
  bitmap_offset, index_offset, data_offset
[256 B 页位图]        2048 bit，1=该页存在
[8 KiB 页索引]        2048 × uint32，存该页在分区内的偏移；0xFFFFFFFF=全零页
[611 × 4 KiB 数据页]  2,502,656 B
```

总计约 2.51 MiB，roms 分区余量 3.55 MiB。

随机访问设计：

- 64 KiB 页缓存（16 × 4 KiB），LRU。
- 代码页局部性极好：2 s 的 trace 只触达 6 个 4 KiB 页，其中 0x200 页占 91.69%，0x200+0x230 合计 99.79%。16 页缓存足够覆盖启动/菜单热代码。
- 数据页（图块、字模）可能更分散，但可以按页换入；最坏情况是每次访问都 miss，此时 4 KiB SPI 读取会成为瓶颈。需要 P1 在 PC 上先统计真实访问页集合，再决定缓存大小。
- 不建议在 C3 上做整镜像解压：8 MiB 解压结果无处放；按页 zlib 虽然只有 313 KB，但每页解压的 CPU 开销会直接吃掉解释器预算。

写路径（尚未验证）：以上只解决只读镜像。若原机固件在运行中写 SPI flash（存档/设置），不能直接改稀疏镜像；需要 dirty-page overlay：把写过的 4 KiB 页落到 NVS 或第二个小分区，启动时叠加。MAME 侧还没有统计写/擦命令频率，P1 要在 trace 里查 SPI 0x02/0x20/0xD8 等命令。若写频繁，dirty 页会吃掉 roms 分区余量，Gate B 的预算需要重算。

### 2.4 闸门 B 判定

**只读容量判定：通过。** 8 MB 镜像以 4 KiB 稀疏页存放只需 2.39 MiB，roms 分区放得下，且不需要压缩。真正的工程问题是随机访问的页缓存命中率，以及运行期写 flash 的 dirty-page overlay；这两项属于 P1/P2 的验证项，不改变容量结论。

---

## 3. 闸门 C：C3 能不能实时模拟 48–96 MHz GPCE4

### 3.1 官方 unSP-2.0 周期表

MAME 的周期模型不能直接用。ref/unsp/unsp_fxxx.cpp:30-35 的 ds=imm6（FE00）分支不扣 m_icount，unsp_exxx.cpp 的寄存器/内存 BITOP 也不扣周期，execute_muls_ss() 同样不扣。按 MAME 源码算出的 CPI=4.899 只是残差，不是原机值。

官方《unSP Programmer's Guide》V1.0（ref/unsp2.0.pdf，191 页）第 143–157 页给出周期：

| 指令组 | 寻址/形式 | 周期 |
|---|---|---:|
| LOAD / STORE | IM6 | 1 |
| LOAD / STORE | IM16 | 2 |
| LOAD / STORE | [BP+IM6] | 1（STORE 注 1–2） |
| LOAD / STORE | [A6] | 1 |
| LOAD / STORE | [A16] | 2 |
| LOAD / STORE | R | 1 |
| LOAD / STORE | [R] | 2（++/-- 前缀 3） |
| ADD/ADC/SUB/SBC/NEG/CMP/AND/OR/XOR/TEST | 同上 | 1 / 2 / 1 / 1 / 2 / 1 / 2–3 |
| MUL | R | 1 |
| DIVQ / DIVS / EXP | R | 1 |
| 移位/旋转（ASR/LSL/LSR/ROL/ROR 及其 ALU 组合） | R | 1 |
| 位操作（TSTB/SETB/CLRB/INVB） | R / 直接偏移 | 1 |
| PUSH | N 个寄存器 | N+1 |
| POP | N 个寄存器 | N+2 |
| CALL A22 / CALL MR | — | 3 / 4 |
| GOTO A22 / GOTO MR | — | 3 / 2 |
| RETF | — | 6 |
| RETI | — | 6（IRQ nest off）/ 7（on） |
| BREAK | — | 4 |
| 条件分支 | taken / not taken | 4 / 1 |
| JMP | — | 4 |
| MULS | N 级内积 | N+2（无冲突/FIR_MOV off）、2N+1（无冲突/on）、2N+2（冲突/off）、3N（冲突/on） |

关键差异：官方 ADD IM6 = 1 周期，MAME 记 2；官方 [A6] = 1，MAME 记 3–5；官方分支 not taken = 1，MAME 记 2。MAME 的 CPI 4.899 明显高估。

### 3.2 动态指令流实测

用 MAME 调试器在 2 s 处 trace 10 ms：

- trace 文件：dbgtest/g2s.tr，381,264 条指令，8,038,660 B
- 指令率：381,264 / 0.010 s = **38.1264 M guest instr/s**
- 热循环：0x2002F8–0x200343，每轮约 74 条，10 ms 内重复 5,138 次

按官方周期逐条重算（dbgtest/analyze_official.py）：

| 指令类 | 条数 | 占比 | 周期 | 平均 |
|---|---:|---:|---:|---:|
| 间接寻址 [R] | 118,301 | 31.03% | 236,602 | 2.00 |
| imm16 | 67,031 | 17.58% | 134,062 | 2.00 |
| ds=imm6（FE00） | 61,780 | 16.20% | 61,780 | 1.00 |
| [BP+IM6] | 41,111 | 10.78% | 41,111 | 1.00 |
| imm6 | 25,879 | 6.79% | 25,879 | 1.00 |
| 移位（op1=5） | 15,418 | 4.04% | 15,418 | 1.00 |
| 寄存器 | 10,288 | 2.70% | 10,288 | 1.00 |
| 旋转（op1=6） | 10,278 | 2.70% | 10,278 | 1.00 |
| 移位（op1=4, opn>=4） | 10,276 | 2.70% | 10,276 | 1.00 |
| POP | 5,217 | 1.37% | 31,367 | 6.01 |
| PUSH | 5,216 | 1.37% | 15,784 | 3.03 |
| CALL | 5,181 | 1.36% | 15,543 | 3.00 |
| 条件分支 taken | 5,153 | 1.35% | 20,612 | 4.00 |
| 其它 | 1,734 | 0.45% | 4,394 | — |
| **合计** | **381,264** | **100%** | **629,394** | **1.6508** |

折合：

- 629,394 guest cycles / 10 ms = **62.94 M guest cycles/s**
- 对 96 MHz：65.6% 占用
- 对 48 MHz：131%，物理上跑不完这段指令流

**这说明该 trace 窗口对应的是 96 MHz 档，而不是 48 MHz 档。** 也可能 MAME 的 gtime 因未扣周期而高估了指令率；但即使把 38.13 M 打五折到 19 M，C3 仍然不够。下面的估算用 38.13 M 作为上界，用 29.0 M（48 MHz / 1.6508）作为下界，两种都算。

### 3.3 C3 解释器吞吐估算

C3：单核 RV32IMC @160 MHz，无 PSRAM，SRAM 400 KB，指令 cache 很小。

解释器每条 guest 指令的 host 开销分解（估算）：

| 阶段 | host cycles |
|---|---:|
| 取指/取 opcode（含页缓存命中） | 4–6 |
| 解码 op0/opa/op1/opn/opb | 3–5 |
| 执行 ALU/load/store/分支 | 4–8 |
| 标志位、PC、icount 更新 | 3–5 |
| 分发循环/函数调用开销 | 3–6 |
| 合计 | **17–30** |

按 3.2 的动态 mix 加权，代表性指令类的 host 周期估算如下（RV32IMC，160 MHz，页缓存命中，不含中断和 SPI miss）：

| guest 指令类 | 动态占比 | host cycles/guest instr | 加权贡献 |
|---|---:|---:|---:|
| ALU 立即数 / 寄存器（imm6、ds=imm6、R） | 25.7% | 12–16 | 3.1–4.1 |
| ALU imm16 | 17.6% | 14–20 | 2.5–3.5 |
| load/store [BP+IM6] | 10.8% | 17–24 | 1.8–2.6 |
| load/store [R]（间接） | 31.0% | 19–28 | 5.9–8.7 |
| 移位/旋转（shift、rot、shift2） | 9.5% | 14–18 | 1.3–1.7 |
| PUSH / POP | 2.7% | 18–26 | 0.5–0.7 |
| CALL / 条件分支 | 2.7% | 15–30 | 0.4–0.8 |
| 其它 | 0.5% | 18–22 | 0.1 |
| **加权合计** | **100%** | **15.6–22.1（中值 18.9）** | — |

这不是 C3 实测，是静态估算；P1 在 PC 上用同一份解释器跑 g2s.tr，统计 retired host instructions 和 wall time，可以把这个区间收窄。即使按最乐观的 15.6 cycles/instr，C3 也只有 10.3 M instr/s，仍是 38.13 M 的 27%。

取 20–30 cycles/instr：

| host cycles/guest instr | 160 MHz 吞吐 | 占 38.13 M | 占 29.0 M |
|---:|---:|---:|---:|
| 15（极乐观） | 10.67 M/s | 28.0% | 36.8% |
| 20 | 8.00 M/s | 21.0% | 27.6% |
| 25 | 6.40 M/s | 16.8% | 22.1% |
| 30 | 5.33 M/s | 14.0% | 18.4% |
| 40 | 4.00 M/s | 10.5% | 13.8% |

再考虑页缓存 miss、SPI 读取、标志位分支、中断检查，实际再打 20% 折扣：

| host cycles/guest instr | 实际吞吐（-20%） | 占 38.13 M | 占 29.0 M |
|---:|---:|---:|---:|
| 20 | 6.40 M/s | 16.8% | 22.1% |
| 25 | 5.12 M/s | 13.4% | 17.7% |
| 30 | 4.27 M/s | 11.2% | 14.7% |

**实时所需的 host 预算：**

- 对 38.13 M：160 MHz / 38.13 M = **4.20 cycles/guest instr**
- 对 29.0 M：160 MHz / 29.0 M = **5.52 cycles/guest instr**

一个带标志位、内存映射、SPI flash 页缓存、中断检查的软件解释器，不可能做到 4–5.5 cycles/guest instr。仅“取 opcode + 查表 + 间接跳转”就要 6–10 cycles。

### 3.4 闸门 C 判定

**不通过。**

- 按动态 mix 分类加权：15.6–22.1 host cycles/guest instr，对应 **7.2–10.3 M instr/s**，只有需求的 **19%–27%**（对 38.13 M）。
- 保守取 20–30 cycles/instr：5.3–8.0 M instr/s，只有需求的 **14%–21%**（对 38.13 M）或 **18%–28%**（对 29.0 M）。
- 极乐观 15 cycles/instr：10.67 M/s，也只有 **28.0%**。
- 即使做到 10 cycles/instr：16.0 M/s，只有 **42%**，仍不能实时。
- 需要比常规解释器快 **4.8–7.2 倍**（不含缓存惩罚），或 **6–9 倍**（含缓存惩罚）。

**降频方案与计时影响：**

- 若让 C3 以 5–8 M instr/s 跑，guest 等效主频约 8–13 MHz（按 CPI 1.65），是 96 MHz 的 8%–14%。
- 游戏逻辑若以 60 Hz 帧中断计时，每帧需要约 635k 条指令；C3 每帧要 80–120 ms，实际 **8–12 fps**，慢 5–7 倍。
- 若计时用 RTC/墙钟（进化时间、喂食冷却），可以把 RTC 与 CPU 解耦，墙钟仍然准确；但动画、按键响应、战斗结算仍然慢 5–7 倍。
- 结论：可以做“慢速可玩/博物馆模式”，但不能叫“实时模拟”。

---

## 4. 路线选择

### 4.1 走不通的路

1. **C3 + 纯 C 解释器 + 实时**：不成立，差距 5–7 倍。
2. **C3 + 更努力优化解释器**：即使 10 cycles/instr 也只有 42%，且 10 cycles/instr 在带内存映射和标志位的 unSP 上不现实。
3. **C3 + 整镜像解压到 RAM**：8 MB 结果无处放，直接排除。

### 4.2 还剩的路

| 路线 | 实时性 | 工作量 | 风险 | 备注 |
|---|---|---|---|---|
| A. C3 慢速解释器 | 8–12 fps，慢 5–7 倍 | 中 | 低 | 保留原机 ROM 与全部硬件行为，只是慢；适合“能跑起来”的演示 |
| B. 换 ESP32-S3（240 MHz 双核 + PSRAM） | 可能 50–80%，仍不确定 | 中高 | 中 | 单核跑 CPU、单核跑 LCD/输入；PSRAM 可放整镜像或大缓存 |
| C. 换 Linux SBC（1 GHz+） | 可以实时 | 高 | 中 | 最稳，但硬件形态/供电/屏幕要重做 |
| D. 静态重编译 unSP→C/RV32 | 可以实时 | 很高 | 高 | 需要离线把 ROM 转成代码，和“加载原机 ROM”有冲突；调试困难 |
| E. C3 动态翻译（JIT） | 可能 30–50% | 很高 | 很高 | 400 KB SRAM 里放代码缓存；unSP 指令语义复杂，不推荐作为第一条路 |

### 4.3 我的建议

- 如果目标是**“在 AI Passport 上真模拟原机 ROM”**，选 **A**：接受慢速，先把 P1 解释器、P2 固件骨架、P3 图形链路做出来，验证“原机 ROM 能跑”。实时性另立目标。
- 如果目标是**“实时”**，不要在 C3 上继续投入，直接选 **B 或 C**；其中 C 的成功率最高，B 的形态最接近原机。
- **D/E 不建议现在做**：它们会把项目从“模拟器”变成“重编译器”，与第 1 节的目标定义冲突，且工作量是 A 的 3–5 倍。

---

## 5. 需要拍板的问题

1. 接受 C3 慢速（8–12 fps，慢 5–7 倍），继续 P1/P2/P3？
2. 换硬件（ESP32-S3 + PSRAM，或 Linux SBC），保持实时目标？
3. 停在 P0，先做静态重编译/混合方案的预研？

**在收到选择前，不写固件代码。**

---

## 6. 本轮产物与复现

### 6.1 关键产物

| 文件 | 用途 |
|---|---|
| docs/FEASIBILITY.md | 本报告 |
| dbgtest/g2s.tr | 2 s 处 10 ms unSP 动态 trace，381,264 条 |
| dbgtest/analyze_official.py | 按官方周期重算 CPI |
| dbgtest/pcpages.py | 统计 trace 的代码页局部性 |
| ref/unsp2.0.pdf | 官方 unSP-2.0 手册，191 页 |
| ref/unsp2.0.txt | 手册文本提取 |
| ref/p143_1.png … p157_1.png | 周期表页图 |
| gateA_keepalive.avi | 45 s 带按键注入的 MAME 录像 |
| gateA_keepalive_f2400.png | 40 s 时钟界面截图 |
| gateA_keepalive_f2863.png | 47.7 s 花屏截图 |

### 6.2 复现命令

```powershell
# 1) MAME 基线
cd C:/Users/DuWeisi/mame
./bin/mame.exe digicolr -seconds_to_run 30 -nothrottle -skip_gameinfo -aviwrite C:/Users/DuWeisi/digimon-color-passport/gateA_30s.avi -log

# 2) 按键注入 + 45 s 保持
./bin/mame.exe digicolr -seconds_to_run 45 -nothrottle -skip_gameinfo `
  -aviwrite C:/Users/DuWeisi/digimon-color-passport/gateA_keepalive.avi `
  -autoboot_script C:/Users/DuWeisi/digimon-color-passport/tools/keepalive.lua -log

# 3) 官方周期重算
C:/Users/DuWeisi/py311/python.exe C:/Users/DuWeisi/digimon-color-passport/dbgtest/analyze_official.py `
  C:/Users/DuWeisi/digimon-color-passport/dbgtest/g2s.tr

# 4) 稀疏页统计
C:/Users/DuWeisi/py311/python.exe C:/Users/DuWeisi/digimon-color-passport/tools/pages.py
```

### 6.3 本轮未做

- C3 真机 host cycle 跑分（需要烧一个 benchmark，不属于“不改固件”的范围）。
- MAME sleep/wake 补丁（只做了诊断）。
- 音频反推（压电片，MAME 无实现）。
- 完整 gameplay 验证（当前只到时钟/设置界面）。

---

## 7. 一句话

> 以下是 P0 当时的结论；用户随后选择先做静态重编译，见第 8、9 节。

**A 能出画面但会睡死/花屏，B 容量够，C 算下来差 5–7 倍；C3 实时模拟这条路到此为止，等你选慢速、换硬件，还是改路线。**

---

## 8. 2026-11-20 补记（正文未改，避免覆盖 P0 原始数据）

- 原机 GPCE4064B 的官方最高 CPU 时钟是 **48 MHz**，不是 MAME 驱动里的 96 MHz。依据：GeneralPlus 产品页、`ref/gpce4064a_ds.pdf` 第 28/32 页，以及 GMMan 的 GPCE4 测试笔记；没有拆机示波器实测，属于强证据推断。
- 因此第 3 节的 38.1264 M guest instr/s 与 CPI 1.6508 是 **MAME 96 MHz 下**的观测值；按官方周期表重算同一 trace，CPI 约 1.784–2.054，48 MHz 下真实目标约 **23.4–26.9 M instr/s**。MAME 的 38.13 M 是真实上限的 1.4–1.6 倍，不能把 MAME 的 10 秒当真实 10 秒。
- 静态重编译 PoC v0 已单独放在 `recomp/`，结论见 `recomp/README.md`：两段真实 guest 代码（0x2002F4 hash 函数、0x5376 延时循环）已生成 C，并与 MAME 快照做全 RAM memcmp + 寄存器比对，均 PASS；x86 上分别约 0.54 与 1.44–1.51 host cycles/guest instr，RISC-V 静态汇编 126 与 76 条。C3 真机 cycle 已测，见第 10 节。
- P1 若继续逐帧对齐，必须先把 MAME 时钟改成 48 MHz，或按 guest cycle 对齐，否则基准本身就是错的。

## 9. 2026-11-20 C3 实测补记（AI Passport wasm 模拟器）

- 目的：把 `recomp/` 的两段静态翻译块编进 ESP32-C3 测试固件，在 wasm 模拟器（`esp32c3`，160 MHz）上测 cycle，不再只用静态指令 mix 估算。
- 固件：`firmware/recomp-bench`；入口文档 `firmware/recomp-bench/README.md`；一键 `build.ps1 -Sim`。
- 结果（最终双控制台镜像复跑；早期 UART0 镜像连续 3 次同值）：
  - `HOT_PROFILE0 PASS`、`HOT_PROFILE1 PASS`、`DELAY PASS`、`SIM_RESULT PASS failures=0`
  - `HOT_BENCH n=200000 cycles=26085089 cycles_per_call=130 cycles_per_guest=1.91`
  - `DELAY_BENCH n=2000 cycles=92425201 cycles_per_call=46212 cycles_per_guest=5.77`
- 换算：`hot_func` 83.4 M guest instr/s，`delay_loop` 27.7 M；对照 48 MHz 目标 23.4–26.9 M，两块都进入区间。
- 边界：这是**块级**数据，不是整机实时；未接 internal ROM、SPI flash、LCD、IRQ。`delay_loop` 的 5.77 cycles/guest 主要是 C 代码膨胀（约 45 条 host 指令对应 8 条 guest 指令）。
- 结论：静态重编译在 C3 上块级可行；整机实时仍需更多块、内存映射和 cache 行为验证；真机块级复核已完成（第 10 节）。

## 10. 2026-11-20 C3 真机复核（COM4）

- 目的：把第 9 节的同一测试镜像烧进 AI Passport（ESP32-C3，160 MHz），用 `esp_cpu_get_cycle_count` 复核 wasm 模拟器数字；只测两个已翻译块，不接 LCD/SPI/IRQ。
- 注意：该测试固件不初始化 LCD/背光，屏幕保持黑是预期行为；验收看 COM4 串口 PASS，不是看屏幕。
- 端口：用户指定 **COM4**（COM6 是 Cardputer ADV）。设备枚举为 ESP32-C3 QFN32 rev v1.1、8 MB XMC flash、USB-Serial/JTAG，MAC `4c:11:ae:2f:3f:d4`。
- 控制台：UART0 primary + C3 原生 USB-Serial/JTAG secondary。这样 wasm 模拟器继续从 UART0 收日志，COM4 也能收；两边跑的是同一个镜像。
- 刷机：esptool 4.12.0，460800；`erase_flash` → `write_flash 0x0 recomp-bench-full.bin` → `verify_flash 0x0`，回读 **digest matched**。按用户指令没有保留整片备份。
- 镜像：`firmware/recomp-bench/recomp-bench-full.bin`，273,712 B（0x42D30），SHA256 `F04BDB757E44A85E94C6971B600C7B94709DBCC4B0F93FB9B97C968FE79BD12D`；app 208,176 B（0x32D30），SHA256 `B7983294E10D9304B37EB6C74783D4EBAEFE7FEAA9E06B1088F2583E36A964BF`。
- 真机结果（日志 `firmware/recomp-bench/device_run.log`）：
  - `HOT_PROFILE0 PASS`、`HOT_PROFILE1 PASS`、`DELAY PASS`、`SIM_RESULT PASS failures=0`
  - `HOT_BENCH n=200000 cycles=26970559 cycles_per_call=134 cycles_per_guest=1.98` → **80.68 M guest instr/s**（13.6 M guest insn / 168.566 ms）
  - `DELAY_BENCH n=2000 cycles=98745704 cycles_per_call=49372 cycles_per_guest=6.16` → **25.94 M guest instr/s**（16.006 M guest insn / 617.161 ms）
- 对照：48 MHz 原机目标 23.4–26.9 M；hot 为 3.0–3.4 倍，delay 为 96%–111%。两块都在目标区间内或以上。
- 复现：同一镜像在 wasm 模拟器仍 PASS（hot 130 cycles/call、1.91 cycles/guest、83.4 M；delay 46,212 cycles/call、5.77 cycles/guest、27.7 M），日志 `firmware/recomp-bench/sim_run.log`。
- 解释：真机与模拟器差 2%–7%，在 `esp_cpu_get_cycle_count` 和 C 代码布局波动范围内；这不改变“块级可行、整机未证明”的结论。真机数字只覆盖两个块，不含取指/解码/中断/SPI/LCD/cache 竞争。

## 11. 2026-11-20 显示链路 bring-up（firmware/color-preview）

- 目的：先把 ST7789P3 初始化、SPI2 DMA、背光、96×96→320×240 合成跑通，让真机看到原机参考画面；**不是** unSP 模拟器，不执行内部 ROM 或 `dmc_v1_base.bin`，没有 CPU/定时器/中断/SPI flash/LCD 控制器/按键语义。
- 产物：`firmware/color-preview/color-preview-full.bin` 717,216 B，SHA256 `CBCA3DF60D83AB08F27A152FCB06770CBBFBD811A39E4A63E37F0025549390CB`；app 651,680 B，SHA256 `87EFB2D9E5C59FBCA7F36ABA00D3DEA519B260AFCEFCB65D8625ABEB480E46E8`。
- 帧包：`main/frames.bin` 433,520 B，150 帧，10 fps，15.0 s，源 `short.avi`；RGB565 大端 + 逐帧 zlib。
- 方向：帧包保持 MAME raw 方向，固件运行时默认旋转 180°；上电 2.5 s 测试帧带红色上边条/绿色下边条，任意键循环 0/90/180/270，解决没有实机照片时的方向不确定性。
- 2026-11-20 已连接 COM4：整片 8 MB 备份（`docs/backup/digimon-color-passport-COM4-8MB-20261120-1218.bin`，SHA256 `C74EA4FF72A82F54326892D71F7A4081C55958A0D8B029190660742004CA48F9`）并 verify；`write_flash 0x0` 后 verify digest matched。
- 首刷在 `preview_main.c:68` 触发 `Stack protection fault`：`tinfl_decompress_mem_to_mem` 的调用栈超过 8 KB。把 `preview` 任务栈从 8,192 提到 32,768 后复刷，15 秒日志 `PREVIEW_ROT 180`、`PREVIEW_READY first_frame=0 rot=180`、`frame 0/150` 到 `frame 120/150`，无 panic（`firmware/color-preview/device_run_15s.log`，3,654 B）。
- 屏幕红条位置、`SET` 是否正立、颜色仍未肉眼验收；没有照片。
