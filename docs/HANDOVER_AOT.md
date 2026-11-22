# AOT 重编译项目交接（2026-11-21 20:30）

写给接手的新对话。目标、现状、验证方法、踩过的坑，按接手顺序排列。

---

## 0. 一句话现状

**PC 侧模拟器与 MAME 参考逐指令 100% 恒等（682,005/682,005）。设备侧真实场景 58 fps，但按键延迟与人眼观感仍未让用户满意，且最后两小时的显示改动引入过画面崩溃，已回滚。**

设备固件当前是**可用状态**，实测 `mean 58.02 fps / gmax 12.4 ms / blitmax 1.9 ms`。

---

## 1. 项目目标（用户原话，m00001 / m01749 / m02375）

- m00001：「项目继续进行。读 docs/PLAN_AOT.md，按 §8 的开场动作执行，从 T1 开始。**基线回归必须是 682,005/682,005 = 100%**。」
- m01749：「**静态画面中所谓的60帧是没有意义的**，不论技术路线是什么，最后的结果一定是要能够流畅运行。」
- m02375：「按键延迟依然存在，按下后，迟滞接近1秒屏幕才会有对应反馈。放置一段时间后，画面冻结，多次按键后等待大约5秒，全屏刷新显示后才恢复动态。大面积的动画移动速度非常慢，数码兽移动的时候左右错位依然存在。你说的60fps，只在画面静止时存在，这毫无意义，在按键操作和数码兽移动的时候，fps只有6到20。继续优化。」

约束（不可破）：guest 语义不变、整片存档 0 字节差、帧缓冲 0/9216、HLE 可开关。

---

## 2. 架构

```
tools/aot.py            ← 生成器：读 ROM 镜像 + 块入口清单，发射 C
      ↓
emu/aot_blocks.c        ← 生成物：每个 unSP 基本块一个 C 函数
emu/aot.h               ← 运行时：AotCtx / AotPage / aot_lookup / 发射宏
emu/unsp_glue.cpp       ← 分发点：aot_step / hle_blit_key / hot_prng_n / wait_loop_collapse
      ↓ tools/sync_c3_emu.py（重跑生成器并整份覆盖设备副本）
firmware/digicore-c3/main/emu/
```

**为什么是 C 而不是手写汇编**：生成器用 C 表达，靠 GCC 优化；比手写 emit 快得多，且跨平台（PC 侧同一份代码跑对拍）。

**关键设计决定**

| 决定 | 理由 |
|---|---|
| `AotCtx.r` 是 `uint32_t *` 指向 `m_core->m_r` | 零拷贝，块进出不搬 16 个寄存器 |
| `AotCtx` 全局常驻（`g_aot_ctx`） | 每条 dispatch 只刷 7 个易变字段 |
| 块**整块跑完**，不在块中途退出 | 中途退出会让 PC 落在块中间 → 查表 miss 暴涨（实测 8.4M → 71M） |
| 块内每条指令后测 `c->icount < 0` + `*c->sirq` | 与解释器 run loop 的退出点对齐，否则周期数偏 → 中断落点偏 → 长跑分叉 |
| 块表在设备侧是 flash 常量 + 二分查找 + 64 项页缓存 | C3 的 SRAM 装不下 80 页 × 1,792 B |
| 生成器做贪心去重（`covered` 集） | 不做会生成 22.9 MB C / 73,086 条指令；做了是 3.7 MB / 9,351 条 |

---

## 3. 验证方法（必须现场生成 trace，不要用旧文件）

### 3.1 四个对齐窗口（PLAN_AOT.md §6.1）

```powershell
# 20 ms
.\emu\digicore.exe --frames 2 --trace dbgtest\a.tr --trace-seconds 0.02
C:\Users\DuWeisi\py311\python.exe -u tools\emu_validate.py dbgtest\cov\A\000_t00000.tr dbgtest\a.tr
# 900 ms / 5 s / 20 s
.\emu\digicore.exe --seconds 22 --trace dbgtest\b.tr --trace-start 0.9 --trace-seconds 0.004
C:\Users\DuWeisi\py311\python.exe -u tools\emu_validate.py dbgtest\cov\A\045_t00900.tr dbgtest\b.tr
# 052_t05000.tr 配 --trace-start 5.0；067_t20000.tr 配 --trace-start 20.0
```

