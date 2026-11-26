# v1.13.0 — 第五轮审计：画面流畅性（帧间隔 51 ms → 16 ms）

**里程碑判据：同一台设备、同一场景（时钟界面 `pc=200343`），帧间隔 p50 从 51 ms 降到 16 ms，`guest_fps` 18.5 → 56.4，游戏速度 29.6 M → 90.2 M guest cycles/s（原机 96 MHz 的 94 %）。**

| 项 | 值 |
|---|---|
| 版本 | `1.13.0`（`firmware/digicore-c3/version.txt`） |
| 日期 | 2026-11-26 |
| 报告 | `docs/AUDIT_PERF_2026-11-26_R5.md` |
| 外部资料 | `docs/RESEARCH_EXT_C3_FETCH_PATH_2026-11-26.md` |
| app 镜像 | `digicore-c3-v1.13.0.bin` 1,023,456 B |
| SHA256 | 见 `sha256.txt` |
| 烧写 | 只写 `0x10000`；`save` 与 `nvs` 保留 |
| 回滚 | `tmp/digicore-c3-full-dio.bin`（v1.12.0 全片，DIO） |

## 改了什么

| 项 | 位置 | 内容 |
|---|---|---|
| F2 恢复等待循环折叠 | `main/CMakeLists.txt` `-DC3_WAIT_COLLAPSE=1`；`main/emu/unsp_glue.cpp` 补 `m_insn_count += 74*done` | 本轮最大单项：时钟场景 `gmax` 51 → 7 ms，`guest_pct` 93 → 31 |
| F3 帧预算自适应 | `main/app_main.cpp` `frame_budget_track()` | 默认 1/180，按 8 帧滑动平均在 1/240–1/60 之间调节，目标 14 ms |
| F4 STAT 默认静音 | `s_stat_period_s = 0` | 消除每 5 秒一次 ≈72 ms 的串口阻塞 |
| F1 帧间隔直方图 | `ft/ftmx/ov20/33/50/100` + `fc/auto` | 把「感觉卡」变成可比较的数字 |
| F5 修复扫描摊平 | `blit_frame()` | 6 条带分 6 帧推，消掉每 120 空闲帧一次 11–15 ms 尖峰 |

## 上机数据（COM4，v1.13.0）

```
VER ver=1.13.0 win=0,18 96x96 scale=2 left=64 top=24 fb=128 chunk=16
时钟界面 J 1: guest_fps=56.4 gmax=14000 ft=16/16 fc=1600000 auto=1 collapse≈9.5万圈/10s
park      J 1: guest_fps=56.5 gmax=10300 mi_s=0.60
```

## 已知未做

- 快照每 60 s 一次 140–250 ms 的 flash 写（应挪到暗屏/菜单窗口）。
- 菜单卡一次 77 ms 渲染 + 22 ms 推送。
- `0x230323` 进 IRAM、HLE 检查合并、`aot_step` 内联、尾调超块。
- QIO 启动失败（QE 位未验证），仍用 DIO。
