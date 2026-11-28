# R8 模拟器核心与热路径审计

日期：2026-11-28
角色：模拟器核心与热路径审计员（只读）
范围：`main/emu/{unsp_glue.cpp, gpce4.cpp/h, st7735.cpp, romstore.cpp/h, aot.h, hotblock.h, compat/unsp.h, isa/*}`

## 0. 结论摘要（带优先级）

1. **P0 — `hot_prng_n` 的注释与代码不符，是空闲/待机唯一还有量级的纯开销。**
   注释（`unsp_glue.cpp:629-632`）声称「把 8 个状态字留在 local、每 pass 只写一次内存」，实际循环体每 pass
   对 guest RAM `0x0600..0x0607` 做 ≈11 读 + 8 写 = **19 次 DRAM 访问**（`unsp_glue.cpp:655-707`）。
   空闲 `collapse=285,609 pass/s`（R6B §1.1），≈38 cyc/pass → **≈10.9 M cyc/s ≈ 6.8 % CPU**，
   占空闲 guest_pct（23–33%，PERF_2026-11-22 §1）的大头。把这 8 个字 hoist 成局部、循环尾写回 6 字，
   可直接降待机占空比。这是本轮范围内唯一「性能+续航同向、且不碰生成器/不改行为」的改动。

2. **P1 — 重叠块→入口桩（R7 的 D4）仍是最高杠杆，且现在有了新证据。**
   R6B §3.1 已定位重绘帧 60 %（7.0 M cyc）来自 flash 块体行 miss；重叠块占块体字节 **61.4 %**
   （277,946 / 452,140 B，R7_aot §4.1）。把重叠块改成 `goto L_<pc>` 入口桩（D4）比删块（D1）好：
   D1 让 miss 1.0→50/帧（+1~5 % 帧，R7_aot §4.2），D4 零 miss 损失、省 ≈243 KB、并让唯一热代码
   在 16 KiB I-cache 里常驻 → 少 miss → 少时间 → 降占空比。代价是生成器结构改动 + 需 A/B
   （`AOT_GROUP` 的 groupify 已证 `-O3` 寄存器分配会劣化，R7_aot §6.1）。

3. **P2 — aot_step 仍每 AOT dispatch 两次 64-bit RMW，execute_run 仍每 dispatch 读 m_fasthot/m_fastspin。**
   R7 只清了 W1（HLE 掩码）+W2（wait 预筛），R6B 的 W3/W5/W6 还留着：`m_aot_insns`/`m_aot_blocks`
   是 `uint64_t`（`unsp.h:233/237`），每次 AOT dispatch `+=`/`++`（`unsp_glue.cpp:1115-1116/1143`）；
   `execute_run:1886-1888` 读 `m_fasthot`、`:1932-1934` 读 `m_fastspin`。合计 ≈32 条指令/dispatch，
   ≈2 % 重绘帧。全部可低风险清掉。

4. **P2 — AotEnt 的 `cyc`/`pad` 是死字段，可把 8 B 入口压到 6 B；区间裁剪 D2 本轮降级。**
   `aot_lookup` 的 `cyc` 实参恒为 `nullptr`（`unsp_glue.cpp:1101`），`pad` 无读者。裁掉后 32 B 行
   覆盖 4→5 项（DROM 行密度 +25 %）。而 `[min_slot,max_slot]` 区间裁剪（R7_aot §3.4）只省 64,632 B flash、
   不省周期，与本轮「性能+续航」目标弱相关，建议单列、本轮不动。

5. **P2 — `gpce4::spi_direct_r` 的 64 位地址运算（R7-13）仍开放**（`gpce4.cpp:574-589`），
   `uint64_t` 乘加 + 64 位比较在 RV32 上是双字运算，可 32 位化（`m_spi_bank` 实测只 0/1）。

6. **P1（待裁决）— 待机/睡眠「帧提前进 WFI」的判据 = guest_pct。** 待机时 guest 停在 `0x230321`
   忙等，`wait_loop_collapse` 已把到下一个 64 Hz tick 的全部 pass 折成一次 `hot_prng_n(done)`。
   LFSR 状态**不能跳过**（宠物行为依赖确定性推进），所以「提前进 WFI」等价于「把 hot_prng_n 这条
   纯开销砍掉」= 第 1 条。判据就是 STANDBY 场景的 `guest_pct`。

