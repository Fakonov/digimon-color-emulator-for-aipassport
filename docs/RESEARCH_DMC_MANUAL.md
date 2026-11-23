# 彩屏元祖（Digital Monster Color）按键规格

> 来源：humulos.com 的 [Digital Monster Color User Manual](https://humulos.com/digimon/dmc/manual/)
> （社区维护的官方说明书转录，2026-11-23 抓取）。以下简称 DMC。
> 本机（digimon-color-passport）模拟的就是这台机器的 ROM（`digicolr`）。

---

## 1. 三个按键

屏幕右侧自上而下是 **A / B / C**。

| 键 | 官方定义 | 具体行为 |
|---|---|---|
| **A** | Select | 在屏幕上下两排图标之间循环选中 |
| **B** | Confirm | 确认选中的图标或菜单项。**没有图标被选中且不在菜单里时，B 显示时钟** |
| **C** | Cancel | 有图标选中时取消选中；在菜单里时退出菜单。**没有图标选中且不在菜单里时，按住 C 2 秒打开设置菜单**；按更久可以开/关机（关机前有提示音） |

补充：

- **设置菜单的入口是长按 C**，不是 B。菜单四项：Background / Brightness / Sound / Save and Power Off。
- 关机要用设置里的 `Save and Power Off`，不要长按 C 硬关，否则数据可能不保存。
- 上电：长按 C。

## 2. 组合键（重点）

说明书原文（Button Combos）：

> Sometimes you can press two buttons together to create a different effect. The following combos exist:
>
> - **A+C on main screen.** Mutes the device, which can also be done from the settings menu.
> - **A+C on clock screen.** This lets you change the time. Changing time will not speed up growth on Digimon, but you may decide that your Digimon goes to bed later than you like.
> - **A+B on a Grave.** This will allow you to hatch a new Digimon.

| 组合 | 界面 | 功能 | 屏幕上看得见吗 |
|---|---|---|---|
| **A+C** | 主画面 | **静音 / 取消静音** | **看不见**，只能听出来 |
| **A+C** | 时钟画面（B 显示时钟后） | **进入改时间** | 看得见，时间数字可编辑 |
| **A+B** | 墓碑画面（宠物死亡后） | **孵新蛋** | 看得见 |

三条推论：

1. **A+C 在主画面的唯一效果是静音**。屏幕不会有任何变化——所以在主画面按 A+C「看起来没作用」是正常现象，除非你在听声音。
2. **A+C 唯一有可见反馈的场合是时钟画面**。要先让时钟显示出来（主画面无图标选中时按 B），再按 A+C。
3. 墓碑用的是 **A+B**，不是 A+C。

## 3. 与本机实现的对照

| 项目 | 真机（手册） | 本机 |
|---|---|---|
| 主画面 B | 显示时钟 | 同（`0x0C88` 不变，画面右侧出现时间数字） |
| 设置菜单 | 长按 C 2 s（ROM 自己的菜单） | 本机有**自己的**菜单（长按 A，2026-11-23 起），与 ROM 的菜单是两层东西 |
| A+C 注入 | 有功能 | **实测无法触发**，见下 |

### 3.1 本机的 A+C 注入实测

用 `K 5 <n>`（mask 1=A 2=B 4=C）直接注入 `0xFFD7`，在三个画面各测一遍：

| 画面 | 注入 | 结果 |
|---|---|---|
| 标题（`[0x0C88]=0`） | A+C 6 / 20 / 30 帧 | 无变化 |
| LOAD/RESET 菜单（`1`） | 同上 | 无变化 |
| 宠物主画面 / 时钟叠加（`4`） | 同上，以及「先 A 4 帧、再 A+C 24 帧」的分阶段注入 | 画面无变化 |

`port=ffd7` 每次都完整发出并保持了应有的时长，所以不是没送出去，是 ROM 没接。

**可能原因**（未证实，按可能性排序）：

1. **ROM 的 PORTA 解码不接受两位同低**。真机三键在一条电阻梯上，物理上无法同时产生两个接触，ROM 没有必要处理 `0xFFD7`；组合键在真机上是靠**扫描时序**——ROM 快速轮换扫描线，A+C 是在两轮之间被"拼"出来的，不是一个稳定电平。
2. 需要 A 与 C 之间有确定的先后与间隔，我们现在是同时置位。
3. MAME 的 `digicolr` 可能在 ioport 层把同时按下的多个键合并成了某种行为，那与 ROM 真实逻辑不是一回事。

**下一步（如果还要挖）**：反汇编 ROM 里读 PORTA 的那段，看 `0xFFD7` 落在哪个分支；或者用 MAME 加调试输出对照同一次输入的 ROM 行为。

### 3.2 对产品设计的影响

- 「长按 A 弹 A+C 确认卡」这套手势，价值取决于 A+C 是否真能被 ROM 接受。目前实测是**不能**。
- 如果最终确认 ROM 不认稳定电平的组合键，那这个入口应当换掉（改为回标题、立即存档之类），而不是继续保留一个空操作。

## 4. 其它有用的官方规则（供后续实现参考）

| 项目 | 规则 |
|---|---|
| 自动熄屏 | 16 秒无操作（本机已实现） |
| 自动存档 | 每 1 分钟（本机 `AUTO SAVE 60S` 对齐） |
| 开机流程 | 长按 C → splash → **设时间（A 改小时、B 改分钟、C 确认）** → 蛋 → 8 秒孵化 |
| 蛋 | 8 秒后孵化；10 分钟进化到 Stage II |
| 时间 | 主画面显示时钟需要 B；改时间只能靠时钟画面的 A+C |
| 死亡 | 15 次受伤 / 连续受伤 6 小时 / 饥饿或体力空 12 小时 / 进化超时且 5 次以上的照料失误 |
| 墓碑 | 按 **A+B** 孵新蛋 |
| 设置菜单 | 长按 C 2 秒：Background / Brightness / Sound / Save and Power Off |
