# 第六轮：切换界面卡顿的归因与修复

日期：2026-11-26
基线：v1.13.0 → 本轮 **v1.14.0**
触发：用户报告「开机首次、息屏点亮或放置一会儿后按 B 切计时界面很慢；
慢速切换一次再切回数码兽，再切换就正常；清理便便的画面也一样」。

---

## 0. 结论先说

### 0.1 症状被复现成一个具体数字

复位后按 B 切换，**切换那一帧的 guest 执行时间是 78–116 ms**（正常帧 14–16 ms），
之后立刻回到 16 ms。用户感知的「慢」就是这一帧 —— 它不是持续掉帧，是全屏重绘
落在一帧里。

| 切换 | 修复前帧耗时 | 修复后 |
|---|---|---|
| 第 1 次（开机首次） | 98 ms | **73 ms** |
| 第 2 次 | 116 ms | **88 ms** |
| 第 3 次 | 78 ms | **72 ms** |

### 0.2 三层原因，已修前两层

1. **AOT 覆盖率缺口：切换瞬间 44 % 的 dispatch 是 miss。**
   实测一帧 5445 块、4372 次 miss。miss 最集中的两个 PC 是 `0x005F9B` / `0x005F9C`
   （32 位乘法例程的内部，各 12119 次），其次是 `0x203E14`（707）、`0x005B48`（384）。
   这些 PC 早在 AOT 表的去重里被别的块「覆盖」了，但没有自己的入口，跳进来就
   永远 miss。**修法**：把 miss census 的 top 200 并进 `tools/coverage/out/AB_force.txt`
   （691 → 1170 条），重新生成 `aot_blocks.c`（1466 → 1944 块）。
   **结果：切换帧的 miss 从 4372 / 6931 / 4760 降到 306 / 244 / 120（−93 %…−97 %）。**
2. **绘制代码和精灵数据抢同一个 16 KiB cache。**
   C3 的指令 cache 与数据 cache 是同一块（`CONFIG_SOC_SHARED_IDCACHE_SUPPORTED=y`）。
   切换帧要执行 6–9 千个 AOT 块、6–8 万条指令，这些块原先全在 flash，每块取指都把
   精灵数据挤出 cache。**修法**：把绘制热区 `0x005A00-0x005B60`（像素推送）、
   `0x005F80-0x005FC0`（乘法）、`0x207A00-0x207D00`（精灵绘制）、`0x230300-0x230340`、
   `0x200340-0x200350` 的 **146 个块搬进 IRAM**（连同原有 60 个，共 175 块 / 39 KB）。
   **结果：首帧 98 → 73 ms，第二帧 116 → 88 ms。**
3. **还剩的主体：ROM 图形数据本身要从 SPI flash 读。**
   修完前两层，切换帧仍是 6586 块 / 57034 指令要 81 ms ≈ **每块 12.3 µs、每指令 226
   个主机周期**（正常 3–4）。块体只有 8.6 条指令，代码已在 IRAM，剩下的只能是
   **数据读**：一个 96×96 精灵是 18 KB，比 16 KB 的共享 cache 还大，绘制时每 16 个
   像素就要补一次 32 B cache 行（DIO 下 ≈300 周期）。一个精灵 ≈576 次 miss ≈1 ms，
   全屏重绘几十个精灵就是 30–50 ms。
   **这一层没有修**：`SparseRom` 的 overlay 机制（4 KiB 页覆盖到 RAM）已经存在，
   可以用 64 KB 把绘制热数据段常驻，剩下的是选题和验证。

### 0.3 「切一次再切回来就正常」的解释

不是 guest 学会了什么。切换那一帧的 70–90 ms 是**一次性全屏重绘**，画完画面就静止，
下一帧回到 16 ms。第一次切、第二次切、第三次切都有这个长帧（实测 73/88/72 ms），
只是第一次叠加了启动/唤醒的冷状态，主观上更明显。用户说的「正常」是画面静下来之后
的帧率，不是切换动作本身。

---

## 1. 测量方法

### 1.1 长帧日志（新增）

`run_guest_frame()` 结束后，若该帧 guest 时间 > 60 ms，打一行：

```
LONGF us=98060 pc=005f8b slices=334 aotb=... aoti=... fallback=... miss=...
      dblk=5445 dins=58986 dmiss=4372 dfb=239 dtick=4864
```

- `dblk/dins`：该帧执行的 AOT 块数与指令数（前后差值）
- `dmiss/dfb`：该帧新增的 miss 与 fallback
- `dtick`：该帧花在 `soc.tick()` 里的微秒

有了它，「切换慢」从一句主观描述变成「第 N 帧、多少块、多少 miss、多少 tick」。

