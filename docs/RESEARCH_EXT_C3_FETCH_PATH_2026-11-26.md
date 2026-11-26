# C3 取指路径 / SPI / 计时 —— 外部资料调研（第五轮 · 外部方向）

调研时间：2026-11-26　｜　对象：digicore-c3（ESP-IDF v5.5.3，riscv32-esp-elf esp-14.2.0）
本地对账基准（实测，非引用）：`sdkconfig` = DIO @80 MHz、8 MB；`.flash.text` 612,100 B；`.iram0.text` 89,384 B；
DIRAM 已用 0x33028 = 208,936 B / 393,216 B（余 ≈180 KB）；面板 80 MHz、`trans_queue_depth=10`、
`max_transfer_sz=320*80*2`、每带 `draw_bitmap` 后等信号量；WiFi + coex 已开；`FREERTOS_HZ=1000`；无 PM。

标注约定：**[官方]** = Espressif 文档/源码明示；**[推算]** = 由官方数字算得；**[社区]** = 第三方项目经验。

---

## 0. 三句话结论

1. **flash 取指模式还停在 DIO，这是当前唯一"官方明示能砍掉近一半取指成本"的开关，且没被试过。**
2. **C3 的 ICache 是硬连线的 16 KB / 8 路 / 32 B 行 / 单 bank，没有任何 Kconfig 能调**——所有"调大 cache"的设想直接作废，只剩"改代码放置与布局"和"QIO"两条路。
3. **51 ms 单帧不一定是 cache 的锅**：一次 flash 扇区擦除在默认配置下会让 ICache 停摆、所有任务挂起，量级正好 20–100 ms。先排除 save/NVS 写，再谈取指。

---

## 1. Flash 取指路径（XIP）

### 1.1 QIO/QOUT 在 C3 上可用，且官方给了量化收益 —— 当前配置是 DIO

