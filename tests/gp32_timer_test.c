/* Exercise real guest callbacks while a direct-mode title waits for vblank. */
#include "../src/gp32.c"

static int failures;
#define CHECK(c, msg) do { if (!(c)) { fprintf(stderr, "FAIL: %s (%d)\n", msg, __LINE__); ++failures; } } while (0)

static void run_wait(gp32_t *g, uint32_t budget) {
    g->direct_vblank_wait_cycles = budget;
    CHECK(gp32_run_cycles(g, budget) == GP32_OK, "run idle budget");
    CHECK(g->direct_vblank_wait_cycles == 0u, "consume wait exactly once");
}

static void check_timer(int jit, uint32_t callback_mode) {
    gp32_t *g = gp32_create(NULL);
    CHECK(g != NULL, "create core");
    if (!g) return;
    const uint32_t callback = GP32_RAM_BASE + 0x2000u;
    const uint32_t counter = GP32_RAM_BASE + 0x3000u;
    const uint32_t code[] = {
        0xe59f000cu, /* LDR r0,[pc,#12]: counter address */
        0xe5901000u, /* LDR r1,[r0] */
        0xe2811001u, /* ADD r1,r1,#1 */
        0xe5801000u, /* STR r1,[r0] */
        0xe12fff1eu, /* BX lr: firmware callback-return trap */
        counter,
    };
    const uint32_t mode_code[] = {
        0xe59f001cu, /* LDR r0,[pc,#28]: counter address */
        0xe5901000u, 0xe2811001u, 0xe5801000u,
        0xe3a08055u, /* MOV r8,#0x55: caller must recover shared/FIQ bank */
        0xe1a0200eu, /* MOV r2,lr: preserve return across mode switch */
        0xe321f0c0u | callback_mode, /* MSR cpsr_c: masked IRQ/FIQ mode */
        0xe1a0e002u, /* MOV lr,r2 */
        0xe12fff1eu, /* BX lr */
        counter,
    };
    g->direct_fxe_mode = 1;
    direct_install_stubs(g);
    gp32_set_jit(g, jit);
    const uint32_t *program = callback_mode ? mode_code : code;
    size_t count = callback_mode ? GP32_ARRAY_COUNT(mode_code) : GP32_ARRAY_COUNT(code);
    for (size_t i = 0; i < count; ++i)
        s3c2400_write32(g->soc, callback + (uint32_t)i * 4u, program[i]);
    for (unsigned i = 0; i < 16u; ++i) arm920t_set_reg(g->cpu, i, 0x1000u + i * 4u);
    arm920t_set_cpsr(g->cpu, ARM_MODE_SVC | ARM_I_FLAG | ARM_F_FLAG);
    uint32_t regs[16], cpsr = arm920t_get_cpsr(g->cpu);
    for (unsigned i = 0; i < 16u; ++i) regs[i] = arm920t_get_reg(g->cpu, i);

    g->direct_hle_gpos_timers_enabled = 1u;
    g->direct_hle_gpos_timer[0].configured = 1u;
    g->direct_hle_gpos_timer[0].enabled = 1u;
    g->direct_hle_gpos_timer[0].callback = callback;
    g->direct_hle_gpos_timer[0].tps = 1000u;
    const uint32_t clock = direct_run_clock_hz(g);
    CHECK(clock >= 1000u && clock % 1000u == 0u, "integral millisecond fixture clock");
    run_wait(g, clock / 100u); /* Ten milliseconds, split by the real run loop. */
    CHECK(s3c2400_debug_read32(g->soc, counter) == 10u, "timer fires during vblank wait");
    for (unsigned i = 0; i < 16u; ++i)
        CHECK(arm920t_get_reg(g->cpu, i) == regs[i], "callback restores waiting CPU registers");
    CHECK(arm920t_get_cpsr(g->cpu) == cpsr, "callback restores waiting CPU mode/flags");

    g->direct_hle_gpos_timer[0].enabled = 0u;
    run_wait(g, clock / 100u);
    CHECK(s3c2400_debug_read32(g->soc, counter) == 10u, "disabled timer remains stopped");
    g->direct_hle_gpos_timer[0].enabled = 1u;
    run_wait(g, clock / 1000u - 1u);
    CHECK(s3c2400_debug_read32(g->soc, counter) == 10u, "no early timer firing");
    run_wait(g, 1u);
    CHECK(s3c2400_debug_read32(g->soc, counter) == 11u, "timer remainder survives separate waits");
    gp32_destroy(g);
}

int main(void) {
    for (int jit = 0; jit <= 1; ++jit) {
        check_timer(jit, 0);
        check_timer(jit, 0x12u); /* IRQ banks SP/LR. */
        check_timer(jit, 0x11u); /* FIQ also banks r8-r12. */
    }
    if (failures) return 1;
    puts("PASS: direct GPOS callbacks during vblank wait, disabled timer, split budget and CPU context");
    return 0;
}
