# unSP 2.0 CPU 与代码生成调研（GPCE4 / ESP32-C3）

证据分级，全文一律标注：

- **【手册】** 官方 *unSP Programmer's Guide*（`ref/unsp2.0.pdf`，ch.6 与附录 A–D）。周期表在 PDF 里是**图片**，`ref/unsp2.0.txt` 的文本抽取丢表，本文数字来自页面图像 `ref/p143_1.png`…`ref/p147_1.png` 及 `ref/unsp2.0.txt:4342-4914`。
- **【代码】** MAME 0.289 源码 `ref/unsp/*`（本文引用行号即该副本）。
- **【实测】** 本项目设备/主机测量：`docs/PERF_2026-11-22.md`、`riscv32-esp-elf-size` 输出。
- **【推测】** 我基于上述三类证据的推算，未直接测量。

---

## 1. unSP 2.0 指令与周期速查表

### 1.1 寄存器与状态【手册 ch.2】

| 项 | 规格 |
|---|---|
| 通用寄存器 | R1–R4，16 bit；SR1–SR4 为第二组影子寄存器（`SECBANK` / `FR.BNK` 切换） |
| 扩展寄存器 | **R8–R15，仅 unSP 2.0**；访存/ALU 需 `EXTOP`(0xff80)+2 字组，即 3 个 16 位字 |
| 系统寄存器 | SP、BP、SR、PC、FR；MR = R4:R3（32 bit 乘法/乘加结果对） |
| 地址空间 | 4 MWord（22 bit），**无字节粒度**；CS[5:0] 选指令页、DS[5:0] 选数据页，每页 64 KWord |
| 流水线 | 4 级：Fetch / Decode(发地址) / Memory-Read(移位在此级) / Execution+Writeback(ALU+MAC) |
| 平均 CPI | **2.0 = 2**；1.0 = 6，1.1/1.2/1.3 = 5【手册 附录C】 |
| 乘法单元 | 36 bit 累加器（MULS 内积用），1 周期（2.0） |

SR 位域：`DS[15:10] N[9] Z[8] S[7] C[6] CS[5:0]`。
FR 位域：`AQ[14] BNK[13] FRA[12] FIR[11] SB[10:7] FIQ[6] IRQ[5] INE[4] PRI[3:0]`，复位值 `PRI=1000`（IRQ0–7 全开）、`FIR=1`【代码 unsp.cpp get_fr/set_fr，与手册一致】。

**标志位实际语义（文档缺失，MAME 是事实标准）**【代码 unsp.cpp `update_nzsc`】：

```
N = bit15(value)
Z = (uint16)value == 0
C = bit16(value)
S = bit16(value) != bit15(r0 ^ r1)     // 不是简单的 bit16！
```

`S` 依赖**两个源操作数**，这条决定了第 3 节的 lazy-flags 不能只缓存结果值。MooglyGuy 的 RE 笔记也明确说官方文档“omit how individual opcodes behave, including how C and S are calculated”。

### 1.2 编码格式【手册 ch.2.2】

5 种机器码格式，字段为 `OP / OPD / OPDE / COND / FL(D,@,S,W,SFT,F,I) / RG(SIZE,nn)`；一条指令是 1 或 2 个字组，2.0 扩展指令是 3 个（`EXTOP` + 字组1 + 字组2）。
解码分组（本项目已验证）【代码 unsp.cpp】：
`op0[15:12]` 主组；`opa==7 && op1<2` → 跳转组；`op0==0x0f` → f 组；`op0==0x0e` → e 组；其余 → ALU/访存组。`0x9a98` = `reti`，`pop` 覆盖 r7 等价于 `ret`。

### 1.3 周期速查表（核心指令）

“手册核心”= ch.6.1 表格值，**不含**取指/访存等待；“手册含等待”= 附录 D 公式在 PM=DM=1 时的示例值（该值才是等效真实周期）。MAME 列是 `m_icount` 实扣值。

