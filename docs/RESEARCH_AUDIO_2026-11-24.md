# 彩屏元祖（Digital Monster Color）声音研究：为什么现在只有提示音，怎样才能出原机声音

> 研究日期：2026-11-24（第二轮已推翻第一轮结论，见 §0.1）
> 对象：`firmware/digicore-c3`（AI Passport，ESP32-C3）上的 GPCE4 模拟器 + `roms/dmc_v1_base.bin`（MAME `digicolr`）
> 性质：**只做研究与可行性分析**，本文件不含实现改动。
> 取证明细：`_research/audio_probe/FINDINGS_2026-11-24.md`（带 IO 钩子的实测，1–9 节）

---

## 0. 结论速览

1. **设备上唯一的"声音"是固件自己合成的方波**：`firmware/digicore-c3/main/audio.c` 的 `audio_beep()`，按键 1760 Hz/18 ms（`app_main.cpp:1686`）、开机 880 Hz + 1320 Hz 双音（`app_main.cpp:2317-2319`）。它和 ROM 没有任何关系。
2. **"没有原游戏声音"是两层同时断**：
   - **管道层**：C3 端模拟器根本没有音频接收端。`firmware/digicore-c3/main/emu/gpce4.h:340` 写着 `m_dac_cha1 = 0; // 0x3041, last sample (no audio sink)`，`:448` 写着 `// DAC / PPAM (no audio sink in this port)`；`pwm0_ctrl_w()` 是**空函数**（`emu/gpce4.cpp:911`，只加计数）。
   - **模型层**：ROM 的发声通路是 **TimerB 定音高 + PWM0 + IOA0 硬件方波**，这条通路在 host 和 C3 两版模拟器里都**完全没有建模**。
3. **原机确实有声音**：设置菜单有 `Sound`，三档「無音 / 小 / 大」；A+C 主画面静音；数码兽**饿、筋力归零、到就寝时间**会"call out with a few beeps"；关机前有提示音、死亡时有"singing its dying song"。发声器件是**压电片（piezo disc）**，不是动圈喇叭（MAME driver 注记 `Sound unit is a piezo disc.`，`ref/generalplus_gpce4.cpp:458`；拆机视频可见圆形压电片）。
4. **关键结论（第二轮新增，已实锤）**：这份 ROM **确实在发声**，但**不走 14-bit DAC**，走的是 mask ROM 的硬件 PWM 蜂鸣服务 `0x55A9`：
   - `TimerB_Data/Counter(0x3012/0x3013)` ← 音高周期
   - `Timer_Ctrl(0x3016) |= 0x00F0` ← 开 TimerB
   - `IOA_Direction/Attribute(0x3002/0x3003) |= 0x0201/0x0101` ← 把 `IOA0` 设成 IO_PWM 输出
   - `IOA_Buffer(0x3001) &= 0xFDFE` ← IOA0 拉低
   - `IO_Ctrl(0x300F) |= 0x0008`、`PWM0_Ctrl(0x3020) = 0x02FF` ← 开 PWM
   - 配对关声 `0x561F`：`IO_Ctrl &= 0xFFF7`、`IOA_Buffer &= 0xFCFE`、`PWM0 = 0`
   - flash 里唯一调用点：**`0x20A404`**（`F040 55A9`）
5. **所以"声音没出来"的真正原因**：ROM 在 `0x20A404` 发起声，我们的模拟器把 `0x3020` 当不存在——**没有任何模块把这条 PWM 通路转成 PCM**。DAC 那条路是死的，但那本来就不是它的发声方式。
6. **可行性大幅好转**：要模拟的不是 48 kHz DAC 采样流，而是**方波频率合成**（TimerB 周期 → PWM 频率 → IOA0 方波），不需要重采样、不需要音频数据、不需要采样库。真正的门槛只剩**实时率**（见 §5.3）。

### 0.1 第一轮结论错在哪（留档）

第一轮结论是「这份 ROM 不产生音频」，依据是 DAC 寄存器 `0x3040`–`0x3043` 静态 0 命中、动态 30 分钟 0 次写入、48 kHz 流全零。
**这些数据本身没错，但它们只证明"DAC 这条路没被用"，不能推出"ROM 没声音"。** 错在把「一条通路死」当成「整机无声」——漏掉了 `IO_PWM`（`IOA0`–`IOA3`，手册 7.8）+ `PWM0`–`PWM3` + `TimerB` 这条完全没有建模的硬件通路。

---

## 1. 声音链路拆成三层看

