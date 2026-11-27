# R7 第七轮审计：外设/资源 —— 冗余代码与冗余资源

审计对象：`firmware/digicore-c3/main/` 中的外设与资源侧（display / bezel / font / audio / battery / wallclock / saveimg / gueststate / fbsave / dmc_link / rom_esp / assets / partitions.csv / sdkconfig.defaults）。
方法：只读源码 + `grep` 引用普查 + `riscv32-esp-elf-nm -S` 看目标文件符号尺寸 + `build/digicore-c3.map`（2026-11-27 19:16，与 `digicore-c3.bin` 同批）判断「是否真的进了镜像」。未改任何源码，未跑 `idf.py`。

---

## 0. 先给结论（别指望在这里省出大钱）

| 口径 | 数值 | 说明 |
|---|---|---|
| app 镜像 `digicore-c3.bin` | **1,144,944 B** | 占 2 MiB factory 分区 54.6% |
| `.flash.text` | 0xa981c = **694,300 B** | 其中 `aot_blocks.c.obj` 一家 **580,886 B**（57%，见 §3.1） |
| `.flash.rodata` | 0x4efa0 = **323,488 B** | 其中 internal ROM 65,540 + 砖墙 16,384 + 字体 590 |
| `.dram0.data` / `.dram0.bss` | 7,408 / **112,232 B** | 本轮可回收 13,312 B（其中 8,192 属邻域 romstore.h） |
| 本轮范围内可回收 flash | **14,631 B**（砖墙 14,336 + 字体 295） | 占镜像 1.28% |
| 本轮范围内可回收 RAM | **5,120 B**（`s_header` 4,096 + audio 复用缓冲 1,024） | 分区/表另计 |
| 分区表可回收 | phy_init 4 KiB + nvs 瘦身 12 KiB；另有 **3.31 MiB 整片 flash 没进分区表** | §3.2 |

一句话：**外设侧没有「每帧白干活」级的问题**（§4 逐条证伪），真正的冗余是「静态数据用 8 倍体积存 1 bit 信息」（砖墙）、「一个 4 KiB 缓冲只写 100 B」（DMC1 头）、以及分区表/配置里几处没人管的死参数。想省体积该去 aot_blocks，不在这里。

---

## 1. 摘要表

类型：**DEAD**=无引用（已被 `--gc-sections` 丢弃，仅源码冗余）／**BLOB**=静态数据冗余／**RAM**=常驻内存冗余／**DUP**=两套实现／**STALE**=与代码矛盾的注释或配置／**REDO**=运行期重复劳动。

| ID | 位置 | 类型 | flash / RAM 影响 | 运行期代价 | 风险 | 建议 |
|---|---|---|---|---|---|---|
| P-01 | `main/display.c:269-272`、`display.h:33` | DEAD | 0（map 已丢，源码 118 B） | 无 | 极低 | 删函数+声明 |
| P-02 | `main/display.c:27-31` | STALE | — | — | 低（可能掩盖面板时钟回归） | 二选一：常量降 40 MHz 或改注释 |
| P-03 | `main/display.c:243-246`（对比 v1.15 前） | 行为丢失 | — | 超时不再返回错误 | 低-中 | 加回返回值或明确"仅告警" |
| P-04 | `main/bezel.cpp:81-91` + `brick_a8.h:24` | BLOB | **-14,336 B 可省**（16,384→2,048） | 每次重绘多 3 次查表（仅重绘时） | 中（破坏"逐字节拷贝自姊妹项目"约定） | 待裁决 |
| P-05 | `font5x7.inc:3`（被 `bezel.cpp:30`、`menu.cpp:36` 各包含一次） | DUP | **-295 B 可省**（两份 0x127） | 无 | 极低 | 改成单一定义 + extern |
| P-06 | `main/bezel.cpp:219`（`bezel_visible` 逐像素调 `row_span`） | REDO | — | ~476 px × 每次 ~15 次 while 迭代 ≈ 7k 次空转/次刷新 | 极低 | 每行算一次 x0/x1 |
| P-07 | `main/bezel.cpp:194`（`bezel_paint` 末尾顺带画电量） | DUP | — | 电量角被推两次（band + 角）≈1 KB/次 | 极低 | 不动 |
| P-08 | `main/audio.c:491-508`、`audio.h:38` | DEAD | 0（map 已丢，源码 212 B + 512 B bss 也已丢） | 无 | 极低 | 删 |
| P-09 | `main/audio.c:328-331`、`audio.h:31` | DEAD | 0（源码 14 B） | 无 | 极低 | 删 |
| P-10 | `main/audio.c:86` vs `:447` | DUP | **-1,024 B RAM 可省** | 无 | 低 | 两个方波发生器共用一个缓冲 |
| P-11 | `main/audio.c:433-476` vs `:125-131` | DUP | — | 两套方波合成逻辑（int phase / Q32 phase） | 低 | 合并或注释标注分工 |
| P-12 | `app_main.cpp:3523` → `main/audio.c:321` | REDO | — | 每帧 1 次 `6000000/ticks` 软除 + 1 次 `esp_timer_get_time()` ≈ 250 cyc/帧 ≈ 0.03% CPU | 低（这行是恢复快照后的兜底） | 待裁决 |
| P-13 | `main/battery.c:190-197`、`battery.h:29` | DEAD | 0（源码 96 B，已被内联） | 无 | 极低 | 保留为诊断 API 或删 |
| P-14 | `main/wallclock.cpp:202-209`、`wallclock.h:65` | DEAD | 0（源码 186 B，已内联进 `wallclock_hms`） | 无 | 极低 | 改 static |
| P-15 | `main/wallclock.cpp:75-84` 依赖 `menu.cpp:96` 的 `nvs_flash_init()` | 隐式耦合 | — | 无 | **中**（menu 若失序，时钟静默不落盘） | 待裁决：`nvs_flash_init` 上提到 app_main |
| P-16 | `main/fbsave.cpp:47-64`、`fbsave.h:40` | DEAD（导出层） | 0（内部 3 处调用保活，导出符号被丢） | 无 | 极低 | 改 static |
| P-17 | `main/dmc_link.cpp:378-386`、`dmc_link.h` | DEAD | 0（源码 36 B） | 无 | 极低 | 删 |
| P-18 | `main/dmc_link.cpp:91` + `:258-261` | RAM | **-3,996 B 可省**（4,096→100） | 无 | 中（头扇区尾部会从 0x00 变 0xFF） | 待裁决 |
| P-19 | `main/dmc_link.cpp:312` `s_rom_ok = s_rom_ok;` | 废语句 | 0 | 无 | 极低 | 删 |
| P-20 | `main/dmc_link.cpp:36` `#include "saveimg.h"` | DEAD include | 0 | 无 | 极低 | 删 |
| P-21 | `main/rom_esp.cpp:40,109` `s_image_size` | 只写不读 | 4 B | 无 | 极低 | 删 |
| P-22 | `main/rom_esp.cpp:117` 与 `:133/:144` | REDO | — | `SparseRom::begin()` 每次挂载多跑一次（约几十 cyc，一次性） | 极低 | 删 117 行 |
| P-23 | `main/rom_esp.cpp:565-567` `internal_rom_r` 逐字节拼 16 位 | REDO（热路径） | — | 每次内部 ROM 取指 2 次 XIP 读 + 移位，可 1 次对齐 16 位读 | 低 | 视 §2.3 结论处理 |
| P-24 | `app_main.cpp:910-938` `rot_map()` + `:1163-1171` | DEAD（编译期消） | 0 | 0 | 极低 | 删旋转分支或改成 `if constexpr` |
| P-25 | `partitions.csv:16` `phy_init` | 分区空置 | 4 KiB 分区空间 | 无 | 低（将来开 WiFi/BT 会需要） | 待裁决：先 dump 证空 |
| P-26 | `partitions.csv:15` `nvs` 24 KiB | 分区过大 | 12 KiB 可省 | 无 | 低 | 待裁决 |
| P-27 | `partitions.csv` 表尾 0x4b0000 vs 8 MiB | 布局浪费 | **3,473,408 B 未编址** | 无 | 低 | 待裁决 |
| P-28 | `sdkconfig.defaults:31` `CONFIG_COMPILER_LTO=y` | STALE（未生效） | 生效后可能 -10~20% | 未知（可能伤 aot 命中率） | 中 | 待裁决 |
| P-29 | `sdkconfig.defaults:24` "roms (6 MiB sparse ROM)" | STALE 注释 | — | — | 极低 | 改 2.5 MiB |
| P-30 | `sdkconfig.defaults:30` `LOG_MAXIMUM_LEVEL_DEBUG=y` | 配置冗余 | 极小（main 里 `ESP_LOGD` 仅 3 处） | 无 | 极低 | 不动 |

