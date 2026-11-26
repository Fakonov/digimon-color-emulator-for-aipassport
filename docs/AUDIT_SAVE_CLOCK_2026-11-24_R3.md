# 第三轮代码审计：存档 / 读档 / 自动存档 / 时钟

日期：2026-11-24
基线：v1.9.0（`acae65f`，`firmware/digicore-c3`）
本轮版本：**v1.10.1**（v1.10.0 的同日修订：删掉手动存读档与“采纳客人时间”；已烧 COM4，只写 app 分区，`save` 与 `nvs` 未动）
性质：**源码审计 + 真机测量 + 改动 + 编译 + 上机验证**。文中所有设备数字都来自同一台
AI Passport（ESP32-C3，COM4，MAC `4c:11:ae:2f:3f:d4`）。

---

## 0. 结论速览

用户报的三件事，全部复现并定位：

| 现象 | 根因 | 处理 | 修后实测 |
|---|---|---|---|
| 游戏时钟比真实时间**慢** | ①时钟种子取 `__TIME__`（编译时刻）；②软件 RTC 由 2 Hz 中断 + 帧体 `0x5BE2` 驱动，帧体不跑就不走 | 新增 `wallclock` 宿主时钟 + 每帧纠偏 | 同一测法：**0.317x → 1.002x**（唤醒）、**0.744x → 1.018x**（挂机），绝对误差 ≤ 2 s |
| 读档后时间**跳回编译时刻** | 第 120 帧无条件用 `__TIME__` 重播，覆盖了刚恢复的 RAM 时钟 | 有快照时钟就保留，并按 NVS 纪元补上停机时间 | `clock kept from snapshot 23:52:07` → `+6 s` 修正，`dev=0` |
| （本轮新发现）挂机/读档后时钟**突然快 9 小时** | `[0x0CB1]` 半秒计数器积压到 16 位上限（65535 tick = 32767 s = 9 h 6 m），帧体恢复后每帧补 1 秒 | 每 250 ms 截断积压 + 连续跳变不采纳 | 挂机 120 s 唤醒：`[0x0CB1]=0`、时钟 +121 s、`dev` 0/1 |
| 手动 `SAVE IN` 让游戏**重启**并污染时钟 | 读的是最新自动存档（多半是 ROM 等待循环的 PC），只能重播开机；旧的“采纳客人时间”又把读回的旧时钟当成玩家设置 | 菜单删掉存/读两行；删掉采纳路径，锚点为唯一真相 | 菜单 7 行、`Q 0caf 5` 后自动纠回；断电重启 `restored snapshot seq=148`、RAM 逐字一致 |
| 自动存档**时机不对** | ①首次存档要等满一个周期；②跳过时把计时器重置；③`asleep` 标志在“拒绝休眠”后常驻，挂机后永不存档；④`guest_pc()` 取错 SR 字段，守卫全部误判 | 绝对截止时间 + 10 s 首存 + 失败 5 s 重试 + 修 `guest_pc` + 修 `asleep` | 首次 **11.2 s**、之后 **71.2 s / 131 s**（60 s 整） |

一句话：**时钟不是“慢”，是“没有源、也会停，而且每次开机被编译时间覆盖”。**

---

## 1. 审计方法与基线

### 1.1 范围

- 源码：`app_main.cpp`（时钟种子、快照、自动存档、恢复、控制台）、`saveimg.cpp`、
  `gueststate.cpp/.h`、`fbsave.cpp/.h`、`emu/romstore.cpp/.h`、`emu/gpce4.cpp/.h`、
  `dmc_link.cpp`、`menu.cpp`、`CMakeLists.txt`。
- 逆向依据：`docs/RE_RTC.md`（2 Hz → `0x0CB1` → `0x5BE2` 每秒一次）、
  `docs/AUDIT_PERF_2026-11-24_R2.md`（上一轮留下的“每 60 s 写帧缓冲”欠账）。
- 不复测已结案项：96×96 可见区、面板只写不读、三键电阻梯、CW2017/ES8311 共用 I2C0。

### 1.2 v1.9.0 基线（真机，COM4）

用 `Z caf/cae/cad` 读游客 RTC，同一测法（首末各采一次、中间不打扰）：

