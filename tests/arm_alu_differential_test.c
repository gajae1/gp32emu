/* Deterministic ARMv4 ALU streams exercise dependencies between condition
 * flags, shifts and PC operands. Trace mode with no logger forces exec_arm,
 * so the reference cannot share the decoded ARM cache used by jit=0.
 * This is a backend differential check, not an independent ISA oracle. */
#define main original_jit_test_main
#include "arm_jit_test.c"
#undef main

static uint32_t random_state = 0x91c42ae7u;
static uint32_t random_u32(void) {
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}

int main(void) {
    enum { CASES = 2000, MAX_OPS = 64 };
    uint32_t words[MAX_OPS + 1];
    uint64_t native_insns = 0;
    for (unsigned native = 0; native < 2; ++native) {
        setup_pair();
        arm920t_set_jit(cpu_jit, (int)native);
        for (unsigned k = 0; k < CASES; ++k) {
            char label[80];
            uint32_t seed = random_state;
            snprintf(label, sizeof(label), "alu-stream native=%u case=%u seed=%08x", native, k, seed);
            current_case = label;
            arm920t_reset(cpu_jit, CODE_ADDR);
            arm920t_reset(cpu_ref, CODE_ADDR);
            arm920t_set_trace(cpu_ref, 1, NULL, NULL);
            unsigned n = 8u + random_u32() % (MAX_OPS - 7u);
            for (unsigned r = 0; r < 15; ++r) set_reg_both(r, random_u32());
            uint32_t cpsr = 0xd3u | (random_u32() & 0xf0000000u);
            arm920t_set_cpsr(cpu_jit, cpsr);
            arm920t_set_cpsr(cpu_ref, cpsr);
            for (unsigned i = 0; i < n; ++i) {
                unsigned opcode = random_u32() & 15u;
                unsigned cond = (k & 1u) ? random_u32() % 15u : 14u;
                unsigned rd = random_u32() % 15u;
                unsigned rn = random_u32() & 15u;
                unsigned s = random_u32() & 1u;
                if (opcode >= 8 && opcode <= 11) { s = 1; rd = 0; }
                if (opcode == 13 || opcode == 15) rn = 0;
                unsigned form = random_u32() % 3u;
                uint32_t op2 = 0;
                if (form == 0) op2 = (1u << 25) | (random_u32() & 0xfffu);
                else {
                    unsigned rm = random_u32() & 15u;
                    unsigned type = random_u32() & 3u;
                    op2 = rm | (type << 5);
                    if (form == 1) op2 |= (random_u32() & 31u) << 7;
                    /* Rs=PC is unpredictable; Rd=PC is excluded throughout. */
                    else op2 |= (1u << 4) | ((random_u32() % 15u) << 8);
                }
                words[i] = (cond << 28) | (opcode << 21) | (s << 20) |
                           (rn << 16) | (rd << 12) | op2;
            }
            words[n] = 0xeafffffeu;
            load_both(words, n + 1u);
            if (native) prepare_native_block(cpu_jit, CODE_ADDR);
            uint32_t budget = n + 5u;
            uint64_t before = cpu_jit->prof.native_arm_insns;
            CHECK(arm920t_run(cpu_jit, budget) == arm920t_run(cpu_ref, budget), "budget mismatch");
            native_insns += cpu_jit->prof.native_arm_insns - before;
            compare_state();
            if (failures) {
                for (unsigned i = 0; i < n + 1u; ++i)
                    fprintf(stderr, "%08x: %08x\n", CODE_ADDR + 4u * i, words[i]);
                teardown_pair();
                return 1;
            }
        }
        teardown_pair();
    }
#if ARM920T_NATIVE_BACKEND && !GP32EMU_DISABLE_JIT
    current_case = "alu-stream-coverage";
    CHECK(native_insns > 0u, "no actual native instructions executed");
#endif
    if (failures) return 1;
    printf("PASS: %u seeded ALU programs, native_insns=%" PRIu64 " final_seed=%08x\n",
           CASES * 2u, native_insns, random_state);
    return 0;
}
