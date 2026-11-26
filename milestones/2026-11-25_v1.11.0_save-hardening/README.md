# v1.11.0 — 存档加固：撕裂回退、帧块减写、锚点洗白

**里程碑判据：把最新一条存档的 CRC 打坏，复位后仍从次新记录恢复（不再从零开始）；停机型快照不再擦写 32 KiB 帧块；开机不再重写 save 镜像。**

| 项 | 值 |
|---|---|
| 版本 | `1.11.0`（`firmware/digicore-c3/version.txt`） |
| 日期 | 2026-11-25 |
| 报告 | `docs/AUDIT_SAVE_CLOCK_2026-11-25_R4.md` |
| app 镜像 | `digicore-c3-v1.11.0.bin` 1,014,352 B |
| SHA256 | 见 `sha256.txt` |
| 烧写 | 只写 `0x10000`，`save` 与 `nvs` 保留 |

## 一、修了什么

| # | 问题 | 修法 | 证据 |
|---|---|---|---|
| F1 | 最新记录撕裂 → 5 条全废、开机从零开始 | `gueststate_load()` 按序列降序逐条验 CRC，用第一条通过的 | `seq=165 is torn; falling back` → `restored snapshot seq=164 (older record after a torn one)` |
| F2 | 每 60 s 都写 32 KiB 帧块，包括根本用不上的停机型记录 | 只有 PC 在卡带的"可恢复"记录才写帧块，其余 `fb_crc=0` 走重绘 | `frame skipped (power-on replay)`、`snapf=0`；`D` 后立刻复位仍 `frame=restored` |
| F3 | 每次开机重写一次 save 镜像（16 KiB 擦 + 8 KiB 写） | `overlay_import()` 不再计为客人写；`W` 用 `overlay_touch()` 显式标脏 | 修后开机日志无 `saveimg: stored` |
| F4 | 构建时间锚点被写进 NVS，下次开机当成可信来源 | 只有 `live` 锚点才持久化 | 冷启动保持 `clock source=build` 直到有人给时 |
| F5 | 注释"前七条还在"（实际 5 条） | 改正 | — |

新增：控制台 `i`（模拟写一半掉电，测 F1 的唯一入口）、`CLK` 行的 `snapf`（帧块写次数）。

## 二、验证

```
# 撕裂回退
ACK i invalidated seq_was=165
W gueststate: snapshot seq=165 is torn; falling back to an older record
I gueststate: restored snapshot seq=164 from record 4 (older record after a torn one)

# 帧块只在可恢复时写
I gueststate: snapshot seq=170 -> record 0 (280 + 8192 B)
I DIGICORE: snapshot pc=005251 frame skipped (power-on replay)
CLK ... snapf=0

# 带帧直接恢复
I gueststate: restored snapshot seq=168 from record 3
I DIGICORE: resumed at pc=200343 sr=0020 sp=0ff5 int2=0205 frame=restored clock=kept

# 构建
Flash Code 605,352 B   DRAM 208,464 B（余 112,832 B）
VER ver=1.11.0
```

## 三、擦写账

- 每 60 s：记录 3 扇区；帧块 0 扇区（F2 之前是 4）。
- 15 个扇区轮转 → 288 次擦/扇区/天 → 24/7 一年约 105,000 次（10 万规格线），每天 8 小时约 35,000 次。
- 下一档优化是"状态没变就不存"，需要先量出空闲时真正在变的 RAM 字，见报告 §5。

## 四、回滚

```
git checkout v1.10.1 -- firmware/digicore-c3
powershell -NoProfile -ExecutionPolicy Bypass -File firmware\digicore-c3\build.ps1 -NoSparse
python -m esptool --chip esp32c3 -p COM4 -b 460800 write_flash 0x10000 firmware\digicore-c3\build\digicore-c3.bin
```

`save` 分区格式没变，v1.10.x 写的记录在 v1.11.0 里照常读（反之亦然）。
