# Web 传输工具（DMC1 协议）

> 给社区发布用。**固件镜像里不含 ROM**：8 MiB 的原机镜像既装不进 2 MiB 的 app 分区，
> 也不该跟着仓库走。ROM 由用户在本地用网页写进设备的 `roms` 分区，存档在网页上导出/导入。
>
> 设备端：`firmware/digicore-c3/main/dmc_link.cpp`
> 网页端：`tools/dmc_web.html`（单文件，无依赖）

---

## 1. 为什么分离

| 项 | 数值 |
|---|---|
| app 分区 | 0x10000，2 MiB（`digicore-c3.bin` 现在 1,205,088 B，占 57%） |
| roms 分区 | 0x210000，2.5 MiB，存稀疏页表（611 页 × 4 KiB = 2,515,200 B） |
| save 分区 | 0x490000，128 KiB（两槽各 64 KiB） |

原机镜像 8 MiB，稀疏化后 2.4 MiB。整片镜像 `digicore-c3-full.bin`（4.6 MB）含 ROM，
**永远不入库、不随发布分发**（`.gitignore` 的 `*.bin` + `!milestones/**/*.bin` 就是为这个）。

## 2. 发布物清单

| 文件 | 说明 |
|---|---|
| `digicore-c3-v1.6.0.bin` | 只含 app 分区（0x10000），不含 ROM |
| `dmc_web.html` | 网页工具，托管在 https（GitHub Pages / 本地 `http://127.0.0.1`） |
| `README` | 给用户的步骤：烧固件 → 打开网页 → 传 ROM |

网页必须是 **https 或 localhost**（`navigator.serial` 要求 secure context），
Chrome / Edge 桌面版才支持 Web Serial。

## 3. 协议

文本行 + 原始二进制块，全部 `\n` 结尾，回复以 `DMC1 ` 开头。

| 请求 | 回复 | 说明 |
|---|---|---|
| `DMC1 HELLO` | `DMC1 READY <rom_ok> <sha\|-> <save_ok>` | 状态位 |
| `DMC1 ROM BEGIN <img> <pages> <sha1>` | `DMC1 OK BEGIN` | 擦 header 区，进入 ROM 模式 |
| `DMC1 PAGE <index> <len>` + `<len>` 字节 | `DMC1 ACK <done>` | 一页 4096 B，index 是 ROM 页号 |
| `DMC1 END` | `DMC1 OK END` | 写 header/bitmap/index，重挂载，**300 ms 后重启** |
| `DMC1 SAVE GET` | `DMC1 SAVE <size>` + 二进制 + `DMC1 OK SAVE` | 整块 128 KiB 原样 |
| `DMC1 SAVE BEGIN <size>` | `DMC1 OK BEGIN` | 擦 save 分区 |
| `DMC1 PAGE <offset> <len>` + 数据 | `DMC1 ACK <done>` | offset 是字节偏移 |
| `DMC1 END` | `DMC1 OK END` | 落盘 |
| `DMC1 FINISH` | `DMC1 OK FINISH` | 重启进游戏 |

**稀疏化在浏览器里做**：8 MiB 里 1437 页全零，只传非零的 611 页（2.5 MiB）。
设备端按到达顺序把页写进 `data_offset`，同时填 bitmap 和 index，`END` 时补 header
——和 `tools/make_sparse_rom.py` 在主机上生成的布局逐字节一致。

## 4. 实测（COM4，v1.6.0）

```
DMC1 READY 1 loaded 1
image 8388608 B, 2048 pages, 611 non-zero
sent in 23.9s (102 KiB/s)      # 2.5 MiB over USB-CDC
DMC1 OK END -> restart -> DMC1 READY 1 loaded 1

save export 131072 B
save round trip identical: True
```

擦除按 64 KiB 块提前一步做（`ensure_erased`），不会在上传开头卡 20 秒；
`save` 的擦除是整分区一次（128 KiB，4 个块，瞬时）。

## 5. 三个坑（都是这一轮踩的）

1. **`stdout` 会改写二进制**。ESP-IDF 的 newlib（`CONFIG_NEWLIB_STDOUT_LINE_ENDING_CRLF`）
   和 usb_serial_jtag 的 VFS 都会把 `0x0A` 变成 `0x0D 0x0A`，128 KiB 存档里每个 `0x0A`
   后面都多一个字节，读端从此全错位。导出走 `usb_serial_jtag_ll_write_txfifo()`，
   而且 header 行和尾行也必须走同一条路，混用 stdio 会丢顺序。