| 层 | 内容 | 本项目现状 |
|---|---|---|
| L1 输出硬件 | ESP32-C3 → I2S0 → ES8311 codec → 功放 → 喇叭；I2C0 与 CW2017 共用 | **已通**，`audio.c` 全部拉起来了（48 kHz / 16-bit / MCLK 12.288 MHz） |
| L2 模拟器音频模型 | guest 的发声寄存器 → 环形缓冲 → PCM → I2S | **缺失**：DAC 有模型（桌面版）、PWM/TimerB **两版都没有** |
| L3 guest 音源 | ROM 里的发声服务 `0x55A9`，按事件调用 | **存在且已在跑**（60 s 实测 71 次 TimerB 重装） |

一句话：L1 是好路，L2 是空路，L3 有车——**只是车上装的不是 DAC，是 PWM**。

---

## 2. 原机音频硬件（GPCE4 / GPCE4064B）

### 2.1 三条并存的通路

| 通路 | 载体 | 谁来驱动 | 状态 |
|---|---|---|---|
| 软件通道 CH1/CH2 | 14-bit DAC，`0x3041`/`0x3042` | guest 逐样本写 | **本 ROM 从不使用**（§3.1） |
| **IO_PWM** | `IOA0`–`IOA3`，手册 7.8；`PWM0`–`PWM3` 配置 | `TimerB` 定频率 + `PWM0_Ctrl` 定占空比 | **★ 本 ROM 的发声通路** |
| 模拟输入 ACIN | 外部模拟信号 | 硬件 | 未使用 |

MAME 只接了第一条（`DAC_16BIT_R2R_TWOS_COMPLEMENT` → speaker），后两条**完全没实现**：
`ref/generalplus_gpce4_soc.cpp:529-548` 的四个 `pwmN_ctrl_w` 只 `LOGMASKED` + 存档，不产生波形、也不接 speaker。

### 2.2 发声服务 `0x55A9`（本轮实锤）

`_research/dasm_unsp.py 55A9 561E` 实测（节选）：

```
0055AD: 970B 3012   ; r4 = 3012 (TimerB_Data)
0055B2: 9403 / 9840 / F02C / D4E3   ; [0x3012] = r3   ← 音高周期
0055B3: 970B 3013   ; r4 = 3013 (TimerB_Counter)
0055B7: ...                          ; [0x3013] = r3
0055B8: 970B 3016   ; r4 = 3016 (Timer_Ctrl)
0055BD: B50C FF0F   ; r3 &= 0xFF0F
0055C9: A50C 00F0   ; r3 |= 0x00F0   ← 开 TimerB
0055D5: A50C 0201   ; r3 |= 0x0201   → 写 0x3002 (IOA_Direction)
0055E1: A50C 0101   ; r3 |= 0x0101   → 写 0x3003 (IOA_Attribute)
0055ED: B50C FDFE   ; r3 &= 0xFDFE   → 写 0x3001 (IOA_Buffer)，IOA0 拉低
0055F5: 990C 0C98   ; r5 = 0C98      ← 读 RAM 0x0C98（疑似"声音开关"）
0055FF: A848        ; ...
00560B: B50C FFF7   ; r3 &= 0xFFF7   → 写 0x300F (IO_Ctrl)，清 bit3
        (另一支)     ; r3 |= 0x0008   → 写 0x300F，置 bit3 = IO_PWM 使能
005617: A50C 02FF   ; → 写 0x3020 (PWM0_Ctrl) = 0x02FF
```

关声服务 `0x561F`：`IO_Ctrl &= 0xFFF7`、`IOA_Buffer &= 0xFCFE`、`PWM0_Ctrl = 0`。

flash 全镜像扫 `CALL16 0x55A9`（`F040 55A9`）：**只有一处，`0x20A404`**，上下文 `5DC0 0908 0001 D6C4 F040 55A9 0041 9640 …`。

### 2.3 它真的在跑，而且奏出了乐句

60 s `--autoplay` 实测的 TimerB 重装时间线（共 71 次，`_research/audio_probe/iolog_auto60.txt`）：

```
t=  0.330s  TimerB=E845 (6075 tick)      ← 第 1 声
t=  2.063s  TimerB=E845   dt=1733 ms
t=  5.063s  TimerB=E845   dt=3000 ms     ← 之后每 3.0 s 一次
t=  8.063s  TimerB=E845   dt=3000 ms
t= 11.063s  TimerB=E845   dt=3000 ms
t= 16.328s  TimerB=D335 (11467 tick)     ← 换成双音
t= 16.374s  TimerB=E99A (5734 tick)      ← 之后 D335/E99A 每 46.0 ms 交替
   ...      各 33 次，持续 3.0 s
t= 19.364s  调 0x561F 关声
```

