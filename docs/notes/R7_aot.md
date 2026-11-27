# R7：AOT 表 / 代码生成冗余审计（第七轮）

- 审计对象：`firmware/digicore-c3/main/emu/aot_blocks.c`（4,648,540 B / 190,686 行，生成物）
- 生成器：`tools/aot.py`（1,098 行）
- 生成命令：`docs/AUDIT_PERF_2026-11-26_R6B.md` §7a
- 口径：**只读**。未重生成 `aot_blocks.c`、未改源码、未跑 `idf.py build`。分析脚本全部落在
  `%TEMP%\r7\`，仓库内只新增本文件。
- 数据时间轴：设备采集 2026-11-20（pc 直方图）→ 11-21（AB_dev）→ 11-26（census / force）→
  11-27（r6b 实测）。**块表本身在 11-26 23:07 之后只改过 `AOT_IRAM` 前缀，块体未变**，
  所以 11-20 的 PC 直方图对块集合仍然有效。

---

## 0. 结论先说

1. **「永不命中」的块：0 个是严格意义的零证据，37 个是「设备侧零证据」**（5,758 B）。
   这 37 个在 MAME/PC 侧采样（`AB_dev.txt`）里全部出现过，其中 34 个是 `AB_blocks.txt`
   里的静态函数入口 → 属于「罕见路径」，按本轮规则**不能删**。
2. **真正的大头不是死块，是重叠块**：1,944 块里 **1,097 块（56.4%）的每条指令都已被更早
   的块翻译过**，合计 **277,946 B（61.4% 的块体字节）**。其中 929 块在最新 block census 里
   零命中，共 **230,330 B**。删掉它们能省 **≈230–258 KB flash**，代价是 dispatch miss 从
   1.0 次/帧回到 50 次/帧（实测值），重绘帧 **+1.0%**、普通帧 **+4.9%**。
3. **入口表 155,648 B，槽位利用率 10.0%**（1,944 / 19,456），空槽 140,096 B。
   删块**不会**让表变小——除非整页 256 槽全空（省 2,048 B/页）。本轮删除候选里没有一页会清空，
   所以「删块省表」= 0 B。真正能省表的是把每页 ent 数组裁到 `[min_slot, max_slot]`：
   **155,648 → 91,016 B，省 64,632 B**（要改 `aot.h` 查表，见 §3.2）。
4. **dispatch 不会因为删死块变快**：死块从不进 lookup 的热路径，删了只是 flash 变短；
   `aot_lookup()` 的页缓存（128 槽 / 76 页，零冲突）与槽位密度无关，删块不改变查表指令数。
   唯一能改善 dispatch 的是删**会命中**的块——那会让 miss 变多，方向相反。
5. **IRAM 名单没有可安全删除的块**：208 块 / 44,294 B 与源码逐块一致（0 偏差），
   76 块在 census 里零命中但仍在 PC 直方图里执行（钩子 bail 路径，R6B 已用 28% 变慢的实测
   推翻过「按 census 删 IRAM」）。唯一可议的是 4 个 0 指令包装块（60 B，其中 `00486A` 在 IRAM）。
6. **生成器侧有 132 行死代码**（`AOT_GROUP` 超块机制，实测失败后默认关）、
   2 个无效选项、1 个死宏、1 个无人引用的常量，以及**生成物头部缺 provenance**
   （只记 `AB_blocks.txt`，不记 `AOT_EXTRA/AOT_FORCE/AOT_IRAM_LIST`，导致生成物不可溯源）。

### 摘要表（候选块集）

| # | 候选集 | 数量 | 块体字节（符号口径） | 命中证据 | 风险 | 建议 |
|---|---|---|---|---|---|---|
| A | 0 指令 fallback 包装块 | 16 | 228 B | 8 块有 census 命中（`0049DD` 1,231 次等），其中 `00486A`/`005B5D`/`230329`/`230334` 在 IRAM | 极低：删了回到 miss，少一次函数调用 | **删**（生成器侧跳过 `is_entry` 的 0-insn 块） |
| B | 设备侧零证据块（含 A） | 37 | 5,758 B | 只在 MAME 侧 `AB_dev.txt`；34 个是 `AB_blocks.txt` 静态入口；census/miss/pc 直方图全无 | 高：可能是设置卡/存档/开机路径 | **不删**，列入观察 |
| C | force 重叠块（census 零命中） | 929 | 230,330 B | 每条指令都已被更早块翻译；`AB_force.txt` 来源（miss census）；最新 census 零命中 | 中：删后 miss 回到 ~50/帧 | **待裁决**（省 230–258 KB，帧 +1%~+5%） |
| D | 非 force 重叠块（census 零命中） | 120 | 39,508 B | 同上，但不在 force 名单；入口是热 PC | 高：入口即热点，删了必 miss | **不删** |
| E | 入口表空槽 | 17,512 槽 | 140,096 B | ELF 段实测 | 需改运行时 | **待裁决**（区间裁剪省 64,632 B） |
| F | IRAM 零 census 命中块 | 76 | 13,644 B（IRAM） | 但 PC 直方图仍在执行 | 高（R6B 教训） | **不删** |
| G | 生成器死代码/死数据 | 132 行 + 3 项 | — | 源码 | 无 | 归档，不删 |

---

## 1. 审计口径与可复算命令

### 1.1 块集合怎么算出来的

`aot_blocks.c` 里每块有一行固定格式的注释，直接可解析：

```
// 00480F: 3 native instructions, 4 cycles worst case
AOT_IRAM static void aot_b00480F(AotCtx *c)
```

一行脚本（不写任何文件）：

```powershell
python -c "import re,io;t=io.open('firmware/digicore-c3/main/emu/aot_blocks.c',encoding='utf-8',errors='replace').read();m=re.findall(r'^// ([0-9A-F]{6}): (\d+) native instructions, (\d+) cycles worst case',t,re.M);print('blocks',len(m),'native',sum(int(b) for a,b,c in m),'iram',t.count('AOT_IRAM static void'))"
```

原始输出：

```
blocks 1944 native 18126 iram 208
```

与文件头注释 `762 block entries + 5067 hot PCs, 1944 registered, 18126 native instructions.`
和 `g_aot_block_count = 1944u` 三处一致。

### 1.2 字节数怎么算的

符号级（`nm -S`，能定位到每块）：

```powershell
$nm="C:\Users\DuWeisi\.espressif\tools\riscv32-esp-elf\esp-14.2.0_20251107\riscv32-esp-elf\bin\riscv32-esp-elf-nm.exe"
& $nm -S firmware/digicore-c3/build/digicore-c3.elf | Select-String 'aot_b' | Measure-Object
```

段级（`size -A`，含对齐填充，才是真占 flash 的数）：

```powershell
& riscv32-esp-elf-size -A firmware/digicore-c3/build/esp-idf/main/CMakeFiles/__idf_main.dir/emu/aot_blocks.c.obj |
  Select-String '\.text\._ZL11aot_b|\.iram1\.|\.rodata\._ZL1[12]aot_ent'
