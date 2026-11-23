# v1.6.0 — ROM 分离 + Web 传输工具（2026-11-24）

**里程碑判据：发布镜像里没有 ROM；浏览器（Web Serial）能把 8 MiB 的原机镜像写进 `roms` 分区，能导出/导入 128 KiB 存档；没有 ROM 时设备不再卡死，而是显示提示等上传。**

| 项 | 值 |
|---|---|
| 版本 | `1.6.0`（`firmware/digicore-c3/version.txt`） |
| 日期 | 2026-11-24 |
| 源码 | git tag `v1.6.0` |
| 串口自检 | `VER ver=1.6.0 win=0,18 96x96 scale=2 left=64 top=24 off=0,0 home=64,24 fb=128 chunk=16` |
| app 镜像 | 1,205,696 B（2 MiB 分区，占 57%） |
| ROM 状态 | `sparse v1: image 8388608 B, 2048 pages (611 present)` |
| 存档 | `save round trip identical: True` |

---

## 一、为什么把 ROM 拆出去

原机镜像 8 MiB，app 分区只有 2 MiB。以前的做法是：主机上 `tools/make_sparse_rom.py`
把它压成 2.4 MiB 的稀疏页表，再用 esptool 写到 `0x210000`，整片镜像 `digicore-c3-full.bin`
（4.6 MB）一起烧。

发布版不能这样：

- ROM 是 Bandai 的，仓库和发布包里都不能带（`.gitignore` 早就用 `*.bin` 挡掉了）；
- 用户手上的 ROM 版本不同（V1/V2/V3/20th），发布包里钉死一份没有意义；
- 每次换 ROM 都要装 ESP-IDF + esptool 才能动。

所以 ROM 走数据通道：设备端一套 `DMC1` 协议，网页端在浏览器里做稀疏化。

## 二、协议与实现

完整规范见 `docs/WEB_TOOL.md`。要点：

| 项 | 做法 |
|---|---|
| 稀疏化在哪 | **浏览器**。8 MiB 里 1437 页全零，只传非零的 611 页（2.5 MiB） |
| 设备端写什么 | 按到达顺序写 `data_offset` 起，同时填 bitmap/index，`END` 补 header |
| 与主机工具的关系 | 产出的布局与 `tools/make_sparse_rom.py` 逐字节一致，`rom_esp.cpp` 分不出来 |
| 存档 | 整块 128 KiB 原样搬运，不解析 `saveimg` 的槽格式（少一个解析器就少一处吃存档的地方） |
| 结束动作 | ROM 上传后 300 ms 重启；存档导入后不重启，用户自己决定何时重启 |

设备端 `main/dmc_link.cpp`（约 430 行）+ `main/dmc_link.h`；网页 `tools/dmc_web.html`（单文件）。

## 三、无 ROM 不再是致命错误

以前 `guest_init()` 失败就 `return`，设备静默死机。现在：

```
E ROMSTORE: bad magic ffffffff (expected DMC1)
E DIGICORE: no usable ROM store - waiting for a DMC1 upload
```

屏幕画 `NO ROM / OPEN WEB TOOL`，控制台继续响应，主循环 20 ms 空转；`DMC1 ROM BEGIN`
之后 `dmclink_busy()` 让 guest 全程暂停（不能让它读一个正在被重写的分区），`END`
落盘后重启进游戏。这就是发布版第一次开机的样子。

## 四、实测

```
DMC1 READY 1 loaded 1
image 8388608 B, 2048 pages, 611 non-zero, sha1 ebae139f...
sent in 23.9s (102 KiB/s)            # 2.5 MiB over USB-CDC，含擦除
DMC1 OK END -> restart -> DMC1 READY 1 loaded 1

save export 131072 B
save round trip identical: True
```

## 五、三个坑（全部实测踩到）

1. **`stdout` 会改写二进制**：newlib 的 `CONFIG_NEWLIB_STDOUT_LINE_ENDING_CRLF` 和
   usb_serial_jtag 的 VFS 都把 `0x0A` 变 `0x0D 0x0A`，128 KiB 存档里每个 `0x0A`
   都多一字节，读端从此全错位（第一次测出 `round trip identical: False`，差异是
   "整体后移一字节"）。改走 `usb_serial_jtag_ll_write_txfifo()`，且 header/尾行
   必须同一条路——混用 stdio 会丢顺序（第二次测出尾部 `OK SAVE` 收不到）。
2. **状态日志单行 600+ 字节**：主循环每秒一条 `frames=...`，读端按 200 字节判"回复过长"
   就炸。上限放 2048，并跳过所有非 `DMC1 ` 开头的行。
3. **`esp_partition_erase_range` 要 4 KiB 对齐**：header 区 `0x3100` 不是整数倍，
   传原长度返回 `INVALID_ARG` 且一字节不擦；擦 `0x4000`。

## 六、这一版没做

- **网页烧固件**：仍用 esptool。网页里做 esptool stub + flash 协议是另一个项目。
- **ROM 版本校验**：不检查 V1/V2/V3/20th，只要求 8 MiB、4 KiB 对齐、非零页 ≤ 637。
- **传输时画面**：冻结（flash 擦写禁 cache，不值得为了动画冒险）。
- **SHA-1 校验**：header 里写了 sha1，`rom_esp.cpp` 只打印不校验（同一个 ROM 有多个
  合法来源，硬校验会把用户挡在门外）。

## 七、素材

| 文件 | 大小 | 说明 |
|---|---|---|
| `digicore-c3-v1.6.0.bin` | 1,205,696 B | app 分区镜像，**不含 ROM** |
| `dmc_web.html` | 14,735 B | Web Serial 传输页（与 `tools/dmc_web.html` 同一份） |
| `sha256.txt` | — | 归档件 + 两个可重建产物的指纹 |

整片镜像 4,677,888 B 含 ROM，不入库。

## 八、重建与烧写

```powershell
cd firmware\digicore-c3
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1

$py="C:\Users\DuWeisi\.espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe"
& $py -m esptool --chip esp32c3 -p COM4 -b 460800 write_flash --flash_mode dio `
     --flash_freq 80m --flash_size 8MB 0x10000 build\digicore-c3.bin
```

然后打开 `tools/dmc_web.html`（https 或 localhost），连设备，传 ROM、导存档。

协议回归（不需要浏览器）：`tmp/s33/dmc1_test.py rom|save|hello`。