```
== awake: key every 3 s, 60 s window ==
awake:  guest 20 s / host 63.1 s -> ratio 0.3168 (drift +41.0 s per 60 s)
== parked: no input, 60 s window ==
parked: guest 47 s / host 63.2 s -> ratio 0.7438 (drift +15.4 s per 60 s)
```

另一组对照：开机 6 分钟后，界面时钟 `23:13:20`、宿主 `23:19:19`，**慢 5 分 59 秒**；
而 `clock seeded 13:46:13 from build time` 这类日志在历史记录里出现几十次
（`tmp/wk.log:109`、`dbgtest/*.log`），即每次开机都从编译时刻重新开始。

---

## 2. 发现清单

### F1【严重】时钟种子是编译时刻

- 位置：`app_main.cpp` 原 `run_guest_frame()` 内 `kBuildTime = __TIME__`。
- 机制：`__TIME__` 是构建主机在编译 `app_main.cpp` 那一刻的本地时间，与“现在”无关。
  镜像放一天，时钟就慢一天；每次重启都回到同一时刻。
- 证据：`tmp/wk.log` 的 `clock seeded 13:46:13`（实际 22:50 左右）；
  `tmp/press_diag.log` 的 `23:07:38`；本轮基线 `23:13:20` @ 宿主 `23:19:19`。
- 处理：接入 `wallclock`（见 §3.1），种子改为宿主墙钟。

### F2【严重】软件 RTC 的速率取决于客人跑不跑帧体

- 机制（`docs/RE_RTC.md` §1.3）：IRQ7 每 0.5 s 把 `[0x0CB1]` 加 1，帧体
  `0x230105 → call 0x5BE2` 每次调用消费 2 个半秒、`[0x0CAD]++`。帧体不跑，
  半秒计数堆着不走；客人自己 `int off` 时连 IRQ7 都进不来。
- 本轮实测三种状态：

  | 状态 | 客人时钟 / 宿主 |
  |---|---|
  | 按键每 3 s（唤醒） | 20 s / 63.1 s = **0.317x** |
  | 无输入挂机 | 47 s / 63.2 s = **0.744x** |
  | 每 5 s 发一条 `S`（控制台负载） | 更差，日志本身要占主循环 ~78 ms/s |

- 这不是“仿真器慢”，而是**时基绑在客人执行上**：只要有任何一帧被丢、被跳过、
  或客人进入不等帧体的循环，时钟就永久少走那一段。
- 处理：宿主拥有墙钟，每 250 ms 比对一次，偏差超出 ±1 s 就写回
  `0x0CAF/0x0CAE/0x0CAD`（§3.2）。客人在死区内仍由自己走，`0x0CB2` 秒进位
  脉冲和每秒重绘都不受影响。

### F3【严重】读档后被编译时间覆盖

- 位置：原第 120 帧种子块，无条件执行。
- 机制：`restore_machine_state()` 恢复 RAM（含 `0x0CAF..0x0CB2`）→ 启动第 120 帧又被
  `__TIME__` 覆盖，玩家看到的时间回到编译时刻。
- 处理：恢复时判定记录里的时钟是否有效（`0x0CB0==1` 且时分秒在范围内），
  有效则 `clock kept`，不再重播；随后按 NVS 纪元补停机时间。

### F4【严重】`guest_pc()` 取错 SR 字段

- 位置：`app_main.cpp:892`（原 `(sr >> 10) & 0x3f`）。
- 事实：unSP 的 PC bank 在 **SR[0..5]**（`unspdefs.h: UNSP_LPC = ((SR & 0x3f) << 16) | PC`，
  `unsp.h add_lpc()` 用 `SR &= 0xffc0; SR |= bank` 回写）；SR[10..15] 是 **DS**。
- 后果：客人主循环里 `DS=0`，函数只返回 16 位 PC（如 `0x0343`），于是
  ①`snapshot_machine_state()` 的 `pc < 0x200000` 守卫把几乎每次自动存档都判成
  “在 ROM 里”而跳过；②恢复时同样的判断又会把正常的卡带记录当成坏记录，重播开机。
  两处误判互相“抵消”出一部分能用的记录，才让以前的日志看起来偶尔正常。
