# R7：设备侧核心冗余与开销审计（SoC / 解释器 / LCD / 存储）

日期：2026-11-27
基线：`firmware/digicore-c3/build/digicore-c3.elf`（2026-11-27 19:16:19，`nm/size/objdump` 全部取自该 ELF）
角色：模拟器核心审计员（**只读**，未改动任何 `.c/.cpp/.h/.py`，未运行 `idf.py`）
范围：`main/emu/{gpce4.cpp,gpce4.h,unsp_glue.cpp,st7735.cpp,st7735.h,romstore.cpp,romstore.h,aot.h,hotblock.h,isa/*,compat/*}`
采用的前轮硬事实（不再论证）：重绘帧 host cycles/dispatch ≈ **1,107**；稳定态 **16 ms/帧**；SPI 实测 39.8 Mbit/s；
AOT 块体在 flash 有 **32 B 行 miss**；IRAM 名单由 `tools/coverage/out/aot_iram_r6.txt` 控制；`hle_*` 在 slice 边界会 bail。

---

## 0. 结论先说

1. **本轮范围内只有两条"能上 1 %"的浪费，而且都在 dispatch 上，不在像素上。**
   `wait_loop_collapse()` 每 dispatch 一次真调用（≈11 cycles）+ 5 条 HLE 钩子比较链（≈22 cycles），
   合计 ≈33 cycles × 6,788 dispatch = **0.22 M host cycles ≈ 3.0 % 的重绘帧**。
   这两条 R6B 已经逐条量过（W2 / W1），但当时的分母是 1,724 cycles/dispatch，W1-W6 合计 72 cycles 只值 4.2 %，
   结论是"不值得单独开一版"。本轮分母是 **1,107**，同样的 72 cycles 变成 **6.5 %**；
   其中 W1+W2 这 33 cycles（**3.0 %**）可以只在本模块内、不碰 `tools/aot.py` 的前提下拿掉。
   **本报告全部可修项合计 ≈5.3 %（0.40 M / 7.51 M 重绘帧）**：R7-01 1.0 + R7-02 2.0 + R7-05 1.3 + 其余 ~1.0。

2. **像素侧唯一上 1 % 的是 `m_dirty` 位图的"每像素读改写"**（`st7735.cpp:299`、`:357`）。
   一次满屏重绘里真正落进 `fb[]` 的像素 ≈ **27.3 K**（`fillpx=9,111 + pushpx=18,193`，每秒口径；
   `blitpx=18,391` 写的是 guest 的 96 字行缓冲，不碰 `fb[]`，所以**不算**），
   每像素一次 `lbu/ori/sb` ⇒ **~95 K cycles ≈ 1.3 % 的重绘帧**，而它可以按行合并成每行 1 次。

3. **`-DC3_HLEPROF=1` 只作用于 5 个钩子调用点，代价被注释低估了 3 倍，但绝对量小。**
   每次钩子入口 2 次 `rdcycle` + 一次 `.bss` 读改写 ≈ **8–12 host cycles**（注释写"~3"）；
   重绘帧 ≈2,650 次入口 ⇒ **21–32 K cycles ≈ 0.3 %**；稳定态 ≈0.02 %。
   **消费者只有 `>60 ms` 帧的 LONGF 行**（`app_main.cpp:3565-3570`），STAT 行并不用它。
   → 结论：不是"拖慢运行"的元凶，但它是一个**已经答过题**的探针，建议关掉或至少改掉那句注释。

4. **闲置统计字段是真问题，但问题不在 cycles，在"读日志的人会被骗"。**
   本轮用 ELF 逐个核对：`g_wait_top/g_wait_exit/g_frame_body/g_body_alt/g_spin_*/g_isr_hist/g_pc_ring/
   g_cyc_*/g_disp_*/g_isr7_*/g_tick_*/g_aot_uniq` 共 **≈580 B DRAM 全部永不写入**，而 STAT 行照常打印
   `isr7=0/0 collapse=… spin=0/0/0/0 rband=0 caot=0 … pgsw=0`。
   真机日志 `dbgtest/r5_after_force.log` 里这一整段确实是 0。**谁拿 `isr7=0` 去推断"IRQ7 从没进过"就会得出错误结论**
   （实际 IRQ7 未触发的原因是 `app_main.cpp:3146-3151` 用主机直接喂 `[0x0850]`，与 CPU 是否进 ISR 无关）。

5. **三处可证的死分支/自赋值，其中一处是"注释想做、代码没做"。**
   `gpce4.cpp:1048-1050` 的 `if (resume == 0x200300u && …) resume = 0x200300u;` —— 自己赋给自己，
   注释说的"park it back on its own key-wait loop (0x5240)"根本没写出来。
   `unsp_glue.cpp:1790` 的"水平翻转"分支在预扫描之后**恒不成立**（详见 R7-04），连带 20 行死代码。
   `unsp_glue.cpp:2044-2077` 的第二个 run loop 在设备上**永不进入**（三个钩子指针在设备树里无人赋值）。

6. **`Gpce4::read16/write16` 确实有可合并的重复范围判断，但省下来的是 0.0006 % 量级。**
   真机 `probe_auto1200.txt` 实测 IO 写总量 3,151,300 / 1200 s = **2,626 次/秒**，
   合并掉 4 条比较 ≈ 10 K cycles/s。**这是卫生问题，不要当性能项报。**

