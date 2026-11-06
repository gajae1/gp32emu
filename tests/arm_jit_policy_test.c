#define GP32EMU_CPU_PROFILE 1
#include "../src/arm920t.c"
#define main old_native_test_main
#include "arm_jit_test.c"
#undef main

#if ARM920T_NATIVE_BACKEND
#define COLD_PENDING_INITIAL \
    ((uint8_t)(3u + ARM_JIT_COLD_PORTABLE_OBSERVATIONS))
_Static_assert(ARM_JIT_COLD_PORTABLE_OBSERVATIONS >= 1u &&
               ARM_JIT_COLD_PORTABLE_OBSERVATIONS <= 252u,
               "native_ok countdown must fit its reserved byte");

static arm_jit_block_t *policy_block(uint32_t pc) {
    uint32_t slot = arm_jit_block_index(pc);
    for (unsigned way=0; way<ARM_JIT_BLOCK_WAYS; ++way) {
        arm_jit_block_t *b=&cpu_jit->jit_blocks[slot+way];
        if(b->valid && b->generation==cpu_jit->jit_generation && b->tag_pc==pc) return b;
    }
    fail("missing policy block"); return NULL;
}

static arm_jit_block_t *observe_cold_once(uint32_t pc, unsigned observed) {
    run_cache_pair(pc,2u);
    arm_jit_block_t *b=policy_block(pc);
    uint8_t want=observed<ARM_JIT_COLD_PORTABLE_OBSERVATIONS
        ? (uint8_t)(COLD_PENDING_INITIAL-observed) : 0u;
    CHECK(b && !b->native && b->native_ok==want,
          observed<ARM_JIT_COLD_PORTABLE_OBSERVATIONS
              ? "cold observation remains portable and decrements"
              : "last cold observation clears pending state");
    return b;
}

static void policy_lifecycle(void) {
    const uint32_t target=RAM_BASE+0x3000u;
    current_case="cold-observation-policy";
    setup_cache_pair();
    set_mem_both(target,0xe3a04011u); /* MOV r4,#17 */
    set_mem_both(target+4u,0xe12fff1eu); /* BX lr */
    set_reg_both(14u,CODE_ADDR+0x1000u);
    arm_jit_block_t *b=arm_jit_translate(cpu_jit,target);
    CHECK(b && b->native_ok==COLD_PENDING_INITIAL && !b->native,
          "initial cold countdown");
    run_cache_pair(target,0u);
    CHECK(b->native_ok==COLD_PENDING_INITIAL && !b->native,
          "zero budget must not consume an observation");
    b=observe_cold_once(target,1u);
    CHECK(ref_reg(4u)==17u,"first portable result");
    if (ARM_JIT_COLD_PORTABLE_OBSERVATIONS>1u) {
        run_cache_pair(CODE_ADDR,1u); /* unchanged fetch, renewed epoch */
        b=policy_block(target);
        CHECK(b && !b->native && b->native_ok==COLD_PENDING_INITIAL-1u,
              "unchanged revalidation preserves the cold countdown");
    }
    for (unsigned observed=2u;
         observed<=ARM_JIT_COLD_PORTABLE_OBSERVATIONS; ++observed)
        b=observe_cold_once(target,observed);
    CHECK(b && !b->native,"no native code before the countdown expires");
    run_cache_pair(target,2u); b=policy_block(target);
    CHECK(b && b->native && b->native_ok==1u,
          "first encounter after countdown publishes native code");
    uint64_t used=cache_code_used();
    run_cache_pair(CODE_ADDR,1u); /* actual guest I-cache maintenance */
    run_cache_pair(target,2u); b=policy_block(target);
    CHECK(b && b->native && b->native_ok==1u && cache_code_used()==used,"unchanged mature code stays native");
    set_mem_both(target,0xe3a04022u);
    arm920t_invalidate_icache_range(cpu_jit,target,4u);
    arm920t_invalidate_icache_range(cpu_ref,target,4u);
    b=observe_cold_once(target,1u);
    CHECK(ref_reg(4u)==34u,"changed code starts portable with fresh instructions");
    for (unsigned observed=2u;
         observed<=ARM_JIT_COLD_PORTABLE_OBSERVATIONS; ++observed)
        b=observe_cold_once(target,observed);
    CHECK(b && !b->native,"changed code observes the full countdown");
    run_cache_pair(target,2u); b=policy_block(target);
    CHECK(b && b->native && b->native_ok==1u,
          "changed code promotes after the countdown");
    arm920t_set_jit(cpu_jit,0); arm920t_set_jit(cpu_jit,1);
    run_cache_pair(target,1u); b=policy_block(target);
    CHECK(b && !b->native && b->native_ok==
              (ARM_JIT_COLD_PORTABLE_OBSERVATIONS==1u ? 0u : COLD_PENDING_INITIAL-1u),
          "short nonzero encounter consumes one observation without compiling");
    for (unsigned observed=2u;
         observed<=ARM_JIT_COLD_PORTABLE_OBSERVATIONS; ++observed)
        b=observe_cold_once(target,observed);
    CHECK(b && !b->native,"disable/enable starts a fresh countdown");
    run_cache_pair(target,1u); b=policy_block(target);
    CHECK(b && !b->native && b->native_ok==0u,
          "expired countdown still requires a full block budget");
    run_cache_pair(target,2u); b=policy_block(target);
    CHECK(b && b->native,"later full budget permits promotion");
    teardown_pair();
}
#if defined(ARM_JIT_NATIVE_A64)
/* Native promotion may split an unvisited exit tail, but that tail remains
 * real guest code. Compare every execution against raw interpretation,
 * including first exit, modified exit code, and an exit visited while cold. */
