/* Real guest wait instructions, normal CPU/hardware, both execution backends. */
#include "../src/gp32.c"

static int failures;
#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "FAIL: %s (%d)\n", m, __LINE__); ++failures; } } while (0)
static const uint32_t fn = GP32_RAM_BASE + 0x2000u, marker = GP32_RAM_BASE + 0x4000u;
static const uint32_t surface = GP32_RAM_BASE + 0x5000u, caller = GP32_RAM_BASE + 0x20000u;

static void code(gp32_t *g, uint32_t addr, const uint32_t *words, size_t n) {
    for (size_t i = 0; i < n; ++i) s3c2400_write32(g->soc, addr + (uint32_t)i * 4u, words[i]);
}
static gp32_t *fixture(int jit, uint64_t time) {
    gp32_t *g = gp32_create(NULL);
    CHECK(g != NULL, "create wait core"); if (!g) return NULL;
    direct_set_fxe_mode(g, 1u); direct_install_stubs(g); gp32_set_jit(g, jit);
    s3c2400_write32(g->soc, 0x14800004u, 0x3000u); s3c2400_write32(g->soc, 0x14800014u, 0u);
    g->elapsed = (gp32_elapsed_time_t){time, 0u, 66000000u}; direct_update_fw_tick(g);
    s3c2400_write32(g->soc, surface, GP32_RAM_BASE + 0x40000u);
    s3c2400_write32(g->soc, surface + 4u, 16u);
    s3c2400_write32(g->soc, surface + 8u, 320u); s3c2400_write32(g->soc, surface + 12u, 240u);
    s3c2400_write32(g->soc, caller, 0xeafffffeu);
    arm920t_set_cpsr(g->cpu, 0xa00000d3u);
    for (unsigned i = 0; i < 13u; ++i) arm920t_set_reg(g->cpu, i, 0x11110000u + i);
    arm920t_set_reg(g->cpu, 13u, GP32_RAM_BASE + 0x1f000u);
    arm920t_set_reg(g->cpu, 14u, caller); arm920t_set_reg(g->cpu, 15u, caller);
    return g;
}
static void wait_callback(gp32_t *g, int unmask) {
    const uint32_t words[] = {
        0xe92d4000u, 0xe59f0020u, 0xe59fc020u, 0xe1a0e00fu, 0xe12fff1cu,
        0xe8bd4000u, 0xe59f1014u, 0xe3a0004du, 0xe5810000u, 0xe12fff1eu, 0xeafffffeu,
        surface, direct_stub_addr(g), marker,
    };
    if (unmask) s3c2400_write32(g->soc, fn, 0xe321f013u);
    code(g, fn + (unmask ? 4u : 0u), words, GP32_ARRAY_COUNT(words));
}
static void start_timer(gp32_t *g) {
    g->direct_hle_gpos_timers_enabled = 1u;
    g->direct_hle_gpos_timer[0].configured = g->direct_hle_gpos_timer[0].enabled = 1u;
    g->direct_hle_gpos_timer[0].callback = fn; g->direct_hle_gpos_timer[0].tps = 1000u;
    g->direct_hle_gpos_timer[0].accum = 65984000u;
    CHECK(gp32_run_cycles(g, 16u) == GP32_OK && g->direct_hle_callback_running, "real timer consumer starts with no extra budget");
    g->direct_hle_gpos_timer[0].enabled = 0u;
}
static int arm_wait(gp32_t *g) {
    for (unsigned i = 0; i < 200u && gp32_get_pc(g) != direct_stub_addr(g) + 0x30u; ++i)
        if (gp32_run_cycles(g, 1u) != GP32_OK) return 0;
    return gp32_get_pc(g) == direct_stub_addr(g) + 0x30u;
}
static uint64_t call_deadline(gp32_t *g) {
    return ((uint64_t)gp32_get_cpu_reg(g, 2u) << 32) | gp32_get_cpu_reg(g, 1u);
}
static uint8_t *save(gp32_t *g, size_t *size) {
    gp32_clear_audio(g); *size = gp32_state_size(g); uint8_t *data = malloc(*size);
    CHECK(data && gp32_save_state_data(g, data, *size) == GP32_OK, "save suspended wait"); return data;
}
static int finish(gp32_t *g) {
    for (unsigned i = 0; i < 100000u && g->direct_hle_callback_running; ++i) {
        uint32_t pc = gp32_get_pc(g), base = direct_stub_addr(g);
        /* Use small budgets near the real return so no new foreground audio
         * interval follows the trap in this consumer-tail comparison. */
        int polling = pc >= base + 0x30u && pc <= base + 0x50u;
        uint64_t deadline = ((uint64_t)gp32_get_cpu_reg(g, 2u) << 32) | gp32_get_cpu_reg(g, 1u);
        uint64_t delta = deadline - g->elapsed.nanoseconds;
        uint32_t budget = polling && delta > 120000u && delta < (UINT64_C(1) << 63) ? 4096u : 1u;
        if (gp32_run_cycles(g, budget) != GP32_OK) return 0;
    }
    return !g->direct_hle_callback_running && gp32_get_pc(g) == caller;
}
static void check_callback_wait(int jit, uint64_t origin, int change_clock, const char *path) {
    gp32_t *g = fixture(jit, origin), *clone = fixture(jit, 0u);
    if (!g || !clone) { gp32_destroy(g); gp32_destroy(clone); return; }
    wait_callback(g, 0); start_timer(g); CHECK(arm_wait(g), "display wait inside callback is legal");
    uint64_t armed = g->elapsed.nanoseconds, deadline = call_deadline(g);
    CHECK(deadline - armed == 16666666u, "deadline starts after actual SWI time");
    uint64_t cycles = gp32_get_cycles(g);
    CHECK(gp32_run_cycles(g, 64u) == GP32_OK && gp32_get_cycles(g) - cycles < 100u &&
          g->direct_hle_callback_running && !s3c2400_debug_read32(g->soc, marker), "public budget suspends wait without false success");
    if (change_clock) {
        s3c2400_write32(g->soc, 0x14800004u, 0xe000u); s3c2400_write32(g->soc, 0x14800014u, 2u);
        CHECK(gp32_get_run_clock_hz(g) == 48000000u, "restored bus/run clock fixture");
        CHECK(call_deadline(g) == deadline, "clock change preserves per-call time deadline");
    }
    size_t size = 0; uint8_t *image = save(g, &size);
    if (image) {
        CHECK(!memcmp(image, "GP32STATEv0014", 14u), "v14 combined state writer");
        CHECK(gp32_load_state_data(clone, image, size) == GP32_OK, "restore pending display/callback");
        CHECK(gp32_save_state(g, path) == GP32_OK && gp32_load_state(clone, path) == GP32_OK, "file wait continuation roundtrip");
        CHECK(gp32_get_pc(g) == gp32_get_pc(clone) && call_deadline(clone) == deadline, "restore live guest deadline without rearming");
        CHECK(finish(g) && finish(clone), "return through display epilogue and real callback trap");
        CHECK(s3c2400_debug_read32(g->soc, marker) == 77u && s3c2400_debug_read32(clone->soc, marker) == 77u,
              "guest tail executes once on both machines");
        CHECK(gp32_get_cycles(g) == gp32_get_cycles(clone) && g->elapsed.nanoseconds == clone->elapsed.nanoseconds,
              "suspension/restore neither loses nor repeats wait time");
        CHECK(g->elapsed.nanoseconds - armed >= 16666666u && g->elapsed.nanoseconds - armed < 17000000u,
              "wide deadline works across low/full counter wrap");
        CHECK(gp32_get_cpsr(g) == 0xa00000d3u && gp32_get_cpu_reg(g, 8u) == 0x11110008u,
              "real trap restores foreground flags/registers");
        printf("callback-wait jit=%d origin=%" PRIu64 " clock=%u elapsed=%" PRIu64 " cycles=%" PRIu64 "\n",
               jit, origin, gp32_get_run_clock_hz(g), g->elapsed.nanoseconds - origin, gp32_get_cycles(g));
    }
    CHECK(gp32_reset(clone) == GP32_OK && !clone->direct_vblank_time.valid && !clone->direct_callback.owner &&
          !clone->frame_time.valid, "reset clears wait/callback/frame ownership");
    free(image); gp32_destroy(g); gp32_destroy(clone);
}