7. **日志类代码在 ISA 五个文件里是"编译期彻底删掉"的，一个字节都不花。**
   `compat/emu.h:30-36` 把 `logerror/LOGMASKED` 定义成 `do{}while(0)`，参数不参与求值；
   `machine().describe_context()` 这种根本编不过的表达式就写在 LOGMASKED 参数里，能编过本身就是证据。
   → 第一轮担心的"printf 参数求值"在 `isa/*.cpp` 里**不存在**；真正的日志税只剩 R7-03/R7-10 与 `m_px_writes`。

---

## 1. 审计口径

| 量 | 取值 | 来源 |
|---|---|---|
| 重绘帧预算 | 6,788 dispatch × 1,107 = **7.51 M host cycles**（160 MHz 下 47 ms） | 前轮分母 × 本轮给定的 1,107 |
| 稳定态帧预算 | 16 ms × 160 MHz = **2.56 M host cycles** | 本轮给定 |
| dispatch 数/重绘帧 | 6,788 | `docs/notes/R6B_aot.md` §3 |
| 一次满屏重绘的像素 | **进 `fb[]` 的 27.3 K**（push 18.2 K + fill 9.1 K）；另有 blit 18.4 K 只写 guest 行缓冲 | `dbgtest/r5_after_force.log` STAT 窗口 |
| IRAM（`.iram0.text`） | 119,350 B 全镜像；本模块 ≈11.9 KB | ELF `size -A` |

编译开关现状（`main/CMakeLists.txt:48-51`）与"实际生效"：

| 开关 | 值 | 生效位置 | 每帧代价 |
|---|---|---|---|
| `C3_PROF` | 0 | 编译期删 | 0 |
| `C3_SPLIT` | 0 | 编译期删（`g_cyc_*` 只留 36 B BSS） | 0 |
| `C3_AOTACC` | 0 | 编译期删（`g_rd_*` 连定义都没有） | 0 |
| `C3_MISS_CENSUS` / `C3_BLOCK_CENSUS` | 0 | `aot_note_miss/block` 变成空 inline | 0 |
| `C3_HLEPROF` | **1** | `unsp_glue.cpp:1922/1932/1943/1954/1964` 五处 | 21–32 K/重绘帧 |
| `C3_WAIT_COLLAPSE` | **1** | `unsp_glue.cpp:1896-1903`，**每 dispatch 一次调用** | 75 K/重绘帧 |
| `AOT_NO_HOOKS` | 定义 | `aot.h:302-309` 把三个 hook 宏清空 | 0（但见 R7-10） |

> 注意：`unsp_glue.cpp:84-91` 的注释说 WAIT_COLLAPSE "permanently disabled … default 0"，
> 而 CMakeLists 传的是 **1**。注释与构建不一致，读注释的人会以为这个折叠没开。

关键符号的真实尺寸/位置（ELF `nm -S`）：

| 符号 | 大小 | 地址 | 区 |
|---|---|---|---|
| `unsp_device::execute_run()` | **5,434 B** | 0x40382fd4 | IRAM |
| `unsp_device::wait_loop_collapse()` | 838 B | 0x40382aec | IRAM |
| `unsp_device::spin_collapse()` | 312 B | 0x403829b4 | IRAM |
| `unsp_device::aot_step()` | 418 B | 0x40382e32 | IRAM |
| `Gpce4::read16` | 244 B | 0x4201b3de | **flash** |
| `Gpce4::write16` | 276 B | 0x4201c7e6 | **flash** |
| `St7735::fill_run` / `write_run` | 336 / 376 B | 0x4201da42 / 0x4201db92 | **flash** |
| `unsp_device::hle_blit_key()` | 830 B | 0x4201ffd6 | **flash**（其余四个 HLE 钩子被内联进 execute_run → IRAM） |

---

## 2. 摘要表