- 处理：改为 `(sr & 0x3f) << 16 | PC`；`emu/gpce4.cpp` 里两处诊断 PC 同样修正。
  证据：修复后同一台机器 `resumed at pc=200343`（旧日志里同一条会显示 `pc=000343`）。

### F5【中】`fb_crc` 写了但从不校验

- 位置：`SnapBlob::fb_crc`（`app_main.cpp:855` 一带）只写不读。
- 机制：`fbsave` 只有**一个** 32 KiB 帧块，被 5 条记录共用。后一次快照即使记录写失败，
  也已经把帧块覆盖；恢复旧记录时会把“另一时刻的画面”贴到“这一刻的 RAM”上。
- 处理：`fbsave_stored_crc()` 暴露帧块 CRC，`load_panel_frame(expect_crc)` 比对，
  不匹配就只重绘、不贴错帧，并打一行 `frame block ... does not match record`。

### F6【中】`gueststate_load()` 先拷后校验

- 位置：`gueststate.cpp` 原实现。
- 机制：把记录读进调用者的 `s_soc.ram` 之后才验 CRC。CRC 不对时返回 `false`，
  但活 RAM 已经被写坏——对 `U`（运行时读档）就是“读档失败还把游戏搞坏”。
- 处理：改成两遍：先按 128 B 分块从 flash 算 CRC，通过后才拷进目标缓冲区。
  目标缓冲区不参与校验，失败路径零副作用。

### F7【严重】自动存档节拍

- 原实现：`s_snap_last_us` 在每个“跳过”分支里被重置，且首次要等满一个周期。
- 三个具体问题：
  1. 首次存档在开机后 60 s 才发生——前一分钟断电全丢；
  2. 客人睡着（`asleep`）或 PC 被判在 ROM 时，每次都“重置倒计时”，等于永不存档；
  3. 周期从上一次**尝试**算起，不是从上一次**成功**算起。
- 处理：绝对截止时间 `s_snap_next_us`；首次 **10 s**；失败 **5 s** 重试；成功后
  按 `menu_autosave_sec()`（60/120/300/关）排下一次；只有“RAM CRC 未变”才算无事可做。

### F8【中】`system_sleep_w()` 拒绝休眠后仍置 `asleep=1`

- 位置：`emu/gpce4.cpp:1006`。
- 机制：固件本来就用 `set_sleep_inhibit(0x200300)` 把 PC 弹回帧循环，但函数开头已经
  `m_asleep = true` 且再也不清。于是 `snapshot_machine_state()` 每次都以为“客人在掉电
  流程里”而跳过——挂机 16 s 之后自动存档彻底停摆。
- 处理：走到“parked on pc=…”分支时把 `m_asleep` 清回 false；只有真正放行休眠才置位。

### F9【中】恢复失败时“starting from reset”其实什么都没重置

- 位置：`restore_machine_state()` 的 `restore_state()` 失败分支。
- 机制：只打一行日志就 return，RAM 已经是记录的、CPU 是旧的、SoC 是新的三拼一。
- 处理：失败时 `s_soc.reset()` + `guest_reset_cpu()`，日志改成 `replaying power-on`；
  `U` 命令同样处理，并回 `NAK U bad SoC block; replayed power-on`。

### F11【严重，本轮中途发现】半秒计数器积压会把时钟推快 9 小时

- 位置：ROM `0x486B`（IRQ7）写 `[0x0CB1]`，`0x5BE2` 每帧消费 2、加 1 秒。
- 机制：这套账只有在“帧体每秒跑 60 次”时才平衡。客人一旦停帧（挂机、
  菜单、丢帧），`[0x0CB1]` 只进不出；恢复时每帧消费 2，于是**每秒最多补 60 秒**。
  计数器是 16 位，满值 65535 个半秒 = **32767 秒 = 9 小时 6 分**。
- 证据（2026-11-25 00:12）：23:57 时 `CLK guest=23:57:32` 正常；00:09 再测变成
  `CLK guest=09:09:10 src=guest`——恰好 +9 小时，与 65535 个 tick 的上限一致；
  屏幕上同步显示 `9:09`（`docs/c3_clock_shot_2310.png`）。
