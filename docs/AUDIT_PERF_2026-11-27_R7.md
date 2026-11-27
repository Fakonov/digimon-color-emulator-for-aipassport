# 第七轮审计：把「无用代码」清出去，先分清哪本账

日期：2026-11-27  
基线：v1.16.2 → 本轮 **v1.17.0**  
题目：去除刷写版里的冗余代码，找出拖慢运行的无用代码，在不影响稳定、功能、流畅的前提下清理。  
性质：第三方审计视角的团队作业（五路并行取证 + 主审裁决 + 源码改动 + 编译/真机验证）。

---

## 0. 结论先说（三句话）

1. **真正拖慢游戏的，不是一眼看上去「没人调用」的函数，而是每次指令分发都要付的几个周期。**
   死函数在链接阶段就被垃圾回收了（镜像里 0 字节），而每次 dispatch 的 20 来个周期 × 每帧几千次，
   才是能感觉到的部分。这轮清的就是这笔「小税」。
2. **最大的一笔冗余是数据，不是代码**：AOT（预翻译代码）的入口表 155,648 B 里有 140,096 B 全是空座位
   （占用率 10 %），另有 1,097 个「同一条指令被翻译了两三遍」的重叠块（277,946 B）。
   这两笔能省 0.2–0.4 MB，但一个要 A/B、一个要改生成器，**这轮只给方案不动刀**。
3. **这轮交付**：镜像 −10,816 B、DRAM 静态余量 +4,894 B、每次 dispatch 少约 15 条指令；
   功能验收全部通过；帧时的改善在 40–55 ms 的场景噪声里**测不出来**——这一点如实写在第 5 节。

---

## 1. 团队怎么分工

| 角色 | 负责范围 | 产出 |
|---|---|---|
| 应用层审计员 | `app_main.cpp`（3,958 行）、`menu.cpp`、`buttons.c` | `docs/notes/R7_app.md`（391 行） |
| 模拟器核心审计员 | `gpce4.*`、`unsp_glue.cpp`、`st7735.*`、`romstore.*`、`aot.h`、`hotblock.h`、ISA 五个文件 | `docs/notes/R7_core.md`（521 行） |
| AOT 表审计员 | `aot_blocks.c`（4.6 MB 生成物）、`tools/aot.py`、覆盖数据 | `docs/notes/R7_aot.md`（473 行） |
| 外设/资源审计员 | `display.c`、`bezel.cpp`、`audio.c`、`battery.c`、`wallclock.*`、`dmc_link.cpp`、`rom_esp.cpp`、分区与资源 | `docs/notes/R7_periph.md`（230 行） |
| 构建/二进制审计员 | `sdkconfig`、CMake、map/ELF 体积账本、字符串与库 | `docs/notes/R7_build.md`（582 行） |
| 主审（本文件） | 定口径、裁决冲突、改代码、编译、真机验收、写总报告 | 本文件 + 固件改动 |

五路都只读：不改源码、不跑构建。所有改动由主审统一做，避免并发踩踏（这个仓库的 `emu/` 与
`firmware/.../emu/` 是近亲副本，两边同时改一定会出事）。

---

## 2. 先分清三本账

「省资源」这句话本身没意义，得先知道省的是哪一种：

| 账本 | 现状（v1.16.2） | 紧张吗 | 通俗解释 |
|---|---|---|---|
| Flash（仓库） | app 1.14 MB / 2 MB 分区（54.6 %） | **不紧张**，剩 0.95 MB | 放代码和素材的仓库，还空着一半 |
| DRAM（工作台） | 静态占用 239 KB，剩 82 KB | **紧** | 运行时真正的工作台，只剩 1/4 |
| 每帧时间（心跳） | 16.7 ms 一帧，稳定态 16 ms | **最紧** | 一帧里跑不完就会卡，所有优化都为它服务 |

一句话：**这固件不缺仓库，缺工作台和心跳**。所以「删代码省 flash」本身价值有限，
真正值钱的是「每帧少做无用功」和「省 DRAM」。

---

## 3. 什么叫「无用代码」：这轮挖到的四类

### 3.1 永远不执行的代码（死代码）