| ID | 位置 | 类型 | 每次调用额外开销 | 影响（重绘帧 7.5 M） | 风险 | 建议 |
|---|---|---|---|---|---|---|
| R7-01 | `unsp_glue.cpp:1896-1903` | 每 dispatch 真调用一个 838 B 函数判 2 个 PC | 4 条 + callee 8 条 ≈ 10–11 cyc | 75 K ≈ **1.0 %**（稳定态≈0） | 低 | 先内联 `lpc==0x230321\|\|lpc==0x000321` 再调；照抄隔壁 `spin_collapse` 的写法 |
| R7-02 | `unsp_glue.cpp:1920-1969` | 5 条「全局 bool + PC 比较」链 | 24 条指令 + 5 分支 ≈ 22 cyc | 149 K ≈ **2.0 %** | 低 | 32 位低位掩码预筛（≈4 条）→ 期望 6.8 cyc，省 ~1.4 %；或按 R6B W1 走 `AotEnt.pad` |
| R7-05 | `st7735.cpp:299`、`:357` | 每像素 `m_dirty[ry>>4] \|= bit` | 3–4 cyc/px | 27.3 K px ⇒ 95 K ≈ **1.3 %** | 低 | 按行合并成每行 1 次写入；语义完全等价 |
| R7-06 | `unsp_glue.cpp:1231/1271/1272` | `g_blit_cyc_per_px` 非 const 全局，每像素重读+重乘 | 1 load + 1 mul ≈ 3 cyc/px | 55 K ≈ 0.7 %（**推测**） | 极低 | 函数入口取一次到局部 |
| R7-03 | `unsp_glue.cpp:143-165, 1922…1966` | `C3_HLEPROF=1`：每入口 2×rdcycle + BSS 读改写 | 8–12 cyc/入口 | ≈2,650 入口 ⇒ 25 K ≈ **0.33 %** | 极低 | 关掉（或改注释：3→10，消费者只有 LONGF） |
| R7-07 | `unsp_glue.cpp:2044-2077`、`:1862` | 设备上永不进入的第二个 run loop + 每 dispatch 判空 `m_block_hist` | 3 cyc/dispatch（可去） | 20 K ≈ 0.3 % + 死 IRAM | 中（`sync_c3_emu.py` 会覆盖） | 待主审裁决（R7-07 条目） |
| R7-04 | `unsp_glue.cpp:1703-1710`、`:1790` | 每行重扫记录表的"翻转"预检 + 20 行恒不执行分支 | ~3 cyc/条记录 | <1 K ≈ 0.01 % | 低 | 删死分支；预检提到"每 band 一次"或按 R7-04 方案合并 |
| R7-08 | `gpce4.cpp:591-600` | 饱和后仍先线性扫 64 项再判"满了" | 64 cmp ≈ 130 cyc/次 | 实测 ≈0（`probe_*.txt` 800 次/1200 s） | 极低 | 把 `count >= MAX` 判断提到循环之前（行为逐位等价） |
| R7-09 | `gpce4.cpp:651-671`、`:697-712` | read16 的 ROM 两段可合一段；write16 三条路径都落到 `unmapped_write` | 4 cmp | ≈10 K/s ⇒ **0.0006 %** | 极低 | 只作卫生修（v2 合并式见正文） |
| R7-10 | `gpce4.cpp:1204-1211`、`unsp_glue.cpp:1473-1495`、`aot.h:360-368` | 12 个永不写入的统计字段，≈580 B DRAM，STAT 恒 0 | 0（无人写） | 0 cycles，**误导性 100 %** | 低 | 删或加 `#if`；至少把 STAT 里恒 0 的字段标 `n/a` |
| R7-11 | `gpce4.cpp:1048-1050` | 自赋值死分支（注释说要跳到 0x5240，代码写成原地） | — | 0 cycles | 中（疑似漏改） | 确认意图后要么删、要么写对 |
| R7-12 | `unsp_glue.cpp:363` | `op0 < 0xf` 在上一行已 `return`，恒真 | 1 cmp/解释指令 | <1 K ≈ 0.01 % | 极低 | 删掉该子条件 |
| R7-13 | `gpce4.cpp:574-589` | flash 数据读每次都算 64 位 `offset*2 + bank*0x400000` 并做 64 位比较 | 2–4 cyc/次（**推测**） | 未知，需 `rds` 打开才能测 | 低 | 32 位化；先开 `C3_AOTACC` 测一次 `rds` |
| R7-14 | `st7735.h:54`、`st7735.cpp:91/288/339` | `m_px_writes` 只写不读 | 1 RMW/字节路径像素 | 0（无读者） | 极低 | 删字段或接到 STAT |

---

## 3. 逐条证据

### R7-01 · `wait_loop_collapse()` 每 dispatch 一次真调用（≈1.0 %）

`main/emu/unsp_glue.cpp:1896-1903`

```cpp
#if C3_WAIT_COLLAPSE
			if (wait_loop_collapse(lpc))
			{
				if (core->m_sirq)
					check_irqs();
				continue;
			}
#endif
```

机理：`C3_WAIT_COLLAPSE=1`（CMakeLists 传的），所以这行不是编译期的空。
`wait_loop_collapse` 在 ELF 里是 **0x346 = 838 B 的独立函数**（0x40382aec），不可能被内联；
进入后先读 `m_fastwait`（DRAM），再比 `lpc != 0x230321u`、`lpc != 0x000321u`，然后返回 false。
**除折叠点外每次 dispatch 都为这个"否"付一次真调用 + 一次 DRAM 读。**

量化：R6B W2 反汇编计数 4 条 + callee 8 条 ≈ 11 cycles。11 × 6,788 = **75 K cycles ≈ 1.0 % 的重绘帧**。
稳定态：真机 `collapse=285,609`/s、`collcalls=19,154`/s ⇒ 60 fps 下 **≈319 dispatch/帧**
（其中 319 次是折叠命中，本身要付这次调用），11 × 319 ≈ **3.5 K ≈ 0.14 %**。
即：**这一项只在重绘/切换帧上值钱**，那正是用户抱怨"切换时慢动作"的场景。

改法（零风险）：把 PC 预筛内联，函数只保留真正的折叠逻辑 ——
`unsp_glue.cpp:1904-1908` 的注释已经把同一件事为 `spin_collapse` 写过一遍：
"Calling it unconditionally spent a cross-TU function call on every dispatch … so ~97 % of dispatches paid for a lookup"。
隔壁 1909-1911 就是这么改的，只有 1896 这处漏了。

```cpp
			if ((lpc == 0x230321u || lpc == 0x000321u) && wait_loop_collapse(lpc))
```
（`m_fastwait` 的检查留在函数内部，语义不变。）

### R7-02 · 5 条 HLE 钩子比较链（≈2.0 %）

`main/emu/unsp_glue.cpp:1920-1969`（只留条件行，每行的 `{}` 里还有 `HLE_T0()` 与钩子调用）

```cpp
			if (m_hle_blit && lpc == AOT_HLE_BLIT_ENTRY)      // 0x207CA3
			if (g_hle_push_px && lpc == AOT_HLE_PUSH_ENTRY)   // 0x005B4A
			if (g_hle_fill_px && lpc == AOT_HLE_FILL_ENTRY)   // 0x005B1E
			if (g_hle_mul32 && lpc == AOT_HLE_MUL_ENTRY)      // 0x005F8B
			if (g_hle_render_band && lpc == AOT_HLE_BAND_LOOP)// 0x207A35
```

