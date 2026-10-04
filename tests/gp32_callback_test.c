/* Behavioral regressions for the two real callback consumers and v11 state.
 * No ROM, mocked CPU or alternate execution backend. */
#include "../src/gp32.c"

static int failures;
#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "FAIL: %s (%d)\n", m, __LINE__); ++failures; } } while (0)
static const uint32_t fn = GP32_RAM_BASE + 0x2000u;
static const uint32_t marker = GP32_RAM_BASE + 0x4000u;
static const uint32_t caller = GP32_RAM_BASE + 0x20000u;

static void put_code(gp32_t *g, uint32_t addr, const uint32_t *code, size_t count) {
    for (size_t i = 0; i < count; ++i) s3c2400_write32(g->soc, addr + (uint32_t)i * 4u, code[i]);
}
static gp32_t *fixture(int jit) {
    gp32_t *g = gp32_create(NULL);
    CHECK(g != NULL, "create core");
    if (!g) return NULL;
    direct_set_fxe_mode(g, 1u);
    direct_install_stubs(g);
    gp32_set_jit(g, jit);
    s3c2400_write32(g->soc, 0x14800004u, 0x3000u);
    s3c2400_write32(g->soc, 0x14800014u, 0u);
    s3c2400_write32(g->soc, 0x14a00000u, 1u); /* enable legacy LCD timing */
    s3c2400_write32(g->soc, caller, 0xeafffffeu);
    arm920t_set_cpsr(g->cpu, ARM_MODE_SVC | ARM_I_FLAG | ARM_F_FLAG);
    arm920t_set_reg(g->cpu, 8u, 0x12345678u);
    arm920t_set_reg(g->cpu, 15u, caller);
    return g;
}
static void timer_due(gp32_t *g, unsigned slot, uint32_t entry) {
    g->direct_hle_gpos_timers_enabled = 1u;
    g->direct_hle_gpos_timer[slot].configured = 1u;
    g->direct_hle_gpos_timer[slot].enabled = 1u;
    g->direct_hle_gpos_timer[slot].callback = entry;
    g->direct_hle_gpos_timer[slot].tps = 1000u;
    g->direct_hle_gpos_timer[slot].accum = 65984000u;
}
static gp32_status_t finish_callback(gp32_t *g) {
    gp32_status_t status = GP32_OK;
    for (unsigned i = 0; i < 10000u && g->direct_hle_callback_running && status == GP32_OK; ++i)
        status = gp32_run_cycles(g, 1u);
    CHECK(!g->direct_hle_callback_running, "short callback finishes through real trap");
    return status;
}
static uint8_t *save(gp32_t *g, size_t *size) {
    *size = gp32_state_size(g);
    uint8_t *data = malloc(*size);
    CHECK(data && gp32_save_state_data(g, data, *size) == GP32_OK, "capture public state");
    return data;
}

static void rejected_state(gp32_t *g, const uint8_t *pristine, size_t size, uint8_t *bad, size_t bad_size) {
    CHECK(gp32_load_state_data(g, bad, bad_size) == GP32_ERR_IO, "malformed continuation rejected");
    size_t after_size = 0;
    uint8_t *after = save(g, &after_size);
    CHECK(after && after_size == size && !memcmp(after, pristine, size), "failed load preserves pending CPU/RAM/tail/audio");
    free(after);
}

