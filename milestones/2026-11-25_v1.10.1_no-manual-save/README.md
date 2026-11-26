# v1.10.1 — 删掉手动存读档，时钟锚点唯一真相

**里程碑判据：设置卡从 9 行变 7 行，`SAVE OUT` / `SAVE IN` 消失；断电重启仍能恢复最后一次自动存档（实测 RAM 逐字一致）；把客人时钟改乱，250 ms 内被纠回。**

| 项 | 值 |
|---|---|
| 版本 | `1.10.1`（`firmware/digicore-c3/version.txt`） |
| 日期 | 2026-11-25 |
| 报告 | `docs/AUDIT_SAVE_CLOCK_2026-11-24_R3.md` §F12 / §F13 / §4.8 |
| app 镜像 | `digicore-c3-v1.10.1.bin` 1,012,096 B |
| SHA256 | 见 `sha256.txt` |
| 烧写 | 只写 `0x10000`，`save` 与 `nvs` 保留 |

## 一、为什么删

用户回报“手动读档不太正常”。控制台复现（v1.10.0）：

```
menu: load last requested
gueststate: restored snapshot seq=139 from record 4
W DIGICORE: snapshot pc=005376 outside the cartridge; replaying power-on
I DIGICORE: resumed at pc=00480d sr=0000 sp=0000 int2=0001 frame=restored clock=cold
I DIGICORE: clock -> 01:42:49 (+6169 s, 113 correction(s))
```

三件事叠在一起：

1. `SAVE IN` 读的是环里**最新**的一条，也就是自动存档；自动存档多半发生在客人停在 ROM 等待循环（`pc=005376`）时，那个 PC 没有可恢复的栈帧，只能重播开机 → 用户看到“读档 = 游戏重启”。
2. `SAVE OUT` 存不出一个“可继续”的点位：60 s 后下一次自动存档就把它顶掉，再 `SAVE IN` 读到的已经不是刚才那份。
3. v1.10.0 的“采纳玩家设的时间”把读档带回来的旧时钟当成玩家操作（`src=guest`），锚点被拖到 `01:42`，而真实时间是 `00:34`；之后每次自动存档都把错误纪元写进 NVS。

## 二、改了什么

| 文件 | 改动 |
|---|---|
| `main/menu.h` | `MENU_ITEMS` 9 → 7；删 `MENU_IDX_SAVE_OUT/SAVE_IN`，`ABOUT` 6；删 `MENU_ACTION_SAVE/LOAD` |
| `main/menu.cpp` | 删两行的按键处理、值文本、标签 |
| `main/app_main.cpp` | 删 `MENU_ACTION_SAVE/LOAD` 分支；删“跳变检测 + 采纳客人时间”，`clock_sync()` 只做截断积压与纠偏 |
| `main/wallclock.h/.cpp` | 删 `wallclock_adopt_local()` |
| `version.txt` | 1.10.0 → **1.10.1** |

保留：控制台 `D`（立即存档）/ `U`（立即读档），只给台架用；自动存档（首存 10 s、周期 60/120/300 s）；
半秒计数器截断（v1.10.0 的 F11）。

## 三、验证

菜单（长 A 打开，走 3 行按 B）：

```
menu: open
long A (1210 ms) -> settings menu
menu: bright -> 40%       ← 第 3 行仍是 BRIGHT，行号没有串
menu: close
```

时钟锚点：`Q 0caf 5` 把客人小时改成 5，下一个检查周期即纠回：

```
CLK guest=00:39:26 wall=00:39:27 dev=1 steps=4 holds=37
```

断电重启（硬复位，v1.10.1）：

```
I (902) gueststate: restored snapshot seq=148 from record 3
I (914) DIGICORE: resumed at pc=00480d ... frame=restored clock=cold
I (6608) DIGICORE: clock seeded 00:41:27 from nvs
I (11296) gueststate: snapshot seq=149 -> record 4
```

RAM 片段 `0x0C90..0x0C9F` 复位前后逐字相同；`0x0C86`/`0x0C88` 变化来自 ROM 自己的开机初始化
（真机断电重启同样如此）。`gueststate_load()` 的 CRC 保证整块 8 KiB 与存档时一致。

**结论：断电最多丢“最后一次自动存档之后”的内容（默认 60 s，开机 10 s 就有第一份）。**

## 四、代价与回滚

- 游戏自带的“设置时间”界面改不动系统时间（锚点唯一真相）。板子没有 RTC，本来也守不住；
  要改时间用 `python tools\c3_setclock.py COM4 480`，写进 NVS，重启后仍在。
- 回滚到 v1.10.0：`git checkout v1.10.0 -- firmware/digicore-c3`（若该 tag 不存在，用
  `33021B2D…` 那版镜像 `milestones/2026-11-24_v1.10.0_save-clock/`），重编后只刷 `0x10000`。
