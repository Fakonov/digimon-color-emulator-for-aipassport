# R6B 显示管线审计：把 7.4 ms 的阻塞式 SPI 推送改成与 guest 重叠

日期：2026-11-26
基线：v1.14.0（`firmware/digicore-c3`）
性质：**只读源码审计 + 算术复核**。本轮没有改 `firmware/` 下任何一个字节，设备未上电（只有 COM1），
所有「实测」数字都来自仓库里已有的设备日志（`dbgtest/*.log`），不是本轮采的。
本机 IDF：`C:\Users\DuWeisi\idf-setup\esp-idf-5.5.3`（v5.5.3）。

> **行号基准**：本报告所有 `app_main.cpp` / `emu/st7735.cpp` 行号按 v1.14.0 提交态（git HEAD）写。
> 审计期间工作区有并行改动（另一个 agent 正在改 guest 像素推送路径：`app_main.cpp` 在 2253 行后
> 插入 10 行、`emu/st7735.cpp` 在 313 行后追加 `write_run()`）；这些插入点都在本报告引用行之后，
> 只有 `app_main.cpp:3106`（`panel_fill_black` 第二调用点）与 `:3202`（`s_force_full`）会顺延约 10 行。
> 本报告没有改 `firmware/` 任何文件。

---

## 0. 结论先说

| # | 问题 | 结论 | 一句话 |
|---|---|---|---|
| Q1 | `tx_color(io, -1, …)` 只发数据？ | **是**，分支在 `esp_lcd_panel_io_spi.c:325`（`send_cmd = (lcd_cmd >= 0)`） | 但**不能靠连续调它来做重叠**，见 Q2 |
| Q2 | 两次 `tx_color` 会不会互等？ | **会**，而且是两道锁 | 显式 drain：`:326-333`；隐式 drain：`:323` 的 `acquire_bus`，它必须等本设备在飞事务清空 |
| Q3 | 设一次窗 + 多次只发数据，靠 CS 拉高再拉低续传？ | 模型成立；真实芯片「推测」成立 | **但方案根本不需要它**：整个窗口期间 CS 一直是低的 |
| Q4 | 能不能与 guest 重叠？ | **能**，最小改动 = 整窗一次 `tx_color(RAMWR, 73,728)`，单缓冲 73,728 B | 每帧阻塞 **14.4 ms → 0.6 ms**，guest 回收 ≈13.8 ms |
| Q5 | 80 MHz 推 73,728 B 是 7.37 ms？ | **板子跑不到 80 MHz** | 12,288 B 实测 2.468 ms = **39.8 Mbit/s**，即 40 MHz 档；整帧真值 **14.4–14.8 ms** |
| Q6 | `max_transfer_sz` / `panel_fill_black` | 不越界、不需重发 | 51,200 只多占 216 B 描述符；黑屏填充是一次性的 15 带全屏写，本来就必须整屏 |

**对 R5 结论的复核**（`docs/AUDIT_PERF_2026-11-26_R5.md:200-206`、`:240`）：

1. 「双缓冲收益为 0」——**按带双缓冲确实为 0**，但这句被当成了「异步没救」的结论，错了。带级双缓冲之所以为 0，
   是因为每次 `tx_color` 都要重新 `acquire_bus`（Q2），带宽仍旧逐带串行；**帧级**缓冲（一个 73,728 B 的整窗缓冲、
   一帧只调一次 `tx_color`）就没有这个问题。
2. 「真异步要整窗一次设窗 + `tx_color(-1)` 续传」——**前半句对、后半句错**。一次 `tx_color(RAMWR, 73728)` 内部就把
   73,728 B 切成 3 笔 32,768/32,768/8,192 的 DMA（`esp_lcd_panel_io_spi.c:371-379`），非末笔自动置
   `SPI_TRANS_CS_KEEP_ACTIVE`，**CS 全程不抬**，所以「RAMWR 跨 CS 续传」这条未验证的硬件行为压根不需要押。
3. 「80 MHz 是真的」——**与本报告第 6 节矛盾**，且与 R5 自己引用的 `blitmax` 矛盾。见 Q5。

**净结论**：73 KB DRAM 这个代价 R5 估对了；「卡在未验证的硬件行为」估错了。按下面的改法，
整窗重绘帧的 SPI 阻塞从 14.4 ms 降到 0.6 ms 量级，代价是 73,728 B 静态 DRAM（余量 88,440 B，紧但能试）。

---

## 1. 证据索引

### 1.1 IDF（v5.5.3，本机）

