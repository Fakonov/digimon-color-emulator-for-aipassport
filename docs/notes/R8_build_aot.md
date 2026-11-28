# R8 构建配置与 AOT/数据审计员 审计

日期：2026-11-28
基线：`firmware/digicore-c3/build/digicore-c3.elf`（16,530,196 B，2026-11-28 16:16:02）、
`digicore-c3.bin`（1,134,032 B）、`digicore-c3.map`（3,318,081 B）、`build.log`（2,762,621 B）。
角色：构建配置与 AOT/数据审计员。**只读**：未改任何 `.c/.cpp/.h/.py/.txt/sdkconfig`，未跑 `idf.py build/size/reconfigure`，
未刷机/开串口/git。唯一写入的文件就是本文件。工具只用 `riscv32-esp-elf-{nm,size,readelf,objdump}` + python（脚本落在 `%TEMP%`）。

---

## 0. 结论摘要

1. **R7 的三本账数字全部复核一致（脚本重跑，非照抄）。** AOT 入口表 155,648 B、空槽 140,096 B、
   重叠块 1,097 块 / 277,946 B（symbol 口径）全部复现。当前 v1.18.0：app bin **1,134,032 B**（2 MB 分区余 **963,120 B**）、
   DRAM 静态 **234,104 B**（IRAM .text 118,664 + .bss 108,192 + .data 7,248）、heap 余 **87,192 B**（27.1%）。
   三本账里最紧的仍是 DRAM（每帧预算与 16 KB icache 更紧，但那是别的路的账）。

2. **LTO 确认从未生效，且本轮不建议开。** `sdkconfig.defaults:33` 写了 `CONFIG_COMPILER_LTO=y`，但
   `sdkconfig`（唯一权威生成文件）里**没有这条**、`build.ninja` 里 **0 处 `-flto`**、`compile_commands.json`
   的编译命令里**没有 `-flto`**。根因是 ESP-IDF 的 `sdkconfig.defaults` 只在 `sdkconfig` 首次生成时播种，
   之后 defaults 被忽略；而这条是后加的。收益判断见 §2.5。

3. **AOT 两项给出明确判断：都「做」，都 P1（需 A/B）。** 入口表 `[min_slot,max_slot]` 裁剪省 **64,632 B**，
   且 `AotPageDesc` 现有 2 字节 padding 正好装下 `base/n`（结构体不涨，`g_aot_desc` 仍 608 B）；
   重叠块「入口桩」省 **242,842 B**（277,946 → 35,104 B），复用 `tools/aot.py` 里现成的 `groupify` 标签机制，
   命中率零损失。具体路径见 §2.1/§2.2。

4. **开关型冗余（WiFi/BT/TLS/OTA/SPIFFS/FATFS/LWIP）继续净贡献 0 字节。** map 链接区里
   `libesp_wifi.a(`/`liblwip.a(`/`libspiffs.a(`/`libfatfs.a(`/`libesp-tls.a(`/`libesp_https_ota.a(` 的成员数**全部为 0**。
   改成 `=n` 只省构建时间，不写进成绩单。

5. **生成物冗余是「磁盘占用」问题，不是「git 膨胀」问题。** `.gitignore` 已经把 `*.avi`/`/*.png`/`*.exe`/
   `firmware/*/build.log`/`firmware/*/sdkconfig.bak-*`/`dist/`/`tmp/` 全部覆盖。真正该动手的是**磁盘清理**：
   根目录 avi/png **410 MB**、`tmp/` **2.1 GB**、`dist/` 11.7 MB、`build.log` 2.7 MB、`sdkconfig.bak-wifion` 约 85 KB。
   唯一要小心的是 `.gitignore` 是 GBK 编码（我读出来是乱码），见 §2.7。

---

## 1. 范围与方法

### 1.1 读了哪些文件

| 类别 | 文件 |
|---|---|
| 配置 | `firmware/digicore-c3/sdkconfig`（2019 行）、`sdkconfig.defaults`（32 行）、`sdkconfig.bak-wifion`（2019 行，diff 对比）、`partitions.csv`、`version.txt` |
| 构建 | `CMakeLists.txt`（顶层）、`main/CMakeLists.txt`、`build.ps1`、`.gitignore` |
| AOT 运行时 | `main/emu/aot.h`（414 行全读）、`main/emu/hotblock.h` |
| AOT 生成物 | `main/emu/aot_blocks.c`（**只读头部 + 脚本统计，未整读**） |
| 生成器 | `tools/aot.py`（1–95、680–829、860–1098 段） |
| 历史 | `docs/notes/R7_build.md`（582 行）、`docs/notes/R7_aot.md`（620 行）、`docs/AUDIT_PERF_2026-11-27_R7.md` |
| 复用 R7 中间脚本 | `%TEMP%\r7\{parse_blocks,shadow,shapes,join_nm}.py` 及 `blocks.tsv/block_pcs.json/syms.json/census.json`（仍存在，直接重跑） |