```

原始输出（聚合后）：

```
flash block fn : 1736 段  470,910 B
iram  block fn :  208 段   44,294 B
.rodata ent    :   76 段  155,648 B   (76 × 2,048)
.rodata g_aot_desc       608 B
.srodata g_aot_desc_n      4 B
.srodata g_aot_block_count 4 B
```

**两个口径差 62,484 B（flash 侧）**，是每函数 16 B 对齐的填充；下面凡是「省多少」都同时给两个口径。

### 1.3 命中数据

| 数据 | 文件 | 规模 | 语义 |
|---|---|---|---|
| PC 直方图（10 s） | `dbgtest/pc_hist_10s.txt` | 242,116,283 指令 / 5,310 PC | 设备执行过的**指令 PC** |
| PC 直方图（20 s） | `dbgtest/pc20_aot.csv` | 477,655,809 指令 / 7,437 PC | 同上，窗口更大 |
| 块直方图 | `dbgtest/blockhist10{,b,c}.txt` | 5,244 PC | `blk_hist[]`，块内非首指令计数 |
| 块 census | `dbgtest/r5_blkcensus.log` 等 20 份 | 最新 `BLKHEAD 801` | **dispatch 命中次数（按块）** |
| miss census | `dbgtest/r5_miss.log` / `miss_pcs.txt` | `MISSHEAD 925` | lookup 失败 PC（= 执行过但当时无块） |
| 生成器输入 | `AB_blocks.txt` 762 / `AB_dev.txt` 5,067 / `AB_force.txt` 1,170 | | 静态块 / hot PC / 强制入口 |

**关键语义**：`BLKHEAD n` 是那次采集里**命中过的 distinct 块数**（不是 top-N 行数）；
`r5_blkcensus.log` 打 363 行但 header 是 801，所以「census 未出现」≠「零命中」，
只能作为**弱证据**。因此下文把「census 零命中」与「PC 直方图零执行」分开算。

---

## 2. Q1：有多少块永不命中

### 2.1 三道筛子

| 筛子 | 判据 | 结果 |
|---|---|---|
| S1 | 入口 PC 不在任何 PC 直方图（`pc_hist_10s` ∪ `pc20` ∪ `blockhist*` ∪ `iram_hot` ∪ miss） | **179** 块 |
| S2 | S1 且不在 `AB_force.txt`（= 从未作为 miss 出现） | **37** 块 |
| S3 | S2 且不在 `AB_dev.txt` / `AB_blocks.txt`（= 连生成器输入都没有） | **0** 块 |

命令（S2/S3 的核心两行）：

```powershell
python "$env:TEMP\r7\census.py"    # 合并 20 份 census + 4 份 PC 直方图
python "$env:TEMP\r7\tiers.py"     # 分级
```

原始输出（节选）：

```
device-evidence PCs: 11182 ; AB_dev 5067 ; AB_force 1170 ; AB_blocks 762
A (no evidence at all)      :   0 blocks  bytes      0  native     0
B (only AB_dev/AB_blocks)   :  36 blocks  bytes   4998  native   245
C (in AB_force = miss census):   0 blocks  bytes      0  native     0
```

S2 的 37 块清单（`PC/native/字节`，全部 `iram=0`）：

```
0051BB/45/278  005217/9/184   005240/16/294  005251/3/128   005252/2/86
005254/1/62    005255/5/180   005259/1/34    00525A/13/248  00526B/1/34
00526D/1/68    00526F/1/68    005271/18/232  005289/122/760 00530D/18/176
005322/0/14    005323/5/150   00532B/17/248  00533F/13/232  005350/5/176
005365/16/294  005376/3/128   005377/2/86    005379/1/62    00537A/5/180
00537C/3/100   00537F/13/248  005390/1/34    005392/5/176   005398/5/176
0053A8/6/186   0053B0/1/34    200045/5/178   20004B/1/36    200058/1/36
20005F/1/72    200061/2/80
```

分布：页 `051:1`、`052:13`、`053:18`、`2000:5`，合计 **37 块 / 1,050 native / 5,758 B**。

> 判据细节：这里用的是「**入口 PC** 无任何执行证据」。若再加一层「块体任一指令也无证据」，
> 会变成 36 块——唯一被排除的是 `005289`，它的块体第 2 条指令 `00528E` 在 `pc_hist_10s.txt`
> 里出现过（因为另有块翻译了同一条指令，见 §4.1）。两个数字不矛盾，只是判据宽严不同；
> 本报告一律用 37，因为块是否被 dispatch 由入口决定，块体中段被执行不代表入口被 lookup 过。

### 2.2 为什么这 37 块不能删

- **34/37 在 `AB_blocks.txt` 里**，即静态反汇编认过的**函数入口**。unSP 的函数调用是
  `CALL16`（间接，目标在 r3/r4），静态不可达分析对它们无效——只能靠执行证据。
- **37/37 在 `AB_dev.txt` 里**，即 MAME/PC 侧采样执行过。说明游戏代码里真实存在这条路径
  （设置卡、存档、开机自检、或某个菜单分支），只是 C3 的采集窗口没走到。
- 0 个在 `AB_force.txt`：如果真的在 C3 上执行过而当时无块，必然出现在 miss census 里。
  这是很强的反证——**它们在设备上确实一次都没跑**。
- 但这正是「只在罕见路径执行」的定义。本轮规则明确：这类不能删。

### 2.3 与 R6B 的 IRAM 教训交叉验证

R6B 删了 67 块「落在 HLE 吸收区间」的 IRAM 块，实测慢 28%，原因是**钩子在 slice 边界
bail 之后，这些块要执行**，而 census 采不到 bail 路径。同样的陷阱适用于本节：
`census 零命中` 单独不能作为删除依据，必须是 `census 零命中 ∧ PC 直方图零执行 ∧ miss 零`。
按这三重条件筛，IRAM 里是 **0 块**。

### 2.4 附带发现：0 指令块（真·负收益）

16 块 `native=0`，块体只有 `AOT_I` + `c->fallback()` + `icount -= cyc`：

```c
// 00486A: 0 native instructions, 0 cycles worst case
AOT_IRAM static void aot_b00486A(AotCtx *c)
{
	int32_t cyc = 0;
	AOT_I(c, 0x00486Au);
	c->fallback(c->dev, c, 0x00486Au);
	c->icount -= cyc;
	return;
}
```

命中的代价 = lookup 成功 + 一次函数调用 + `fallback()`（内部再解释 1 条）+ return，
**比 miss 还贵**。它们来自 `AB_blocks.txt` 里不可翻译的静态入口（`is_entry=True` 分支
不做 `native < MIN_NATIVE_INSNS` 检查；force/hot 分支做了，所以 14 个 force PC 被正确跳过）。

| 块 | native | 字节 | IRAM | census 命中 |
|---|---|---|---|---|
| 00486A | 0 | 14 | **是** | 0 |
| 005B5D | 0 | 14 | **是** | 15,408 |
| 230329 | 0 | 16 | **是** | 10,869 |
| 230334 | 0 | 16 | **是** | 10,869 |
| 0049DD | 0 | 14 | 否 | 1,231 |
| 004A10 | 0 | 14 | 否 | 182 |
| 005F18 | 0 | 14 | 否 | 98 |
| 其余 9 个 | 0 | 14×9 | 否 | 0 |

`005B5D`/`230329`/`230334` 命中上万次 → 说明这几个 PC **不可翻译但极热**，每次命中都在
函数调用后立刻 fallback，等于白付一次调用。删块会让它们走 miss 路径（少一次调用），
**删了更快**。建议：生成器对 `is_entry` 也套 `native < 1` 跳过，或把这几个 PC 加进
`aot_fallback_one()` 的 HLE 快表。

---

## 3. Q2：入口表 / 描述表的体积、结构、在 flash 的哪一段

### 3.1 结构

`aot.h:315-346`（`AOT_CONST_TABLES` 分支）：

```c
typedef struct { AotFn fn; uint16_t cyc; uint8_t cnt; uint8_t pad; } AotEnt;      // 8 B
typedef struct { uint16_t page; const AotEnt *ent; } AotPageDesc;                // 8 B（2+4，对齐到 8）
```

- **每页固定 256 项**，未用槽位写 `{ nullptr, 0, 0, 0 }`（`tools/aot.py:1028` 的注释明确说
  因为 minGW 不接受指定初始化器，只能整表铺开）。
- 76 页 → `76 × 256 × 8 = 155,648 B`。
- `g_aot_desc` 76 项 × 8 B = **608 B**。
- `g_aot_desc_n` + `g_aot_block_count` = 8 B（`.srodata`）。
- **表合计 156,264 B**。

ELF 核对：

```
$ riscv32-esp-elf-nm -S build/digicore-c3.elf | grep -E "g_aot_desc|aot_ent_048"
3c0ebc38 00000800 d _ZL11aot_ent_048     # 2,048
3c0c61d8 00000260 D g_aot_desc           # 608
3c0c61d4 00000004 D g_aot_desc_n
```

`size -A` 逐段求和：76 个 `.rodata._ZL1[12]aot_ent_*` 各 2,048 B，合计 155,648 B。

### 3.2 在 flash 的哪一段

`build/digicore-c3.map`：

```
.flash.rodata   0x3c0b0120   0x4efa0     # 323,488 B，DROM 映射
.flash.text     0x42000020   0xa981c     # 694,300 B，IROM 映射
.iram0.text     0x40380000   0x1d236     # 119,350 B
```

- 表在 **`.flash.rodata`（DROM）** 内，起始 `0x3c0c61d8` → flash 内偏移
  `0x3c0c61d8 - 0x3c000000 + 0x10000 = 0x0D61D8`（＝877,528 B）。
- 块体在 **`.flash.text`（IROM）**，符号地址区间 `0x4200xxxx`–`0x4207F00A`；
  IRAM 块在 `.iram1.*` → `0x4038450E`–`0x40389Dxx`。
- app bin 1,144,944 B / 分区 2,097,152 B = **54.6%**。
  AOT 占 app bin 的 `(470,910 + 44,294 + 156,264) / 1,144,944 = 58.6%`。

### 3.3 槽位密度（决定「删块能不能省表」）

```
pages 76  slots used 1944 / 19456 (10.0%)  full table 155648 B
sparsest: 200B 1块(F3)  2005 2块(F3..FF)  200A 2块(06..07)
densest : 2040 167块(00..FF)  05D 79块(4D..FC)  2073 65块(25..FE)
```

**删块省表的唯一条件是整页 256 槽全空**（省 2,048 B）。本轮所有候选集都做不到：

- 删 37 个零证据块 → 页 051/052/053/2000 分别还剩 18/1/9/42 块 → **省 0 B**；
- 删 929 个 force 重叠块 → 只有页 `200D` 会清空 → **省 2,048 B**；
- 删全部 1,097 个重叠块 → 只清空页 `200D` 一页 → **省 2,048 B**。

所以「删块省 flash」几乎全部来自块体，不是表。

### 3.4 表本身的结构性冗余：区间裁剪省 64,632 B

每页 ent 数组的 `[min_slot, max_slot]` 跨度合计 **11,377 槽**（不是 19,456）：

```
tight [min,max] span: 11377 slots -> 91016 B (saves 64632 B with a base_slot field)
```

改法（`aot.h`）：

```c
typedef struct AotPageDesc { uint16_t page; uint8_t base; uint8_t n; const AotEnt *ent; } AotPageDesc; // 8 B，不涨
...
const unsigned slot = lpc & 0xffu;
if (slot < d->base || slot >= d->base + d->n) return nullptr;   // 2 次比较，取代原来的空指针判断
const AotEnt *e = &d->ent[slot - d->base];
```

- 省 **64,632 B** flash（155,648 → 91,016），`g_aot_desc` 仍是 608 B（结构体填满 8 B）。
- 代价：每次 dispatch 多 2 条比较（区间外直接 nullptr），热路径实际执行的是 `slot < base`
  与 `slot >= base+n` 两次无分支可预测比较，实测成本应在 1–2 cycles（**推测**，未上机）。
- **注意**：这会让 `ent` 指针不再 256×8 B 对齐，`page & 127` 缓存的假设不变，但
  `AotEnt` 的 32 B cache line 覆盖从「固定 4 槽」变成「跨页边界」，理论上更差；需要实测。

**dispatch 查表会不会变快？不会。** `aot_lookup()` 在 `AOT_CONST_TABLES` 下是：

1. `pslot = page & 127` → 读 `g_aot_pg_tag`（DRAM，3 cycles）；
2. 命中则直接 `d = g_aot_pg_ref[pslot]`；未命中才二分（76 项，7 次比较，冷路径）；
3. `e = &d->ent[lpc & 0xff]` —— **一次基址+偏移 load，与槽位密度无关**。

76 页 < 128 槽 → 页缓存**零冲突**，二分永不重复。删块（哪怕删到 70 页）只让二分少一次比较，
而这本来就只在页切换时发生。**删块对 dispatch 的收益 = 0，对 I-cache 的收益 = 0**
（死块不执行，不进 cache）。

---

## 4. Q3：代码生成层面的冗余

### 4.1 重叠块：1,097 块 / 277,946 B，占块体 61.4%

同一 guest 指令被多个块重复翻译：

```
== how many blocks translate the same instruction PC ==
   1 block(s):  4513 PCs
   2 block(s):   623 PCs
   3 block(s):   303 PCs
   ...
  56 block(s):     1 PC      <- 200343，被 56 个块各翻译一遍