---

## 2. 逐条证据

### 2.1 `display.c` / `display.h` —— 三层推送里没有死中间层，死的是最外面那个

**P-01 `display_flush()` 零引用。**

```
$ grep -n display_flush main/*.c main/*.cpp main/*.h
display.c:269: void display_flush(void)
display.h:33:  void display_flush(void);
```

只有定义与声明，没有任何调用点。`display.c.obj` 里该函数 0x76 = 118 B（`riscv32-esp-elf-nm -S`），map 第 4066–4067 行落在 **Discarded input sections**（地址 `0x00000000`），即最终镜像 0 字节。**参与编译（在 `SRCS` 里），但被 `--gc-sections` 丢掉。**

**三层结构核查（题目第 5 问）：**

| 层 | 行 | 调用方 | 判决 |
|---|---|---|---|
| `display_push`（static 核心） | 230 | 251、266 | 活，唯一实现，**不是死层** |
| `display_draw_bitmap`（同步） | 250 | app_main 877/983/1176/1900-1964、menu 586/644/709/757/888、bezel 189/231/271 | 活，主力路径 |
| `display_draw_bitmap_async`（排队） | 264 | 仅 `app_main.cpp:1130`（整窗推） | 活，单点使用；函数体与 `display_push` 完全等价（`return display_push(...)`），保留语义即可 |

结论：**没有"已死的中间层"**。三层各自承担不同语义（同步/异步/共享核心），唯一该删的是 `display_flush`（异步路径的收尾由"下一次 push 头部的 `display_wait_idle()`"承担，见 `display.c:239`）。

**P-02 注释说 40 MHz，代码写 80 MHz。**

- `display.c:27-30`：「40 MHz, not 80: at 80 the panel showed occasional horizontal displacement… Halving it…」
- `display.c:31`：`#define LCD_PCLK_HZ (80 * 1000 * 1000)`
- 交叉证据 `app_main.cpp:404-406`：「The link on this board runs at **39.8 Mbit/s, not the 80 MHz display.c asks for**: … a 12,288 B band at 2.2-2.7 ms, which is the 40 MHz class」

即：注释描述的补丁**从未落在常量上**，而实测链路本来就只有 39.8 Mbit/s（驱动把请求钳到 40 MHz 档）。`git log` 显示 v1.15.0（efdf865）只改了异步推送，没动这个常量——是 baseline 起就存在的矛盾。要么把常量改成 40 MHz 让代码与注释一致，要么删掉注释里的"40 MHz 结论"。

**P-03 v1.15 把一个错误码吃掉了。**

