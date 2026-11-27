# 待机画面 STANDBY + 短按 C 取消快进（v1.16.0）

日期：2026-11-27  
基线：v1.15.0 → **v1.16.0**  
触发：用户提出「时间快进改为短按 C 取消」；「machine dark（ROM 发起的待机状态）时画面全黑 + 中间白色字 STANDBY，
去掉原来的 machine dark 提示字，待机状态下按任意键唤醒」。  
性质：状态机改动 + 面板绘制改动 + **上机实测**（COM4，AI Passport，只写 0x10000）。

---

## 0. 结论先说

| 需求 | 实现 | 实测证据（`dbgtest/r6b_*.log`） |
|---|---|---|
| 短按 C 取消快进 | 快进运行中短按 C → 立即回 1x，并把当前 guest 时钟当作新锚点；按键被吞掉不下发 | `C on the lapse notice -> time lapse off (was 60x)`、`time lapse off; clock 20:14:50 taken as the new anchor` |
| 待机全黑 + 白字 STANDBY | 16 s 无输入进入待机：整屏黑底、居中白字（5x7 字模 4 倍，168x28 @ (76,106)） | `standby up after 16002 ms idle` / `16003 ms` |
| 去掉旧提示字 | `MACHINE DARK` banner 管线整段删除（`s_banner_text`、`s_off_notice_done`、`s_notice_until_us`、`menu_banner_present` 调用） | 代码里已无引用 |
| 任意键唤醒 | 待机期间第一下按键只唤醒，不转发给 guest；唤醒后外壳重画 + 整窗重推 | `ACK K 1 8` → `standby off after 3273 ms`；`ACK K 2 8` → `standby off after 1907 ms` |

**一个必须说明的设计决定**：默认 `AUTO OFF = 16S`，如果让「待机卡出现」和「背光熄灭」都落在 16 s，卡片会在同一帧被熄灭，
用户永远看不到。所以背光熄灭时间改成 `max(AUTO OFF, 16 s + 3 s)`：默认设置下卡片能看见 3 s，
`AUTO OFF = 30S` 时能看见 14 s（实测 16.0 s 出卡、30.1 s 熄屏），`NEVER` 则一直亮到按键。

---

## 1. 短按 C 取消快进

原来停快进只有两条路：设置卡里把 `TIME LAPSE` 那行循环回 OFF（三次按键），或者在快进提示条上按 B。
用户要的是「短按 C 取消」，所以：

- **快进提示条在屏上时**：C 停止快进（并收起提示条）；**B 改为只收起提示条**，不再停快进。
  提示条文案随之从 `LAPSE 60X  B=STOP` 改成 `LAPSE 60X  C=STOP`。
- **提示条不在、快进仍在跑**：短按 C 同样停快进，且这一下**不下发**给 guest（否则会顺带取消游戏内的菜单）。
- **没有快进时**：C 仍是原来的游戏取消键，行为不变。

停止动作和设置卡那一行走的是同一条路径：`menu_set_time_lapse_idx(0)` + `sync_time_lapse(true)`，
所以「离开快车道时把 guest 时钟当作新锚点」（`wallclock_adopt_local`）也照旧生效，不会在停下后被时钟同步拽回去。

## 2. STANDBY 待机画面

### 2.1 为什么是「画出来」而不是「熄屏」

ROM 在 16 s 无输入时写 `0x3036` 请求 System_Sleep，而帧循环**一直拒绝**这个请求：它的掉电尾巴跑到了自己代码之外
（`app_main.cpp` 顶部注释、`set_sleep_inhibit(kGuestResumePc)`）。所以「机器暗了」这个状态从来没有被 guest 表达过，
一直是宿主自己模拟的：以前是顶部 3 s 的 `MACHINE DARK` 字 + 到点熄背光。现在改成画一张卡：

```
16 s 无输入 ──► standby_enter():  整屏黑 + 居中白字 STANDBY（背光保持）
                  │
                  ├─ 任意键 ──► standby_leave(): 外壳重画 + 整窗重推 + 背光亮回
                  │
                  └─ idle ≥ max(AUTO OFF, 19 s) ──► 背光熄灭（卡片内容留在 GRAM 里）
```

