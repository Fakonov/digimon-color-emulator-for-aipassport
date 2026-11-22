"""Deadline based 60 Hz pacer.

Sleeping "the remainder of the frame" each iteration accumulates error: one
FreeRTOS tick is 1 ms and vTaskDelay() can overshoot to the next boundary, which
cost 3 fps.  Instead keep an absolute deadline that advances by exactly one
frame, sleep the bulk of the wait, spin the last 2 ms, and resynchronise if the
loop ever falls a whole frame behind.
"""
import io

PATH = "firmware/digicore-c3/main/app_main.cpp"

OLD_START = "// Pace to the handset's frame rate."
OLD_END = "const bool content = window_has_content();"

NEW = """// Pace to the handset's frame rate.  The model can run a 60 Hz frame faster
        // than 60 times a second, and letting it run flat out would make the
        // guest age too fast (the pet's clock is the guest's own).  The deadline
        // is absolute and advances by exactly one frame, so tick quantisation
        // cannot accumulate: sleep the bulk, spin the last 2 ms.
        {
            constexpr int64_t kFrameUs = 1000000 / 60;
            static int64_t s_deadline_us = 0;
            const int64_t now_us = esp_timer_get_time();
            if (s_deadline_us == 0) {
                s_deadline_us = now_us + kFrameUs;
            } else {
                s_deadline_us += kFrameUs;
                if (s_deadline_us + kFrameUs < now_us) {
                    s_deadline_us = now_us;  // fell behind: resynchronise
                }
            }
            const int64_t wait = s_deadline_us - esp_timer_get_time();
            if (wait > 2000) {
                vTaskDelay(pdMS_TO_TICKS((wait - 2000) / 1000));
            }
            while (esp_timer_get_time() < s_deadline_us) {
            }
        }

        """


def main():
    s = io.open(PATH, encoding="utf-8").read()
    i = s.find(OLD_START)
    if i < 0:
        raise SystemExit("pacer anchor missing")
    j = s.find(OLD_END, i)
    if j < 0:
        raise SystemExit("pacer end anchor missing")
    s = s[:i] + NEW + s[j:]
    io.open(PATH, "w", encoding="utf-8", newline="\n").write(s)
    print("deadline pacer installed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
