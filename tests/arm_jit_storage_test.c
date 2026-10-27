#define _GNU_SOURCE 1 /* expose mremap before the allocation-failure wrapper */
#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <time.h>
#include <stdatomic.h>
static atomic_int reject_allocation, rejected_allocations;
static unsigned fail_calloc_at;
static void *storage_malloc(size_t bytes) {
    if (atomic_load(&reject_allocation)) {
        atomic_fetch_add(&rejected_allocations, 1);
        return NULL;
    }
    return malloc(bytes);
}
static void *storage_calloc(size_t count, size_t bytes) {
    return fail_calloc_at && !--fail_calloc_at ? NULL : calloc(count, bytes);
}
#define malloc storage_malloc
#define calloc storage_calloc
#define GP32EMU_CPU_PROFILE 1
#include "../src/arm920t.c"
#undef malloc
#undef calloc
#define main original_test_main
#include "arm_jit_test.c"
#undef main

int main(void) {
    current_case="decoded-metadata-allocation-failure";
    for (unsigned failure=1; failure<=3; ++failure) {
        setup_cache_pair();
        fail_calloc_at=failure;
        CHECK(!arm_jit_alloc(cpu_jit),"metadata allocation failure");
        CHECK(!cpu_jit->jit_blocks && !cpu_jit->jit_data && !cpu_jit->jit_poll,
              "partial metadata allocation cleared");
        fail_calloc_at=0;
        CHECK(arm_jit_alloc(cpu_jit),"metadata allocation retry");
        teardown_pair();
    }
    current_case="stable-decoded-storage";
    setup_cache_pair();
    const uint32_t pc=RAM_BASE+0x3000u;
    set_mem_both(pc,0xe5975000u); /* LDR r5,[r7]: retains decoded operand. */
    set_mem_both(pc+4u,0xe12fff1eu);
    set_reg_both(7,IO_ADDR);
    set_reg_both(14,pc+0x1000u);
    CHECK(arm_jit_alloc(cpu_jit),"allocate metadata");
    reject_allocation=1;
    CHECK(!arm_jit_translate(cpu_jit,pc),"allocation failure rejects unpublished block");
    CHECK(!cpu_jit->jit_op_slabs,"failed allocation leaves no slab");
    run_cache_pair(pc,2u);
    CHECK(!cpu_jit->jit_op_slabs,"allocation failure retains interpreter fallback");
    reject_allocation=0;
    arm_jit_block_t *b=arm_jit_translate(cpu_jit,pc);
    CHECK(b!=NULL,"retry after allocation failure");
    arm_jit_storage_t *storage=cpu_jit->jit_data[b-cpu_jit->jit_blocks];
    for(unsigned n=0;n<ARM_JIT_COLD_PORTABLE_OBSERVATIONS+1u;++n)run_cache_pair(pc,2u);
#if ARM920T_NATIVE_BACKEND
    CHECK(b->native!=NULL,"original helper compiled");
#endif
    unsigned used=0;
    for(uint32_t other=pc+0x200u;used<ARM_JIT_OP_SLAB_SLOTS*2u;other+=8u) {
        if(arm_jit_block_index(other)==arm_jit_block_index(pc))continue;
        set_mem_both(other,0xe2800001u);
        set_mem_both(other+4u,0xe12fff1eu);
        CHECK(arm_jit_translate(cpu_jit,other)!=NULL,"grow beyond two slabs");
        ++used;
    }
    CHECK(cpu_jit->jit_data[b-cpu_jit->jit_blocks]==storage,"growth keeps helper operand address");
    run_cache_pair(pc,2u);
    cpu_jit->jit_generation=UINT32_MAX;
    arm920t_jit_invalidate_all(cpu_jit,ARM_JIT_INV_API_FLUSH);
    CHECK(cpu_jit->jit_data[b-cpu_jit->jit_blocks]==storage,"generation wrap retains stable address");
    b=arm_jit_translate(cpu_jit,pc);
    CHECK(arm_jit_ops(cpu_jit,b)==storage->ops,"retranslation reuses stable storage");
    run_cache_pair(pc,2u);
#if ARM_JIT_STORAGE_THREADS
    CHECK(cpu_jit->jit_storage_started && cpu_jit->jit_storage_worker,
          "larger workloads start one preparation worker");
#endif
    teardown_pair();
#if ARM_JIT_STORAGE_THREADS
    /* Private allocation failure terminates the producer; consumption still
     * falls back without waiting and shutdown must join the failed worker. */
    reject_allocation=1;
    atomic_store(&rejected_allocations,0);
    arm_jit_storage_worker_t *w=arm_jit_storage_start();
    CHECK(w!=NULL,"create worker for allocation failure");
    /* Observe the producer's failed allocation before taking on the owner.
     * Otherwise an empty queue followed by stop could skip the OOM path. */
    for (unsigned n=0;n<5000u && !atomic_load(&rejected_allocations);++n) {
        struct timespec pause={0,1000000};
        nanosleep(&pause,NULL);
    }
    CHECK(atomic_load(&rejected_allocations)>0,"producer attempted allocation");
    CHECK(arm_jit_storage_take(w)==NULL,"empty queue preserves allocation failure");
    arm_jit_storage_stop(w);
    reject_allocation=0;
    for(unsigned n=0;n<16u;++n) {
        w=arm_jit_storage_start();
        CHECK(w!=NULL,"start before immediate shutdown");
        arm_jit_storage_stop(w);
    }
#endif
    if(failures){fprintf(stderr,"STORAGE_FAIL %d\n",failures);return 1;}
    puts("PASS: decoded storage growth, helper lifetime, OOM fallback and generation wrap");
    return 0;
}