## 1. 范围与方法

读了：`unsp_glue.cpp`（2114 行，全文）、`gpce4.cpp`（1598 行，全文）、`gpce4.h`、`compat/unsp.h`、
`isa/unsp_{other,fxxx,exxx,jumps,extended}.cpp`、`hotblock.h`、`aot.h`、`romstore.h/.cpp`、
`st7735.cpp`，以及 `rom_esp.cpp:80-159`（为核实 romstore 的 `m_index` 放置，仅参考）。

只读命令：
- `riscv32-esp-elf-nm -S --radix=d build/digicore-c3.elf`：复核 `execute_run=5142 B`、
  `wait_loop_collapse=838 B`、`spin_collapse=312 B`、`aot_step=418 B`、`hle_blit_key=806 B`
  （均与 R7 一致；ELF 时间 2026-11-28 16:16）。
- grep 历史数据：`docs/PERF_2026-11-22.md`、`docs/notes/R6B_aot.md`、`docs/AUDIT_PERF_2026-11-27_R7.md`、
  `docs/notes/R7_{core,aot}.md` 的实测口径。

未读 `aot_blocks.c` 全文 / `*.bin` / `*.avi`，未跑构建/刷机/串口/git。引用历史实测均注明出处。

沿用前轮硬口径（不重新论证）：重绘帧 6,788 dispatch / 11.7 M host cycles（73.2 ms），稳定态 16 ms
（2.56 M cycles），host cycles/dispatch 重绘 1,107（R6B §1/§2，R7 §1 复核）。

## 2. 发现表