static void check_foreground_and_invalid(int jit) {
    for (unsigned invalid = 0; invalid < 2u; ++invalid) {
        gp32_t *g = fixture(jit, 0u); if (!g) return;
        arm920t_set_reg(g->cpu, 0u, invalid ? 0u : surface); arm920t_set_reg(g->cpu, 15u, direct_stub_addr(g));
        CHECK(arm_wait(g), "foreground reaches private display service");
        uint64_t start = g->elapsed.nanoseconds;
        uint64_t deadline = call_deadline(g);
        for (unsigned i = 0; i < 1000u && direct_time_pending(deadline, g->elapsed.nanoseconds); ++i)
            CHECK(gp32_run_cycles(g, 4096u) == GP32_OK, "pay for foreground wait from public cycles");
        CHECK(!direct_time_pending(deadline, g->elapsed.nanoseconds), "bounded foreground deadline progress");
        for (unsigned i = 0; i < 200u && gp32_get_pc(g) != caller; ++i) {
            CHECK(gp32_run_cycles(g, 1u) == GP32_OK, "step normal display instructions");
        }
        printf("foreground jit=%d invalid=%u pc=%08x sp=%08x cpsr=%08x elapsed=%" PRIu64 "\n",
               jit, invalid, gp32_get_pc(g), gp32_get_cpu_reg(g, 13u), gp32_get_cpsr(g), g->elapsed.nanoseconds - start);
        CHECK(gp32_get_pc(g) == caller && gp32_get_cpu_reg(g, 0u) == 1u, "display returns through caller LR");
        CHECK(gp32_get_cpu_reg(g, 1u) == 0x11110001u && gp32_get_cpu_reg(g, 2u) == 0x11110002u &&
              gp32_get_cpu_reg(g, 3u) == 0x11110003u && gp32_get_cpu_reg(g, 12u) == 0x1111000cu &&
              gp32_get_cpu_reg(g, 13u) == GP32_RAM_BASE + 0x1f000u && gp32_get_cpsr(g) == 0xa00000d3u,
              "trampoline preserves ABI scratch contract, stack and flags");
        CHECK(invalid ? g->elapsed.nanoseconds - start < 10000u : g->elapsed.nanoseconds - start >= 16666666u,
              "invalid surface returns promptly; valid foreground executes a full wait");
        gp32_destroy(g);
    }
}