unique instruction PCs: 6619 ; instances: 18223 ; duplication factor 2.75x
```

`200343`（重绘帧最热 PC，census 命中 1,418,662 次）周边 0x2002F4–0x200343 这段循环，
被 **56 个不同起点**各翻译了一遍。原因在 `tools/aot.py:876`：

```python
if not is_entry and entry in covered and entry not in force:
    continue
```

去重只看**入口 PC 是否已被覆盖**，不看入口之后的指令是否重复。所以只要一个 hot PC 落在
别的块**外部**（分支目标），它就新建一整块，块体与主块重叠。

按「块内每条指令都已有更早起点翻译」定义**完全重叠块**：

| 分类 | 块数 | 字节（符号） | census 命中 | 说明 |
|---|---|---|---|---|
| 重叠 ∧ force ∧ 零命中 | **929** | **230,330 B** | 0 | force 名单生成，但最新 census 没命中 |
| 重叠 ∧ force ∧ 有命中 | 11 | 1,804 B | 有 | 真在干活 |
| 重叠 ∧ 非 force ∧ 零命中 | 120 | 39,508 B | 0 | hot PC 重叠，入口即热点 |
| 重叠 ∧ 非 force ∧ 有命中 | 37 | 6,304 B | 有 | 真在干活 |
| **合计** | **1,097** | **277,946 B** | 48 | |

### 4.2 删重叠块的收益与代价（分开算）

**flash 收益**（929 块）：

```
符号口径 230,330 B；段口径 230,330 + 929×36（每函数对齐填充）= ≈263,774 B
占 app bin 1,144,944 B 的 20.1%–23.0%
```

**dispatch / miss 影响**（实测，不是推算）：

| 构建 | 日志 | frames | aotb | aotmiss | miss/帧 |
|---|---|---|---|---|---|
| force **前** | `r5_bswitch.log`（18:48） | 1,189 | 1,587,934 | **59,542** | 50.1 |
| force **后** | `r5_after_force.log`（18:57） | 1,236 | 1,672,924 | **1,232** | 1.0 |
| v1.14.0 | `r5_v114.log`（19:07） | 1,091 | 1,451,607 | 994 | 0.9 |

删 929 块 = 把 miss 从 1.0/帧 推回 50/帧。单次 miss 代价 =
lookup 失败（几十 cycles）+ 解释器 1 条（**403 host cycles**，`docs/C3_PORT.md:287`）+
重新 dispatch（`1,107`，用户给定）≈ **1,500 cycles**。

- 普通帧：1,353 dispatch/帧 × 1,107 = 1.50 M cycles；+49 × 1,500 = **+73,500 cycles = +4.9%**
- 重绘帧：6,788 × 1,107 = 7.5 M cycles；同样 +73,500 = **+1.0%**（≈0.46 ms @160 MHz）

**结论**：929 块是「用 230 KB flash 换 4.9% 普通帧 / 1.0% 重绘帧」。这是本轮唯一有量级的
取舍项，建议主审裁决；**不建议在没有 A/B 实测前直接删**。

### 4.3 碎块：466 块 / 29,426 B，但不能一刀切

| native | 块数 | 字节 | 以 `aot_set_pc` 结尾 | 以 fallback 结尾 | census 命中 |
|---|---|---|---|---|---|
| 0 | 16 | 228 | 0 | 16 | 8 |
| 1 | 264 | 13,230 | 207 | 15 | 71 |
| 2 | 202 | 16,196 | 149 | 12 | 26 |
| 3 | 184 | 23,414 | 162 | 6 | 44 |

- 1 指令块里命中最高的是 `200343`（1,418,662 次，62 B）和 `230321`（1,418,276 次，84 B）。
  这类块**必须留**：命中一次省下 403 cycles 的解释器开销，成本只有一次 dispatch（~68 cycles，
  `aot.py:907` 注释）。
- `aot.py` 自己的负结果（`:909-910`）说的是 **JMPF 跳转块**：加 `0x207A57/0x207A73` 两个短跳转块
  让 fps 从 36.9 掉到 34.3。所以「短块不划算」只对**以控制流结尾**的块成立。
  当前 1 指令块里 207/264 是以 `aot_set_pc` 结尾的跳转块——这些才是该审的，不是全部 264 个。
- 0 指令块见 §2.4，是唯一「删了更快」的一类。

### 4.4 `AOT_NO_HOOKS` 下永远不会走的钩子分支

`aot.h:296-310`：

```c
#ifdef AOT_NO_HOOKS
#undef AOT_I
#undef AOT_IH
#undef AOT_CYC
#define AOT_I(c, pc) ((void)0)
#define AOT_IH(c, pc) ((void)0)
#define AOT_CYC(c, pc, n) ((c)->icount -= (n))
#endif
```

CMake 设备构建带 `-DAOT_NO_HOOKS`，所以生成物里 18,126 处 `AOT_I/AOT_IH`、10,705 处
`AOT_CYC` 在预处理后**全部消失**，`insn_hook`/`blk_hist` 分支不进二进制。

但 `aot.h` 里 `AotCtx` 仍有 4 个钩子字段（`insn_hook/insn_opaque/cycle_hook/cycle_opaque`
+ `blk_hist`，共 **20 B DRAM**），设备构建永不赋值、永不解引用；`aot_step()` 里
`m_block_hist` 的每 dispatch 判空是 R7_core 的 R7-07 条目。AOT 侧能用的是
`AotEnt.pad`（8 B 结构里那个 `uint8_t pad`）——R6B 的 W1 方案，gen 时写入 hook_id，
`aot_step()` 用一次字节读替代 5 条 PC 比较链。**本轮不动，只记录它是唯一零成本的表内空洞**。

### 4.5 局部化比例（供参考，不是冗余）

```
localised 1278 blocks / 399652 B, avg 313 B ; plain 666 blocks / 53068 B, avg 80 B
bytes per native insn: localised 23.6, plain 43.5
```

1,278 块走 `AOT_LOAD_REGS` 版本（≥4 指令），666 块走 `c->r[]` 版本。后者每指令字节开销
高 84%，但都是 1–3 指令的短块，`AOT_LOCAL_MIN` 4→10 的实测（`docs/DELIVERY_2026-11-22.md:286`）
证明调阈值会变慢，**现状是对的**。

---

## 5. Q4：IRAM 名单是否还有白占

### 5.1 现状核对

- `tools/coverage/out/aot_iram_r6.txt`：**208 条**，无注释、无重复。
- `aot_blocks.c` 里 `AOT_IRAM static`：**208 个**，与名单**逐 PC 一致，0 偏差**（脚本核对）。
- ELF：208 个 `.iram1.*` 段，合计 **44,294 B**，地址 `0x4038450E`–`0x40389Dxx`。
- **纠偏 1**：`docs/AUDIT_PERF_2026-11-26_R6B.md:237` 写「141 条 / 23,682 B」，那是**删 67 块
  后的中间状态**；回滚后名单是 208 条，同文档 §7a 也写了「208 块 / 44,294 B」。141 这个数字
  已经过期。
- **纠偏 2**：本轮任务书写「`.iram0.text` 约 98.7 KB，静态剩余约 103 KB」。`build/digicore-c3.map`
  现在是 `.iram0.text 0x1d236 = 119,350 B`（R6B 之后又加了 20.6 KB 代码）。IRAM 比任务书更紧，
  AOT 块占其中 **37.1%**。

### 5.2 有没有「已不执行还占着 IRAM」的块

| 判据 | 结果 |
|---|---|
| census 零命中 | 76 块 / 13,644 B |
| PC 直方图零执行 | 0 块 |
| 三重筛子（census ∧ 直方图 ∧ miss 全零） | **0 块** |
| 0 指令包装块 | 4 块 / 60 B（`00486A` 14、`005B5D` 14、`230329` 16、`230334` 16） |

- 76 块 census 零命中里包括 `00480F`、`004813`、`00481A`…`005AD9`–`005AF8`、`207BEB`–`207BF5`
  等。它们在 PC 直方图/`iram_hot.txt` 里**有**执行记录，属于 HLE 钩子 bail 后要跑的块，
  R6B 已用「删 67 块慢 28%」的实测否决过按 census 删 IRAM 的做法。**不要重复这个错误。**
- 唯一可议的是 4 个 0 指令块：`00486A` 在 IRAM 且 census 0 命中、每次命中都调 `fallback()`。
  移出 IRAM 只省 14 B，收益可忽略；但如果按 §2.4 删掉这个块，14 B 顺带回收。

**结论：IRAM 名单没有可安全删除的白占块（0 块 / 0 B）。** 44,294 B 全部有执行证据。
真要腾 IRAM，方向是 R6B 的 W1（`AotEnt.pad` 存 hook_id，省下 5 条比较链）和 R7_core 的
R7-07（去掉 `m_block_hist` 判空），不是删块。

---

## 6. Q5：生成器侧的默认包袱

### 6.1 死代码：`AOT_GROUP` 超块机制（132 行）

`tools/aot.py:690-821`（`exit_targets` / `groupable` / `groupify` / `build_groups`）+ `:919-925`、
`:954-1005`、`:1013-1018`、`:1086-1089`。默认 `GROUP=False`，生成命令从未带 `AOT_GROUP`，
`aot_blocks.c` 里 `superblock` 出现 **0 次**、`goto LAB_` **0 次**。

历史结论（`docs/RESEARCH_PERF_ROUTES_2026-11-24.md:95`）：
> 分组超块（`AOT_GROUP`）覆盖率 12.1%，`caot` 39.7%→40.0%；「唯一入口」条件把循环体排除；
> 组函数变大后 `-O3` 寄存器分配变差。

**建议**：保留代码（可复现负结果），但在 `AOT_GROUP` 分支上加一行注释指回该结论，
避免下一轮再试一次。

### 6.2 无效 / 冗余选项

| 项 | 位置 | 状态 |
|---|---|---|
| `AOT_NO_EXTRA` | `aot.py:42` | 与 `AOT_EXTRA=""` 完全等价；只有 `sync_c3_emu.py:150` `pop` 它，从没 `set` 过 → **无效** |
| `AOT_GROUP_BLOCKS` / `AOT_GROUP_INSNS` | `aot.py:79-80` | 只在 `GROUP` 为真时读 → 随 6.1 一起沉睡 |
| `AOT_IRAM_DATA` | `aot_blocks.c:9,12` | 定义了 `__attribute__((section(".dram0.data")))`，**全文 0 处使用** → 死宏 |
| `#include <cstring>` | `aot_blocks.c:5` | 只有非 `CONST_TABLES` 的 `aot_init()` 用 `std::memset`；设备构建白包含 |
| `g_aot_block_count` | `aot_blocks.c` 末尾 | `extern` 声明在 `aot.h:344`，**固件里 0 处引用** → 4 B 死数据 |
| `AOT_MIN`（不存在） | — | `MIN_NATIVE_INSNS = 1` **硬编码**，无法用环境变量调；而 `aot.py:907-910` 的注释说 1–2 指令块不划算，两者矛盾（实际只对跳转块成立，见 §4.3） |

