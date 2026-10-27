#define main existing_arm_jit_main
#include "arm_jit_test.c"
#undef main

/* Exact state comparisons against raw execution, with independently checked
 * destination effects. Mutations happen after native collection to exercise
 * target mismatch, changed mapping, code invalidation and return mismatch. */
int main(void) {
    unsigned cases=0;
    for(unsigned cached=0;cached<2;++cached)
    for(unsigned mmu=0;mmu<2;++mmu)
    for(unsigned scenario=0;scenario<8;++scenario) {
        current_case="guarded-indirect-call";
        setup_pair();
        const uint32_t callee=CODE_ADDR+0x100u, other=CODE_ADDR+0x180u;
        const uint32_t program[]={0xe1a0e00fu,0xe591f000u,0xe2866001u,0xeafffffeu};
        const uint32_t body[]={0xe92d4000u,0xe2855001u,0xe8bd8000u};
        load_both(program,GP32_ARRAY_COUNT(program));
        for(unsigned i=0;i<3;++i){set_mem_both(callee+4*i,body[i]);set_mem_both(other+4*i,body[i]);}
        if(scenario==5) {
            set_mem_both(callee+8,0xe58d3000u); /* STR r3,[sp]: change saved return */
            set_mem_both(callee+12,0xe8bd8000u);
            set_reg_both(3,CODE_ADDR+12u);
        }
        if(scenario==7) {
            set_mem_both(callee+4,0xe5940000u); /* LDR r0,[r4]: IRQ callback */
            set_mem_both(callee+8,0xe2855001u);
            set_mem_both(callee+12,0xe8bd8000u);
            set_mem_both(0x18u,0xe5895000u); /* capture r5 before callee resumes */
            set_mem_both(0x1cu,0xe5883000u); /* acknowledge */
            set_mem_both(0x20u,0xe25ef004u); /* exception return */
            set_reg_both(4,IO_ADDR);set_reg_both(8,IO_ADDR+4u);
            set_reg_both(9,DATA_ADDR+0x200u);
            set_mem_both(DATA_ADDR+0x200u,0xdeadbeefu);
            arm920t_set_cpsr(cpu_jit,0x1fu);arm920t_set_cpsr(cpu_ref,0x1fu);
            bus_jit.observe_cpu=cpu_jit;bus_ref.observe_cpu=cpu_ref;
            bus_jit.io_raise_at=bus_ref.io_raise_at=1u;
        }
        set_mem_both(other+4,0xe2855007u);
        set_mem_both(DATA_ADDR,callee);
        set_mem_both(DATA_ADDR+0x1000u,other);
        set_reg_both(1,DATA_ADDR);
        set_reg_both(13,DATA_ADDR+0x800u);
        if(mmu) {
            cpu_jit->cp15[1]=cpu_ref->cp15[1]=1u;
            itlb_store(cpu_jit,0u,0u,0xfffu);itlb_store(cpu_ref,0u,0u,0xfffu);
            tlb_store_checked(cpu_jit,DATA_ADDR,DATA_ADDR,0xfffu,NULL);
            tlb_store_checked(cpu_ref,DATA_ADDR,DATA_ADDR,0xfffu,NULL);
            tlb_store_checked(cpu_jit,DATA_ADDR+0x1000u,DATA_ADDR+0x1000u,0xfffu,NULL);
            tlb_store_checked(cpu_ref,DATA_ADDR+0x1000u,DATA_ADDR+0x1000u,0xfffu,NULL);
            if(scenario==7) {
                tlb_store_checked(cpu_jit,IO_ADDR,IO_ADDR,0xfffu,NULL);
                tlb_store_checked(cpu_ref,IO_ADDR,IO_ADDR,0xfffu,NULL);
            }
        }
        prepare_native_block(cpu_jit,CODE_ADDR);
        arm_jit_block_t *b=NULL;
        unsigned slot=arm_jit_block_index(CODE_ADDR);
        for(unsigned way=0;way<ARM_JIT_BLOCK_WAYS;++way)
            if(cpu_jit->jit_blocks[slot+way].tag_pc==CODE_ADDR && cpu_jit->jit_blocks[slot+way].valid)
                b=&cpu_jit->jit_blocks[slot+way];
        CHECK(b && b->count>2 && arm_jit_ops(cpu_jit,b)[2].pc==callee,
              "fixture reaches collected indirect callee");
        if(cached) arm920t_set_jit(cpu_jit,0);
        /* Force raw execution as the independent decode/dispatch reference. */
        arm920t_set_trace(cpu_ref,1,NULL,NULL);
        if(scenario==1) set_mem_both(DATA_ADDR,other|3u);
        if(scenario==2) set_reg_both(1,DATA_ADDR+0x1000u);
        if(scenario==3) {
            set_mem_both(callee+4,0xe2855003u);
            arm920t_invalidate_icache_range(cpu_jit,callee+4,4);
            arm920t_invalidate_icache_range(cpu_ref,callee+4,4);
        }
        if(scenario==4 && mmu) {
            tlb_store_checked(cpu_jit,DATA_ADDR,DATA_ADDR+0x1000u,0xfffu,NULL);
            tlb_store_checked(cpu_ref,DATA_ADDR,DATA_ADDR+0x1000u,0xfffu,NULL);
        }
        if(scenario==6) {
            /* End immediately after the pointer load; never pre-execute callee. */
            CHECK(arm920t_run(cpu_jit,2)==arm920t_run(cpu_ref,2),"load boundary");
            compare_state();CHECK(ref_reg(5)==0u,"callee not executed early");
        } else {
            CHECK(arm920t_run(cpu_jit,64)==arm920t_run(cpu_ref,64),"native-sized budget");
            compare_state();
            gp32_cpu_profile_t profile;arm920t_get_cpu_profile(cpu_jit,&profile);
            if(!cached && scenario!=3 && profile.native_backend)
                CHECK(profile.native_arm_insns>=2u,"native call prefix executed");
        }
        const unsigned budgets[]={1,2,5,13,29};
        for(unsigned i=0;i<GP32_ARRAY_COUNT(budgets);++i) {
            CHECK(arm920t_run(cpu_jit,budgets[i])==arm920t_run(cpu_ref,budgets[i]),"run budget");
            compare_state();
        }
        CHECK(ref_reg(5)==((scenario==1 || scenario==2 || (scenario==4 && mmu))?7u:scenario==3?3u:1u),
              "actual chosen callee executed once");
        CHECK(ref_reg(6)==(scenario==5?0u:1u),"actual saved return respected");
        if(scenario==7) {
            CHECK(gp32_ld32le(bus_ptr(&bus_ref,DATA_ADDR+0x200u,4u))==0u,
                  "IRQ precedes the next callee instruction");
            CHECK(bus_jit.io_count==1u && bus_ref.io_count==1u &&
                  bus_jit.io_acks==1u && bus_ref.io_acks==1u,"one callback and acknowledge");
            CHECK(bus_jit.io_pc[0]==callee+8u && bus_ref.io_pc[0]==callee+8u,
                  "callback observes its own PC+4");
        }
        if(failures) fprintf(stderr,"context cached=%u mmu=%u scenario=%u r5=%u r6=%u pc=%08x\n",cached,mmu,scenario,ref_reg(5),ref_reg(6),arm920t_get_pc(cpu_ref));
        teardown_pair();cases++;
    }
    printf("indirect trace: %u cases, %d failures\n",cases,failures);
    return failures?1:0;
}