| 项 | 多大 | 为什么永远不执行 |
|---|---|---|
| A+C 确认卡 `menu_confirm_present` | 1,868 B | 自 v1.5 和弦变成设置卡第 0 行后，`menu_confirm_set(true)` 再无调用点，`s_confirm` 恒 false |
| STAT 的 `hot=` / `tick_pct=` / `slices=` | 3 个字段 | 三个变量从来没被任何代码自增过，日志里永远是 0 |
| `Y` / `M` / `O` 的 PHASE 行 | ~170 B | 对应编译开关恒 0，命令只剩空桩 |
| `s_b_hold_locked`、`s_snap_count`、`kCmdNone`、`kAcHoldDefaultUs`、`kAutoPressAt` 分支 | 源码卫生 | 只写不读 / 恒 0 / 空枚举 |
| `rot_map()` 与 `kRot != 0` 分支 | 0 B（编译器已消） | `kRot` 是 `constexpr 0`；顺带发现 `case 4/5` 配 `kRot & 3` 是**真 bug**：镜像那两条分支永远进不去 |

> 人话：这些是抽屉里的备用钥匙，但锁已经换了。它们大多在链接时就被丢掉（镜像 0 字节），
> 留着只浪费读代码的人的时间；确认卡是例外——它有 1.8 KB 真的在镜像里。

### 3.2 每次都要付的「记账税」（这才是性能项）

| 项 | 每 dispatch / 每像素代价 | 一帧累计（重绘帧 6,788 dispatch） | 通俗解释 |
|---|---|---|---|
| 5 组 HLE 入口判定（`bool && pc == 常数`） | ~22 周期 | 149 K ≈ 1.9 % | 每帧几千次「检票」，每次问五句 |
| `wait_loop_collapse()` 无条件调用 | ~11 周期 | 75 K ≈ 1.0 % | 为一个必然的「不是」付一次函数调用 |
| `m_dirty` 每像素读改写 | 3–4 周期/像素 | ≈95 K ≈ 1.3 % | 27,000 个像素各刷一次「已改动」标记 |
| `C3_HLEPROF` 计时探针 | 8–12 周期/钩子入口 | ≈25 K ≈ 0.33 % | 每帧给 2,650 次钩子量体温，只为打印一行日志 |
| `hle_blit_key` 每像素重读全局系数 | 3 周期/像素 | ≈55 K ≈ 0.7 % | 把乘法系数从内存里反复取出来 |
| 闲置统计字段与恒 0 分支 | 0–3 周期 | ≈20 K ≈ 0.3 % | 记在账本上，但从没人看 |

合计约 **4–5 % 的重绘帧**。单看每项都不大，但它们的共同点是「每帧都付、永远在付」。

### 3.3 数据层面的冗余（体积 + cache 密度）

| 项 | 体积 | 说明 |
|---|---|---|
| AOT 入口表空槽 | **140,096 B** | 76 页 × 256 槽 × 8 B = 155,648 B，实际只登记 1,944 个块（占用率 10 %） |
| AOT 重叠块 | **277,946 B** | 1,097 个块（56.4 %）的每条指令都已被更早的块翻译过；同一条 `0x200343` 被 56 个块各译一遍，指令重复率 2.75× |
| 砖墙贴图 `kBrickA8` | 16,384 B | 取值只有 `0x00` 和 `0xFA` 两种，实际是 1 bit 信息（打包后 2,048 B） |
| `font5x7.inc` | 590 B（两份） | 同一个字体表被 `menu.cpp` 与 `bezel.cpp` 各包含一次 |

> 人话：停车场画了 256 个车位，只停了 21 辆车；同一段路修了两三条并行的车道。
> 空座位不影响开车，但它把「有效数据」稀释到 1.62 块/32 B 行，指令 cache 的命中率跟着吃亏。

### 3.4 库与配置（白带的行李）

| 项 | 体积 | 处置 |
|---|---|---|
| 编译期日志上限是 DEBUG | 72 条 D 级格式串 / 3,789 B | 降到 INFO（运行级本来就是 INFO，**零行为变化**） |
| 按键路径每按一次打 1–3 条 INFO | 每条 ≈5 ms @115200 | `tap` 降为 DEBUG（按键一次就打掉 1/3 帧预算） |
| DMC1 header 暂存页 4,096 B 只为写 60 B | 3,996 B DRAM | 缩到 64 B |
| `CONFIG_COMPILER_LTO=y` 写了但没生效 | 未知（可能几十 KB + 性能） | **只报告不动**：`sdkconfig` 比 `sdkconfig.defaults` 旧，LTO 从未参与构建；对 16 KB 指令 cache 的影响未知，需单独 A/B |
| WiFi / TLS / OTA / SPIFFS 开关全开 | **0 B** | 链接器已把对应库成员全部丢弃，别在这里找体积 |

---

## 4. 这轮改了什么

每条都遵循一个原则：**能在编译期证明不改变行为的才改**；需要 A/B 的一律不动。