| 指令形态 | 手册核心 | 手册含等待 | MAME 实扣 | 备注 |
|---|---|---|---|---|
| `Rd op IM6`（ADD/ADC/SUB/SBC/CMP/AND/OR/XOR/TEST） | 1 | 2 | 2 | ALU 主模板 |
| `Rd op IM16`（2 字） | 2 | 4 | 4 | |
| `Rd op Rs` | 1 | 2 | **3**（opa==7 时 5） | MAME 按 1.2 的 3 周期计 |
| `Rd op [A6]`（page0 直址） | 1 | 3 | **5** | |
| `Rd op [A16]`（2 字） | 2 | 5 | **7** | |
| `Rd op [BP+IM6]` | 1/2 | 3 | **6** | 手册注：STORE 只需 1 周期 |
| `Rd op {D:}[Rs@]` | 2/3 | 4（`++` 5） | **6**（`++` 7） | 3 = 带 `++` 前缀字 |
| `Rd = IM6` / `= IM16` / `= [A6]` / `= [A16]` | 1 / 2 / 1 / 2 | 2 / 4 / 3 / 4 | AOT 内联 | LOAD 不影响任何标志 |
| `[A6] = Rd` / `[A16] = Rd` | 1 / 2 | 3 / 4 | AOT 内联 | STORE 不影响标志 |
| `PUSH n` / `POP n` | n+1 / n+2 | 1+N+N·DM+PM / 2+N+N·DM+PM | **4+2n** | |
| **`MR = Rd*Rs`（MUL ss/us/uu）** | **1** | **2** | **12** | 冲突最大的一条 |
| `MR=[Rd]*[Rs],N`（MULS 内积） | 见下注 | 13 / 22 / 23 / 30 | 12（固定） | |
| `DIVS` / `DIVQ` / `EXP` | 1 | 2 | DIVQ 15 步循环，DIVS 未实现 | |
| 16 位移位（`asr/lsl/lsr/asror/lslor/lsror`） | 1 | 2 | e 组按模板 | 2.0 用 log shifter |
| BITOP 寄存器 / `[Rd]` / `[A16]` | 1 / 1 / 2 | 2 / 3 / 5 | e 组按模板 | Z 恒取自**原始位** |
| `CALL` / `RETF` / `RETI` / `GOTO` | — | 7 / 5 / 5（嵌套 7）/ 5 | 9 / 4 级 / 8 / 5 | |
| `Jcc` 未取/取 | — | 2 / 6 | **2 / 4** | |
| `NOP`、`INT/FIRQ/SECBANK/FRACTION/FIR_MOV/IRQNEST` 模式切换 | — | 2 | 2 | 模式切换本身也计时 |
| **R8–R15 扩展组（全部）** | — | 4–15 | **0** | MAME 缺陷，见 1.6 |

MULS 周期注【手册 p147 脚注】：`N+2`（DM/IM 无冲突、FIR_MOV=Off）/ `2N+1`（无冲突、On）/ `2N+2`（有冲突、Off）/ `3N`（有冲突、On），`N=0..15`，`N=0` 即 16 级内积。

### 1.4 关键语义细节

- **MUL**（`MR = Rd*Rs`，`Ss` 字段选 ss/us/uu）：16×16→32，结果 `R4=高16, R3=低16`；**不改任何标志**；2.0 里 1 周期。MAME 的符号修正实现（与手册一致）：`lres=a*b; if(b&0x8000) lres-=a<<16; if(a&0x8000) lres-=b<<16;`【代码 unsp_fxxx.cpp:66-100】。
- **MULS**（16 级内积，36 bit 累加）：`MR=[Rd]*[Rs],N` 读 `N+1` 个连续字，累加进 36 bit 累加器，`SB[10:7]` 保留保护位；`FIR_MOV=On` 时自动把 `[Rd]` 数组按 FIR 语义前移并回写。**MAME 把 `m_sb` 直接置 0（手册要求放入 36 bit ALU 的保护位），并且忽略 `FRA`（手册规定 FRA=1 时每个乘积先左移 1 位再累加）**——这是 MAME 的已知简化，本项目 762 块内未出现 MULS，暂时不影响【代码 unsp_fxxx.cpp execute_muls_* / 实测 grep aot_blocks.c 无 MULS】。
- **DIVQ**：15 步恢复除法，`AQ` 位取累加器 bit31，结果在 R3；`DIVS` 在 MAME 里仍是 `unimplemented`。
- **移位**：2.0 有 16 bit 桶形移位器，`SFT` 字段或 `Rs & 0x1f` 给位数；`asror/lslor` 借用 R3|R4 的进位位；MAME 的 `rol/ror` 未实现。
- **BITOP**：`tstb/setb/clrb/invb`；`D` 位选择 `ds:` 前缀；**无论哪种操作，Z 都由操作前的原始位决定**（bit=0 → Z=1）【代码 unsp_fxxx.cpp 0xf240 / unsp_exxx.cpp】。
- **Banked register**：`SECBANK` 交换 R1–R4 与 SR1–SR4；`FR.BNK` 同效。切换后紧跟 2 条使用 R1–R4 的指令有 bank-change stall（见 1.5）。
- **中断入口**【代码 unsp.cpp:461-516】：FIQ 压 PC、SR，`PC=read16(vec+0x06)`；IRQ 压 PC、SR（`INE=1` 时再压 FR），`PC=read16(vec+0x08+line)`，SR 清 0。

### 1.5 流水线停顿（12 条）【手册 ch.6.3，`ref/unsp2.0.txt:4145-4272`】

DAG 读写相关 / 跳转后立即读内存 / IAG 输入寄存器未就绪（`R3=0x1234; GOTO MR`）/ DAG 输入未就绪 / MAC 前递 / **bank 切换后 2 条指令** / 移位器源未就绪（`SFT_CNT`、ROL/ROR 进位）/ 延迟计算寄存器未就绪（`[++Rs]`、POP、`BP+IM6`）/ FIR_MOV 下 MAC 写停顿 / 数据与指令总线未就绪 / 总线争用。每条约 +1 周期。