### 1.2 跑了哪些只读命令（可复算）

```powershell
$T="C:/Users/DuWeisi/.espressif/tools/riscv32-esp-elf/esp-14.2.0_20251107/riscv32-esp-elf/bin"
$B="C:/Users/DuWeisi/digimon-color-passport/firmware/digicore-c3/build"
$PY="C:/Users/DuWeisi/py311/python.exe"

# 1) 段表（权威）
& $T/riscv32-esp-elf-readelf.exe -S $B/digicore-c3.elf
# 2) AOT 块/表/重叠（自写脚本，见 §A）
& $PY %TEMP%\r8_aot_recompute.py
# 3) 每块符号字节（nm）
& $T/riscv32-esp-elf-nm.exe -S --radix=d $B/digicore-c3.elf | Select-String 'aot_b'
# 4) aot_blocks.c.obj 段级尺寸
& $T/riscv32-esp-elf-size.exe -A $B/esp-idf/main/CMakeFiles/__idf_main.dir/emu/aot_blocks.c.obj
# 5) 组件成员数（map）
Select-String -Path $B/digicore-c3.map -Pattern 'libesp_wifi\.a\(|liblwip\.a\(|...' | Measure-Object
# 6) 堆/DRAM
& $T/riscv32-esp-elf-nm.exe $B/digicore-c3.elf | Select-String '_heap_start|_heap_end|_bss_end'
# 7) libc 符号
& $T/riscv32-esp-elf-nm.exe -S --radix=d $B/digicore-c3.elf | Select-String '__ssvfscanf_r|_vfprintf_r|dtoa|esp_err_to_name'
# 8) 断言/sscanf 调用点
& $T/riscv32-esp-elf-objdump.exe -d $B/digicore-c3.elf | Select-String '<__assert_func>' | Measure-Object  # 317
```

---

## 2. 发现表

