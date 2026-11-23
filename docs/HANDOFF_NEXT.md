# 交接：接手后的状态（2026-11-23 22:10）

先说结论：**"进不去游戏"解决了。根因是 PORTA 按键极性被改反，已改回 v1.0.0 的低有效。**
复核过程见 `docs/AUDIT_2026-11-23.md`，那份文档同时列了上一任的方法论错误和我自己的两次翻车。

---

## 1. 当前固件（已烧在设备上）

| 项 | 状态 | 判据 |
|---|---|---|
| 冷启动画面 | ✅ logo | `FB_CRC 62C4882E` |
| 短按 A | ✅ 进 LOAD/RESET 菜单 | `FB_CRC BA73E2DD`，`[0x0C88]=1` |
| 短按 B | ✅ 光标移动 | `FB_CRC 09605E02` |
| 短按 C / A 连按 | ✅ 到宠物主画面 | `[0x0C88]=4`，见 `tmp/s28/fb_state4.png` |
| 长按 A 1.2 s | ✅ **弹确认卡，不向 ROM 发任何键** | 日志 `long A -> A+C confirm card` |
| 卡片按 B | ✅ 发 A+C（`0xFFD7`）3.2 s | 日志 `chord card: B -> A+C sent` |
| 卡片按 C | ✅ 取消 | 日志 `chord card: cancelled` |
| 设置菜单（长按 B 1.5 s） | ✅ 保留，且不会再被卡片确认误触 | `GEST ... menu=0` |
| 自动存档 / 读档 | ✅ 未改动 | — |

**极性是唯一的关键**：`bits &= ~kPortB1` 表示按下，A 按下时 PORTA 读 `0xFFF7`。**不要**改回高有效。

---

## 2. 为什么之前"只有按键音"

三层原因叠在一起：

1. **极性反了（上一任）**：ROM 收到的是"所有键都松开 + 一个不存在的位"，单键全废。
2. **确认卡的 B 键同时触发长按 B 菜单（我）**：设备一帧 60–300 ms，注入 30 帧 = 10 s，远超 1.5 s 的菜单阈值。卡片一确认，设置菜单就打开并 park 掉 guest，之后所有按键都被菜单吃掉。修法：`s_b_hold_locked`，B 松开前不参与长按判定。
3. **状态字卡住**：`menu=1` 时 `update_buttons` 提前 return，`s_ac_active` 也就不会过期，看起来像"A+C 一直按着"。菜单不再被误触后这条自动消失。

---

## 3. 调试入口（下次直接用，别再猜）

```
S  →  STAT ...                                 帧率/pc/guest_pct
      IRQ isr=… rise=… set2=… clr2=… sirq=…  中断链是否真的在投递
      GEST ac=… fired=… adown=… blank=… card=… menu=… scr=… pad=…
                                              手势状态机，一眼看出卡片/菜单卡在哪
O  →  IRQ7 handler 逐 PC 直方图（0x486B..0x48AF）+ 帧节拍写入计数
N <n> → 抓 n 个 band 的菜单/确认卡像素（c 开头的行是确认卡，B 开头是设置菜单）
```

**结论先说：中断链是好的，别再动它。** `rise/fall` 相等、`set2/clr2` 相等就说明 64 Hz 边沿和 IRQ7 都正常。`[0x0850]` 在 host 上同样是 0，它不是判据。

---

## 4. 还没做的

### 4.1 16 s 空闲超时后的 park 点

ROM 16 s 无键后写 `0x3036`，host 拒绝睡眠并 park 到 `0x200300`。若此时还在标题页，`[0x0C88]` 会跳到 `0x1C`，按键失效。试过的 park 点：`0x200300`（状态错乱）、`0x230321`（不采样端口）、`0x5240`（立刻退出循环）、`0x5230`（能采键但后续仍走 sleep）。**最好的下一步是让 ROM 永不写 `0x3036`，或者 park 的同时清掉它的空闲计时器。**