static void check_refill(int jit, int final_probe, const char *path) {
    gp32_t *g = fixture(jit), *clone = fixture(jit);
    if (!g || !clone) { gp32_destroy(g); gp32_destroy(clone); return; }
    const uint32_t mixer = GP32_RAM_BASE + 0x6000u, cursor = mixer - 0x100u;
    const uint32_t buffer = GP32_RAM_BASE + 0x8000u, obj = GP32_RAM_BASE + 0xa000u;
    const uint32_t code[] = {
        0xe3a03014u, 0xe2533001u, 0x1afffffdu, /* bounded delay */
        0xe5904000u, 0xe5814000u,              /* fill released half */
        0xe5905004u, 0xe3a03000u, 0xe5853000u, /* invalidate scan metadata */
        0xe12fff1eu,
    };
    put_code(g, fn, code, GP32_ARRAY_COUNT(code));
    s3c2400_write32(g->soc, obj, 0xa710a710u); /* unsigned PCM +10000 */
    s3c2400_write32(g->soc, obj + 4u, cursor);
    g->direct_hle_sdk_sndmixer_addr = mixer;
    g->direct_hle_sdk_rate = 44100u;
    s3c2400_write32(g->soc, mixer, buffer);
    s3c2400_write32(g->soc, mixer + 4u, buffer + (final_probe ? 0u : 4u));
    s3c2400_write32(g->soc, mixer + 8u, 4u);
    s3c2400_write32(g->soc, mixer + 12u, 4u);
    s3c2400_write32(g->soc, mixer + 16u, 1u);
    s3c2400_write32(g->soc, buffer, 0xa710a710u);
    s3c2400_write32(g->soc, buffer + 4u, 0x80008000u);
    s3c2400_write32(g->soc, cursor, mixer + 4u);
    s3c2400_write32(g->soc, cursor + 8u, 1u);
    s3c2400_write32(g->soc, cursor + 12u, buffer);
    s3c2400_write32(g->soc, cursor + 24u, 2u);
    s3c2400_write32(g->soc, cursor + 28u, obj);
    s3c2400_write32(g->soc, cursor + 32u, fn);
    CHECK(gp32_run_cycles(g, 2994u) == GP32_OK, "two-sample foreground interval");
    CHECK(gp32_get_cycles(g) == 2994u && g->direct_hle_callback_running, "refill starts without extra execution");
    CHECK(s3c2400_debug_read32(g->soc, cursor - 4u) == 0u, "pending refill has no half acknowledgment");
    uint64_t frames = 0;
    const int16_t *audio = s3c2400_audio_samples(g->soc, &frames, NULL);
    CHECK(frames == (final_probe ? 2u : 0u), "no partial-buffer mixing; final probe keeps prefix only");
    if (final_probe && audio) CHECK(audio[0] == 10000 && audio[2] == 10000, "prefix samples precede refill");
    /* Save policy clears queued output on load, so compare only pending work. */
    gp32_clear_audio(g);
    CHECK(gp32_run_cycles(g, 5u) == GP32_OK && g->direct_hle_callback_running, "suspend within real filler");
    size_t size = 0;
    uint8_t *image = save(g, &size);
    CHECK(image && !memcmp(image, "GP32STATEv0011", 14u), "v11 writer");
    if (image) {
        CHECK(gp32_load_state_data(clone, image, size) == GP32_OK, "load active refill");
        CHECK(gp32_save_state(g, path) == GP32_OK && gp32_load_state(clone, path) == GP32_OK, "file continuation roundtrip");
        CHECK(finish_callback(g) == GP32_OK && finish_callback(clone) == GP32_OK, "resume filler and host mix tail");
        CHECK(s3c2400_debug_read32(g->soc, cursor) == 0u && s3c2400_debug_read32(g->soc, cursor - 4u) == 1u,
              "acknowledge captured address despite changed scan metadata");
        CHECK(s3c2400_debug_read32(clone->soc, cursor - 4u) == 1u, "restored captured acknowledgment");
        uint64_t a_frames = 0, b_frames = 0;
        const int16_t *a = s3c2400_audio_samples(g->soc, &a_frames, NULL);
        const int16_t *b = s3c2400_audio_samples(clone->soc, &b_frames, NULL);
        CHECK(a_frames == (final_probe ? 0u : 2u) && b_frames == a_frames,
              "resume exact remaining sample count, including zero-frame final probe");
        CHECK(!a_frames || (a && b && !memcmp(a, b, (size_t)a_frames * 4u)), "restored sample tail matches uninterrupted tail");
        CHECK(arm920t_get_reg(g->cpu, 8u) == 0x12345678u && arm920t_get_pc(g->cpu) == caller, "foreground restored only at trap");
    }
    printf("refill jit=%d final=%d ack=%u\n", jit, final_probe, s3c2400_debug_read32(g->soc, cursor - 4u));
    free(image); gp32_destroy(g); gp32_destroy(clone);
}