| 主题 | 路径:行 |
|---|---|
| `tx_color` 全函数 | `components/esp_lcd/spi/esp_lcd_panel_io_spi.c:316-406` |
| `lcd_cmd < 0` 分支 | 同上 `:325` |
| 带命令时的显式 drain | 同上 `:326-333` |
| 分块队列 + CS_KEEP_ACTIVE + 末笔回调 | 同上 `:355-401`（关键 `:371-379`） |
| 只有末笔触发 `on_color_trans_done` | 同上 `:431-435` |
| `tx_param` 也 drain | 同上 `:216-221` |
| API 文档：`lcd_cmd` 传 -1、缓冲生命周期 | `components/esp_lcd/include/esp_lcd_panel_io.h:25-26, 60-66` |
| ST7789 `draw_bitmap`：CASET/RASET 走 tx_param、RAMWR 走 tx_color | `components/esp_lcd/src/esp_lcd_panel_st7789.c:201-230` |
| `spi_device_queue_trans` 异步、CS_KEEP_ACTIVE 需先 acquire | `components/esp_driver_spi/src/gpspi/spi_master.c:1255-1300`（检查 `:1264-1270`） |
| `acquire_bus` / `release_bus` | 同上 `:1351-1382` / `:1385-1413` |
| 单笔事务长度上限检查 | 同上 `:1119-1120`、`:1152`（DMA 上限 `SPI_LL_DMA_MAX_BIT_LEN`） |
| `spi_bus_get_max_transaction_len` = min(max_transfer_sz, 32768) | 同上 `:1509-1524` |
| `cs_keep_active` 进 HAL | 同上 `:801`；落到 `spi_ll_master_keep_cs`（`hal/esp32c3/include/hal/spi_ll.h:693`） |
| 完成回调在 ISR | 同上 `:858-872`、`:970-1102` |
| 总线上限/描述符池 | `components/esp_driver_spi/src/gpspi/spi_common.c:303-325`、`:847-856` |
| bus_lock：acquire 必须等本设备在飞请求清空 | `components/esp_hw_support/spi_bus_lock.c:102-108`、`:723-748`（断言 `:743-746`） |
| C3 单笔 DMA 上限 2^18 bit = 32 KiB | `components/hal/esp32c3/include/hal/spi_ll.h:42` |
| C3 时钟计算（> 3/4 源频直接 equ_sysclk） | 同上 `:757-770` |
| C3 SPI2 IOMUX 引脚：CLK=6 / MOSI=7 / CS=10 | `components/soc/esp32c3/include/soc/spi_pins.h:19-25` |
| GPIO matrix 40 MHz 限制（官方文档） | `docs/en/api-reference/peripherals/spi_master.rst:446-456`、`:499-501` |
| 描述符单片上限 4092 B | `components/hal/include/hal/dma_types.h:59` |
| 实际时钟查询 API | `spi_master.c:653`、`include/driver/spi_master.h:364` |

### 1.2 本项目

| 主题 | 路径:行 |
|---|---|
| PCLK 80 MHz 与那段过时注释 | `firmware/digicore-c3/main/display.c:27-31` |
| `max_transfer_sz = 320*80*2` | 同上 `:133` |
| `trans_queue_depth = 10` | 同上 `:145` |
| 回调 + 二值信号量 | 同上 `:72-84`、`:119` |
| 阻塞式 `display_draw_bitmap` | 同上 `:205-224`（等信号量 `:219-221`） |
| `s_strip` 尺寸推导（12,288 / 15,360） | `app_main.cpp:360-389` |
| `blit_frame()` 六带循环与计时点 | 同上 `:969-1115`（计时 `:1088-1094`） |
| 空闲修补扫描（一轮一带） | 同上 `:1023-1048` |
| `panel_fill_black()` | 同上 `:921-942`，调用点 `:1591`、`:3106` |
| PC 侧 ST7735 模型 | `emu/st7735.cpp:95-106`、`:152-155`、`:188-233` |

### 1.3 设备日志（仓库内已有，非本轮采集）

| 文件:行 | 内容 |
|---|---|
| `dbgtest/r5_final.log:5` | v1.13.0，`blitmax=2468 rows=192 blits=6`（整窗修补扫描的一整带） |
| `dbgtest/r5_v114.log:105/108/112/114` | v1.14.0，`blitmax=2179/2378/2324/2664`，`rows=192 blits=6` |
| `dbgtest/r5_v114.log:117/118` | `blitmax=2656 rows=128 blits=4`、`2080 rows=64 blits=2` |
| `dbgtest/r5_v114.log:60` | `heap 144352 B (largest block 114688 B)` |
| `dbgtest/spi40.log:41` | 早期构建（320 宽全屏带）`blitmax=4157 rows=192` |

---

## 2. Q1：`lcd_cmd = -1` 到底做了什么

**答：表示只发颜色数据、不发命令。**

- 分支只有一行：`bool send_cmd = (lcd_cmd >= 0);`（`esp_lcd_panel_io_spi.c:325`）。
  `send_cmd == false` 时，整个 `if (send_cmd) { … }` 块（`:326-352`）被跳过：不发 RAMWR、
  不做 drain、不写 DC 命令电平（DC 只在 pre_cb 里按事务的 `dc_gpio_level` 设置，数据笔是 `dc_data_level`，
  `:382`），直接进分块队列循环（`:355-401`）。