| ID | 位置(文件:行) | 现象/证据 | 影响机制 | 估算 | 与R7关系 | 建议改动 | 风险 | 验证 | 优先级 |
|---|---|---|---|---|---|---|---|---|---|
| R8B-01 | `aot_blocks.c:185589-190686` | 入口表 76 页 × 256 槽 × 8 B = **155,648 B**，登记 1,944 条，空槽 **140,096 B**（脚本重跑） | `.flash.rodata` 的 48.1%、镜像 13.6%，只稀释 cache 密度 | 复用 R7 口径，无偏差 | 复核 R7B-01 | 见 R8B-02 | — | §A | P1 |
| R8B-02 | `tools/aot.py:1025-1041` + `aot.h:332-336` | 入口表裁剪：tight span 11,377 槽 → 91,016 B，省 **64,632 B**；`AotPageDesc` 现有 `uint16_t page; const AotEnt* ent`（6 B 对齐到 8 B，2 B padding）加 `uint8_t base,n` **正好填满，不涨 608 B** | 省 flash rodata | 64,632 B | 升级 R7A E/D2 | **做**：生成器按页输出 `[base..max]` 紧凑数组 + `g_aot_desc` 带 base/n；`aot_lookup` 改 `slot` 范围判 | 热路径多 1 次无符号比较；AotEnt 跨 32 B cache line | 改生成器→宿主四窗口 trace→真机 ft/gmax | P1 |
| R8B-03 | `tools/aot.py:876,914` + `shadow.py:33` | 重叠块 1,097 块 / **277,946 B**（symbol，native 11,560）：块内每条指令都已被更小 PC 的块翻译过 | 同一指令被 2.75× 重复翻译（`200343` 被 56 块各译一遍） | 入口桩 32 B×1097=35,104 B，省 **242,842 B** | 升级 R7A D4 | **做**：dedup 从「continue 跳过」改为「发射 32 B 桩 `goto L_<pc>` 进主块」；复用 `groupify`（`tools/aot.py:726-762`）的标签机制 | 局部化形式（loc/plain）须一致（`build_groups` 已判）；桩的寄存器态须与主块入口一致 | 改生成器→宿主等价→真机 ft/aotmiss | P1 |
| R8B-04 | `sdkconfig.defaults:33` vs `sdkconfig`(无) vs `build.ninja:137` | `CONFIG_COMPILER_LTO=y` 只在 defaults，sdkconfig 无此条、build.ninja 0 处 `-flto`、compile_commands 无 `-flto` | defaults 仅在 sdkconfig 首次生成时播种；后加 = 永不生效 | LTO 净 0 收益（当前） | 升级 R7B（"只报告不动"） | 若坚持要开：删 sdkconfig 重建或 menuconfig 手开；**建议不开** | 编译时间暴涨；aot_blocks 是单 TU，LTO 对块体无用；库级 DCE 已被 `--gc-sections` 覆盖 | A/B：重建对比 bin + ft | P2 |
| R8B-05 | `sdkconfig:503,510,687,1386` | 日志已降到 INFO（`LOG_MAXIMUM_LEVEL=3`，R7 已改）；assertion 仍全开（`ASSERTION_LEVEL=2`）；`ESP_ERR_TO_NAME_LOOKUP=y` | assertion 317 处调用点 ≈15.5 KB；esp_err 名表 8,224 B | 仍可省 ≈24 KB，但换调试能力 | R7B-04/05 | 不动（见 §4） | — | — | P2 |
| R8B-06 | `main/CMakeLists.txt:51-55,66` | main 组件 `-O3`，aot_blocks.c 单独 `-Os`（compile_commands 实测 `-O2 … -O3 … -Os`，末尾 `-Os` 胜出） | `-Os` 把块体压到 515,204 B（flash 470,910 + IRAM 44,294），对比注释里的 `-O3` 673 KB 省 ~158 KB | 密度 28.4 B/native insn | 复核 R7 §5.2 | 保持（已 A/B） | — | — | P2 |
| R8B-07 | `brick_a8.h:24` | `kBrickA8[128×128]` 16,384 B 仅两值：`0x00`×13,889、`0xFA`×2,495 | 1-bit 信息，打包 2,048 B | 省 14,336 B | R7B-06 | 可做 1bpp，需视觉复核（破坏与姊妹项目逐字节一致） | 观感 | 视觉对比图 | P2 |
| R8B-08 | `app_main.cpp:1270-1648` + `dmc_link.cpp:413-450` + `wallclock.cpp:61-63` | 22 处 `sscanf` 调用点 → `__ssvfscanf_r` **8,290 B**（nm 实测）；另有 `dtoa/strtod/mprec` ~7.5 KB（`%f` 本工程不用但被完整版 printf 拖入） | libc 体积 | sscanf 手写解析省 ~8.3 KB；关 `%f` 省 ~7.5 KB | R7B-16/17 | P2：DMC1/时钟解析要逐条回归 | 协议回归 | 手写解析 + DMC1 回归 | P2 |
| R8B-09 | map 链接区 | `libesp_wifi.a(`=0、`liblwip.a(`=0、`libspiffs.a(`=0、`libfatfs.a(`=0、`libesp-tls.a(`=0、`libesp_https_ota.a(`=0、`libmqtt.a(`=0、`libesp_netif.a(`=0；`libmbedcrypto.a(`=409（全被 gc-sections 丢） | 开关开着但链接器没拉进任何成员 | 0 B | 复核 R7B §3.2 | 改 `=n` 只省构建时间，可顺手做但不算成绩 | 无 | map 计数 | P2 |
| R8B-10 | `partitions.csv:18` + `build.log` size 报告 | factory 2 MiB，app 1,134,032 B（46.0%），余 963,120 B；DRAM 静态 234,104 B / 321,296 B（72.9%），heap 余 87,192 B | flash 不紧，DRAM 紧 | — | 复核 R7 §1.2 | 吃 DRAM 的改动要警惕：任何 IRAM 名单加块、任何大 `.bss` 缓冲 | — | idf.py size | P0 |
| R8B-11 | `.gitignore` + 磁盘 | 根 avi/png **410 MB**、`tmp/` **2.1 GB**、`dist/` 11.7 MB、`build.log` 2.7 MB、`sdkconfig.bak-wifion` ~85 KB——**全部已被 .gitignore 覆盖** | 磁盘占用，非 git 膨胀 | 清理可回收 ~2.5 GB 磁盘 | 新（R7 未列） | 该删：avi/png、tmp、dist、build.log、bak-wifion（都 gitignored 且可再生/可删）；不能动：`main/emu/`（设备侧 fork，见 §4） | 误删有价值的录屏证据 | 删前 `Get-ChildItem` 确认 | P1 |
| R8B-12 | `aot_blocks.c`（4,648,540 B / 190,686 行）+ `build.log` | 单一 TU，`-Os`，ninja 30 步里最大的一步；build.log 里 `aot_blocks.c:\d+` 警告行 **11,091** 条（`-Wunused-variable`，生成代码噪声） | 编译时间被它主导；警告刷屏 | 编译时间无精确值（ninja 不打 per-target 时间） | 新 | 生成器侧清掉未用变量（`r2v` 等）可减警告，不省 flash | 无 | 重生成看警告数 | P2 |