机理：每条 = 一次全局 `lbu`（`.bss`/`.data`，C3 无 D-cache）+ `lui/addi` 立即数 + `beq`，
5 条各带一个分支；`lpc` 在寄存器里但 5 个比较都串行。R6B W1 反汇编计数 **24 条指令 / 5 分支 ≈ 22 cycles**。

量化：22 × 6,788 = **149 K ≈ 2.0 %**。这是本模块单项最大。

省钱的非生成器改法（不需要动 `tools/aot.py`、不会被 `sync_c3_emu.py` 覆盖掉逻辑）：
把 5 个 PC 的低位摊到一个 32 位掩码里做一次预筛 ——
`(lpc >> 1) & 31` 对 5 个 PC 的落点是 {17, 5, 15, 5, 26}
（`0x005B4A` 与 `0x005F8B` 撞在 5 —— 碰撞只造成假阳性，链内还要精确比较，不影响正确性），
于是 `if (hook_mask & (1u << ((lpc >> 1) & 31))) { …原来的 5 条链… }`
= 1 shift + 1 and + 1 beq（掩码常驻寄存器）≈ 4 条，假阳性率 **4/32 = 12.5 %**。
期望 4 + 0.125×22 ≈ **6.8 cycles** ⇒ 每 dispatch 省 ~15 ⇒ **1.4 %**。
彻底方案仍是 R6B W1（`AotEnt.pad` 存 hook_id，aot_step 返回 0/1/2），但那要改生成器。

### R7-05 · 每像素一次 dirty 位读改写（≈1.3 %）

`main/emu/st7735.cpp:296-300`（`fill_run`，`write_run:354-358` 同形）

```cpp
        if (rx < unsigned(PANEL) && ry < unsigned(PANEL))
        {
            fb[ry * PANEL + rx] = pixel;
            m_dirty[ry >> 4] |= uint8_t(1u << (rx >> 4));
        }
```

机理：`m_dirty` 是 DRAM 里的 8 字节位图，`|=` 编译成 `lbu/ori/sb`（C3 无 D-cache，SRAM 每次访问 ~1–2 cycles），
而同一个字节在一次 run 里会被 OR **几十到上百次**（一行 96 像素、16 像素一个 tile ⇒ 6 个 tile 反复置位）。
`fb[]` 的那次 `sh` 是真正必要的工作，`m_dirty` 的读改写是纯重复。

量化：一次满屏重绘落进 `fb[]` 的像素 = fill 9,111 + push 18,193 ≈ **27.3 K**
（`blitpx=18,391` 写的是 `m_ram + 0x07BA` 的行缓冲，由后面的 push 再进 `fb[]`，所以只算一次），
3–4 cycles/px ⇒ **~95 K ≈ 1.3 % 的重绘帧**；稳定态 ≈455 px/帧 ⇒ 1.6 K ≈ 0.06 %。

改法（语义完全等价 —— 置的是同一批位，只是不再逐像素落盘）：
一次 run 内 `ry` 与 `rx>>4` **只在换行时改变**，所以把这一行的位先在局部字节里累积，换行/结束时写一次：

```
mask |= 1u << (rx >> 4);          // 纯寄存器
if (行切换或 run 结束) { m_dirty[ry >> 4] |= mask; mask = 0; }
```
每行 1 次 RMW 取代 96 次（`fill_run` 用 `m_posx` 判换行，`write_run` 用 `px`）。
`blit_frame()` 是"读一次、清一次"，一帧内提前置位不会漏推；`store_pixel`（字节路径，2.5 K px/s）可以不动。

### R7-06 · `g_blit_cyc_per_px` 每像素重读（≈0.7 %，推测）

`main/emu/unsp_glue.cpp:1231`、`:1271`、`:1272`

```cpp
	if (budget < (int32_t)g_blit_cyc_per_px)
		return false;                   // no room for even one pixel: interpret
	…
		cycles += g_blit_cyc_per_px;
		if (++pixels > 0x10000u)
```

机理：`g_blit_cyc_per_px` 是**非 const 全局**（`unsp_glue.cpp:1199`，控制台 `B <n>` 可改），
循环体内含 `m_bus->fetch_window()`（虚调用）与 `write16()`（可能走虚调用），
编译器无法证明这些调用不写该全局，于是**每像素重读一次 + 重算一次乘法**。
注释里"per-pixel 139"是常量折叠的假设，代码不是。

量化：18,391 blit px（≈1 次重绘的量）⇒ 3 cycles × 18.4 K ≈ **55 K ≈ 0.7 %**。标**推测**：GCC 可能把乘法强度削弱，
但重读几乎肯定存在（有虚调用挡着）。
改法：函数入口 `const uint32_t cpp = g_blit_cyc_per_px;`（一行，可被 hoist）。

### R7-03 · `-DC3_HLEPROF=1` 的真实作用域与代价

生效位置只有五个（`unsp_glue.cpp`）：1922（blit）、1932（push）、1943（fill）、1954（mul）、1964（band），
每个都是 `HLE_T0(); …hook…; HLE_ADD(g_h_x);`。宏在 `:153-165`：

```cpp
#define HLE_T0() const uint32_t __ht0 = esp_cpu_get_cycle_count()
#define HLE_ADD(var) var += (esp_cpu_get_cycle_count() - __ht0)
```

机理与代价：每次**钩子入口**（注意不是每次 dispatch，也不是每像素）付
2 × `csrr 0xC00` + 1 sub + 全局 `lw/add/sw` ≈ **8–12 cycles**，
外加把 `__ht0` 钉在一个寄存器上穿过钩子调用（会加一次 spill，未计入）。
`unsp_glue.cpp:146-149` 的注释写"the hook is entered once per row/blit, not once per pixel, so the cost is
~3 host cycles per call - noise against a 14 ms frame" —— **"3 cycles"低估约 3 倍**，而且"14 ms frame"是稳定态，
HLE 最忙的恰恰是 47 ms 的重绘帧。