- 二次伤害：本轮最初那版“采纳玩家时间”的逻辑把这个 45 倍速的排水过程当成
  “玩家在设时间”，每次检查都采纳，等于把跑飞的值写进锚点，形成正反馈。
- 处理（两处）：
  1. `clock_drop_backlog()`：每 250 ms 检查一次，`[0x0CB1] > 2` 就截到 2。
     只截不补，客人最多保留一秒的待处理时间；差额由宿主纠偏补。
     读档路径在客人跑起来之前也调用一次（记录里可能带着饱和值）。
  2. 采纳逻辑加“连续跳变”判据：只有**孤立的一次**跳变才认作玩家设时间，
     连续两次跳变判定为排水/跑飞，直接走宿主纠偏（写入同时把 `[0x0CB1]` 清零）。
- 验证：同一台机器挂机 120 s 后唤醒，`[0x0CB1]=0`、时钟 +121 s（正常速率），
  唤醒后 24 s 内 `dev` 在 0/1 之间，`steps` 只从 8 涨到 9，没有跑飞。

### F12【严重，用户回报后复现】手动 `SAVE IN` 会让游戏重启，而且会污染时钟锚点

- 复现（v1.10.0，控制台开菜单走到第 7 行按 B）：

  ```
  menu: load last requested
  gueststate: restored snapshot seq=139 from record 4
  W DIGICORE: snapshot pc=005376 outside the cartridge; replaying power-on
  I DIGICORE: resumed at pc=00480d sr=0000 sp=0000 int2=0001 frame=restored clock=cold
  I DIGICORE: clock -> 01:42:49 (+6169 s, 113 correction(s))
  ```

- 三个问题叠在一起：
  1. `SAVE IN` 读的是**环里最新的一条**，也就是自动存档，而自动存档多半发生在客人停在
     ROM 等待循环（`pc=005376`）的时候。那个 PC 没有可恢复的栈帧，只能重播开机 ——
     用户看到的就是“读档 = 游戏重启”。
  2. 所以“手动存档”其实存不出一个“可继续”的点位：`SAVE OUT` 刚写完，下一次自动存档
     60 s 后又把它顶掉；`SAVE IN` 再来读，读到的已经不是你存的那一份。
  3. 当时那版“采纳玩家设的时间”把读档带回来的**旧时钟**当成玩家操作：`src=guest`，
     锚点被拖到 `01:42`，真机时间其实是 `00:34`（`CLK guest=01:43:07 wall=01:43:08 src=guest`）。
     之后每次自动存档都把错误纪元写进 NVS。
- 处理（v1.10.1）：
  - 菜单删掉 `SAVE OUT` / `SAVE IN` 两行（`MENU_ITEMS` 9 → 7，`ABOUT` 前移到 6）；
    控制台 `D` / `U` 保留，只给台架用。
  - 删掉“采纳客人时间”整条路径（`wallclock_adopt_local()` 与跳变检测），宿主锚点是唯一真相：
    客人时钟任何跳变都会在 250 ms 内被纠回。实测：`Q 0caf 5`（把小时改成 5）后
    下一个检查周期就回到 `00:39`。
  - 半秒计数器截断（F11）保留。
- 代价：游戏自带的“设置时间”界面不再能改掉系统时间。板子没有 RTC，本来也守不住；
  要改时间用 `python tools\c3_setclock.py COM4 480`（或 `DMC1 HELLO <epoch>`），
  写进 NVS，重启后仍在。

### F13【已确认无问题】断电重启是否丢进度

- 测法：等到一次自动存档落盘 → 记下 RAM 片段 `0x0C80..0x0C9F` → 硬复位 → 读开机日志与同一片段。
- v1.10.1 实测：

  ```
  I (902) gueststate: restored snapshot seq=148 from record 3
  I (914) DIGICORE: resumed at pc=00480d ... frame=restored clock=cold
  I (6608) DIGICORE: clock seeded 00:41:27 from nvs
  I (11296) gueststate: snapshot seq=149 -> record 4
  ```

  RAM 片段 `0x0C90..0x0C9F` 复位前后逐字相同；只有 `0x0C86`/`0x0C88` 变了，那是 ROM 自己的
  开机初始化写的（和真机断电重启一样）。`gueststate_load()` 的 CRC 校验保证整块 8 KiB
  RAM 与存档时逐字节一致。