### 6.3 生成物头部缺 provenance（可复现性缺陷）

```c
// AUTO-GENERATED by tools/aot.py -- do not edit by hand.
// Source: tools/coverage/out/AB_blocks.txt, roms/*.bin
```

只记了 `AOT_BLOCKS` 的默认值。实际生成还依赖 `AOT_EXTRA=AB_dev.txt`、
`AOT_FORCE=AB_force.txt`、`AOT_IRAM_LIST=aot_iram_r6.txt`、`AOT_CONST_TABLES=1`、`AOT_LOOSE=1`。
R6B 之所以要在文档里手抄命令（§7a），就是因为生成物不自证。**建议**把这几项和环境变量
哈希写进头注释（1 行，不占 flash——注释在预处理后消失）。

### 6.4 生成时才用、运行时不参与的大块

- `_AOT_CYC_RE`、`_SET_PC_RE`、`note()`/`REASONS`：只在生成期用，不进二进制（正常）。
- `aot.py` 里 `REASONS` 统计（`:1090-1093`）只在 `--` 打印，从不写入生成物 → 覆盖率报告
  每次都要重跑生成器才能看到截断原因。这是「生成时才用」里最可惜的一项。

---

## 7. 不要动清单

| 对象 | 理由 |
|---|---|
| `200343`、`230321`、`004869`、`0054E6` 等 1–2 指令热点块 | census 命中 50 万–140 万次；删了每次多付 403 cycles |
| HLE 吸收区间内的 67 块（`0x207CA3-0x207CC4`、`0x005B1E-0x005B2E`、`0x005B4A-0x005B5C`、`0x005F8B-0x005F9A`、`0x207A3E-0x207D0F`） | R6B 实测删了慢 28%，bail 路径要执行 |
| 37 个设备零证据块（§2.1 清单） | 34 个是静态函数入口，MAME 侧走过；属「罕见路径」 |
| 120 个非 force 重叠块 | 入口即热 PC，删了必 miss |
| 76 个 IRAM 零 census 命中块 | PC 直方图仍在执行；census 采不到 bail |
| `AOT_LOCAL_MIN = 4` | 4→10 实测 `gmax` 46→56 ms（`DELIVERY_2026-11-22.md:286`） |
| `AotEnt` 的 8 B 布局 | fn/cyc/cnt 同行是拿 17 fps 换来的（`aot.h:321-330`） |