`11467 / 5734 = 2.0003` —— **正好一个八度**。背光不会每 46 ms 改一次周期，只有音高会。

换算频率（两个候选分频，`0x3016` 的分频位含义未验证）：

| TimerB | ticks | ÷16（6 MHz） | ÷64（1.5 MHz） |
|---|---|---|---|
| `0xE845` | 6075 | 987.7 Hz | 246.9 Hz |
| `0xD335` | 11467 | 523.2 Hz | 130.8 Hz |
| `0xE99A` | 5734 | 1046.4 Hz | 261.6 Hz |

→ 一段「每 3 s 一响」的提示音 + 一段 3 s 的两音颤音（相差整八度）。这是**音乐性**的，不是背光。

### 2.4 背光是另一路，别混

| 用途 | 通路 | 实测 |
|---|---|---|
| **声音** | `TimerB` + `PWM0(0x3020)` + `IOA0` | 76 次写，71 次 `0x02FF`（满占空比）、5 次 `0x0000`（关）；每次写之前必重装 TimerB |
| **亮度** | `PWM2(0x3022)` + `IOA2` + 6 档表 `00FE/A6/7D/53/2B/00`（99%/65%/49%/32%/17%/0%）+ 使能位 `0x0100` | 60 s 内只有 3 次：`0x0153`(4 档) → `0x01A6`(2 档) → `0x0000`(关) |

`0x202E71`–`0x202F0A` 那段（6 档表 + `|0x0100`）只在 `t=0.3296/0.3300 s` 跑过一次，是亮度初始化，不是声音。

---

## 3. 数据面：flash 里没有音频数据，因为这声音不需要数据

### 3.1 DAC 通路在这份 ROM 里是死的（数据没错，解释要改）

- `roms/dmc_v1_base.bin`（8 MiB）：`0x3040`–`0x3043` 作为立即操作数 **0 命中**；`rX = 0x304x` 0 处；`rX op= 0x304x` 0 处；`0x3000 + 0x41` 构造路径 **0 条**（`0x3000` 装载 16 处与 imm `0x40`–`0x4F` 的 7 处无同一寄存器交集）。
- `roms/internal_rom_221017a.bin`（64 KiB）：16 处，全在产测直通（`0x424A`）与 4 个从未被调用的淡出助手（`0x58D7`/`0x58F4`/`0x5911`/`0x592E`）。
- 动态：`--autoplay` 120 s / 1200 s、手动 A→C、长按 C 10 s、A→C→C 后 1800 s，`0x3040`–`0x3043` **全部 0 次**；48 kHz 流 5760 万帧全零。
- `INT_Ctrl(0x3050)` 60 s 内写 21 次（`0x0000` ×11、`0x0010` ×10），**bit15（timer_a / 20 kHz）从未置位**；IRQ0 handler `0x4834` 是空 `reti`。

**结论：DAC 这条路是彻底死的——但声音本来就不从那里出。**

### 3.2 flash 分区：2.75 MB 有效数据，没有一段音频

64 KiB × 128 块测绘：

- 有效内容只到 `0x2BFFFF`（2.75 MB）；`0x2C0000`–`0x7EFFFF` **83 块 100% 全零**（5.2 MB）；`0x7F0000` 只有存档区（`0x7FE000` 附近 30 字节非零）。
- 高熵块 `0x0C0000`/`0D0000`/`0E0000`/`0F0000`/`100000`/`130000`（6.17–6.60 bit/B）：**一阶差分熵比原始熵还高 0.5–1.1 bit**（如 `0x0F0000`：6.3802 → 7.5240），最长 ±1 单调斜坡只有 43 字节。8-bit PCM 的签名是反过来的（差分熵低 1–2 bit、斜坡上千字节）→ **这些是压缩/抖动过的图形**。
- 没有 4-bit ADPCM 系数表，也没有「相邻项比值 ≈ 0.9438（2^-1/12）」的 12 平均律频率表。
- ASCII/UTF-16 关键词全 0 命中：`DIGI`/`DMC`/`VER`/`Sound`/`BGM`/`VOL`。

**结论：没有音频数据区——因为它是实时合成的（换 TimerB 周期），不需要采样库。**

### 3.3 软件 PWM / GPIO 位翻转也不成立

60 s / 314.7 万条 IO 写：

