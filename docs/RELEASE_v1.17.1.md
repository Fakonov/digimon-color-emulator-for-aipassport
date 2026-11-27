# v1.17.1 — 设置卡去掉重复的 VERSION 行

## 变更

设置卡的标题栏右侧本来就有一枚灰色版本标签（`SETTINGS` 旁边），最后一行 `VERSION` 又把同一个数字
写了一遍。删掉这一行：

- 卡片从 212 px 收到 **192 px**，七行：SEND A+C / SEND A+B / TIME LAPSE / AUTO OFF / BRIGHT /
  VOLUME / AUTO SAVE。
- 版本号只在右上角显示；`MENU_ITEMS` 8 → 7，`MENU_IDX_ABOUT` 删除，
  `label_for()` / `name_for()` 的默认分支不再打印版本。
- 行高、字号、行距不变，最后一行底部 183，距边框 9 px。

## 数字

| 指标 | v1.17.0 | v1.17.1 |
|---|---|---|
| app 镜像 | 1,134,128 B | **1,133,760 B**（−368） |
| 设置卡行数 | 8 | **7** |
| 设置卡高度 | 212 px | **192 px** |
| 稳定态 fps / 帧间隔 | 56.4–56.7 / 16 ms | 不变 |

## 验证

真机（COM4）长按 A 打开设置卡，用 `N` 命令把面板收到的像素回读到串口，解码成 320×240：
`SETTINGS` 标题、右上角 `1.17.1`、七行内容、**没有 VERSION 行**；B 键切换选中行、C 键关闭正常。
`V` 报 `ver=1.17.1`。

## 烧写

app 分区镜像，只写 `0x10000`，`save` 与 `nvs` 保留：

```
esptool --chip esp32c3 -p COM4 -b 460800 write_flash 0x10000 digicore-c3-v1.17.1.bin
```

## 报告

- `docs/AUDIT_PERF_2026-11-27_R7.md`（第七轮审计，v1.17.0 的主体改动）
- `docs/RELEASE_v1.17.0.md`（上一版发布说明）