### 1.6 unSP 2.0 与 1.0/1.1/1.2/1.3 的差异【手册 附录 A/C】

| | 1.0 | 1.1 | 1.2 | 1.3 | 2.0 |
|---|---|---|---|---|---|
| 平均 CPI | 6 | 5 | 5 | 5 | **2** |
| MAC 周期 | 13 | 12 | 12/13 | 12/13 | **1** |
| 第二寄存器组 / 嵌套 IRQ / FR / DS 访问 / EXP / 位操作 / 16 位移位 | 无 | 无 | 有 | 有 | 有 |
| SS/MDS 段寄存器、`DIVUU`/`DIVSS` 单指令除法、字节间接寻址 | — | — | — | **独有** | 2.0 取消 |
| R8–R15、扩展指令、分离指令/数据总线、4 级流水、log shifter | — | — | — | — | **独有** |

**GPCE4 用的是 unSP 2.0**【代码 `ref/generalplus_gpce4_soc.cpp:27`：`generalplus_gpce4_soc_device : unsp_20_device(...)`】。但驱动注释自相矛盾：`ref/generalplus_gpce4.cpp:370` 写 “internal ROM uses unsp2.0 opcodes, should be 48MHz (but unSP2.0 opcode take fewer cycles?)”，`ref/generalplus_gpce4.cpp:18` 推荐的却是 `unidasm -arch unsp12`。**实测上本作没有用到 R8–R15 扩展组与 MULS**（`aot_blocks.c` 中 ext 组、MULS 均 0 命中），所以 1.2 与 2.0 的差异对本作不构成行为差异，只影响周期模型。

### 1.7 MAME `unsp.cpp` 的实现质量

**会影响正确性的（4 条）**

1. **扩展组（R8–R15）完全不计周期**：`unsp_extended.cpp` 里没有任何 `m_icount -=`，而它要取 2–3 个指令字。用 R8–R15 的程序在 MAME 里会跑得明显偏快。**本作不受影响**。
2. **MUL/MULS 计 12 周期，手册 2.0 是 1 周期**。MAME 用的是 1.2 时代的 12/13 周期模型。驱动把时钟设成 96 MHz（≈ 48 MHz × 2）来补偿，因此**指令级时间分布仍然是错的**（乘法被高估 12 倍，其它指令相对被低估），只在帧粒度上近似守恒。
3. **MULS 的 `us/uu/su` 三种变体与 `DIVS` 未实现**（`logerror` + `unimplemented_opcode` → fatalerror）。
4. **中断采样点与手册不同**：MAME 在**每条指令之后**调 `check_irqs()`（`reti` 除外）；手册说 2.0 在**每条指令的 decode 阶段**采样（`RA16/RW16/RETI` 除外）。本项目进一步粗化到**基本块边界**，属于第二层偏差。

**已知简化但不影响本作**：`m_sb` 恒 0、忽略 `FRA`、`rol/ror` 未实现、`execute_min_cycles()==max_cycles()==5`（CPU 按 5 周期量子调度，本身就很粗）。

**没有 JIT**：`unspdrc.cpp` 是一套完整的 UML DRC（`code_compile_block` / `static_generate_entry_point` / `invalidate_cache`），但被 `ENABLE_UNSP_DRC (0)` 编译掉了，`m_enable_drc` 恒 false，**从未启用、无性能数据**。

> 未查证：MAME GitHub 上的 unsp 相关 issue/commit 历史（本环境 GitHub API 被限流，只有原始文件可抓）。

---

## 2. 静态重编译的成熟模式

### 2.1 粒度：函数级 vs 基本块级

| | N64Recomp（N64） | 本项目（unSP/C3） |
|---|---|---|
| 粒度 | **每个 guest 函数一个 C 函数** | **每个基本块一个 C 函数**（762 个） |
| 调用 | `jal` → 直接 C 调用 | `call` → 压 PC 到 guest 栈，写 `c->pc`，返回调度器 |
| 跳转 | `j`/`b` 识别成尾调用 → C 调用 | 条件分支 → `if(cond){pc=T;return;} pc=F; return;` |
| 返回 | C `return` | 回到调度器（lambda ∘ 不递归） |
| 寄存器 | `ctx->r4` 结构体成员，靠编译器缓存 | `AOT_LOAD_REGS/SAVE_REGS` 每块进出各 9 次访存 |
| 延时槽 | 复制指令填充 | unSP 无延时槽 |

N64Recomp 的取舍在台式机成立（32 MB+ cache、C 调用 ~2 周期）；在 MCU 上**块级 + 返回调度器**是对的：没有 C 栈深度问题、每块都是中断检查点、单块代码可被链接器单独搬到 IRAM。代价是每块一次 `call/ret` + 一次表查找（实测 689 周期/次调度里 68 周期 = 9.9% 花在表和上下文上）。

