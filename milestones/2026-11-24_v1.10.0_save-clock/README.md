# v1.10.0 — 存档 / 读档 / 自动存档 / 时钟（第三轮审计）

**里程碑判据：同一台机器、同一种测法，游戏时钟从 0.317x 变成 1.002x；读档后时钟不再被编译时间覆盖；自动存档在开机 11 s 落第一次、之后严格 60 s。**

| 项 | 值 |
|---|---|
| 版本 | `1.10.0`（`firmware/digicore-c3/version.txt`） |
| 日期 | 2026-11-24 |
| 报告 | `docs/AUDIT_SAVE_CLOCK_2026-11-24_R3.md` |
| 串口自检 | `CLK guest=23:52:38 wall=23:52:39 dev=0 steps=9 holds=94 src=nvs en=1` |
| app 镜像 | `digicore-c3-v1.10.0.bin` 1,014,224 B |
| 整片镜像 | `digicore-c3-full.bin` 4,677,888 B |
| SHA256 | 见 `sha256.txt` / `firmware/digicore-c3/build.log` |
| 烧写 | 只写 `0x10000`，`save` 与 `nvs` 保留 |

---

## 一、问题与结论

用户报“游戏里的时钟比真实时间慢”。实测确认不是“慢一点”，而是三种毛病叠加：

1. **没有时间源**：种子取 `__TIME__`（编译时刻）。开机 6 分钟后界面显示 `23:13:20`、宿主 `23:19:19`，慢 5 分 59 秒；每次重启都回到同一时刻。
2. **软件 RTC 会停**：时钟由 2 Hz 中断 + 帧体 `0x5BE2` 驱动（`docs/RE_RTC.md`）。客人挂机、关中断、或宿主丢帧时，帧体不跑，秒就不走。同测法：唤醒态 **0.317x**、挂机 **0.744x**。
3. **读档覆盖时钟**：第 120 帧无条件重播 `__TIME__`，把刚从快照恢复的 RAM 时钟冲掉。

另有本轮中途发现的一条反向故障：**挂机/读档后时钟会突然快 9 小时**——`[0x0CB1]` 半秒计数器积压到 16 位上限（65535 tick = 32767 s = 9 h 6 min），帧体恢复后每帧补 1 秒。实测 23:57 正常、00:09 跑到 `09:09`（`docs/c3_clock_shot_2310.png`）。修复是每 250 ms 截断积压 + 连续跳变不采纳；挂机 120 s 唤醒复测 `[0x0CB1]=0`、时钟 +121 s、`dev` 0/1。

顺带查出四个存档侧缺陷（详见报告 §2）：`guest_pc()` 取错 SR 字段（DS 当 PC bank）、`fb_crc` 从不校验、`gueststate_load()` 先拷后校验、`system_sleep_w()` 拒绝休眠后常驻 `asleep=1` 导致挂机后永不再存档。

## 二、实现

| 文件 | 改动 |
|---|---|
| `main/wallclock.h` / `wallclock.cpp` | **新增**。NVS 纪元 + 固定时区 + `days_from_civil`；来源优先级 console > nvs > guest > build |
| `main/app_main.cpp` | `clock_sync()` 每 250 ms 纠偏（±1 s 死区）；第 120 帧种子按来源决定；读档保留时钟；自动存档改绝对截止时间（首存 10 s、失败 5 s 重试）；`t <epoch> [tz]` 命令；`S` 增加 `CLK` 行；`guest_pc()` 改取 SR[0..5]；帧块 CRC 配对；恢复失败真复位 |
| `main/emu/gpce4.cpp` | 拒绝休眠时不再置 `asleep`；两处诊断 PC 修正 |
| `main/gueststate.cpp` | 两遍读：先验 CRC 再落缓冲区 |
| `main/fbsave.*` | 新增 `fbsave_stored_crc()` |
| `main/dmc_link.cpp` | `DMC1 HELLO [epoch] [tz_min]` 顺带对时 |
| `tools/c3_setclock.py` | **新增**，PC 侧对时工具 |

设计上是“只纠偏、不接管”：客人的 `0x5BE2` 继续跑，12 小时制显示与秒进位都是 ROM 自己的逻辑；宿主只在偏差超过 ±1 s 时写回三个寄存器，并置 `0x0CB2` 触发重绘。玩家在游戏里自己设的时间（跳变 > 3 s）会被采纳为新锚点，不会被抢回去。

## 三、验证

```
# 对时
ACK t 23:48:42 src=console tz=+480 guest=23:48:42
CLK guest=23:48:43 wall=23:48:44 dev=1 steps=6 holds=68 src=console en=1
offset 0 s

# 速率（与基线同测法，60 s 窗口）
awake : guest 62 s / host 61.9 s -> ratio 1.0020      （基线 0.3168）
parked: guest 63 s / host 61.9 s -> ratio 1.0181      （基线 0.7438）
高负载 60 s：|dev| <= 2 s

# 读档（卡带态）
resumed at pc=200343 sr=0020 sp=0ff5 int2=0205 frame=restored clock=kept
clock kept from snapshot 23:52:07
clock -> 23:52:13 (+6 s, 1 correction(s))

# 读档（停机型，PC 在 ROM）
snapshot pc=00015e outside the cartridge; replaying power-on
resumed at pc=00480d sr=0000 sp=0000 int2=0001 frame=restored clock=cold
clock seeded 23:49:18 from nvs

# 自动存档
snapshot seq=84 -> record 4     (11.2 s)
snapshot seq=85 -> record 0     (71.3 s, +60.0 s)
```

## 四、回滚

```
git checkout v1.9.0 -- firmware/digicore-c3
powershell -NoProfile -ExecutionPolicy Bypass -File firmware\digicore-c3\build.ps1 -NoSparse
python -m esptool --chip esp32c3 -p COM4 -b 460800 write_flash 0x10000 firmware\digicore-c3\build\digicore-c3.bin
```

`save` 分区里 v1.9.0 写的记录（`Gpce4State` 版本未变）在 v1.10.0 里仍可读；
反过来 v1.10.0 写的记录 v1.9.0 也能读，只是退回旧行为（时钟按编译时刻重播）。

## 五、已知限制

- 没有 RTC：冷启动只能从上次快照的纪元续，误差 = 关机时长。要精确必须外部给时（`t` 命令或 `DMC1 HELLO <epoch>`）。
- 帧块仍是单块 32 KiB，60 s 一次擦写；已用 CRC 阻止错帧，磨损问题留待双块方案。
- 设置卡打开时客人停跑：时钟由宿主继续纠正，游戏画面本身暂停（原有行为）。
