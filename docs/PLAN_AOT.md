# PLAN_AOT — 把 unSP 代码静态重编译到 C（交接用可执行计划）

> 本文档是给**新对话**的施工图。读完后不需要回看历史对话即可开工。
> 现状小抄：设备（ESP32-C3 / COM4）静止 **58.4–58.9 fps**，但换屏/按键响应 **300 ms 级**，
> 原因是换屏那一秒每帧要**解释执行 162,000–270,000 条 guest 指令**（静止时只有 3,700 条），
> 而每条解释指令约 **200 个主机周期**。AOT 的目标是把这 200 周期压到 2 周期量级。

---

## 1. 目标与验收标准

| 指标 | 现在 | 目标 | 验收手法 |
|---|---|---|---|
| 换屏/按键响应 | 300 ms 级（fps 掉到 3） | **< 20 ms**（fps 不掉下 55） | 按一次键，串口 `fps=` 序列不出现 < 55 |
| 静止帧率 | 58.4–58.9 | 60.00（节拍锁死） | 串口 `guest_fps=60.00` 连续 60 s |
| 指令级正确性 | 100% | **保持 100%** | `tools/emu_validate.py`（必须现场生成 trace，见 §6） |
| 整片存档 | 0 字节差 | **0 字节差** | `--save-flash` 两路对比 |
| 帧缓冲 | 0/9216 | **0/9216** | 设备第 60 帧 vs PC 参考帧 |

**不做的事**：不改 guest 语义、不做 HLE 猜测、不在没验证的情况下上机。

---

## 2. 现状与已排除的路线

### 2.1 已实现并实测有效的（保留，别推倒）

| 手段 | 收益 | 位置 |
|---|---|---|
| PRNG 块原生执行（68 条指令） | 1.09 → 7.82 fps | `emu/hotblock.h`（由 `tools/gen_hotblock.py` 从 `recomp/hot_func.c` 生成） |
| 等待循环折叠（0x230321 / 0x5F35 / 0x5251 / 0x5376） | 7.69 → 14.15 fps | `unsp_glue.cpp::wait_loop_collapse()` / `spin_collapse()` |
| blit 重写（只发窗口 + 脏行跳过 + 32 位写） | 14.42 → 24.5 fps | `app_main.cpp::blit_frame()`，`tools/patch_blit.py` |
| 切片 48,000 cycles + 关 300 kHz SPI2 源 + 屏蔽中断源不排事件 | 24.5 → 51.2 fps | `Gpce4::cycles_to_next_event()` / `source_enabled()` |
| 绝对截止时间节拍 | → 60.00 fps | `app_main.cpp` 的 `s_deadline_us` |
| 面板清黑 + 1:1 正方形窗口 + fps 浮层 | 画面正确 | `tools/patch_panel_fill_fix.py` / `patch_unify_scale.py` / `patch_fps_overlay.py` |

### 2.2 已试过、**实测零收益或负收益**（不要再试）

- 把 ISA 合并成单个 TU（`emu/isa/unsp_amalgam.cpp`）→ 41.95 vs 41.95 fps
- 打开 `-fjump-tables -ftree-switch-conversion` → 0
- 取指窗口缓存替代虚函数 `read16` → 0（PC 上更慢）
- 常见 ALU 指令内联 `fast_step()` → 负收益（6.86 → 6.60）
- `mmap` 直接映射 flash 分区 → 0
- 热函数塞 IRAM（`.iram1.text`，9 条函数）→ 静止被节拍锁住看不出，换屏 9.6 → 11.3（≈17%，留下但非关键）
- `-flto` → IDF 5.5.3 的 configure 阶段直接失败

---

## 3. 架构决策（已定，不要再讨论）

1. **unSP → C → gcc -O3 → RISC-V**。不直接发射 RV32 机器码（寄存器分配/验证成本不值那 0.3 周期差）。
2. **一个基本块一个 C 函数**，块内直线指令顺序展开；块尾按控制转移类型收尾：
   - 条件跳转 → `if (cond) { c->pc = T; return; } c->pc = F; return;`
   - 无条件/`goto` → `c->pc = T; return;`
   - `call` → 直接用 C 函数调用目标块，返回点地址写进 C 的局部返回栈（见 §4.4）
   - `ret` / `reti` / 未知 → `return` 给调度器（或走回退）
3. **调度器**：`aot_run(uint32_t lpc)`，两级查表
   `page = (lpc >> 8) & 0x3ff` → `uint8_t kind = page_kind[page]`；命中则 `block_fn[lpc & 0xff]()`。
   未命中 → `aot_fallback()`（直接调现有 `execute_one()` + `add_lpc()`）。