**能在桌面/大核上抄的三条**：
1. `jr` 的 switch-case 判定（跳转表识别）；
2. 单文件输出模式 + 链接器符号覆盖，用来只重编译改动的函数（本项目的 HLE 打补丁可以照此做：HLE 版块放静态库、优先于生成代码参与链接）；
3. 用 ELF/符号元数据驱动生成，而不是硬编码 PC 列表。

### 2.2 Guest 寄存器 → 宿主寄存器

三种成熟方案，按“寄存器压力”排序：

| 方案 | 做法 | 适用 |
|---|---|---|
| A. 内存上下文（N64Recomp） | 全部在 `ctx->rX`，靠编译器局部提升 | 寄存器多、cache 大的宿主 |
| B. 块内局部 + 块尾写回（**本项目现状**） | 每块 `LOAD_REGS` 拷到局部，块尾 `SAVE_REGS` 写回 | 通用、实现简单；每块 18 次访存 |
| C. 全局钉住 + 单 trampoline | r1–r4/SP/BP/SR 固定占用 s0–s6（callee-saved），**块之间不返回 C**，用 computed goto 直接跳下一块 | RV32 只有 16 个寄存器，最省 |

RV32 里可用 callee-saved `s0–s11`（12 个）。unSP 热寄存器是 R1–R4 + SP + BP + SR = 7 个，钉住完全可行，代价是必须放弃“块 = 独立 C 函数”的 ABI（改成 `static void b_xxx(void)` + `goto` 链，或用 tail-call 属性）。方案 C 能直接消掉每块 18 次访存 + 一次 call/ret。

### 2.3 标志位惰性求值（lazy flags）

业界标准做法（QEMU 的 `cc_op`/`CC_SRC` 机制、Dolphin Jit64 的 `FinalizeCarry`）：**不在 ALU 指令处算标志，而是记录“上一次设置标志的操作类型 + 操作数”，等到真正读标志的指令（Jcc/CMP/ADC/SBB）再求值**。

QEMU 的做法是 `cc_op` 枚举 + `cc_src/cc_src2/cc_dst` 三元组，`gen_compute_eflags` 在需要时按 `cc_op` 反推；Dolphin 对 PowerPC 的 XER.CA 用 `FinalizeCarry` 把进位延迟到分支前，并提供 `locked` 标志禁止跨基本块移动。

unSP 的特殊约束：`S = bit16(v) != bit15(r0^r1)` 需要**两个源操作数**，所以缓存不能只存结果：

```c
// 惰性标志的最小状态
enum { F_NONE, F_ADD, F_SUB, F_LOGIC } pend;
uint32_t pend_v, pend_x;   // v = 结果, x = r0^r1（仅 ADD/SUB 需要）

// ALU 指令处：只算值
static inline void set_add(uint16_t a, uint16_t b) {
    uint32_t v = (uint32_t)a + b;
    r[d] = (uint16_t)v;  pend_v = v; pend_x = a ^ b; pend = F_ADD;
}
// 只有在读标志时才物化
static inline void flush_flags(void) {
    if (pend == F_NONE) return;
    if (pend == F_ADD) {
        uint32_t nz = pend_v & 0x8000u ? F_N : 0;
        if ((uint16_t)pend_v == 0) nz |= F_Z;
        nz |= (pend_v >> 16 & 1) ? F_C : 0;
        if (((pend_v >> 16) & 1) != ((pend_x >> 15) & 1)) nz |= F_S;
        sr = (sr & ~0x03c0u) | nz;
    }
    pend = F_NONE;
}
```

收益估算：现在每条带标志的 ALU 指令都要跑 `aot_nzsc_sr`（约 6–8 条 RV32）。ADC/SBC 链里只有最后一条的标志有用，中间全是浪费。

### 2.4 内存访问内联

成熟做法是把 guest 内存当成宿主可直接寻址的一维数组，**只有跨页/跨段时才走慢路径**：

```c
// 快路径：地址落在 RAM 镜像内
static inline uint16_t rd(uint32_t a) {
    return (a <= RAM_LIMIT) ? ram[a] : bus_rd(dev, a);   // 分支可预测
}
```

本项目已如此；进一步可以用“页基址表 + 无分支”形式（把 `ram` 指针按 DS 页预计算），把每条访存的 3–4 条 RV32（比较+分支+索引）压到 1–2 条。N64Recomp 直接 `rdram[addr]`，靠 MMU 页表在宿主侧处理，思路一致。

### 2.5 间接后继（本项目几乎不需要）

| 场景 | 成熟方案 |
|---|---|
| 函数返回 | 返回地址栈（RAS）预测；静态重编译里更简单——**返回点集合是静态可知的**，可生成 `ret_target` 位图或小 switch |
| 跳转表 | 识别 `jmp [Rn]` + 前置 `cmp/`表加载 → 生成 C `switch`（N64Recomp 的 `jr` 处理） |
| 虚调用/覆盖层 | 按地址查函数指针表（`LOOKUP_FUNC(addr)`） |
| 动态代码 | inline cache：目标地址 → 已编译块指针的哈希缓存（本项目已用 128 项页缓存做静态版） |

