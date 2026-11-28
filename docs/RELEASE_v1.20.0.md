# v1.20.0 — 手动存档槽：独立于自动存档，读档后接管最新自动存档

## 变更

设置卡新增两行：**SAVE NOW / LOAD SAVE**。

- `SAVE NOW`：把当前机器状态写进独立手动槽（`save` 分区尾部，偏移 `0x20000`，12 KiB）。
  不占自动存档的 5 条环，不改 `AUTO SAVE` 周期，游戏继续跑。
- `LOAD SAVE`：恢复这个槽，然后把同一份状态写成自动存档环的最新记录。
  读档后立刻掉电也不会丢；之后自动存档按原周期继续。
- 存档内容：CPU/SoC 快照 + 8 KiB guest RAM，带 CRC。读档先流式验 CRC，通过后才写 guest RAM，
  半写槽不会污染当前游戏。
- 手动槽不存画面（24.5 KiB 放不下 12 KiB 槽）；读档后整窗重绘。
- 菜单 7 行 → 9 行；卡片 192 → 214 px、上移到 y=6，避开右下角电量；行高 18→16、行距 20→18。
- 看门狗在菜单/待机期间不再误踢：两者都不绘制 guest 窗口，CRC 不变不是冻结。
- 控制台新增 `m s` / `m l`（`M` 大写仍是 AOT miss dump）。

## 分区表

`save` 从 128 KiB 扩到 140 KiB（尾部追加，现有偏移一个都没动）。

**首次升级必须同时写分区表**，否则手动槽初始化会失败（菜单显示 `NO SLOT`）：

```
esptool --chip esp32c3 -p COM4 -b 460800 write_flash ^
    --flash_mode dio --flash_freq 80m --flash_size 8MB ^
    0x8000 partition-table.bin 0x10000 digicore-c3.bin
```

或者整片（第一次装 / 救砖，会覆盖 ROM 分区，需重新传 ROM）：

```
esptool --chip esp32c3 -p COM4 -b 460800 write_flash ^
    --flash_mode dio --flash_freq 80m --flash_size 8MB ^
    0x0 digicore-c3-full.bin
```

## 数字

| 指标 | v1.19.0 | v1.20.0 |
|---|---|---|
| app 镜像 | 1,133,808 B | **1,137,664 B** |
| 整片镜像 | 4,677,888 B | 4,677,888 B |
| 分区表 | 128 KiB save | **140 KiB save** |
| 设置卡 | 7 行 / 192 px | **9 行 / 214 px** |

## 验证

真机（COM4，v1.20.0）：

- `m s` → `manualsave: stored 284 + 8192 B at 0x20000`，`pc=200343`；
- `m l` → `manual load ... autosave=newest`，自动存档环 `seq=5137`；
- 紧接着 `D` → `seq=5138`，证明自动存档周期没有被手动操作打断；
- 长按 A 打开设置卡（`long A (1216 ms) -> settings menu`，10 band 重绘）；
- `VER ver=1.20.0`。

## 已知限制

- 存档瞬间 guest 停在 ROM park（待机久了、还没回游戏画面）时，读档走 power-on replay：
  宠物、时钟、RAM 都在，但会重放开机动画。在游戏画面里存档则直接恢复。
- 手动槽不存画面，读档后整窗重绘。
- 用旧版整片镜像刷机会把分区表退回 128 KiB，手动存档失效；请用本版的
  `partition-table.bin` + `digicore-c3.bin`，或本版的 `digicore-c3-full.bin`。

## 报告

- `CHANGELOG.md` 1.20.0 条目
- `README.md` 存档一节