- **结论**：C3 支持 QIO/QOUT；官方两处说法分别是"QIO/QOUT 相比默认 DIO **几乎翻倍**（almost double）"和"DIO 比 QIO **慢约 45%**"、并明确"用 Dual SPI 回填 cache 的耗时**约为 Quad 的一半速度**"。当前 `CONFIG_ESPTOOLPY_FLASHMODE="dio"`，等于白扔这 45–50%。
- **证据**：
  - [Speed Optimization (ESP32-C3)](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/api-guides/performance/speed.html) — "Set CONFIG_ESPTOOLPY_FLASHMODE to QIO or QOUT ... Both almost double the speed at which code is loaded or executed from flash compared to the default DIO mode."
  - [esptool · SPI Flash Modes (ESP32-C3)](https://docs.espressif.com/projects/esptool/en/latest/esp32c3/advanced-topics/spi-flash-modes.html) — 表格：qio 最快，**dio 约慢 45%**，dout 约慢 50%；FAQ："refilling the cache with a Dual SPI read is approximately half as fast as its Quad SPI equivalent"；"For ESP chips, **32 bytes is read per command**"。
- **对本项目动作**：
  1. `CONFIG_ESPTOOLPY_FLASHMODE_QIO=y`（`sdkconfig.defaults` 里那段注释本来就写着"XMC 8 MB part supports it"，但配置是 DIO——注释和配置自相矛盾，先修这个）。
  2. 重出**整片镜像**：bootloader + app 头的 flash mode 必须一致（`esptool merge-bin --flash-mode keep` 时两个镜像各自的头都要是 qio），否则 ROM 阶段按 dio 引导、后半段行为不一致。
  3. 先在**手头这块板**上验证 `esptool flash-id`（确认是 XMC/GD 的 quad 型号）＋ 上电跑一次 `K` 场景；C3 模块的 flash 走专用 MSPI 引脚 GPIO12/13/14/15/16/17（HD/WP/CS/CLK/MOSI/MISO，[spi_pins.h](https://github.com/espressif/esp-idf/blob/release/v5.5/components/soc/esp32c3/include/soc/spi_pins.h)），WP/HD 是硬连的，电气上不是问题，风险只在 flash 型号是否支持 EBh 及 bootloader 的 quad 使能序列。
- **收益/风险**：理论取指带宽 ×1.8–2.0，直接作用在 miss penalty 上；`guest_pct`/`mi_s` 应有两位数百分比改善（估算：若 miss 占墙钟 60–70%，QIO 后同场景墙钟 −25~35%）。风险：flash 型号不支持时**板子起不来**——保留 DIO 全片镜像与 `esptool` 恢复流程（`--before default-reset` 强制下载模式）即可回滚，属于可逆实验。

### 1.2 ICache 结构：16 KB / 8 路 / 32 B 行 / 单 bank / 512 行 / 64 组

- **结论**：C3 的 ICache 参数是 ROM 头里的常量，**固定不可配**：`MIN_ICACHE_SIZE = MAX_ICACHE_SIZE = 16384`、`MIN/MAX_ICACHE_WAYS = 8`、`MIN_CACHE_LINE_SIZE = 32`、`MAX_ICACHE_BANK_NUM = 1`。换算：**512 行、64 组、每组 8 路**。整个 `esp_system/port/soc/` 下只有 esp32/s2/s3/c2/c5/c6/c61/p4/h2 有 `Kconfig.cache`，**esp32c3 没有**——`CONFIG_ESP32C3_INSTRUCTION_CACHE_SIZE` 这个符号在 5.5 里不存在。
- **证据**：[rom/cache.h (release/v5.5)](https://github.com/espressif/esp-idf/blob/release/v5.5/components/esp_rom/esp32c3/include/esp32c3/rom/cache.h)；本地 `sdkconfig` 亦有 `CONFIG_SOC_CACHE_MEMORY_IBANK_SIZE=0x4000`。[官方]
- **对本项目动作**：
  - 放弃"调大 ICache / 拆 bank"方向；把预算按 **64 组 × 8 路** 来理解：8 路意味着**8 个冲突地址才会互相踢**，所以"600 KB 随机跳"真正的问题是**工作集 > 16 KB**，不是冲突。任何"把热块压进 8–12 KB"的布局努力直接等于命中率。
  - 每行 32 B ≈ 8 条 32-bit 指令（RVC 压缩指令下最多 16 条）。AOT 块体若平均 < 32 B，一次 miss 只服务不到 8 条指令，miss 密度天然高——块体拆分粒度要以"行"为单位核算，而不是以"块"。
- **收益/风险**：认知校准，零风险；但它否掉了 Q4 里的一半选项（见 §4）。

### 1.3 miss penalty：官方没有给周期数，可给出带宽下限

- **结论**：Espressif **没有公开 C3 的 ICache miss penalty 周期数**（TRM 只描述 cache 行为，不给数字；IDF 文档只说"may reduce delays caused by a cache miss"）。但用官方两条事实可以夹出下限：**每次 fill 读 32 B**（esptool）+ **SPI 时钟 80 MHz**。
  - DIO：cmd 8 clk（1-bit）+ addr 24 bit @2-line = 12 clk + 少量 dummy + data 256 bit @2-line = 128 clk ≈ **152 clk ≈ 1.9 µs ≈ 304 个 CPU 周期**（160 MHz）。
  - QIO：8 + 6 + 少量 dummy + 64 ≈ **82 clk ≈ 1.0 µs ≈ 165 周期**。
  - 比值 ≈1.85×，与官方"约一半速度/慢 45%"吻合。以上全部 **[推算]**，不含 tag 比对、总线仲裁、prefetch 命中；真机只会更贵。
- **证据**：同上两条 + [Memory Types](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/api-guides/memory-types.html)（"ESP32-C3 reads code and data from flash via the MMU cache ... placing a function into IRAM may reduce delays caused by a cache miss"）。**未公开项**：miss penalty cycles、prefetch 深度、victim 选择策略。
- **对本项目动作（诊断口径）**：把"每条 guest 指令摊到多少 host 周期"当主指标。当前 `5.7–6.5 M guest insn/s` @160 MHz = **24–28 host 周期/guest 指令**——这个数字对一个 AOT 重编译块是荒谬的（应 3–10），差额只能由 miss 解释。用 §1.5 的方法把"每帧 miss 次数"变成可观测量，再乘 1.9 µs 就能对上 51 ms。
- **收益/风险**：无风险；但如果算出来 miss 只占 30%，就别再往 IRAM 里搬代码。

### 1.4 官方对"大代码 + 随机跳转"的建议，逐条对账

| 官方建议 | 出处 | 本项目现状 | 动作 |
|---|---|---|---|
| 热函数放 IRAM，"loaded once at boot, then always execute at full speed" | [speed.rst](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/api-guides/performance/speed.html) Targeted Optimizations | **全工程 0 处 `IRAM_ATTR`**（grep 结果），解释器与 AOT 块全在 flash | §2.3 |
| `-Os` 换更小代码 = 每块更少行 | 同上（"IRAM is a limited resource"，与 size 权衡）+ 本地 CMakeLists 注释 | `aot_blocks.c` 已单独 `-Os`，其余 `-O3` | 已做 |
| `-fjump-tables -ftree-switch-conversion` 对热 switch 有用 | 同上 Targeted Optimizations | 已对 main 组件打开 | 已做 |
| IDF 默认全工程 `-fno-jump-tables -fno-tree-switch-conversion` | [Memory Types](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/api-guides/memory-types.html) IRAM 段 | 同上，已覆盖 | — |
| 小函数 microbenchmark 会被 cache 布局带偏，"binary layout can cause different patterns of cache misses" | [speed.rst](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/api-guides/performance/speed.html) Measuring Performance | 他们的 A/B 是整机日志，不是 microbench | 做单块 IRAM vs flash 的 A/B 时注意这点 |
| 链接脚本片段可以按**函数/整文件**指定 `noflash` 放置 | [Linker Script Generation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/api-guides/linker-script-generation.html) | 未用 | §2.3 提供做法 |

### 1.5 只用官方 ROM API 就能"锁住"热代码行（实验性，风险中高）

- **结论**：C3 ROM 提供 **ICache 行级 lock / prelock / manual preload**：`Cache_Lock_ICache_Items(addr, items)`、`Cache_Enable_ICache_PreLock(lock_config)`、`Cache_Start_ICache_Preload(addr,size,order)`。把解释器 dispatch + Top-N 块的行钉死在 cache 里，等价于"16 KB 软 IRAM"，**不占 DRAM**。代价：被钉死的行不可替换，其余代码的有效 cache 变小。
- **证据**：[rom/cache.h](https://github.com/espressif/esp-idf/blob/release/v5.5/components/esp_rom/esp32c3/include/esp32c3/rom/cache.h)（`struct lock_config {addr, size, group}`，两个 group）。**注意**：ESP-IDF 自身**从不调用**这些函数（全树 grep 只有声明），属于"ROM 有、SDK 不背书"的能力；头文件字面写着 "Please do not call this function in your SDK application"。**[官方能力 / 无 SDK 支持 = 社区级风险]**
- **动作**：只有当 §1.1/§2.3 都做完、还想再挤 10–20% 时才碰。先做 8 KB prelock（= 全部 16 KB 的一半），A/B 记录 `guest_pct`；任何 cache error 中断（`ETS_CACHEERR_INUM=25`）都意味着回滚。
- **收益/风险**：命中率提升上限很高；风险是 cache 错误直接 panic、且没有任何 IDF 代码路径为它做兼容。

---

## 2. SRAM 布局：IRAM/DRAM 共用一块物理 SRAM

### 2.1 400 KB = 16 KB 只读指令窗 + 384 KB I/D 共用窗

- **结论**：`SOC_IRAM_LOW 0x4037C000`、`SOC_IRAM_HIGH 0x403E0000`（400 KB）；`SOC_DIRAM_IRAM_LOW 0x40380000` / `SOC_DRAM_LOW 0x3FC80000`（**384 KB**，同一块物理 SRAM 的两个别名，偏移 0x400000）。低 16 KB 只有指令别名。**代码进 IRAM 与堆/缓冲争的是同一块 384 KB**——1 字节代码 = 1 字节堆。
- **证据**：[soc.h (release/v5.5)](https://github.com/espressif/esp-idf/blob/release/v5.5/components/soc/esp32c3/include/soc/soc.h)；[Memory Types](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/api-guides/memory-types.html)："The maximum statically allocated DRAM size is reduced by the IRAM size of the compiled application. The available heap ... is reduced by the total static IRAM and DRAM usage." [官方]
- **本项目实测**：IRAM text 89,384 B + DRAM data 7,400 + bss 111,928 = 208,936 B，剩 **≈180 KB**。也就是说：**塞 64 KB 热代码进 IRAM 后仍有 ≈116 KB 堆**——可行，但不是免费。

### 2.2 DMA 只能看 384 KB D 窗，且必须 32-bit 对齐

- **结论**：`SOC_DMA_LOW/HIGH = 0x3FC80000/0x3FCE0000`；SPI DMA 缓冲必须在 DRAM（`MALLOC_CAP_DMA` 或 `DMA_ATTR`）且 32-bit 对齐，否则驱动会分配临时缓冲并**多一次拷贝**（官方："the transaction efficiency will be affected due to the allocation and copying of temporary buffers"）。
- **证据**：[soc.h](https://github.com/espressif/esp-idf/blob/release/v5.5/components/soc/esp32c3/include/soc/soc.h)；[Memory Types · DMA-Capable Requirement](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/api-guides/memory-types.html)；[SPI Master · Write and Read Phases](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/api-reference/peripherals/spi_master.html)。[官方]
- **动作**：`s_strip` 用 `heap_caps_malloc(size, MALLOC_CAP_DMA|MALLOC_CAP_8BIT)` 或 `DMA_ATTR static`；确认 `esp_ptr_dma_capable()` 为真再喂给 `draw_bitmap`。（当前若用普通 malloc 也在 DMA 区，但一旦以后把缓冲挪进 RTC/静态 IRAM 就会静默降速。）
- **收益/风险**：避免"看起来在跑、实际每帧多一次 memcpy"的隐性成本。

### 2.3 取舍：代码进 IRAM vs 缓冲进 DRAM

- **结论（判断）**：本项目缓冲需求是"几条 320×N 条带"（320×24×2 = 15.4 KB/条），属于**小且必需**；热代码需求是"几百 KB 里的几十 KB"，属于**大且可裁剪**。所以优先级是：先保住 2–3 条条带（≈45 KB），把剩下 **~100 KB** 判给 IRAM 热代码。
- **做法（官方支持的两种）**：
  1. 源码级：`IRAM_ATTR`（注意串常量/跳表会留在 flash，要配合 `DRAM_ATTR`/`DRAM_STR`）。
  2. 链接级（更适合 AOT 生成代码）：给组件加 `linker.lf` 片段
     ```
     [mapping:aot_hot]
     archive: libmain.a
     entries:
         aot_hot_* (noflash)
     ```
     然后把 `aot_blocks.c` 里的热块改成 `aot_hot_<n>` 命名或用 `__attribute__((section(".iram1.aot_hot")))`——`noflash` 放置对**整文件/按符号**都生效，且会把函数内常量一并搬进 DRAM（`IRAM_ATTR` 不会，见 Memory Types 的注意事项）。
- **证据**：[Memory Types · How to Place Code in IRAM](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/api-guides/memory-types.html#how-to-place-code-in-iram)；[Linker Script Generation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/api-guides/linker-script-generation.html)。[官方]

---

## 3. SPI Master / esp_lcd 的异步语义

### 3.1 `esp_lcd_panel_draw_bitmap` 是**异步**的，`on_color_trans_done` 在 **ISR** 里回调

- **结论**：`draw_bitmap` = CASET/RASET（polling，几十 µs）→ `tx_color`（把颜色数据切成 chunk 逐个 `spi_device_queue_trans`）→ **立即返回**。真正传完才在 SPI post-transaction 回调里调 `on_color_trans_done`。所以"draw 返回"≠"屏上有了"。头文件原文："push into a queue. The real transmission is performed in the background (DMA+interrupt). ... Recycling of color buffer should be done in the callback `on_color_trans_done()`. "
- **证据**：[esp_lcd_panel_io.h (release/v5.5)](https://github.com/espressif/esp-idf/blob/release/v5.5/components/esp_lcd/include/esp_lcd_panel_io.h)；[esp_lcd_panel_io_spi.c](https://github.com/espressif/esp-idf/blob/release/v5.5/components/esp_lcd/spi/esp_lcd_panel_io_spi.c)（`post_cb = lcd_spi_post_trans_color_cb` → 调 `on_color_trans_done`，且 `lcd_spi_pre_trans_cb` 带 `IRAM_ATTR` 说明回调在 ISR 上下文）。[官方]
- **对本项目**：当前 `display_draw_bitmap()` 内**立刻等信号量**（`xSemaphoreTake(..., 1000ms)`），等于把异步驱动当同步用——每带串行。回调里已经用 `xSemaphoreGiveFromISR`，语义是对的，只是没用上异步。

### 3.2 `trans_queue_depth` 的真实语义：池大小，满了就阻塞回收最老的

- **结论**：`trans_queue_depth = N` 分配 N 个事务描述符；`tx_color` 循环里若 `num_trans_inflight < queue_size` 就取新的，否则**阻塞**在 `spi_device_get_trans_result(portMAX_DELAY)` 上回收最老的一个。**深度 1 == 完全同步**；深度 ≥2 才可能"上一笔在飞、下一笔已入队"。它同时也被塞进 `spi_device_interface_config_t::queue_size`。
- **证据**：[esp_lcd_panel_io_spi.c L355–401](https://github.com/espressif/esp-idf/blob/release/v5.5/components/esp_lcd/spi/esp_lcd_panel_io_spi.c)（`if (num_trans_inflight < queue_size) ... else get_trans_result`）。[官方源码]
- **动作**：保持 10 没问题，但**要让它有意义**，调用侧必须"不等完成就喂下一带"（见 3.3）；否则 10 只是白占 10 个描述符的 DRAM。

### 3.3 官方双缓冲流水线写法（本项目差这一步）

- **结论**：官方例程 `peripherals/spi_master/lcd` 的注释原话是 "With the help of the DMA, we can do **render and flush in parallel**"——两块以上缓冲 + 只在"要复用这块缓冲"时才等它的完成回调。
- **证据**：[spi_master 文档 · Application Examples](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/api-guides/performance/speed.html)（原文在 [SPI Master Driver 页](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/api-reference/peripherals/spi_master.html) Application Examples 段）；例程目录 `examples/peripherals/spi_master/lcd`。
- **对本项目动作**：把 `s_strip` 变 **2 条带 + 每带一个二值信号量**：
  ```
  band[i % 2] 填像素 → draw_bitmap(band[i%2])  → 不等
  下一带 i+1 开始算之前，才 take(band_done[(i+1) % 2])
  ```
  即"生产者跑在两带之外"。按当前 band 高度 24 行算，单带线时 320×24×2/10 MB/s = **1.54 ms**，双缓冲可把这 1.54 ms 从关键路径上摘掉。
- **收益/风险**：**收益上限 = 现在的 blit 阻塞时间**（`blit_pct` 0–1% 时几乎为 0，但重绘帧的峰值段才是要看的）；风险：多一条带的堆（15–45 KB）＋"屏比模拟器落后一帧"，对 60 fps 目标可接受，对输入延迟敏感的判定场景要注意。

### 3.4 单笔传输上限 32 KB，超过就分段，段间 SPI 时钟停 3–30 µs

- **结论**：`spi_bus_get_max_transaction_len()` = `MIN(max_transfer_sz, SPI_LL_DMA_MAX_BIT_LEN/8)`；C3 的 `SPI_LL_DMA_MAX_BIT_LEN = 1<<18` bit = **32 KB**。超过 32 KB 由驱动按 DMA 对齐切段，**CS 保持有效**，但时钟暂停 **3–30 µs**。单笔内部再由 4092 B 一个的 DMA 描述符链起来，CPU 不参与。
- **证据**：[spi_ll.h (esp32c3)](https://github.com/espressif/esp-idf/blob/release/v5.5/components/hal/esp32c3/include/hal/spi_ll.h)（`1<<18`）；`spi_master.c` `SPI_LL_DMA_MAX_BIT_LEN / 8` 与 `LLDESC_MAX_NUM_PER_DESC (4096-4)`；[SPI Master 文档](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/api-reference/peripherals/spi_master.html)："For transfers longer than 32 KB, the driver will segment ... SPI clock will briefly pause about 3-30us."；[spi_lcd 文档](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/api-reference/peripherals/lcd/spi_lcd.html) 示例用 `H_RES * 80 * 2` 作 `max_transfer_sz`。[官方]
- **对本项目**：`max_transfer_sz = 320*80*2 = 51,200` > 32 KB，所以实际单笔被钉在 32,768 B；一整帧 153,600 B = 5 段 = 4 个 3–30 µs 的缝（≤120 µs，可忽略）。**但** `max_transfer_sz` 决定 DMA 描述符池大小，设成 51,200 只是多占 DRAM。建议 `max_transfer_sz = 320*24*2 = 15,360`（恰好一条带，1 段、0 缝、描述符池最小）。
- **收益/风险**：省 DRAM、消除段间缝；零风险。

---

## 4. 编译/链接/缓存选项：C3 能动的比想象少

| 问题 | 结论 | 依据 |
|---|---|---|
| `CONFIG_ESP32C3_INSTRUCTION_CACHE_SIZE` | **不存在**。C3 无 `Kconfig.cache`，容量/行宽/相联度全硬连线 | [rom/cache.h](https://github.com/espressif/esp-idf/blob/release/v5.5/components/esp_rom/esp32c3/include/esp32c3/rom/cache.h) + 本地源码树 [官方] |
| QIO/QOUT 在 C3 可用？ | **可用**（不被 OPI 条件排除），且引脚是专用 MSPI 12–17 | [esptool flash modes](https://docs.espressif.com/projects/esptool/en/latest/esp32c3/advanced-topics/spi-flash-modes.html)、[spi_pins.h](https://github.com/espressif/esp-idf/blob/release/v5.5/components/soc/esp32c3/include/soc/spi_pins.h) [官方] |
| 120 MHz flash？ | **C3 没有这个选项**。`components/spi_flash/esp32c3/Kconfig.flash_freq` 只有 80/40/26/20 MHz | [Kconfig.flash_freq](https://github.com/espressif/esp-idf/blob/release/v5.5/components/spi_flash/esp32c3/Kconfig.flash_freq) [官方] |
| `-Os` vs `-O2` vs `-O3` | 官方只给"`-O2` 大概率提性能（体积略增）"，**没有**针对 XIP 命中率的结论。对 16 KB cache + 600 KB 代码，决定成本的是**每块的行数**而非指令数：`-Os` 减行、`-O3` 减指令但加行，方向相反 | [speed.rst](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/api-guides/performance/speed.html) [官方] + [推算] |
| `-ffunction-sections -Wl,--gc-sections` | IDF 默认已开（本项目编译/链接行可见），无需再动；它的价值是让"按函数放置 IRAM"和"没被引用的块滚蛋"成立 | 本地 `build.ninja` LINK_FLAGS `-Wl,--gc-sections`；编译行 `-ffunction-sections -fdata-sections` [实测] |
| 90/160 MHz CPU | 官方 performance 页只提 `CONFIG_COMPILER_OPTIMIZATION`/`HW_STACK_GUARD`，CPU 频率在 `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ`（本项目已 160） | 本地 sdkconfig [实测] |

- **动作**：Q4 只剩一件事没做——**QIO**。其余全是"确认过不可动"，不必再花时间。
- **收益/风险**：把精力从"调 cache 参数"转向"QIO + 放置"，避免在死路上做实验。

---

## 5. FreeRTOS / 计时

### 5.1 tickless idle 与帧时间无关

- **结论**：`CONFIG_FREERTOS_USE_TICKLESS_IDLE` **依赖 `PM_ENABLE`**，作用是"空闲时进 light sleep"，只省电、不改帧时间；且 1 kHz tick 的开销在这个量级完全可忽略（每帧 16.7 tick，tick ISR 数 µs 级 → 每帧 <0.3%）。
- **证据**：[freertos/Kconfig (release/v5.5)](https://github.com/espressif/esp-idf/blob/release/v5.5/components/freertos/Kconfig)（`config FREERTOS_USE_TICKLESS_IDLE` → `depends on PM_ENABLE`，`default n`，注释 "Currently not supported in SMP FreeRTOS yet"）。[官方]
- **动作**：不动 tick。真要省电，改 pacer（他们已做：自旋 5.5 ms → 1.5 ms + `vTaskDelay`）。**不要**为了省电开 PM：light sleep 会牵动 SPI/时钟，和 80 MHz 面板刷新冲突。

### 5.2 高优先级任务抢占才是抖动来源 —— 本项目 **WiFi 是开着的**

- **结论**：官方给了内置任务的优先级：**WiFi 驱动 23、esp_timer 22、lwIP 18、main 1**；并明确"不建议把任务设到 WiFi/BT 之上"、"不做网络的实时任务用 19"、"每次写内置 flash 时**任务执行完全挂起**，只有 IRAM-safe ISR 还能跑"。
- **证据**：[speed.rst · Task Priorities / Built-in Task Priorities](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/api-guides/performance/speed.html)；[Flash 并发约束](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/api-reference/peripherals/spi_flash/spi_flash_concurrency.html)。[官方]
- **本项目实测**：`CONFIG_ESP_WIFI_ENABLED=y`、`CONFIG_ESP_COEX_ENABLED=y`、`CONFIG_LWIP_ENABLE=y`（`TCPIP_TASK_PRIO=18`）。**在单核 C3 上，WiFi 任务 23 一定会抢占模拟器任务**，任何 WiFi 事件都会在帧内插入不可预测的延迟。
- **动作**：拿 `sdkconfig.bak-wifion` 做一次严格 A/B，记录 **p99 帧时间**（不是均值）。若 51 ms 尖峰在 WiFi off 后消失或显著变短，这一条就结案，跟 cache 一点关系没有。

### 5.3 帧时间抖动怎么测

- **结论**：官方明确推荐——`esp_timer_get_time()` 微秒级但有中等开销；**短区间用 `cpu_hal_get_cycle_count()`**（= `esp_cpu_get_cycle_count()`，RISC-V 的 `mcycle`，读一次 ~几周期，160 MHz 下 32-bit 回绕 26.8 s）。
- **证据**：[speed.rst · Basic Performance Measurements](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/api-guides/performance/speed.html)（"including hal/cpu_hal.h and calling the HAL function cpu_hal_get_cycle_count() returns the number of CPU cycles executed ... lower overhead ... good for measuring very short execution times"）。[官方]
- **动作**：每帧记 `(start_cycles, end_cycles, misses_proxy)` 三元组进环形缓冲，`J` 命令导出 **min/p50/p95/p99/max**。当前日志只有 `guest_fps` 均值——均值永远看不见 51 ms。
- **收益/风险**：零风险；这是判断"是 cache 还是抢占/擦除"的唯一手段。

### 5.4 51 ms 的另一个嫌疑人：flash 写/擦

- **结论**：默认配置下**任何 SPI1 读/写都会关 ICache 并挂起所有任务**，直到操作完成；一次 4 KB 扇区擦除（`SE`）典型 30–100 ms，页编程 0.3–3 ms。量级与本项目的 51 ms 尖峰**直接吻合**。C3 的 `CONFIG_SPI_FLASH_AUTO_SUSPEND` 默认关，开了也要 flash 支持 75h/7Ah。
- **证据**：[Flash 并发约束](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/api-reference/peripherals/spi_flash/spi_flash_concurrency.html)（"caches are disabled during SPI1 operations ... all other tasks are suspended"；auto suspend 默认不启用；`tsus` 典型 40 µs）。[官方]
- **动作**：在 save/clock 写入点前后各打一次 cycle count，日志里标 `FLASHWR`；跑同一个"卡顿"场景，看 51 ms 帧的时间戳是否落在 `FLASHWR` 窗口内。若是 → 改成"暂停时写、或先在 DRAM 攒着、或分帧写 4 KB"，别去优化 cache。
- **收益/风险**：可能一次性解释掉尖峰；零风险。

---

## 6. 同类项目经验

### 6.1 Anemoia-ESP32（NES on ESP32，ST7789 320×240，无 PSRAM）

- **做法**：Arduino/IDF 3.2.1 + **自定义 `platform.txt`**，全局
  `-Ofast -ffunction-sections -fdata-sections -funroll-loops -fjump-tables -ftree-switch-conversion -fno-tree-vectorize -frename-registers -fno-plt -flto -mlongcalls`，链接 `-Wl,--gc-sections`；并注明"不同 board 版本性能不同，务必用 3.2.1"。
- **结果**：NTSC 60.10 fps，**带 1 帧跳**，含完整音频；`Kirby's Adventure`（频繁换 bank）掉到 59.57 fps。
- **证据**：[platform.txt](https://github.com/phenax/Anemoia-ESP32/blob/main/platform.txt)、[README](https://github.com/phenax/Anemoia-ESP32/blob/main/README.md)。[社区]
- **对本项目**：可借鉴的是**"开 jump tables + LTO"已经和本项目一致**，说明这条路走到头了；真正不可复制的是硬件差——NES 1.79 MHz 跑在 240 MHz 双核上（**134:1** 的时钟比），本项目是 96 MHz guest 跑 160 MHz 单核（**0.6:1**）。差 **220 倍**的余量，别拿"别人 60 fps"当目标锚点。

### 6.2 共同模式（多项目归纳，[社区]）

- 把 **dispatch/解码循环**整块放 IRAM（不是零散热函数）——对应本项目 `unsp_amalgam` + `AOT` 的入口。
- **块链接（block chaining）**：块尾直接跳下一个块，避免回 scheduler；直接线程化（computed goto / labels-as-values）+ 单次间接跳转，比 switch 派发少一次间接。
- 让**热块在 flash 里物理相邻**：32 B/行的 fill 粒度下，相邻块共享行的概率高；把"同一 guest 场景里连续执行的块"按 profile 顺序重排，等价于把 16 KB cache 的有效容量放大。
- 用 `-Os` 压块体（本项目已做）与"块内联 vs 块大小"反复 A/B。

### 6.3 对本项目的排序判断

1. QIO（一次性、全局、官方量化）。
2. 布局：热块按 profile 连续排布 + 块链接，把工作集压到 <12 KB。
3. IRAM：dispatch + Top-N 块（≈64 KB）。
4. ICache prelock（实验）。
5. 若都要不到 30 fps，硬件换代（S3 双核 240 MHz + 可调 32 KB ICache + PSRAM）才是解，不是继续抠 C3。

---

## 7. 面板侧：ST7789 @ 80 MHz 的硬上限

- **结论 1（硬算术，[推算]）**：320×240×16bpp = **153,600 B**；80 MHz 单线 = **10 MB/s** → **一帧全屏 = 15.36 ms**。这意味着**任何"每帧全屏刷"的方案在 60 fps 下都不可能**：15.36 ms/16.7 ms = **92% 的总线占用**，CPU 只剩 1.3 ms。要 60 fps 就必须"只刷脏区"或"屏率 < 帧率"。
- **结论 2（官方公式，[官方]）**：中断事务的墙钟 = **20 + 8n/Fspi[MHz] µs**（n=字节）。n=15,360 @80 MHz → **1.56 ms**；n=32,768 → **3.30 ms**；n=153,600（会被切成 32 KB 段）→ **15.4 ms** 加段缝。轮询事务固定 ~10 µs/笔，比中断少 10–18 µs/笔。
  - 证据：[SPI Master · Cache Missing / 传输速度](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/api-reference/peripherals/spi_master.html)（"the overall cost is 20+8n/Fspi"；DMA 链表建立 ~2 µs/笔；轮询 10 µs / 中断 28 µs 每字节基线表）。
- **结论 3（控制器 spec，[社区/未取到原始 datasheet]）**：ST7789 系列 4 线串行写时钟的 spec 通行值是 **16 ns 周期 = 62.5 MHz**；本项目跑 **80 MHz（超 spec 28%）**。实践中 ESP32 社区普遍跑 80 MHz，但这是"超频"，失效表现为**偶发像素错位/颜色撕裂**，且与排线长度、供电强相关——出问题时**极容易被误判成模拟器画错**。
  - 证据：本次网络环境下 Sitronix/buydisplay/displayfuture 的原始 PDF 均被拦截（403/406），**未能取得一手 datasheet 页**；因此这一条只能标"社区通行值"。
- **结论 4（本项目实测基线）**：`pclk_hz = 80 MHz`、`max_transfer_sz = 51,200`（>32 KB，被驱动钉到 32 KB）、`trans_queue_depth = 10`、每带 `draw_bitmap` 后等信号量 → **屏率 = 帧率的一半**（历史审计已记录）。
- **动作**：
  1. 算清"每帧实际要刷多少带"：`带数 × 1.54 ms` 就是面板占用的硬开销，与模拟器无关。
  2. 若目标是 60 fps：屏必须 ≤30 Hz 或改成脏矩形（理想：每帧 ≤3 带 ≈4.6 ms）。
  3. 若出现花屏：先把 pclk 降到 62.5/60 MHz 复现一次，别先怀疑模拟器。

---

## 8. 与本项目对账：还没试过的与已经排除的

| # | 事项 | 状态 | 外部依据强度 |
|---|---|---|---|
| 1 | flash QIO | **未做**（配置 DIO，注释却写着支持 QIO） | 官方，量化（−45%） |
| 2 | 热代码进 IRAM（0 处 `IRAM_ATTR`） | **未做**，余量 ≈180 KB | 官方 |
| 3 | 热块按 profile 重排（布局局部性） | **未做** | 官方 + 推算 |
| 4 | 双缓冲流水线（现在每带同步等） | **未做** | 官方例程 |
| 5 | WiFi off 的 p99 A/B | **未做**（WiFi 现为 on） | 官方 + 实测配置 |
| 6 | save/NVS 写与尖峰的时间相关 | **未做** | 官方（擦除量级） |
| 7 | 调大/调 ICache | **不可做**（硬件固定） | 官方 |
| 8 | 120 MHz flash | **不可做**（C3 无此档） | 官方 |
| 9 | `-O3` + jump tables + `--gc-sections` | **已做** | — |
| 10 | `-Os` 压 AOT 块体 | **已做**（单文件） | — |

---

## 9. 最值得先验证的 5 条（按 收益/成本 排序）

1. **QIO 全片镜像 A/B**（半天）。同一场景、同一 ROM、`guest_pct` + `mi_s` + 帧时间 p99 三项对比。判据：`mi_s` 提升 <10% 说明瓶颈不在取指带宽，立刻转向第 3/5 条。
2. **给帧时间装 p99 直方图**（2 小时）。cycle counter 起止 + `FLASHWR` 标记；先分清"51 ms 是 cache、是抢占、还是擦除"。没有这个，后面四条都是猜。
3. **WiFi off 的严格 A/B**（1 小时，配置现成）。只看 p99/max，不看均值。
4. **IRAM 放置一个热块集合的 A/B**（1 天）。取 profile 里 Top-16 块（≈32–64 KB），`IRAM_ATTR` 或 `linker.lf noflash`，对比同场景墙钟。判据：若单块 IRAM 化带来 >1.5× 的该块耗时下降，就把预算加到 ~100 KB。
5. **热块按执行序重排 + 块链接**（1–2 天）。用现有 `C3_BLOCK_CENSUS` 的 profile 输出排序，让同一场景的块在 `.flash.text` 里连续；配 computed-goto 块尾直跳。这是唯一能把"16 KB cache"当"32–48 KB 用"的手段。

（第 6 条备选：`max_transfer_sz` 改 15,360 + 双缓冲条带，1 小时，专治重绘帧的 blit 阻塞。）
