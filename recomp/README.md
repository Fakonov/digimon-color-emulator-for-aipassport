# recomp — unSP 静态重编译 PoC v0

这是 Digimon Color（digicolr）PC 侧静态重编译的第一个可验证切片。
不是固件，不接 LCD/SPI/中断。真机测试固件在 `firmware/recomp-bench`，端口 **COM4**（COM6 是 Cardputer ADV）。

## 做了什么

从 MAME 的 `dasm` 输出出发，把两段真实 guest 代码降到 C：

| 单元 | guest 范围 | 指令数 | 控制流 | 用途 |
|---|---|---|---|---|
| `hot_func` | `0x2002F4..0x200343` | 68 | 直线 | 32-bit 状态 hash/混合，trace 里最热的函数 |
| `delay_loop` | `0x5376..0x537E` | 8（循环 1000 次） | 条件分支 + 回跳 | 启动/等待循环，验证 CFG lowering |

生成器读取 `dbgtest/hot.asm` 与 `dbgtest/delay.asm`，按指令逐条发射 C；
运行时没有取指、解码、dispatch。`unsp_state.h` 只提供当前两个单元用到的
unSP 语义（寄存器、SR 标志、RAM、移位/旋转、compare/branch）。

## 验证

golden 快照由 MAME 0x289 调试器在真实断点处抓取：

- `dbgtest/entry_ram.bin` / `dbgtest/exit_ram.bin`：`0x2002F4` 入口与 `0x200343` 出口（2 s）
- `dbgtest/entry2_ram.bin` / `dbgtest/exit2_ram.bin`：同一函数在 10 s 的另一组数据（入口寄存器相同、RAM 不同）
- `dbgtest/delay_entry_ram.bin` / `dbgtest/delay_exit_ram.bin`：`0x5376` 入口与 `0x537F` 出口

两个 harness 都把生成的 RAM 与 golden 做全 4K word `memcmp`，并逐项检查寄存器。
当前结果：

```
hot_func   RAM vs exit_ram.bin: PASS   registers: PASS
hot_func   RAM vs exit2_ram.bin: PASS  registers (profile 1): PASS
delay_loop RAM vs delay_exit_ram.bin: PASS   registers: PASS
```

## 性能（x86-64，w64devkit GCC -O2，仅参考）

| 单元 | 单次 host cycles | cycles / guest insn | 说明 |
|---|---:|---:|---|
| `hot_func` | 35–48（200 万次热调用） | 0.51–0.71 | 68 guest insns，直线；两段快照 |
| `delay_loop` | 12,325（含每次重置状态） | 1.541 | 8,003 guest insns，1000 次循环 |

单次冷调用 `hot_func` 约 275–325 cycles，`delay_loop` 约 11,500 cycles；不同运行有波动，量级不变。
x86 是超标量，0.5–1.5 cycles/guest 不代表 C3；它只证明静态翻译把
“每 guest 指令 20–30 host cycles”的解释器成本压掉了一个数量级。

## C3 实测（AI Passport wasm 模拟器）

用 `riscv32-esp-elf-gcc -O2 -march=rv32imc -mabi=ilp32 -S` 生成，静态规模：

| 单元 | RISC-V 静态指令 | 分支 | load/store | 备注 |
|---|---:|---:|---:|---|
| `hot_func` | 126 | 0 | 17 / 21 | 无分支直线块 |
| `delay_loop` | 76 | 2 | 4 / 7 | 循环体约 45 条，1000 次 |

在 `firmware/recomp-bench` 里把两段代码编进 ESP32-C3 测试固件，用 AI Passport
wasm 模拟器（`esp32c3`，160 MHz）跑，连续 3 次完全一致：

| 单元 | host cycles / call | cycles / guest insn | guest instr/s（160 MHz） |
|---|---:|---:|---:|
| `hot_func` | 130 | 1.91 | 83.4 M |
| `delay_loop` | 46,212 | 5.77 | 27.7 M |

对照 P0 的 48 MHz 目标 23.4–26.9 M guest insn/s：两个块都进入区间，但
`delay_loop` 只比下限高一点；它的 C 代码展开成约 45 条 host 指令才对应 8 条
guest 指令，5.77 cycles/guest 主要是代码膨胀。**不能据此宣布整机实时**：
还没有 internal ROM、SPI flash、LCD、定时器、中断、banked registers，也没有覆盖
游戏主循环；整机平均指令 mix 和 cache 行为会让数字变化。