- API 契约也这么写：`esp_lcd_panel_io.h:66`「`lcd_cmd` … set to -1 if no command needed」，`:62` 说明
  tx_color 是「塞进队列、后台 DMA+中断发」并且**缓冲生命周期由调用者负责，回收点是 `on_color_trans_done()`**。

**但**：`tx_color` 的第一行有效代码是 `spi_device_acquire_bus(spi_panel_io->spi_dev, portMAX_DELAY)`
（`:323`），它在 `if (send_cmd)` 之外。所以「-1」只是省掉了命令字节和显式 drain，
**并没有省掉总线锁**——见 Q2。

**最短实现路径**：不需要自己调 `spi_device_queue_trans`，也不需要改 IDF。用
`esp_lcd_panel_draw_bitmap(panel, x0, y0, x1, y1, buf)` 一次把整窗交给 io 层：
ST7789 的 `draw_bitmap` 发 CASET/RASET（各 4 字节参数）后调用
`esp_lcd_panel_io_tx_color(io, LCD_CMD_RAMWR, color_data, len)`（`esp_lcd_panel_st7789.c:213-227`），
后面的分块、CS 保持、末笔回调全部由 io 层完成。理由见 Q4。

---

## 3. Q2：两次 `tx_color` 会不会互等？`trans_queue_depth=10` 用上了吗

**会互等，而且是两道独立的锁。**

### 3.1 显式 drain（只在带命令时）

`esp_lcd_panel_io_spi.c:326-333`：`send_cmd` 为真时，先把 `num_trans_inflight` 笔
`spi_device_get_trans_result(…, portMAX_DELAY)` 全部收完，才发命令。这就是
`esp_lcd_panel_io.h:25-26` 写的那句「如果还有 tx_color 排队的传输，本函数会等到队列空」。
`tx_param` 里同一段在 `:216-221`（无条件执行，因为 tx_param 总带命令）。

### 3.2 隐式 drain（每次调用都有，包括 `lcd_cmd = -1`）

`spi_device_acquire_bus()` → `spi_bus_lock_acquire_start()`（`spi_master.c:1357`）→ 阻塞在
`dev_wait(dev_handle, wait)`（`spi_bus_lock.c:731-738`）。授予条件写在同文件的状态机注释里：

> 「Should not go to the STATE_ACQ (unblock the acquiring task) until all requests of the acquiring
> device are finished. This is to preserve the sequence of foreground (polling) and background operations
> of the device.」（`spi_bus_lock.c:102-108`）

并在 `:743-746` 用断言收尾：拿到锁时本设备的 BG 位必须已经清空。换句话说：
**只要上一笔 `tx_color` 排的 DMA 还没发完，下一次 `tx_color`（哪怕 `cmd=-1`）就会在
`io_spi.c:323` 卡住。** 这就是 R5「带命令调用会 drain」之外真正致命的那一半——
即使换成 `tx_color(-1)`，逐带串行也不会消失。

### 3.3 `trans_queue_depth = 10` 现在有没有用上

**没有，实际在飞事务恒为 1。**

- 事务池大小 = `trans_queue_depth`（`io_spi.c:76`），SPI 设备队列深度 = 同一个值（`:87`）。
- 一次 `display_draw_bitmap` 送 12,288 B；io 层的单笔上限
  `spi_trans_max_bytes = spi_bus_get_max_transaction_len()`（`:121-122`）= min(53,196, 32,768)
  = **32,768 B**（`spi_master.c:1518`）。
  12,288 < 32,768 ⇒ 一次 `tx_color` 只产生 **1 笔**事务，`num_trans_inflight` 在返回时是 1，
  下一带一进来就被 3.2 排空。
- 真正把 10 用起来的是「一次 submit 超过 32,768 B」的场景——也就是 Q4 的整窗 73,728 B（3 笔）。
  10 仍然够（3 ≤ 10），但从此它不再是死参数。

---

## 4. Q3：RAMWR 的地址计数器跨 CS 是否继续

### 4.1 先说明：**推荐方案不需要这个行为**

Q4 的方案里，整个窗口的 CASET/RASET/RAMWR 加 3 笔数据都在**同一次 `spi_device_acquire_bus()`** 里发出，
非末笔带 `SPI_TRANS_CS_KEEP_ACTIVE`（`io_spi.c:371-379`），HAL 层落到
`spi_ll_master_keep_cs(hw, trans->cs_keep_active)`（`components/hal/spi_hal_iram.c:201`），
**CS 从头到尾是低的**，面板看到的是一条连续的数据流。跨 CS 续传在这条路径上不存在。

### 4.2 如果一定要跨 CS（例如想在帧中途释放总线）

