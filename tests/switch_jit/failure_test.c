/* Reuse the CPU differential bus; inject libnx allocation/transition failures. */
#define ARM_JIT_CODE_SIZE (128u * 1024u)
#define GP32EMU_CPU_PROFILE 1
#include "../../src/arm920t.c"
#define main differential_test_main
#include "../arm_jit_test.c"
#undef main

_Static_assert(ARM920T_NATIVE_BACKEND == 2u, "Switch must compile the A64 backend");

static int has_native(uint32_t pc) {
    arm_jit_block_t *set = &cpu_jit->jit_blocks[arm_jit_block_index(pc)];
    for (unsigned i = 0; i < ARM_JIT_BLOCK_WAYS; ++i)
        if (set[i].valid && set[i].tag_pc == pc && set[i].native) return 1;
    return 0;
}

static void run_at(uint32_t pc) {
    set_reg_both(15u, pc);
    set_reg_both(1u, pc + 31u * 4u);
    CHECK(arm920t_run(cpu_jit, 32u) == arm920t_run(cpu_ref, 32u), "failure-path budget");
    compare_state();
}

int main(void) {
    const uint32_t first = RAM_BASE + 0x400u, second = RAM_BASE + 0x800u;
    for (unsigned failure = 0; failure < 3u; ++failure) {
        current_case = "switch-jit-failure";
        setup_pair();
        for (unsigned i = 0; i < 32u; ++i) {
            uint32_t insn = i == 31u ? 0xe1a0f001u : 0xe2800001u;
            set_mem_both(first + 4u * i, insn);
            set_mem_both(second + 4u * i, insn);
        }
        if (failure == 0) switch_test_fail_create = 1;
        else {
            run_at(first);
            CHECK(has_native(first), "warm block was not native");
            if (failure == 1) switch_test_fail_write = 1;
            else switch_test_fail_publish = 1;
        }
        run_at(second);
        CHECK(cpu_jit->jit_memory_failed, "failed permission request was not retained");
        CHECK(!has_native(first) && !has_native(second),
              "unusable RX entry remained callable");
        run_at(first);
        switch_test_fail_create = switch_test_fail_write = switch_test_fail_publish = 0;
        teardown_pair();
        CHECK(switch_test_live_buffers == 0u, "JIT buffer leaked");
    }
    if (failures) return 1;
    puts("PASS: Switch permission failures preserve interpreter state and retire RX entries");
    return 0;
}