---

## 8. 待裁决项

| # | 事项 | 收益 | 代价 | 建议动作 |
|---|---|---|---|---|
| **D1** | 删 929 个 force 重叠块（`AB_force ∩ 完全重叠 ∩ census 零命中`） | flash **230–258 KB**（−20% app bin） | miss 1.0→50/帧；普通帧 +4.9%，重绘帧 +1.0% | 先做 A/B：保留 force 名单但生成时跳过 929 个，上机测 `ft`/`gmax` 与 `aotmiss` |
| **D2** | 入口表区间裁剪（`[min_slot,max_slot]` + base/n） | flash **64,632 B** | 改 `aot.h` 查表 +2 比较；`AotEnt` 跨行，需实测 | 单独一轮，与 D1 互不依赖 |
| **D3** | 16 个 0 指令块（含 4 个 IRAM） | 228 B flash + 60 B IRAM + 少量 dispatch | 无（删了走 miss，更快） | **可以直接做**；生成器 `is_entry` 分支加 `native < 1 → skip` |
| **D4** | 把「重叠块」改成「入口桩」（`goto L_<pc>` 进主块） | 理论上 277,946 → ~35 KB，省 **243 KB**，且**不损失任何命中** | 需要块内标签 + 局部化形式一致（`AOT_GROUP` 的 `groupify` 已有现成实现，只是准入条件要放宽到「入口在任何块内」） | 比 D1 更优的长期方案；但它改的是生成器结构，超出本轮只读范围 |
| **D5** | IRAM 里 4 个 0 指令块 + `005FBC-BF` | 约 650 B IRAM | 低（有 census 命中，但都很低） | 与 D3 一起做，需先确认 `005FBC-BF` 不在 bail 路径 |
| **D6** | 清理 `AOT_NO_EXTRA` / `AOT_IRAM_DATA` / `g_aot_block_count` / `<cstring>` | 8 B flash + 可读性 | 无 | 顺手 |
| **D7** | 生成物头注释写全 provenance | 0 B（注释） | 无 | 顺手，防下一轮再靠文档手抄命令 |