- 结论：**断电最多丢“最后一次自动存档之后”的内容**。默认周期 60 s、开机 10 s 就有第一份，
  所以最坏 60 s，通常更少。手动存读档没有额外价值，删掉不影响断电续命。

### F10【低/记录】帧块磨损与共享

- `fbsave_store()` 每次快照擦写整块 32 KiB；60 s 一次 = 1440 次/天，单块 4 扇区
  年擦写 ~21 万次，超过常见 10 万次额定。R2 §5 第 7 条已记，本轮不改（改成双块
  轮换要多 32 KiB 分区，且要让 `fb_crc` 带 slot 号，属于独立改动）。
- 本轮只加了 CRC 配对校验，让“错帧”变成“重绘”，不再是错误画面。

---

## 3. 修复实现

### 3.1 新模块 `wallclock.{h,cpp}`（189 行）

宿主墙钟，锚点 = `(Unix 秒, esp_timer 读数)`，来源优先级：

| 来源 | 何时 | 精度 |
|---|---|---|
| `console` | `t <epoch> [tz_min]` 或 `DMC1 HELLO <epoch> [tz_min]` | 精确 |
| `nvs` | 每次成功快照落盘（`wallclock_persist()`） | 误差 = 断电时长 |
| `guest` | 玩家在游戏里自己设时间（自动采纳） | 玩家说了算 |
| `build` | 以上都没有（刚刷的新板） | 误差 = 镜像年龄 |

`wallclock_local_sec()` 用固定时区偏移（默认 +480）算本地时分秒，不依赖 newlib 的
`TZ`；`days_from_civil()` 自己实现，`__DATE__/__TIME__` 解析不使用 `mktime()`。

### 3.2 `clock_sync()`（`app_main.cpp:2363`）

每帧调用，内部 250 ms 限流：

1. 把半秒计数器的积压截到 ≤2（F11）：只截不补，客人最多保留一秒的待处理时间。
2. 与宿主墙钟比较，差在 ±1 s 死区内就交给客人自己走（`holds++`）。
3. 超出死区：写回三个寄存器 + `0x0CB0=1`、`0x0CB1=0`、`0x0CB2=1`（秒进位脉冲，
   触发界面重绘），`steps++`。

v1.10.0 里还有第 0 步“连续跳变检测 + 采纳玩家设的时间”，v1.10.1 删掉了（F12）：
它把读档带回来的旧时钟、把计数器排水的加速都当成“玩家”，一次就能把锚点带偏一小时。
现在的规则是**锚点唯一真相**：客人怎么跳，250 ms 内纠回。

设计取舍：**只纠偏、不接管**。客人自己的 `0x5BE2` 仍然在跑，所以 12 小时制显示、
秒进位、每日切换这些 ROM 逻辑都不需要复刻。

### 3.3 种子与读档

```cpp
if (s_frames == 120 && !s_clock_seeded) {          // app_main.cpp ~2470
    if (s_clock_from_snapshot) {                    // 记录里有时钟
        log "clock kept from snapshot hh:mm:ss";
        if (wallclock_live()) clock_sync(true);     // 有真锚点就顺手补停机差
        else s_clock_keep_guest = true;             // 只有编译时间时就别覆盖
    } else {                                        // 冷启动
        wallclock_hms(&h,&m,&s);
        guest_clock_write(h,m,s);
        log "clock seeded hh:mm:ss from <source>";
    }
}
```

### 3.4 恢复判定（`restore_machine_state()`，`app_main.cpp:2244`）

| 记录里的 PC | 处理 | 理由 |
|---|---|---|
| `0x200000..0x3FFFFF`（卡带） | 原样恢复 | 已验证：`pc=200343 sr=0020 sp=0ff5` 逐字段一致 |
| `< 0x200000`（ROM/低地址） | `guest_reset_cpu()` 重播开机，RAM 保留 | 客人停在 0x52xx 的按键等待循环，栈帧对不上 |
| `>= 0x400000` | 同上 | 24 位地址外的脏值 |
| `io.asleep` 且 PC 在卡带 | `set_sleep_inhibit` + `wake_from_sleep` | 旧逻辑保留 |

