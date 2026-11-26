# ROM 镜像（不随仓库分发）

`roms/*.bin` 是 Bandai **Digital Monster Color**（彩屏元祖）的 8 MiB 卡带镜像，
**版权不属于本项目**，因此不进仓库，也不在 GitHub 发布包里。

## 你需要自己准备

1. 从你自己的彩屏元祖读出镜像（或使用你合法持有的副本）；
2. 得到一个 **8 MiB（8,388,608 字节）** 的 `.bin`，文件名随意；
3. 刷完固件后屏幕会显示 `NO ROM`，用网页把镜像传进去：
   - 打开 `tools/dmc_web.html`（需 https 或 `http://127.0.0.1`，Chrome / Edge）
   - 连接设备 → 选 `.bin` → 上传 ROM → 设备自动重启进游戏

网页会在浏览器里做稀疏化：8 MiB 里 1,437 页是全零，只传有数据的 611 页（约 2.5 MiB，实测 24 秒）。

## 命令行等价操作

网页不是必须的，`DMC1` 协议本身是文本 + 二进制帧，`tools/dmc1_test.py` 是它的 Python 实现：

```bash
python tools/dmc1_test.py hello      # 状态
python tools/dmc1_test.py rom        # 上传（默认读 roms/dmc_v1_base.bin）
python tools/dmc1_test.py save       # 存档导出 → 写回 → 比对
```

## 版本差异

彩屏元祖有 V1 / V2 / V3 / 20th 等多个批次，固件**不校验**具体版本：只要求 8 MiB、4 KiB 对齐、
非零页不超过 637 页（`roms` 分区容量）。换镜像不需要重新编译。