---

## 9. 附录：复算命令与原始输出

### A. 块集合与字节

```powershell
# 块数 / 指令数 / IRAM 块数
python -c "import re,io;t=io.open('firmware/digicore-c3/main/emu/aot_blocks.c',encoding='utf-8',errors='replace').read();m=re.findall(r'^// ([0-9A-F]{6}): (\d+) native instructions, (\d+) cycles worst case',t,re.M);print('blocks',len(m),'native',sum(int(b) for a,b,c in m),'iram',t.count('AOT_IRAM static void'))"
# -> blocks 1944 native 18126 iram 208

# 每块符号字节
& $nm -S firmware/digicore-c3/build/digicore-c3.elf | Select-String 'aot_b' |
  ForEach-Object { $p=$_ -split '\s+'; [pscustomobject]@{pc=$p[3]; size=[Convert]::ToInt64($p[1],16)} }
# -> 1944 条，合计 452,720 B（flash 408,426 + iram 44,294），min 4 / max 1,234 / mean 232.9
```

### B. 重叠块

```powershell
# 每个块翻译了哪些指令 PC（从 AOT_I/AOT_IH 的立即数取）
python -c "import re,io;t=io.open('firmware/digicore-c3/main/emu/aot_blocks.c',encoding='utf-8',errors='replace').read();b=re.split(r'^// [0-9A-F]{6}: ',t,flags=re.M)[1:];pcs=[set(int(x,16) for x in re.findall(r'AOT_I[H]?\(c, 0x([0-9A-F]{6})u\)',s)) for s in b];print('blocks',len(pcs),'unique',len(set().union(*pcs)),'instances',sum(len(s) for s in pcs))"
# -> blocks 1944 unique 6619 instances 18223  (2.75x)
```