static void policy_observed_exit(int visit_tail, int forward) {
    const uint32_t entry=RAM_BASE+0x3020u;
    const uint32_t outside=forward ? entry+0x1000u : entry-0x20u;
    current_case=visit_tail?"observed-exit-kept":"unvisited-exit-split";
    setup_cache_pair();
    set_mem_both(outside,0xea000000u | (((entry-outside-8u)>>2)&0xffffffu)); /* B entry */
    set_mem_both(entry,0xe2500001u); /* SUBS r0,r0,#1 */
    set_mem_both(entry+4u,0x1a000000u | (((outside-entry-12u)>>2)&0xffffffu)); /* BNE outside */
    for(unsigned i=2;i<30u;++i) set_mem_both(entry+i*4u,0xe1a01001u);
    set_mem_both(entry+120u,0xe3a04011u); /* MOV r4,#17 */
    set_mem_both(entry+124u,0xe12fff1eu); /* BX lr */
    set_reg_both(14u,outside+0x1000u);
    for(unsigned i=0;i<ARM_JIT_COLD_PORTABLE_OBSERVATIONS;++i) {
        set_reg_both(0u,visit_tail && i==0u ? 1u:100u);
        run_cache_pair(entry,visit_tail && i==0u ? 32u:2u);
    }
    arm_jit_block_t *b=policy_block(entry);
    CHECK(b && !b->native && b->count==32u,"cold collection keeps exit tail");
    /* Build the old whole-block proof before promotion: a split must discard
     * it, otherwise later revalidation could retain discarded code words. */
    CHECK(arm_jit_fetch_unchanged(cpu_jit,b),"cold proof is valid");
    set_reg_both(0u,100u);
    run_cache_pair(entry,32u);b=policy_block(entry);
    CHECK(b && b->native,"observed block reaches native execution");
    CHECK(b && b->count==(visit_tail?32u:2u),"promotion honors observed exit");
    set_reg_both(0u,1u);run_cache_pair(entry,32u);
    CHECK(ref_reg(4u)==17u,"first exit executes the unvisited tail");
    set_mem_both(entry+120u,0xe3a04022u);
    run_cache_pair(CODE_ADDR,1u); /* real I-cache maintenance */
    set_reg_both(0u,1u);run_cache_pair(entry,32u);
    CHECK(ref_reg(4u)==34u,"changed exit code is fetched after maintenance");
    teardown_pair();
}
#endif

#endif

/* A first revalidation must compare against the original decode even if
 * its compact proof has not been needed yet. Repeat after replacing that
 * same cache slot, so stale proof metadata cannot accept the old opcode. */
static void first_revalidation_changed(void) {
    const uint32_t target = RAM_BASE + 0x3000u;
    current_case = "first-revalidation-changed";
    setup_cache_pair();
    set_mem_both(target, 0xe3a04011u); /* MOV r4,#17 */
    set_mem_both(target + 4u, 0xe12fff1eu); /* BX lr */
    set_reg_both(14u, CODE_ADDR + 0x1000u);
    run_cache_pair(target, 2u);
    CHECK(ref_reg(4u) == 17u, "original decoded instruction");
    for (unsigned value = 34u; value <= 51u; value += 17u) {
        set_mem_both(target, 0xe3a04000u | value);
        run_cache_pair(CODE_ADDR, 1u); /* guest I-cache maintenance */
        run_cache_pair(target, 2u);
        CHECK(ref_reg(4u) == value, "first revalidation rejects changed instruction");
    }
    teardown_pair();
}

int main(void) {
#if ARM920T_NATIVE_BACKEND
    policy_lifecycle();
#if defined(ARM_JIT_NATIVE_A64)
    policy_observed_exit(0,0); policy_observed_exit(1,0);
    policy_observed_exit(0,1); policy_observed_exit(1,1);
#endif
#endif
    first_revalidation_changed();
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