**反例记录**（本轮踩过并回滚）：把 ROM PC 的记录硬弹到 `kGuestResumePc=0x200300`，
真机跑 3 帧后掉进 `pc=0000d0 sr=fc00`、每帧 33 s。原因是 park 循环的栈帧还在栈上。
所以“PC 不在卡带就重播开机”不是保守，是唯一正确解；代码里留了注释和实测数字。

### 3.5 改动文件

| 文件 | 改动 |
|---|---|
| `main/wallclock.h` / `wallclock.cpp` | **新增**，宿主墙钟 + NVS 纪元（v1.10.1 删掉“采纳玩家时间”）|
| `main/app_main.cpp` | `guest_pc()` 修正；`clock_sync()`；种子/读档时钟；自动存档调度；`t` 命令；`CLK` 诊断行；`load_panel_frame(expected_crc)`；恢复失败复位 |
| `main/emu/gpce4.cpp` | `system_sleep_w()` 不再常驻 `asleep`；两处诊断 PC 取 SR[0..5] |
| `main/gueststate.cpp` | 两遍读：先验 CRC 再落缓冲区 |
| `main/fbsave.h` / `fbsave.cpp` | 新增 `fbsave_stored_crc()` |
| `main/dmc_link.cpp` | `DMC1 HELLO [epoch] [tz_min]` 顺带对时 |
| `main/menu.h` / `menu.cpp` | v1.10.1：删 `SAVE OUT` / `SAVE IN` 两行（9 → 7 项），`MENU_ACTION_SAVE/LOAD` 同步删除 |
| `main/CMakeLists.txt` | 加 `wallclock.cpp`；v1.10.1 把 `version.txt` 声明成 `CMAKE_CONFIGURE_DEPENDS`（改了版本号不重跑 cmake 时，`V` 会继续报旧版本）|
| `version.txt` | 1.9.0 → **1.10.0** |
| `tools/c3_setclock.py` | **新增**，PC 侧一条命令对时 |

**没碰**：`tools/dmc_web.html`（用户未提交改动）、分区表、`roms`/`save`/`nvs` 内容、
AOT 块、显示/音频/电池路径。

---

## 4. v1.10.0 上机验证

### 4.1 构建

```
Flash Code 603,462 B   DRAM 208,456 B（余 112,840 B）      ← v1.10.1
digicore-c3.bin      1,012,096 B  SHA256 F3DA05F0D36887E74D17C583648148154F42E09D4265460BE463BA2850C30EAB
digicore-c3-full.bin 4,677,888 B  SHA256 FA2D713C6D5E77A7E43B470A7527358543049A2F0CDB2717D5A85581859618BD
（v1.10.0 为 1,014,224 B / 33021B2D…，v1.10.1 删掉两条菜单与采纳路径后小 2,128 B）
```

（F11 的修复在最后一次构建里；上文 §4.2–4.6 的数字来自修复前一次构建，逻辑未变，
最后一版重新验证过对时、挂机/唤醒与存读档，见 §4.8。）

烧写：只写 `0x10000`（app），`Hash of data verified`；`save` 与 `nvs` 保留。

### 4.2 对时

```
python tools/c3_setclock.py COM4 480
ACK t 23:48:42 src=console tz=+480 guest=23:48:42
CLK guest=23:48:43 wall=23:48:44 dev=1 steps=6 holds=68 src=console en=1
host    23:48:42
handset 23:48:42  source console
offset  0 s
```

### 4.3 速率（与基线同测法）

```
== awake, 60 s, key every 3 s ==
guest 62 s / host 61.9 s -> ratio 1.0020
== parked, 60 s ==
guest 63 s / host 61.9 s -> ratio 1.0181
```

60 s、每 3 s 一次按键、每 5 s 一条 `S` 的高负载场景下，`CLK` 行的 `dev` 全程
`|dev| ≤ 2 s`（脚本统计 `worst |dev| over the run: 2 s`）。

### 4.4 读档：卡带态直接恢复

```
I (411) wallclock: clock source=nvs tz=+480 min epoch=1795535527
I (903) gueststate: restored snapshot seq=92 from record 2
I (916) DIGICORE: resumed at pc=200343 sr=0020 sp=0ff5 int2=0205 frame=restored clock=kept
I (7159) DIGICORE: clock kept from snapshot 23:52:07
I (7159) DIGICORE: clock -> 23:52:13 (+6 s, 1 correction(s))
```

