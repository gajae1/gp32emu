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
    direct_hle_gpos_timer_tick(batch, 33000u, direct_run_clock_hz(batch)); /* 100 scheduler ticks. */
    for (unsigned i = 0; i < 100u; ++i) direct_hle_gpos_timer_tick(split, 330u, direct_run_clock_hz(split));
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

static void check_peripheral_clock_boundary(int jit) {
    const uint32_t code = GP32_RAM_BASE + 0x4000u;
    for (unsigned audio = 0; audio < 2u; ++audio) {
        gp32_t *g = gp32_create(NULL);
        CHECK(g != NULL, "create peripheral clock core");
        if (!g) return;
        gp32_set_jit(g, jit);
        s3c2400_write32(g->soc, 0x14800004u, 0u); /* 48 MHz. */
        s3c2400_write32(g->soc, 0x14800014u, 0u);
        unsigned old_cycles = audio ? 250u : 100u;
        for (unsigned i = 0; i < old_cycles - 1u; ++i)
            s3c2400_write32(g->soc, code + i * 4u, 0xe1a00000u); /* NOP */
        s3c2400_write32(g->soc, code + (old_cycles - 1u) * 4u, 0xe5801000u);
        s3c2400_write32(g->soc, code + old_cycles * 4u, 0xeafffffeu);
        arm920t_set_reg(g->cpu, 0, 0x14800014u);
        arm920t_set_reg(g->cpu, 1, 1u); /* PCLK halves, CPU clock stays 48 MHz. */
        arm920t_set_reg(g->cpu, 15, code);
        if (audio) {
            s3c2400_write32(g->soc, 0x14600040u, GP32_RAM_BASE);
            s3c2400_write32(g->soc, 0x14600044u, 0x35508010u);
            s3c2400_write32(g->soc, 0x14600048u, 0x10800008u);
            s3c2400_write32(g->soc, 0x14600058u, 2u);
            s3c2400_write32(g->soc, 0x15508000u, 1u);
            CHECK(gp32_run_cycles(g, 505u) == GP32_OK, "run to just before IIS boundary");
            uint64_t frames;
            uint32_t rate;
            s3c2400_audio_samples(g->soc, &frames, &rate);
            CHECK(frames == 0u, "no IIS frame before old/new clock boundary");
            gp32_run_cycles(g, 1u);
            s3c2400_audio_samples(g->soc, &frames, &rate);
            CHECK(frames == 1u && rate == 93750u, "IIS credits pre-write cycles at old PCLK");
        } else {
            s3c2400_write32(g->soc, 0x1510000cu, 99u); /* 200 cycles before, 400 after. */
            s3c2400_write32(g->soc, 0x15100008u, 9u);
            CHECK(gp32_run_cycles(g, 299u) == GP32_OK, "run to just before PWM boundary");
            CHECK(!(s3c2400_read32(g->soc, 0x14400000u) & (1u << 10)), "no premature PWM IRQ");
            CHECK(s3c2400_read32(g->soc, 0x15100014u) == 0u, "PWM counter includes pre-write progress");
            gp32_run_cycles(g, 1u);
            CHECK(s3c2400_read32(g->soc, 0x14400000u) & (1u << 10), "PWM expires at old/new clock boundary");
            s3c2400_write32(g->soc, 0x14400000u, 1u << 10);
            g->direct_fxe_mode = 1;
            direct_install_stubs(g);
            for (unsigned i = 0; i < 400u; ++i)
                s3c2400_write32(g->soc, code + i * 4u, 0xe1a00000u);
            s3c2400_write32(g->soc, code + 1600u, 0xe12fff1eu); /* BX lr */
            arm920t_flush_jit(g->cpu);
            CHECK(direct_call_guest_callback(g, code), "guest callback returns after hardware tick");
            CHECK(s3c2400_read32(g->soc, 0x14400000u) & (1u << 10), "callback execution advances hardware timer");
        }
        gp32_destroy(g);
    }
}

/* The guest changes its CPU divider at the start of a host frame. A fixed
 * clock/60 budget incorrectly turns the frame into 8.3 or 33.3 ms. */
