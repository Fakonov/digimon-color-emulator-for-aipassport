# v1.11.1 — 时钟编辑重新生效

**里程碑判据：控制台一次性重放 ROM 的时钟提交（`y 9 41 30`），客人时钟变成 09:41:30 且宿主锚点采纳它（`src=guest`），31 秒后仍在走；随后 `t` 能恢复真实时间。**

| 项 | 值 |
|---|---|
| 版本 | `1.11.1`（`firmware/digicore-c3/version.txt`） |
| 日期 | 2026-11-25 |
| 报告 | `docs/AUDIT_SAVE_CLOCK_2026-11-25_R5.md` |
| app 镜像 | `digicore-c3-v1.11.1.bin` 1,016,960 B |
| SHA256 | 见 `sha256.txt` |
| 烧写 | 只写 `0x10000`，`save` 与 `nvs` 保留 |

## 一、问题

时钟界面 A+C 打开 ROM 自己的时间编辑，改完按 C，时间又跳回去——v1.10.1 删掉“采纳玩家时间”后，
`clock_sync()` 每 250 ms 把客人时钟纠回宿主锚点，玩家的提交活不过 250 ms。

## 二、修法

用**硬件写操作**识别提交，而不是猜 RAM 跳变：

- ROM 的提交函数（flash `0x204179`）是唯一在 `0x0CB0` 已置 1 之后写 `Timebase_Clear(0x3033)` 的地方；
  启动路径（irom `0x42BF`/`0x586B`）也写，但时钟是关的，`0x0CB0 == 0` 时不采纳。
- `timebase_clear_w()` 置标志，`clock_sync()` 消费：把客人的 `hh:mm:ss` 收为新锚点。
- 种子改成“最多试 4 次”：ROM 的时间设置界面每帧清 `0x0CB0`，硬写每帧对打会让画面卡在 logo
  （实测），所以失败就交还给玩家 + 采纳路径；同时种子从第 300 ms 起每帧写、直到活下来，
  正常开机 1.3 s 内打印 `clock running … (nvs)`，早于玩家能按到 CONTINUE。

## 三、验证

```
y 9 41 30                  → ACK y 09:41:30
CLK guest=09:41:30 wall=09:41:31 dev=1 steps=2 holds=21 seed=1 tbw=2 src=guest en=1
（31 s 后）CLK guest=09:42:01 wall=09:42:01 dev=0 src=guest   ← 自己走了，没被纠回
t <epoch>                  → ACK t 01:35:21 src=console，offset 0 s
```

新增测试钩子：`y <hh> <mm> <ss>`（原子重放提交）、`j <addr> <val>`（写任意地址）。

## 四、玩家仍要按的一步

关机重开：标题 → A+C → CONTINUE/RESET → CONTINUE → **时间界面** → C。
固件在 300 ms–1.3 s 内已把 RTC 设好，界面里的暂存值取自 RTC，所以直接按 C 即可；调整过的时间会被采纳。
这一步是 ROM 没有 RTC 的设计，固件不碰 ROM 绕不过去。

## 五、回滚

```
git checkout v1.11.0 -- firmware/digicore-c3
powershell -NoProfile -ExecutionPolicy Bypass -File firmware\digicore-c3\build.ps1 -NoSparse
python -m esptool --chip esp32c3 -p COM4 -b 460800 write_flash 0x10000 firmware\digicore-c3\build\digicore-c3.bin
```
