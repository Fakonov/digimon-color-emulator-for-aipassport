"""Add a stuck-frame watchdog to the device build.

The device started to crawl after the pacing change while the host run at the
same slice length stayed healthy, so the fix needs evidence, not another guess:
if one guest frame stops advancing cycles we log the full CPU state once and
skip the rest of that frame instead of hanging.
"""
import io

PATH = "firmware/digicore-c3/main/app_main.cpp"

OLD = """static void run_guest_frame(void)
{
    const int64_t frame_end = s_cycle + kFrameCycles;

    while (s_cycle < frame_end) {"""

NEW = """static void run_guest_frame(void)
{
    const int64_t frame_end = s_cycle + kFrameCycles;
    const int64_t wall0 = esp_timer_get_time();
    int64_t stuck_cycle = s_cycle;
    int64_t stuck_since = wall0;

    while (s_cycle < frame_end) {"""

OLD2 = """        s_prof_tick += t_end - t_tick0;
#endif
    }"""

NEW2 = """        s_prof_tick += t_end - t_tick0;
#endif

        if (s_cycle != stuck_cycle) {
            stuck_cycle = s_cycle;
            stuck_since = esp_timer_get_time();
        } else if (esp_timer_get_time() - stuck_since > 400000) {
            const uint32_t lpc =
                (uint32_t(s_cpu.m_core->m_r[unsp_device::REG_SR] & 0x3f) << 16) |
                uint32_t(s_cpu.m_core->m_r[unsp_device::REG_PC]);
            ESP_LOGW(TAG, "STUCK frame=%u lpc=%06x sr=%04x sp=%04x icount=%d "
                          "slice=%d cycle=%lld",
                     unsigned(s_frames), unsigned(lpc),
                     unsigned(s_cpu.m_core->m_r[unsp_device::REG_SR]),
                     unsigned(s_cpu.m_core->m_r[unsp_device::REG_SP]),
                     int(s_cpu.m_core->m_icount), int(slice),
                     (long long)s_cycle);
            s_cycle = frame_end;
            break;
        }
    }"""


def main():
    s = io.open(PATH, encoding="utf-8").read()
    if "STUCK frame=" in s:
        print("watchdog present")
        return 0
    for old, new in ((OLD, NEW), (OLD2, NEW2)):
        if old not in s:
            raise SystemExit("anchor missing: %r" % old[:40])
        s = s.replace(old, new, 1)
    io.open(PATH, "w", encoding="utf-8", newline="\n").write(s)
    print("watchdog added")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
