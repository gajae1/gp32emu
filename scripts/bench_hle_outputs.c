/* Host HLE output component benchmark. Build each complete source snapshot
 * with identical flags. Guest setup, warmup and whole-RAM validation are
 * outside the clock; no guest instruction executes inside the timed loop. */
#ifndef GP32_CORE_UNDER_TEST
#define GP32_CORE_UNDER_TEST "../src/gp32.c"
#endif
#include GP32_CORE_UNDER_TEST
#include <errno.h>
#include <stdatomic.h>
#include <time.h>

static uint64_t bench_now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) exit(2);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static uint32_t bench_number(const char *s) {
    char *end;
    errno = 0;
    unsigned long n = strtoul(s, &end, 10);
    if (errno || *end || n > UINT32_MAX) exit(2);
    return (uint32_t)n;
}

static void bench_batch(gp32_t *g, uint32_t address, unsigned writer,
                        const char *string, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        if (writer == 0u) direct_write32_if_ram(g, address, i);
        else if (writer == 1u) direct_write8_if_ram(g, address, (uint8_t)i);
        else direct_write_cstr_if_ram(g, address, string);
        /* Materialize repeated host outputs in both implementations. */
        atomic_signal_fence(memory_order_seq_cst);
    }
}

int main(int argc, char **argv) {
    if (argc != 6) {
        fprintf(stderr, "usage: %s writer placement I iterations timed\n", argv[0]);
        return 2;
    }
    unsigned writer = bench_number(argv[1]), placement = bench_number(argv[2]);
    unsigned icache = bench_number(argv[3]), count = bench_number(argv[4]);
    unsigned timed = bench_number(argv[5]);
    if (writer > 3u || placement > 1u || icache > 1u || !count ||
        count > 1000000000u || timed > 1u) return 2;
    gp32_t *g = gp32_create(NULL);
    if (!g) return 2;
    uint32_t size = (uint32_t)s3c2400_ram_size(g->soc);
    arm_bus_t bus = s3c2400_get_bus(g->soc);
    uint8_t *ram = bus.fastmem(bus.user, GP32_RAM_BASE, size, 1);
    uint8_t *expected = malloc(size);
    if (!ram || !expected || size < 0x80000u) return 2;
    memset(ram, 0xa5, size);
    const uint32_t control = GP32_RAM_BASE + 0x10000u;
    const uint32_t code = GP32_RAM_BASE + 0x20000u;
    const uint32_t address = placement ? code + 128u : GP32_RAM_BASE + 0x40000u;
    arm920t_set_jit(g->cpu, 0);
    s3c2400_write32(g->soc, control, 0xee010f10u); /* MCR p15,0,r0,c1,c0,0 */
    arm920t_set_reg(g->cpu, 0u, 0x70u | (icache << 12));
    arm920t_set_reg(g->cpu, 15u, control);
    if (arm920t_run(g->cpu, 1u) != 1u) return 1;
    s3c2400_write32(g->soc, code, 0xe3a00011u);
    s3c2400_write32(g->soc, code + 4u, 0xe12fff1cu);
    arm920t_set_reg(g->cpu, 12u, control);
    for (unsigned warm = 0; warm < 2u; ++warm) {
        arm920t_set_reg(g->cpu, 15u, code);
        if (arm920t_run(g->cpu, 2u) != 2u || arm920t_get_reg(g->cpu, 0u) != 0x11u ||
            arm920t_get_pc(g->cpu) != control) return 1;
    }
    const unsigned bytes = writer == 0u ? 4u : writer == 1u ? 1u : writer == 2u ? 32u : 256u;
    char string[256];
    for (unsigned i = 0; i < sizeof(string); ++i) string[i] = (char)('A' + i % 26u);
    string[bytes - 1u] = '\0';
    bench_batch(g, address, writer, string, 64u);
    memcpy(expected, ram, size);
    uint64_t cycles = arm920t_get_cycles(g->cpu);
    arm920t_reset_cpu_profile(g->cpu);
    uint64_t start = timed ? bench_now_ns() : 0u;
    bench_batch(g, address, writer, string, count);
    uint64_t elapsed = timed ? bench_now_ns() - start : 0u;
    uint8_t *out = expected + (address - GP32_RAM_BASE);
    if (writer == 0u) gp32_st32le(out, count - 1u);
    else if (writer == 1u) *out = (uint8_t)(count - 1u);
    else memcpy(out, string, bytes);
    if (memcmp(ram, expected, size) || arm920t_get_cycles(g->cpu) != cycles ||
        arm920t_get_reg(g->cpu, 0u) != 0x11u || arm920t_get_pc(g->cpu) != control ||
        arm920t_get_cpsr(g->cpu) != 0xd3u) return 1;
    uint64_t invalidations = 0u;
    unsigned profiling = 0u;
#if defined(GP32EMU_CPU_PROFILE) && GP32EMU_CPU_PROFILE
    gp32_cpu_profile_t profile;
    arm920t_get_cpu_profile(g->cpu, &profile);
    profiling = 1u;
    invalidations = profile.jit_inv_cp15_cache;
    if (invalidations) return 1;
#endif
    printf("{\"writer\":%u,\"placement\":%u,\"icache\":%u,\"iterations\":%u,"
           "\"bytes\":%u,\"timed\":%u,\"elapsed_ns\":%" PRIu64 ",\"ns_per_call\":%.9f,"
           "\"verified_bytes\":%u,\"guest_cycles\":0,\"profile_enabled\":%u,"
           "\"invalidations\":%" PRIu64 "}\n", writer, placement, icache, count,
           bytes, timed, elapsed, (double)elapsed / count, size, profiling, invalidations);
    free(expected);
    gp32_destroy(g);
    return 0;
}
