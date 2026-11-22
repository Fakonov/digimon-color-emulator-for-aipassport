"""Pixel-perfect window plus a later frame dump.

Two changes:
  * kScale 1 - the guest renders 96x96, so one panel pixel per guest pixel is
    both sharper and eight times cheaper over SPI.  At 2x a full redraw was
    147 KB per frame, which is 15 ms of link time before the guest does any work.
  * dump a frame at guest frame 400 as well, so the host can see what the device
    is actually showing after the auto-press at frame 300.
"""
import io

PATH = "firmware/digicore-c3/main/app_main.cpp"


def main():
    s = io.open(PATH, encoding="utf-8").read()

    old_scale = "static constexpr int kScale = 2;"
    new_scale = "static constexpr int kScale = 1;   // 1:1 with the guest framebuffer"
    if old_scale in s:
        s = s.replace(old_scale, new_scale, 1)
        print("scale -> 1:1")
    elif new_scale not in s:
        raise SystemExit("scale anchor missing")

    old_dump = "if (s_clock_seeded && !s_shot2_done && s_frames >= 130) {"
    new_dump = "if (s_clock_seeded && !s_shot2_done && s_frames >= 400) {"
    if old_dump in s:
        s = s.replace(old_dump, new_dump, 1)
    elif new_dump not in s:
        raise SystemExit("dump anchor missing")

    io.open(PATH, "w", encoding="utf-8", newline="\n").write(s)
    print("dump moved to frame 400")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
