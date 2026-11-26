// ---------------------------------------------------------------------------
//  wallclock.cpp - NVS anchored wall clock (see wallclock.h)
// ---------------------------------------------------------------------------
#include "wallclock.h"

#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

namespace {

constexpr const char *TAG = "wallclock";
constexpr const char *kNamespace = "digicore";
constexpr const char *kKeyEpoch = "epoch";     // int64, Unix seconds (UTC)
constexpr const char *kKeyTz = "tzmin";        // int16, minutes east of UTC
constexpr int kDefaultTz = 480;                // UTC+8: this deck is sold in CN

// Anchor: epoch at `s_base_us`, so local time == epoch + (now - base) + tz.
int64_t s_epoch = 0;
int64_t s_base_us = 0;
int s_tz_min = kDefaultTz;
const char *s_source = "none";
bool s_live = false;
nvs_handle_t s_nvs = 0;
bool s_nvs_ok = false;

// Days since 1970-01-01 for a civil date.  Howard Hinnant's algorithm; used
// instead of mktime()/TZ so the build timestamp does not depend on the C
// library's timezone state.
int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = unsigned(y - era * 400);
    const unsigned doy = unsigned((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + int64_t(doe) - 719468;
}

int month_from_name(const char *s)
{
    static const char *const kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    for (int i = 0; i < 12; i++)
        if (std::strncmp(s, kMonths[i], 3) == 0)
            return i + 1;
    return 0;
}

// __DATE__ is "Nov 24 2026" and __TIME__ is "23:13:20": both are the build
// host's local time, so the epoch needs the same default offset applied.
int64_t build_epoch()
{
    static const char kDate[] = __DATE__;
    static const char kTime[] = __TIME__;
    int day = 0, year = 0, hh = 0, mm = 0, ss = 0;
    const int mon = month_from_name(kDate);
    if (mon == 0 || std::sscanf(kDate + 4, "%d %d", &day, &year) != 2)
        return 0;
    if (std::sscanf(kTime, "%d:%d:%d", &hh, &mm, &ss) != 3)
        return 0;
    const int64_t days = days_from_civil(year, mon, day);
    return days * 86400 + int64_t(hh) * 3600 + int64_t(mm) * 60 + ss -
           int64_t(kDefaultTz) * 60;
}

int64_t now_us()
{
    return esp_timer_get_time();
}

void nvs_open_once()
{
    if (s_nvs_ok)
        return;
    if (nvs_open(kNamespace, NVS_READWRITE, &s_nvs) != ESP_OK) {
        ESP_LOGW(TAG, "nvs open failed; the clock will not survive a reboot");
        return;
    }
    s_nvs_ok = true;
}

}  // namespace

void wallclock_init()
{
    s_base_us = now_us();
    nvs_open_once();

    int64_t stored = 0;
    int16_t tz = int16_t(kDefaultTz);
    if (s_nvs_ok) {
        if (nvs_get_i64(s_nvs, kKeyEpoch, &stored) != ESP_OK)
            stored = 0;
        if (nvs_get_i16(s_nvs, kKeyTz, &tz) != ESP_OK)
            tz = int16_t(kDefaultTz);
    }
    if (tz < -14 * 60 || tz > 14 * 60)
        tz = int16_t(kDefaultTz);
    s_tz_min = tz;

    if (stored > 1600000000LL) {
        // Best guess after a power cut: the unit resumed at the instant of the
        // last snapshot.  The downtime is unknowable without an RTC; a PC on
        // the console fixes it exactly with `t <epoch>`.
        s_epoch = stored;
        s_source = "nvs";
        s_live = true;
    } else {
        s_epoch = build_epoch();
        s_source = "build";
        s_live = false;
    }
    ESP_LOGI(TAG, "clock source=%s tz=%+d min epoch=%lld",
             s_source, int(s_tz_min), (long long)s_epoch);
}

bool wallclock_live()
{
    return s_live;
}

const char *wallclock_source()
{
    return s_source;
}

void wallclock_set(int64_t epoch_sec, int tz_minutes)
{
    if (epoch_sec < 1600000000LL)
        return;
    if (tz_minutes < -14 * 60 || tz_minutes > 14 * 60)
        tz_minutes = kDefaultTz;
    s_epoch = epoch_sec;
    s_tz_min = tz_minutes;
    s_base_us = now_us();
    s_source = "console";
    s_live = true;
    wallclock_persist();
}

void wallclock_persist()
{
    nvs_open_once();
    if (!s_nvs_ok || s_epoch <= 0)
        return;
    // Never launder the build timestamp into NVS.  Doing that made a guess look
    // authoritative: the next boot read it back as source "nvs", stopped keeping
    // a restored snapshot clock, and corrected the guest to the age of the
    // image.  A build anchor stays a build anchor until something real arrives
    // (console `t`, `DMC1 HELLO`, or a previous real anchor in NVS).
    if (!s_live)
        return;
    const int64_t epoch_now = s_epoch + (now_us() - s_base_us) / 1000000;
    // Re-anchor on every store so the extrapolation never drifts from rounding.
    s_epoch = epoch_now;
    s_base_us = now_us();
    if (nvs_set_i64(s_nvs, kKeyEpoch, epoch_now) != ESP_OK)
        return;
    nvs_set_i16(s_nvs, kKeyTz, int16_t(s_tz_min));
    nvs_commit(s_nvs);
}

// Shared by adopt and track: move the anchor to `local_sec` on the current
// local day.  Both callers then differ only in whether the result is persisted.
void wallclock_adopt_local(int64_t local_sec)
{
    if (local_sec < 0 || local_sec >= 86400)
        return;
    const int64_t epoch_now = s_epoch + (now_us() - s_base_us) / 1000000;
    const int64_t tz_off = int64_t(s_tz_min) * 60;
    const int64_t day = (epoch_now + tz_off) / 86400;      // local day number
    s_epoch = day * 86400 + local_sec - tz_off;
    s_base_us = now_us();
    s_source = "guest";
    // A player-set time is a real source, not the build fallback: it may be
    // persisted, unlike a build anchor (see wallclock_persist).
    s_live = true;
    wallclock_persist();
    ESP_LOGI(TAG, "adopted %02d:%02d:%02d from the guest clock editor",
             int(local_sec / 3600), int((local_sec / 60) % 60), int(local_sec % 60));
}

void wallclock_track_local(int64_t local_sec)
{
    if (local_sec < 0 || local_sec >= 86400)
        return;
    const int64_t epoch_now = s_epoch + (now_us() - s_base_us) / 1000000;
    const int64_t tz_off = int64_t(s_tz_min) * 60;
    const int64_t day = (epoch_now + tz_off) / 86400;
    s_epoch = day * 86400 + local_sec - tz_off;
    s_base_us = now_us();
    s_source = "lapse";
    // Virtual time is still real time for persistence purposes: it may be
    // written back by the next snapshot (wallclock_persist checks s_live).
    s_live = true;
}

int64_t wallclock_local_sec()
{
    const int64_t epoch_now = s_epoch + (now_us() - s_base_us) / 1000000;
    int64_t local = (epoch_now + int64_t(s_tz_min) * 60) % 86400;
    if (local < 0)
        local += 86400;
    return local;
}

void wallclock_hms(int *h, int *m, int *s)
{
    const int64_t t = wallclock_local_sec();
    if (h) *h = int(t / 3600);
    if (m) *m = int((t / 60) % 60);
    if (s) *s = int(t % 60);
}