static void check_frame_clock_change(int jit) {
    for (unsigned up = 0; up < 2u; ++up) {
        gp32_t *g = gp32_create(NULL);
        CHECK(g != NULL, "create frame clock core");
        if (!g) return;
        gp32_set_jit(g, jit);
        const uint32_t code = GP32_RAM_BASE + 0x4000u;
        s3c2400_write32(g->soc, 0x14800004u, 0x3000u);
        s3c2400_write32(g->soc, 0x14800014u, up ? 2u : 0u);
        s3c2400_write32(g->soc, code, 0xe5801000u); /* STR r1,[r0] */
        s3c2400_write32(g->soc, code + 4u, 0xeafffffeu); /* B . */
        arm920t_set_reg(g->cpu, 0, 0x14800014u);
        arm920t_set_reg(g->cpu, 1, up ? 0u : 2u);
        arm920t_set_reg(g->cpu, 15, code);
        CHECK(gp32_run_frame(g) == GP32_OK, "run clock-changing frame");
        CHECK(g->elapsed.nanoseconds >= 16666666u && g->elapsed.nanoseconds < 16666760u,
              "frame stays at 1/60 second across guest clock write");
        /* Save at a fractional frame boundary and compare the continuation,
         * as a run-ahead/rewind frontend would. */
        gp32_t *clone = gp32_create(NULL);
        size_t size = gp32_state_size(g);
        uint8_t *state = malloc(size);
        CHECK(clone && state && gp32_save_state_data(g, state, size) == GP32_OK,
              "save frame pacing");
        if (clone && state) {
            CHECK(gp32_load_state_data(clone, state, size) == GP32_OK, "restore frame pacing");
            gp32_set_jit(clone, jit);
            for (unsigned frame = 1; frame < 60u; ++frame) {
                CHECK(gp32_run_frame(g) == GP32_OK && gp32_run_frame(clone) == GP32_OK,
                      "advance restored frame");
                CHECK(gp32_get_cycles(g) == gp32_get_cycles(clone) &&
                      g->elapsed.nanoseconds == clone->elapsed.nanoseconds &&
                      g->elapsed.remainder == clone->elapsed.remainder &&
                      memcmp(&g->frame_time, &clone->frame_time, sizeof(g->frame_time)) == 0,
                      "restored frame cadence is deterministic");
            }
            CHECK(g->elapsed.nanoseconds >= 1000000000u && g->elapsed.nanoseconds < 1000000100u,
                  "sixty frames remain one second without accumulating cycle overshoot");
        }
        free(state);
        gp32_destroy(clone);
        CHECK(gp32_run_cycles(g, 3u) == GP32_OK && !g->frame_time.valid,
              "explicit CPU stepping rebases frame pacing");
        gp32_destroy(g);
    }
}

static void check_frame_callback_debt(int jit) {
    gp32_t *g = gp32_create(NULL), *clone = gp32_create(NULL);
    CHECK(g && clone, "create callback frame cores");
    if (!g || !clone) { gp32_destroy(g); gp32_destroy(clone); return; }
    g->direct_fxe_mode = 1;
    direct_install_stubs(g);
    gp32_set_jit(g, jit);
    s3c2400_write32(g->soc, 0x14800004u, 0x3000u);
    s3c2400_write32(g->soc, 0x14800014u, 0u);
    const uint32_t callback = GP32_RAM_BASE + 0x2000u;
    const uint32_t code[] = {0xe3a00801u, 0xe2500001u, 0x1afffffdu, 0xe12fff1eu};
    for (unsigned i = 0; i < GP32_ARRAY_COUNT(code); ++i)
        s3c2400_write32(g->soc, callback + i * 4u, code[i]);
    g->direct_hle_gpos_timers_enabled = 1u;
    g->direct_hle_gpos_timer[0].configured = 1u;
    g->direct_hle_gpos_timer[0].enabled = 1u;
    g->direct_hle_gpos_timer[0].callback = callback;
    g->direct_hle_gpos_timer[0].tps = 60u;
    g->direct_vblank_wait_cycles = 6600000u;
    CHECK(gp32_run_frame(g) == GP32_OK, "run callback at frame deadline");
    CHECK(g->elapsed.nanoseconds > 18000000u && g->elapsed.nanoseconds < 24000000u,
          "real guest callback crosses frame deadline");
    g->direct_hle_gpos_timer[0].enabled = 0u;
    size_t size = gp32_state_size(g);
    uint8_t *state = malloc(size);
    CHECK(state && gp32_save_state_data(g, state, size) == GP32_OK, "save callback overshoot");
    if (state) {
        CHECK(gp32_load_state_data(clone, state, size) == GP32_OK, "load callback overshoot");
        CHECK(gp32_run_frame(g) == GP32_OK && gp32_run_frame(clone) == GP32_OK,
              "repay callback time in next frame");
        CHECK(g->elapsed.nanoseconds >= 33333333u && g->elapsed.nanoseconds < 33333400u &&
              g->elapsed.nanoseconds == clone->elapsed.nanoseconds &&
              gp32_get_cycles(g) == gp32_get_cycles(clone),
              "callback time is not added again after state restore");
        free(state);
    }
    CHECK(gp32_reset(g) == GP32_OK && !g->frame_time.valid, "reset clears frame deadline");
    gp32_destroy(g);
    gp32_destroy(clone);
}

/* Peripheral writes must not retroactively change the elapsed part of a
 * CPU slice. Compare a single run against instruction-sized boundaries. */
