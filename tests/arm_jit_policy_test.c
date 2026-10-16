#define GP32EMU_CPU_PROFILE 1
#include "../src/arm920t.c"
#define main old_native_test_main
#include "arm_jit_test.c"
#undef main

#if ARM920T_NATIVE_BACKEND
static arm_jit_block_t *policy_block(uint32_t pc) {
    uint32_t slot = arm_jit_block_index(pc);
    for (unsigned way=0; way<ARM_JIT_BLOCK_WAYS; ++way) {
        arm_jit_block_t *b=&cpu_jit->jit_blocks[slot+way];
        if(b->valid && b->generation==cpu_jit->jit_generation && b->tag_pc==pc) return b;
    }
    fail("missing policy block"); return NULL;
}

static void policy_lifecycle(void) {
    const uint32_t target=RAM_BASE+0x3000u;
    current_case="second-encounter-policy";
    setup_cache_pair();
    set_mem_both(target,0xe3a04011u); /* MOV r4,#17 */
    set_mem_both(target+4u,0xe12fff1eu); /* BX lr */
    set_reg_both(14u,CODE_ADDR+0x1000u);
    arm_jit_block_t *b=arm_jit_translate(cpu_jit,target);
    CHECK(b && b->native_ok==4u && !b->native,"pending first encounter");
    run_cache_pair(target,0u);
    CHECK(b->native_ok==4u && !b->native,"zero budget must not promote");
    run_cache_pair(target,2u); b=policy_block(target);
    CHECK(b && !b->native && b->native_ok==0u,"first real encounter stays portable");
    CHECK(ref_reg(4u)==17u,"first portable result");
    run_cache_pair(target,2u); b=policy_block(target);
    CHECK(b && b->native && b->native_ok==1u,"second encounter publishes native code");
    uint64_t used=cache_code_used();
    run_cache_pair(CODE_ADDR,1u); /* actual guest I-cache maintenance */
    run_cache_pair(target,2u); b=policy_block(target);
    CHECK(b && b->native && b->native_ok==1u && cache_code_used()==used,"unchanged mature code stays native");
    set_mem_both(target,0xe3a04022u);
    arm920t_invalidate_icache_range(cpu_jit,target,4u);
    arm920t_invalidate_icache_range(cpu_ref,target,4u);
    run_cache_pair(target,2u); b=policy_block(target);
    CHECK(b && !b->native && b->native_ok==0u && ref_reg(4u)==34u,"changed code starts portable with fresh instructions");
    run_cache_pair(target,2u); b=policy_block(target);
    CHECK(b && b->native && b->native_ok==1u,"changed code promotes on reuse");
    arm920t_set_jit(cpu_jit,0); arm920t_set_jit(cpu_jit,1);
    run_cache_pair(target,1u); b=policy_block(target);
    CHECK(b && !b->native && b->native_ok==0u,"disable/enable resets first encounter");
    run_cache_pair(target,1u); b=policy_block(target);
    CHECK(b && !b->native,"short budget does not compile a full block");
    run_cache_pair(target,2u); b=policy_block(target);
    CHECK(b && b->native,"later full budget permits promotion");
    teardown_pair();
}
#endif

int main(void) {
#if ARM920T_NATIVE_BACKEND
    policy_lifecycle();
#endif
    /* Preserve existing cache assertions; only warm both compilation tiers
     * before its arena-size snapshot. No checks are removed or relaxed. */
    case_cache_unchanged();
    case_cache_modified(0); case_cache_modified(1);
    case_aliased_fetch_proof(0); case_aliased_fetch_proof(1);
    case_data_aliased_fetch_proof(0); case_data_aliased_fetch_proof(1);
    case_data_aliased_fetch_proof(2); case_decode_refill_order();
    if(failures) {fprintf(stderr,"POLICY_CHECK_FAIL %d\n",failures);return 1;}
#if ARM920T_NATIVE_BACKEND
    puts("PASS: JIT promotion lifecycle and cache maintenance");
#else
    puts("PASS: portable cache maintenance (native promotion not applicable)");
#endif
    return 0;
}