static void check_state_validation(int jit) {
    gp32_t *g = fixture(jit);
    if (!g) return;
    s3c2400_write32(g->soc, fn, 0xeafffffeu);
    timer_due(g, 0u, fn);
    CHECK(gp32_run_cycles(g, 16u) == GP32_OK, "start state-validation callback");
    size_t size = 0;
    uint8_t *image = save(g, &size), *bad = malloc(size);
    if (image && bad) {
        const size_t off = 16u + sizeof(gp32_state_image_t) + 32u;
        /* Each breaks a distinct domain invariant, not a byte-per-field test. */
        const struct { unsigned word; uint32_t value; } corrupt[] = {
            {2u, 99u}, {54u, DIRECT_TICK_AFTER_REFILL}, {55u, 1u},
            {56u, 0x200u}, {59u, 4u}, {60u, 9u}, {26u, 0u}, {8u, 0u},
            {0u, 0x1000000u}, {1u, 67000000u}, {5u, GP32_RAM_BASE - 4u},
        };
        for (size_t i = 0; i < GP32_ARRAY_COUNT(corrupt); ++i) {
            memcpy(bad, image, size);
            gp32_st32le(bad + off + corrupt[i].word * 4u, corrupt[i].value);
            rejected_state(g, image, size, bad, size);
        }
        rejected_state(g, image, size, image, off + 20u);
        rejected_state(g, image, size, image, size - 1u);
        /* v9 can retain a stale returned flag, but has no active foreground
         * snapshot. An active legacy image must be rejected before SoC commit. */
        memcpy(bad, image, off);
        memcpy(bad, gp32_state_magic_v9, 16u);
        memcpy(bad + off, image + off + GP32_CONTINUATION_BYTES, size - off - GP32_CONTINUATION_BYTES);
        rejected_state(g, image, size, bad, size - GP32_CONTINUATION_BYTES);
    }
    printf("state-validation jit=%d\n", jit);
    CHECK(gp32_reset(g) == GP32_OK && !g->direct_hle_callback_running &&
          gp32_run_cycles(g, 1u) == GP32_OK, "reset cancels an ordinary suspended callback");
    free(bad); free(image); gp32_destroy(g);
}

static void check_clock_and_volume(int jit) {
    gp32_t *g = fixture(jit);
    gp32_options_t config = {0}; config.ram_size = 4u * 1024u * 1024u;
    gp32_t *clone = gp32_create(&config);
    if (!g || !clone) { gp32_destroy(g); gp32_destroy(clone); return; }
    gp32_set_jit(clone, jit);
    /* Switch to 132 MHz FCLK first: half HCLK changes effective run rate. */
    s3c2400_write32(g->soc, 0x14800004u, 0xe000u);
    CHECK(direct_run_clock_hz(g) == 132000000u, "132 MHz old denominator");
    const uint32_t code[] = {0xe59f0010u, 0xe3a01002u, 0xe5801000u,
        0xe3a00015u, 0xef000017u, 0xe12fff1eu, 0x14800014u};
    put_code(g, fn, code, GP32_ARRAY_COUNT(code));
    timer_due(g, 0u, fn);
    g->direct_hle_gpos_timer[0].accum = 131984000u;
    g->direct_hle_sdk_accum = 100000000u; /* valid at 132 MHz, invalid at 66 MHz */
    CHECK(gp32_run_cycles(g, 16u) == GP32_OK && g->direct_hle_callback_running, "start clock-changing timer");
    g->direct_hle_gpos_timer[0].enabled = 0u;
    CHECK(gp32_run_cycles(g, 3u) == GP32_OK && direct_run_clock_hz(g) == 48000000u, "clock write yields live callback at existing effective bus rate");
    size_t size = 0;
    uint8_t *image = save(g, &size);
    if (image) {
        CHECK(gp32_load_state_data(clone, image, size) == GP32_OK && s3c2400_ram_size(clone->soc) == 0x800000u,
              "validate incoming eight-MiB RAM and suspended old clock, not target four-MiB/current clock");
        CHECK(finish_callback(g) == GP32_OK && finish_callback(clone) == GP32_OK, "resume clock/volume callback");
        CHECK(g->direct_hle_sdk_accum == 36363636u && clone->direct_hle_sdk_accum == 36363636u,
              "normalize old tick denominator exactly once");
        s3c2400_audio_append_s16_stereo(g->soc, 10000, -10000, 44100u);
        s3c2400_audio_append_s16_stereo(clone->soc, 10000, -10000, 44100u);
        uint64_t frames = 0;
        const int16_t *a = s3c2400_audio_samples(g->soc, &frames, NULL);
        CHECK(a && frames == 1u && a[0] == 1000 && a[1] == -1000, "callback volume settles before next instruction");
        const int16_t *b = s3c2400_audio_samples(clone->soc, &frames, NULL);
        CHECK(b && frames == 1u && b[0] == 1000 && b[1] == -1000, "restored callback volume ordering");
    }
    printf("clock-volume jit=%d clock=%u fraction=%llu\n", jit, direct_run_clock_hz(g), (unsigned long long)g->direct_hle_sdk_accum);
    free(image); gp32_destroy(g); gp32_destroy(clone);
}