static void check_audio_write_boundary(int jit) {
    for (unsigned mode = 0; mode < 4u; ++mode) {
        gp32_t *g[2] = {gp32_create(NULL), gp32_create(NULL)};
        CHECK(g[0] && g[1], "create audio write boundary cores");
        if (!g[0] || !g[1]) { gp32_destroy(g[0]); gp32_destroy(g[1]); return; }
        for (unsigned split = 0; split < 2u; ++split) {
            gp32_t *m = g[split];
            gp32_set_jit(m, jit);
            s3c2400_write32(m->soc, 0x14800004u, 0u); /* 48 MHz, 500 cycles/sample */
            s3c2400_write32(m->soc, 0x14800014u, 0u);
            const uint32_t code = GP32_RAM_BASE + 0x4000u;
            for (unsigned i = 0; i < 600u; ++i)
                s3c2400_write32(m->soc, code + i * 4u, i == 599u ? 0xe5801000u : 0xe1a00000u);
            s3c2400_write32(m->soc, GP32_RAM_BASE, 0x12345678u);
            s3c2400_write32(m->soc, 0x14600040u, GP32_RAM_BASE);
            s3c2400_write32(m->soc, 0x14600044u, 0x35508010u);
            s3c2400_write32(m->soc, 0x14600048u, 0x10800008u);
            s3c2400_write32(m->soc, 0x14600058u, 2u);
            s3c2400_write32(m->soc, 0x15508000u, mode == 1u ? 0u : 1u);
            const uint32_t addr[] = {0x15508000u, 0x15508000u, 0x14600058u, 0x15508008u};
            const uint32_t value[] = {0u, 1u, 4u, 32u}; /* stop/start IIS, stop DMA, divider */
            arm920t_set_reg(m->cpu, 0, addr[mode]);
            arm920t_set_reg(m->cpu, 1, value[mode]);
            arm920t_set_reg(m->cpu, 15, code);
            if (split) for (unsigned i = 0; i < 600u; ++i) gp32_run_cycles(m, 1u);
            else gp32_run_cycles(m, 600u);
        }
        gp32_audio_desc_t a, b;
        gp32_get_audio(g[0], &a); gp32_get_audio(g[1], &b);
        CHECK(a.frame_count == b.frame_count && a.sample_rate_hz == b.sample_rate_hz,
              "audio control write preserves pre-write samples/rate");
        CHECK(b.frame_count == (mode == 1u ? 0u : 1u), "instruction-step audio boundary oracle");
        if (a.frame_count == b.frame_count && a.frame_count)
            CHECK(memcmp(a.samples_s16_interleaved, b.samples_s16_interleaved,
                         (size_t)a.frame_count * 4u) == 0, "audio boundary PCM exact");
        CHECK(s3c2400_read32(g[0]->soc, 0x1460004cu) == s3c2400_read32(g[1]->soc, 0x1460004cu),
              "DMA count independent of CPU slice size");
        gp32_destroy(g[0]); gp32_destroy(g[1]);
    }
}

/* Exercise the largest store instruction, including the bus's byte-lane
 * fallback, against immediate writes with the audio clock disabled. */
static void check_dma_store_lanes(int jit) {
    for (unsigned unaligned = 0; unaligned <= 1u; ++unaligned) {
        gp32_t *g[2] = {gp32_create(NULL), gp32_create(NULL)};
        CHECK(g[0] && g[1], "create DMA store lane cores");
        if (!g[0] || !g[1]) { gp32_destroy(g[0]); gp32_destroy(g[1]); return; }
        for (unsigned direct = 0; direct < 2u; ++direct) {
            gp32_t *m = g[direct];
            gp32_set_jit(m, jit);
            const uint32_t code = GP32_RAM_BASE + 0x4000u;
            s3c2400_write32(m->soc, code, 0xe880ffffu); /* STMIA r0,{r0-r15} */
            for (unsigned r = 0; r < 15u; ++r) arm920t_set_reg(m->cpu, r, 0u);
            arm920t_set_reg(m->cpu, 0, 0x14600000u + unaligned);
            arm920t_set_reg(m->cpu, 15, code);
            if (direct) arm920t_run(m->cpu, 1u);
            else s3c2400_run_cpu(m->soc, 1u);
        }
        for (unsigned off = 0; off <= 64u; off += 4u)
            CHECK(s3c2400_read32(g[0]->soc, 0x14600000u + off) ==
                  s3c2400_read32(g[1]->soc, 0x14600000u + off),
                  "deferred STM preserves every DMA store lane");
        gp32_destroy(g[0]); gp32_destroy(g[1]);
    }
}

int main(void) {
    check_scheduler_catchup();
    for (int jit = 0; jit <= 1; ++jit) {
        check_audio_write_boundary(jit);
        check_dma_store_lanes(jit);
        check_frame_clock_change(jit);
        check_frame_callback_debt(jit);
        check_peripheral_clock_boundary(jit);
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