- **主机侧事实**：`spi_device_queue_trans` 只允许在持有总线时置 CS_KEEP_ACTIVE
  （`spi_master.c:1268-1270`，否则返回 `ESP_ERR_INVALID_ARG`），所以「抬 CS 再拉低」必须自己释放/重取总线，
  中间还会被 3.2 的 drain 卡住。这条路的收益是负的。
- **芯片侧**：ST7789V 的 RAMWR（2Ch）是流式写显存命令，地址计数器由 CASET/RASET 装载、每写一个像素自增，
  CSX 只是 4 线串行接口的门控。**推测**：CSX 拉高只是暂停移位，不重置地址计数器（也无从重置——它没有对应的
  复位逻辑）。**本轮未能取得数据手册原文逐字表述**：目标 PDF（Sitronix ST7789V，308 页，RAMWR 在
  §9.1.27 / 聚合站第 199 页附近）被 Cloudflare 挡在 `web_fetch` 之外（403 / application/pdf），
  所以这一段按「推测」标注，不给假页码。
- **要坐实只要一个 5 分钟实验**（本报告不要求做）：设窗 192×192，第一遍一次发完整帧；
  第二遍分 3 次发（每次 24,576 B，中间 `tx_color(io, -1, …)`），两遍画面逐像素一致 ⇒ 跨 CS 续传成立。
- **模型侧**：`emu/st7735.cpp` 里**根本没有 CS 的概念**——只有 `command_w()` 和 `data_w()`。
  0x2C 的数据字节按 `m_posx/m_posy` 落盘并在 `m_posmaxx/m_posmaxy` 处回卷
  （`:221-233`）；`m_posx/m_posy` 只在 CASET/RASET 的参数字节里重设
  （`:201`、`:215`，而且 MAME 的口径是**每个参数字节都重设一次**），SWRESET 才清零（`:65-66`）。
  因此在这个模型里「设一次窗 + N 次只发数据」与「一次发完」**逐字节等价**；
  也就是说模拟器侧永远无法替我们证伪或证实真实芯片的跨 CS 行为，这条只能上机。

---

## 5. Q4：最小改动方案

### 5.1 为什么是「整窗一次」，不是「双 12,288 B」，也不是「单 12,288 + 4 KB」

把流水线算清楚就一目了然。设一带的 DMA 时间 `D ≈ 2.4 ms`（实测，见 Q5），打包一带
`P ≈ 0.05 ms`，一帧 6 带，环形缓冲 N 块（每块 12,288 B）：

- 队列里最多能领先 N 带；第 k+N 带要等第 k 带的 DMA 结束才能覆盖它的缓冲。
- 于是 CPU 在帧内仍要等 `(6 − N) × D`：

| N | 缓冲 | 帧内仍阻塞 | 回收 | 结论 |
|---|---|---|---|---|
| 1（现状，逐带等） | 12,288 B | 6 × 2.4 = 14.4 ms | 0 | 现状 |
| 2（「双 12,288 B」） | 24,576 B | 4 × 2.4 = 9.6 ms | 4.8 ms | 与 R5「收益为 0」接近，确实不划算 |
| 4 | 49,152 B | 2 × 2.4 = 4.8 ms | 9.6 ms | 退档方案 |
| **6（整窗一块）** | **73,728 B** | **≈0** | **≈14 ms** | 推荐 |

「单 12,288 + 4 KB」只够 N=2，即回收 4.8 ms，不值得为它改结构。
**正确的缓冲单位是「一帧要发的那块矩形」，不是「一带」。**

### 5.2 数据流

```
guest 帧结束
  └─ blit_frame():
       wait_prev_frame()            // 只等一次；通常返回 0（guest 跑了 14+ ms）
       bbox = 所有脏 tile 的并集    // 一次窗口，不再逐带设窗
       pack bbox → s_frame[]        // 2x 放大 + RGB565 大端，与现在同一段映射代码
       esp_lcd_panel_draw_bitmap(panel, x0, y0, x1, y1, s_frame)   // 只入队，不等
  └─ 立刻回 guest 执行（DMA 在后台跑 14.4 ms）
```

内部实际发生的事（全部在 IDF 里，已有代码）：

1. ST7789 `draw_bitmap`：`tx_param(CASET)` → `tx_param(RASET)` → `tx_color(RAMWR, buf, len)`
   （`esp_lcd_panel_st7789.c:213-227`）。
2. `tx_color`：acquire 总线（此时上一帧早已发完，不阻塞）→ 轮询发 RAMWR（1 字节，带 CS_KEEP_ACTIVE）
   → 分块：73,728 B ⇒ 32,768 + 32,768 + 8,192，前两笔 CS_KEEP_ACTIVE、末笔 `en_trans_done_cb=1`
   （`io_spi.c:371-379`）→ 三次 `spi_device_queue_trans`（`:394`）→ release 总线（`:404`）。
   三笔 DMA 由 ISR 串起来连续发，CS 不抬。