---

## 2.1 AOT 入口表裁剪：具体实现路径

**目标**：155,648 B → 91,016 B，省 64,632 B（= 64.6 KB）。

**改动点（两处，纯生成器 + 头文件，不碰块体）**：

1. `tools/aot.py:1025-1041`（`CONST_TABLES` 分支）：现在每页固定输出
   `static const AotEnt aot_ent_%03X[0x100] = { … }`（256 项，未用槽写 `{ nullptr,0,0,0 }`）。
   改成先算该页 `slot` 的 `min/max`，只输出 `[min..max]` 区间，并记 `base=min, n=max-min+1`。
2. `aot.h:332-336`：`typedef struct AotPageDesc { uint16_t page; const AotEnt *ent; }`（现在 6 B 对齐到 8 B，
   有 2 B 隐式 padding）改为 `{ uint16_t page; uint8_t base; uint8_t n; const AotEnt *ent; }` —— 恰好仍 8 B，
   `g_aot_desc`（76×8=608 B）不变。
3. `aot.h:395`（`aot_lookup` 热路径）：`const AotEnt *const e = &d->ent[lpc & 0xffu];` 改为
   `const unsigned slot = lpc & 0xffu; if ((unsigned)(slot - d->base) >= d->n) return nullptr; const AotEnt *e = &d->ent[slot - d->base];`

**对 `aot_lookup` 热路径的影响**：
- 当前热路径（页缓存命中后）是：读 tag → 命中则 `e = &d->ent[slot]` 一次基址+偏移 load，`return e->fn`。
- 裁剪后多一次 `(unsigned)(slot - base) >= n` 无符号比较（可预测、无分支跳转），实测应在 1–2 cycles（**推测**，
  与 R7 §3.2 一致）。页缓存（`g_aot_pg_tag/ref[128]`，`aot.h:360-361`）机制不变。
- 代价：`AotEnt` 数组不再从 256×8 B 边界开始，`AotEnt` 的 32 B cache line 覆盖从「固定 4 槽」变「跨页边界」，
  理论 cache 行为略差——但省掉的 64.6 KB rodata 本身减少 flash 页压力，净收益大概率为正。**需真机 A/B 确认。**

**A/B 方案**：改 `tools/aot.py` + `aot.h` 生成两份镜像（基线 / 裁剪），真机跑同一脚本比
`ft`(帧间隔 p50/p95)、`gmax`(切换帧)、`aotmiss/帧`。若三者不劣化即通过。**与入口桩（R8B-03）互不依赖，可单独做。**

---

## 2.2 重叠块入口桩：具体实现路径

**目标**：277,946 B → 35,104 B，省 242,842 B（= ~243 KB），命中率零损失。

**机制**（R7_aot §4.1 已论证，本轮复算确认）：1,097 个「完全重叠块」的每条指令都已被更小 PC 的块翻译过。
其中 929 个来自 `AB_force.txt`（miss census 驱动的强制入口，最新 census 零命中），120 个是静态函数入口
（`AB_blocks.txt`，`is_entry=True` 分支，`tools/aot.py:866` 永不跳过），37/11 个有命中（真在干活，**不能动**）。

**改动点（`tools/aot.py`）**：
1. `tools/aot.py:876` 现在：`if not is_entry and entry in covered and entry not in force: continue`（只跳过非入口、
   非 force 的 covered PC）。对「完全重叠」（`pcs ⊆ covered`）的块，改成发射 **32 B 入口桩**：
   `aot_b%06X` 函数体 = `AOT_LOAD_REGS`（若主块是局部化形式）+ `goto L_<pc>`（跳进主块内部标签）。
2. 主块内部需要 `L_<pc>:` 标签——这正是 `groupify()`（`tools/aot.py:726-762`）已经实现的能力
   （`L_%06X:` 标签 + `goto LAB_%06X`），只是 `GROUP` 默认关（`tools/aot.py:78`，历史负结果）。
   复用思路：不为「分组超块」服务，只为「给覆盖块打标签供桩跳入」服务。
3. 准入条件：R7_aot D4 说的「放宽到『入口在任何块内』」——即只要某块入口 PC 落在另一块体内，就给覆盖块打标签。

