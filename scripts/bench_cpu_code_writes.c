/* Synthetic CPU store cost; no ROM, video, audio or device timing.
 * Compile with -O3 -std=c2x -D_XOPEN_SOURCE=700 -Isrc -Iinclude.
 * Arguments: execution (0 portable, 1 native, 2 raw), CP15.I, MMU,
 * guest instructions (multiple of 3, or 5 for shape 2), placement (0 other page,
 * 1 same 4 KiB page/different 64 bytes, 2 same 64 bytes/different instruction
 * word), shape (0 scalar STR, 1 STM of all 16 registers, 2 STR with a 4 KiB
 * stride across the full RAM window, executing five instructions in BIOS).
 * CORE_UNDER_TEST may select a baseline checkout's src/arm920t.c. */
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

int main(int argc, char **argv) {
    unsigned mode = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 10) : 1u;
    unsigned icache = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 0u;
    unsigned mmu = argc > 3 ? (unsigned)strtoul(argv[3], NULL, 10) : 0u;
    unsigned budget = argc > 4 ? (unsigned)strtoul(argv[4], NULL, 10) : 30000000u;
    unsigned same_page = argc > 5 ? (unsigned)strtoul(argv[5], NULL, 10) : 0u;
    unsigned shape = argc > 6 ? (unsigned)strtoul(argv[6], NULL, 10) : 0u;
    unsigned loop_insns = shape == 2u ? 5u : 3u;
    if (argc > 7 || mode > 2u || icache > 1u || mmu > 1u || same_page > 2u || shape > 2u ||
        (shape == 2u && same_page) || !budget || budget % loop_insns) {
        fprintf(stderr, "usage: %s [mode:0..2] [I:0..1] [MMU:0..1] [instructions:positive multiple of loop length] [placement:0..2] [shape:0..2]\n", argv[0]);
        return 2;
    }
    const uint32_t pc = shape == 2u ? CODE_ADDR : RAM_BASE + 0x10000u,
                   data = shape == 2u ? RAM_BASE : same_page == 2u ? pc + 0x20u :
                          same_page ? pc + 0x200u : RAM_BASE + 0x30000u;
    const uint32_t program[] = {0xe2800001u, shape == 1u ? 0xe887ffffu : 0xe5870000u,
                               shape == 2u ? 0xe2877a01u : 0xeafffffcu, /* ADD r7,r7,#0x1000 */
                               0xe3c77502u, 0xeafffffau}; /* BIC r7,r7,#0x800000; B loop */
    setup_pair();
    arm920t_set_jit(cpu_jit, mode == 1u);
    arm920t_set_trace(cpu_jit, mode == 2u, NULL, NULL);
    cpu_jit->cp15[1] = 0x70u | (icache ? 1u << 12 : 0u) | (mmu ? 1u : 0u);
    if (mmu) {
        itlb_store(cpu_jit, pc, pc, 0xfffu);
        if (shape == 2u) {
            for (unsigned section = 0; section < RAM_SIZE; section += 0x100000u)
                tlb_store_checked(cpu_jit, RAM_BASE + section, RAM_BASE + section, 0xfffffu, NULL);
        } else tlb_store_checked(cpu_jit, data, data & ~0xfffu, 0xfffu, NULL);
    }
    for (unsigned i = 0; i < loop_insns; ++i)
        gp32_st32le(bus_ptr(&bus_jit, pc + i * 4u, 4u), program[i]);
    cpu_jit->r[7] = data; cpu_jit->r[15] = pc;
    arm920t_run(cpu_jit, 30000u);
    arm920t_reset_cpu_profile(cpu_jit);
    uint32_t epoch = cpu_jit->jit_cache_epoch, generation = cpu_jit->jit_generation;
    struct timespec begin, end;
    clock_gettime(CLOCK_MONOTONIC, &begin);
    uint32_t retired = arm920t_run(cpu_jit, budget);
    clock_gettime(CLOCK_MONOTONIC, &end);
    double seconds = end.tv_sec - begin.tv_sec + (end.tv_nsec - begin.tv_nsec) * 1e-9;
    uint32_t final_data = shape == 2u ? RAM_BASE + ((cpu_jit->r[7] - RAM_BASE - 0x1000u) & (RAM_SIZE - 1u)) : data;
    size_t watch_bytes = 0u;
#ifdef ARM_JIT_CODE_WATCH_SHIFT
    watch_bytes = sizeof(cpu_jit->jit_code_watches);
#endif
    printf("mode=%u I=%u MMU=%u same_page=%u shape=%u retired=%u ns_per_insn=%.4f native=%llu epoch_changed=%d generation_changed=%d r0=%u memory=%u cpu_bytes=%zu watch_bytes=%zu\n",
           mode, icache, mmu, same_page, shape, retired, seconds * 1e9 / retired,
           (unsigned long long)cpu_jit->prof.native_arm_insns,
           epoch != cpu_jit->jit_cache_epoch, generation != cpu_jit->jit_generation,
           cpu_jit->r[0], gp32_ld32le(bus_jit.ram + final_data - RAM_BASE), sizeof(*cpu_jit), watch_bytes);
    int ok = retired == budget && (same_page || epoch == cpu_jit->jit_cache_epoch) && generation == cpu_jit->jit_generation &&
             cpu_jit->r[0] == (budget + 30000u) / loop_insns && cpu_jit->r[0] == gp32_ld32le(bus_jit.ram + final_data - RAM_BASE);
#if defined(ARM_JIT_CODE_WATCH_SHIFT) && ARM_JIT_CODE_WATCH_SHIFT == 2u
    /* Every placement avoids instruction bytes, so the exact-word candidate
     * must never invalidate, including for the full STM range at pc+0x20. */
    ok &= epoch == cpu_jit->jit_cache_epoch;
#endif
    if (mode == 1u) ok &= cpu_jit->prof.native_arm_insns != 0u;
    teardown_pair();
    return !ok;
}
