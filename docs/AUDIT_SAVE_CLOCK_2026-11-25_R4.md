# 第四轮代码审计：存档 / 读档 / 自动存档 / 时钟（收口与减耗）

日期：2026-11-25
基线：v1.10.1（`firmware/digicore-c3`）
本轮版本：**v1.11.0**（已烧 COM4，只写 app 分区）
性质：源码审计 + 改动 + 编译 + 真机验证。目标与第三轮相同，这一轮只做三件事：
**把丢档路径补死、把每次存档的 flash 代价降下来、把时钟锚点里最后一处"猜"去掉。**

---

## 0. 结论速览

| # | 问题 | 后果 | 处理 | 实测 |
|---|---|---|---|---|
| R4-F1 | 最新记录 CRC 失败时**不再找次新**，直接返回失败 | 一次掉电写坏最新记录 = 5 条全废，开机从零开始（宠物没了） | 按序列降序逐条试，第一条 CRC 通过的就用 | `snapshot seq=165 is torn; falling back` → `restored snapshot seq=164 ... (older record after a torn one)` |
| R4-F2 | 每次自动存档都写 32 KiB 帧块，**包括无法直接恢复的停机型记录** | 每 60 s 擦 4 扇区（全板最大磨损项）+ 主循环阻塞 ~150 ms | 只有"PC 在卡带、可直接恢复"的记录才写帧块 | `frame skipped (power-on replay)`；`snapf` 计数不再增长 |
| R4-F3 | 开机时 `overlay_import()` 把存档页当成"客人写了" | 每次开机重存一次 save 镜像：16 KiB 擦 + 8 KiB 写，纯浪费 | 导入不再计为写；控制台 `W` 用新的 `overlay_touch()` 显式标脏 | 修前开机日志有 `saveimg: stored 2 page(s)`，修后消失 |
| R4-F4 | `wallclock_persist()` 会把**构建时间锚点**写进 NVS | 猜测被"洗白"成可信来源：下次开机 `src=nvs`，覆盖快照时钟并按镜像年龄纠偏 | 只有 `live`（NVS/控制台）锚点才持久化 | 冷启动日志 `clock source=build` 保持到有人给时为止 |
| R4-F5 | `gueststate.h` 注释写"前七条还在"（实际 5 条） | 误导后续维护 | 改正 | — |
| R4-F6 | 60 s 周期下的擦写预算 | 5 条 × 3 扇区 = 15 扇区，1440 次/天 → **288 次/扇区/天**，24/7 一年约 10.5 万次，正好压在 10 万规格线上 | 帧块已从热路径移除；记录本身不动 | 见 §5 |

额外产出：控制台新增 `i`（模拟"写一半掉电"的撕裂记录，专门测 F1）；`S` 的 `CLK` 行新增 `snapf=`
（帧块写次数），本轮所有"省了多少"都靠它计数。

---

## 1. 方法

- 重读四条链路的当前实现：`gueststate.cpp`（环、store、load）、`fbsave.cpp`（帧块）、
  `saveimg.cpp` + `romstore.cpp`（flash 覆盖层）、`wallclock.cpp` + `app_main.cpp` 的
  `clock_sync()` / `snapshot_machine_state()` / `maybe_snapshot_machine_state()`。
- 只在真机上量（COM4），用现有控制台命令与 `tools/c3_clock_audit.py`。
- 复用第三轮的结论与工具，不重复测已结案项（96×96 几何、2 Hz 时基、半秒计数器截断）。

---

## 2. 发现与处理

### R4-F1【P0】最新记录撕裂 = 整盘作废

- 位置：`gueststate.cpp: gueststate_load()`。
- 机制：`gueststate_store()` 先写头、再写 280 B CPU 块、再写 8 KiB RAM。掉电落在中间时，
  头部（含 magic/长度/CRC）是完整的，payload 是残的。旧 `gueststate_load()` 只挑**序列号最大**
  的那条，CRC 不对就 `return false`，剩下的四条好记录看都不看。
- 后果：`restore_machine_state()` 收到 false → "no machine snapshot; starting from reset" →
  RAM 是复位后的空值，宠物/进度全丢。也就是说"5 条环"在掉电场景下退化成 1 条。
- 处理：收集所有头部完好、长度匹配的记录，按序列降序逐条做 CRC（`payload_ok()`，128 B 分块
  直接读 flash，不碰调用者缓冲区），第一条通过的就拷进目标并返回；失败的那条打一行
  `snapshot seq=N is torn; falling back to an older record`。