| # | 改动 | 位置 | 收益 | 怎么证明没改坏 |
|---|---|---|---|---|
| 1 | HLE 入口判定改 32 位掩码位测（命中后再精确比较） | `emu/unsp_glue.cpp` `execute_run` | `execute_run` 5,434 → **5,142 B**；每 dispatch −15 周期 | 反汇编尺寸 + 真机功能 |
| 2 | `wait_loop_collapse` 先内联判两个 PC 再调用 | 同上 | 每 dispatch −11 周期 | 与 `spin_collapse` 同型，语义不变 |
| 3 | 脏块位按 16 像素行合并写入 | `emu/st7735.cpp` `fill_run` / `write_run` | 27 K 像素/帧，写内存次数降 16 倍 | **宿主等价测试 5 用例 fb/state/dirty 逐位一致** |
| 4 | `g_blit_cyc_per_px` 入口取一次 | `hle_blit_key` | 每像素 −3 周期 | 同一钩子内值不变 |
| 5 | `C3_HLEPROF` 默认 0 | CMake + `unsp_glue.cpp` | 每帧 −25 K 周期 | 只影响 LONGF 的 `hp=`，STAT 不用 |
| 6 | A+C 确认卡编译掉（`-DC3_CONFIRM_CARD=0`） | `menu.cpp` / `menu.h` | −1,868 B | 函数签名保留为常量，开宏可原样恢复 |
| 7 | STAT 三个恒 0 字段 + 三个变量删除 | `app_main.cpp` | −12 B DRAM + 日志更诚实 | 全工程检索：零自增 |
| 8 | 5 个只写变量 / 空枚举 / 恒 0 分支删除 | `app_main.cpp` | 源码 −40 行 | 编译器本来就已消除，仅卫生 |
| 9 | 日志上限 DEBUG→INFO；按键 `tap` 降级 | `sdkconfig` / `sdkconfig.defaults` / `app_main.cpp` | ≈−6 KB；按键路径少 5 ms | 运行级 INFO，D 级本来就打不出来 |
| 10 | DMC1 header 暂存 4,096 → 64 B | `dmc_link.cpp` | **−3,996 B DRAM** | `rom_esp::mount()` 只读 60 B |
| 11 | `note_unmapped` 先判满再扫；恒真条件删除 | `gpce4.cpp` / `unsp_glue.cpp` | 每 IO 未命中 −130 周期 | 满表返回值完全相同 |

---

## 5. 验证：怎么证明没改坏

### 5.1 编译与体积

| 项 | v1.16.2 | v1.17.0 | 变化 |
|---|---|---|---|
| app 镜像 | 1,144,944 B | **1,134,128 B** | −10,816 B（−0.94 %） |
| `execute_run()` | 5,434 B | **5,142 B** | −292 B（每 dispatch 更短） |
| `St7735::fill_run` / `write_run` | 336 / 376 B | 424 / 468 B | +180 B（一次性代码，换循环内每像素 3 条指令） |
| DRAM 静态剩余 | 82,306 B | **87,200 B** | +4,894 B |

### 5.2 宿主等价测试（不碰设备）

`St7735::write_run()` 与「真实字节流」逐位对比，5 个用例（96 px 行、96×96 整屏、40 px 局部、
MV=0 旋转、列窗口）全部 `fb=ok state=ok`，包括 `m_dirty` 位图。这是第 3 条改动的直接背书。

### 5.3 真机验收（COM4，v1.17.0）

- 启动：`BOOT_OK first non-blank guest frame at 33 frames`，无 panic / abort。
- 画面：`T` 命令回读 framebuffer，解码为 96×96 图，与基线同一界面一致。
- 按键：长按 A 1,216 ms → 设置卡打开；B 选第 0 行 → `A+C in 700 ms`；C 关闭。日志与基线一致。
- 存档与时钟：刷 app 不碰 ROM/存档分区，重启后时钟从 NVS 恢复（11:19 → 显示正常）。
- 版本：`V` 报 `ver=1.17.0`。

### 5.4 帧时（诚实记录）

同一脚本（`J 1` → 一次 B 键注入 → 采集 2.4 s）在基线与本版各跑多次：

| 指标 | 基线 v1.16.2 | v1.17.0 |
|---|---|---|
| 稳定态 `ft` p50/p95 | 16/16 ms | 16/16 ms |
| 稳定态 `guest_fps` | 56.2–56.4 | 56.4–56.7 |
| 切换界面最坏帧 `gmax` | 38–79 ms（多次 38/46/57/79） | 44–55 ms（多次 44/51/54） |