v1.15 之前 `display_draw_bitmap` 超时会 `return ESP_ERR_TIMEOUT`（`git diff 5ddc892 efdf865 -- main/display.c`）；现在 `display_wait_idle()`（`display.c:217-228`）超时只 `ESP_LOGW`，`display_draw_bitmap` 仍返回 `ESP_OK`。上层 `app_main.cpp:1183` 的 `err == ESP_OK` 分支因此会把"传输超时"当成成功并清 dirty map → 那一帧永久丢失。属功能可靠性问题，不是冗余，列此备查。

### 2.2 `bezel.cpp` / `bezel.h` / `brick_a8.h` / `font5x7.inc`

**外壳是否每帧重画？否——v1.16 的说法成立。**
`bezel_paint` 全部 6 个调用点：`app_main.cpp:1658`（控制台 `G 0`）、`1682`（`E` 改窗位）、`3349`（开机）、`3433`（设置卡关闭）、`3495`（菜单关闭）、`3513`（lapse 卡消失）。**没有一个在帧循环里。** 帧循环里只有 `bezel_draw_battery`（`app_main.cpp:3612`，且被 `pct != s_batt_pct` 与 30 s 门限双重限制）。唤醒路径 `app_main.cpp:2009-2034` 明确只改背光，注释写着「the whole shell here, which cost ~30 ms of blocking SPI on the wake frame」——已被移除。

**P-04 砖墙图 16,384 B 存 1 bit 信息。**

`brick_a8.h` 源码 103,446 B，数组 `static const uint8_t kBrickA8[128*128]`（`brick_a8.h:24`），`nm -S bezel.cpp.obj` → `_ZL8kBrickA8 0x4000`，map 第 22456 行 `.rodata._ZL8kBrickA8 0x4000 @0x3c0c0b3c`。**16,384 B 实打实进 flash。**

取值普查（对文件内所有 `0xNN` 字面量去重）：

```
00 x13889
FA x2495     ← 共 16,384 个，只有两个值
```

而消费方 `bezel.cpp:83,87,89` 只做 `if (kBrickA8[...])` 的真假判断（`bezel.cpp:9-11` 的注释也承认"250 = mortar joint, 0 = stone face"）。**这是 1 bpp 掩码，用 8 bpp 数组存**。打包成 1 bpp = 2,048 B，省 **14,336 B**；解码代价是每次查表多一次移位，而查表只发生在 `bezel_paint`（非每帧）。

风险点：`brick_a8.h:3-8` 声明「Copied byte for byte from the sibling project」，重新打包会破坏这条"可回拷"约定（姊妹项目 `digimon-passport/main/dg_brick.h` 用的是 A8 图像描述符）。**列为待裁决。**

**P-05 字体被编译两份。**

`font5x7.inc:3` 是 `static const uint8_t kFont5x7[] = {...}`（59 字符 × 5 = **295 B**），被两个 TU 各包含一次：

```
bezel.cpp:30: #include "font5x7.inc"      → menu.cpp:36 同
```

map 里出现两次，这就是铁证：

```
22444: .rodata._ZL8kFont5x7  0x127  libmain.a(menu.cpp.obj)   @0x3c0c08ec
22453: .rodata._ZL8kFont5x7  0x127  libmain.a(bezel.cpp.obj)  @0x3c0c0a14
```

`static` 在头/inc 里就是"每 TU 一份"。省 295 B，改法：`.inc` 里只放数据、某个 TU 定义、头里 `extern const uint8_t kFont5x7[295];`。顺带一提 `app_main.cpp` 还自带第三套字形 `kFont3x5`（50 B，map 第 22355 行），是控制台字符画用途，不重复。

**P-06 `bezel_draw_battery` 逐像素重算行跨度。**

`bezel.cpp:219`：`uint16_t c = bezel_visible(x, y) ? shell_px(x, y) : 0;` —— `bezel_visible`（`bezel.cpp:154-162`）内部调 `row_span(y,...)`，而 `row_span`（`bezel.cpp:118-132`）对圆角行要跑一个 `while ((inset+1)² + edge² <= r²) inset++` 的循环（r=30，最多 30 次）。电量框 `tw×th` 最大 46×14 = 644 px，且全部落在 y=222..235 —— **正好在 30 px 圆角带里**，于是每像素都在跑那个 while。

代价估算：644 px × ~15 次迭代 × ~4 cyc ≈ 4 万 cyc ≈ **0.25 ms**，30 s 才一次。结论：**是真冗余但不值得改**（改法也就一行：把 x0/x1 提到行循环外）。列出来是为了证明"确实查过"，不建议动。

**P-07 `bezel_paint` 顺手画电量（`bezel.cpp:194`）。** 角框 y=222..235 与最后一个 band（224..239）重叠，等于同 476~644 像素推两遍（~1 KB 多余 SPI）。属"设计上省不掉"（222..223 属上一个 band），**不动**。

### 2.3 `assets/internal_rom_221017a.bin` —— 64 KiB 是真的在用，不能删

- 链接方式：`main/CMakeLists.txt:79-81` `target_add_binary_data(... BINARY RENAME_TO internal_rom)`；map 第 22726-22731 行：`.rodata.embedded 0x10004 @0x3c0ec438`，符号 `internal_rom` / `internal_rom_length`。**65,540 B 全在 flash（drom0），RAM 0 B。**
- 使用点：`app_main.cpp:62-63` 声明 → `app_main.cpp:2504` `s_soc.set_images(internal_rom, internal_rom_length, romstore::image())` → `emu/gpce4.cpp:562-568` `internal_rom_r()` 读 `m_irom[ofs]`，服务地址 `0x004000-0x00bfff` 与镜像段 `0x00c000-0x00ffff`（`gpce4.cpp:658-662`）。
- 是否重复：**没有第二份**。`roms` 分区里是 8 MiB 卡带镜像的稀疏页（`tools/make_sparse_rom.py`），内部掩码 ROM 不在其中；也没有"既在 flash 又复制到 RAM"——`internal_rom_r` 直接解引用 flash 映射地址。
- 唯一可议之处（P-23）：`internal_rom_r` 用 `m_irom[ofs] | (m_irom[ofs+1] << 8)` 两次字节读拼字，而 `ofs` 恒为偶数、`.incbin` 4 字节对齐，理论上可以一次 `*(const uint16_t*)`。这是内部 ROM 取指热路径（蜂鸣器音高、ROM 子程序都在这 64 KiB 里），每次省 1 次 XIP 访问 + 1 次移位。收益小、改动面在 emu/（不属本轮范围），**建议只记录**。