`pc/sr/sp/int2` 与快照一致，画面 `frame=restored`，时钟保留并补上 6 s 停机差；
15 s 后 `CLK guest=23:52:38 wall=23:52:39 dev=0`，`guest_fps=59.18`。

运行中读档（`U`）同样验证过：`ACK U seq=100 pc=200343 frame=restored`，机器继续跑、
时钟不跳（`CLK guest=23:56:49 wall=23:56:50 dev=1`）；失败路径不再写坏活 RAM（§F6）。

### 4.5 读档：停机型记录（PC 在 ROM）

```
I (894) gueststate: ring 5 x 12288 B newest seq=88
I (903) gueststate: restored snapshot seq=88 from record 3
W (914) DIGICORE: snapshot pc=00015e outside the cartridge; replaying power-on
I (915) DIGICORE: resumed at pc=00480d sr=0000 sp=0000 int2=0001 frame=restored clock=cold
I (6593) DIGICORE: clock seeded 23:49:18 from nvs
```

RAM（宠物、时钟）保留，PC 走 ROM 自己的 CONTINUE 路径，时钟从 NVS 纪元重播。

### 4.6 自动存档节拍

一次 80 s 抓取（复位后）：

```
I (11244) gueststate: snapshot seq=84 -> record 4 (280 + 8192 B)     ← 11.2 s
I (71262) gueststate: snapshot seq=85 -> record 0 (280 + 8192 B)     ← 71.3 s（+60.0 s）
```

首次 11.2 s（10 s 截止 + 到点的那一帧），之后严格 60 s；`SAVE OUT`（菜单）走
`force=true`，即使 RAM 未变也会落一条。

### 4.7 F11 修复后的复测（最后一版构建）

```
# 挂机 120 s 再唤醒：时钟 +121 s，半秒计数器为 0，没有跑飞
before park: CLK guest=00:12:24 wall=00:12:25 dev=1 ...
half counter after park: 0
after park  : CLK guest=00:14:25 wall=00:14:25 dev=0 ...
waking with A ...  +2s..+24s  dev=0/1  steps=8->9
half counter: 0
```

同一版构建的存读档往返（`D` → 复位 → 自动恢复）：

```
ACK D stored seq=121
I (412) wallclock: clock source=nvs tz=+480 min epoch=1795537061
I (904) gueststate: restored snapshot seq=121 from record 1
I (916) DIGICORE: resumed at pc=200343 sr=0020 sp=0ff5 int2=0205 frame=restored clock=kept
I (7124) DIGICORE: clock kept from snapshot 00:17:41
I (7125) DIGICORE: clock -> 00:17:47 (+6 s, 1 correction(s))
I (11278) gueststate: snapshot seq=122 -> record 2
```

### 4.8 v1.10.1：删掉手动存读档、锚点唯一真相

菜单（长 A 打开，走 3 行按 B）：

```
menu: open
long A (1210 ms) -> settings menu
menu: bright -> 40%            ← 第 3 行仍是 BRIGHT，行号没串
menu: close
```

行数从 9 变 7，`SAVE OUT`/`SAVE IN` 不再出现在卡上；`D`/`U` 只在控制台。

时钟：把客人小时直接改成 5，下一个检查周期就被纠回：

```
Q 0caf 5  →  CLK guest=00:39:26 wall=00:39:27 dev=1 steps=4 holds=37
```

断电重启（v1.10.1，硬复位）：

```
I (902) gueststate: restored snapshot seq=148 from record 3
I (914) DIGICORE: resumed at pc=00480d ... frame=restored clock=cold
I (6608) DIGICORE: clock seeded 00:41:27 from nvs
I (11296) gueststate: snapshot seq=149 -> record 4
RAM 0x0C90..0x0C9F 复位前后逐字相同（0x0C86/0x0C88 是 ROM 开机初始化改的）
```

---

## 5. 已知限制 / 本轮没做