本作**无间接跳转、无自修改代码**（`tools/coverage/unsp_static.py` 的静态扫块已确认），所以唯一要处理的是 `ret/retf/reti`。因为调用图静态，可以为每个块预计算“可能返回到哪些块”的小集合。

### 2.6 静态重编译 vs 模板 JIT（低端 MCU）

| | 静态重编译（本项目） | 模板 JIT |
|---|---|---|
| 代码体积 | 全部 656 KB，只能靠链接顺序/IRAM 分区控制 | 只编热路径，典型 32–128 KB |
| i-cache | 冷块也占 i-cache 空间 | 热代码集中，命中率高 |
| 启动时间 | 0（XIP 直接跑） | 需要预热编译（C3 上写 flash 极慢，只能进 RAM） |
| RAM | 0（代码在 flash） | 需要块缓存（RAM 紧张）+ W^X 问题（C3 无 MMU，但 RAM 可执行） |
| 确定性 | 完全静态，可做指令级 trace 对齐 | 动态，trace 对齐困难 |
| 调试 | 生成代码可读、可下断点 | 难 |

MCU 上结论：**静态重编译优先**。JIT 唯一优势（体积）在本项目已经被“762 块全量 + 无间接跳转”这条约束抵掉——真的要做体积优化，用“静态裁剪 + 按需从 flash 加载到 RAM 的分区”比 JIT 简单得多。

### 2.7 “每 guest 指令宿主周期”的公开实测数据

老实说：**没有找到带方法学的公开数字**。N64Recomp/VSEMUR/MooglyGuy 三处都没有性能测量，MAME 的 unsp DRC 从未启用。可用的只有本项目自己的两组实测：

| 指标 | 值 | 来源 |
|---|---|---|
| 端到端（含渲染/总线/中断） | **110 宿主周期/guest 指令** | 【实测】154,451 insn/帧、9 fps、92% guest 占用、160 MHz |
| 纯 AOT 路径（`C3_SPLIT`） | **63 宿主周期/guest 指令** | 【实测】532 周期块体 / 11.0 指令 |
| 每次调度开销 | 689 周期（表+上下文 68 = 9.9%） | 【实测】 |
| 生成码密度 | **95.7 B RV32 / guest 指令 ≈ 24 条 RV32** | 【实测】656,880 B / 6,867 指令 |
| 反推 IPC | 63 / 24 ≈ 2.6 周期/RV32 指令（IPC≈0.38） | 【推测】按上述两行相除 |

**IPC 0.38 对 C3 偏低**（4 级流水、无分支预测、flash XIP）。这部分损失来自 i-cache miss 与访存相关。注意本项目已有反证：`-Os` 让生成码少 33.6%，fps 只 +4.8% ⇒ **代码体积不是主要瓶颈，指令条数才是**。这条实测结论直接决定了第 3 节的优化顺序。

---

## 3. 本项目 762 块的落地方案

### 3.1 现状量化

| 项 | 值 | 来源 |
|---|---|---|
| 基本块 / guest 指令 | 762 块 / 6,867 条（均值 9.0 条/块） | 【实测】aot_blocks.c 头注释 |
| 生成 C 源码 | 1.69 MB / 73,461 行 | 【实测】 |
| 编译后 `.text.*` | 656,880 B，最大单块 2,244 B | 【实测】`aot_blocks.c.obj` |
| 固件总计 | `.flash.text` 822,986 B、`.iram0.text` 99,568 B | 【实测】`digicore-c3.elf` |
| 每帧 guest 指令 / 块调度 | 154,451 / 12,744 | 【实测】 |
| 目标 | 20–35 周期/指令 | 用户目标 |

**换算**：要 30 fps 需 34.5 周期/指令，60 fps 需 17。当前 110。若 IPC 不变，34.5 周期 ⇒ 需要 **≤ 13 条 RV32/guest 指令**（现在是 24）。所以“把 24 压到 12–13”就是全部工程目标。

### 3.2 24 条 RV32 花在哪里（逐项可优化量）

| 开销源 | 估计 RV32 条数/guest 指令 | 依据 |
|---|---|---|
| `AOT_LOAD_REGS` + `AOT_SAVE_REGS` | ~1.6 | 18 次访存 ÷ 11 指令/块 |
| 立即数标志物化 `aot_nzsc_sr` / `aot_nz_sr` | ~4 | 6–8 条 × 约 60% 指令带标志 |
| `aot_rd/aot_wr` 边界检查 | ~1.3 | 3–4 条 × 约 35% 访存 |
| 16 bit 截断 `& 0xffff` | ~1 | 每次写寄存器 |
| `aot_op_sr` 返回 `AOT_NO_WRITE` 哨兵 + 分支 | ~1.5 | CMP/TEST 路径 |
| 块尾 pc 赋值 + `return` + 调度 | ~2 | 689 周期/块 ÷ 11 |
| **真正的语义运算** | ~12 | 余量 |

