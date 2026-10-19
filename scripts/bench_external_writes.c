/* Paired component benchmark. Reuse the independent fixture, compile each
 * complete CPU implementation into this TU with identical flags, and verify
 * guest work and every RAM byte outside the timed region. No game/FPS claim. */
#define main external_write_oracle_main
#include "../tests/arm_external_write_test.c"
#undef main
#include <errno.h>
#include <time.h>
#include <stdatomic.h>

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

static uint64_t bench_batch(arm920t_t *cpu, unsigned scenario, uint32_t count) {
    const uint32_t code = ARM_JIT_RAM_BASE_ADDR + 0x2000u;
    uint32_t data = code + (scenario == 2u ? 0x800u : scenario == 3u ? 0x1000u : 8u);
    uint32_t notified = scenario == 4u ? code : data;
    uint32_t bytes = scenario == 2u ? 128u : 4u;
    uint64_t retired = 0;
    for (uint32_t i = 0; i < count; ++i) {
        write32(NULL, data, i);
        /* Keep the host store materialized even in the no-notification
         * control. This is a compiler barrier, not a host memory fence. */
        atomic_signal_fence(memory_order_seq_cst);
        if (scenario) arm920t_invalidate_icache_range(cpu, notified, bytes);
        arm920t_set_reg(cpu, 0u, 0x66u);
        arm920t_set_reg(cpu, 15u, code);
        retired += arm920t_run(cpu, 2u);
    }
    return retired;
}

int main(int argc, char **argv) {
    if (argc != 6) {
        fprintf(stderr, "usage: %s native I scenario iterations timed\n", argv[0]);
        return 2;
    }
    unsigned native = bench_number(argv[1]), icache = bench_number(argv[2]);
    unsigned scenario = bench_number(argv[3]), count = bench_number(argv[4]);
    unsigned timed = bench_number(argv[5]);
    if (native > 1u || icache > 1u || scenario > 4u || !count || count > 1000000000u || timed > 1u)
        return 2;
    arm920t_t *cpu = fixture(native, icache, 0x2000u, 0);
    if (!cpu || failures) return 1;
    if (native && !arm920t_get_jit_enabled(cpu)) return 2;
    (void)bench_batch(cpu, scenario, 64u);
    arm920t_reset_cpu_profile(cpu);
    uint64_t cycles = arm920t_get_cycles(cpu);
    uint32_t epoch = cpu->jit_cache_epoch;
    uint64_t start = timed ? bench_now_ns() : 0u;
    uint64_t retired = bench_batch(cpu, scenario, count);
    uint64_t elapsed = timed ? bench_now_ns() - start : 0u;
    if (retired != 2u * (uint64_t)count || arm920t_get_cycles(cpu) != cycles + retired ||
        arm920t_get_reg(cpu, 0u) != 0x11u || arm920t_get_pc(cpu) != 0x100u ||
        arm920t_get_cpsr(cpu) != 0xd3u) return 1;
    uint32_t data = 0x2000u + (scenario == 2u ? 0x800u : scenario == 3u ? 0x1000u : 8u);
    const uint32_t code_words[] = {0xe3a00011u, 0xe12fff1cu};
    for (uint32_t i = 0; i < sizeof(ram); ++i) {
        uint8_t expected = 0u;
        if (i >= 0x2000u && i < 0x2008u)
            expected = (uint8_t)(code_words[(i - 0x2000u) / 4u] >> ((i & 3u) * 8u));
        else if (i >= data && i - data < 4u)
            expected = (uint8_t)((count - 1u) >> ((i - data) * 8u));
        if (ram[i] != expected) {
            fprintf(stderr, "RAM mismatch at %08x\n", ARM_JIT_RAM_BASE_ADDR + i);
            return 1;
        }
    }
    uint64_t native_insns = 0u;
#if ARM920T_PROFILING
    native_insns = cpu->prof.native_arm_insns;
    if (native && native_insns != retired) return 1;
#endif
    printf("{\"native\":%u,\"icache\":%u,\"scenario\":%u,\"iterations\":%u,"
           "\"timed\":%u,\"elapsed_ns\":%" PRIu64 ",\"ns_per_call\":%.9f,"
           "\"guest_cycles\":%" PRIu64 ",\"epoch_changes\":%u,\"verified_bytes\":%zu,"
           "\"profile_enabled\":%u,\"native_insns\":%" PRIu64 "}\n",
           native, icache, scenario, count, timed, elapsed, (double)elapsed / count,
           retired, cpu->jit_cache_epoch - epoch, sizeof(ram), (unsigned)ARM920T_PROFILING, native_insns);
    arm920t_destroy(cpu);
    return 0;
}
