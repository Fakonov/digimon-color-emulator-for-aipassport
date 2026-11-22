"""Append the 60 fps milestone to the project logs (UTF-8, append only)."""
import io

PROGRESS = """

## 2026-11-20 22:5x — 设备侧 24.5 → 60.00 fps（锁定 60 帧）

- 先量：`guest_pct=58 / blit_pct=40`（14.15 fps 时）。上一轮「skip blit 无变化」的结论只在 7.6 fps
  配置下成立，作废。
- blit 重写（`tools/patch_blit.py`）：只发 192 宽窗口（省掉 40% 永不变化的黑边）、脏行跳过
  （`s_prev[]` 逐行比较）、32 位双像素写入（`pair = sw | sw<<16`）→ **24.5 fps**、blit 降到 3%。
- 削 guest 侧固定开销四步：collapse 每圈只读一次总线（1,260 → 245 主机周期/圈）、
  `kSliceCycles` 2,000 → 48,000、300 kHz SPI2 驱动源默认不排事件、被屏蔽的 `timer_a`
  不再排事件（`source_enabled()`）→ **79.7 fps**（帧 12.5 ms）。`tick()` 从 42 M 周期/秒降到 3.0 M。
- 限速：79.7 fps 会让 guest 以 1.33 倍速变老。第一版把微秒当毫秒传给 `pdMS_TO_TICKS()`
  → 每帧睡 3.1 秒、设备 0.16 fps；修单位后 57.3 fps（tick 向上取整吃掉 0.7 ms/帧）。
  最终用绝对截止时间（`tools/patch_pacer_deadline.py`：睡 `wait-2 ms`、最后 2 ms 忙等、
  落后整帧才重同步）→ **60.00 fps 稳定**。
- 验证：连续 60 s soak（`dbgtest/final60.log`）3,116 帧全部 60.00、无 panic/STUCK；
  第 60 帧 dump 与 PC 参考帧 **0/9216 像素不同**；app 382,992 B（sha256 1BC10952…）。
- 新增工具：`tools/patch_blit.py`、`patch_wait_collapse.py`、`patch_collapse_hoist.py`、
  `patch_masked_timer.py`、`patch_pacer_deadline.py`、`patch_stuck_watchdog.py`、
  `patch_slice_knob.py`、`port_wait_loop.py`、`add_collapse_stats.py`。
"""

HANDOFF = """## 最新（2026-11-20 22:5x）— 设备侧锁 60 fps

- COM4 已刷 `firmware/digicore-c3/build/digicore-c3.bin`（382,992 B，sha256 前 16 位 `1BC109527C2F55E4`，
  只刷 app 区 0x10000，ROM 分区未动）。实测 **60.00 guest fps** 连续 60 秒，`blit_pct=3`。
- 速度阶梯：1.09 → 7.82（重编译 PRNG 块）→ 14.15（等待循环折叠）→ 24.5（blit 重写）→ 79.7（切片/
  定时器三项）→ 60.00（绝对截止时间节拍）。细节见 `docs/C3_PORT.md` §8.2.3。
- 设备仍有的限制：时钟靠编译时间播种（无 RTC）、音响未接（游戏不发声）、按键一次生效需 5 帧拉伸。
"""


def append(path, text, mode="a"):
    with io.open(path, mode, encoding="utf-8", newline="\n") as fp:
        fp.write(text)
    print("updated", path)


def prepend_after_title(path, block):
    s = io.open(path, encoding="utf-8").read()
    marker = "\n## "
    i = s.find(marker)
    if i < 0:
        raise SystemExit("no section marker in " + path)
    s = s[: i + 1] + block.rstrip("\n") + "\n" + s[i + 1 :]
    io.open(path, "w", encoding="utf-8", newline="\n").write(s)
    print("prepended", path)


def main():
    append("PROGRESS.md", PROGRESS)
    prepend_after_title("docs/HANDOFF.md", HANDOFF)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
