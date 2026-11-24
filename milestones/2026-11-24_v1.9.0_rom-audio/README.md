# v1.9.0 — 彩屏元祖的声音（ROM buzzer，TimerB + PWM0）

**里程碑判据：设备上按 A/B/C 能听到原 ROM 自己演奏的音符，而不是固件合成的 1760 Hz 提示音；音量在菜单 20 % 时听感合适。**

| 项 | 值 |
|---|---|
| 版本 | `1.9.0`（`firmware/digicore-c3/version.txt`） |
| 日期 | 2026-11-24 |
| 源码 | git tag `v1.9.0` |
| 串口自检 | `VER ver=1.9.0 win=0,18 96x96 scale=2 left=64 top=24 off=0,0 home=64,24 fb=128 chunk=16` |
| 音频状态 | `AUDIO hz=0 blocks=0 codec=1 vol=20 buzzer_hz_now=0`（静音时） |
| app 镜像 | `digicore-c3-v1.9.0.bin` 1,007,392 B |
| SHA256 | 见 `sha256.txt` |

---

## 一、问题与结论

v1.8.0 及之前，设备上唯一的声音是 `audio.c` 合成的方波（按键 1760 Hz / 18 ms、开机双音）。ROM 从未被听到。

本轮把 ROM 的发声通路挖出来并接上：

- **ROM 有声音，但不走 14-bit DAC**，走的是 mask ROM `0x55A9` 这个「硬件 PWM 蜂鸣服务」：
  `TimerB_Data/Counter(0x3012/0x3013)` 定音高 → `Timer_Ctrl(0x3016) |= 0x00F0` 开 TimerB →
  `IOA_Direction/Attribute(0x3002/0x3003) |= 0x0201/0x0101` 把 IOA0 设成 IO_PWM →
  `IOA_Buffer(0x3001) &= 0xFDFE` → `IO_Ctrl(0x300F) |= 0x0008` → `PWM0_Ctrl(0x3020) = 0x02FF`。
  关声是 `0x561F`。全 flash 只有一处调用：`0x20A404`（`F040 55A9`）。
- 音高换算：**f = 6 MHz / ticks**（TimerB 时钟 96 MHz ÷ 16）。实测频率精确落在音名上：
  6075→987.7 Hz(B5)、7224→830.6 Hz(G#5)、8591→698.4 Hz(F5)、11467→523.2 Hz(C5)、5734→1046.4 Hz(C6)。
- **DAC 通路在这份 ROM 里是死的**：`0x3040`–`0x3043` 全镜像 0 处立即数、`rX=0x304x` 0 处、`0x3000+0x41` 构造 0 条；动态 30 分钟 0 次写入。`MACHINE_NO_SOUND` 与 `dac=0` 都由此而来。

完整取证见 `docs/RESEARCH_AUDIO_2026-11-24.md` 与 `_research/audio_probe/FINDINGS_2026-11-24.md`。

## 二、实现

| 文件 | 改动 |
|---|---|
| `main/emu/gpce4.h` | 新增 `buzzer_hz()`（`PWM0!=0 && IO_Ctrl bit3 && TimerB!=0` → `6000000/ticks`）、`PwmEvent` 环形缓冲（24 条）与取值接口 |
| `main/emu/gpce4.cpp` | `timerb_data_w()` / `pwm0_ctrl_w()` 存值、记事件、**并调用 `audio_set_buzzer()`**（事件驱动）；`pwm0_ctrl_r()` 返回存值 |
| `main/audio.c` | `audio_task` 改为常驻方波合成器：读 `s_buz_hz` → 相位累加 → 50 % 方波 → 512 帧/块写 I2S；`audio_set_buzzer()`；`audio_debug()`；符号播放时 `audio_idle_poll()` 不关 codec |
| `main/audio.h` | `audio_set_buzzer()` / `audio_buzzer_active()` / `audio_debug()` |
| `main/app_main.cpp` | 每帧 `audio_set_buzzer(s_soc.buzzer_hz())` 兜底；删除合成按键 click；`p` 命令增加 `AUDIO` 行；新增 `a <0-100>` 现场调音量 |

## 三、四个必须记住的坑

1. **轮询救不了短音符**：设备帧率 15–18 fps（60 ms/帧），而音符只有 50 ms。必须**在写寄存器的路径上直接推送**音高，否则每次采样都错过。
2. **音频任务优先级不能是 1**：与模拟器主循环同级时，FreeRTOS 轮转让 5.3 ms 的块 16.8 ms 才写一次（实测 `blocks=190/3.2 s`，不到 1/3 速率，听不出音）。**改成 3**，块 512 帧后实测 `blocks=48/0.5 s`，即 100 % 实时。
3. **codec 会在空闲 15 s 后断电**：`audio_idle_poll()` 关掉 ES8311 + I2S，而 buzzer 路径原先只 `if (!s_codec_on) continue;`，于是空闲后第一次按键被吞掉。现在音符到来时会先 `codec_power_on()`。
4. **音量要按听感标定，不能拍脑袋**：基准幅度经过 20000 → 10000 → 5000 → 2500 → **1250** 五轮真机试听。最终 `amp = 1250 × vol%`：菜单 20 % → 250 counts（0.76 % FS），40 % → 500（上一轮 20 % 的响度）。听感上 20 % 已经偏轻，是用户认可的档位。

## 四、验证

```
p                     → AUDIO hz=0 blocks=... codec=1 vol=20 buzzer_hz_now=0
                        PWM events=N last pwm=0000 tb=E845 io=0000
                        #0 t=... pwm=02FF tb=E3C8 io=0008     ← 开声、G#5
                        #1 t=... pwm=0000 tb=E3C8 io=0000     ← 关声
a 40                  → ACK a vol=40          （现场调音量，不落盘）
```

- 上机实测：按 B 触发一串 50 ms 换音的音阶（B5→G#5→F5→D5→C5→…），设备能听到。
- `blocks` 增长率 = 96 块/秒（512 帧 × 10.7 ms），与 48 kHz 实时一致。
- 事件链与 `PWM0/TimerB/IO_Ctrl` 三个寄存器完全对应，`io=0008` 即 IO_PWM 使能位。

## 五、遗留

| 项 | 状态 |
|---|---|
| 真机录音对音高 | 未做（换算结果精确落在音名上，但缺原机对照） |
| `RAM 0x0C98` 是否为 Sound 开关 | 设备上恒为 1，未做关声对照实验 |
| 呼叫 / 进化 / 战斗 / 死亡音效 | 未逐一触发验证（机制相同，只是 TimerB 值不同） |
| 设备实时率 | 88–93 %（`docs/HANDOFF_2026-11-24.md` 的 27 % 已过时） |