期望：`682005 / 682005`、`95260 / 95260`、`95251 / 95251`、`93343 / 93343`，全部 `PASS: identical stream`。
`emu_validate.py` 跑一次约 4 分钟，**必须后台或 timeout ≥ 300 s**。

### 3.2 逐字节恒等（比 validate 更快，先跑这个）

```powershell
.\emu\digicore.exe --frames 1 --trace dbgtest\f.tr
# SHA256 应为 C3A980DDB662779422A1B587A2F336F32E283B36FC51A7D04D3687A56EFEDBF9（562,126 条）
.\emu\digicore.exe --seconds 20 --autoplay --save-flash dbgtest\fl.bin
# SHA256 应为 6872AEBEC2A79E2155E664FDB15210012EF5F7D35265DBA5DFE7A6A193B60760
.\emu\digicore.exe --seconds 20 --autoplay --shot dbgtest\sh.bmp
# SHA256 应为 B22302E9FBAA7EC58E4B583B2A1AEBE79101FF7D4B33262C8B96A3C9906D192F
```

### 3.3 周期对拍

```powershell
.\emu\digicore.exe --frames 2 --trace-cycles dbgtest\c.tr   # 输出 1,161,005 行，截前 682,009 行
C:\Users\DuWeisi\py311\python.exe tools\cyc_diff.py dbgtest\mame_cyc.tr dbgtest\c_cut.tr
# 期望 compared 682005 instructions, 0 cycle mismatches
```

### 3.4 设备

```powershell
# 生成 + 同步
C:\Users\DuWeisi\py311\python.exe tools\sync_c3_emu.py
# 构建（必须同一个 pwsh 调用里）
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force
. "$env:USERPROFILE\passport-toolkit\env.ps1" | Out-Null
idf.py --project-dir firmware\digicore-c3 build
# 整片 + 刷写
$py = "C:\Users\DuWeisi\.espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe"
$b = "firmware\digicore-c3\build"
& $py -m esptool --chip esp32c3 merge_bin -o "$b\digicore-c3-full.bin" --flash_mode dio --flash_freq 80m --flash_size 8MB 0x0 "$b\bootloader\bootloader.bin" 0x8000 "$b\partition_table\partition-table.bin" 0x10000 "$b\digicore-c3.bin"
& $py -m esptool --chip esp32c3 -p COM4 -b 921600 write_flash --flash_mode dio --flash_freq 80m --flash_size 8MB 0x0 "$b\digicore-c3-full.bin"
# 抓日志（脚本已加 reset_input_buffer，不加会读到上次残留）
& $py tools\c3_serial_capture.py COM4 dbgtest\x.log 60
& $py dbgtest\_fw7.py dbgtest/x.log
```

**串口只读可用，PC→设备写入超时**（`SerialTimeoutException`）——这块载板只接了 UART TX，命令控制台代码保留但不可用。

---

## 4. 关键常量与文件

`firmware/digicore-c3/main/app_main.cpp`：
- `kWinX=0`、`kWinY=14`、`kWinW=96`、`kWinH=96`、`kScale=2`、`kChunkRows=16`、`kFbPanel=128`
- `kSrcRowStride = kRowBytes * kScale`（**384**）
- `kFrameCycles=1,600,000`、`kSliceCycles=48000`、`kFrameBudgetUs=200000`、`kPressFrames=2`
- `C3_AUTOWALK 0`、`C3_SELFTEST 0`、`C3_DEBUG_DUMP 0`、`C3_MISS_CENSUS 0`、`C3_BLOCK_CENSUS 0`

`tools/aot.py` 环境变量：`AOT_BLOCKS`（762）、`AOT_EXTRA`（`AB_dev.txt` 5,067）、`AOT_FORCE`（`AB_force.txt` 691）、`AOT_IRAM_LIST`（`aot_iram_dev60.txt`）、`AOT_OUT`、`AOT_CONST_TABLES`、`AOT_NO_HOOKS`、`AOT_LOOSE`、`AOT_NO_EXTRA`。