4. **周期计费**：每条指令编译期写死**实测众数周期**（数据来源见 §6.2），块尾一次性 `c->icount -= N`。
   中断只在块边界检查（**必须**：现在就是块边界投递，改了会破坏与 MAME 的对拍）。
5. **回退保底**：发射器没覆盖的指令一律回退到解释器，性能按比例下降但**语义永远正确**。
   这样第一天就能上机，覆盖度慢慢加。
6. **不碰**：不改 `Gpce4`（定时器/LCD/SPI 已验证）、不改 `wait_loop_collapse`（已验证 0 字节差）、
   不改 `blit_frame`（已 60 fps）。

---

## 4. 指令发射器

### 4.1 权威来源（按优先级）

1. `ref/unsp/unsp*.cpp`（MAME 0.289，**唯一权威语义**）
2. `recomp/model.py`（Python 语义参考，已与 MAME 对拍过）
3. `recomp/unsp_state.h`（C 版 `op_*` 助手，已被真机 golden 验证）
4. `emu/isa/*.cpp`（现有解释器，可直接抄实现）

### 4.2 覆盖优先级（按动态频次，数据来自 `dbgtest/pc_hist_10s.txt` 与指令形式直方图）

先实现这 30 种，动态覆盖 ≈ 99%：

```
rX = imm6 / rX += imm6 / rX -= imm6 / rX cmp imm6 / rX &= imm6 / rX |= imm6 / rX ^= imm6
rX = [bp+imm6] / [bp+imm6] = rX / rX = [imm16] / [imm16] = rX
ds = imm6 / ds = rX / rX = ds:[rX] / ds:[rX++] / ds:[rX] = rY / ds:[rX++] = rY
rX = rY / rX += rY / rX &= rY / rX |= rY / rX ^= rY / rX cmp rY
push rN, rM / pop rN, rM / rX lsr imm / rX lslor rX / rX asr imm / rX ror imm
call imm16 / goto imm16 / jcc imm6 / jmp imm6 / ret / reti / nop / int / brk
```

### 4.3 发射器接口约定（新对话按这个写 `tools/aot.py`）

```python
def emit_insn(img, pc) -> tuple[list[str], int] | None
    """返回 (C 代码行, 消耗的 word 数)；None 表示不覆盖 → 上层生成 aot_fallback 调用。"""
```

生成的 C 用统一前缀避免与现有符号冲突：`aot_b_00480f(Cpu *c)`、辅助宏前缀 `AOT_`。

### 4.4 call/ret 的处理（最容易写错的地方）

这台 CPU 的 `call` **只压返回地址，不压 SR**（我们在这上面踩过坑：按压 PC+SR 实现，SP 每圈漂一格，
四万圈后栈被冲穿，`PC=004020 SR=8280 SP=C099`）。发射器必须与解释器**逐字一致**：

- `call target`：`push(返回地址)` → `c->pc = target` → `return`
- `ret`：`pop` → `c->pc = 目的地址` → `return`
- 不做 C 层递归调用（栈深不可控，且 guest 的栈语义要一致）

### 4.5 已知的坑（写翻译器前必读）

| 坑 | 现象 | 正确做法 |
|---|---|---|
| PC 是 16 位 + SR[0..5] 存高 6 位 | 直接写 `m_r[REG_PC]` 会拼出错误 LPC | 一律用 `set_pc_lpc(uint32_t)` |
| 跳转偏移 | 写成 `uint16_t` 让后退跳变前进 65531 | 用 `int32_t` |
| `op0 == 0x0e/0x0f` 是独立译码组 | 落进通用 ALU 分支会解错 | 先判这两组 |
| `opa == 7` 是跳转组 | 同上 | 通用 ALU 要求 `opa != 7` |
| `pop` 覆盖寄存器 7 | 等价于 `ret` | 发射成 `ret` 语义 |
| 标志位 | `N=0x0200 Z=0x0100 S=0x0080 C=0x0040`，S 由 `BIT(value,16) != BIT(r0^r1,15)` 决定 | 直接复用 `recomp/unsp_state.h::unsp_update_nzsc` |

---

## 5. 分步任务（每步都能独立验收，不许跳步）

### T1 — 生成器骨架（半天）
- 写 `tools/aot.py`：读 `tools/coverage/out/AB_blocks.txt`（762 个块入口）+ 两个 ROM 镜像，
  用 `tools/coverage/unsp_static.py::Images/decode` 译码，发射 C。