1. **没有 RTC 就没有“断电多久”**。冷启动只能从上次快照的纪元继续；误差 = 关机时长。
   要精确必须外部给时：`t` 命令、`DMC1 HELLO <epoch>`，或以后接 SNTP。
   `tools/dmc_web.html` 里加一行 `DMC1 HELLO ${Math.floor(Date.now()/1000)} 480`
   即可自动对时——那个文件是用户自己的，本轮不动。
2. **帧块仍是单块**（F10）：已用 CRC 阻止错帧，磨损问题留待双块方案。
3. **设置卡里没有手动存读档了**（v1.10.1）：靠自动存档 + 断电恢复；`D`/`U` 只在控制台。游戏自带的“设置时间”界面改不动系统时间（锚点唯一真相），要改时间用 PC 工具。
4. **设置卡打开时客人停跑**：菜单是宿主的模态 UI，`run_guest_frame()` 不执行；
   时钟由 `clock_sync()` 继续纠正，但游戏画面本身停在那 一段时间（原有行为）。
4. **挂机后按键可能“没反应”**（`HANDOFF_2026-11-24` 待办第 5 条，R2 §5.8）：ROM 自己停在 0x5251/0x5376 等待按键，屏幕停在最后一帧。本轮没有改这个路径；按 A/B/C 任一个（控制台 `K 1 20`）能让 `[0x0C88]` 变化，但画面要等 ROM 走到重绘分支才更新。与本轮时钟改动无关，但用户会以为“死了”。
5. **恢复后前 ~10 s 可能停在卡带等待循环**（`pc=200343`，约 19 fps），随后客人自己
   回到 ROM park（`pc=005251`，59.4 fps）。这是 AOT 对 0x230321 等待循环的既有表现，
   不是本轮回归；`C3_WAIT_COLLAPSE=1` 可复现上一轮的对照。
6. **`CLK` 行是控制台命令 `S` 的输出**，不进 1 Hz 的 STAT 行，避免影响日志解析器。
7. **`tools/c3_autotest.py` 在 USB-Serial-JTAG 上不可靠**：它用 `serial.Serial(port, 115200)`
   直接开端口，DTR/RTS 默认拉高会把本板复位，而它的 0.35 s / 0.8 s 固定等待又对不上
   复位后的控制台窗口（实测每次都报 `no VER line`，同一时刻手工发 `V` 立即有回应）。
   本轮验证改用 `tmp/cmd_multi.py` 直接发命令（`V`/`S`/`K`/`D`/`U`/`t` 全过），
   工具本身保持原样，未在未验证的情况下改动。

---

## 6. 复现命令

```powershell
# 构建（只 app，不重建 sparse ROM）
powershell -NoProfile -ExecutionPolicy Bypass -File firmware\digicore-c3\build.ps1 -NoSparse

# 只刷 app（保留 save / nvs）
python -m esptool --chip esp32c3 -p COM4 -b 460800 --before default_reset `
  --after hard_reset write_flash --flash_mode dio --flash_freq 80m --flash_size 8MB `
  0x10000 firmware\digicore-c3\build\digicore-c3.bin

# 对时 + 自检
python tools\c3_setclock.py COM4 480      # ACK t / CLK，offset 应为 0 s
python tools\c3_clock_audit.py check      # 一行 CLK + STAT
python tools\c3_clock_audit.py anchor     # 对时后 60 s 采样，打印 worst |dev|
python tools\c3_clock_audit.py load       # 60 s 连续按键 + 采样
python tools\c3_clock_audit.py snap       # D 立即存档
python tools\c3_clock_audit.py u          # U 运行中读档

# 控制台
S            # STAT / IRQ / GEST / CLK
D            # 立即存档      U  # 立即读档
t 1795535527 480   # 手动对时
```

---

## 7. 给下一轮

1. `fbsave` 双块轮换 + 记录里带 slot 号，把磨损从“单块 21 万次/年”摊到两块。
2. 把 `DMC1 HELLO` 的自动对时接进 `dmc_web.html`，让用户永远不用手敲 `t`。
3. 若以后加 WiFi，`esp_sntp` 只需在 `wallclock_init()` 之后调一次，接口已经留好
   （`wallclock_set()`），上层零改动。
4. 帧块/记录的一致性还能再进一步：把 `seq` 也写进帧块头，恢复时可以直接判断
   “这块帧是不是这条记录的”，而不是只比 CRC。