⇒ **可砍掉约 9–11 条**，落到 13–15 条，对应 **35–40 周期/指令**（【推测】，按 IPC 不变）。再叠加 HLE 才有可能进 20–25。

### 3.3 分阶段计划

| 阶段 | 内容 | 预期（【推测】） | 验证 |
|---|---|---|---|
| **P0 基线冻结** | 已有：`emu_validate.py` 100%、`cyc_diff.py` 0 diff、`fb_f60.bin` 帧哈希、`--save-flash` 0 字节差 | — | 每阶段必须全绿才能进下一阶段 |
| **P1 寄存器钉住 + computed goto** | 去掉 `AOT_LOAD_REGS/SAVE_REGS`；r1–r4/SP/BP/SR 钉到 `s0–s6`；块尾 `goto *tab[page(...)]` 而非 `return`+调度 | 63 → **52–56** | 同上；另加“每块 RV32 指令条数”统计 |
| **P2 lazy flags** | 按 2.3 的方案，只缓存 `(v, r0^r1)`，Jcc/CMP 处 `flush_flags()` | 再 −15~20% ⇒ **44–48** | 重点测 `S` 标志：构造 `0x7fff+1`、`0x8000-1` 等边界，逐条比对 `cyc900`/trace |
| **P3 访存快路径** | 按 DS 页预计算 `ram` 基址；`[A6]`/`[BP+IM6]` 走无分支路径；仅跨页走 `bus_rd/wr` | 再 −5~8% ⇒ **41–45** | 保存/闪现 diff 必须仍为 0 |
| **P4 空间局部性** | 按每帧执行序重排 762 块（同一条 UI 路径的块相邻）；热块 IRAM 分区（不是 14 个，而是按 trace 覆盖前 100 KB） | ±5–15%，**方向不确定**（`-Os` 实验说明体积不是主因） | A/B 实测 fps |
| **P5 HLE** | 见第 4 节 | 视区域而定 | 差分测试 |

**注意 P4 的优先级**：本项目已经试过“14 个热块进 IRAM → 无变化”和“`-Os` → −33.6% 代码只 +4.8% fps”，两条反证都指向“i-cache 不是主要瓶颈”。P4 应该只做“按执行序重排”，不要投入 IRAM 搬迁。

### 3.4 全程序重编译成单文件是否可行

**可行，而且已经在做**，但要区分两件事：