- `0x3001` IOA_Buffer：557 次（9.95/s），最快位 bit12 全程 ≈ **8 Hz**。
- `0x3005` IOB_Buffer：49,616 次，bit12 有 33,065 次翻转、10 ms 内最多 301 次（≈30 kHz）——但源 PC `0x5B48/0x5B4A/0x5B5F` 与 `0x3092 SPI2_TXData`（304 万次）同步，手册 7.8 明确 **IOB12/13 = SPI2 CS/CK**，是 LCD 总线。
- 可接压电片的 `IOA0`–`IOA3` 上**没有任何 kHz 级翻转**。

→ 声音不是"软件翻转 GPIO"做出来的，是**硬件 PWM**（`0x3020`）输出到 `IOA0`。

---

## 4. 可行性分析：要补什么才能听到原机声音

| 条件 | 现在 | 说明 |
|---|---|---|
| C1. guest 发起声 | **✓ 已有** | `0x20A404` → `0x55A9`，60 s 内 71 次 TimerB 重装 |
| C2. 模拟器把 PWM 通路转成 PCM | ✗ | 需要新写一个 PWM/TimerB → 方波的模型 |
| C3. 输出时钟与模拟时间一致 | ✗ | 设备实时率约 20 %，音频会被拉长 5 倍 |

### 4.1 C2：PWM 方波模型（工作量比 DAC 方案更小）

需要拦截 / 维护的状态：

| 寄存器 | 用途 |
|---|---|
| `0x3012`/`0x3013` TimerB_Data/Counter | 音高周期（决定频率） |
| `0x3016` Timer_Ctrl | TimerB 使能 + 分频位 |
| `0x3002`/`0x3003` IOA_Direction/Attribute | IOA0 是否为输出 |
| `0x3001` IOA_Buffer | IOA0 电平 |
| `0x300F` IO_Ctrl | IO_PWM 使能（bit3） |
| `0x3020` PWM0_Ctrl | 占空比 / 使能（`0x02FF` = 满，`0x0000` = 关） |

模型本身很简单：`enable && (PWM0 != 0)` 时输出一个频率 `f = f_timerb / 2` 的方波（占空比 50 %，因为 `0x02FF` 是满档），否则输出 0。占空比若将来出现中间值再按 `duty/256` 展开。

**比 DAC 方案省事的地方**：不需要 48 kHz 重采样、不需要 `tick()` 里做时间加权积分、不需要大环形缓冲（方波可以按相位直接合成），CPU 开销可以压到 < 1 %。

**要先解决的一个未知**：`TimerB` 的时钟源与分频（直接影响音高）。`0x3016` 写入值实测有 `0x0F0F`×72、`0x0FFF`×71 两种交替——高低字节分别是 TimerA/TimerB 的控制位，分频表手册没给全。两种候选（÷16 → 988/523/1046 Hz，÷64 → 247/131/262 Hz）差两个八度，**必须用真机录音或示波器定标准**。

### 4.2 C3 端的接线位置

- `firmware/digicore-c3/main/emu/gpce4.cpp:911` 的 `pwm0_ctrl_w(uint16_t)` 现在是空函数，改成存值 + 更新频率。
- `dac_cha1_data_w` 保持现状（DAC 不用）。
- `audio.c` 的 `render_beep()` 换成"常驻方波合成器"：读当前音高状态 → 按 I2S 采样率生成样本 → 写 I2S。现有 ES8311/I2S0 初始化不用改。
- 任务优先级：现有 `audio_task` 是 1（与 app_main 同级），常驻合成后建议提到 2，避免帧循环忙等时饿死音频。

### 4.3 时序：20 % 实时率仍然是硬约束

- 模拟 1 s 需要 4.9 s 墙钟（本轮 1800 s / 367 s 实测）。
- 方波按**模拟时间轴**合成，再以 48 kHz 播出去 → 音高被拉低约 4.9 倍（两个多八度）。
- 三种处理都不好听：慢放（音调低）、补静音（断续）、音频追画面（脱节）。
- **唯一正解仍是把实时率提到 ≥ 1.0x**；在 20 % 上做任何音频方案，出来的都不是原机那个音。

> 补充：好在这套声音是方波，不是采样。若将来接受"只在事件发生时按墙钟播完整段"（例如一次 beep 0.2 s），听感会比连续流好得多——但那就不是"逐样本模拟"了，需要产品侧拍板。

### 4.4 明确不做

- 外挂音效库 / 原机录音采样配音：不是 ROM 的输出。
- 按 UI 状态推断事件再播预制音：等于重写玩法语义。