| ID | 位置(文件:行) | 现象/证据 | 影响机制 | 估算 | 与R7关系 | 建议改动 | 风险 | 验证 | 优先级 |
|---|---|---|---|---|---|---|---|---|---|
| R8-C1 | `unsp_glue.cpp:655-707`（循环体）；`:629-632`（错误注释） | 循环体每 pass 读 `m[0x0602/0x0603/0x0600×2/0x0601×3/0x0604/0x0605/0x0606/0x0607]` + 写 `m[0x0600×2/0x0601×2/0x0604/0x0605/0x0606/0x0607]`（≈19 次 DRAM 访问）；注释却写 "keeps the eight state words ... in locals and writes memory once per pass" | guest RAM（`s->ram`，DRAM、无 D-cache）每 pass 重读重写；空闲 `collapse=285,609 pass/s`（R6B §1.1）| **≈10.9 M cyc/s ≈ 6.8 % CPU**；空闲 guest_pct 23–33 %（PERF_2026-11-22 §1）里占 ~23 %。hoist 后循环内 ≈0 次 DRAM（仅尾 6 写回）| 新（R7 只动调用点预筛，未进 hot_prng_n）| 把 0x0600/0x0601/0x0604/0x0605/0x0606/0x0607 六字 + 种子 0x0602/0x0603 两字 hoist 成局部，寄存器内旋转，循环尾一次性写回 | 低（只碰折叠路径；必须保证写回顺序与逐 pass 等价）| PC 侧 `digicore.exe --autoplay` RAM/存档逐字节；host 单测 `hot_prng_n(N)` vs `hot_block_prng×N` golden；真机 STANDBY 对比 `guest_pct` | **P0** |
| R8-C2 | `tools/aot.py` groupify 准入；`aot_blocks.c` 1,097 重叠块/277,946 B（R7_aot §4.1） | 重叠块 = 块体字节 61.4 %；R6B §3.1 (a) 块体 flash 行 miss = 7.0 M cyc = 60 % 重绘帧；R7_aot §4.2 证 D1（删）cost +1~5 % 帧，D4（桩）零 miss 损失 | 冗余翻译把唯一热代码稀释出 16 KiB I-cache → 每次块体 miss 304 cyc/行 | 块体 452→~35 KB（重叠部分），省 **≈243 KB** flash + I-cache 常驻 → 少 miss → 降占空比 | R7 遗留 D4，本轮给新证据（(a) 60 % 与重叠 61.4 % 同源）| 放宽 groupify 准入到「入口在任意已译块内」，生成 `goto L_<pc>` 入口桩 | 中（`AOT_GROUP` 负结果：大块 `-O3` 寄存器分配变差，R7_aot §6.1）| 改 `tools/aot.py` 生成，PC 侧逐位 + 真机 `ft/gmax/aotmiss` A/B | **P1** |
| R8-C3 | `unsp_glue.cpp:1115-1116/1143`；`unsp.h:233/237`；`execute_run:1886-1888/1932-1934` | `m_aot_insns`/`m_aot_blocks` 是 `uint64_t`，每 AOT dispatch `+=`/`++`；`m_fasthot`/`m_fastspin` 每 dispatch 读 DRAM + 比较（`m_insn_count` 已是 32bit，`unsp.h:114`）| RV32 上 64-bit RMW 是双字运算；两个全局 flag 运行期不变却每 dispatch 重读 | ≈32 instr/dispatch × 6,788 ≈ 0.22 M instr ≈ **~2 % 重绘帧**（空闲可忽略）| R6B W3/W5/W6 残留，R7 未清 | `m_aot_insns/m_aot_blocks` 改 slice 内 32bit 累加、帧尾并入 64bit；`m_fasthot/m_fastspin` 读提到循环外或并入 HLE 掩码 | 低（STAT 只读；32bit 单帧不溢出）| 反汇编 `execute_run/aot_step` 尺寸 + STAT 数字不变 | P2 |
| R8-C4 | `aot.h:324-330`；`aot.h:395-400`；`unsp_glue.cpp:1101` | `aot_lookup(lpc,&cnt,nullptr)`：`cyc` 恒 null，`pad` 无读者（R6B §5「cyc 死字段且数值错误」）| 入口项 8 B→6 B，32 B 行覆盖 4→5 项，DROM miss 概率略降 | 推测 0.1–0.3 M cyc/重绘帧（d' 9–18 % 的一小块）；flash −3,888 B（对齐后 ≈8 KB 内）| 新（区别于 R7 的 D2 区间裁剪）| 保留 fn+cnt 同行（R7_aot §7 的 17fps 教训），只裁 `cyc`+`pad` | 低（cyc/pad 无读者）；需 A/B 确认无对齐反噬 | `nm/size` 段体积 + 真机 `ft/gmax` 同场景 | P2 |
| R8-C5 | `gpce4.cpp:574-589` | `uint64_t base = uint64_t(offset)*2u + uint64_t(m_spi_bank)*0x400000u; if (base+1 >= m_flash->size())` | 每 flash 数据读一次 64-bit 乘加 + 64-bit 比较（RV32 双字）| 推测 2–4 cyc/读；redraw 精灵数据读路径，量小 | R7-13 遗留（当时标推测未动）| `uint32_t base = offset*2u + (m_spi_bank << 22); if (base+1 >= size)`（m_spi_bank 实测 0/1）| 低 | 反汇编 `spi_direct_r` + 真机功能 | P2 |
| R8-C6 | `unsp_glue.cpp:722-820`（wait_loop_collapse）+ app_main 待机路径（引 R8_BRIEF §2）| 待机 guest 停在 `0x230321`，collapse 已把到 64 Hz tick 的全部 pass 折成一次 `hot_prng_n(done)`；`m_insn_count += 74*done`、`m_collapse_passes += done` 证明它真在跑 LFSR | LFSR 状态必须确定性推进（宠物行为依赖），不能跳过；「提前进 WFI」等价于砍掉 hot_prng_n 这条纯开销 | 见 C1 | 新（判据化）| 与 C1 合并验证；若 STANDBY guest_pct 仍 >25 % 再评估 LFSR 跳跃表（预计算跳 N 步多项式） | 低（先只做 C1）| STANDBY 场景 `guest_pct` 前后对比 | P1 |

### Q1 补充：每 dispatch 现在的剩余税