`emu/gpce4.h:29`：`uint16_t fb[128*128]` —— **guest 帧缓冲 128×128，真实内容区 96×96（y=14..109）**。

---

## 5. 踩过的坑（每条都花过时间，别再踩）

### AOT 语义

1. **块运行期间 r7 停在块入口**。任何提前退出（写 SR、sirq 检查、icount 检查）都必须显式 `aot_set_pc(c, next)` 或写 `c->r[7]`，否则 PC 回退成死循环。
2. **fallback 必须把寄存器回传到 `ac`**。`aot_fallback_one` 执行完那条指令后，要把 `m_core->m_r/m_sb/m_icount` 拉回 `c->r/c->sb/c->icount`，否则 aot_step 出口会用它覆盖掉解释器的结果。
3. **fallback 前先 `set_pc_lpc(lpc)`**。`m_core->m_r[7]` 里是块入口 PC，`read16/add_lpc` 基于它算会导致后续 PC 全错。
4. **`AOT_NO_WRITE` 哨兵必须是 `0x80000000u`**。用 `0x10000u` 会撞车：`sub r5, 1` 在 r5==1 时结果正好是 0x10000，写回被丢弃。
5. **`AOT_CYC` 自己扣 icount**。任何再写 `c->icount -= cyc` 的地方都是双扣（曾导致整帧只有 391,539 条而不是 562,126）。
6. **必须以 `emu/isa/` 为准，不是 `ref/unsp/`**。`unsp_20_device` 继承 `unsp_12_device`，后者覆盖了 `execute_exxx_group`、`execute_fxxx_101_group`、`execute_divq`。`execute_fxxx_101_group` 在 `unsp_12_device` 里是 **`-= 1` 然后 default 分支再 `-= 2`，合计 3 周期**，且 case 表里**没有 0xf165**。按基类的 2 周期写会偏 19 个 PC。
7. **block census 的哈希必须和表容量匹配**。`g_miss_pc[512]` 配 `>> 22`（10 位，0..1023）会越界写 4 KB → 设备 22 次重启（`MEPC 0x000055ac / MCAUSE 0x1`）。改 `>> 23`。
8. **`g_verify` 重放会重复计数**。`sync_c3_emu.py` 给 `execute_one(op);` 插计数时，要排除前 120 字符里含 `*core = save;` 的那一处。

### 显示

9. **2x 垂直放大的行间距必须是 `kSrcRowStride`，不是 `kRowBytes`**。写错的话每个奇数面板行的右半截永远不被写入 → 精灵移动时"左右错位"。
10. **`s_prev` 只能在传输成功后更新**。原来在 `display_draw_bitmap()` 之前就置 `s_have_prev = true`，一次失败就永久失步 → 画面冻结到下次内容变化。
11. **这块 ST7789P3 没有 vsync**。任何"每帧都写面板"的方案（滚动巡检带、每帧全刷）都会让整个画面持续微闪。静止时应该**一个字节都不发**。
12. **`kChunkRows` 增大会 DRAM 溢出**。24 行时 `s_strip` = 18,432 B，`region dram0_0_seg overflowed by 896 bytes`。省 DRAM 要先关普查表（`g_miss_pc/g_miss_cnt/g_blk_pc/g_blk_cnt` 各 4 KB）。
13. **`-Os` 把 app 从 1,193,168 B 降到 900,912 B，但单帧峰值从 114 ms 涨到 217 ms**（用户反馈"更严重"）。体积换不来延迟。

### 帧循环

14. **帧预算会拉长帧，而按键每帧只采样一次** —— 加预算 = 直接加按键延迟。用户明确骂过这一点。
15. **跳帧（`s_cycle = frame_end`）会让游戏变速**。20 ms 预算 + 48K slice 时每帧只跑 17% 周期，计数器显示 45 fps，实际是六分之一速慢放。**必须核对 `cyc/帧 == 1,600,000`**。
16. **AUTOWALK 是调试钩子，必须默认 0**。开着它用户会看到"没按键但状态图标循环点亮、自己进子界面"，而且会把所有性能测量污染成极端负载。