### 2.4 `audio.c` / `audio.h` —— 没有"闲置的 PWM 旧实现"，但有两条死 API + 两套方波

**先纠正题目里的假设：`audio.c` 里根本没有 LEDC/PWM 输出代码。** 蜂鸣是 `i2s_channel_write` 送方波给 ES8311 的 DAC 路径（`audio.c:118-134`）。"PWM" 一词在本工程只出现在 **guest 侧**：`emu/gpce4.cpp:954-968` 的 `pwm0_ctrl_w()` 记录卡带对 IO_PWM 的写并转发 `audio_set_buzzer()`。所以不存在"旧 PWM 实现闲置"。

**真正闲置的两条 API（都已从镜像消失）：**

| 符号 | 源码 | 引用普查 | 镜像 |
|---|---|---|---|
| `audio_silence` | `audio.c:491-508`（212 B），`audio.h:38` | `grep` 全工程仅定义 1 处，0 调用 | map 4055 行 **Discarded**；其 `static int16_t zeros[256]`（512 B bss）一并丢弃（map 4057） |
| `audio_buzzer_active` | `audio.c:328-331`（14 B），`audio.h:31` | 仅定义，0 调用 | map 4053 行 **Discarded** |

`audio.h` 对 `audio_beep` 的描述（第 20-21 行 "Blocking square wave"）也与实现相反——`audio_beep`（`audio.c:478-489`）现在是入队（非阻塞），阻塞点搬到了 `audio_task`。属文档债。

**P-10/P-11 两套方波：**

- `audio_task`：`static int16_t buf[1024]`（`audio.c:86`，bss `buf.3` 0x800）+ Q32 相位（`s_phase += inc`，`audio.c:118-131`）。
- `render_beep`：`static int16_t buf[512]`（`audio.c:447`，bss `buf.2` 0x400）+ `int phase`（`audio.c:449-463`）。

两者**跑在同一个任务里**（`render_beep` 只由 `audio_task` 第 94 行调用），所以 `buf[512]` 完全可以复用 `buf[1024]`，省 **1,024 B bss**。两套相位算法是"两套实现"的实体：一套 Q32 定点（长音、频率任意）、一套 int 半周期（短音）。**合并有回归风险**（短音 timing 是实测调过的），建议只做缓冲复用。

**P-12 每帧重发同一音符。**
`app_main.cpp:3523` 每帧 `audio_set_buzzer(s_soc.buzzer_hz())`；而 `buzzer_hz()`（`emu/gpce4.h:382-391`）内含一次 `6000000 / ticks` **软除法**（RV32IMC 无硬件除法，走 `__divsi3`），`audio_set_buzzer`（`audio.c:321-326`）在 `hz>0` 时再调一次 `esp_timer_get_time()`。
代价：60 帧/s × ~250 cyc ≈ 15k cyc/s ≈ **0.03% CPU**——可忽略，但它确实是"每帧重算常量"。
**别急着删**：这行还是快照恢复后唯一的补发通道（`app_main.cpp:1551` 的注释与 `audio.c:104-116` 的 codec 唤醒都依赖 `s_last_beep_us` 被刷新）。真要删，得在恢复路径补一次显式发布。**待裁决。**

### 2.5 `battery.c` / `battery.h`

- 6 个 static 函数（`cw_read`/`cw_write`/`cw_enter_sleep`/`cw_enter_active`/`cw_profile_matches`/`cw_update_profile`）**全部有引用**（引用数 9/7/2/3/2/2），无死函数。
- `s_profile[80]`（`battery.c:51`，`.rodata.s_profile` 80 B）每次开机被 `cw_profile_matches` 读 80 次比对——这是 CW2017 的厂商模型，**必须留**。
- **P-13 `battery_mv()` 是"只给自己用"的导出 API**：`grep` 全工程只有 `battery.c:176`（`battery_init` 的日志）引用，`app_main` 从不调用；map 第 4061 行 `.text.battery_mv 0x60` 落在 Discarded（被内联进 `battery_init`）。`battery.h:27` 说它是"Diagnostic"。**结论：不是死代码，是死导出**——留着无害，改 `static` 更诚实。

### 2.6 `wallclock.cpp` / `wallclock.h`

`days_from_civil`、`month_from_name`、`build_epoch`、`now_us`、`nvs_open_once` 全部有引用；8 个导出函数里 7 个被 app_main/dmc_link 调用。

**P-14 `wallclock_local_sec()` 是死导出**：`wallclock.h:65` 声明，唯二引用是定义（`wallclock.cpp:202`）与 `wallclock_hms` 内部（`wallclock.cpp:213`）；map 第 4024 行 `.text._Z19wallclock_local_secv 0xba` 落在 Discarded（内联）。改 `static` 即可。