3. 末笔完成 → post_cb（ISR 上下文，`spi_master.c:858-872`）→ `on_color_trans_done` → 释放信号量。

### 5.3 代码形状（伪代码，改 `display.c` + `app_main.cpp`，不动 IDF）

```c
// display.c
static uint8_t s_frame[DISPLAY_FRAME_BYTES];   // 192*192*2 = 73,728
static bool    s_pending;

esp_err_t display_draw_bitmap_async(int x, int y, int w, int h, const void *data)
{
    esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, x, y, x + w, y + h, data);
    if (err == ESP_OK) s_pending = true;       // 只入队
    return err;
}

esp_err_t display_wait_transfer(TickType_t ticks)
{
    if (!s_pending) return ESP_OK;
    if (xSemaphoreTake(s_trans_done, ticks) != pdTRUE) {
        s_pending = false;                     // 半帧/超时：交还重绘责任
        return ESP_ERR_TIMEOUT;
    }
    s_pending = false;
    return ESP_OK;
}
```

```c
// app_main.cpp: blit_frame() 开头
bool prev_ok = (display_wait_transfer(pdMS_TO_TICKS(100)) == ESP_OK);
if (!prev_ok) s_blit_repaint_all = true;       // 复用现有「全脏」通路

// …脏 tile 并集 → bbox（tile 粒度，输出坐标仍用 left_px()/top_px()）
int bx0, by0, bx1, by1;                        // tile 下标
size_t bytes = (bx1 - bx0 + 1) * kTile * kScale * 2
             * (by1 - by0 + 1) * kTile * kScale;

if (bytes >= kBigBlitBytes /* 例如 36,864 */) {
    pack_bbox_to(s_frame);                     // 现有 2x 映射代码，目标指针换成 s_frame
    err = display_draw_bitmap_async(outx, outy, wpx, hpx, s_frame);
} else {
    /* 老路径原样保留：逐带 display_draw_bitmap()，最多 2 带 */
}
```

**为什么留一个阈值**：时钟界面每帧只脏一两行 tile，bbox 可能只有 2–4 KB；无脑按整窗发要 14.4 ms，
在 40 MHz 上等于把总线占满。所以先用「bbox 字节数 ≥ 36,864（半窗）走异步、否则走老路径」做 A/B。
注意这只是**省带宽的策略，不是正确性要求**：阈值设成 0（全部走 bbox 异步）同样正确，代价是散布脏点
（比如左上角时钟 + 中间精灵）时会把中间的空白一起发出去。**时钟数字每秒跳一次的那种小更新也必须
异步**——它现在是一条带 2.4 ms、两条 4.8 ms，占 16.7 ms 帧的 15–29%，正是用户说的「画面移动慢」；
如果 A/B 发现小更新还在卡，就把阈值降到 0。

### 5.4 时间预算（一帧 192×192 全脏）

| 项 | 数值 | 依据 |
|---|---|---|
| 窗口设置（CASET+RASET+RAMWR = 11 B） | 11 B ⇒ **0.14 µs@80M / 0.28 µs@40M** 线上时间；加 3 次轮询事务与 GPIO 开销，**≈20–60 µs**（推测） | `st7789.c:213-227`；轮询路径 `spi_master.c:1415-1507` |
| CPU 打包 73,728 B | 9,216 源像素 → 18,432 次 32-bit 写；按 8–12 host cycle/源像素 @160 MHz ⇒ **0.46–0.69 ms**（推测，可用 `esp_timer` 包住打包循环校准） | `app_main.cpp:1065-1086` 的映射代码 |
| DMA 73,728 B | **7.37 ms@80 MHz / 14.75 ms@40 MHz**；按实测速率（Q5）**≈14.4–14.8 ms** | 算术 + `dbgtest/r5_final.log:5` |
| **帧内阻塞合计** | **≈0.5–0.75 ms**（打包）+ 上一帧未完成时的等待 `max(0, 14.4 − guest_ms)` | 同上 |
| **回收** | 从 6×2.4 = **14.4 ms** 降到 **≈0.6 ms**（guest 帧 ≥ 14 ms 时等待为 0） | 对比 `app_main.cpp:1089` 的逐带阻塞 |

**一句话**：整窗帧的最短墙上时间 = max(guest 14 ms + 打包 0.5 ms, DMA 14.75 ms) ≈ **14.8 ms**（40 MHz 档）；

换算成帧率：
- 持续大面积动画：14 ms(guest) + 14.4 ms(blit) = **28.4 ms / 35 fps** → max(14.6, 14.8) ≈ **14.8 ms / 60 fps**（被 pacer 封顶）。
- 切页那一帧（R6 实测 guest 73 ms）：88 ms → **73.6 ms**，少 16%。

### 5.5 失败 / 超时 / 半帧怎么恢复

