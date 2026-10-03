/* Force cache churn with a small arena; reuse the interpreter differential bus. */
#define ARM_JIT_CODE_SIZE (128u * 1024u)
#define GP32EMU_CPU_PROFILE 1
#ifndef ARM_JIT_NATIVE_MAX_BYTES
#if defined(__aarch64__)
#define ARM_JIT_NATIVE_MAX_BYTES 65536u
#else
#define ARM_JIT_NATIVE_MAX_BYTES 16384u
#endif
#endif
#ifndef ARM_JIT_RECYCLE_SOURCE
#define ARM_JIT_RECYCLE_SOURCE "../src/arm920t.c"
#endif
#include ARM_JIT_RECYCLE_SOURCE
#define main arm_jit_differential_main
#include "arm_jit_test.c"
#undef main

int main(void) {
#if !ARM920T_NATIVE_BACKEND
    puts("SKIP: native backend unavailable");
    return 0;
#else
    current_case = "arena-churn";
    setup_pair();
    /* Two immutable programs collide in the direct-mapped block table. */
    const uint32_t addresses[] = {RAM_BASE + 0x400u, RAM_BASE + 0x10400u};
    for (unsigned j = 0; j < 2u; ++j) {
        for (unsigned i = 0; i < 32u; ++i) {
            uint32_t insn = i == 31u ? 0xEAFFFFFEu : 0xE2800001u;
            set_mem_both(addresses[j] + 4u * i, insn);
        }
    }
    uint32_t initial_generation = cpu_jit->jit_generation;
    for (unsigned i = 0; i < 3000u; ++i) {
        set_reg_both(15u, addresses[i & 1u]);
        CHECK(arm920t_run(cpu_jit, 32u) == arm920t_run(cpu_ref, 32u), "churn budget");
        arm_jit_block_t *b = &cpu_jit->jit_blocks[(addresses[i & 1u] >> 2) & ARM_JIT_BLOCK_MASK];
        CHECK(b->native_ok && b->native, "new block lost native execution after cache fill");
        if ((i % 100u) == 0u) compare_state();
        if (failures) break;
    }
    compare_state();
    /* Invoke the native entry directly with insufficient budget: its early
     * return must unwind the host frame without changing guest state. */
    arm_jit_block_t *short_block = &cpu_jit->jit_blocks[(addresses[1] >> 2) & ARM_JIT_BLOCK_MASK];
    CHECK(short_block->native && short_block->native_ok, "short-budget block unavailable");
    if (short_block->native && short_block->native_ok) {
        CHECK(short_block->native(cpu_jit, short_block->count - 1u) == 0u,
              "native short-budget entry executed guest instructions");
        compare_state();
    }
    CHECK(cpu_jit->jit_generation != initial_generation, "arena never recycled");
    /* Exercise the same churn-triggered reset across the generation wrap. */
    cpu_jit->jit_generation = UINT32_MAX;
    cpu_jit->jit_code_used = cpu_jit->jit_code_size - 16u;
    set_reg_both(15u, addresses[0]);
    CHECK(arm920t_run(cpu_jit, 32u) == arm920t_run(cpu_ref, 32u), "wrap budget");
    compare_state();
    CHECK(cpu_jit->jit_generation == 1u, "generation wrap failed");
    CHECK(cpu_jit->jit_blocks[(addresses[0] >> 2) & ARM_JIT_BLOCK_MASK].native_ok,
          "wrap cleared the block under translation");
    /* A guest cache epoch can wrap while its MCR native block is active.
     * It must retire that arena without overwriting the returning block. */
    uint32_t before_cache_wrap = cpu_jit->jit_generation;
    cpu_jit->jit_cache_epoch = UINT32_MAX;
    set_mem_both(CODE_ADDR, 0xee070f15u); /* MCR p15,0,r0,c7,c5,0 */
    set_reg_both(15u, CODE_ADDR);
    CHECK(arm920t_run(cpu_jit, 1u) == arm920t_run(cpu_ref, 1u), "cache epoch wrap budget");
    compare_state();
    CHECK(cpu_jit->jit_generation != before_cache_wrap && cpu_jit->jit_code_used == 0u,
          "cache epoch wrap retained obsolete native code");
    set_reg_both(15u, addresses[0]);
    CHECK(arm920t_run(cpu_jit, 32u) == arm920t_run(cpu_ref, 32u), "post-cache-wrap budget");
    compare_state();
    teardown_pair();
    if (failures) return 1;
    puts("PASS: native arena churn and generation wrap match interpreter");
    return 0;
#endif
}