- 先只实现 5 种指令（`rX=imm6`、`rX=[bp+imm6]`、`[bp+imm6]=rX`、`jcc`、`goto`），其余全走回退。
- 输出 `emu/aot_blocks.c` + `emu/aot_blocks.h`（块表 + 函数声明）。
- **验收**：PC 侧 `tools/emu_validate.py` 对 0–20 ms 窗口仍 **682,005/682,005 = 100%**，
  并且 `--save-flash` 与 `GPCE4_FASTWAIT=0` 参考跑 **0 字节差**。

### T2 — 覆盖到 95% 动态指令（1–2 天）
- 按 §4.2 顺序补发射器，每补一批跑一次 T1 的两个验收 + 900 ms / 5 s / 20 s 三个窗口。
- 统计手段：在 `aot_fallback()` 里累加 `m_aot_fallback_count` 与按 PC 计数，
  `--pc-hist` 已能给出「还有哪些 PC 走回退」。
- **验收**：回退率 < 5%（按动态指令数），四个窗口全部 100%。

### T3 — 全 762 块 + 设备接入（1 天）
- 用 `tools/sync_c3_emu.py` 把 `emu/` 的 AOT 文件同步进 `firmware/digicore-c3/main/emu/`
  （按现有 `port_*.py` 的风格写一个 `port_aot.py`，锚点缺失就 `sys.exit`）。
- `main/CMakeLists.txt` 把 `aot_blocks.c` 加入 SRCS；先全放 flash（XIP），量一次 fps。
- **验收**：设备 `guest_fps=60.00` 且**按键后不再掉到 3 fps**（目标 ≥55）；第 60 帧 0/9216。

### T4 — 热块进 IRAM（半天）
- 用 `--block-hist` 的排名取前 N 个块（估计 50 KB 够），加 `IRAM_ATTR`；
  剩余 DRAM 约 100 KB，别一次塞满。
- **验收**：换屏响应再降 20% 以上，或明确记录「无收益」并回退。

### T5 — 中断例程原生化（可选，半天）
- 先量：每秒 2 kHz × 每帧约 33 次 IRQ6，处理函数当前消耗多少指令/周期（用 `--pc-hist` 过滤 0x485D 之后的范围）。
- 若占比 > 10%，把 IRQ 入口那几块也纳入 AOT（它们本来就是普通块，可能已被覆盖）。
- **验收**：静止帧率不变或更高，中断计数与 MAME 对拍一致。

### T6 — 模式替换（可选，最后做，必须可开关）
- 已识别的模式：`0x207CA3..0x207CC4` = 带透明色键（`0x07E0` 纯绿跳过）的位图拷贝。
- 发射器识别该模式 → 调 `aot_blit_key(dst_page, src_page, w, h, key)` 的 C 实现。
- **必须**做成 `#if AOT_HLE_BLIT` 开关，默认关；开的时候用帧缓冲逐像素 + 存档逐字节双重验证。
- **验收**：换屏再降 25%，像素 0/9216 不变。

---

## 6. 验证手法（命令级，照抄）

### 6.1 逐指令流对拍（**必须现场生成 trace**）

```powershell
cd C:\Users\DuWeisi\digimon-color-passport
.\emu\digicore.exe --frames 2 --trace dbgtest\emu_now.tr --trace-seconds 0.02
C:\Users\DuWeisi\py311\python.exe tools\emu_validate.py dbgtest\cov\A\000_t00000.tr dbgtest\emu_now.tr
# 期望：matched 682005 / 682005 (100.000%)，并打印 skip 条数
```
窗口：`000_t00000`（0–20 ms）、`045_t00900`（900 ms）、`052_t05000`（5 s）、`067_t20000`（20 s），
对应生成 trace 时用 `--trace-start 0.9 / 5.0 / 20.0 --trace-seconds 0.004`。

> **教训**：旧 trace 文件（改动前生成）会把已修好的流报成 12.4%。每次都要现生成。

### 6.2 逐条周期对拍

```powershell
.\emu\digicore.exe --frames 2 --trace-cycles dbgtest\emu_cyc.tr --trace-seconds 0.02
C:\Users\DuWeisi\py311\python.exe tools\cyc_diff.py dbgtest\mame_cyc.tr dbgtest\emu_cyc.tr
# 期望：0 处差异（MAME 侧 trace 动作在执行前触发，所以 charge[i]=cyc[i+1]-cyc[i]）
```
实测众数周期可从 `dbgtest/cyc900.txt`（27,649,615 行 `PC CHARGE`）统计，或用
`tools/block_rank.py`。