### 4.2 guest 只有 ~27% 实时

`insns` 5.7 M/s、AOT 命中率约 12%。一帧 60–300 ms，所有"按住 N 秒"的判定都要乘 3.7。**这条不解决，时序类问题会反复回来。**

### 4.3 确认卡外观

`menu_confirm_present()` 已能用 `N` 命令抓下来（`tmp/s28/card_capture.png`），但还没上机目视核对边框和字距。

---

## 5. 工具速查

```powershell
# 构建（不烧写）
cd firmware\digicore-c3; powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1

# 刷整片
$py="C:\Users\DuWeisi\.espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe"
& $py -m esptool --chip esp32c3 -p COM4 -b 460800 write_flash --flash_mode dio `
     --flash_freq 80m --flash_size 8MB 0x0 firmware\digicore-c3\digicore-c3-full.bin

# 擦快照（保住 saveimg）
& $py -m esptool --chip esp32c3 -p COM4 erase_region 0x490000 0x18000

# 验收脚本
& $py tmp\s28\final_accept.py    # 短按 A 进游戏 + 长按 A 弹卡 + B 发 A+C
& $py tmp\s28\lowactive.py       # 按键矩阵（低有效）
& $py tmp\s28\card_capture.py    # 抓确认卡像素
& $py tmp\dump_fb.py COM4 x.png "8;K 4 40;5;K 4 40;6"   # 带命令序列抓帧

# 串口命令
#   Z <addr>      读 RAM/IO/flash       K <mask> <frames>  注入按键（1=A 2=B 4=C）
#   C             FB_CRC                O                  IRQ handler 直方图
#   S             统计 + IRQ + GEST     T                  dump 可见区
#   N <n>         抓菜单/确认卡像素     D / U              存 / 恢复快照
#   E <left> [top] 画面落点在面板上的绝对坐标（现场校准，不用重烧）
#   G 0..3        诊断图：1=fb 框 2=面板四边 3=对齐靶（刻度尺+绿框+中心十字）
```

**注意**：`H <ms>` 是长按 A 阈值，`H 0` 不是"关闭"而是"一按下就触发"。

---

## 6. 交付清单（v1.1.0 / commit `2c6b33b`）

| 类别 | 路径 | 大小 |
|---|---|---|
| 可烧写 | `firmware/digicore-c3/digicore-c3-full.bin` | 4,677,888 B（22:07） |
| 可烧写 | `firmware/digicore-c3/build/digicore-c3.bin` | 1,174,128 B（app 分区镜像） |
| 源码 | `firmware/digicore-c3/main/app_main.cpp` | 2,286 行 / 111,126 B |
| 源码 | `firmware/digicore-c3/main/menu.cpp` | 636 行 / 26,936 B |
| 源码 | `firmware/digicore-c3/main/menu.h` | 79 行 / 4,270 B |
| 源码 | `firmware/digicore-c3/main/emu/gpce4.cpp` | 1,354 行 / 61,767 B |
| 源码 | `firmware/digicore-c3/main/emu/unsp_glue.cpp` | 1,663 行 / 63,103 B |
| 文档 | `docs/AUDIT_2026-11-23.md` | 105 行 / 8,563 B（审核报告） |
| 文档 | `docs/HANDOFF_NEXT.md` | 本文 |
| 证据 | `tmp/s28/fb_state4.png` / `fb_pet.png` / `card_capture.png` | 抓帧，tmp 不进库 |

**版本号单一来源**：`firmware/digicore-c3/version.txt`（当前 `1.1.0`）。
**注意**：只改 `version.txt` 不会让固件重新带上新版本号——`file(READ ...)` 在 CMake 配置阶段执行。要么 `build.ps1 -Clean`，要么碰一下 `main/CMakeLists.txt` 的时间戳强制重配。踩过一次：bin 里一直是 `1.0.0`，`V` 也报 `1.0.0`。