### C. census 合并

```powershell
# 20 份 *census*.log / *.log 的 BLKHEAD + BLK 行
Get-ChildItem dbgtest/*.log | Where-Object { (Select-String -Path $_.FullName -Pattern BLKHEAD -Quiet) } |
  ForEach-Object { $_.Name }
# -> back1 blk2 blk3 budget1 fasthot2 final2 fix1 fix2 fix3 hle1 hle3 hle4 hle5 hle6 hle7
#    iram60 miss3 os1 r5_blkcensus r5_switchcensus
# 最新一次：r5_blkcensus.log  BLKHEAD 801，打印 363 行
```

### D. 表体积与位置

```powershell
& $nm -S firmware/digicore-c3/build/digicore-c3.elf | Select-String 'g_aot_desc|aot_ent_'
# -> 3c0c61d8 00000260 D g_aot_desc ; 3c0ebc38 00000800 d _ZL11aot_ent_048 ...
& riscv32-esp-elf-size -A .../aot_blocks.c.obj | Select-String 'rodata\._ZL1[12]aot_ent_'
# -> 76 × 2,048 B
Select-String -Path firmware/digicore-c3/build/digicore-c3.map -Pattern '^\.flash\.(text|rodata)|^\.iram0\.text'
# -> .flash.text 0x42000020 0xa981c ; .flash.rodata 0x3c0b0120 0x4efa0 ; .iram0.text 0x40380000 0x1d236
```