static void check_legacy(int jit) {
    gp32_t *g = fixture(jit, 1234u), *clone = fixture(jit, 0u);
    if (!g || !clone) { gp32_destroy(g); gp32_destroy(clone); return; }
    size_t size = 0; uint8_t *image = save(g, &size);
    const size_t off = 16u + sizeof(gp32_state_image_t) + 32u;
    if (image) {
        uint8_t *v10 = malloc(size); CHECK(v10 != NULL, "allocate v10 fixture");
        if (v10) {
            memcpy(v10, image, size); memcpy(v10, gp32_state_magic_v10, 16u);
            size_t cut = off + GP32_CONTINUATION_V10_WORDS * 4u;
            size_t drop = GP32_CONTINUATION_BYTES - GP32_CONTINUATION_V10_WORDS * 4u;
            memmove(v10 + cut, v10 + cut + drop, size - cut - drop);
            CHECK(gp32_load_state_data(clone, v10, size - drop) == GP32_OK, "old callback v10 remains readable"); free(v10);
        }
        memcpy(image, gp32_state_magic_v9, 16u);
        memmove(image + off, image + off + GP32_CONTINUATION_BYTES, size - off - GP32_CONTINUATION_BYTES);
        size -= GP32_CONTINUATION_BYTES;
        gp32_state_image_t *direct = (gp32_state_image_t *)(image + 16u);
        direct->direct_vblank_wait_cycles = 66000u;
        CHECK(gp32_load_state_data(clone, image, size) == GP32_OK, "load legacy outstanding cycle wait");
        CHECK(gp32_get_pc(clone) == direct_stub_addr(clone) + 0x80u, "legacy wait migrates to ordinary guest instructions");
        size_t migrated_size = 0; uint8_t *migrated = save(clone, &migrated_size);
        CHECK(migrated && gp32_load_state_data(g, migrated, migrated_size) == GP32_OK, "save/load during migrated legacy wait");
        if (migrated) {
            CHECK(gp32_run_cycles(g, 70000u) == GP32_OK && gp32_run_cycles(clone, 70000u) == GP32_OK,
                  "legacy guest loop resumes within shared budgets");
            CHECK(gp32_run_cycles(g, 4096u) == GP32_OK && gp32_run_cycles(clone, 4096u) == GP32_OK,
                  "guest observes time mirror published at the preceding public boundary");
            printf("legacy-return jit=%d pc=%08x cpsr=%08x sp=%08x r0=%08x r12=%08x\n", jit,
                   gp32_get_pc(g), gp32_get_cpsr(g), gp32_get_cpu_reg(g, 13u), gp32_get_cpu_reg(g, 0u), gp32_get_cpu_reg(g, 12u));
            CHECK(gp32_get_pc(g) == caller && gp32_get_pc(clone) == caller && gp32_get_cpsr(g) == 0xa00000d3u &&
                  gp32_get_cpu_reg(g, 0u) == 0x11110000u && gp32_get_cpu_reg(g, 12u) == 0x1111000cu &&
                  gp32_get_cpu_reg(g, 13u) == GP32_RAM_BASE + 0x1f000u, "migration restores interrupted PC/registers/mode");
        }
        free(migrated);
    }
    printf("legacy-wait jit=%d\n", jit);
    free(image); gp32_destroy(g); gp32_destroy(clone);
}