每帧入口数（用真机 STAT 与注释里的计数交叉）：mul ≈2,157 + push ≈190（18,193 px ÷ 96）+ fill ≈95 + band ≈192（96 成功 + 96 bail）+ blit ≈2
⇒ **≈2,650 次/重绘帧** ⇒ 21–32 K cycles ⇒ **0.3–0.4 %**；稳定态 ≈0.02 %。

消费者：只有 `app_main.cpp:3565-3570` 的 **LONGF 行（`frame_us > 60000`）**；STAT 行不含 `g_h_*`。
即"每帧都付、大多数帧不打印"。**建议 `-DC3_HLEPROF=0`**，或把注释改成实测值并只保留入口计数（去掉 rdcycle）。
→ 见「待主审裁决」A。

### R7-04 · `hle_render_band` 的"翻转预检"：每行重扫 + 20 行恒不执行分支

`main/emu/unsp_glue.cpp:1703-1710`（预检）与 `:1790`（主循环里再判一次）

```cpp
	for (uint16_t i = idx; i < count; i++)
	{
		if (d->read16(AOT_HLE_BAND_REC + (uint32_t)i * 12u + 0x0Bu) != 0u)
		{
			g_hle_band_flip++;
			return false;               // 交给 guest
		}
	}
	…
				if (d->read16(rec + 0x0Bu) != 0)   // horizontally flipped
```

机理（**可证**）：
1. 预检对 `[idx, count)` 的每条记录都要求 `rec[0x0B] == 0`，否则整行 bail；
2. 主循环遍历的正是同一个 `[idx, count)`，且循环体只写 `0x07BA..0x0819` 的扫描线缓冲
   （记录表在 `0x0608 + 12n`，`n ≤ (0x07B8-0x0608)/12 = 36`，最高字节 `0x07B7`，**不重叠**）；
3. `count` 在 `:1689` 读一次后不再变。

⇒ `:1790` 的条件恒为假，`:1791-1810` 的翻转分支（20 行、含一个逆序循环）**永不执行**，
同时每条记录多付一次 `read16`。

量化：预检是"每行一次"的 O(count−idx)，而记录表在整条 band 内不变 —— 属于"把常量算在循环里"的变体。
真机稳态 `bandc=510/s`、`bandb=425/s`、`bandr=209/s` ⇒ 成功 85 次/s，每次预检 ≈2.5 条记录 ⇒ ~210 次读/s，
`d->read16` 命中 `m_ram` 快路径（0x0608+ ≤ 0x0FFF）≈3 cycles ⇒ **~700 cycles/帧量级 ⇒ ≈0.01 %**。
**所以这条是"冗余 + 死代码 + 语义陷阱"，不是性能项**：一旦将来有人真的打开了翻转路径，
预检会先把整行 bail 掉，那个分支永远测不到。

建议：(a) 删掉 `:1791-1810`；(b) 预检改为"每个 band 一次"（用 `[bp]` 的 y 或 band 起始做 key），
或直接把预检合并进主循环（需要接受"部分写入再 bail"的语义 —— 由于每条记录的写入是幂等的
`line[bias+i] = v`，guest 重跑同一行会得到同样的内容，但这条要主审确认）。
→ 见「待主审裁决」B。

### R7-07 · 设备上永不进入的第二个 run loop（死 IRAM）

`main/emu/unsp_glue.cpp:1838-1842` 与 `:2044-2077`

```cpp
	if (m_insn_hook == nullptr && m_cycle_hook == nullptr)
	{
		… 热路径 while (core->m_icount >= 0) …
		return;
	}

	while (core->m_icount >= 0)          // ← 设备上永远到不了这里
	{
		…
		if (m_cycle_hook) { constexpr int probe = 1 << 28; … }
```

证据：`grep -r "m_insn_hook|m_cycle_hook|m_block_hist" main/` 在**设备树里没有任何赋值点**，
只有 `compat/unsp.h:141-147/222` 的默认 `nullptr` 声明和 `unsp_glue.cpp` 的读取。
`C3_PROF=0` 也不产生 tracer。⇒ 那段 34 行（含 `1<<28` 探针）是死代码。

代价：(a) 它在 `UNSP_HOT` = `IRAM_ATTR` 里，整个 `execute_run` 是 **5,434 B IRAM**，
死循环与热循环共享大多数内联体（`execute_one`/`check_irqs`），所以**边际 IRAM 无法从 objdump 精确切分，估计 100–300 B（推测）**；
(b) `:1862` 的 `if (m_block_hist) m_block_hist[lpc & 0x3ffffu]++;` 是**每 dispatch 一次 DRAM 读 + 分支**
（R6B W4：3 cycles ⇒ 20 K/重绘帧 ≈ 0.3 %）。

风险：`unsp_glue.cpp` 是 `tools/sync_c3_emu.py` 的**生成产物**（脚本只允许插入 `m_insn_count` 那几行，
其它改动会被 `refuse_clobber` 拦下或直接覆盖）。所以不要在设备副本里手改，必须改 `emu/unsp_glue.cpp` + 脚本，
或者给这三处加一个 `C3_HOSTTRACE` 开关。→ 见「待主审裁决」C。

### R7-09 · read16 / write16 的重复范围判断（可合并，但只值 0.0006 %）

`main/emu/gpce4.cpp:651-671`