**P-15 隐式依赖（本轮唯一的"风险：中"项）：**
`wallclock.cpp:79` 调 `nvs_open(kNamespace, ...)`，前置条件是 `nvs_flash_init()` 已执行——但 `wallclock.cpp` **自己不调**（`wallclock.h:31` 只说 "Call once, after nvs_flash_init()"）。全工程唯一的 `nvs_flash_init()` 在 **`menu.cpp:96`**（`menu_load()` → `menu.cpp:107` 打开同名 namespace "digicore"）。
`app_main.cpp:3332-3334` 的顺序注释确认了这个约定（"NVS is up now (menu_load initialised it)"）。**目前能工作，但只要 menu 的初始化顺序动一下，时钟就会静默退回 build 时间戳**（`nvs_open` 失败只打一条 WARN，`wallclock.cpp:80`）。建议把 `nvs_flash_init()` 上提到 `app_main` 开头（不改语义，只去掉跨模块隐式约定）。

### 2.7 `saveimg.cpp` / `gueststate.cpp` / `fbsave.cpp` —— 三条 save 路径全部活着，没有旧路径残留

| 模块 | 落盘对象 | 区域 | 调用点（全部在 app_main） |
|---|---|---|---|
| `gueststate`（5×12 KiB 环） | CPU 文件 + 8 KiB 客 RAM + SoC 块 | `save` 0x00000-0x0F000 | init 2856、store 2730、load 1534/2858、ready 1527/2696/2755、newest_seq 1479/1521/1558、invalidate 1522 |
| `fbsave`（1×32 KiB 块） | 面板可见帧 24,576 B | `save` 0x0F000-0x17000 | store 2641、load 2666、stored_crc 2661/2663 |
| `saveimg`（2×16 KiB 槽） | SPI flash overlay 页 | `save` 0x18000-0x20000 | init 2560、load 2564、ready 2583、store 2608 |

三者共用一个 128 KiB 分区、偏移相邻、**互不重叠**（0x0F000+0x8000=0x17000 ≤ 0x18000；0x18000+2×0x4000=0x20000）。结论：**没有"旧的 save 路径"**，是三个不同粒度的持久化层，`gueststate.h:36-39` 与 `fbsave.h:18-22` 已把布局写死并互相引用。

- **P-16 `fbsave_init()` 是死导出**：`fbsave.h:40` 声明，外部 0 调用；三个使用者都在内部自调（`fbsave.cpp:68/100/113`）。map 第 4019 行 `.text._Z11fbsave_initv 0xa0` 落在 Discarded（被内联 3 份）。改 `static` 后头文件少一条导出。
- 两处刻意冗余，**不要"优化"**：`fbsave_load` 两遍流式读（先 CRC 再拷，`fbsave.cpp:124-147`）与 `saveimg_store`/`gueststate_store` 只回读头部校验（`saveimg.cpp:198-206`、`gueststate.cpp:147-154`）——原因写在注释里：DRAM 没有第二块 8/24 KiB 级缓冲。删任一处都会把"掉电撕裂"从"可检测"变成"不可检测"。
- `gueststate_invalidate_newest()`（`gueststate.cpp:190-222`）只被控制台 `i`（`app_main.cpp:1522`）调用，是**测试钩子**，属"只在调试时用"，但它存在的理由（模拟撕裂记录）无法用别的手段构造，**保留**。

### 2.8 `dmc_link.cpp` / `dmc_link.h` —— 协议分支没有孤儿，但有 4 KiB 白占

**P-17 两个死导出：**

```
dmclink_rom_ok()          dmc_link.cpp:378-381   引用数 1（仅定义）
dmclink_waiting_for_rom() dmc_link.cpp:383-386   引用数 1（仅定义）
```

map 4043/4045 行 **Discarded**（0xa / 0x1a）。`dmc_link.h` 里对应声明应删。

**协议对账（题目第 5 问"从未被固件处理的命令分支"）**：不存在。固件分支集合 = {`HELLO`, `FINISH`, `END`, `SAVE GET`, `ROM BEGIN`, `SAVE BEGIN`, `PAGE`, 兜底 `ERR unknown`}（`dmc_link.cpp:397-488`）；在役客户端 `tools/dmc_web.html` 发出的命令集合完全一致（`dmc_web.html:537,649,652,660,670,696,698,703,717,719,724`），且 `docs/WEB_TOOL.md:39-47` 的表与之逐条对应。**两边都没有多余分支。**

**P-18 `s_header[4096]` 只写 100 B（真正的 RAM 浪费）。**
`dmc_link.cpp:91`：`alignas(4) uint8_t s_header[kPageSize];`（bss 0x1000），唯一使用者 `rom_end()`：

```
258: std::memset(s_header, 0, sizeof(s_header));
259: std::memcpy(s_header, &h, sizeof(h));      // StoreHeader = 100 B
261: esp_partition_write(s_rom_part, 0, s_header, kPageSize);   // 写 4096
```

读侧只读 `sizeof(Header)` = 100 B（`rom_esp.cpp:93`）。所以 `s_header` 只需 100 B，省 **3,996 B bss**；前提是把 261 行的写入长度改成 `sizeof(h)`——头扇区在 `rom_begin` 里已被擦成 0xFF（`dmc_link.cpp:229-233`），尾部残留 0xFF 与现在的 0x00 语义等价，因为没人读它。**风险：将来若有人对整扇区算 CRC/签名就会变**。列为待裁决。

对照：`s_page[4096]`（0x1000）是 PAGE 帧缓冲，必须留；`s_bitmap[256]`（0x100）是 present bitmap，必须留。

**P-19** `save_end()` 第 312 行 `s_rom_ok = s_rom_ok;` —— 自赋值废句，纯遗留。
**P-20** `#include "saveimg.h"`（`dmc_link.cpp:36`）在该 TU 内**零符号使用**（只有注释提到 saveimg），删。

### 2.9 `rom_esp.cpp` / `rom_esp.h`