**风险与正确性约束**（R7_aot §8 D4 + 我读代码确认）：
- 局部化形式必须一致：桩与主块的 `AOT_LOAD_REGS`/`c->r[]` 形式须匹配（`build_groups` 已判 `loc_of`，`tools/aot.py:810`）。
- 桩跳入后，主块在 `LAB_<pc>` 处要重做「cycle 扣减 + 中断检查」（`groupify` 的 `LAB_` 标签已含此逻辑，`tools/aot.py:986-995`），
  否则 slice 边界漂移，帧 CRC 会变。
- 929 个 force 块里有多少服务重绘帧，现有 census 无法分离（R7_aot §10 不确定度），**需一次带 `C3_BLOCK_CENSUS=1` 的重绘帧采集**才能定死可桩化集合。

**A/B 方案**：改生成器 → 先宿主等价测试（四窗口 trace 逐位一致，`tools/aot.py` 头注释的 T2 口径）→ 真机测
`ft`/`gmax`/`aotmiss`。**这是本轮两笔 AOT 收益里较大的一笔（243 KB），但改动最深，建议单独一轮、与裁剪分开验收。**

**「做/不做」结论**：入口表裁剪（R8B-02）**做**；重叠块入口桩（R8B-03）**做**。两者都 P1（需 A/B），
都基于 R7 已具体化的方案 + 本轮新增的「AotPageDesc 有现成 2 B padding」「groupify 标签机制现成可用」两条实现级证据。

---

## 2.3 三本账现状（v1.18.0 实测）

| 账本 | 数值 | 出处 |
|---|---|---|
| app 镜像 | **1,134,032 B**（`idf.py size` 报 1,133,918 B + 114 B 头/pad） | `build.log` 尾 |
| factory 分区 | 2,097,152 B（partitions.csv:18），**余 963,120 B（46.0% 用）** | 计算 |
| DRAM/IRAM 池 | 321,296 B（0x4e710） | map Memory Configuration |
| IRAM .text | 118,664 B | readelf `.iram0.text` 0x1cf88 |
| .bss | 108,192 B | readelf `.dram0.bss` 0x1a6a0 |
| .data | 7,248 B | readelf `.dram0.data` 0x1c50 |
| **静态合计** | **234,104 B（72.9%）** | `idf.py size` DRAM 列 |
| **heap 余** | **87,192 B（27.1%）** | `idf.py size` / `_heap_start`=0x3fcb92f0 |
| RTC SLOW | 56 B / 8,192 B | `idf.py size` |

**会吃 DRAM 的改动**（本轮其它路若提，需警惕）：任何 IRAM 名单加块（208 块 / 44,294 B 已占 IRAM，每加 4 KB 代码
就挤掉 4 KB 堆）、任何大 `.bss` 静态缓冲、双缓冲 framebuffer。**AOT 两项裁剪（R8B-02/03）省的是 flash，不吃也不吐 DRAM**——
这正是它们「该做」的理由之一：在 DRAM 最紧的时候，去动 DRAM 是刀口，去动 flash 是白捡。

---

## 2.4 库与组件（map 实证，不按 sdkconfig 猜）

| sdkconfig 项 | 值 | map 证据 | 净贡献 |
|---|---|---|---|
| `CONFIG_ESP_WIFI_ENABLED`（sdkconfig:1149） | y | `libesp_wifi.a(` 成员 **0** | 0 B |
| `CONFIG_BT_ENABLED`（sdkconfig:553） | 未 set（已关） | — | 0 B |
| `CONFIG_LWIP_ENABLE`（sdkconfig:1423） | y | `liblwip.a(` 成员 **0** | 0 B |
| `CONFIG_MBEDTLS_*`（sdkconfig:1610-1744） | y | `libmbedtls.a(` 0；`libmbedcrypto.a(` 409（全被 gc） | 0 B |
| `CONFIG_ESP_TLS_*`（sdkconfig:654-663） | y | `libesp-tls.a(` **0** | 0 B |
| `CONFIG_ESP_HTTPS_OTA_*`（sdkconfig:855-857） | y | `libesp_https_ota.a(` **0** | 0 B |
| `CONFIG_SPIFFS_*`（sdkconfig:1900-1929） | y | `libspiffs.a(` **0** | 0 B |
| `CONFIG_FATFS_*`（sdkconfig:1222-1270） | y | `libfatfs.a(` **0** | 0 B |
| `CONFIG_ESP_ERR_TO_NAME_LOOKUP`（sdkconfig:687） | y | `esp_err_to_name` 82 B 代码 + 名表 8,224 B rodata | **8.3 KB 可省** |
| NVS（menu.cpp） | 在用 | `libnvs_flash.a(` 772 | 必需，不可删 |

