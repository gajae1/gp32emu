/* Repeated unchanged-code execution, with or without a cache epoch change.
 * Synthetic ARM programs only. Times include dispatch and complete guest work.
 * cc -std=c2x -O3 -Isrc -Iinclude scripts/bench_code_revalidation.c -o bench-proof
 * CORE_UNDER_TEST may select another checkout's src/arm920t.c. */
#define _XOPEN_SOURCE 700
#ifndef GP32EMU_CPU_PROFILE
#define GP32EMU_CPU_PROFILE 1
#endif
#ifndef CORE_UNDER_TEST
#define CORE_UNDER_TEST "../src/arm920t.c"
#endif
#include CORE_UNDER_TEST
#define main fixture_main
#include "../tests/arm_jit_test.c"
#undef main
#include <time.h>

static uint64_t monotonic_ns(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) abort();
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}

int main(int argc, char **argv) {
    unsigned native = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 10) : 1u;
    unsigned count = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 48u;
    unsigned working = argc > 3 ? (unsigned)strtoul(argv[3], NULL, 10) : 64u;
    unsigned maintenance = argc > 4 ? (unsigned)strtoul(argv[4], NULL, 10) : 1u;
    unsigned rounds = argc > 5 ? (unsigned)strtoul(argv[5], NULL, 10) : 200000u;
    unsigned icache = argc > 6 ? (unsigned)strtoul(argv[6], NULL, 10) : 1u;
    if (argc > 7 || native > 1u || count < 2u || count > ARM_JIT_MAX_INSNS ||
        !working || working > 64u || maintenance > 1u || icache > 1u ||
        !rounds || rounds > 10000000u)
        return 2;
    setup_pair();
    arm920t_set_jit(cpu_jit, native);
    cpu_jit->cp15[1] = 0x70u | (icache << 12);
    const uint32_t base = RAM_BASE + 0x10000u, sentinel = RAM_BASE + 0x700000u;
    cpu_jit->r[1] = sentinel;
    for (unsigned b = 0; b < working; ++b)
        for (unsigned i = 0; i < count; ++i)
            gp32_st32le(bus_jit.ram + base - RAM_BASE + b * 4096u + i * 4u,
                        i + 1u == count ? 0xe1a0f001u : 0xe2800001u);
    /* Native promotion observes a backend-dependent number of executions.
     * Begin timing only when the whole working set is actually native; two
     * warmups no longer suffice on A64. Bound failed promotion explicitly. */
    unsigned ready_native_blocks = 0u, warmup_rounds = 0u;
    do {
        for (unsigned b = 0; b < working; ++b) {
            cpu_jit->r[15] = base + b * 4096u;
            if (arm920t_run(cpu_jit, count) != count || cpu_jit->r[15] != sentinel) return 3;
        }
        ++warmup_rounds;
        ready_native_blocks = 0u;
        for (unsigned i = 0; i < ARM_JIT_BLOCK_COUNT; ++i) {
            const arm_jit_block_t *b = &cpu_jit->jit_blocks[i];
            if (b->valid && b->generation == cpu_jit->jit_generation &&
                b->tag_pc >= base && b->tag_pc < base + working * 4096u &&
                !((b->tag_pc - base) % 4096u) && b->count == count &&
                b->native_ok == 1u && b->native) ++ready_native_blocks;
        }
    } while (warmup_rounds < 256u &&
             (native ? ready_native_blocks != working : warmup_rounds < 2u));
    if (native && ready_native_blocks != working) {
        fprintf(stderr, "native warmup incomplete: %u/%u blocks after %u rounds\n",
                ready_native_blocks, working, warmup_rounds);
        teardown_pair();
        return 4;
    }
    uint32_t initial_r0 = cpu_jit->r[0], generation = cpu_jit->jit_generation;
    size_t compiled_bytes = cpu_jit->jit_code_used;
    arm920t_reset_cpu_profile(cpu_jit);
    uint64_t start = monotonic_ns(), retired = 0;
    for (unsigned r = 0; r < rounds; ++r) {
        if (maintenance) arm920t_jit_invalidate_all(cpu_jit, ARM_JIT_INV_CP15_CACHE);
        cpu_jit->r[15] = base + (r % working) * 4096u;
        retired += arm920t_run(cpu_jit, count);
    }
    uint64_t elapsed = monotonic_ns() - start;
    int ok = retired == (uint64_t)rounds * count && cpu_jit->r[15] == sentinel &&
        cpu_jit->r[0] == initial_r0 + rounds * (count - 1u) &&
        cpu_jit->jit_generation == generation && cpu_jit->jit_code_used == compiled_bytes;
#if ARM920T_PROFILING
    ok &= cpu_jit->prof.jit_native_compiled == 0u;
    if (native) ok &= cpu_jit->prof.native_arm_insns == retired;
    else ok &= cpu_jit->prof.block_interp_arm_insns == retired;
#endif
    printf("mode=%u count=%u working=%u maintenance=%u rounds=%u ns_per_block=%.5f retired=%llu native=%llu compiled=%llu profile=%u icache=%u ready_native_blocks=%u warmup_rounds=%u ok=%d\n",
           native, count, working, maintenance, rounds, (double)elapsed / rounds,
           (unsigned long long)retired, (unsigned long long)cpu_jit->prof.native_arm_insns,
           (unsigned long long)cpu_jit->prof.jit_native_compiled,
           (unsigned)ARM920T_PROFILING, icache, ready_native_blocks, warmup_rounds, ok);
    teardown_pair();
    return !ok;
}
