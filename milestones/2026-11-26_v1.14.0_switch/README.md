# v1.14.0 — 切换界面卡顿：AOT 覆盖率 + 绘制代码进 IRAM

**里程碑判据：按 B 切计时界面的那一帧从 98 / 116 / 78 ms 降到 73 / 88 / 72 ms；切换帧的 AOT miss 从 4372 / 6931 / 4760 降到 306 / 244 / 120；静止画面保持 56 fps / 帧间隔 p50 16 ms。**

| 项 | 值 |
|---|---|
| 版本 | `1.14.0` |
| 日期 | 2026-11-26 |
| 报告 | `docs/AUDIT_SWITCH_2026-11-26_R6.md`（前一轮 `docs/AUDIT_PERF_2026-11-26_R5.md`） |
| app 镜像 | `digicore-c3-v1.14.0.bin` 1,139,280 B |
| SHA256 | 见 `sha256.txt` |
| 烧写 | 只写 `0x10000` |
| 回滚 | `git checkout v1.13.0 -- firmware/digicore-c3`，再跑一次 `tools/aot.py` 重新生成 |

## 改了什么

| 项 | 位置 | 内容 |
|---|---|---|
| AOT 覆盖 | `tools/coverage/out/AB_force.txt`、`main/emu/aot_blocks.c` | miss census top 200 并入（691 → 1170 条），块 1466 → 1944 |
| 绘制热区 IRAM | `tools/coverage/out/aot_iram_r5.txt` | 175 块 / 39 KB（原 60 / 15.9 KB） |
| 帧内归因 | `app_main.cpp` `LONGF` | 帧 > 60 ms 打一行：dblk/dins/dmiss/dfb/dtick |

## 生成 AOT（不要跑 sync_c3_emu.py）

```
AOT_CONST_TABLES=1 AOT_LOOSE=1 \
AOT_EXTRA=tools/coverage/out/AB_dev.txt \
AOT_IRAM_LIST=tools/coverage/out/aot_iram_r5.txt \
AOT_FORCE=tools/coverage/out/AB_force.txt \
AOT_OUT=firmware/digicore-c3/main/emu/aot_blocks.c \
python tools/aot.py
```

`tools/sync_c3_emu.py` 会用 PC 侧副本覆盖设备侧 `unsp_glue.cpp`，本轮补丁会丢。

## 已知未做

- 首帧 70–90 ms 的主体是 ROM 图形数据从 SPI flash 读（每 16 像素补一次 32 B cache 行）。
  下一轮用 `SparseRom` overlay 把绘制热数据段常驻 64 KB RAM。
- 息屏点亮路径未单独测，需要用同一套 `LONGF` 抓一次。