> 结论同 R7：这些开关的体积收益是 0，唯一「非零」的是 esp_err 名表（8,224 B，R7B-05）。改成 `=n` 只省构建时间，
> 但留着开关能防止谁手滑 `#include "esp_wifi.h"` 时突然多出 700 KB。**本轮不动，P2。**

---

## 2.5 LTO：为什么没生效 + 收益/风险/验证

**为什么没生效（优先级机制）**：ESP-IDF 的 `sdkconfig.defaults`（含 `sdkconfig.defaults.<target>`）只在
`sdkconfig` **首次生成**时作为种子合并进去；一旦 `sdkconfig` 存在，它就是唯一权威，后续 `idf.py build` 不再回读 defaults。
`CONFIG_COMPILER_LTO=y` 是后来加进 `sdkconfig.defaults:33` 的，但既有 `sdkconfig`（2019 行，Compiler options 段 499-532 行）
里**没有这条**，所以构建里 LTO 从头到尾是关的。三重证据：`sdkconfig` 无 `CONFIG_COMPILER_LTO`；`build.ninja` 0 处 `-flto`、
`LINK_FLAGS`（build.ninja:137）无 `-flto`；`compile_commands.json` 的 aot_blocks 命令无 `-flto`。

**单独开启的收益**（判断：低）：
- 本工程的热解释器已经 amalgamate 进单一 `unsp_amalgam.cpp`（`main/CMakeLists.txt:25`），`-O3` 在这个单 TU 内
  已经做了跨函数内联——**LTO 最大的「跨 TU 内联」收益已被 amalgamation 提前拿走**。
- 库级 dead-code 已由 `-ffunction-sections -fdata-sections -Wl,--gc-sections`（R7 §5.1 实证丢弃 5,246 段）覆盖。
- 剩余 LTO 价值 ≈ 对 IDF 库间的跨模块合并，预期几十 KB 且不确定。

**风险**：
- 编译/链接时间大涨（LTO 做 whole-program 优化，`aot_blocks.c` 是 190k 行单 TU，链接会非常慢、吃内存）。
- `aot_blocks.c` 是单 TU，LTO 对块体**几乎不起作用**（无跨 TU 边界），但 LTO 的重优化可能**改变 `-Os` 的块体排布**，
  16 KB icache 下的行为不可预测（R7 的顾虑）。

**验证方法**（若要 A/B）：`sdkconfig` 里加 `CONFIG_COMPILER_LTO=y`（IDF 5.5 GCC 工具链即 `CONFIG_COMPILER_LTO_GNU`），
`idf.py build` 对比 `digicore-c3.bin` 大小 + 真机 `ft`/`gmax`。**建议：不开。** 价值低、编译成本高、AOT 块体行为不可控。

---

## 2.6 `-O3` 与 `-Os` 的分工（实测，不动）

`compile_commands.json` 里 aot_blocks.c 的命令是 `… -O2 … -O3 … -Os …`（末尾 `-Os` 胜出）。`size -A` 实测：

| 段 | 字节 | 块数 | 说明 |
|---|---:|---:|---|
| `.text._ZL11aot_b*`（flash） | **470,910** | 1,736 | 块体主体 |
| `.iram1.*`（IRAM） | **44,294** | 208 | 热块 |
| `.rodata._ZL1[12]aot_ent_*` | **155,648** | 76×2048 | 入口表 |
| 合计（aot_blocks.c.obj 可装载段） | ~671 KB | — | 块体 515,204 + 表 155,648 |

- 块体密度：515,204 B / 18,126 native = **28.4 B/native insn**（局部化 1,278 块 23.6 B/insn，plain 666 块 43.5 B/insn）。
- `main/CMakeLists.txt:61-66` 注释说 `-O3` 下块体 673 KB，实测 `-Os` 下 515,204 B → **-Os 省 ~158 KB（-23%）**。
- 结论：分工正确，**不动**。改回 `-O3` 会让 16 KB icache 的块首行几乎必 miss。

---

## 2.7 生成物与仓库冗余（三分类）

**（A）该删（都已 .gitignore，纯磁盘占用，可再生或已过时）**

