# recomp-bench — C3 静态翻译块实测

这是 `recomp/` 的 ESP32-C3 测试固件，不是 Digimon Color 模拟器。
它只回答一件事：两段已经静态翻译成 C 的 unSP 代码，在 C3（160 MHz）上跑多快、
结果是否仍与 MAME 快照一致。

## 测什么

| 单元 | guest 范围 | 动态 guest 指令 | 校验 |
|---|---|---:|---|
| `hot_func` | `0x2002F4..0x200343` | 68 / call | 2 s 与 10 s 两组 RAM + 寄存器 |
| `delay_loop` | `0x5376..0x537E` | 8,003 / call | 1 组 RAM + 寄存器 |

`HOT_PROFILE0`、`HOT_PROFILE1`、`DELAY` 三组都先做全 4K word `memcmp`，再核对寄存器；
全部通过才进入 benchmark。

## 实测结果

### wasm 模拟器

AI Passport wasm 模拟器，`esp32c3`，160 MHz，同一最终镜像连续复跑：

```text
HOT_PROFILE0 PASS
HOT_PROFILE1 PASS
DELAY PASS
HOT_BENCH n=200000 cycles=26085089 cycles_per_call=130 cycles_per_guest=1.91
DELAY_BENCH n=2000 cycles=92425201 cycles_per_call=46212 cycles_per_guest=5.77
SIM_RESULT PASS failures=0
```

### COM4 真机（2026-11-20）

ESP32-C3 QFN32 rv1.1，8 MB XMC，160 MHz；`erase_flash` → `write_flash 0x0`
→ `verify_flash` digest matched。日志 `device_run.log`：

```text
HOT_PROFILE0 PASS
HOT_PROFILE1 PASS
DELAY PASS
HOT_BENCH n=200000 cycles=26970559 cycles_per_call=134 cycles_per_guest=1.98
DELAY_BENCH n=2000 cycles=98745704 cycles_per_call=49372 cycles_per_guest=6.16
SIM_RESULT PASS failures=0
```

换算成块级 guest 吞吐：

| 单元 | 平台 | host cycles / call | cycles / guest insn | guest instr/s（160 MHz） |
|---|---|---:|---:|---:|
| `hot_func` | wasm | 130 | 1.91 | 83.4 M |
| `hot_func` | COM4 | 134 | 1.98 | 80.68 M |
| `delay_loop` | wasm | 46,212 | 5.77 | 27.7 M |
| `delay_loop` | COM4 | 49,372 | 6.16 | 25.94 M |

48 MHz 原机的目标区间是 **23.4–26.9 M guest instr/s**。两个块都进入区间，
但 `delay_loop` 只比下限高一点，而且它的 C 代码展开成约 45 条 host 指令
才对应 8 条 guest 指令，所以 5.77/6.16 cycles/guest 是代码膨胀，不是单纯的 ALU 成本。

**屏幕不会亮**：这个固件不初始化 LCD/背光，只输出串口；黑屏不是刷机失败。

**这不是整机实时证明。** 当前没有 internal ROM、SPI flash、LCD、定时器、中断、
banked registers，也没有覆盖游戏主循环；整机平均指令 mix 和 cache 行为会让数字变化。
它只证明：静态翻译后的块在 C3 上确实比解释器快一个量级，值得继续扩。

## 构建

依赖 ESP-IDF 5.5.3、riscv32-esp-elf、ninja、cmake、Python 3.11。

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\build.ps1
```

脚本会编译、生成 `build/recomp-bench.bin`（208,176 B），并合并成
`recomp-bench-full.bin`（273,712 B / 267.3 KiB / 0x42D30）。加 `-Sim` 会继续跑 wasm 模拟器：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Sim
```

也可以手动执行：

```powershell
idf.py -B build -DIDF_TARGET=esp32c3 build
python -m esptool --chip esp32c3 merge_bin -o ../recomp-bench-full.bin `
  --flash_mode dio --flash_freq 80m --flash_size 8MB `
  0x0 bootloader/bootloader.bin 0x8000 partition_table/partition-table.bin `
  0x10000 recomp-bench.bin
```

模拟器：

```powershell
node C:\Users\DuWeisi\digimon-color-passport\tools\sim_uart_probe.mjs `
  C:\Users\DuWeisi\digimon-color-passport\firmware\recomp-bench\recomp-bench-full.bin 3
```

## 文件

- `main/bench_main.c` — 测试入口、初始化、benchmark、串口输出
- `main/golden_data.h` — 6 组 MAME RAM 快照，由 `tools/gen_sim_golden.py` 生成
- `build.ps1` — 编译 + 合并 + 可选模拟器
- `sim_run.log` — 本次 wasm 模拟器完整日志
- `device_run.log` — 首轮 COM4 真机完整启动与 benchmark 日志
- `reflash_run.log` — 2026-11-20 07:39 清空重刷复核的 COM4 日志
- `recomp-bench-full.bin` — 合并镜像（wasm 模拟器可加载，也可整片烧到 0x0；不是最终模拟器固件）

## 已知边界

- `State` 结构约 8 KiB，测试固件把它放在静态存储；放到 `app_main` 栈上会直接踩爆
  main task 的默认栈。
- `CONFIG_ESP_TASK_WDT_EN=n`，所以没有调用 `esp_task_wdt_deinit`。
- 只映射 RAM `0x0000..0x0FFF`，DS 固定为 0。
- 没有接 LCD/SPI/中断，也没有跑完整帧。
- 真机只跑了两个翻译块；不是整机模拟器。
- 控制台为 UART0 primary + USB-Serial/JTAG secondary；COM4 与 wasm 模拟器共用同一镜像。