2. **设备的状态日志单行 600+ 字节**。主循环每秒打一条 `frames=...`，读端如果按 200 字节
   上限判定"回复过长"，第一次握手就会炸。上限放到 2048，并跳过所有不以 `DMC1 ` 开头的行。
3. **`esp_partition_erase_range` 要求 4 KiB 对齐**。header 区是 `0x3100`（12544 B），
   直接传长度会返回 `INVALID_ARG` 且一字节都不擦；擦 `0x4000` 才对。

## 6. 无 ROM 时设备做什么

`guest_init()` 失败不再 `return`：屏幕显示 `NO ROM / OPEN WEB TOOL`，控制台继续响应，
主循环 20 ms 空转。`DMC1 ROM BEGIN` 之后进入 `dmclink_busy()`，guest 全程暂停
（不能让它读一个正在被重写的分区），`END` 落盘后重启进游戏。

这就是发布版第一次开机的样子：烧完 app、还没传 ROM。

## 7. 局限

- **ROM 版本**：不校验具体是 V1/V2/V3 还是 20th，但要求**正好 8,388,608 字节**——
  网页和固件各有一道闸（v1.6.1 起）。放宽成"≤ 8 MiB"会让 64 KiB 的 SoC 内部 ROM 一类文件
  通过：它会以 16 页的"卡带"写进分区，`romstore` 照常挂载，然后 guest 读到卡带末尾之外。
  非零页超过 637 页同样会被拒绝（分区容量）。
- **网页未做固件烧写**：烧 app 仍用 esptool / ESP-IDF。要做一键烧写得在网页里实现
  esptool 的 stub + flash 协议，不在这一版。
- **传输期间画面冻结**：`hardware` 上 SPI flash 擦写会禁 cache，guest 抖一帧就可能跑飞，
  所以直接停住。

---

## 8. 网页里的攻略页（v1.16.0 起）

`tools/dmc_web.html` 不再只是传输工具，还带三个只在前端跑的标签页（不需要连设备）：

| 标签页 | 内容 | 数据来源 |
|---|---|---|
| **图标菜单** | 主画面 + 8 个图标的原机截图与日/英/中对照，设置菜单、睡眠菜单、开机菜单、时钟叠加的实拍 96×96 面板 | MAME `digicolr` 面板截图（Lua 脚本注入 A/B/C 后 `screen:snapshot()`） |
| **进化路线** | Ver.1 全 19 只（含蛋）的 sprite + 阶段分列，点开看下一阶段条件 | humulos DMC dot sprite |
| **进化预测** | 输入当前形态 + お世話ミス / トレーニング / 満腹 / 睡眠妨害，标出命中分支，未命中的给出「还差几次」 | 原版 Ver.1 ROM 实测规则表（14,112 行扫描归纳） |

图片全部内联在 `tools/dmc_assets.js`（约 85 KB，base64 PNG），由 `tools/build_web_assets.py` 生成；
`dmc_web.html` 保持可读，双击即可用（同目录两个文件一起拷走）。

**版权**：截图是 MAME 跑你自己 ROM 得到的面板输出；数码兽点阵图来自 humulos 的 DMC 素材整理，
版权归 Bandai / 本乡昭由 / 东映，这里只作个人游玩参考，不随固件镜像分发。

## 9. 清空存档（不需要改固件）

`save` 分区是 128 KiB 原始镜像，协议里已有的 `DMC1 SAVE BEGIN <size>` + `PAGE` 分块写入就够了：
网页把整块填成 `0xFF`（闪存擦除态）写回去，设备 `END` 后重启，游戏读到空存档就按新机开局。

所以「清空存档」是**纯前端实现**，固件不用动，也不需要新增命令。实测注意：

- 写入 32 个 4 KiB 页，过程与导入存档一致，最后同样走 `DMC1 END`（自带 300 ms 后重启）；
- 动手前一定先「导出存档」备份，否则原来的宠物直接没了；
- 清空后第一次开机会重新走 `ロード / リセット` → 设时间 → 蛋。