static void check_interrupt_wait(int jit, int fiq) {
    gp32_t *g = fixture(jit, UINT32_MAX - 500000u), *clone = fixture(jit, 0u);
    if (!g || !clone) { gp32_destroy(g); gp32_destroy(clone); return; }
    const uint32_t handler = GP32_RAM_BASE + 0x24000u, flag = marker + 4u, mask = 1u << 10;
    const uint32_t words[] = {
        0xe92d500fu, 0xe59f0038u, 0xe59f1038u, 0xe5801000u, 0xe5801010u,
        0xe59f0030u, 0xe3a01001u, 0xe5801000u,
        0xe59f0028u, 0xe59fc028u, 0xe1a0e00fu, 0xe12fff1cu,
        0xe59f0014u, 0xe3a01002u, 0xe5801000u, 0xe8bd500fu, 0xe25ef004u,
        0x14400000u, mask, flag, surface, direct_stub_addr(g),
    };
    code(g, handler, words, GP32_ARRAY_COUNT(words));
    uint8_t bios[40] = {0}; char error[128];
    gp32_st32le(bios + 0x18u, 0xe59ff000u); gp32_st32le(bios + 0x1cu, 0xe59ff000u);
    gp32_st32le(bios + 0x20u, handler); gp32_st32le(bios + 0x24u, handler);
    CHECK(s3c2400_load_bios_buffer(g->soc, bios, sizeof(bios), error, sizeof(error)), "install actual IRQ/FIQ vectors");
    arm920t_set_cpsr(g->cpu, 0xd2u); arm920t_set_reg(g->cpu, 13u, GP32_RAM_BASE + 0x31000u);
    arm920t_set_cpsr(g->cpu, 0xd1u); arm920t_set_reg(g->cpu, 13u, GP32_RAM_BASE + 0x32000u);
    arm920t_set_cpsr(g->cpu, 0xa00000d3u);
    wait_callback(g, 1); start_timer(g); CHECK(arm_wait(g), "unmasked callback arms outer display wait");
    uint64_t outer = call_deadline(g);
    s3c2400_write32(g->soc, 0x14400004u, fiq ? mask : 0u);
    s3c2400_write32(g->soc, 0x14400008u, ~mask);
    s3c2400_write32(g->soc, 0x15100000u, 0u); s3c2400_write32(g->soc, 0x15100004u, 0u);
    s3c2400_write32(g->soc, 0x1510000cu, 9u); s3c2400_write32(g->soc, 0x15100008u, 1u);
    for (unsigned i = 0; i < 1000u && !(gp32_get_pc(g) == direct_stub_addr(g) + 0x30u &&
         (gp32_get_cpsr(g) & 31u) == (fiq ? 0x11u : 0x12u)); ++i)
        CHECK(gp32_run_cycles(g, 1u) == GP32_OK, "normal PWM boundary enters guest exception handler");
    CHECK((gp32_get_cpsr(g) & 31u) == (fiq ? 0x11u : 0x12u) &&
          gp32_get_pc(g) == direct_stub_addr(g) + 0x30u && s3c2400_debug_read32(g->soc, flag) == 1u,
          "hardware IRQ/FIQ owns a nested guest display call");
    CHECK(call_deadline(g) - outer == 16666667u, "nested display gets its own following deadline");
    size_t size = 0; uint8_t *image = save(g, &size);
    if (image) {
        CHECK(gp32_load_state_data(clone, image, size) == GP32_OK, "restore callback in real interrupt/nested wait");
        CHECK(finish(g) && finish(clone), "interrupt returns normally and callback resumes outer wait/tail");
        CHECK(s3c2400_debug_read32(g->soc, flag) == 2u && s3c2400_debug_read32(clone->soc, flag) == 2u &&
              s3c2400_debug_read32(g->soc, marker) == 77u && s3c2400_debug_read32(clone->soc, marker) == 77u,
              "IRQ and callback consumer tails each complete exactly once");
        CHECK(gp32_get_cycles(g) == gp32_get_cycles(clone) && gp32_get_cpsr(g) == 0xa00000d3u,
              "restored exception stack/deadlines/banks match uninterrupted execution");
        printf("nested-display jit=%d fiq=%d elapsed=%" PRIu64 " cycles=%" PRIu64 "\n", jit, fiq,
               g->elapsed.nanoseconds - (UINT32_MAX - 500000u), gp32_get_cycles(g));
    }
    free(image); gp32_destroy(g); gp32_destroy(clone);
}