- 验证钩子：控制台 `i` 把最新记录的 **CRC 字段**取反（不是擦 magic——擦 magic 只模拟空槽，
  走不到这条路径）。实测：

  ```
  ACK i invalidated seq_was=165
  W (904) gueststate: snapshot seq=165 is torn; falling back to an older record
  I (915) gueststate: restored snapshot seq=164 from record 4 (older record after a torn one)
  I (926) DIGICORE: resumed at pc=00480d ... frame=none clock=cold
  ```

  修前同一场景会是 `no machine snapshot; starting from reset`。

### R4-F2【P1】帧块白写：停机型记录根本用不上它

- 位置：`app_main.cpp: snapshot_machine_state()`。
- 机制：`fbsave_store()` 每次快照擦整块 32 KiB（4 个扇区）再写 24 KiB。但一条记录只有在
  **PC 落在卡带**时才能直接恢复；停在 ROM 等待循环（`pc=0x5251/0x5376`，绝大多数自动存档
  都是这种）的记录只能重播开机，而 ROM 自己会把整屏重画——那份帧从头到尾没被读过。
- 代价：4 扇区擦除 + 24 KiB 编程，每 60 s 一次；`docs/AUDIT_PERF_2026-11-24_R2.md` §5 第 7 条
  记过这笔账，本轮才动。
- 处理：先判 `resumable = guest_pc() ∈ [0x200000, 0x400000)`，可恢复才 `save_panel_frame()`，
  否则 `fb_crc = 0`（`load_panel_frame(0)` 直接跳过、走重绘）。每次快照打一行
  `snapshot pc=%06x frame stored|skipped`，`CLK` 行的 `snapf` 是累计写次数。
- 实测（同一分钟节拍）：

  ```
  I (71149) gueststate: snapshot seq=170 -> record 0 (280 + 8192 B)
  I (71150) DIGICORE: snapshot pc=005251 frame skipped (power-on replay)
  I (131171) gueststate: snapshot seq=171 -> record 1 (280 + 8192 B)
  I (131172) DIGICORE: snapshot pc=005376 frame skipped (power-on replay)
  CLK ... snapf=0
  ```

  直接恢复路径仍然带帧（`D` 后立刻复位）：

  ```
  I (903) gueststate: restored snapshot seq=168 from record 3
  I (915) DIGICORE: resumed at pc=200343 sr=0020 sp=0ff5 int2=0205 frame=restored clock=kept
  ```

### R4-F3【P1】每次开机白写一次 save 镜像

- 位置：`romstore.cpp: overlay_import()`。
- 机制：`overlay_import()` 里有一句 `m_writes++`，本意是让 `persist_save_image()` 注意到
  "覆盖层变了"。但 `overlay_import()` 只在**开机恢复**和控制台 `W` 时被调用——于是每次开机
  2 秒后都会看到一次"变化"，把同样的 8 KiB 写进另一个槽。
- 代价：每次开机 16 KiB 擦 + 8 KiB 写；按每天 10 次开关机算，一年多写 58 GiB 等效扇区擦写。
- 处理：导入不再计为客人写；`W` 探针改用新的 `overlay_touch()` 显式标脏。
- 实测：修前每次开机 `I (2068) saveimg: stored 2 page(s) to slot 1`，修后该行不再出现。

### R4-F4【P2】构建时间锚点被写进 NVS，"猜"变成"证据"

- 位置：`wallclock.cpp: wallclock_persist()`。
- 机制：没有 NVS 纪元时锚点取 `__DATE__/__TIME__`，但 `wallclock_persist()` 照样把它写进 NVS。
  下次开机 `wallclock_init()` 读到它，标成 `src=nvs`（可信来源），于是**不再保留快照里的时钟**，
  并按镜像年龄去纠偏——一个刚刷的新板子会把自己的编译时刻当成"上次的真实时间"永久沿用。
- 处理：只有 `s_live`（NVS 读到或控制台/`DMC1 HELLO` 设置）才持久化。构建锚点保持"构建锚点"，
  直到有人给真时间；在此期间恢复的快照时钟会被保留（`s_clock_keep_guest`）。
- 验证：NVS 有真实纪元时一切照旧（`clock source=nvs`、`clock seeded 00:53:52 from nvs`）。

### R4-F5【P2】注释与实现不符

- `gueststate.h` 的磨损论证写着"前七条还在"，实际 `GUESTSTATE_RECORDS = 5`。已改。
- 同时把 `gueststate_invalidate_newest()` 的用途写进头文件：它是 F1 的唯一可复现入口。