---

## 5. 建议路线

### 阶段 0：把最后两个未知钉死（不改固件）
1. **TimerB 分频/时钟**：真机录音一段 beep，测基频；或示波器测 `IOA0`。这决定方波模型的频率系数（÷16 还是 ÷64，差两个八度）。
2. **Sound 门控点**：`0x55A9` 里读 `RAM 0x0C98` 后分两支写 `IO_Ctrl`。需要确认「Sound = 無音」时是不是走 `&= 0xFFF7` 那支（即整条 PWM 被关）。设备侧 10 分钟可验：串口 `Z 0C98` 读值 + 改设置前后对比。

### 阶段 1：管道（host 先跑通，再上设备）
3. 在 host 版 `emu/gpce4.cpp` 加 PWM/TimerB → 48 kHz 方波模型，`--wav` 导出，用 60 s autoplay 复现 §2.3 那段乐句。这是最小闭环，能直接听。
4. 验证通过后移植到 C3（`pwm0_ctrl_w` + `audio.c` 常驻合成器），环形缓冲 4–8 KiB 足够。

### 阶段 2：性能（音频能否正确的前提）
5. 用 `app_main.cpp` 的 `C3_PROF` 开关（`:2113-2148`）实测 `slice`/`exec`/`tick` 占比，找 3–5 倍的差距。
6. 实时率接近 1.0x 后，音高才可能是对的。

---

## 6. 未验证项

| 项 | 状态 | 影响 |
|---|---|---|
| `TimerB` 时钟源与分频（`0x3016` 高位） | 未验证（两种候选差两个八度） | 决定音高对不对，**必须真机定标** |
| `RAM 0x0C98` 是不是 Sound 开关 | 未验证（`0x55A9` 里明确读它分两支） | 决定静音门控怎么实现 |
| `PWM0_Ctrl` 位定义 | 未公开（手册 7.9.1 只有一句 duty 1/256..254/256） | 目前按"满占空比方波"处理，够用 |
| host 模拟器进不到 ROM 设置菜单（`0x0C88=0x23` 卡死） | 已知阻塞（`docs/HANDOFF_NEXT.md` §4.1 的 park 点问题） | 拿不到菜单像素与"Sound 改哪个 offset"的直接证据 |
| 设备实测 `dac=0 pwm=0` 与 host 60 s 有 76 次 PWM0 写的矛盾 | 未查（可能是设备没触发发声事件，或统计窗口不同） | 需要一次设备侧长跑对账 |
| `mapacman` 在 MAME 里是否也走 PWM（而非 DAC） | 未验证 | 若也走 PWM，说明 MAME 对 GPCE4 音频整体缺失 |
| 原始 dump `mx25l6433f.u2`（CRC `9999e2a0`）与现行的差异 | 未比对 | 与声音关系已不大（声音是代码合成） |

---

## 7. 参考资料

本地：
- `_research/audio_probe/FINDINGS_2026-11-24.md`（本轮取证全量：IO 钩子、时间线、分区测绘、静态扫描）
- `_research/audio_probe/`（`digicore_probe.exe`、`static_scan.py`、`ioarith_scan.py`、`analyze_iolog.py`、`iolog_auto60.txt`）
- `docs/AUDIO.md`（第一轮：DAC 模型与四条证据——现在只作为"DAC 路是死的"的证据）
- `docs/RESEARCH_DMC_MANUAL.md`（官方说明书：Sound 设置、A+C 静音、呼叫 beep、关机音）
- `docs/HANDOFF_2026-11-24.md` §3（五条硬件事实）
- `ref/generalplus_gpce4_soc.cpp:139/146-147/200-203/274-277/529-548/606-628/635-702`
- `ref/generalplus_gpce4.cpp:452-458/462-468/483/494`

外部：
- MAME PR #15246 `generalplus gpce4 refactoring`：<https://github.com/mamedev/mame/pull/15246>
- MAME PR #15282（`more properly factory defaulted digicolr data`）：<https://github.com/mamedev/mame/pull/15282>
- `generalplus_gpce4_soc.cpp`：<https://github.com/mamedev/mame/blob/master/src/devices/machine/generalplus_gpce4_soc.cpp>
- Digital Monster Color 说明书（humulos 转录）：<https://humulos.com/digimon/dmc/manual/>
- 日文攻略站（音量三档、呼出条件）：<https://nhoko.xxxxxxxx.jp/digicolor.html>
- 拆机视频（压电片）：<https://www.youtube.com/watch?v=xWu7XdVVxek>