R7 之后 `execute_run` 热循环（`unsp_glue.cpp:1852-2075`）每 dispatch 固定税 = R6B 的 127 条/168 cyc
减去 W1（≈20 条）+W2（≈10 条）+W4（`m_block_hist` 判空，现已被 `#ifndef ESP_PLATFORM` 包掉，`unsp_glue.cpp:1877-1879`）
≈ **97–105 条 / ~130–140 cyc**（全命中、不含块体）。剩余大头在 aot_step 内部（页缓存探测 18 条 + 间接调用/写回 18 条，R6B §2），
其中可去的只剩 R8-C3 的 ~32 条（W3/W5/W6）。`UNSP_LPC`（`unsp.h:9`，5 条）与循环尾 4 条是结构性的，不动。

### Q2 补充：入口表裁剪与重叠桩的具体改法/预期/风险

- **重叠块→入口桩（D4）**：`tools/aot.py:876` 的去重只看「入口 PC 是否已被覆盖」，不看入口之后的指令是否重复；
  放宽到「入口在任意已译块内」即可生成 `goto L_<pc>` 桩进主块。预期省 ≈243 KB flash + I-cache 常驻收益；
  风险是 groupify 的 `-O3` 寄存器分配劣化（R7_aot §6.1 已测过负结果），必须单独 A/B。
- **入口表裁剪（D2）**：`aot.h:332-336` 的 `AotPageDesc` 加 `uint8_t base; uint8_t n;`（结构体仍 8 B），
  `slot < base || slot >= base+n → nullptr`，`e = &d->ent[slot-base]`。省 64,632 B flash（155,648→91,016）；
  代价是热路径 +2 次比较（1–2 cyc，推测）且 `AotEnt` 跨 32 B 行边界，**不省周期、不改善 I-cache**。
  本轮降级为 P2（flash 不紧，R8_BRIEF §2），仅报告。

### Q3 补充：spin/wait collapse 判定与误判边界

- `spin_collapse`（`unsp_glue.cpp:377-454`）：预筛已内联（`m_fastspin && 3 PC`），函数内 `value > 0x03e7` 判
  「循环已结束」、`passes = 0x3e7 - value`、留最后一 pass 给解释器——边界正确（`0x3e7` 时 passes=0 返回 false）。
  剩余成本只有每 dispatch 的 `m_fastspin` 读 + 3 比较（R8-C3 覆盖）。
- `wait_loop_collapse`（`unsp_glue.cpp:722-820`）：预筛已内联；`done = (tick==0 && !m_sirq) ? passes : 1`，
  单次读 tick（64 Hz handler 不会在 slice 中改它），边界正确。剩余成本是 hot_prng_n 本体（R8-C1）。
- 待机/睡眠判据：见 R8-C6。

### Q4 补充：指令实现的 64 位/重复 fetch/分支/记账

- 解释器只跑 ~245 条/帧（fallback 244 + miss 1，R6B §3.2），≈1 % 重绘帧；ISA 内的
  `execute_muls_ss` int64 累加（`unsp_fxxx.cpp:533/548`）、imm16 的重复 `read16(UNSP_LPC)+add_lpc` 双重算
  （`unsp_other.cpp:192/199/212`）都**不值得为它开一版**，仅记录。
- `m_icount` 记账：AOT 块在 `AOT_NO_HOOKS` 下每条指令 `c->icount -= n`（`aot.h:308`，.bss RMW ≈3 cyc），
  58,477 块内指令/帧 ≈ 0.18 M cyc ≈ 1.5 %。可「整块一次性减」，但需改生成器 + 保住 fallback 语义，P2 以下。
- `tick()`（`gpce4.cpp:1518-1598`）：每 execute_run slice 一次；空闲/重绘均由 64 Hz tick 定界 ≈ 60 次/s，
  快路径已合并周期定时器（R5/R6），非热点，不动。

### Q5 补充：IRAM 208 块名单

R7_aot §5 已证 208 条与 `aot_iram_r6.txt` 逐 PC 一致、零白占。真正缺口仍是 R6B §4.2 指出的
**render_band 设置块 `0x207000-0x207A00`（223 块/69,466 B）与 `0x203000-0x206000`（389 块/94,384 B）全在 flash**，
是重绘帧最大单一缺口；但 163,850 B > IRAM 预算（≤48 KB），不能全搬。**正确顺序是：先做 R8-C2（重叠去重，砍 61 % 块体），
再按「重绘帧 block census × 行数」重排 IRAM 名单**——而那次 census 目前缺失（R7_aot §10 已说明现有 census 是切换帧、不是重绘帧）。