static void check_refill_wait(int jit) {
    gp32_t *g = fixture(jit, 0u), *clone = fixture(jit, 0u);
    if (!g || !clone) { gp32_destroy(g); gp32_destroy(clone); return; }
    const uint32_t mixer = GP32_RAM_BASE + 0x6000u, cursor = mixer - 0x100u;
    const uint32_t buffer = GP32_RAM_BASE + 0x8000u, obj = GP32_RAM_BASE + 0xa000u;
    const uint32_t words[] = {0xe92d4007u, 0xe59f0018u, 0xe59fc018u, 0xe1a0e00fu, 0xe12fff1cu,
        0xe8bd4007u, 0xe5903000u, 0xe5813000u, 0xe12fff1eu, surface, direct_stub_addr(g)};
    code(g, fn, words, GP32_ARRAY_COUNT(words));
    g->direct_hle_sdk_sndmixer_addr = mixer; g->direct_hle_sdk_rate = 44100u;
    s3c2400_write32(g->soc, obj, 0xa710a710u);
    s3c2400_write32(g->soc, mixer, buffer); s3c2400_write32(g->soc, mixer + 4u, buffer + 4u);
    s3c2400_write32(g->soc, mixer + 8u, 4u); s3c2400_write32(g->soc, mixer + 12u, 4u);
    s3c2400_write32(g->soc, mixer + 16u, 1u);
    s3c2400_write32(g->soc, buffer, 0x80008000u); s3c2400_write32(g->soc, buffer + 4u, 0xa710a710u);
    s3c2400_write32(g->soc, cursor, mixer + 4u); s3c2400_write32(g->soc, cursor + 8u, 1u);
    s3c2400_write32(g->soc, cursor + 12u, buffer); s3c2400_write32(g->soc, cursor + 24u, 2u);
    s3c2400_write32(g->soc, cursor + 28u, obj); s3c2400_write32(g->soc, cursor + 32u, fn);
    CHECK(gp32_run_cycles(g, 1497u) == GP32_OK && g->direct_callback.owner == DIRECT_CB_REFILL,
          "real SDK refill consumer suspends before mixing its half");
    CHECK(arm_wait(g), "refill can call the legitimate display wait");
    CHECK(!s3c2400_debug_read32(g->soc, cursor - 4u) &&
          s3c2400_debug_read32(g->soc, buffer) == 0x80008000u, "display suspension does not acknowledge or fabricate refill");
    size_t size = 0; uint8_t *image = save(g, &size);
    if (image) {
        CHECK(gp32_load_state_data(clone, image, size) == GP32_OK && finish(g) && finish(clone),
              "restore display wait then resume actual refill and mix tail");
        uint64_t frames = 0, other = 0;
        const int16_t *pcm = s3c2400_audio_samples(g->soc, &frames, NULL);
        const int16_t *restored = s3c2400_audio_samples(clone->soc, &other, NULL);
        printf("refill-audio jit=%d frames=%" PRIu64 " other=%" PRIu64 " first=%d last=%d buffer=%08x\n", jit,
               frames, other, pcm && frames ? pcm[0] : 0, pcm && frames ? pcm[frames * 2u - 2u] : 0,
               s3c2400_debug_read32(g->soc, buffer + 4u));
        CHECK(s3c2400_debug_read32(g->soc, cursor - 4u) == 1u &&
              s3c2400_debug_read32(clone->soc, cursor - 4u) == 1u && frames == 1u && other == frames &&
              s3c2400_debug_read32(g->soc, buffer) == 0xa710a710u &&
              pcm && restored && pcm[0] == 10000 && !memcmp(pcm, restored, 4u),
              "captured ack and pending sample follow real guest fill, once");
    }
    printf("refill-display jit=%d ack=%u\n", jit, s3c2400_debug_read32(g->soc, cursor - 4u));
    free(image); gp32_destroy(g); gp32_destroy(clone);
}