---

## 6. 已知无解 / 已排除的方向（都有实测数据）

| 方向 | 结果 |
|---|---|
| 扩块表（1,323 → 1,466 块） | mean 34.87 → 34.87，miss 降 1.6%，fps 不动 |
| `MIN_NATIVE_INSNS` 1→4（短块交解释器） | 35.07，没变 |
| `-Os` 缩代码 24.5% | +0.6 fps，峰值反而更差 |
| 强制补 top-11 miss PC | miss 34.1% → 32.5%，fps 不动 |
| 内部 `for(;;)` 连续跑块（减 dispatch） | **21.56 fps**（饿死外层 `fasthot`/`wait_loop_collapse`，`collapse` 从 313,710 掉到 37） |
| `esp_timer` TASK dispatch 做 pacing | 58.92 fps，比 vTaskDelay 自旋差 |
| JIT | [ESP32 Emu Turbo](https://pjcau.github.io/esp32-emu-turbo/docs/next-steps/jit-plan) 的 68000 JIT「exact and **switched off — slower than the interpreter**」，与我们 AOT 走 flash 的结果一致。C3 的 IRAM 只剩 57 KB，也放不下 JIT 代码缓存 |

**根因算术**：C3 @160 MHz 模拟 unSP @96 MHz，1 个模拟周期只有 1.67 个 CPU 周期。实测每模拟周期花 27 个 CPU 周期，要 60 fps 需要压到 9.6。AOT 块平均 8.6 条指令、每块 dispatch 固定 880 周期（占 guest 时间 57%）。

---

## 7. 下一步建议（按性价比）

1. **先让用户确认画面是否恢复**（当前 `kChunkRows=16` + 120 帧整屏刷新，`blits=1`/秒）。
2. **超块（superblock）**：沿唯一后继把相邻基本块串成一个函数，把 1,826 次块调用/帧降到 400–500。这是唯一还能在 C3 上拿到实质提升的软件方向，理论 50 fps。要改 `tools/aot.py` 的 `emit_body`，加块内标签和 `goto`。
3. **分辨率**：现状 96×96 最近邻 2x。用户觉得"像分辨率降低了"。备选 1x 居中（原生像素、最清晰、只占 1/9 屏）。双线性 C3 撑不住。
4. **按键延迟**：已到 2 帧确认（~34 ms）。若用户仍觉得慢，问题在 guest 内部 UI 状态机，需要具体现象描述才能查。
5. **硬件**：C3 是 QFN32 焊死，S3 是 QFN56，脚位不兼容。**换芯片必须重新流片**，先问清板子是自绘还是量产。

---

## 8. 当前产物

| 文件 | 大小 | 说明 |
|---|---|---|
| `firmware/digicore-c3/build/digicore-c3.bin` | 1,194,736 B | app（43% 分区余量） |
| `firmware/digicore-c3/build/digicore-c3-full.bin` | 1,259,760 B | 整片，已刷 COM4 |
| `firmware/digicore-c3/main/emu/aot_blocks.c` | ~3.8 MB | 设备版生成物（1,466 块 / 13,788 指令） |
| `emu/aot_blocks.c` | ~3.7 MB | PC 版生成物（1,097 块 / 9,915 指令） |
| `emu/aot.h` | 9,868 B | 运行时 |
| `emu/unsp_glue.cpp` | 27,133 B | 分发 + HLE |
| `tools/aot.py` | 31,549 B | 生成器 |
| `tools/sync_c3_emu.py` | 5,122 B | 同步 |
| `docs/PLAN_AOT.md` | — | 原始计划（§3 架构 / §4 移植 / §5 T1-T6 / §6 验收 / §8 环境） |

设备侧备份：`docs/backup/digimon-color-passport-COM4-8MB-20261120-1218.bin`；最近一次完整备份 `dbgtest/flash_before_aot.bin`（8,388,608 B）。