| 项 | 大小 | 证据 | 说明 |
|---|---:|---|---|
| 根目录 `*.avi`/`*.png` | **410 MB**（14 文件） | `Get-ChildItem` 实测 | 2026-11-19 测试录屏/截图，`.gitignore` 已盖 `*.avi`/`/*.png` |
| `tmp/` | **2.1 GB**（8,156 文件） | 实测 | 大量中间产物（buildN.log、regs.txt 1.7 MB×3、clock_trace.txt 166 MB、ta/ti.txt 37 MB×2 等），`.gitignore` 已盖 `tmp/` |
| `dist/` | 11.7 MB | 实测 | v1.17.0/v1.17.1 发布快照，`.gitignore` 已盖 `dist/` |
| `firmware/*/build.log` | 2.7 MB | 实测 | `.gitignore` 已盖 `firmware/*/build.log` |
| `firmware/*/sdkconfig.bak-wifion` | ~85 KB | diff：比现行多 `ESP_WIFI_IRAM_OPT=y`×2 + `LOG_MAXIMUM_LEVEL=4(DEBUG)`，是「WiFi IRAM 开」时代的旧备份 | `.gitignore` 已盖 `firmware/*/sdkconfig.bak-*` |
| 临时 `*.exe`（`tmp/*.exe`、`firmware/.../tmp/*.exe`、`hosttest/*.exe`、`recomp/*.exe`、`emu/*.exe`） | ~10 MB | 实测 | `.gitignore` 已盖 `*.exe` |

**（B）该 gitignore（已经在 .gitignore 里，无需新加）**
- 以上所有条目都**已经被覆盖**；`.gitignore` 唯一的问题是**编码**：文件是 GBK，我 UTF-8 读出来全是乱码
  （如「鏋勫缓浜х墿」应为「构建产物」）。git 按字节匹配模式仍正确，但任何人编辑它都会踩编码坑。**建议顺手转 UTF-8。**

**（C）不能动（防误删）**
- `main/emu/`（设备侧 fork）：`main/CMakeLists.txt:3-7` 注释明说「`main/emu/*` 是 `emu/*` 的副本，带三处设备侧编辑」，
  它**不是**纯冗余，是构建必需。**一行不要删。**
- `firmware/digicore-c3/build/`：已有 elf/map/bin 是审计与下一轮 A/B 的对照基线，本轮只读。
- `roms/*.bin`、`saves/`、`nvram/`、`milestones/**/*.bin`：ROM/存档/里程碑，`.gitignore` 明确保留（校验见 SHA256SUMS）。

---

## 2.8 `aot_blocks.c` 编译时间与 `-Os` 密度

- **编译时间**：`build.log` 里 ninja 不输出 per-target 耗时，所以没有精确秒数；但它是 30 步构建里唯一的
  190,686 行单 TU（`-Os`），且产生 **11,091 条警告行**（`aot_blocks.c:\d+`，全是 `-Wunused-variable`，
  `compile_commands` 里 `-Wno-error=unused-variable` 让它们不致命但刷屏）。**它是编译时间的主导项**（推测）。
- **密度**：见 §2.6，28.4 B/native insn（`-Os`）。
- **减小生成物而不损命中率**（除 R8B-02/03 两笔外）：
  1. 生成器清掉未用变量（`r2v` 等），砍掉 11k 条警告 → 只省 build.log 体积与编译时间，不省 flash。
  2. `-Os` 已经是密度最优，**不要再往 `-O2`/`-O3` 走**（R7 §5.2 已 A/B）。
  3. 16 个 0 指令包装块（R7_aot §2.4，228 B flash + 60 B IRAM）**删了反而更快**（少一次函数调用直接走 miss），
     是「减生成物且不损命中率」里唯一白捡的几字节，可并入 R8B-03 一起做。

---

## 3. 需要主审裁决的冲突/不确定项

1. **R8B-03（入口桩）的 929 个 force 块里有多少服务重绘帧**：现有 census 是切换帧场景，采不到重绘帧。
   删/桩化前必须做一次 `C3_BLOCK_CENSUS=1` 的重绘帧采集（`R` 键十连），否则有「按错误证据删热块」的 R6B 前车之鉴
   （删 67 块慢 28%）。**这是 R8B-03 的唯一硬前提。**
2. **R8B-02 的「+2 比较成本」是推测**（1–2 cycles），未上机。与 R7_aot §10 一致。主审若要先做低风险项，可先做 R8B-02
   （只碰 `aot.h` + 生成器表输出，块体一字不动），真机 A/B 通过后再碰 R8B-03。
3. **`sdkconfig` 里 `CONFIG_COMPILER_LTO` 缺失 vs `sdkconfig.defaults` 有**：这是「配置漂移」。要消除漂移只有两个办法——
   要么把 defaults 里的 `CONFIG_COMPILER_LTO=y` 删掉（承认不用 LTO），要么真正开 LTO 并 A/B。**建议前者（删掉那行）**，
   否则下一个审计员还会以为 LTO 在生效。
4. **`.gitignore` 的 GBK 编码**：转 UTF-8 是 1 行 git 操作，但本轮纪律禁 git；建议主审在改动窗口顺手转。

---

## 4. 明确不动的清单（防误删）