### Q6 补充：romstore/XIP 读路径

`romstore.h:95-124` 映射路径已近最优：`page=off>>12` + 2 次 overlay 比较 + `m_index[page]`（DRAM，`rom_esp.cpp:43` 的
`uint32_t s_index[2048]`）+ `m_base+po` 字读（XIP）。XIP miss 成本（~304 cyc/行）的大头不在数据读，而在块体取指
（R6B (a) 60 %）与 AotEnt DROM（d' 9–18 %）。数据读（精灵，R6B (b) 1.5–3 %）顺序访问、prefetch 友好。
「更紧凑布局减 miss」的落点就是 R8-C2（块体去重）与 R8-C4（AotEnt 变密），romstore 本身无额外可挖。

### Q7 补充：预算表（每帧可省 X 周期 = Y %）

分母：重绘帧 11.7 M cyc / 6,788 dispatch（R6B §1/§3.1）；空闲按 160 MHz 每秒计。

| 优化 | 单项节省 | 帧内/秒内量 | 省周期 | 占重绘帧 % | 场景 |
|---|---|---|---|---|---|
| R8-C1 hot_prng_n hoist | ~38 cyc/pass（19 DRAM→≈0） | 285,609 pass/s | ~10.9 M cyc/s | —（按秒） | 空闲/待机（guest_pct −~6 pt，推测） |
| R8-C2 重叠块→桩 | 减少块体 miss 行（3.4 行/dispatch 反推，R6B §3.1） | 6,788 dispatch | 7.0 M 预算的一部分，可达 20–40 % | **可达 20–40 %** | 重绘 |
| R8-C3 计数器+flag | ~30 cyc/dispatch | 6,788 | ~0.20 M | ~1.8 % | 重绘 |
| R8-C4 AotEnt 6B | DROM 行密度 +25 % | 6,788 | 推测 0.1–0.3 M | ~1–2 % | 重绘 |
| R8-C5 spi_direct_r 32bit | 2–4 cyc/读 | 精灵数据读 | 推测 <0.1 M | <1 % | 重绘 |

## 3. 需要主审裁决的冲突/不确定项

1. **R8-C2 是否本轮就动生成器？** 收益最大但改动在 `tools/aot.py`（不在本路可写范围），且有 `AOT_GROUP`
   负结果。建议主审拍板：单独一轮、真机 A/B（`ft/gmax/aotmiss`），不与其他改动混同。
2. **R8-C1 的 `hot_prng_n` hoist 与注释不一致，是「注释作废」还是「代码漏写」？** 我判断是代码漏写（注释
   描述的目标语义正是安全且更快的），但需主审确认写回顺序的逐 pass 等价性后落地。
3. **R8-C6 的「提前进 WFI」**：本路结论是它等价于 C1（砍 hot_prng_n 开销），LFSR 不能跳；若主审想要更大
   的待机收益，需另立「LFSR 跳跃表」研究项，本路不主张本轮做。

## 4. 明确不动的清单（防误删）

| 对象 | 理由 |
|---|---|
| `wait_loop_collapse` / `spin_collapse` 函数本体与预筛逻辑 | 折叠是活的（collapse=285 K pass/s）；只改 C1 的内部循环，不动判定 |
| `hle_push_pixels` / `hle_fill_span` / `hle_mul32` / `hle_render_band` / `hle_blit_key` | HLE 是重绘帧唯一有效提速（PERF_2026-11-22 §3「唯一有效 +12.6 %」） |
| `aot.h` 的 8 B `AotEnt` 布局（fn+cnt 同行） | 17 fps↔59 fps 的教训（R7_aot §7）；C4 只裁死字段 cyc/pad，不动 fn/cnt 同行 |
| `romstore.h` 的 overlay 范围测试与 `m_base` 直映射顺序 | 0xFFFFFFFF 绕回 bug 换来的（R7_core §4） |
| `gpce4.cpp` 的 tick() 快路径合并、`cycles_to_next_event` | 30 ms→3.2 ms 的既有成果（R5/R6），非热点不动 |
| `st7735.cpp` 的窗口推进 / `data_w` 状态机 | 字节流状态一致性；R7 已只动 `m_dirty` 一行，不再碰 |
