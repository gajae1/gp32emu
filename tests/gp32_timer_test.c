/* Exercise real guest callbacks while a direct-mode title waits for vblank. */
#ifndef GP32_SOURCE
#define GP32_SOURCE "../src/gp32.c"
#endif
#include GP32_SOURCE

static int failures;
#define CHECK(c, msg) do { if (!(c)) { fprintf(stderr, "FAIL: %s (%d)\n", msg, __LINE__); ++failures; } } while (0)

static void run_wait(gp32_t *g, uint32_t budget) {
    g->direct_vblank_wait_cycles = budget;
    CHECK(gp32_run_cycles(g, budget) == GP32_OK, "run idle budget");
    CHECK(g->direct_vblank_wait_cycles == 0u, "consume wait exactly once");
}

static void check_timer(int jit, uint32_t callback_mode, int thumb) {
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
    const uint16_t thumb_code[] = {
        0x4802u, /* LDR r0,[pc,#8]: literal at callback + 12 */
        0x6801u, /* LDR r1,[r0] */
        0x3101u, /* ADD r1,#1 */
        0x6001u, /* STR r1,[r0] */
        0x4770u, /* BX lr: return to the ARM firmware trap */
        0x46c0u, /* NOP: align literal */
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
    if (thumb) {
        for (size_t i = 0; i < GP32_ARRAY_COUNT(thumb_code); ++i)
            s3c2400_write16(g->soc, callback + (uint32_t)i * 2u, thumb_code[i]);
        s3c2400_write32(g->soc, callback + 12u, counter);
    }
    for (unsigned i = 0; i < 16u; ++i) arm920t_set_reg(g->cpu, i, 0x1000u + i * 4u);
    arm920t_set_cpsr(g->cpu, ARM_MODE_SVC | ARM_I_FLAG | ARM_F_FLAG);
    uint32_t regs[16], cpsr = arm920t_get_cpsr(g->cpu);
    for (unsigned i = 0; i < 16u; ++i) regs[i] = arm920t_get_reg(g->cpu, i);

    g->direct_hle_gpos_timers_enabled = 1u;
    g->direct_hle_gpos_timer[0].configured = 1u;
    g->direct_hle_gpos_timer[0].enabled = 1u;
    g->direct_hle_gpos_timer[0].callback = callback | (thumb ? 1u : 0u);
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

static void check_callback_starts_timer(int jit, unsigned starter, int reconfigure) {
    gp32_t *g = gp32_create(NULL);
    CHECK(g != NULL, "create timer mutation core");
    if (!g) return;
    const unsigned target = 1u - starter;
    const uint32_t start_fn = GP32_RAM_BASE + 0x2000u;
    const uint32_t count_fn = GP32_RAM_BASE + 0x4000u;
    const uint32_t command = GP32_RAM_BASE + 0x6000u;
    const uint32_t counter = GP32_RAM_BASE + 0x7000u;
    const uint32_t start_code[] = {0xe59f0004u, 0xef000013u, 0xe12fff1eu, command};
    const uint32_t restart_code[] = {
        0xe59f000cu, 0xef000013u, 0xe59f0008u, 0xef000013u, 0xe12fff1eu,
        command, command + 20u,
    };
    const uint32_t count_code[] = {
        0xe59f000cu, 0xe5901000u, 0xe2811001u, 0xe5801000u, 0xe12fff1eu, counter,
    };
    g->direct_fxe_mode = 1;
    direct_install_stubs(g);
    gp32_set_jit(g, jit);
    s3c2400_write32(g->soc, 0x14800004u, 0x3000u);
    s3c2400_write32(g->soc, 0x14800014u, 0u);
    CHECK(direct_run_clock_hz(g) == 66000000u, "66 MHz timer fixture");
    for (size_t i = 0; i < GP32_ARRAY_COUNT(start_code); ++i)
        s3c2400_write32(g->soc, start_fn + (uint32_t)i * 4u, start_code[i]);
    if (reconfigure)
        for (size_t i = 0; i < GP32_ARRAY_COUNT(restart_code); ++i)
            s3c2400_write32(g->soc, start_fn + (uint32_t)i * 4u, restart_code[i]);
    for (size_t i = 0; i < GP32_ARRAY_COUNT(count_code); ++i)
        s3c2400_write32(g->soc, count_fn + (uint32_t)i * 4u, count_code[i]);
    for (unsigned i = 0; i < 2u; ++i) {
        g->direct_hle_gpos_timer[i].configured = 1u;
        g->direct_hle_gpos_timer[i].enabled = i == starter || reconfigure;
        g->direct_hle_gpos_timer[i].tps = 1000u;
        g->direct_hle_gpos_timer[i].callback = i == starter ? start_fn : count_fn;
    }
    g->direct_hle_gpos_timers_enabled = 1u;
    s3c2400_write32(g->soc, command, 4u); /* Guest callback starts the other slot. */
    s3c2400_write32(g->soc, command + 4u, target);
    if (reconfigure) {
        s3c2400_write32(g->soc, command, 1u); /* Same function/rate, new timer lifetime. */
        s3c2400_write32(g->soc, command + 8u, count_fn);
        s3c2400_write32(g->soc, command + 12u, 1000u);
        s3c2400_write32(g->soc, command + 16u, 0u);
        s3c2400_write32(g->soc, command + 20u, 4u);
        s3c2400_write32(g->soc, command + 24u, target);
    }
    run_wait(g, 66000u); /* Last 464-cycle slice invokes the starter. */
    g->direct_hle_gpos_timer[starter].enabled = 0u;
    CHECK(g->direct_hle_gpos_timer[target].enabled, "guest SWI starts target timer");
    CHECK(s3c2400_debug_read32(g->soc, counter) == 0u, "new timer does not inherit pending expiry");
    CHECK(g->direct_hle_gpos_timer[target].accum == 0u, "new timer gets no pre-start time");
    run_wait(g, 65536u);
    CHECK(s3c2400_debug_read32(g->soc, counter) == 0u, "started timer does not expire early");
    run_wait(g, 464u);
    CHECK(s3c2400_debug_read32(g->soc, counter) == 1u, "started timer expires after full period");
    gp32_destroy(g);
}

static void check_halfword_thumb_callback(int jit) {
    gp32_t *g = gp32_create(NULL);
    CHECK(g != NULL, "create halfword Thumb callback core");
    if (!g) return;
    const uint32_t callback = GP32_RAM_BASE + 0x2002u;
    const uint32_t counter = GP32_RAM_BASE + 0x3000u;
    const uint32_t caller_pc = GP32_RAM_BASE + 0x5002u;
    g->direct_fxe_mode = 1;
    direct_install_stubs(g);
    gp32_set_jit(g, jit);
    s3c2400_write16(g->soc, callback - 2u, 0x4770u); /* BX lr: wrong aligned entry. */
    s3c2400_write16(g->soc, callback, 0x204du);      /* MOV r0,#77 */
    s3c2400_write16(g->soc, callback + 2u, 0x6008u); /* STR r0,[r1] */
    s3c2400_write16(g->soc, callback + 4u, 0x4770u); /* BX lr */
    arm920t_set_cpsr(g->cpu, ARM_MODE_SVC | ARM_I_FLAG | ARM_F_FLAG | ARM_T_FLAG);
    /* Reach a halfword-only PC through execution, independently of set_reg. */
    s3c2400_write16(g->soc, caller_pc - 2u, 0x46c0u); /* NOP */
    arm920t_set_reg(g->cpu, 15, caller_pc - 2u);
    CHECK(arm920t_run(g->cpu, 1u) == 1u, "advance Thumb caller one instruction");
    CHECK(arm920t_get_pc(g->cpu) == caller_pc, "fixture reaches halfword caller PC");
    uint32_t cpsr = arm920t_get_cpsr(g->cpu);
    uint64_t before = arm920t_get_cycles(g->cpu);
    CHECK(direct_call_guest_function3(g, callback | 1u, 0u, counter, 0u), "Thumb callback returns");
    CHECK(arm920t_get_cycles(g->cpu) - before == 4u, "callback stops after three Thumb instructions and return trap");
    CHECK(s3c2400_debug_read32(g->soc, counter) == 77u, "callback starts at exact Thumb halfword");
    CHECK(arm920t_get_pc(g->cpu) == caller_pc, "callback restores exact Thumb caller PC");
    CHECK(arm920t_get_cpsr(g->cpu) == cpsr, "callback preserves Thumb caller state");
    arm920t_set_cpsr(g->cpu, cpsr & ~ARM_T_FLAG);
    arm920t_set_reg(g->cpu, 15, caller_pc | 1u);
    CHECK(arm920t_get_pc(g->cpu) == (caller_pc & ~3u), "ARM PC remains word aligned");
    before = arm920t_get_cycles(g->cpu);
    CHECK(arm920t_run(g->cpu, 1u) == 1u, "caller can execute after callback yield");
    CHECK(arm920t_get_cycles(g->cpu) - before == 1u, "callback yield does not leave CPU halted");
    gp32_destroy(g);
}

static void check_callback_register_banks(int jit, uint32_t caller_mode) {
    gp32_t *g = gp32_create(NULL);
    CHECK(g != NULL, "create callback register-bank core");
    if (!g) return;
    const uint32_t callback = GP32_RAM_BASE + 0x2000u;
    const uint32_t counter = GP32_RAM_BASE + 0x3000u;
    const uint32_t code[] = {
        0xe1a0300eu, /* MOV r3,lr: return across arbitrary mode changes. */
        0xe5801000u, /* STR r1,[r0]: callback memory effects must survive. */
        0xe16ff001u, /* MSR SPSR_fsxc,r1: overwrite suspended SVC status. */
        0xe321f0d1u, /* MSR CPSR_c,#0xd1: FIQ */
        0xe3a08088u, 0xe3a0d066u, 0xe16ff001u,
        0xe321f0d7u, /* ABT */
        0xe3a0d055u, 0xe16ff001u,
        0xe321f0dbu, /* UND */
        0xe3a0d044u, 0xe16ff001u,
        0xe321f0d2u, /* IRQ */
        0xe3a08077u, 0xe3a0d033u, 0xe16ff001u,
        0xe12fff13u, /* BX r3: return trap. */
    };
    g->direct_fxe_mode = 1;
    direct_install_stubs(g);
    gp32_set_jit(g, jit);
    for (size_t i = 0; i < GP32_ARRAY_COUNT(code); ++i)
        s3c2400_write32(g->soc, callback + (uint32_t)i * 4u, code[i]);
    const uint32_t modes[] = {0x1fu, 0x11u, 0x13u, 0x17u, 0x12u, 0x1bu};
    for (size_t i = 0; i < GP32_ARRAY_COUNT(modes); ++i) {
        arm920t_set_cpsr(g->cpu, modes[i] | ARM_I_FLAG | ARM_F_FLAG);
        for (unsigned r = 8u; r <= 14u; ++r)
            arm920t_set_reg(g->cpu, r, 0x10000u * modes[i] + r * 4u);
    }
    arm920t_set_cpsr(g->cpu, caller_mode | ARM_I_FLAG | ARM_F_FLAG);
    /* Canonicalize the active bank cache before comparing architectural state. */
    arm920t_set_cpsr(g->cpu, 0x1fu | ARM_I_FLAG | ARM_F_FLAG);
    arm920t_set_cpsr(g->cpu, caller_mode | ARM_I_FLAG | ARM_F_FLAG);
    arm920t_state_image_t before, after;
    state_io_t out = state_io_writer(&before, sizeof(before));
    CHECK(arm920t_state_save_io(g->cpu, &out), "capture complete caller register state");
    CHECK(direct_call_guest_function3(g, callback, counter, 0xa0000030u, 0u),
          "mode-changing callback returns");
    uint32_t returned_cpsr = arm920t_get_cpsr(g->cpu);
    arm920t_set_cpsr(g->cpu, (returned_cpsr & ~0x1fu) | 0x1fu);
    arm920t_set_cpsr(g->cpu, returned_cpsr);
    out = state_io_writer(&after, sizeof(after));
    CHECK(arm920t_state_save_io(g->cpu, &out), "capture callback result state");
    CHECK(!memcmp(before.bank_svc, after.bank_svc, sizeof(before.bank_svc)),
          "private callback stack does not leak into inactive SVC bank");
    CHECK(!memcmp(&before, &after, offsetof(arm920t_state_image_t, cp15)),
          "callback restores all register banks and saved status registers");
    CHECK(s3c2400_debug_read32(g->soc, counter) == 0xa0000030u,
          "register restoration retains callback RAM writes");
    CHECK(after.cycles_total > before.cycles_total, "register restoration retains executed CPU cycles");
    gp32_destroy(g);
}

static gp32_t *scheduler_fixture(void) {
    gp32_t *g = gp32_create(NULL);
    if (!g) return NULL;
    const uint32_t callback = GP32_RAM_BASE + 0x2000u;
    const uint32_t tasks = GP32_RAM_BASE + 0x4000u;
    const uint32_t entry = GP32_RAM_BASE + 0x6000u;
    const uint32_t limits[] = {80u, 200u, UINT32_MAX};
    const uint32_t elapsed[] = {0u, 7u, UINT32_MAX};
    g->direct_fxe_mode = 1;
    s3c2400_write32(g->soc, 0x14800004u, 0x3000u);
    s3c2400_write32(g->soc, 0x14800014u, 0u);
    g->direct_hle_gpos_scheduler_callback = callback;
    g->direct_hle_gpos_task_first = tasks;
    g->direct_hle_gpos_task_last = tasks + 2u * 0x34u;
    g->direct_hle_gpos_timers_enabled = 1;
    g->direct_hle_gpos_timer[0].configured = 1;
    g->direct_hle_gpos_timer[0].enabled = 1;
    g->direct_hle_gpos_timer[0].callback = callback;
    g->direct_hle_gpos_timer[0].tps = 200000u;
    for (unsigned i = 0; i < 3u; ++i) {
        uint32_t task = tasks + i * 0x34u;
        uint32_t stack = GP32_RAM_BASE + 0x8000u + i * 0x100u;
        s3c2400_write32(g->soc, task, stack);
        s3c2400_write32(g->soc, task + 0x14u, 4u);
        s3c2400_write32(g->soc, task + 0x20u, elapsed[i]);
        s3c2400_write32(g->soc, task + 0x24u, limits[i]);
        s3c2400_write32(g->soc, task + 0x30u, entry);
        s3c2400_write32(g->soc, stack + 60u, entry);
    }
    return g;
}

static void check_scheduler_catchup(void) {
    gp32_t *batch = scheduler_fixture(), *split = scheduler_fixture();
    CHECK(batch && split, "create scheduler catchup cores");
    if (!batch || !split) { gp32_destroy(batch); gp32_destroy(split); return; }
    CHECK(direct_run_clock_hz(batch) == 66000000u, "scheduler fixture clock");
    /* The same elapsed time must wake the same tasks regardless of slicing. */
    direct_hle_gpos_timer_tick(batch, 33000u); /* 100 scheduler ticks. */
    for (unsigned i = 0; i < 100u; ++i) direct_hle_gpos_timer_tick(split, 330u);
    uint32_t tasks = GP32_RAM_BASE + 0x4000u;
    CHECK(s3c2400_debug_read32(batch->soc, tasks + 0x14u) == 2u,
          "scheduler does not discard ticks beyond 64");
    CHECK(s3c2400_debug_read32(batch->soc, tasks + 0x34u + 0x20u) == 107u,
          "sleeping task retains every elapsed tick");
    CHECK(s3c2400_debug_read32(batch->soc, tasks + 2u * 0x34u + 0x14u) == 2u,
          "expired maximum counter wakes without wraparound");
    for (unsigned i = 0; i < 3u * 0x34u; i += 4u)
        CHECK(s3c2400_debug_read32(batch->soc, tasks + i) ==
              s3c2400_debug_read32(split->soc, tasks + i), "scheduler slice equivalence");
    gp32_destroy(batch);
    gp32_destroy(split);
}

static void check_elapsed_clock_change(int jit) {
    gp32_t *g = gp32_create(NULL);
    gp32_t *restored = gp32_create(NULL);
    CHECK(g && restored, "create elapsed-time cores");
    if (!g || !restored) { gp32_destroy(g); gp32_destroy(restored); return; }
    const uint32_t code = GP32_RAM_BASE + 0x4000u;
    const uint32_t divider = 0x14800014u;
    g->direct_fxe_mode = 1;
    gp32_set_jit(g, jit);
    s3c2400_write32(g->soc, 0x14800004u, 0x3000u);
    s3c2400_write32(g->soc, divider, 0u);
    run_wait(g, 6600000u);
    CHECK(direct_elapsed_ms(g) == 100u, "initial 100 ms at 66 MHz");
    s3c2400_write32(g->soc, code, 0xe5801000u); /* STR r1,[r0]: change clock */
    s3c2400_write32(g->soc, code + 4u, 0xeafffffeu); /* B . */
    arm920t_set_reg(g->cpu, 0, divider);
    arm920t_set_reg(g->cpu, 1, 2u);
    arm920t_set_reg(g->cpu, 15, code);
    CHECK(gp32_run_cycles(g, 330001u) == GP32_OK, "guest clock decrease and 10 ms");
    CHECK(direct_run_clock_hz(g) == 33000000u && direct_elapsed_ms(g) == 110u,
          "old time preserved and slice split at clock write");
    size_t size = gp32_state_size(g);
    uint8_t *state = malloc(size);
    CHECK(state && gp32_save_state_data(g, state, size) == GP32_OK, "save divided-clock elapsed time");
    if (state) {
        CHECK(gp32_load_state_data(restored, state, size) == GP32_OK, "restore elapsed time");
        CHECK(direct_elapsed_ms(restored) == 110u, "restored time retains prior clock history");
        run_wait(restored, 330000u);
        CHECK(direct_elapsed_ms(restored) == 120u, "restored time advances at loaded clock");
        free(state);
    }
    arm920t_set_reg(g->cpu, 1, 0u);
    arm920t_set_reg(g->cpu, 15, code);
    CHECK(gp32_run_cycles(g, 660001u) == GP32_OK, "guest clock increase and 10 ms");
    CHECK(direct_elapsed_ms(g) == 120u, "clock increase never rewinds elapsed time");
    CHECK(gp32_reset(g) == GP32_OK && direct_elapsed_ms(g) == 0u, "reset clears firmware time");

    g->direct_fxe_mode = 1;
    direct_install_stubs(g);
    s3c2400_write32(g->soc, 0x14800004u, 0x3000u);
    s3c2400_write32(g->soc, divider, 0u);
    run_wait(g, 6600000u);
    const uint32_t callback[] = {0xe5801000u, 0xe2522001u, 0x1afffffdu, 0xe12fff1eu};
    for (unsigned i = 0; i < GP32_ARRAY_COUNT(callback); ++i)
        s3c2400_write32(g->soc, code + i * 4u, callback[i]);
    arm920t_flush_jit(g->cpu);
    CHECK(direct_call_guest_function3(g, code, divider, 2u, 33000u), "callback changes clock and returns");
    CHECK(direct_elapsed_ms(g) == 102u, "callback time uses both clock domains");
    const uint32_t toggles[] = {0xe5801000u, 0xe2211002u, 0xe2522001u, 0x1afffffbu, 0xe12fff1eu};
    for (unsigned i = 0; i < GP32_ARRAY_COUNT(toggles); ++i)
        s3c2400_write32(g->soc, code + i * 4u, toggles[i]);
    arm920t_flush_jit(g->cpu);
    CHECK(direct_call_guest_function3(g, code, divider, 0u, 300u),
          "frequent clock yields do not prematurely exhaust callback budget");
    gp32_destroy(g);
    gp32_destroy(restored);
}

int main(void) {
    check_scheduler_catchup();
    for (int jit = 0; jit <= 1; ++jit) {
        check_elapsed_clock_change(jit);
        check_timer(jit, 0, 0);
        check_timer(jit, 0x12u, 0); /* IRQ banks SP/LR. */
        check_timer(jit, 0x11u, 0); /* FIQ also banks r8-r12. */
        check_timer(jit, 0, 1); /* Thumb callback, ARM caller and return trap. */
        check_callback_starts_timer(jit, 0u, 0);
        check_callback_starts_timer(jit, 1u, 0);
        check_callback_starts_timer(jit, 0u, 1);
        check_halfword_thumb_callback(jit);
        check_callback_register_banks(jit, 0x13u);
        check_callback_register_banks(jit, 0x11u);
    }
    if (failures) return 1;
    puts("PASS: direct GPOS callbacks during vblank wait, disabled timer, split budget and CPU context");
    return 0;
}
