"""Toggle the 60 Hz pacer without touching the emulator.

The pacer sits in front of the blit so a slow frame still lands on the LCD at the
handset's refresh rate.  Set ENABLE=False to build a flat-out version, which is
useful as an A/B reference when a change makes the frame loop crawl.
"""
import io

PATH = "firmware/digicore-c3/main/app_main.cpp"
ENABLE = True

BLOCK = """        {
            constexpr int64_t kFrameUs = 1000000 / 60;
            int64_t spent = esp_timer_get_time() - t_guest0;
            if (spent < kFrameUs) {
                const int64_t wait = kFrameUs - spent;
                // Tick granularity is 1 ms, so sleep the bulk and spin the rest;
                // rounding up a whole tick per frame would cost us 3 fps.
                if (wait > 2000) {
                    vTaskDelay(pdMS_TO_TICKS(wait - 1000));
                }
                while (esp_timer_get_time() - t_guest0 < kFrameUs) {
                }
            }
        }
"""


def main():
    s = io.open(PATH, encoding="utf-8").read()
    if BLOCK in s:
        if ENABLE:
            print("pacer already enabled")
            return 0
        s = s.replace(BLOCK, "", 1)
        io.open(PATH, "w", encoding="utf-8", newline="\n").write(s)
        print("pacer removed")
        return 0
    if not ENABLE:
        print("pacer already absent")
        return 0
    anchor = """        const bool content = window_has_content();"""
    if anchor not in s:
        raise SystemExit("anchor missing")
    s = s.replace(anchor, BLOCK + "\n" + anchor, 1)
    io.open(PATH, "w", encoding="utf-8", newline="\n").write(s)
    print("pacer inserted")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
