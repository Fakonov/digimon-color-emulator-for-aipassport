# v1.17.0 — 第七轮审计：清掉热路径记账税与不可达代码

## 数字

| 指标 | v1.16.2 | v1.17.0 |
|---|---|---|
| app 镜像 | 1,144,944 B | **1,134,128 B**（−10,816） |
| DRAM 静态剩余 | 82,306 B | **87,200 B**（+4,894） |
| `execute_run()`（每次 dispatch 都跑） | 5,434 B | **5,142 B** |
| 稳定态 `guest_fps` | 56.2–56.4 | 56.4–56.7 |
| 稳定态 `ft` p50/p95 | 16/16 ms | 16/16 ms |
| 切时钟最坏帧 | 38–79 ms | 44–55 ms（区间与基线重叠） |

## 主要内容

- **HLE 入口判定改 32 位掩码位测**：五个钩子入口只做一次移位/按位与，命中后才精确比较；
  每个 dispatch 少约 15 个 host 周期。
- **`wait_loop_collapse()` 先内联判两个 PC 再调用**：不再为一个必然的「否」付一次跨 TU 调用。
- **`St7735::fill_run()` / `write_run()` 的脏块位按 16 像素行合并**：整屏重绘 27 K 像素，
  写内存次数降 16 倍；宿主等价测试 5 用例逐位一致。
- **`C3_HLEPROF` 默认关**：每次钩子 2 次 `rdcycle` + BSS 读改写的探针税（只喂 LONGF 的 `hp=`）。
- **A+C 确认卡编译掉**（`C3_CONFIRM_CARD=0`，−1,868 B）：自 v1.5 起就不可能被打开。
- **编译期日志上限 DEBUG → INFO**，按键 `tap` 日志降级：运行级本来就是 INFO，D 级字符串永远打不出来；
  按键一次不再花 5 ms 在串口上。
- **DMC1 header 暂存页 4,096 → 64 B**（`rom_esp::mount()` 只读 60 B），省 3,996 B DRAM。
- 删除 STAT 三个从未自增的字段、五个只写不读的变量与恒 0 分支。

## 网页端 V2

`tools/dmc_web.html` + `tools/dmc_assets.js`（V2）：

- 传输：ROM 上传、存档导出/导入、清空存档。
- 攻略（纯前端、不连设备）：图标菜单（8 项，原机 96×96 截图 + 中/英/日名）、
  进化路线（Ver.1 全部 19 只）、进化预测（输入照顾/训练/战斗/睡眠次数）。
- **两个文件必须放在同一目录**：`dmc_assets.js` 是内联的 base64 图片资源（约 85 KB）。
- 网页需要 https 或 localhost（`navigator.serial` 的 secure context 要求）。

## 烧写

app 分区镜像，只写 `0x10000`，`save` 与 `nvs` 保留：

```
esptool --chip esp32c3 -p COM4 -b 460800 write_flash 0x10000 digicore-c3-v1.17.0.bin
```

硬件：FoloToy AI Passport（ESP32-C3，8 MB flash，ST7789P3 320×240）。

## 不包含

ROM 镜像（版权原因）。本 release 只提供 app 分区，`roms` 分区需用网页工具自行上传。

## 报告

- `docs/AUDIT_PERF_2026-11-27_R7.md`（总报告，通俗版）
- `docs/notes/R7_app.md`、`R7_core.md`、`R7_aot.md`、`R7_periph.md`、`R7_build.md`

## 已知限制

- 切界面首帧仍是 40–55 ms（ROM 全屏重绘 + 图形数据从 SPI flash 读）；本轮清理的税约 4 %，
  落在这个噪声区间内，真机单次测量区分不出来。
- AOT 入口表占用率 10 %（空槽 140 KB）与 1,097 个重叠块（278 KB）**这版没动**：
  前者要 A/B，后者要改生成器，留给下一轮。
- `CONFIG_COMPILER_LTO=y` 写在 `sdkconfig.defaults` 里但从未生效（`sdkconfig` 比它旧）；
  对 16 KB 指令 cache 的影响未知，未启用。
- 自动存档每 60 s 有一次 140–250 ms 的 flash 写。
- QIO 试过，全片镜像在本机 flash 上起不来（QE 位），仍用 DIO。