- 待机期间 **guest 照常运行**（宠物继续按游戏内时间变老），只是 `blit_frame()`、快进提示条、电量角标、FPS 角标全部停止推送。
- 唤醒时 `bezel_paint()` + `s_blit_repaint_all = true`，整幅画面重新推到位，不留残影。
- 待机期间**不进入设置卡**（`menu_is_open()` 时不允许进入待机；反过来待机中第一下按键只唤醒，够不到长 A 手势）。

### 2.2 绘制

`menu_standby_present(strip, 320, 240, "STANDBY")`（`menu.cpp`）：

- 5x7 字模先栅格化成 1x 位图（7 行 × ≤120 列，`static`，不占栈），再按 4 倍放大扫描；
- 320×240 分 10 条带推（每条 24 行，正好是 `s_strip` 的 15,360 B），失败会打 `standby band y=... failed`；
- 文字盒实测 **168×28 @ (76,106)**，即水平垂直都居中。离屏预览：`tmp/r6b_standby_preview.png`（按同一字模/几何渲染）。

## 3. 实测（COM4，v1.16.0，只写 0x10000）

```
VER ver=1.16.0 win=0,18 96x96 scale=2 left=64 top=24 off=0,0 home=64,24 fb=128 chunk=16
I (16984) DIGICORE: standby up after 16003 ms idle      <- 16 s 无输入
ACK K 2 8                                               <- 注入 B（唤醒键）
I (18915) DIGICORE: standby off after 1907 ms
I (21012) DIGICORE: lapse notice up (60X)               <- 控制台 f 2 打开 60x
I (21014) DIGICORE: time lapse 60x (0x0D03=1)
ACK K 4 8                                               <- 注入 C（取消）
I (24007) DIGICORE: time lapse off; clock 20:14:50 taken as the new anchor
I (24008) DIGICORE: time lapse 1x (0x0D03=0)
I (24012) DIGICORE: C on the lapse notice -> time lapse off (was 60x)
```

熄屏时机（`AUTO OFF = 30S` 的机器设置下，34 s 不按键）：

```
I (16975) DIGICORE: standby up after 16002 ms idle
I (31027) DIGICORE: panel dark after 30078 ms idle (auto-off 30 s)
```

稳定态没有退化：`guest_fps` 58.4–59.5、`gmax` 10.4–13.6 ms、`ft` p50/p95 16/16；
待机期间 guest 仍在 58.7 fps 跑（`host_fps` 掉到 19.6 是因为面板不再推送，符合预期）。

## 4. 已知代价

唤醒那一帧是 **72 ms**（`LONGF us=71877`，`hp=0/0/0/0/0`）：其中约 30 ms 是 `bezel_paint()` 重画外壳
（砖墙 + 内框 + 圆角遮罩，153,600 B @ 40 Mbit/s），剩下是 guest 那一帧。一次性，不进入稳态；
要再省就得让待机卡片只覆盖玻璃窗口、外壳保持原样，那是下一轮的事。

## 5. 文件

| 文件 | 改动 |
|---|---|
| `main/menu.h` / `main/menu.cpp` | 新增 `menu_standby_present()`；快进提示条文案 `B=STOP` → `C=STOP`；注释更新 |
| `main/app_main.cpp` | 待机状态机（`s_standby`、`standby_enter/leave`、`kStandbyGraceMs`）、`cancel_time_lapse()`、
|  | 待机期间的推送抑制、删除 banner 管线、C 键取消接入 |
| `VERSION` / `version.txt` | 1.16.0 |
| `CHANGELOG.md` / `README.md` | 本轮的说明与按键表 |

## 6. 回滚

- 只回待机画面：把 `update_sleep_backlight()` 里 `standby_enter()` 那段删掉、删除主循环的 `!s_standby` 判断即可
  （`s_standby` 永远为 false，等价于旧行为；此时背光仍按新公式 `max(AUTO OFF, 19 s)` 熄灭）。
- 只回 C 取消：删掉 `update_buttons()` 里的 C 分支和 `cancel_time_lapse()` 调用，B 在提示条上的原语义需要一并恢复。
- 整版回滚：`git checkout v1.15.0 -- firmware/digicore-c3`，重编后只烧 `0x10000`。