```cpp
    if (addr >= 0x004000 && addr <= 0x00bfff)  // 64 KB internal mask ROM
        return internal_rom_r(addr - 0x004000);
    if (addr >= 0x00c000 && addr <= 0x00ffff)  // first 32 KB mirrored
        return internal_rom_r(addr - 0x00c000);
```

两段可合成一段（**逐位等价**，已验证：`0xC000-0x4000 = 0x8000`，`& 0x7FFF = 0`，正好等于镜像到 0）：

```cpp
    if (addr >= 0x004000 && addr <= 0x00ffff)
        return internal_rom_r((addr - 0x004000) & 0x7fffu);
```
省 2 条比较。

`main/emu/gpce4.cpp:697-712`

```cpp
    if (addr >= 0x004000 && addr <= 0x00ffff) {  // ROM is read only
        unmapped_write(addr, data); return; }
    if (addr >= 0x003000 && addr <= 0x0030ef) { io_w(…); return; }
    if (addr >= 0x200000 && addr <= 0x3fffff) {  // flash window, read only
        unmapped_write(addr, data); return; }
    unmapped_write(addr, data);
```
三条路径的终点都是 `unmapped_write`，可以整段塌成"RAM → IO → 其余一律 unmapped"：
非 RAM 写从 5–6 条比较降到 3 条，**IO 写从 5 条降到 3 条**。

但要说清楚量级：真机 `dbgtest/audio/probe_auto1200.txt` 实测 IO 写 **2,626 次/秒**（1200 s 共 3,151,300 次，
其中 `SPI2_TXData` 3,043,022 —— 即未被 HLE 吸收的 LCD 字节路径）。省 4 条比较 ≈ **10 K cycles/s**，
相对 160 MHz 是 **0.006 %**。→ 归类"卫生"，不要写进性能收益表。

### R7-08 · `note_unmapped()` 饱和后仍在扫 64 项

`main/emu/gpce4.cpp:591-600`

```cpp
    for (int i = 0; i < count; i++)
        if (list[i] == addr)
            return false;
    if (count >= UNMAPPED_LOG_MAX)          // ← 应该放在循环之前
        return false;
```

机理：一旦两个日志都攒满 64 个不同地址（`UNMAPPED_LOG_MAX = 64`），
**之后每一次未映射访问都要扫 64 项**（2 loads/cmp + 分支 ≈130 cycles）才得出"不会再记了"。
真机日志 `dbgtest/audio/probe_*.txt` 里 "unmapped writes = 64 distinct address(es)" 就是这个饱和态，
说明路径**确实会走满**；只是触发地址（PC 树侧的 `0x300c-0x300e`、`0x3036`，设备侧后三个已映射）每 1200 s 才 800 次，所以现在≈0。
一行改动（把 `if (count >= MAX) return false;` 提到循环前），行为逐位等价。
**它今天的代价是 0，但它是"日志代码在日志结束之后继续收费"的典型，而且一旦某个未映射地址进了热循环就会变成每访问 130 cycles。**

### R7-10 · 永不写入的统计字段（≈580 B DRAM + 一整排恒 0 的 STAT 字段）

本轮用 ELF 逐个核对（`nm -S`，指针在 `.bss` 就说明它没被 GC 掉、确实占 DRAM）：

| 字段 | 定义处 | 唯一写入点 | 实际 |
|---|---|---|---|
| `g_wait_top / g_wait_exit / g_frame_body / g_body_alt` | `unsp_glue.cpp:1492-1495` | **无** | 恒 0 |
| `g_spin_calls/seen/big/short/val/bp/ic/ok` | `unsp_glue.cpp:19-26` | `SPIN_PROBE`，`C3_SPIN_DIAG=0` 时是 `((void)0)` | 恒 0 |
| `g_isr_hist[0x45]`（276 B） | `unsp_glue.cpp:1485` | `execute_run:1852`，`C3_IRQ_DIAG=0` | 恒 0 |
| `g_pc_ring[48]`（192 B） | `unsp_glue.cpp:1473` | `:1857`，同上 | 恒 0（`aot_dump_pc_ring` 只能打出 `PCRINGHEAD/PCRINGEND` 两行） |
| `g_isr7_hits / g_isr7_insn` | `unsp_glue.cpp:1480-1481` | 同上 | 恒 0（**而 `g_irq7_refused` 是活的**） |
| `g_tick_writes/pc/val` | `gpce4.cpp:1208-1210` | `C3_TICK_DIAG=0` | 恒 0 |
| `g_cyc_*/g_disp_*`（36 B） | `unsp_glue.cpp:112-128` | `C3_SPLIT=0` | 恒 0 |
| `g_aot_pg_switch` | `aot.h:367` | **无**（grep 全树只有定义与打印） | 恒 0，但 `pgsw=` 照打 |
| `g_aot_uniq` | `unsp_glue.cpp:900` | `aot_mark_uniq` 只被 `#ifndef AOT_NO_HOOKS` 调用 ⇒ 设备上不调用 | 恒 0（`CENSUS end 0`） |
| `m_px_writes` | `st7735.h:54` | `store_pixel:91`、`fill_run:288`、`write_run:339` | **只写不读，无任何读者** |

顺带一个好消息：`g_aot_uniq_bits[128]`（512 B）**已经被链接器 GC 掉了**（ELF 里没有这个符号），
所以"512 B 位图白占 DRAM"这条不成立 —— 只有 `g_aot_uniq` 这 4 B 还在。