**结论：稳定态不变，切换帧的改善落在场景噪声里，单次真机测不出来。**
原因是这轮清掉的税合计约 4 %（≈0.4 ms/帧），而切换帧本身在 40–80 ms 之间随画面内容浮动，
噪声比收益大一个量级。要证明它，只能靠反汇编（`execute_run` 确实短了 292 B）与逐项周期的预算表。
**不把这写成「提速 30 %」是这份报告最重要的部分。**

---

## 6. 不动的清单（防误删）

| 项 | 为什么留 |
|---|---|
| 控制台命令表 `console_exec`（5,066 B） | `T/N/G/E` 是出图与屏几何标定的唯一手段，删了以后没法验收画面 |
| 每帧末尾 ≤1.5 ms 忙等 | 帧对齐靠它；删了会掉帧 |
| AOT 块体与 208 条 IRAM 名单 | 名单逐 PC 与源码一致、零白占；块体是重绘帧唯一能跑动的依靠 |
| `internal_rom` 64 KiB | 内部掩码 ROM，XIP 直读、无 RAM 副本，guest 离不开 |
| NVS / save 分区 / `internal_rom` | 存档与设置，动一下就是用户数据 |
| `-O3` + aot_blocks 的 `-Os` | 实测分工正确：解释器要速度，块体要密度 |
| 砖墙 `kBrickA8` 的逐字节拷贝约定 | 打包成 1bpp 能省 14 KB，但会破坏与姊妹项目的一致性，留作可选项 |

---

## 7. 下一轮建议（按性价比排序）

| 优先级 | 事项 | 预期 | 代价/风险 |
|---|---|---|---|
| 1 | **AOT 重叠块改「入口桩 goto」** | 块体 277,946 → ~35 KB，**省 ≈243 KB**，命中率不损失 | 要改 `tools/aot.py` 的 groupify 准入条件 |
| 2 | **入口表按 [min_slot,max_slot] 裁剪** | 155,648 → 91,016 B，**省 64.6 KB** | `aot_lookup` 多 2 次比较，必须真机 A/B |
| 3 | **LTO 单独 A/B** | 体积与跨 TU 内联都可能受益 | 编译时间大增；I-cache 可能反噬；`sdkconfig.defaults` 里那个 `CONFIG_COMPILER_LTO=y` 至今没生效 |
| 4 | 砖墙 1bpp | 省 14,336 B | 破坏逐字节拷贝约定，观感需复核 |
| 5 | `sscanf` 换手写解析 | 省 ≈9.1 KB | DMC1 协议与时钟解析要逐条回归 |
| 6 | `wallclock.cpp` 的 `nvs_open` 依赖 `menu.cpp` 先初始化 | 消除一个静默回退隐患 | 初始化顺序调整，需一次上机验证 |

---

## 8. 附录

### 8.1 复算命令

```powershell
# 体积
Get-Item firmware\digicore-c3\build\digicore-c3.bin | Select-Object Length
# 热函数大小
riscv32-esp-elf-nm -S --radix=d firmware\digicore-c3\build\digicore-c3.elf | Select-String 'execute_run|fill_run|write_run'
# AOT 页表利用率（每页 256 槽）
python -c "import re;src=open(r'firmware/digicore-c3/main/emu/aot_blocks.c',encoding='utf-8').read();..."
# 宿主测试
g++ -std=c++20 -O2 -DAOT_CONST_TABLES -I main/emu -I main/emu/compat -I main `
    -o st7735_test.exe hosttest\st7735_run_test.cpp main\emu\st7735.cpp; .\st7735_test.exe
```

### 8.2 本轮文件

| 类别 | 路径 |
|---|---|
| 固件 | `firmware/digicore-c3/main/app_main.cpp`、`menu.cpp`、`menu.h`、`dmc_link.cpp`、`CMakeLists.txt` |
| 固件 | `firmware/digicore-c3/main/emu/unsp_glue.cpp`、`st7735.cpp`、`gpce4.cpp` |
| 配置 | `firmware/digicore-c3/sdkconfig`、`sdkconfig.defaults` |
| 版本 | `VERSION`、`firmware/digicore-c3/version.txt`、`CHANGELOG.md`、`README.md` |
| 报告 | `docs/AUDIT_PERF_2026-11-27_R7.md`（本文件） |
| 分报告 | `docs/notes/R7_app.md`、`R7_core.md`、`R7_aot.md`、`R7_periph.md`、`R7_build.md` |