### E. 中间产物位置（不在仓库内）

```
%TEMP%\r7\parse_blocks.py     blocks.tsv / block_pcs.json
%TEMP%\r7\cross.py            第一版交叉（仅 PC 直方图）
%TEMP%\r7\tiers.py            分级（A/B/C）
%TEMP%\r7\census.py           census.json（20 份合并）
%TEMP%\r7\join_nm.py          syms.json（ELF 字节）
%TEMP%\r7\pages.py            页密度与删块效果
%TEMP%\r7\shapes.py           重叠/碎块/局部化
%TEMP%\r7\shadow.py           shadow.json（929/120/11/37 分解）
%TEMP%\r7\audit_iram.py       IRAM 名单核对
```

---

## 10. 不确定度

- `census 零命中` 只代表**那一次采集场景**没命中（`r5_blkcensus` 19:00，切换帧）。重绘帧的
  块集与切换帧不同，所以 929 个 force 块里有多少服务重绘帧，本轮无法从现有日志分离——需要
  一次带 `C3_BLOCK_CENSUS=1` 的**重绘帧**采集（`R` 键十连）才能定死 D1。
- 「段口径 263,774 B」里的每函数 36 B 对齐是均值推算（62,484 / 1,736），删块后实际回收
  取决于链接器重排，±5%。
- `AotEnt` 区间裁剪的 +2 比较成本是**推测**，未上机。
- 37 个零证据块里 `005289`（122 指令 / 760 B）、`0051BB`（45 / 278 B）是最大的两个，内容为
  「批量寄存器初始化 + set SR + load」序列，**推测**是内部 ROM 的初始化/未走到例程，
  未做反汇编确认。