### 1.2 miss census（设备侧）

`-DC3_MISS_CENSUS=1` 构建，跑 8 次切换，控制台发 `M`，得到 top 200 miss PC。
本次 `MISSHEAD 925`，最高两个 `005F9B/005F9C` 各 12119 次。

### 1.3 block census（设备侧）

`-DC3_BLOCK_CENSUS=1` 构建，跑切换，发 `Y`，得到 top 200 块入口。
`0x200343/0x230321/0x230323` 各 36.9 万次（时钟循环），`0x00485D/0x0053DB/0x0054E6/0x004869`
各 3.8 万次（park），`0x005F9B` 1.9 万次（乘法）。

### 1.4 符号尺寸

`riscv32-esp-elf-nm --print-size`：1944 块共 448,704 B，平均 231 B；
IRAM 化后 175 块 39,120 B，DRAM 余 88,440 B。

---

## 2. 改动清单（v1.14.0）

| 项 | 位置 | 内容 |
|---|---|---|
| AOT 覆盖扩展 | `tools/coverage/out/AB_force.txt`、`main/emu/aot_blocks.c` | 并入 600 条 miss PC（合并后 1170 条），块数 1466 → 1944，原生长指令 14,847 → 18,126 |
| 绘制热区进 IRAM | `tools/coverage/out/aot_iram_r5.txt`、`aot_blocks.c` | 175 块 / 39 KB（原 60 块 / 15.9 KB） |
| 帧内归因日志 | `app_main.cpp` `run_guest_frame()` | `LONGF`，阈值 60 ms |
| 版本 | `version.txt` | 1.13.0 → 1.14.0 |

生成命令（可复现）：

```
AOT_CONST_TABLES=1 AOT_LOOSE=1 \
AOT_EXTRA=tools/coverage/out/AB_dev.txt \
AOT_IRAM_LIST=tools/coverage/out/aot_iram_r5.txt \
AOT_FORCE=tools/coverage/out/AB_force.txt \
AOT_OUT=firmware/digicore-c3/main/emu/aot_blocks.c \
python tools/aot.py
```

> 注意：`tools/aot.py` 读环境变量，**不要**跑 `tools/sync_c3_emu.py`——它会把设备侧的
> `unsp_glue.cpp` 用 PC 侧副本覆盖掉，本轮的 `m_insn_count` 补丁和 `LONGF` 都会丢。

---

## 3. 实测（COM4，v1.14.0）

复位 → 等 7 s → `J 1` → 每 5 s 按一次 B：

```
W LONGF us=73484 pc=207ca3 dblk=6926 dins=59747 dmiss=304 dfb=244 dtick=2644
W LONGF us=88073 pc=005f8b dblk=9457 dins=81906 dmiss=244 dfb=246 dtick=3057
W LONGF us=72448 pc=200343 dblk=7416 dins=61447 dmiss=152 dfb=225 dtick=3256
```

静止画面（时钟界面）：`guest_fps 56.2`、`gmax 15.8 ms`、帧间隔 p50 16 ms（v1.13.0 水平未退化）。

构建：app 1,139,280 B（`AECFDE24…`）；Flash Code 694,606 B；DRAM 232,856 / 321,296（余 88,440 B）。

---

## 4. 没解决的问题（下一轮）

1. **首帧 70–90 ms 仍在**。要再降，只能从「数据读」入手：
   - 方案 A：`SparseRom` overlay 把绘制热数据段（建议先量 `hle_blit_key` 的 `src` 地址分布）
     常驻 64 KB RAM，把每次 32 B 行 miss 变成 RAM 读。需要确认 overlay 区间与 4 KiB 粒度。
   - 方案 B：HLE 绘制循环里显式预读（一次读 32 B 到栈缓冲），把 cache miss 变成
     顺序预取——收益取决于 ROM 是否本来就顺序读。
   - 方案 C：QIO（本轮试过，启动失败，QE 位未解决）。
2. **切换首帧无法靠「降低帧预算」消除**：guest 的全屏重绘在一帧内完成，宿主只能让它
   更快，不能把它拆开。自适应预算的作用是让后续帧回到 16 ms，不是消除首帧。
3. **息屏点亮** 的路径没有单独测（需要等 16 s 自动息屏再按键）。本轮测的是复位后首次
   切换，量级一致；如果点亮时还有额外初始化，需要用同一套 `LONGF` 抓一次。

---

## 5. 回滚

- 固件：`git checkout v1.13.0 -- firmware/digicore-c3`（aot 文件与 force 列表在 git 里，
  回滚后重新生成一次 `aot_blocks.c`）。
- `aot_blocks.c` 生成前快照：`tmp/aot_blocks_r5_before.c`。