- `s_index[2048]`（8,192 B bss）与 `SparseRom s_rom`（`s_rom` 0x4080 = 16,512 B bss）都在 map 里（14281-14283 行），**都是活的**：`s_index` 被 `set_mapped(..., s_index)` 直接交给 `SparseRom` 做页偏移查找（`rom_esp.cpp:134`），`s_rom` 里 16 KiB 是 4 槽 COW overlay（`emu/romstore.h` 的 `m_overlay[DIRTY_SLOTS * PAGE_WORDS]`）。
- 邻域可省但不在本轮文件清单：`SAVEIMG_MAX_PAGES = 2`（`saveimg.h:23`，注释明说游戏只写 2 个 4 KiB 扇区），而 `SparseRom::DIRTY_SLOTS = 4`（`romstore.h:35`）→ overlay 多留 2 槽 = **8,192 B bss**。逻辑上 saveimg 永远导不出超过 2 页，第 3/4 槽不可达。**建议单独核实后改 DIRTY_SLOTS=2**（属 emu/romstore.h，跨范围，此处仅记录）。
- **P-21** `s_image_size`（`rom_esp.cpp:40`）只被赋一次值（第 109 行），**从不读取**。4 B，删。
- **P-22** `rom_esp.cpp:117` `s_rom.begin(hdr.image_size, load_page, nullptr);` 之后，两个分支又各调一次 `s_rom.begin(...)`（第 133、144 行）。117 行是纯冗余（中间没有任何依赖它的操作），删掉即可；每挂载一次省一次 `begin()`。
- `hex20()`/`digits` 有引用（`hex20` 在 150 行），非死代码。

### 2.10 `partitions.csv`

| 分区 | 大小 | 使用情况 | 判决 |
|---|---|---|---|
| `nvs` 0x9000 | 24 KiB | wallclock 2 键 + menu 7 键（同 namespace "digicore"） | 偏大，NVS 最少 3 扇区 = 12 KiB（P-26） |
| `phy_init` 0xf000 | 4 KiB | **无任何代码引用**（`grep '"phy"'` 零命中；`esp_phy` 只链进 2 字节的 `include_esp_phy_override`） | 疑似空置（P-25） |
| `factory` 0x10000 | 2 MiB | 镜像 1,144,944 B = 54.6% | 余 0.9 MiB（无 OTA 双槽，属预留） |
| `roms` 0x210000 | 0x280000 = 2.5 MiB | 611 页 × 4 KiB + 头 ≈ 2,502,656 B（分区注释写 2,515,200 B） | 合理，容量 637 页 > 611 |
| `save` 0x490000 | 128 KiB | ring 60 + fbsave 32 + **洞 4 KiB** + saveimg 32 = 128 | 见下 |

- **`save` 分区内 4 KiB 空洞**：`fbsave` 0x0F000+0x8000 = 0x17000，`saveimg` 从 0x18000 起（`saveimg.cpp:37`），中间 0x17000-0x18000 无人使用。`gueststate.h:39` 自己写了"60 + 32 + 32 = 124 KiB plus a 4 KiB tail"——**已声明、可接受**，但注意 DMC1 传整块 128 KiB 时这 4 KiB 也上线（无害）。
- **P-27 表尾与芯片容量不符**：`partitions.csv:8` 写 "0x4b0000 end of flash"，但 `CONFIG_ESPTOOLPY_FLASHSIZE_8MB`（`sdkconfig.defaults:5`）。0x4b0000 = 4,915,200 B，**8 MiB 剩下 3,473,408 B（3.31 MiB）没有编址**。当前不需要它，但注释是错的，且 `roms` 只剩 106 KB 余量时本可扩。

### 2.11 `sdkconfig.defaults`

**P-28 `CONFIG_COMPILER_LTO=y`（第 31 行）没有生效——这是本轮最值钱的一条配置发现。**

```
sdkconfig.defaults:31:  CONFIG_COMPILER_LTO=y        (mtime 2026-11-26 18:08)
sdkconfig        (mtime 2026-11-24 01:29)  → grep 'CONFIG_COMPILER_LTO' 零命中
build/config/sdkconfig.h (mtime 2026-11-26 18:00) → grep 'LTO' 零命中
build/digicore-c3.bin    (mtime 2026-11-27 19:16)
```

`sdkconfig` 已存在时 ESP-IDF 以它为基，defaults 里新加的符号不会自动落到已有配置（`sdkconfig.h` 最后一次生成还早于 defaults 修改 8 分钟）。**结论：现在这版 1,144,944 B 的镜像是无 LTO 编出来的。** 要么执行一次真正的重配置把 LTO 打开（需 A/B：LTO 可能反而伤 aot 块与 ICache 命中率，`main/CMakeLists.txt:70-76` 的 `-Os` 就是为命中率调的），要么把这一行删掉，不要留一个"看起来开了其实没开"的开关。

**P-29 注释过期**：`sdkconfig.defaults:24` 写 "roms (6 MiB sparse ROM)"，实际 `0x280000` = **2.5 MiB**。

**配置里"为本项目用不到的功能打开"的选项——实测贡献 ≈ 0，别指望省：**
`build/config/sdkconfig.h` 里 `CONFIG_ESP_WIFI_ENABLED=1`、`CONFIG_ESP_COEX_ENABLED=1`、`CONFIG_ESP_TLS_*`、`CONFIG_ESP_HTTP_CLIENT_*`、`CONFIG_ESP_HTTPS_OTA_*` 全部为开；但 map 里 `esp_wifi` 相关 49 处、`netif` 7 处、`libpp` 6 处**全部落在 Discarded（地址 0x0）**，`libcoex` 0 处。因为没有任何代码引用 `esp_wifi_init`，链接器整库没拉进来。`libapp_update` 只留下 `esp_ota_get_running_partition`（被 `partition_target.c` 引用）这一条。**所以：sdkconfig 层面看着吓人，镜像里一分钱没花。** 唯一能落地的是 P-28（LTO）和 `LOG_MAXIMUM_LEVEL_DEBUG`（第 30 行）——后者在 `main/` 只有 3 处 `ESP_LOGD`，可忽略。