static void check_timer_epoch(int jit) {
    gp32_t *g = fixture(jit), *clone = fixture(jit);
    if (!g || !clone) { gp32_destroy(g); gp32_destroy(clone); return; }
    const uint32_t command = GP32_RAM_BASE + 0x8000u, target = fn + 0x1000u;
    const uint32_t replace[] = {0xe59f000cu, 0xef000013u, 0xe59f0008u, 0xef000013u, 0xe12fff1eu, command, command + 20u};
    const uint32_t count[] = {0xe59f000cu, 0xe5901000u, 0xe2811001u, 0xe5801000u, 0xe12fff1eu, marker};
    put_code(g, fn, replace, GP32_ARRAY_COUNT(replace)); put_code(g, target, count, GP32_ARRAY_COUNT(count));
    s3c2400_write32(g->soc, command, 1u); s3c2400_write32(g->soc, command + 4u, 1u);
    s3c2400_write32(g->soc, command + 8u, target); s3c2400_write32(g->soc, command + 12u, 1000u);
    s3c2400_write32(g->soc, command + 20u, 4u); s3c2400_write32(g->soc, command + 24u, 1u);
    timer_due(g, 0u, fn); timer_due(g, 1u, target);
    g->direct_hle_gpos_timer_epoch[0] = 5u; g->direct_hle_gpos_timer_epoch[1] = 9u;
    CHECK(gp32_run_cycles(g, 16u) == GP32_OK, "capture both timer expiries");
    CHECK(gp32_run_cycles(g, 2u) == GP32_OK && g->direct_hle_callback_running, "replace later timer while earlier callback pending");
    g->direct_hle_gpos_timer[0].enabled = 0u;
    size_t size = 0; uint8_t *image = save(g, &size);
    if (image) {
        CHECK(gp32_load_state_data(clone, image, size) == GP32_OK, "load timer lifetime continuation");
        CHECK(finish_callback(g) == GP32_OK && finish_callback(clone) == GP32_OK, "resume same selected timer slot");
        CHECK(s3c2400_debug_read32(g->soc, marker) == 0u && s3c2400_debug_read32(clone->soc, marker) == 0u,
              "replacement does not inherit captured expiry despite same callback/rate");
        CHECK(gp32_run_cycles(g, 66000u) == GP32_OK && gp32_run_cycles(clone, 66000u) == GP32_OK,
              "new lifetime earns its own full period");
        CHECK(finish_callback(g) == GP32_OK && finish_callback(clone) == GP32_OK, "complete new timer invocation");
        CHECK(s3c2400_debug_read32(g->soc, marker) == 1u && s3c2400_debug_read32(clone->soc, marker) == 1u,
              "new timer fires once after its own period");
    }
    printf("timer-epoch jit=%d count=%u\n", jit, s3c2400_debug_read32(g->soc, marker));
    free(image); gp32_destroy(g); gp32_destroy(clone);
}

