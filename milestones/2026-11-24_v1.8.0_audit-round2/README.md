# v1.8.0 第二轮性能审计（2026-11-24）

**判据：同一台设备、同一个 park 场景（`pc=0x5251`/`0x5376`，`sp=0ff4`），
`guest_fps` 57.5 → 59.3，`mi_s` 0.61 → 0.63，`guest_pct` 96 → 62。**
分析全文见 `docs/AUDIT_PERF_2026-11-24_R2.md`。

| 项 | 值 |
|---|---|
| 版本 | `1.8.0`（`firmware/digicore-c3/version.txt`） |
| 日期 | 2026-11-24 |
| 串口自检 | `VER ver=1.8.0 win=0,18 96x96 scale=2 left=64 top=24 off=0,0 home=64,24 fb=128 chunk=16` |
| app 镜像 | 1,005,792 B（2 MiB 分区，占 48%） |
| DRAM | 206,032 / 321,296 B，**余 115,264 B**（v1.7.0 余 107,230） |
| Flash code | 598,078 B |
| 刷写方式 | **只写 `0x10000` app 分区**，未动 bootloader / 分区表 / `roms` / `save` |

---

## 一、这一版改了什么

### 1. `aot_blocks.c` 从 `-O3` 换成 `-Os`（本轮最大单项）

3.8 MB 生成代码、1,466 个块、16 KiB 指令 cache。

| | `-O3` | `-Os` |
|---|---|---|
| Flash code | 870,812 | 598,078 |
| app 镜像 | 1,289,840 | 1,005,792 |
| DRAM | 217,404 | 206,032 |
| park `guest_pct` | 91 | **64** |
| park `guest_fps` | 59.28 | 59.33 |

代码小 31%，CPU 时间少 27 个百分点，fps 不掉。在 16 KiB i-cache 上，
块体大小就是命中率。

### 2. 三处热路径税

| 位置 | 每次做什么 | 开关 |
|---|---|---|
| `aot_rd` / `aot_wr` | 每个 guest 访存一次 DRAM 读改写 | `C3_AOTACC=0` |
| `run_guest_frame` | 每个调度切片 4 次 `esp_cpu_get_cycle_count()` | `C3_PROF=0` |
| `spin_collapse` 调用点 / 探针 | 每次 dispatch 一次跨 TU 调用 + 计数写 | 先内联比较三个 PC；`C3_SPIN_DIAG=0` |

STAT 行格式不变：关掉的字段继续打印，值为 0。

### 3. `wait_loop_collapse()` 从"永久 false"改成编译开关，默认关

2026-11-20 它把帧率从 7.69 提到 14.15；2026-11-23 为调查"设备不读 PORTA"
被临时关掉，调查结案后没恢复。本轮先恢复再实测，**结果是负的**：

| `-Os` 构建 | `guest_pct` |
|---|---|
| collapse 关 | **64** |
| collapse 开 | 66 |

`-Os` 之后 AOT 块已经不比 `hot_prng_n` 慢多少，collapse 的状态搬运反而贵。
现在它是 `C3_WAIT_COLLAPSE`（默认 0），热路径零成本。

### 4. 帧缓冲脏检测改成写时标记

`blit_frame()` 以前每 host 帧对 9,216 个可见像素算 FNV-1a（约 50k 主机周期，
画面不动也照算）。现在 `St7735` 在 `store_pixel`/`fill_run` 里按 16×16 tile
标脏，blitter 只读位图，推送成功后清零；传输失败保留位、下帧重试。
直接写 `fb[]` 的两处（`paint_pattern`、`fbsave_load`）显式 `dirty_all()`。

### 5. 音频 codec 空闲关断

唯一声源是按键 beep。以前 MCLK 12.288 MHz、ES8311 模拟轨、I2S DMA 空转从
开机跑到关机。现在 15 秒无 beep 关断（先关模拟轨再停 MCLK），下次 beep 由
audio task 唤醒（约 2 ms I2C，不占主循环）。

上机：`ACK K 1 6` → beep → `audio: codec + I2S off after 15 s without a beep`。

### 6. 面板刷新率取消硬上限

`kBlitEvery` 从编译期 2 改成运行时变量，默认 **1**（面板每帧跟随 guest）。
控制台 `k 1`..`k 8` 可改，`k 2` 即旧行为，不用重烧。

### 7. 删除实验残留

`main/app_main.cpp.pregion`、`main/emu/unsp_glue.cpp.pregion`、
`main/emu/aot.h.compact.bak`（区域化 AOT 实验副本，不参与编译）。

---

## 二、上机实测

| 构建 | `guest_fps` | `mi_s` | `guest_pct` |
|---|---|---|---|
| v1.7.0 | 57.5 | 0.61 | 96 |
| 本轮 `-O3`，collapse 开 | 59.28 | 0.63 | 91 |
| 本轮 `-Os`，collapse 开 | 59.31 | 0.63 | 66 |
| 本轮 `-Os`，collapse 关 | 59.33 | 0.63 | 64 |
| **本轮 `-Os`，collapse 关，探针关** | **59.3** | **0.63** | **62** |

55 秒 soak：59.22–59.37 fps，无 `STUCK`、无 panic。

菜单验证：长按 A → `settings menu`（`render 79 ms, total 99 ms`），按 C →
`menu: close` → 全屏重绘一次（`blit_pct=3`, `rows=2688`）→ 回到 `blit_pct=0`。

---

## 三、没做 / 下一轮入口

- **热块 IRAM**：census 已取到（`0x230321`/`0x230323`/`0x200343` 各 240 万次），
  但要先给 `tools/aot.py` 加热表，手工改生成文件会被覆盖。
- **区域化 AOT**：1.4–1.8×，4–8 周，独立立项。
- **重绘帧实测**：本机 park 场景画面静止，`kBlitEvery=1` 的代价没有 A/B 数据；
  进游戏画面量一次再定默认值。
- 背光默认 80%、`lcd.fb` 96 列、`CONFIG_PM_ENABLE`：理由见审计文档 §5。

---

## 四、回滚

```powershell
git checkout v1.7.0 -- firmware/digicore-c3
cd firmware/digicore-c3
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 -NoSparse
# 只烧 app：
& $py -m esptool --chip esp32c3 -p COM4 -b 460800 write_flash `
    --flash_mode dio --flash_freq 80m --flash_size 8MB 0x10000 build\digicore-c3.bin
```

ROM 与存档分区不在任何回滚路径上。