static void check_state_reset_and_watchdog(int jit) {
    gp32_t *g = fixture(jit, 0u), *clone = fixture(jit, 0u);
    if (!g || !clone) { gp32_destroy(g); gp32_destroy(clone); return; }
    wait_callback(g, 0); start_timer(g); CHECK(arm_wait(g), "lifecycle starts within real display wait");
    size_t size = 0; uint8_t *image = save(g, &size), *bad = malloc(size);
    if (image && bad) {
        const size_t off = 16u + sizeof(gp32_state_image_t) + 32u;
        /* Separate domains: cadence phase, validity and obsolete host wait. */
        for (unsigned kind = 0; kind < 3u; ++kind) {
            memcpy(bad, image, size);
            if (kind < 2u) gp32_st32le(bad + off + (83u + kind) * 4u, kind ? 2u : 60u);
            else ((gp32_state_image_t *)(bad + 16u))->direct_vblank_wait_cycles = 1u;
            CHECK(gp32_load_state_data(g, bad, size) == GP32_ERR_IO, "reject contradictory display state before commit");
            size_t after_size = 0; uint8_t *after = save(g, &after_size);
            CHECK(after && after_size == size && !memcmp(after, image, size), "rejected wait state preserves pending machine");
            free(after);
        }
        /* Old callback-only v10 body: keep all 81 fields and live callback,
         * remove the later cadence/task extensions. Its legacy stub is upgraded. */
        memcpy(bad, image, size); memcpy(bad, gp32_state_magic_v10, 16u);
        size_t cut = off + GP32_CONTINUATION_V10_WORDS * 4u;
        size_t drop = GP32_CONTINUATION_BYTES - GP32_CONTINUATION_V10_WORDS * 4u;
        memmove(bad + cut, bad + cut + drop, size - cut - drop);
        CHECK(gp32_load_state_data(clone, bad, size - drop) == GP32_OK && finish(clone), "load/resume active v10 callback");
        CHECK(gp32_reset(g) == GP32_OK && !g->direct_callback.owner && !g->direct_tick.clock &&
              !g->direct_vblank_time.valid && !g->direct_vblank_wait_requested, "reset cancels pending callback and wait");
        CHECK(gp32_load_state_data(g, image, size) == GP32_OK, "restore pending wait for image replacement");
        uint8_t raw[32] = {0}; gp32_st32le(raw, 0xeafffffeu); gp32_st32le(raw + 4u, 0xeafffffeu);
        gp32_st32le(raw + 4u, GP32_RAM_BASE);
        for (unsigned i = 8u; i <= 16u; i += 4u) gp32_st32le(raw + i, GP32_RAM_BASE + sizeof(raw));
        CHECK(gp32_load_fxe_data(g, raw, sizeof(raw), "wait-reset.gxb") == GP32_OK && !g->direct_callback.owner &&
              !g->direct_tick.clock && !g->direct_vblank_time.valid, "image replacement cancels old wait/callback ownership");
    }
    free(bad); free(image); gp32_destroy(g); gp32_destroy(clone);
    g = fixture(jit, UINT64_MAX - 500000000u); clone = fixture(jit, 0u);
    if (!g || !clone) { gp32_destroy(g); gp32_destroy(clone); return; }
    s3c2400_write32(g->soc, fn, 0xeafffffeu); start_timer(g);
    CHECK(gp32_run_cycles(g, 70000000u) == GP32_ERR_CPU_FAULT && g->direct_callback.owner == DIRECT_CB_FAULT,
          "nonreturning callback has bounded visible watchdog across uint64 wrap");
    CHECK(g->elapsed.nanoseconds - (UINT64_MAX - 500000000u) >= 1000000000u &&
          g->elapsed.nanoseconds - (UINT64_MAX - 500000000u) < 1000070000u && !s3c2400_debug_read32(g->soc, marker),
          "watchdog never fabricates callback return or consumer success");
    uint64_t cycles = gp32_get_cycles(g);
    CHECK(gp32_run_cycles(g, 4096u) == GP32_ERR_CPU_FAULT && gp32_get_cycles(g) == cycles, "wrapped watchdog is sticky");
    image = save(g, &size);
    if (image) CHECK(gp32_load_state_data(clone, image, size) == GP32_OK && gp32_run_cycles(clone, 1u) == GP32_ERR_CPU_FAULT,
                     "wrapped fault roundtrip remains sticky");
    printf("wait-reset-watchdog jit=%d cycles=%" PRIu64 "\n", jit, cycles);
    free(image); gp32_destroy(g); gp32_destroy(clone);
}