1. `esp_lcd_panel_draw_bitmap` 返回错误（队列满、参数非法）⇒ `s_pending` 不置位，记 `s_blit_errs`，
   置 `s_blit_repaint_all`，下一帧整窗重发。窗口由 CASET/RASET 重设，面板地址计数器自然回到窗口起点。
2. 已入队但 DMA 没完成（回调丢、总线卡死）⇒ 下一帧 `display_wait_transfer(100 ms)` 超时
   ⇒ **先 `xSemaphoreTake(s_trans_done, 0)` 把可能迟到的 give 抽干**，再整窗重发；否则那一笔迟到的 give
   会让下一帧的等待提前返回、覆盖还在被 DMA 读的缓冲。连续两次超时则 `esp_lcd_panel_init(s_panel)`
   重跑厂商序列（`display.c:165-181` 已有）并整窗重发。
3. 半帧：一次 `tx_color` 的三笔事务要么全发完（末笔回调），要么一笔都没有——中间失败只会表现为 (2)，
   不会出现「面板收到半帧而且我们以为它收到了」。即使真收到半帧，下一帧的 CASET/RASET/RAMWR 从窗口首地址重写，
   会把整块覆盖掉。
4. **缓冲生命周期**：`s_frame` 是静态的，且在同一帧内不会再被写（下一次写发生在
   `display_wait_transfer()` 返回之后）。这是 `esp_lcd_panel_io.h:62-63` 要求的全部条件。

### 5.6 对现有 `on_color_trans_done` 的影响

- **回调函数本身不用改**：`display.c:72-84` 已经是 `xSemaphoreGiveFromISR`，而 post_cb 就在 ISR 里跑
  （`spi_master.c:858-872`、`:970-1102`）。
- **触发次数变了**：现在每带一次（一帧 6 次），改后整窗一帧 **1 次**（`io_spi.c:431-435` 只在
  `en_trans_done_cb` 的那笔上回调，即最后一笔）。信号量是二值的，**必须保证「一帧一次 take、一次 give」**，
  否则会丢 give 或提前返回——这就是 5.3 里 `s_pending` 的作用。老路径（逐带）保持「call → take」成对，不受影响。
- `s_trans_done` 的超时语义仍是 `pdMS_TO_TICKS(1000)`（`display.c:219`），只是等待点从「每带末尾」
  挪到「下一帧开头」。
- **混用风险（必须一起改）**：信号量是二值的，不排队。异步帧的 give 如果被菜单/bezel 那条阻塞路径的 take
  吃掉，菜单会以为自己那笔发完了，而它复用的 `s_strip` 还在被 DMA 读。所以**所有** panel 调用（含菜单、
  bezel、`panel_fill_black`）都要在入口先 `display_wait_transfer()`，做到「一次入队一次 take」严格配对。

### 5.7 RAM

| 方案 | 静态占用 | 剩余（88,440 B 起算） | 回收 |
|---|---|---|---|
| 整窗（推荐） | 73,728 B | 14,712 B | ≈14 ms |
| 4 带环 | 49,152 B | 39,288 B | 9.6 ms |
| 2 带环 | 24,576 B | 63,864 B | 4.8 ms |

注意 `s_strip`（15,360 B，`app_main.cpp:385-389`）仍要给菜单用，不能拿它当整窗缓冲。
整窗方案要么加一个静态数组，要么 `heap_caps_malloc(73728, MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL)` 在
`display_init()` 里一次拿死（开机日志最大块 114,688 B，`r5_v114.log:60`，早期拿得到；
但 heap 会被模拟器继续用，**优先静态数组**，链接期就能看到余量）。
**推测**：静态 73.7 KB 能链过，因为 88,440 B 是 1.14.0 构建的剩余 DRAM；但这是全项目最紧的一笔，
必须 `idf.py size` 复核，链不过就退到 4 带环。

### 5.8 被否掉的替代

- **RGB332（COLMOD 0x22）**：缓冲与线上数据都减半（36,864 B、7.4 ms@40 MHz），但把 16 位色压成 256 色。
  **推测**：若整帧唯一色数 ≤ 256 就无损——可以先统计 `s_soc.lcd.fb` 的唯一色数再决定。这是唯一能把
  40 MHz 的 14.8 ms 拉回 7.4 ms 的软件手段。
- **逐带 `tx_color(-1)` 续传**：被 3.2 否掉（每次 acquire 都 drain）。
- **自己调 `spi_device_queue_trans` 并跨帧持总线**：可以，但要自己造 `spi_device_handle_t`、自己管 DC，
  而且 `esp_lcd_panel_io_tx_param` 会在返回时把总线放掉（`io_spi.c:256`），混用会撕裂数据流。
  收益不比 5.3 高，风险大得多，不建议。

---

## 6. Q5：80 MHz 的理论时间与实测带宽

### 6.1 理论