---

## 3. 静态数据总账（题目第 2 问）

| 资源 | 源码体积 | 实际 flash | RAM | 真的被用？ | 重复？ |
|---|---|---|---|---|---|
| `assets/internal_rom_221017a.bin` | 65,536 B 二进制 | **65,540 B**（`.rodata.embedded` 0x10004，map 22726） | 0 | 是（`gpce4.cpp:562` 服务 0x4000-0xFFFF） | 无 |
| `brick_a8.h` | 103,446 B 文本 | **16,384 B**（map 22456） | 0 | 是（`bezel.cpp:83-89`） | 无（但只需 2,048 B，P-04） |
| `font5x7.inc` | 2,201 B 文本 | **590 B = 295 × 2**（map 22444/22453） | 0 | 是（bezel 224/264、menu 349/389/573/673） | **是，两份**（P-05） |
| `ST7789P3_CMDS` | `display.c:49-65` | 260 B（`.rodata.ST7789P3_CMDS`） | 0 | 是（`display.c:173`） | 无 |
| `battery s_profile` | `battery.c:51` | 80 B | 0 | 是（`battery.c:106/119/126`） | 无 |
| `audio buf[1024]` + `buf[512]` | `audio.c:86,447` | 0 | **3,072 B** | 是 | **功能重复**（P-10，可省 1,024） |
| `dmc_link s_page/s_header/s_bitmap` | `dmc_link.cpp:89-91` | 0 | **8,448 B** | `s_page`/`s_bitmap` 是；`s_header` 只用前 100 B（P-18，可省 3,996） |
| `romstore s_index` + `s_rom` | `rom_esp.cpp:38,43` | 0 | **24,704 B** | 是（`s_index` 给 mmap 查表；`s_rom` 含 16 KiB overlay） | 邻域可省 8,192（§2.9） |

**"既在 flash 又复制到 RAM"检查：没有一处。** 所有 `static const` 都在 `.rodata`/`.rodata.embedded`（drom0，XIP），没有 `__attribute__((section(".data")))` 之类的 RAM 落地；`s_strip`（app_main，栈外静态）与 `s_frame_buf`（heap 73,728 B）是绘图缓冲，与本轮静态数据无关。

---

## 4. 运行期"白干活"总账（题目第 4 问）

先给结论：**外设侧没有每帧级的白干活。** 逐项核过的候选：

| 候选 | 真相 | 代价 |
|---|---|---|
| 砖墙平铺每帧重画 | **否**，`bezel_paint` 只在 6 个事件点调用（§2.2）；帧循环里只有电量角 | — |
| 圆角遮罩每次重算 | 是，但只随 `bezel_paint`（`row_span` 每行一次，240 行 × ≤30 迭代 ≈ 7k cyc，可忽略） | <0.05 ms |
| 字体渲染 | 每次现算位图（`bezel.cpp:224/264`、`menu.cpp:349...`），无字形缓存；但只在卡片/角标重绘时跑，644 px 级 | <0.5 ms/次 |
| 电量百分比 | `battery_soc()` 一次 I2C 读（~0.3 ms@100 kHz）**每 30 s**，且只在数值变化时推屏（`app_main.cpp:3606-3613`） | 0.01% |
| 时钟格式化 | `wallclock_hms` 里 2 次 64 位除法/取模（RV32 软实现），但被 `clock_sync` 的 250 ms 死区门限（`app_main.cpp:3036`）限到 ≤4 次/秒 | 可忽略 |
| 每帧重设同一寄存器 | **只有 1 处**：`audio_set_buzzer(buzzer_hz())`（P-12），含 1 次软除 + 1 次 timer 读 | ~250 cyc/帧 ≈ 0.03% |
| 每次 push 重建表 | 无；`ST7789P3_CMDS` 是静态表，只在 `display_init` 跑一遍 | — |
| 缓存了却不复用 | `fbsave`/`saveimg`/`gueststate` 的 CRC 每次都重算——但这是掉电安全的前提（§2.7），**不算白干** | — |

**唯一的"每次重绘都重算、结果恒定"的大件是 `bezel_paint` 本身。** 估算（标注：SPI 部分按 `app_main.cpp:404` 实测 39.8 Mbit/s 折算，计算部分为估算）：

- 像素计算：320×240 = 76,800 px，每 px 走 `shell_px`（玻璃判定 + 2 次 `inside_round` + 1~2 次圆角边判定 + `stone_px` 的 1~3 次查表），估 40-80 cyc/px → **3.1-6.1 M cyc ≈ 19-38 ms @160 MHz**
- SPI：15 个 band × 10,240 B = 153,600 B ÷ 39.8 Mbit/s ≈ **30.9 ms**（与 `app_main.cpp:2034` 注释的 "~30 ms of blocking SPI" 吻合）
- 合计 **≈ 50-70 ms/次**，每次开机 → 设置卡关闭 → 菜单关闭 → warp 卡消失 各一次，约 **5 次/上电**，合计 0.25-0.35 s

要不要预计算？320×240 RGB565 = 153,600 B，**DRAM 放不下**；128×128 砖块贴图打成 1 bpp 也要 2 KB 且省不下 `inside_round`。**结论：保持现算，别动**（见 §5）。

---

## 5. 不要动清单