static void check_wrapped_frame(int jit) {
    uint64_t origin = UINT64_MAX - 8000000u;
    gp32_t *g = fixture(jit, origin), *clone = fixture(jit, 0u);
    if (!g || !clone) { gp32_destroy(g); gp32_destroy(clone); return; }
    wait_callback(g, 0); start_timer(g);
    uint64_t start = g->elapsed.nanoseconds;
    CHECK(gp32_run_frame(g) == GP32_OK && g->direct_hle_callback_running &&
          g->elapsed.nanoseconds - start >= 16666666u && g->elapsed.nanoseconds - start < 16666700u,
          "frame deadline wraps and suspends callback display wait within caller time");
    size_t size = 0; uint8_t *image = save(g, &size);
    if (image) {
        CHECK(gp32_load_state_data(clone, image, size) == GP32_OK, "load wrapped frame and display deadlines");
        CHECK(gp32_run_frame(g) == GP32_OK && gp32_run_frame(clone) == GP32_OK && !g->direct_callback.owner &&
              !clone->direct_callback.owner && g->elapsed.nanoseconds - start >= 33333333u &&
              g->elapsed.nanoseconds - start < 33333400u && g->elapsed.nanoseconds == clone->elapsed.nanoseconds &&
              gp32_get_cycles(g) == gp32_get_cycles(clone), "next wrapped frame resumes guest wait without adding hidden time");
    }
    printf("wrapped-frame jit=%d elapsed=%" PRIu64 "\n", jit, g->elapsed.nanoseconds - start);
    free(image); gp32_destroy(g); gp32_destroy(clone);
}

static void check_display_cadence(int jit) {
    gp32_t *g = fixture(jit, 0u); if (!g) return;
    uint64_t first = 0u, previous = 0u;
    for (unsigned call = 0; call < 3u; ++call) {
        arm920t_set_reg(g->cpu, 0u, surface); arm920t_set_reg(g->cpu, 15u, direct_stub_addr(g));
        CHECK(arm_wait(g), "arm successive foreground display calls");
        uint64_t target = call_deadline(g);
        if (!call) first = target;
        else CHECK(target - previous == 16666667u, "reserve next cadence slot despite guest return overhead");
        previous = target;
        for (unsigned i = 0; i < 1000u && direct_time_pending(target, g->elapsed.nanoseconds); ++i)
            CHECK(gp32_run_cycles(g, 4096u) == GP32_OK, "execute cadence wait with bounded public budget");
        for (unsigned i = 0; i < 200u && gp32_get_pc(g) != caller; ++i)
            CHECK(gp32_run_cycles(g, 1u) == GP32_OK, "complete cadence epilogue");
        CHECK(gp32_get_pc(g) == caller, "cadence call returns normally");
    }
    CHECK(previous - first == 33333334u && g->direct_vblank_time.remainder < 60u, "rational nanosecond phase is carried");
    CHECK(gp32_run_cycles(g, 6600000u) == GP32_OK, "advance guest beyond missed reservation");
    arm920t_set_reg(g->cpu, 0u, surface); arm920t_set_reg(g->cpu, 15u, direct_stub_addr(g));
    CHECK(arm_wait(g) && call_deadline(g) - g->elapsed.nanoseconds == 16666666u,
          "missed cadence rebases from emulated now, without catch-up host skipping");
    printf("display-cadence jit=%d target=%" PRIu64 "\n", jit, previous);
    gp32_destroy(g);
}