static void check_faults(int jit) {
    for (unsigned fault = 0; fault < 3u; ++fault) {
        gp32_t *g = fixture(jit), *clone = fixture(jit);
        if (!g || !clone) { gp32_destroy(g); gp32_destroy(clone); continue; }
        const uint32_t command = GP32_RAM_BASE + 0x8000u;
        const uint32_t task_code[] = {0xe59f0008u, 0xef000014u, 0xe5801000u, 0xe12fff1eu, command};
        if (fault == 0u) s3c2400_write32(g->soc, fn, 0xeafffffeu);
        else if (fault == 1u) { put_code(g, fn, task_code, GP32_ARRAY_COUNT(task_code)); s3c2400_write32(g->soc, command, 0x20u); }
        else if (fault == 2u) s3c2400_write32(g->soc, fn, 0xe12fff1eu);
        timer_due(g, 0u, fn);
        CHECK(gp32_run_cycles(g, 16u) == GP32_OK, "start fault fixture");
        if (fault == 2u) {
            /* Existing ARM920T WFI is maintenance only. Exercise its supported
             * persistent halt state through the real CPU state API instead. */
            arm920t_state_image_t *cpu = malloc(sizeof(*cpu));
            CHECK(cpu != NULL, "allocate halted CPU image");
            if (cpu) {
                state_io_t io = state_io_writer(cpu, sizeof(*cpu));
                CHECK(arm920t_state_save_io(g->cpu, &io), "capture running callback CPU");
                cpu->halted = 1;
                arm920t_state_apply(g->cpu, cpu);
                free(cpu);
            }
        }
        CHECK(gp32_run_cycles(g, fault ? 4096u : 70000000u) == GP32_ERR_CPU_FAULT, "nonreturning/unsupported callback fails visibly");
        uint64_t cycles = gp32_get_cycles(g);
        CHECK(gp32_run_cycles(g, 4096u) == GP32_ERR_CPU_FAULT && gp32_get_cycles(g) == cycles, "sticky fault spends no more guest cycles");
        CHECK(arm920t_get_pc(g->cpu) != caller && s3c2400_debug_read32(g->soc, marker) == 0u,
              "fault preserves guest CPU and never fabricates foreground return");
        if (fault == 0u) {
            CHECK(g->elapsed.nanoseconds >= 1000000000u && g->elapsed.nanoseconds < 1000070000u, "bounded one-second emulated watchdog");
            CHECK(s3c2400_debug_read32(g->soc, direct_fw_tick_addr(g)) >= 1000u, "firmware time advances during callback");
            gp32_framebuffer_desc_t fb;
            CHECK(gp32_get_framebuffer(g, &fb) == GP32_OK && fb.frame_counter > 0u, "normal hardware advances during callback");
        } else if (fault == 1u) CHECK(s3c2400_debug_read32(g->soc, command) == 0x20u, "task switch fault precedes guest mutation");
        size_t size = 0; uint8_t *image = save(g, &size);
        if (image) CHECK(gp32_load_state_data(clone, image, size) == GP32_OK && gp32_run_cycles(clone, 1u) == GP32_ERR_CPU_FAULT,
                         "fault state persists through load");
        printf("fault jit=%d case=%u cycles=%llu error=%s\n", jit, fault, (unsigned long long)cycles, gp32_get_error(g));
        CHECK(gp32_reset(g) == GP32_OK && gp32_run_cycles(g, 1u) == GP32_OK, "reset cancels fault and suspended tick");
        free(image); gp32_destroy(g); gp32_destroy(clone);
    }
}