1. **`main/emu/`（设备侧 fork）**：构建必需，三处设备侧编辑（`main/CMakeLists.txt:3-7`），不是冗余。
2. **`-Os` on aot_blocks、`-O3` on main**：已 A/B，`-Os` 省 158 KB 且 icache 更友好（§2.6）。
3. **IRAM 名单 208 块 / 44,294 B**：R7_aot §5 逐块核对 0 偏差，全有执行证据（含 bail 路径），R6B 已用「慢 28%」否决过按 census 删。
4. **`internal_rom` 65,540 B、NVS 18,741 B、`s_soc` 42,272 B**：guest 状态/ROM/存档，删了跑不起来或丢用户数据。
5. **assertion 全开（`ASSERTION_LEVEL=2`）+ panic/int_wdt**：≈15.5 KB + 4.9 KB 换崩溃现场，真机调试命根子（R7B-04）。
6. **`g_aot_desc` 608 B 结构（即使加 base/n 也不涨）**：R8B-02 的关键前提，别把它改成三列并行数组（R7 的 17 fps 教训）。
7. **`factory` 2 MiB 分区 / partitions.csv**：app 只占 46%，别为了「省」去动分区表。
8. **`roms/`、`saves/`、`nvram/`、`milestones/`**：用户数据与 ROM，不可再生。

---

## A. 复算脚本（R8B-01/02/03 的数字出处）

```python
# %TEMP%\r8_aot_recompute.py —— 从 aot_blocks.c 独立重算（与 R7 %TEMP%\r7\ 脚本结论一致）
import re, io
t = io.open(r"C:/Users/DuWeisi/digimon-color-passport/firmware/digicore-c3/main/emu/aot_blocks.c",
            encoding="utf-8", errors="replace").read()
blocks = re.findall(r'^// ([0-9A-F]{6}): (\d+) native instructions, (\d+) cycles worst case', t, re.M)
print("blocks", len(blocks), "native", sum(int(b) for a,b,c in blocks), "iram", t.count("AOT_IRAM static void"))
ent = re.compile(r'static const AotEnt aot_ent_([0-9A-F]+)\[0x100\] = \{(.*?)\n\};', re.S)
pages = []
for m in ent.finditer(t):
    used = [i for i,e in enumerate(re.findall(r'\{\s*(nullptr|aot_b[0-9A-F]+)\s*,', m.group(2))) if e != 'nullptr']
    pages.append((m.group(1), used))
n_used = sum(len(u) for _,u in pages); n_slots = len(pages)*256
print("pages", len(pages), "slots", n_slots, "used", n_used,
      "empty", n_slots-n_used, "empty_bytes", (n_slots-n_used)*8, "table_bytes", n_slots*8)
span = sum(max(u)-min(u)+1 for _,u in pages if u)
print("tight_span_bytes", span*8, "trim_save", n_slots*8 - span*8)
# 重叠（R7 口径：o < p 数值比较，非文件顺序）
parts = re.split(r'^// [0-9A-F]{6}: \d+ native instructions, \d+ cycles worst case\n', t, flags=re.M)[1:]
pcs = [set(re.findall(r'AOT_I[H]?\(c, 0x([0-9A-F]{6})u\)', b)) for b in parts]
own = {}
for e, s in zip([int(b,16) for b in re.findall(r'^// ([0-9A-F]{6}):', t, re.M)], pcs):
    for x in s: own.setdefault(x, []).append(e)
shadow = [e for e, s in zip([int(b,16) for b in re.findall(r'^// ([0-9A-F]{6}):', t, re.M)], pcs)
          if s and all(any(o < e for o in own[x]) for x in s)]
print("fully_shadowed", len(shadow))
```
输出：`blocks 1944 native 18126 iram 208`；`pages 76 slots 19456 used 1944 empty 17512 empty_bytes 140096 table_bytes 155648`；
`tight_span_bytes 91016 trim_save 64632`；`fully_shadowed 1097`（字节数由 `%TEMP%\r7\shapes.py` 复跑得 277,946 B）。

---

## 9. 一句话给主审

**flash 不缺（app 46%），DRAM 紧（heap 余 87 KB，72.9% 已用）。** 所以本路两条 AOT 收益都落在 flash、不碰 DRAM，
是「白捡」性质：入口表裁剪省 64.6 KB、重叠块入口桩省 243 KB，两者「做」、都 P1（需 A/B）。LTO 确认没生效且别开。
开关型冗余（WiFi/TLS/OTA/SPIFFS…）继续 0 字节，别写进成绩单。真正该动手的是磁盘清理：根 avi/png 410 MB + `tmp/` 2.1 GB，
全部已 gitignore，删了即回血 ~2.5 GB。