- 12,288 B = 98,304 bit；@80 MHz = **1.2288 ms**；@40 MHz = **2.4576 ms**。
- 73,728 B = 589,824 bit；@80 MHz = **7.3728 ms**；@40 MHz = **14.7456 ms**。

### 6.2 实测折算

| 日志 | 载荷 | blitmax | 折算带宽 | 折合时钟 |
|---|---|---|---|---|
| `r5_final.log:5`（修补扫描带，强制满 6 tile） | 12,288 B | 2,468 µs | **39.8 Mbit/s** | 40 MHz 档 |
| `r5_v114.log:114`（同场景） | 12,288 B 或更窄 | 2,664 µs | 36.9–44 Mbit/s | 37–44 MHz 档 |
| `r5_v114.log:105`（同场景） | 12,288 B 或更窄 | 2,179 µs | 45.1 Mbit/s（若满宽） | 偏快，见下 |
| `spi40.log:41`（320 宽全屏带） | 20,480 B | 4,157 µs | 39.4 Mbit/s | 39 MHz 档 |

修补扫描把一行 tile 全置脏（`app_main.cpp:1031-1041`），所以这一类的带宽是确定的 12,288 B；
2,468 µs 折 39.8 Mbit/s，与 40 MHz 的 2.4576 ms 同档。
同场景其余样本（2,179–2,664 µs）的带**可能不满 6 tile**（`wpx` 取脏 tile 包围盒，`app_main.cpp:1060-1061`），
所以「2,179 µs ⇒ 45 Mbit/s」不能当作真的超过 40 MHz——它更可能是 5 tile 带（10,240 B ⇒ 37.6 Mbit/s）。
**关键事实不是精确值，而是量级：同一台机器上所有样本都在 2.1–2.7 ms，没有任何一笔接近 80 MHz 应有的 1.23 ms。**

**结论：这块板子的 SPI 线上速率是 40 Mbit/s 一档（满宽带 ≈2.4 ms/12,288 B），不是 80 MHz。**
于是「整窗 7.37 ms」不成立，真值是 **14.4–14.8 ms**（保守区间 13–16 ms）。R5 §5.2 的「80 MHz 是真的（用菜单
render 79 ms / total 100 ms 反推）」是一次间接推断，被直接的 `blitmax` 推翻——**直接测量优先于反推**。
另注：2,468 µs 与 40 MHz 的 2.4576 ms 只差 0.4%，但同批样本散布 ±10%，所以不能据此宣称「协议开销为零」；
只能说量级相符。

### 6.3 为什么是 40 MHz（推测，需示波器确认）

- 代码请求 80 MHz（`display.c:31`），IDF 也确实按 80 配寄存器：源 = APB 80 MHz，
  `expected_freq > 3/4 × 源频` ⇒ 走 `clk_equ_sysclk=1` 分支，SCK = 源频（`spi_ll.h:757-770`）。
- 但引脚是 SCLK=GPIO8、MOSI=GPIO9（`display.c:19-20`），**不是 C3 的 SPI2 IOMUX 脚**
  （IOMUX 是 CLK=6、MOSI=7、CS0=10，`soc/esp32c3/include/soc/spi_pins.h:19-25`），
  所以走 GPIO matrix。IDF 官方文档对 ESP32 写得很直白：「Allows signals with clock frequencies only up to
  40 MHz, as opposed to 80 MHz if IO_MUX pins are used」（`spi_master.rst:456`），对 C3 的说法是
  「≤40 MHz 时 matrix 与 IOMUX 表现相同」（`:501`）。**推测**：40 MHz 以上 matrix 不可靠，
  实际到达面板的就是 40 MHz 档（或干脆每两个时钟丢一个边沿）。
- 这段推理有一个说不通的地方必须说清楚：**若外设真的按 80 MHz 移位，DMA 一定 1.23 ms 发完，与实测 2.47 ms 矛盾**；
  所以「寄存器是 80、线上是 40」这个组合只有在时钟源/分频不是我们以为的那一档时才自洽。
  要一次定死，只有两条路：
  1. 上机打一行 `spi_device_get_actual_freq(spi_dev, &khz)`（`spi_master.c:653`，头文件
     `driver/spi_master.h:364`）——但它报的是**分频器算出来的名义值**，不能证明引脚上的波形；
  2. 示波器/逻辑分析仪量 SCLK（或数 1 s 内的 SCK 边沿）。**这是唯一能定案的证据。**
- 不管定案结果如何，**方案不变**：异步重叠的收益只取决于「DMA 总时长 vs guest 帧时长」，
  40 MHz 是 14.8 ms（刚好藏进 14–16 ms 的 guest 帧），80 MHz 是 7.4 ms（富余）。两种都能回收 ~14 ms。

### 6.4 差距在哪

