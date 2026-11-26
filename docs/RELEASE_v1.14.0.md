# v1.14.0 — 流畅性：等待循环折叠、自适应帧预算、切换路径 AOT 覆盖

## 数字

| 指标 | v1.12.0 | v1.14.0 |
|---|---|---|
| 时钟界面 `guest_fps` | 18.5 | **56.4** |
| 帧间隔 p50 | 51 ms | **16 ms** |
| 单帧 guest 最长 | 51 ms | **14 ms** |
| 游戏速度 | 29.6 M guest cycles/s | **90.2 M（原机 96 MHz 的 94%）** |
| 按 B 切界面首帧 | 98–116 ms | **73–88 ms** |
| 切换帧 AOT miss | 44 % 的 dispatch | **3 %** |

## 主要内容

- **恢复 `C3_WAIT_COLLAPSE=1`**：设备此前跑的是 PC 参考实现从不使用的路径。等待循环
  一圈 298 guest cycles 要付 1478 host cycles，其中 1200 花在调度三个 1–5 条指令的外壳块上。
- **自适应帧预算**：目标 14 ms，在 1/240–1/60 之间按 8 帧滑动平均调节；`F <hz>` 可手动锁定。
- **AOT 覆盖率**：miss census 的 top 200 并入 `AB_force`（块 1466 → 1944），
  切换帧 miss 从 44 % 降到 3 %。
- **绘制热区进 IRAM**：146 块（连同原有 60 块，共 175 块 / 39 KB），切换首帧 98 → 73 ms。
- **STAT 默认静音**：那条每 5 秒的日志在 115200 波特率下要阻塞 72 ms（UART0 忙等 + USB 再写一遍）。
- **新诊断**：帧间隔直方图 `ft/ftmx/ov20/33/50/100`，以及 `LONGF`（单帧 > 60 ms 时打印
  `dblk/dins/dmiss/dfb/dtick`）。

## 烧写

app 分区镜像，只写 `0x10000`，`save` 与 `nvs` 保留：

```
esptool --chip esp32c3 -p COM4 -b 460800 write_flash 0x10000 digicore-c3-v1.14.0.bin
```

硬件：FoloToy AI Passport（ESP32-C3，8 MB flash，ST7789P3 320×240）。

## 不包含

ROM 镜像（版权原因）。本 release 只提供 app 分区，`roms` 分区需自行准备。

## 报告

- `docs/AUDIT_PERF_2026-11-26_R5.md`
- `docs/AUDIT_SWITCH_2026-11-26_R6.md`
- `docs/RESEARCH_EXT_C3_FETCH_PATH_2026-11-26.md`

## 已知限制

- 切换界面首帧仍是 73–88 ms（ROM 全屏重绘 + 图形数据从 SPI flash 读）。
- 菜单卡一次 77 ms 渲染 + 22 ms 推送。
- 自动存档每 60 s 有一次 140–250 ms 的 flash 写。
- QIO 试过，全片镜像在本机 flash 上起不来（QE 位），仍用 DIO。
