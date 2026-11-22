#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "unsp_state.h"

#define HOT_GUEST_INSNS 68u
void hot_func(State *s);

#if defined(__x86_64__) || defined(__i386__)
static inline uint64_t host_cycles(void) {
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
#else
static inline uint64_t host_cycles(void) { return 0; }
#endif

static int load_words(const char *path, uint16_t *dst, size_t words) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return 0; }
    size_t n = fread(dst, 2, words, f);
    fclose(f);
    if (n != words) { fprintf(stderr, "%s: short read %zu/%zu\n", path, n, words); return 0; }
    return 1;
}

static int write_words(const char *path, const uint16_t *src, size_t words) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return 0; }
    size_t n = fwrite(src, 2, words, f);
    fclose(f);
    if (n != words) { fprintf(stderr, "%s: short write %zu/%zu\n", path, n, words); return 0; }
    return 1;
}

static void init_state(State *s, const uint16_t *ram) {
    memset(s, 0, sizeof(*s));
    memcpy(s->ram, ram, sizeof(s->ram));
    s->r[0] = 0x0FF6; /* SP */
    s->r[1] = 0x0000;
    s->r[2] = 0x089D;
    s->r[3] = 0x0C86;
    s->r[4] = 0x0000;
    s->r[5] = 0x0FF9; /* BP */
    s->r[6] = 0x02A0; /* SR */
    s->r[7] = 0x2002F4; /* PC */
    s->sb = 0;
}

static void dump_state(const State *s) {
    printf("r0/sp=%04X r1=%04X r2=%04X r3=%04X r4=%04X r5/bp=%04X r6/sr=%04X r7/pc=%06X sb=%X\n",
        s->r[0], s->r[1], s->r[2], s->r[3], s->r[4], s->r[5], s->r[6], s->r[7], s->sb);
    printf("ram[0600..0607]=");
    for (int i = 0x600; i <= 0x607; i++) printf("%04X%s", s->ram[i], i == 0x607 ? "\n" : " ");
    printf("stack[0FF3..0FF5]=%04X %04X %04X\n", s->ram[0x0FF3], s->ram[0x0FF4], s->ram[0x0FF5]);
}

int main(int argc, char **argv) {
    const char *entry_path = argc > 1 ? argv[1] : "dbgtest/entry_ram.bin";
    const char *exit_path  = argc > 2 ? argv[2] : "dbgtest/exit_ram.bin";
    const char *out_path   = argc > 3 ? argv[3] : "recomp/out_ram.bin";
    int profile = argc > 4 ? atoi(argv[4]) : 0;

    uint16_t entry[0x1000], expected[0x1000];
    if (!load_words(entry_path, entry, 0x1000)) return 1;
    if (!load_words(exit_path, expected, 0x1000)) return 1;

    State s;
    init_state(&s, entry);
    printf("ENTRY "); dump_state(&s);

    uint64_t t0 = host_cycles();
    hot_func(&s);
    uint64_t t1 = host_cycles();

    printf("EXIT  "); dump_state(&s);
    printf("single-call host cycles: %llu (%.3f cycles/guest-insn, %u guest insns)\n",
        (unsigned long long)(t1 - t0),
        (double)(t1 - t0) / (double)HOT_GUEST_INSNS,
        (unsigned)HOT_GUEST_INSNS);

    if (!write_words(out_path, s.ram, 0x1000)) return 1;
    int ram_ok = memcmp(s.ram, expected, sizeof(s.ram)) == 0;
    int regs_ok;
    if (profile == 1) {
        /* 10 s snapshot: same entry registers, different RAM contents. */
        regs_ok = (s.r[0] == 0x0FF5 && s.r[1] == 0xEE2E && s.r[2] == 0xCB76 &&
                   s.r[3] == 0x588B && s.r[4] == 0x0601 && s.r[5] == 0x0FF3 &&
                   s.r[6] == 0x0020 && s.r[7] == 0x200343 && s.sb == 0xC);
    } else {
        regs_ok = (s.r[0] == 0x0FF5 && s.r[1] == 0x90F0 && s.r[2] == 0x1F30 &&
                   s.r[3] == 0xC08D && s.r[4] == 0x0601 && s.r[5] == 0x0FF3 &&
                   s.r[6] == 0x0020 && s.r[7] == 0x200343 && s.sb == 1);
    }
    printf("RAM vs %s: %s\n", exit_path, ram_ok ? "PASS" : "FAIL");
    printf("registers (profile %d): %s\n", profile, regs_ok ? "PASS" : "FAIL");

    /* Throughput: the translated block has no branches, so repeated calls
       execute the same host path; RAM contents only change operands. */
    const int N = 2000000;
    State b;
    init_state(&b, entry);
    clock_t c0 = clock();
    uint64_t h0 = host_cycles();
    for (int i = 0; i < N; i++) hot_func(&b);
    uint64_t h1 = host_cycles();
    clock_t c1 = clock();
    double sec = (double)(c1 - c0) / (double)CLOCKS_PER_SEC;
    printf("bench %d calls: %.3f s, %.2f M calls/s, %.2f host cycles/call\n",
        N, sec, (double)N / sec / 1e6, (double)(h1 - h0) / (double)N);
    printf("per-call guest rate equivalent: %.2f M guest insn/s if host ran at this speed (reference only)\n",
        ((double)N * HOT_GUEST_INSNS) / sec / 1e6);
    return (ram_ok && regs_ok) ? 0 : 2;
}