### 6.3 存档与帧

```powershell
.\emu\digicore.exe --seconds 20 --autoplay --save-flash dbgtest\aot_flash.bin
$env:GPCE4_FASTWAIT='0'; .\emu\digicore.exe --seconds 20 --autoplay --save-flash dbgtest\ref_flash.bin
C:\Users\DuWeisi\py311\python.exe -c "a=open('dbgtest/aot_flash.bin','rb').read();b=open('dbgtest/ref_flash.bin','rb').read();print(sum(1 for x,y in zip(a,b) if x!=y))"
# 期望 0
```

### 6.4 设备侧

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\c3_build.ps1
python -m esptool --chip esp32c3 -p COM4 -b 460800 write_flash --flash_mode dio --flash_freq 80m --flash_size 8MB 0x10000 firmware\digicore-c3\build\digicore-c3.bin
& "C:\Users\DuWeisi\.espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe" tools\c3_serial_capture.py COM4 dbgtest\aot_dev.log 30
C:\Users\DuWeisi\py311\python.exe tools\c3_shot_all.py dbgtest\aot_dev.log dbgtest\aot_dev_block
```
帧比对基准：`dbgtest/fb_f60.bin`（PC `--frames 60 --shot-raw` 生成，裁 `[18..113][0..95]`）。
**不要**用 `docs/c3_device_frame60.png` 当基准（旧产物，会得到 9,052 像素差的假阳性）。

---

## 7. 环境与工具清单

| 用途 | 路径 |
|---|---|
| 项目根 | `C:\Users\DuWeisi\digimon-color-passport` |
| 工具链装载 | `. "$env:USERPROFILE\passport-toolkit\env.ps1"` |
| 设备串口 | **COM4**（COM6 是 Cardputer ADV，禁止刷） |
| Python | `C:\Users\DuWeisi\py311\python.exe`（**无 pyserial**；串口用 IDF 的 python） |
| IDF python | `C:\Users\DuWeisi\.espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe` |
| PC 构建 | `powershell -NoProfile -ExecutionPolicy Bypass -File emu\build.ps1` |
| 设备构建 | `powershell -NoProfile -ExecutionPolicy Bypass -File tools\c3_build.ps1` |
| 块清单 | `tools/coverage/out/AB_blocks.txt`（762 行）、`AB_pcs.txt`（4,788 行） |
| 现有原生块 | `emu/hotblock.h` + `tools/gen_hotblock.py` |
| 语义助手 | `recomp/unsp_state.h`、`recomp/model.py` |
| 反汇编器 | `tools/coverage/unsp_static.py`（`decode(img, pc) -> (len, kind, target, op)`） |
| 备份 | `docs/backup/digimon-color-passport-COM4-8MB-20261120-1218.bin`（回滚用，未覆盖） |

---

## 8. 新对话的开场动作（按顺序执行）

1. 读 `docs/HANDOFF.md` 顶部三条 → 本文件 → `docs/C3_PORT.md` §8 → `docs/EMU.md`。
2. 跑一次基线回归，确认起点没坏：
   ```powershell
   .\emu\digicore.exe --frames 2 --trace dbgtest\base.tr --trace-seconds 0.02
   C:\Users\DuWeisi\py311\python.exe tools\emu_validate.py dbgtest\cov\A\000_t00000.tr dbgtest\base.tr
   ```
   期望 100%。若不是 100%，先修环境，别动 AOT。
3. 确认设备在线：`tools\c3_serial_capture.py COM4 dbgtest\probe.log 8`，看到 `guest_fps=` 行即可。
4. 从 **T1** 开始，一次只推进一步，每步跑完 §6 的验收再进下一步。

---

## 9. 风险与止损

- **最大风险**：翻译器语义偏差导致偶发跑飞（不一定立刻暴露）。止损手段是 §6.1/§6.2/§6.3 三重对拍，
  每步必跑；一旦出现非 0 差异，先回退到上一个可用提交点（保留 `aot_fallback` 全量模式即可）。
- **次大风险**：代码体积超预期（flash XIP 变慢）。止损：先全 XIP 量基线，再按 `--block-hist` 挑热块进 IRAM。
- **不要**在没有对拍的情况下「顺手优化」`Gpce4`、`blit_frame`、`wait_loop_collapse` —— 这三处都是靠对拍抠出来的。