| 对象 | 理由 |
|---|---|
| `assets/internal_rom_221017a.bin`（65,540 B） | 内部掩码 ROM 是功能数据，XIP 直读，无 RAM 副本，删了 guest 直接跑飞（§2.3） |
| `bezel.cpp` 几何与配色（`kStoneFace`…、`kCornerR=30`、rim/well 半径） | 全部对零售机照片采样/测出来的（`bezel.cpp:14-19,39-56`），改一个数就是外观回归 |
| `bezel.cpp:79-92` 的 1 px 斜面算法 | 注释说明"没有它单色红墙会读成马赛克"，是刻意的视觉处理 |
| `display_wait_idle` 里丢弃陈旧信号量的两次 take（`display.c:227`） | 二进制信号量语义必需，删了会把上次完成误判成本次 |
| `fbsave_load` 的两遍流式读、`saveimg/gueststate` 的只回读头部校验 | 掉电撕裂检测的唯一手段，DRAM 没有第二块缓冲（§2.7） |
| `save` 分区三段偏移（0x0/0xF000/0x18000）与 ring=5 | 三个模块 + 磨损分布已经互相引用的硬约定，动一个全崩 |
| `es8311_init` 的完整寄存器序列（`audio.c:169-239`） | 镜像厂商驱动，注释记录了"少写一条就静音"的实测；不要"精简" |
| `audio_task` 优先级 3 / 10.7 ms 块长 | 注释带实测数据（等优先级导致 DAC 饿死）；改了就哑 |
| `battery.c:51` 80 B 电池模型 | CW2017 厂商 profile，换板才换 |
| `dmc_link` 协议字符串与 `tools/dmc_web.html` 的对应关系 | 客户端已发布，改名即断（`docs/WEB_TOOL.md:39-47`） |
| `clock_sync` 的 250 ms 死区、`adopt_local`/`track_local` 分工 | 带硬件实测的收敛策略（`app_main.cpp:3004-3062`） |
| `gueststate_invalidate_newest()` | 唯一能构造"撕裂记录"的钩子，删了 fallback 路径无法测 |
| `s_frame_buf` 整窗异步推（`app_main.cpp:1108-1136`） | v1.15 实测：blitmax 1,980-2,462 µs → 290 µs，别回退成逐 band |
| `display_backlight_set` 无条件写 LEDC | 只在事件点调用，加"值没变就跳过"省不到任何东西，反而多一个状态机 |

---

## 6. 待裁决项（需要项目所有者拍板）

1. **砖墙 1 bpp 打包（省 14,336 B flash）** —— 收益最大，但破坏 `brick_a8.h:3-8` 的"与姊妹项目逐字节一致、可回拷"约定。若接受，需同时在 `digimon-passport` 侧生成同源 1 bpp 数据，否则两份又不一致。
2. **LTO 到底开不开（P-28）** —— 现在 defaults 写着 `=y` 而实际镜像没开。要么重配置 + 跑一轮 A/B（看 `mi_s`/帧时间/`aot_miss`），要么删掉该行。**这是本轮唯一可能带来体积级收益的开关，但也最可能反噬 aot 命中率。**
3. **`s_header` 4 KiB → 100 B（P-18，省 3,996 B bss）** —— 需接受"头扇区尾部是 0xFF 而非 0x00"；读取侧只读 100 B，安全，但会改变 flash 内容指纹。
4. **`phy_init` 分区是否删（4 KiB）** —— 建议先 `esptool read_flash 0xF000 0x1000` dump 证明全 0xFF，再删；若将来要开 WiFi/BT 必须加回。
5. **`nvs` 24 KiB → 12 KiB（P-26）** —— NVS 最少 3 扇区；现只有 9 个 byte-key，安全，但会重置已存配置（需接受一次"恢复默认"）。
6. **分区表尾部 3.31 MiB 未编址（P-27）** —— 是无意留白还是有意留白？若要给 `roms` 扩容（当前只剩 106 KB），这是唯一可用的地方；同时要修 `partitions.csv:8` 的错误注释。
7. **`LCD_PCLK_HZ` 80 MHz 与注释 40 MHz 的矛盾（P-02）** —— 降常量（诚实、且与实测 39.8 Mbit/s 一致）还是删注释（保留"驱动会钳"的现状）？顺带补回 P-03 的超时错误码。
8. **`rot_map` / 旋转功能去留（P-24）** —— `kRot=0` 是编译期常量，5 个旋转分支 + 唯一调用点全被编译器消掉。要么删（连带 `rot_map`），要么改成 `if constexpr` 保留未来面板换向的能力。
9. **`nvs_flash_init()` 上提（P-15）** —— 改 `app_main` 一处，消掉 wallclock→menu 的隐式顺序依赖。
10. **每帧 `audio_set_buzzer` 去留（P-12）** —— 删则需在快照恢复路径补一次显式发布；留则接受 0.03% CPU。

---

## 7. 复核用的原始命令（可复现）

```powershell
# 1) 引用普查（示例：死导出）
Select-String -Path main\*.c,main\*.cpp,main\*.h -Pattern 'display_flush|audio_silence|audio_buzzer_active|dmclink_rom_ok|dmclink_waiting_for_rom|fbsave_init|battery_mv|wallclock_local_sec'

# 2) 目标文件符号尺寸
& $env:ESPRESSIF_RISCV_NM -S build\esp-idf\main\CMakeFiles\__idf_main.dir\bezel.cpp.obj

# 3) 死段证据（Discarded input sections 区，map 第 858-9124 行）
Select-String -Path build\digicore-c3.map -Pattern 'display_flush|audio_silence|dmclink_rom_ok'

# 4) 砖墙取值分布
(Get-Content main\brick_a8.h -Raw | Select-String -AllMatches '0x([0-9A-Fa-f]{2})').Matches |
  ForEach-Object { $_.Groups[1].Value } | Group-Object | Sort-Object Count -Descending
```

`riscv32-esp-elf-nm` 路径：`C:\Users\DuWeisi\.espressif\tools\riscv32-esp-elf\esp-14.2.0_20251107\riscv32-esp-elf\bin\riscv32-esp-elf-nm.exe`