### R4-F6【记录】擦写预算

- 每 60 s：3 个扇区（记录）+ 0 个扇区（帧块，F2 之后停机型不再写）。
- 5 条记录 × 3 扇区 = 15 个扇区轮转 → 1440 × 3 / 15 = **288 次擦/扇区/天**；
  24/7 一年 ≈ 105,000 次，正好压在常见 10 万次规格线上；按每天 8 小时算 ≈ 35,000 次。
- 想再降一档需要"状态没变就不存"（排除 `0x0600..0x0607` 的 PRNG、`0x0850` 的 64 Hz 计数、
  时钟寄存器），但排除表一旦漏掉一个真状态字就会漏存，本轮不做；留作下一轮，先测出
  空闲时到底哪些字在动。

---

## 3. 改动文件

| 文件 | 改动 |
|---|---|
| `main/gueststate.cpp` | `payload_ok()` 抽出；`gueststate_load()` 逐条降序回退；新增 `gueststate_invalidate_newest()`（`i` 钩子） |
| `main/gueststate.h` | 新钩子声明；修正"七条"注释 |
| `main/app_main.cpp` | 帧块只给可恢复记录写；`snapf` 计数进 `CLK`；`i` 命令；`W` 显式 `overlay_touch()` |
| `main/emu/romstore.cpp/.h` | `overlay_import()` 不再计写；新增 `overlay_touch()` |
| `main/wallclock.cpp` | `wallclock_persist()` 只在 `live` 时落盘 |
| `version.txt` | 1.10.1 → **1.11.0** |

没碰：分区表、`save`/`nvs` 内容、AOT 块、显示/音频/电池路径、菜单布局。

---

## 4. 上机验证

构建：

```
Flash Code 605,352 B   DRAM 208,464 B（余 112,832 B）
digicore-c3.bin      1,014,352 B  SHA256 F95A79E71DDF08DFEAD4D80C945FB57FF16220ADB57172E9E61D819FC591BE18
digicore-c3-full.bin 4,677,888 B  SHA256 87C1517FCC21BD32C21A71EE2D8AE2B71B6304E16DFE9247381A9ADA256A0607
VER ver=1.11.0
```

| 项 | 结果 |
|---|---|
| 撕裂回退 | `seq=165 is torn` → `restored snapshot seq=164 ... (older record after a torn one)` |
| 帧块跳过 | 两次 60 s 快照均 `frame skipped`，`snapf=0` |
| 带帧恢复 | `resumed at pc=200343 ... frame=restored clock=kept` |
| 开机不再写 save 镜像 | 修后无 `saveimg: stored` 行 |
| 自动存档节拍 | 71.1 s / 131.2 s（+60.0 s） |
| 时钟 | 100 s 内 `dev` 0/1，`00:54:36 → 00:56:18`（+102 s / 102 s） |
| 时钟（带按键负载） | 60 s 每 3 s 按键：`worst |dev| = 2 s`；`ACK t` 偏移 0 s |
| 帧块只在可恢复时写 | 同一次负载里 `snapf` 只 +1（按键把客人留在卡带循环，那次快照值得存帧） |
| 版本号 | `ver=1.11.0`（`version.txt` 已是 cmake 依赖，不会再报旧版本） |

---

## 5. 还没做 / 下一轮

1. **"状态没变就不存"**：需要先量出空闲时真正在变的 RAM 字（`0x0600..0x0607` 已知，
   `0x0850` 的 64 Hz 计数、时钟寄存器、`0x0881` 秒镜像待确认），再定排除表。
   收益是把空闲期擦写从 288 次/扇区/天降到 0；风险是漏排除 → 漏存。
2. **`saveimg` 的 8 KiB DRAM**：`spise/spipp/spiwren` 全程为 0，说明这份 ROM 从不写卡带存档区，
   这条路径对它是死重量（保留是为了用户换 ROM 镜像的情况）。真要省，需要先做一个
   "客人真的写过 flash 才分配缓冲"的方案。
3. **快照仍然阻塞主循环 ~100 ms**：AOT 代码在 flash 里，擦除期间 CPU 取指停滞，挪到任务里
   也省不掉；能省的只有 F2 那 150 ms。
4. **时钟**：没有 RTC，冷启动误差 = 关机时长；要进一步只能靠外部给时（已支持 `t` / `DMC1 HELLO`）。