static void check_remaining_timer_calls(int jit) {
    gp32_t *g = fixture(jit), *clone = fixture(jit);
    if (!g || !clone) { gp32_destroy(g); gp32_destroy(clone); return; }
    const uint32_t count[] = {0xe59f000cu, 0xe5901000u, 0xe2811001u, 0xe5801000u, 0xe12fff1eu, marker};
    put_code(g, fn, count, GP32_ARRAY_COUNT(count));
    s3c2400_write32(g->soc, 0x14800004u, 0x3f3u); /* minimum undivided PLL: 184615 Hz */
    timer_due(g, 0u, fn); g->direct_hle_gpos_timer[0].accum = 0u;
    g->direct_hle_gpos_timer_epoch[0] = 42u;
    CHECK(gp32_run_cycles(g, 2000u) == GP32_OK, "capture ten expiries at slow supported clock");
    CHECK(gp32_run_cycles(g, 6u) == GP32_OK && g->direct_hle_callback_running &&
          s3c2400_debug_read32(g->soc, marker) == 1u, "return first invocation and install next with spent budget");
    size_t size = 0; uint8_t *image = save(g, &size);
    if (image) {
        CHECK(gp32_load_state_data(clone, image, size) == GP32_OK, "load remaining same-slot invocations and nonzero epoch");
        CHECK(finish_callback(g) == GP32_OK && finish_callback(clone) == GP32_OK, "resume remaining timer calls");
        CHECK(s3c2400_debug_read32(g->soc, marker) == 8u && s3c2400_debug_read32(clone->soc, marker) == 8u,
              "keep eight-call cap and execute each remaining invocation once after load");
    }
    printf("remaining-timer-calls jit=%d count=%u\n", jit, s3c2400_debug_read32(g->soc, marker));
    free(image); gp32_destroy(g); gp32_destroy(clone);
}

static void check_exception_bank_state(int jit) {
    gp32_t *g = fixture(jit), *clone = fixture(jit);
    if (!g || !clone) { gp32_destroy(g); gp32_destroy(clone); return; }
    const uint32_t code[] = {0xe1a0200eu, 0xe321f0d2u, 0xe1a0e002u, 0xe12fff1eu};
    put_code(g, fn, code, GP32_ARRAY_COUNT(code)); timer_due(g, 0u, fn);
    CHECK(gp32_run_cycles(g, 16u) == GP32_OK && gp32_run_cycles(g, 2u) == GP32_OK,
          "suspend after guest IRQ mode switch");
    CHECK((arm920t_get_cpsr(g->cpu) & 0x1fu) == 0x12u && arm920t_get_reg(g->cpu, 13u) == 0u,
          "legitimate unused IRQ stack bank is zero");
    size_t size = 0; uint8_t *image = save(g, &size);
    if (image) {
        CHECK(gp32_load_state_data(clone, image, size) == GP32_OK, "load suspended callback with unused exception stack bank");
        CHECK(finish_callback(g) == GP32_OK && finish_callback(clone) == GP32_OK, "return through normal CPU from restored IRQ context");
        CHECK(arm920t_get_pc(g->cpu) == caller && arm920t_get_pc(clone->cpu) == caller &&
              arm920t_get_cpsr(g->cpu) == 0xd3u && arm920t_get_cpsr(clone->cpu) == 0xd3u,
              "restore original foreground PC and SVC mode after exception-bank continuation");
    }
    printf("exception-bank-state jit=%d\n", jit);
    free(image); gp32_destroy(g); gp32_destroy(clone);
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--exception-bank")) {
        check_exception_bank_state(0); check_exception_bank_state(1);
        printf("exception-bank regression: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
        return failures ? 1 : 0;
    }
    const char *path = argc > 1 ? argv[1] : "callback-state.tmp";
    for (int jit = 0; jit <= 1; ++jit) {
        check_refill(jit, 0, path); check_refill(jit, 1, path);
        check_state_validation(jit); check_clock_and_volume(jit); check_timer_epoch(jit);
        check_remaining_timer_calls(jit); check_faults(jit);
        check_exception_bank_state(jit);
    }
    printf("callback regression: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
