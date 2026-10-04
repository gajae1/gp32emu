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

/* Way-aware lookups: a set holds ARM_JIT_BLOCK_WAYS slots, so matching the
 * set base alone no longer identifies this PC's own block. */
static arm_jit_block_t *block_for_pc(uint32_t pc) {
    arm_jit_block_t *set = &cpu_jit->jit_blocks[arm_jit_block_index(pc)];
    for (unsigned way = 0; way < ARM_JIT_BLOCK_WAYS; ++way)
        if (set[way].valid && set[way].tag_pc == pc) return &set[way];
    return NULL;
}
static arm_jit_block_t *native_block_for_pc(uint32_t pc) {
    arm_jit_block_t *set = &cpu_jit->jit_blocks[arm_jit_block_index(pc)];
    for (unsigned way = 0; way < ARM_JIT_BLOCK_WAYS; ++way)
        if (set[way].valid && set[way].tag_pc == pc && set[way].native_ok && set[way].native)
            return &set[way];
    return NULL;
}

int main(void) {
#if !ARM920T_NATIVE_BACKEND
    puts("SKIP: native backend unavailable");
    return 0;
#else
    current_case = "arena-churn";
    setup_pair();
    /* Find a real set overflow rather than depending on one particular hash.
     * Two ways keep a colliding pair resident, so a third PC in the same set
     * is what still forces intra-generation replacement here. */
    uint32_t addresses[] = {RAM_BASE + 0x400u, 0u, 0u};
    for (uint32_t pc = addresses[0] + 0x100u; pc <= RAM_BASE + RAM_SIZE - 0x100u; pc += 4u) {
        if (arm_jit_block_index(pc) != arm_jit_block_index(addresses[0])) continue;
        if (!addresses[1]) { addresses[1] = pc; continue; }
        addresses[2] = pc;
        break;
    }
    CHECK(addresses[1] != 0u && addresses[2] != 0u, "churn fixture could not find a cache collision");
    if (!addresses[2]) { teardown_pair(); return 1; }
    for (unsigned j = 0; j < 3u; ++j) {
        for (unsigned i = 0; i < 32u; ++i) {
            /* A register PC write ends exactly at 32 operations; B self can
             * be stitched once more and force this budget into portable code. */
            uint32_t insn = i == 31u ? 0xe1a0f001u : 0xe2800001u;
            set_mem_both(addresses[j] + 4u * i, insn);
        }
    }
    uint32_t initial_generation = cpu_jit->jit_generation;
    for (unsigned i = 0; i < 3000u; ++i) {
        set_reg_both(15u, addresses[i % 3u]);
        set_reg_both(1u, addresses[i % 3u] + 31u * 4u);
        CHECK(arm920t_run(cpu_jit, 32u) == arm920t_run(cpu_ref, 32u), "churn budget");
        CHECK(native_block_for_pc(addresses[i % 3u]) != NULL,
              "new block lost native execution after cache fill");
        if ((i % 100u) == 0u) compare_state();
        if (failures) break;
    }
    compare_state();
    CHECK(cpu_jit->prof.native_block_calls == 3000u, "churn did not execute each native block");
    /* Invoke the native entry directly with insufficient budget: its early
     * return must unwind the host frame without changing guest state. */
    arm_jit_block_t *short_block = native_block_for_pc(addresses[2999u % 3u]);
    CHECK(short_block != NULL, "short-budget block unavailable");
    if (short_block) {
        CHECK(short_block->native(cpu_jit, short_block->count - 1u) == 0u,
              "native short-budget entry executed guest instructions");
        compare_state();
    }
    CHECK(cpu_jit->jit_generation != initial_generation, "arena never recycled");
    /* Exercise the same churn-triggered reset across the generation wrap. */
    cpu_jit->jit_generation = UINT32_MAX;
    cpu_jit->jit_code_used = cpu_jit->jit_code_size - 16u;
    set_reg_both(15u, addresses[0]);
    set_reg_both(1u, addresses[0] + 31u * 4u);
    CHECK(arm920t_run(cpu_jit, 32u) == arm920t_run(cpu_ref, 32u), "wrap budget");
    compare_state();
    CHECK(cpu_jit->jit_generation == 1u, "generation wrap failed");
    arm_jit_block_t *wrapped_block = block_for_pc(addresses[0]);
    CHECK(wrapped_block && wrapped_block->native_ok,
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