- **源码层单文件**：1.69 MB / 73,461 行，gcc 编译约 30–60 s，无问题。真正的问题是编译期优化边界——单文件让 `static` 函数无法被丢弃，但换来跨块内联的可能性。
- **机器码体积**：656 KB 已超 C3 的 i-cache 一个数量级以上（C3 的 L1 指令缓存为十几 KB 量级，见 [ESP-IDF 存储器类型](https://docs.espressif.com/projects/esp-idf/zh_CN/v4.4.3/esp32c3/api-guides/memory-types.html#iram)），所以**恒定的 i-cache miss 一定存在**，只能靠“热块集中”降低，不能消除。C3 有 400 KB SRAM，当前 `.iram0.text` 只用了 99.5 KB，**还有约 200 KB 余量**可以放热块——但 P4 的实测结论说收益有限。

结论：单文件重编译可行；**i-cache 不是本项目的主要矛盾，不要为它做结构性妥协**（比如拆成多个编译单元、按热度分片加载），那些复杂度换不来 fps。

### 3.5 绘制区 0x207900–0x207D40 整体 HLE 的可行性

已知：724 条 guest 指令，占每帧指令 46.2%、占 host 周期 17.3%。

**需要逆向的输入**（按依赖顺序）：
1. 绘制入口的**参数约定**：哪些寄存器/栈槽传 sprite 表指针、调色板指针、目标缓冲基址；
2. **sprite 表格式**：每条目字段（X/Y/尺寸/翻转/颜色索引/下一帧指针），是否 RLE/位平面；
3. **调色板格式**：ST7735 LCDC 的 RGB565 转换在哪一步做（guest 做还是 SoC 做）；
4. **裁剪与透明色**规则，以及是否有 Z 序/覆盖顺序副作用；
5. 绘制区是否**回读**自己写的内容（决定 HLE 是否需要写回 guest RAM）。

**输出对齐方法**：不要“重写渲染”，先做**影子实现**——guest 照原样跑，HLE 函数同时算一份，逐像素比对；完全一致后再摘掉 guest 路径。

**回归验证**：
- 帧缓冲字节级比对（现成基线 `dbgtest/fb_f60.bin`，PC 端 `--frames 60 --shot-raw`）；
- 600 帧连续比对 + 随机输入驱动（用 `tools/coverage` 的输入注入）；
- 指令级 trace 只能放宽（HLE 后指令流必然不同），改为比 **guest RAM/显存快照 + 帧缓冲哈希**。

**收益判断（重要）**：17.3% 的 host 周期 ⇒ 理论上限约 **1.2×**（9 → 10.8 fps，或 30 fps 时省 17%）。但 HLE 这条路径要逆向整套渲染，风险高、回归面大。**建议排在 P1–P3 之后**，且只做“影子实现 + 逐像素比对”这一步先拿到数据。

### 3.6 内部 ROM 0x005F8B 定点乘加库 HLE 的语义重建要点

已从 `emu/aot_blocks.c:16887-17047` 完整解码（17 条指令）：`push`（BP/SR/PC）→ `BP = SP+1` → 从 `[BP+3]..[BP+6]` 读 4 个 16 bit 参数 → **3 次 16×16→32 乘法**（第一次 `MUL uu`，后两次 `MUL ss`，带 `if (b&0x8000) lres -= a<<16` 式符号修正）→ 结果按 32 bit 高低半字分别累加（**半字之间不传递进位**）→ `pop BP,SR,PC` 返回。AOT 侧计费合计 **108 MAME 周期**（源码注释写“36 cycles worst case”，与实际计费不符）。0x005F8E 是去掉序言的同体（15 条）。

HLE 实现要点：
1. 32×32→64 不需要：guest 只有 16×16→32，产物是 `R4:R3`。**别用 int64 图省事**，会改变溢出语义；
2. **符号修正公式**必须逐字照抄 `lres = a*b; if (b&0x8000) lres -= a<<16; if (a&0x8000) lres -= b<<16;`（这是 ss 乘法的 32 bit 截断结果）；
3. 两个半字的累加**不传播进位**（`r2 += r4` 与 `r3 += r3'` 各自 16 bit 回绕），这是 16 bit ALU 的直译结果；
4. 舍入/饱和：本块内**没有**饱和与舍入（是纯整数乘加），不要凭空加 clamp；
5. 标志位：块内 `ADD` 会改 N/Z/C/S，`pop ... SR` 会恢复 SR ⇒ 出口标志由 pop 决定。HLE 后必须保证 SR 恢复正确（这是最容易错的一点）。

**验证**：差分测试 harness——同一组 4 个参数，分别跑 AOT 原块与 HLE，比较 R1–R7/SP/SR/内存写集合；参数空间取 `{0, 1, 0x7fff, 0x8000, 0xffff, 随机}` 的笛卡尔积 + 200k 随机（PC 模拟器上毫秒级），要求 100% 一致。

**但要先核实一件事**：任务书说该库“占 40% 指令 / 极少时间”，而按 110 周期/指令的平均值推算它会占掉大头。两份统计口径（指令数 vs host 周期）在 `docs/PERF` 里没有直接对齐。**动手前先用 `dbgtest/cyc900.txt`（249 MB 的 `PC CHARGE` 三元组）重算“每区域 host 周期/指令”**，否则 HLE 优先级可能排反。

### 3.7 整条渲染管线 host 化

可行性：**技术上可行，工程上不划算**，除非目标 fps 高于 30。理由：绘制区只占 17.3% host 周期，而“其它 AOT 51% + 解释器 15%”才是大头。

方法论（如果要走）：
1. 在 PC 模拟器上加**写钩子**，记录所有写进显存区间（`0x200000` 段内帧缓冲）的 PC，聚类成函数边界；
2. 用 `tools/coverage/unsp_static.py` 的静态调用图交叉验证，列出所有“被绘制路径调用”的函数；
3. 按“入口参数 → 输出像素”逐函数 HLE，每个都做影子比对；
4. 分阶段替换：先 sprite 合成，再文字（字模位图），最后调色板转换；
5. 全部替换后，guest 侧只保留“写命令 + 等 vblank”。

需要逆向的函数清单（待第 1 步产出）：绘制入口、sprite 表遍历、行拷贝、透明色判定、文字渲染、调色板转换、裁剪、vblank 同步。

---

## 4. HLE 清单与优先级

| 优先级 | 目标 | 现状占比 | 输入/输出 | 语义风险 | 验证 | 预期收益（【推测】） |
|---|---|---|---|---|---|---|
| **已完成** | push 循环 `0x005B4A` | 1.1% 周期 | — | 低 | 已完成 | **+12.6% fps（实测）** |
| **已完成** | 忙等 `0x230321/0x5F35/0x5251/0x5376` | 5.5% 指令 | — | 低 | 已完成 | 7.69 → 14.15 fps（实测） |
| **已完成** | PRNG 块（68 条） | — | — | 中 | 已完成 | 1.09 → 7.82 fps（实测） |
| **P1** | 定点乘加库 `0x005F8B/0x005F8E` | 39.8% 指令 | `[BP+3..6]` → R3/R4 + SR | **中高**（ss 符号修正、半字不回绕、SR 恢复） | 差分 200k 组 | 若真是“40% 指令”，fps 提升显著；先按 3.6 口径复核 |
| **P2** | 绘制区 `0x207900–0x207D40` | 46.2% 指令 / 17.3% 周期 | sprite 表/调色板/帧缓冲指针 → 像素 | **高**（逐像素对齐） | 影子实现 + 600 帧比对 | ≤1.2× |
| **P3** | 整条渲染管线 | 同上 | 命令流 | 很高 | 分阶段影子比对 | 同 P2 上限 |
| **不建议** | 逐块手工 HLE 剩余杂项 | <10% | — | 低但琐碎 | — | <1.05× |

**排序理由**：指令数占比大 ≠ fps 收益大。已经验证过的三个 HLE 里，最大单笔收益来自“把 5.5% 的忙等折叠掉”（因为它是纯空转，等价于无限快）；而 push 循环占 1.1% 周期只换来 12.6%（因为它是每帧上千次调用的短函数，HLE 后省掉的是调度开销）。**判断某个 HLE 值不值得，要看“每次调用的固定开销 × 调用次数”，不是看指令占比。**

---

## 5. 风险与未解决问题

1. **周期模型是 1.2 的，不是 2.0 的**（MUL 12 vs 1、ALU 3 vs 1、PUSH 4+2n vs n+1）。当前靠 96 MHz 时钟补偿，帧粒度正确、指令粒度错误。改动周期表会让“游戏速度正确”这个来之不易的现状失效——**不要动**，只在文档里标注。
2. **`S` 标志的语义是 MAME 猜的**（官方文档缺失）。lazy flags 必须缓存 `r0^r1`，任何“标准”lazy flag 实现（只缓存结果）都会算错 S。
3. **中断采样点三层不一致**：手册（decode 阶段）↔ MAME（每指令后）↔ 本项目（块边界）。目前靠 trace 对齐“看起来一样”，但 IRQ 到达时刻的相位差没有被系统性验证过。建议做一次“同一帧内 IRQ 响应 PC 序列”比对。
4. **HLE 优先级可能排反**：`docs/PERF` 的“指令数占比”与“host 周期占比”两份统计没有对齐，MAC 库到底是 40% 时间还是“极少时间”必须先复核（`dbgtest/cyc900.txt` 现成）。
5. **没有 unSP 汇编器**：所有 HLE 补丁只能手搓 16 bit 字再反汇编回验，出错概率高，必须建立“打补丁 → 反汇编 → 逐字比对”的固定流程。
6. **MAME 的扩展组 0 周期缺陷**：本作不受影响，但一旦换游戏（尤其 V.Smile/JAKKS 系）就会踩，AOT 生成器应显式检测 EXTOP 并报错而不是静默。
7. **`-Os` 实验的反证**：代码体积 −33.6% 只换来 +4.8% fps，说明 i-cache 不是主因。任何“靠缩小代码换性能”的方案都要先过 A/B 实测。
8. **公开参考实现稀缺**：MAME（唯一完整实现，DRC 未启用）、VSEMUR（Rust 解释器，unSP 1.2）、MooglyGuy/unsp（纯 RE 笔记，无模拟器）。**没有任何公开的 unSP JIT/静态重编译**，本项目在这条路上没有前人数据可对标。
9. **未查证项**：MAME GitHub 的 unsp issue/commit 历史（API 限流）；ESP32-C3 i-cache 精确容量（文档页未抓到数字，只按“十几 KB 量级”使用）；unSP 2.0 移位指令的标志位影响（ch.6.1 的移位表未逐页核对）。

---

## 附：参考实现（D 节）

| 项目 | 语言/形态 | 覆盖版本 | 性能数据 | 结构要点 |
|---|---|---|---|---|
| MAME `src/devices/cpu/unsp` | C++ 解释器（+未启用的 UML DRC） | 1.0/1.1/1.2/2.0 | 无（DRC 从未启用） | 按 op0 分组解码；1 文件/指令族；周期硬编码 |
| MAME `unspdrc.cpp` | UML DRC | 同上 | 无 | `code_compile_block` + 入口点/无码处理器；`ENABLE_UNSP_DRC 0` |
| VSEMUR（[JZJisawesome/VSEMUR](https://github.com/JZJisawesome/VSEMUR)） | Rust 解释器 | unSP 1.2（V.Smile） | 无 | 作者重写了指令解码流程，记录了文档歧义与 MAME 的取舍优先级 |
| MooglyGuy/unsp（[repo](https://github.com/MooglyGuy/unsp)） | 纯 RE 笔记 | 1.0–2.0 | — | 寄存器/向量/SPG24x-28x 外设布局；明确指出文档不讲 C/S 语义 |
| 本项目 AOT | 静态重编译 → C → RV32 | 2.0（子集） | 110 / 63 周期每指令 | 块级函数 + 两级页表调度 |

**没有找到** unSP 的 JIT 或第三方静态重编译。Sunplus SPG240/280 与 unSP 的关系：SPG24x/SPG28x/PAC300 是**围绕 μnSP 的 SoC**（10 KWord RAM、2800 PPU、2900 scroll、2B00 palette、2C00 sprite RAM 每条目 4 字、3000 SPU、3D00 I/O），CPU 核仍是 μnSP，不是另一套 ISA。