为什么这条值得写进报告而不是"4 字节无所谓"：真机日志 `dbgtest/r5_after_force.log` 里
`isr7=0/0 … spin=0/0/0/0 … rband=0 caot=0 cinf=0 … pgsw=0 rdf=0 rds=0` 全都打着 0。
**谁拿 `isr7=0/0` 推断"CPU 没进过 IRQ7"，结论就是错的**（实际是 `app_main.cpp:3146-3151` 用主机直接喂 `[0x0850]`，
64 Hz 源在模型里根本没起来 —— 这正是 `unsp_glue.cpp:1488-1496` 那段 "if the device leaves zero times, the tick never arrives"
注释要查的事，而查它的计数器已经全部编译掉了）。
同理 `pgsw=0` 是"没人写"，不是"没有跨页"。

建议：删掉，或统一加 `#if` 并在 STAT 里打 `n/a`。**不要留着一排 0 让人当测量结果读。**

### R7-11 · `system_sleep_w()` 的自赋值死分支

`main/emu/gpce4.cpp:1044-1054`

```cpp
        uint32_t resume = m_sleep_resume_pc;
        if (resume == 0x200300u && ram[0x0C88] == 0)
            resume = 0x200300u;      // ← 自己赋给自己
```

机理：条件成立时赋的还是原值，**整个 if 是空操作**。
而上面的注释明确说要"park it back on its own key-wait loop (**0x5240**, the one that reads PORTA into `[0x0853]`)"。
⇒ 要么是漏改（本该写 `0x005240`），要么这段意图已经作废。
运行代价 0（睡眠路径 16 s 才走一次），但它是**行为与注释矛盾**的证据，值得进"待确认"而不是"性能"。

### R7-12 · `execute_one()` 里恒真的 `op0 < 0xf`

`main/emu/unsp_glue.cpp:354-370`

```cpp
	if (op0 == 0xf)
		return execute_fxxx_group(op);

	if (op0 < 0xf && opa == 0x7 && op1 < 2)   // ← op0 < 0xf 恒真
		return execute_jumps(op);
```
上一行已经把 `op0 == 0xf` 返回掉。删掉子条件省 1 条比较/解释指令。
解释器指令在重绘帧里是少数（多数走 AOT 块），所以 ≈0.01 %。**卫生项。**

### R7-13 · flash 数据读的 64 位地址运算（机制确认，量未测）

`main/emu/gpce4.cpp:574-589`

```cpp
    const uint64_t base = uint64_t(offset) * 2u + uint64_t(m_spi_bank) * 0x400000u;
    if (base + 1 >= m_flash->size()) { … }
    return m_flash->read16(uint32_t(base));
```
`m_spi_bank` 在整局里基本是 0，但每次都算一次 64 位乘加 + 64 位比较（RV32 上是 2 字运算）。
32 位化后是 `offset*2 + (m_spi_bank << 22)`（8 MB 镜像 = 2 × 0x400000，`m_spi_bank` 实际只有 0/1）——**2–4 cycles（推测）**。