- 若定案是 40 MHz：差距基本都在时钟档位，不在协议开销——最干净的满宽样本 2.468 ms 对 2.4576 ms 只差 0.4%，
  即 **CASET/RASET、CS 翻转、信号量往返加起来只有几十微秒，没有优化空间**（同批 ±10% 的散布来自带宽度与抖动，
  不是可回收的固定开销）。
- 若定案真是 80 MHz：那 1.2 ms 的差额只能来自「每带固定开销 ≈1.2 ms」，而
  `tx_param/tx_color` 的代码里没有任何一处能产生这个量级（轮询事务是微秒级，
  `io_spi.c:240/251/350`），逻辑上不成立——这也是我倾向 40 MHz 的原因。

---

## 7. Q6：两个技术债

### 7.1 `max_transfer_sz = 320*80*2 = 51,200`

- **不超限、不报错**。C3 单笔 DMA 硬上限是 `SPI_LL_DMA_MAX_BIT_LEN = 1<<18 bit = 32,768 B`
  （`hal/esp32c3/include/hal/spi_ll.h:42`），`check_trans_valid` 会拒掉超长事务
  （`spi_master.c:1152`）；但 io 层先把它夹到
  `min(bus_attr->max_transfer_sz, 32768)`（`spi_master.c:1518`），所以永远走不到那个检查。
- 真实代价只是描述符池：`spi_common.c:305-311` 按 `ceil(size/4092)` 各分配 tx/rx 两组描述符，
  51,200 ⇒ (51,200+4091)/4092 = **13** 片 ×12 B ×2 = **312 B**；改 15,360 ⇒ 4 片 ⇒ 96 B。
  **净浪费 216 B**，与 R5 说的「白占描述符池」方向一致，但量级是几百字节，不是问题。
- 建议按 R5 改 15,360（`320*24*2`），理由不是省 RAM，而是**让配置值表达真实意图**：
  一笔最多 32,768 B，写 51,200 会误导下一个读代码的人以为可以一次发 50 KB。

### 7.2 `panel_fill_black()` 要不要整窗重发

- **要，而且必须整屏**。逐帧 blitter 只碰 192×192 窗口（`app_main.cpp:963-967` 的注释就是这条规则），
  窗口外那圈（320×240 减去 192×192）没有任何人重绘，面板上电 GRAM 是随机的，所以开机必须把
  **320×240 全画一遍**。现在的 15 带 × 10,240 B 循环（`app_main.cpp:930-940`）就是干这个的，
  一次性 153,600 B ⇒ 30.7 ms@40 MHz（开机时付，用户看不见）。
- 唯一可优化的是「15 次窗口设置 + 15 次阻塞」：如果哪天有了 ≥153,600 B 的缓冲，可以一次
  `tx_color(RAMWR)` 发完；以现在的 DRAM 余量不值得。**memset 本身（10 KB）不是瓶颈，别动它。**
- 调用点两处（`app_main.cpp:1591`、`:3106`）都在启动/无 ROM 路径，不在活动画面里。

---

## 8. 上机验收清单（设备到手后按顺序做，每步一条命令）

1. **定死时钟**：`display_init()` 里打 `spi_device_get_actual_freq()`（名义值）+ 用示波器量 SCLK；
   或走第 2 步的间接法。
2. **定死单带速率**：`S` 前先触发一次整窗修补（`s_force_full` 已有，`app_main.cpp:1129/3202`），
   读 `blitmax`：≈2.46 ms ⇒ 40 MHz；≈1.23 ms ⇒ 80 MHz。**这一条决定后面所有预算。**
3. **改后 A/B**：同一场景（时钟界面 + 一次 B 切页）对比 `ft p50/p95`、`gmax`、`blit_pct`、
   新增的 `blit_wait_us`（帧首等待耗时）。验收线：整窗帧 `blit_pct` 从 ~10 % 降到 ~1–2 %，
   `ft p95` 不超过 16 ms，`gmax` 不变（guest 不受影响）。
4. **撕裂检查**：让精灵做对角移动，拍照看有没有水平错位——有就说明 `s_pending` 计数错了（提前覆盖缓冲）。
5. **回归**：菜单卡（走老路径）+ 时钟（走老路径）+ 切页（走新路径）三条都要过，`blitmax` 不许超过 3 ms。

---

## 9. 本轮没做的事 / 明确标注的推测

- **没上机**（设备只有 COM1，无 USB），所有「实测」都是仓库日志的二次解读；本轮没有采任何新数据。
- **没改 `firmware/`**：报告是唯一产物。
- **推测项**（正文已逐条标）：窗口设置开销 20–60 µs；打包 0.46–0.69 ms；
  「40 MHz 是 GPIO matrix 限制」的机理；ST7789V 跨 CS 续传的数据手册原文（未取得）；
  73,728 B 静态数组能链过。
- **没做的验证**：RAM 实际余量（要 `idf.py size`）、示波器测 SCLK、真实面板的跨 CS 实验。