复现：`firmware/recomp-bench/build.ps1 -Sim`，日志见
`firmware/recomp-bench/sim_run.log`。

## C3 真机复核（COM4，2026-11-20）

同一镜像烧进 AI Passport（ESP32-C3 QFN32 rv1.1，8 MB XMC，160 MHz），
`erase_flash` → `write_flash 0x0` → `verify_flash` digest matched：

| 单元 | host cycles / call | cycles / guest insn | guest instr/s（160 MHz） |
|---|---:|---:|---:|
| `hot_func` | 134 | 1.98 | 80.68 M |
| `delay_loop` | 49,372 | 6.16 | 25.94 M |

- 原始数字：`HOT_BENCH cycles=26970559`，`DELAY_BENCH cycles=98745704`；三组校验全 PASS，`SIM_RESULT PASS failures=0`。
- 对照 48 MHz 原机 23.4–26.9 M：hot 3.0–3.4 倍，delay 96%–111%。
- 日志：`firmware/recomp-bench/device_run.log`。控制台是 UART0 primary + USB-Serial/JTAG secondary，COM4 和模拟器共用同一镜像。
- 仍然是**块级**结果；不含 internal ROM、SPI flash、LCD、IRQ、整机主循环。

## 构建 / 运行

```powershell
# 生成 C
C:\Users\DuWeisi\py311\python.exe recomp/gen_recomp.py
C:\Users\DuWeisi\py311\python.exe recomp/gen_delay.py

# x86 验证
C:\Users\DuWeisi\w64devkit\bin\gcc.exe -O2 -std=c11 -Wall -Wextra -Irecomp recomp/harness.c recomp/hot_func.c -o recomp/hot_test.exe
.\recomp\hot_test.exe dbgtest/entry_ram.bin dbgtest/exit_ram.bin recomp/out_ram.bin
.\recomp\hot_test.exe dbgtest/entry2_ram.bin dbgtest/exit2_ram.bin recomp/out2_ram.bin 1

C:\Users\DuWeisi\w64devkit\bin\gcc.exe -O2 -std=c11 -Wall -Wextra -Irecomp recomp/delay_harness.c recomp/delay_func.c -o recomp/delay_test.exe
.\recomp\delay_test.exe dbgtest/delay_entry_ram.bin dbgtest/delay_exit_ram.bin recomp/delay_out_ram.bin

# RISC-V 汇编（只做静态统计，不运行）
& "C:\Users\DuWeisi\.espressif\tools\riscv32-esp-elf\esp-14.2.0_20251107\riscv32-esp-elf\bin\riscv32-esp-elf-gcc.exe" -O2 -march=rv32imc -mabi=ilp32 -fno-pic -S recomp/hot_func.c -Irecomp -o recomp/hot_func_rv32.s
C:\Users\DuWeisi\py311\python.exe recomp/count_asm.py recomp/hot_func_rv32.s recomp/delay_func_rv32.s
```

或直接：

```powershell
.\recomp\build.ps1
```

C3 测试固件与模拟器：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File firmware\recomp-bench\build.ps1 -Sim
```

## 当前边界

- `unsp_state.h` 只映射 RAM（`0x0000..0x0FFF`），没有 internal ROM、SPI flash、IO、中断。
- `hot_func` 是 trace 里最热的 hash 函数，不代表游戏主循环。
- `delay_loop` 只验证分支/回跳 lowering，不是完整函数。
- 没有做 banked registers、MULS、BITOP、FIR_MOV、IRQ、SPI/LCD 时序。
- C3 真机 cycle 已测（COM4，2026-11-20），见上节；但仍只覆盖两个块。
- 没有接 `internal_rom_221017a.bin` 的启动路径，也没有跑完整帧。

## 下一步

1. 扩到 `0x230321` 调用者的基本块，处理 `call`/`ret`/`push`/`pop`。
2. 加 internal ROM + SPI flash 的 4 MB 窗口和 bank 选择，再跑 MAME 的 0–17 s golden 段。
3. 逐帧对齐时先把 MAME 的 96 MHz 改成 48 MHz，或用 guest cycle 对齐，避免拿错误的 10 秒当真实 10 秒。
4. 真机下一步不是再跑单块 benchmark，而是把内存映射、定时器和 LCD 接上，做整机帧率/计时验收。