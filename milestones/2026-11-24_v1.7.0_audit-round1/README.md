# v1.7.0 第一轮性能审计（2026-11-24）

**判据：同一台设备、同一个空闲场景（ROM park 到内部 ROM，`pc=0x5376/0x5251`），
`guest_fps` 52.1 → 57.5（+9.5%），`mi_s` 0.55 → 0.61（+10.9%）。**
分析全文见 `docs/AUDIT_PERF_2026-11-24.md`。

| 项 | 值 |
|---|---|
| 版本 | `1.7.0`（`firmware/digicore-c3/version.txt`） |
| 日期 | 2026-11-24 |
| 源码 | git tag `v1.7.0` |
| 串口自检 | `VER ver=1.7.0 win=0,18 96x96 scale=2 left=64 top=24 off=0,0 home=64,24 fb=128 chunk=16` |
| app 镜像 | 1,192,640 B（2 MiB 分区，占 57%） |
| DRAM | 214,074 / 321,296 B，**余 107,222 B**（v1.6.1 余 1,138 B） |
| Flash code | 777,422 B（−2,936） |
| 刷写方式 | **只写 `0x10000` app 分区**，未动 bootloader / 分区表 / `roms` / `save` |

---

## 一、这一版改了什么

四类，按收益排序。

### 1. 一条真 bug：`SparseRom` 的 overlay 区间恒真

`romstore.h` 用 `page - m_ov_lo <= m_ov_hi - m_ov_lo` 判断某页是否被写入过，
空区间写成 `m_ov_lo = 1, m_ov_hi = 0`。无符号减法把空区间算成 `0xFFFFFFFF`，
条件对所有页成立，于是：

- 每次 flash 读都先跨 TU 调 `read16_overlay()`（`rflash` 十万量级/秒）；
- `page_window()` 同样返回 null，**`fetch16()` 的 4 KiB 取指窗口从来没建立过**。

改成显式双向比较 + `lo > hi` 表示空区间。host 单测 `tmp/test_mmap.cpp` 里
`page_window(0x230000)` 修复前是 null，修复后指向 `image + 0x230000`。

### 2. 删掉塞在热路径上的诊断（4 处，全部改成编译开关）

| 位置 | 每次做什么 | 开关 |
|---|---|---|
| `execute_run()` | lpc 区间判断 + ISR 直方图 + 48 项 PC ring（含 `% 48`） | `C3_IRQ_DIAG` |
| `aot_step()` | 6 次比较 + 2 次 DRAM 读改写（分区指令统计） | `C3_REGION_STAT` |
| `ioa_data_r()` / `ioa_buffer_r()` | `++n % 5000`（riscv32imc 无硬件除法，每次一次 `__umodsi3`） | `C3_IOA_TRACE` |
| `write16()` | 每次写 RAM 比一次 `addr == 0x0850` | `C3_TICK_DIAG` |

**注意**：`riram/rflash/rram/rpush/rdraw/rband` 在 STAT 行里现在恒为 0，那不是故障。
要看得加 `-DC3_REGION_STAT=1` 重编。

### 3. 帧循环与功耗

- pacer 自旋窗口 5.5 ms → 1.5 ms，其余交给 `vTaskDelay`（空闲任务进 WFI）。
- STAT 行 1 秒 → 5 秒（`J <sec>` 可调，`J 0` 全关）；面板 fps 数字保持 1 Hz。
- `window_has_content()` 每帧扫 9,216 像素 → 首帧后短路。
- `tick()` 的周期 printf、两个开机微基准（`C3_BENCH`）条件编译关掉。

### 4. 内存

- 64 KiB `SparseRom` 页缓存：mmap 成功时从不读，改成堆上按需分配。−65,532 B
- miss census 8 KiB、开机基准 24 KiB、清屏缓冲 10 KiB、`W` 命令 probe 4 KiB：全部复用或条件编译。

---

## 二、上机实测

v1.6.1 与 v1.7.0 都处于「16 秒无按键 → ROM 写 System_Sleep → park」的同一状态，
`pc`、`sp` 一致，所以是**同场景**对比：

| 指标 | v1.6.1 | v1.7.0 | 变化 |
|---|---|---|---|
| `guest_fps` | 50.5–52.6（均值 52.1，8 样本） | 57.50 / 57.57 | **+9.5%** |
| `mi_s` | 0.53–0.56（均值 0.55） | 0.61 | **+10.9%** |
| `guest_pct` | 90–92 | 96 | — |
| `[ioa]`/`[tick]` 刷屏 | 每秒数行 | 0 行 | — |

游戏画面场景（`pc=0x200343`）新固件 `guest_fps 16.5 / mi_s 6.34`。

### 分区完整性（本次刷写）

| 分区 | 刷前 → 刷后 |
|---|---|
| `roms` 头部 4 KiB | SHA256 一致（`C244169F…`） |
| `save` 128 KiB | 差 10 字节，全在 slot B 的页 24/25/28：DGSV 记录头序列号 `0x42/0x43 → 0x44/0x45`（+2，游戏自己存了两次档）+ 2 字节数据。**slot A 前 64 KiB 逐字节未变** |
| app | 写入 `0x10000`–`0x1332C0`，距 `roms` 起点 `0x210000` 尚有 0xDDD40 |

---

## 三、没做 / 下一轮入口

- `kBlitEvery` 2 → 1（观感更顺，需单独 A/B）
- `lcd.fb` 128 → 96 列宽（−14 KiB）
- I2S/ES8311 开机常开（静态功耗，需验 codec 重新锁定）
- 背光默认 80% → 60%（产品决策）
- **热区 IRAM**：DRAM 余量现在 107 KiB，这是第二轮的正式入口
- 区域化 AOT / token 解释器

---

## 四、回滚

```powershell
git checkout v1.6.1 -- firmware/digicore-c3
cd firmware/digicore-c3
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 -NoSparse
# 只烧 app：
& $py -m esptool --chip esp32c3 -p COM4 -b 460800 write_flash `
    --flash_mode dio --flash_freq 80m --flash_size 8MB 0x10000 build\digicore-c3.bin
```

ROM 与存档分区不在任何回滚路径上。