static void check_mirror_pair(int jit) {
    const uint64_t origin = (UINT64_C(12) << 32) + UINT32_MAX - 96u;
    gp32_t *g = fixture(jit, origin); if (!g) return;
    arm920t_set_reg(g->cpu, 0u, surface); arm920t_set_reg(g->cpu, 15u, direct_stub_addr(g));
    CHECK(arm_wait(g) && gp32_run_cycles(g, 1u) == GP32_OK && gp32_get_pc(g) == direct_stub_addr(g) + 0x34u,
          "place actual mirror reader immediately before low-word rollover");
    CHECK(gp32_run_cycles(g, 1u) == GP32_OK && gp32_get_cpu_reg(g, 12u) == 12u,
          "guest reads the old high word before boundary settlement");
    for (unsigned i = 0; i < 4u; ++i) CHECK(gp32_run_cycles(g, 1u) == GP32_OK, "step torn-pair retry");
    CHECK(gp32_get_cpu_reg(g, 14u) == 13u && gp32_get_pc(g) == direct_stub_addr(g) + 0x34u &&
          direct_time_pending(call_deadline(g), g->elapsed.nanoseconds), "new high word rejects torn sample instead of returning early");
    printf("coherent-mirror jit=%d old-high=12 new-high=%u\n", jit, gp32_get_cpu_reg(g, 14u));
    gp32_destroy(g);
}

static void check_legacy_request(int jit) {
    gp32_t *g = fixture(jit, 0u), *clone = fixture(jit, 0u);
    if (!g || !clone) { gp32_destroy(g); gp32_destroy(clone); return; }
    s3c2400_write32(g->soc, 0x14800004u, 0xe000u); s3c2400_write32(g->soc, 0x14800014u, 2u);
    size_t size = 0; uint8_t *pristine = save(g, &size), *legacy = malloc(size);
    if (pristine && legacy) {
        const size_t off = 16u + sizeof(gp32_state_image_t) + 32u;
        memcpy(legacy, pristine, size); memcpy(legacy, gp32_state_magic_v9, 16u);
        memmove(legacy + off, legacy + off + GP32_CONTINUATION_BYTES, size - off - GP32_CONTINUATION_BYTES);
        size_t legacy_size = size - GP32_CONTINUATION_BYTES;
        ((gp32_state_image_t *)(legacy + 16u))->direct_vblank_wait_requested = 1;
        CHECK(gp32_load_state_data(clone, legacy, legacy_size) == GP32_OK && gp32_get_run_clock_hz(clone) == 48000000u &&
              call_deadline(clone) - clone->elapsed.nanoseconds == 16666666u,
              "legacy unscheduled request uses one frame at incoming 48 MHz, not a fixed 66 MHz cycle count");
        arm920t_state_image_t *cpu = (arm920t_state_image_t *)(legacy + off);
        cpu->r[13] = GP32_RAM_BASE + 64u * 1024u * 1024u;
        CHECK(gp32_load_state_data(g, legacy, legacy_size) == GP32_ERR_IO, "migration stack must fit incoming RAM before commit");
        size_t after_size = 0; uint8_t *after = save(g, &after_size);
        CHECK(after && after_size == size && !memcmp(after, pristine, size), "legacy migration failure is transactional");
        free(after);
    }
    printf("legacy-request jit=%d clock=%u\n", jit, gp32_get_run_clock_hz(clone));
    free(pristine); free(legacy); gp32_destroy(g); gp32_destroy(clone);
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "wait-state.tmp";
    for (int jit = 0; jit <= 1; ++jit) {
        check_callback_wait(jit, 0u, 0, path);
        check_callback_wait(jit, UINT32_MAX - 1000000u, 1, path);
        check_callback_wait(jit, UINT64_MAX - 8000000u, 0, path);
        check_foreground_and_invalid(jit); check_legacy(jit);
        check_interrupt_wait(jit, 0); check_interrupt_wait(jit, 1);
        check_refill_wait(jit);
        check_state_reset_and_watchdog(jit);
        check_wrapped_frame(jit);
        check_display_cadence(jit);
        check_mirror_pair(jit); check_legacy_request(jit);
    }
    printf("guest wait regression: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
