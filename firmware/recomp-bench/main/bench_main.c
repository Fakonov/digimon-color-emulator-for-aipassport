/* ESP32-C3 recomp self-test.
 *
 * Runs the two statically translated unSP blocks against the MAME golden RAM
 * snapshots, then measures mcycle around the generated code.  Output is plain
 * UART0 text so the AI Passport wasm simulator can capture it without a board
 * model.  This is a test firmware, not the final emulator.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_cpu.h"
#include "esp_task_wdt.h"

#include "unsp_state.h"
#include "golden_data.h"

void hot_func(State *s);
void delay_loop(State *s);

#define HOT_GUEST_INSNS 68u
#define DELAY_GUEST_INSNS 8003u
#define HOT_BENCH_N 200000u
#define DELAY_BENCH_N 2000u

static int failures;

static void init_hot(State *s, const uint16_t *ram)
{
    memset(s, 0, sizeof(*s));
    memcpy(s->ram, ram, sizeof(s->ram));
    s->r[0] = 0x0FF6;
    s->r[1] = 0x0000;
    s->r[2] = 0x089D;
    s->r[3] = 0x0C86;
    s->r[4] = 0x0000;
    s->r[5] = 0x0FF9;
    s->r[6] = 0x02A0;
    s->r[7] = 0x2002F4;
    s->sb = 0;
}

static void init_delay(State *s, const uint16_t *ram)
{
    memset(s, 0, sizeof(*s));
    memcpy(s->ram, ram, sizeof(s->ram));
    s->r[0] = 0x0FF6;
    s->r[5] = 0x0FF5;
    s->r[6] = 0x0140;
    s->r[7] = 0x5376;
    s->sb = 0;
}

static void report_regs(const char *tag, const State *s)
{
    printf("%s r0=%04X r1=%04X r2=%04X r3=%04X r4=%04X r5=%04X r6=%04X r7=%06X sb=%X\n",
           tag, (unsigned)s->r[0], (unsigned)s->r[1], (unsigned)s->r[2],
           (unsigned)s->r[3], (unsigned)s->r[4], (unsigned)s->r[5],
           (unsigned)s->r[6], (unsigned)s->r[7], (unsigned)s->sb);
}

static int check_hot(const char *tag, const uint16_t *entry, const uint16_t *exit_ram,
                     uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3,
                     uint32_t r4, uint32_t r5, uint32_t r6, uint32_t r7, uint32_t sb)
{
    static State s;
    init_hot(&s, entry);
    hot_func(&s);
    int ok = 1;
    if (memcmp(s.ram, exit_ram, sizeof(s.ram)) != 0) {
        printf("%s RAM MISMATCH\n", tag);
        ok = 0;
    }
    if (s.r[0] != r0 || s.r[1] != r1 || s.r[2] != r2 || s.r[3] != r3 ||
        s.r[4] != r4 || s.r[5] != r5 || s.r[6] != r6 || s.r[7] != r7 ||
        s.sb != sb) {
        printf("%s REG MISMATCH\n", tag);
        report_regs(tag, &s);
        ok = 0;
    }
    printf("%s %s\n", tag, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
    return ok;
}

static int check_delay(void)
{
    static State s;
    init_delay(&s, GOLDEN_DELAY_ENTRY);
    delay_loop(&s);
    int ok = 1;
    if (memcmp(s.ram, GOLDEN_DELAY_EXIT, sizeof(s.ram)) != 0) {
        printf("DELAY RAM MISMATCH\n");
        ok = 0;
    }
    if (s.r[4] != DELAY_R4 || s.r[5] != DELAY_R5 || s.r[6] != DELAY_R6 ||
        s.r[7] != DELAY_R7 || s.sb != DELAY_SB) {
        printf("DELAY REG MISMATCH\n");
        report_regs("DELAY", &s);
        ok = 0;
    }
    printf("DELAY %s\n", ok ? "PASS" : "FAIL");
    if (!ok) failures++;
    return ok;
}

void app_main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("\nRECOMP_BENCH v0 start\n");

    check_hot("HOT_PROFILE0", GOLDEN_HOT_ENTRY, GOLDEN_HOT_EXIT,
              HOT0_R0, HOT0_R1, HOT0_R2, HOT0_R3, HOT0_R4, HOT0_R5, HOT0_R6, HOT0_R7, HOT0_SB);
    check_hot("HOT_PROFILE1", GOLDEN_HOT_ENTRY2, GOLDEN_HOT_EXIT2,
              HOT1_R0, HOT1_R1, HOT1_R2, HOT1_R3, HOT1_R4, HOT1_R5, HOT1_R6, HOT1_R7, HOT1_SB);
    check_delay();

    static State s;
    init_hot(&s, GOLDEN_HOT_ENTRY);
    uint32_t c0 = esp_cpu_get_cycle_count();
    for (uint32_t i = 0; i < HOT_BENCH_N; i++) {
        hot_func(&s);
    }
    uint32_t c1 = esp_cpu_get_cycle_count();
    uint32_t hot_cycles = c1 - c0;
    uint32_t hot_per_call = hot_cycles / HOT_BENCH_N;
    uint64_t hot_guest = (uint64_t)HOT_BENCH_N * HOT_GUEST_INSNS;
    printf("HOT_BENCH n=%u cycles=%u cycles_per_call=%u cycles_per_guest=%u.%02u\n",
           (unsigned)HOT_BENCH_N, (unsigned)hot_cycles, (unsigned)hot_per_call,
           (unsigned)((uint64_t)hot_cycles / hot_guest),
           (unsigned)((((uint64_t)hot_cycles * 100u) / hot_guest) % 100u));

    init_delay(&s, GOLDEN_DELAY_ENTRY);
    c0 = esp_cpu_get_cycle_count();
    for (uint32_t i = 0; i < DELAY_BENCH_N; i++) {
        s.r[4] = 0;
        s.r[5] = 0x0FF5;
        s.r[6] = 0x0140;
        s.sb = 0;
        s.ram[0x0FF5] = 0;
        delay_loop(&s);
    }
    c1 = esp_cpu_get_cycle_count();
    uint32_t delay_cycles = c1 - c0;
    uint32_t delay_per_call = delay_cycles / DELAY_BENCH_N;
    uint64_t delay_guest = (uint64_t)DELAY_BENCH_N * DELAY_GUEST_INSNS;
    printf("DELAY_BENCH n=%u cycles=%u cycles_per_call=%u cycles_per_guest=%u.%02u\n",
           (unsigned)DELAY_BENCH_N, (unsigned)delay_cycles, (unsigned)delay_per_call,
           (unsigned)((uint64_t)delay_cycles / delay_guest),
           (unsigned)((((uint64_t)delay_cycles * 100u) / delay_guest) % 100u));

    printf("SIM_RESULT %s failures=%d\n", failures ? "FAIL" : "PASS", failures);
}