更要紧的是位置事实：**`Gpce4::read16` 在 flash（0x4201b3de），`execute_run`/`aot_step` 在 IRAM**。
每次 AOT 块访问 `> 0x0FFF` 的地址（mask ROM 数据表、flash 图形数据）都要
`aot_rd` → 间接调用 `aot_io_rd` → 虚调用 `read16`（flash 行 miss ≈300 cycles）→ `spi_direct_r` → `SparseRom::read16`。
这条链上有 4–5 次 flash 行触达。**本轮不主张把它算作"冗余"（它是 R6B (d') 的同一件事），
只指出：想量它必须先打开 `C3_AOTACC` 拿 `rds` —— 而那个计数器现在恒 0（见 R7-10）。**

### R7-14 · `m_px_writes` 只写不读

`main/emu/st7735.h:54` / `st7735.cpp:91`

```cpp
    uint32_t m_px_writes = 0;   // "Total pixels the guest has pushed into the panel"
```
`grep -rn "m_px_writes" main/` 只有三处 `++/+=`，**没有任何读者**（STAT 用的是 `pushpx/fillpx/blitpx` 三个钩子计数）。
`store_pixel` 每次多一次 DRAM 读改写（字节路径 2.5 K/s，≈0），`fill_run/write_run` 每次 run 一次（可忽略）。
建议删除，或把它接进 STAT 当"模型侧真实像素数"（与钩子计数交叉验证反而有用）。

---

## 4. 不要动清单（买来的性能，本轮明确不建议删/改）

| 位置 | 为什么不能动 |
|---|---|
| `unsp_glue.cpp:717-815` `wait_loop_collapse` **函数本体** | 它把 298 cycles/pass 的忙等折成一次；真机 `collapse=285,609` pass/s、`collcalls=19,154`/s，是活路径。**只该改调用点的预筛（R7-01），不该关它。** |
| `unsp_glue.cpp:1342-1432` `hle_push_pixels` + `Gpce4::lcd_push_pixels` → `St7735::write_run` | 一次调用吸收整段 span，把"每字节一次 IO 解码 + FIFO 记账"变成一次 `fb[]` 直写。真机 `pushpx=18,193/s`，是活路径。 |
| `unsp_glue.cpp:1516-1582` `hle_fill_span` + `lcd_fill_pixels` → `St7735::fill_run` | 同上；`fillpx=9,111/s`。 |
| `unsp_glue.cpp:1605-1642` `hle_mul32` | 2157 次/满屏重绘，把 45 % 的指令量压成一次调用。**它没有虚调用问题**（`d->read16/write16` 在 `≤0x0FFF` 上走 `m_ram` 内联快路径），别再"优化"它。 |
| `unsp_glue.cpp:1241-1256`、`:1781-1789` 的 2 KiB `fetch_window` 手法 | 把每像素一次 SparseRom 页查找换成一次范围比较；`page_window()` 返回 null 时还有 `read16` 兜底。 |
| `romstore.h:95-124` `read16` 的 overlay 范围测试 + `m_base` 直映射 | 注释 `:101-107` 记录的 0xFFFFFFFF 绕回 bug 是用血换的；overlay 必须排在 mmap 之前。 |
| `romstore.h:52-61` `page_window` / `romstore.cpp` 的 `read16_miss`/`slot_holding`/page cache | mmap 失败时是唯一退路（`rom_esp.cpp:139-145` 会走 `alloc_cache`）。**看起来是死代码，但它是兜底，删了换板子就起不来。** |
| `st7735.cpp:283-314/334-372` 的**窗口推进**逻辑（`m_posx/m_posy` 回绕、`m_commandstep += 2*count`、`m_pixellatch`） | 与 `data_w()` 逐字节路径的状态一致性全靠它，改它等于改模型语义。R7-05 只动同一循环里的 `m_dirty` 那一行，不碰窗口。 |
| `aot.h:352-368` 128 槽页缓存（1 KB DRAM） | 注释写明没有它是 17 fps vs 59 fps；`g_aot_pg_switch` 字段本身是死的（R7-10），但缓存不是。 |
| `gpce4.cpp` 的 `m_iob_out` 传 `spi2_txdata_w` 命令/数据判别、`m_spi2_tx_fill` 记账 | 是"字节流状态机正确"的一部分（`lcd_push_pixels` 里补 `0x8000` 也是同一类）。 |
| `compat/emu.h` 的 `logerror/LOGMASKED` 空宏 | 编译期删净，ISA 五个文件的日志成本 = 0。**唯一的风险是有人把它改成真 printf。** |

---

## 5. 待主审裁决项

**A. `-DC3_HLEPROF=1` 是否还要留着？**
代价 0.3 %/重绘帧、0.02 %/稳定帧；消费者只有 `>60 ms` 帧的 LONGF 行。
R6B 的结论已经把"host 时间去了哪里"答完了（flash 行 miss，不是钩子）。
→ 若没有正在进行的归属实验，建议置 0；若保留，请把 `unsp_glue.cpp:146-149` 的"~3 host cycles"改成"~10"。

**B. `hle_render_band` 的翻转预检能不能合并进主循环？**
技术上可证：预检范围 ≡ 主循环范围，记录表在 band 内不变，`line[]` 与记录表不重叠。
合并后可以顺带删掉 20 行死分支，但会改变"含翻转记录的行是整行 bail"这一语义
（变成"先写一部分，遇到翻转再 bail"）。**本审计员判断这在当前 ROM 上等价（写入幂等），但这是模型语义变更，需主审点头。**

**C. `execute_run` 的死 host 路径谁来清？**
`unsp_glue.cpp` 由 `tools/sync_c3_emu.py` 生成，手改会被覆盖。要清必须
(i) 改 `emu/unsp_glue.cpp` + 脚本，或 (ii) 加 `#ifndef ESP_PLATFORM`/`C3_HOSTTRACE` 把第二个 loop 与
`m_block_hist`/`m_insn_hook` 判空一起编掉。收益 ≈0.3 % + 100–300 B IRAM（推测），风险在同步链上。

**D. `gpce4.cpp:1048-1050` 的自赋值：** 注释要的是 `0x005240`（PORTA 轮询环），代码是原地不动。
是"漏改"还是"意图作废"？**这影响睡眠/唤醒行为，不只是冗余。**

**E. STAT 行要不要瘦身？**
现在一行里 `rdf/rds/wrf/wrs/caot/cinf/cfast/daot/dinf/cpa/cpi/clk/cfn/pd/pp/pgsw/rband/spin/isr7/collapse…`
约 18 个字段恒 0，且每秒要付 ~15 次 64 位除法（`uint64_t(d) * 100 / window`，RV32 上是 `__udivdi3`）。
一次性打印 ≈30 µs，对帧率无害，但**读日志的人会被骗**（见 R7-10）。

---

## 6. 量化方法与不确定度

- 每项都给了绝对 host cycles，百分比分母用 **7.51 M（重绘帧）** 与 **2.56 M（稳定态帧）**，因为两者差 3 倍，
  同一个绝对量在两处的百分比差 3 倍 —— 前几轮的口径混乱大多来自这一点。
- `6,788 dispatch/帧`、`11 cycles (W2)`、`22 cycles (W1)`、`3 cycles (W4)` 取自 `docs/notes/R6B_aot.md` 的**反汇编计数**，本轮用 ELF 复核了符号存在与尺寸，但没有重新逐条数指令。
- 钩子入口数（2,650/重绘帧）是"注释里的 2,157（mul）+ STAT 实测像素数 ÷ 每次调用的像素数"推算出来的，**不是实测**。
  要精确，需要一次 `C3_HLEPROF` 之外的分项入口计数（现在的 `g_h_*` 只有 cycles，没有次数）。
- `R7-06`（每像素重读全局）与 `R7-13`（64 位地址运算）明确标**推测**：靠的是"循环体内有虚调用 ⇒ 全局必须重载"这一编译器常识，未做反汇编确认。
- 真机数字全部来自 `dbgtest/r5_after_force.log`（1 s 窗口：`blitpx=18,391 pushpx=18,193 fillpx=9,111 bandc=510 bandb=425 bandr=209`）
  与 `dbgtest/audio/probe_auto1200.txt`（1200 s IO 写普查）。**没有在这台机器上跑任何构建或刷机。**
