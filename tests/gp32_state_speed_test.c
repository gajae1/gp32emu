/* Saving or rejecting a load must not consume peripheral time. Temporary
 * conversion to nominal CPU speed used to floor phases in both directions.
 * At 150%, for example, two PWM cycles became one after a save. Use the const
 * wire writer to observe live state without repeating that conversion, then
 * execute more guest cycles against an untouched control. No ROM is needed. */
#ifndef GP32_SOURCE
#define GP32_SOURCE "../src/gp32.c"
#endif
#include GP32_SOURCE

static unsigned cases, failures;
static char scenario[128];
#define CHECK(c, msg) do { if (!(c)) { \
    fprintf(stderr, "FAIL: %s: %s (%d)\n", scenario, (msg), __LINE__); \
    ++failures; \
} } while (0)

static uint32_t timer_count(s3c2400_t *soc, unsigned timer) {
    return s3c2400_read32(soc, timer == 4u ? 0x15100040u : 0x15100014u + timer * 12u);
}

static gp32_t *fixture(uint32_t percent, int tft, uint32_t tag) {
    gp32_options_t opt = {0};
    opt.ram_size = 64u * 1024u;
    gp32_t *g = gp32_create(&opt);
    CHECK(g != NULL, "create machine");
    if (!g) return NULL;
    gp32_set_jit(g, 0);
    s3c2400_write32(g->soc, GP32_RAM_BASE, 0xeafffffeu); /* B . */
    arm920t_set_reg(g->cpu, 4u, tag);
    arm920t_set_reg(g->cpu, 15u, GP32_RAM_BASE);
    s3c2400_write32(g->soc, 0x14800004u, 0x3000u); /* FCLK/HCLK/PCLK 66 MHz. */
    s3c2400_write32(g->soc, 0x14800014u, 0u);
    CHECK(gp32_set_cpu_speed_percent(g, percent) == GP32_OK, "set CPU speed");
    s3c2400_write32(g->soc, 0x14a00004u, 31u << 14);
    s3c2400_write32(g->soc, 0x14a00008u, 31u << 8);
    s3c2400_write32(g->soc, 0x14a00000u, tft ? 0x179u : 0u);

    /* Distinct nonzero PWM phases, including timer 4, and a retained idle
     * audio phase before IIS starts. All five PWM phases lose a cycle in the
     * old 150 -> 100 -> 150 conversion. */
    uint32_t control = 0u;
    for (unsigned timer = 0; timer < 5u; ++timer) {
        s3c2400_write32(g->soc, 0x1510000cu + timer * 12u, 100u + timer * 17u + tag);
        control |= timer == 4u ? 0x500000u : 9u << (timer ? timer * 4u + 4u : 0u);
        s3c2400_write32(g->soc, 0x15100008u, control);
        CHECK(gp32_run_cycles(g, timer == 4u ? 2u : 3u) == GP32_OK, "prime PWM and idle audio");
    }
    for (unsigned i = 0; i < 8u; ++i)
        s3c2400_write8(g->soc, GP32_RAM_BASE + 0x2000u + i, (uint8_t)(0x31u + i * 23u + tag));
    s3c2400_write32(g->soc, 0x14600040u, GP32_RAM_BASE + 0x2000u);
    s3c2400_write32(g->soc, 0x14600044u, 0x35508010u);
    s3c2400_write32(g->soc, 0x14600048u, 0x10800008u); /* repeating byte IIS DMA */
    s3c2400_write32(g->soc, 0x14600058u, 2u);
    s3c2400_write32(g->soc, 0x15508008u, 5u << 5);
    s3c2400_write32(g->soc, 0x15508000u, 1u);
    CHECK(gp32_run_cycles(g, 17u + tag) == GP32_OK, "prime IIS and LCD fractions");
    /* IIS's old wire image contains its derived-cache dirty flag. Make that
     * flag dirty before the const capture, just as a temporary speed change
     * legitimately will. Rewriting the same clock leaves every phase exact. */
    s3c2400_write32(g->soc, 0x14800004u, 0x3000u);
    (void)s3c2400_read32(g->soc, 0x14a00000u); /* warm LCD observation cache */
    (void)s3c2400_read32(g->soc, 0x14a00010u);
    return g;
}

static uint8_t *capture_live(const gp32_t *g, size_t *size) {
    state_io_t io = state_io_counter();
    *size = 0u;
    if (!gp32_state_write(g, &io)) return NULL;
    uint8_t *data = malloc(io.pos);
    if (!data) return NULL;
    *size = io.pos;
    io = state_io_writer(data, *size);
    if (!gp32_state_write(g, &io)) { free(data); *size = 0u; return NULL; }
    return data;
}

static void expect_live(const gp32_t *g, const uint8_t *before, size_t size, const char *what) {
    size_t after_size = 0u;
    uint8_t *after = capture_live(g, &after_size);
    CHECK(after && after_size == size && memcmp(before, after, size) == 0, what);
    free(after);
}

static int write_file(const char *path, const void *data, size_t size) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    int ok = fwrite(data, 1u, size, f) == size;
    if (fclose(f) != 0) ok = 0;
    return ok;
}

static void compare_execution(gp32_t *g, gp32_t *control) {
    const uint32_t slices[] = {1u, 1u, 37u, 509u, 4093u};
    for (unsigned i = 0; i < GP32_ARRAY_COUNT(slices); ++i) {
        CHECK(gp32_run_cycles(g, slices[i]) == GP32_OK &&
              gp32_run_cycles(control, slices[i]) == GP32_OK, "resume both machines");
        int equal = arm920t_get_cycles(g->cpu) == arm920t_get_cycles(control->cpu) &&
                    arm920t_get_pc(g->cpu) == arm920t_get_pc(control->cpu);
        for (unsigned timer = 0; timer < 5u; ++timer)
            equal &= timer_count(g->soc, timer) == timer_count(control->soc, timer);
        equal &= s3c2400_read32(g->soc, 0x14400000u) == s3c2400_read32(control->soc, 0x14400000u);
        equal &= s3c2400_read32(g->soc, 0x14a00000u) == s3c2400_read32(control->soc, 0x14a00000u);
        equal &= s3c2400_read32(g->soc, 0x14a00010u) == s3c2400_read32(control->soc, 0x14a00010u);
        uint64_t frames = 0u, expected_frames = 0u;
        uint32_t rate = 0u, expected_rate = 0u;
        const int16_t *pcm = s3c2400_audio_samples(g->soc, &frames, &rate);
        const int16_t *expected_pcm = s3c2400_audio_samples(control->soc, &expected_frames, &expected_rate);
        equal &= frames == expected_frames && rate == expected_rate &&
                 (!frames || memcmp(pcm, expected_pcm, (size_t)frames * 2u * sizeof(*pcm)) == 0);
        CHECK(equal, "CPU, PWM, IRQ, LCD and PCM match after more guest cycles");
    }
    /* Exercise the previously retained idle-audio phase after active IIS. */
    s3c2400_write32(g->soc, 0x15508000u, 0u);
    s3c2400_write32(control->soc, 0x15508000u, 0u);
    CHECK(gp32_run_cycles(g, 2500u) == GP32_OK && gp32_run_cycles(control, 2500u) == GP32_OK,
          "resume idle audio");
    size_t size = 0u;
    uint8_t *expected = capture_live(control, &size);
    CHECK(expected != NULL, "capture executed control");
    if (expected) expect_live(g, expected, size, "full live state matches after execution");
    free(expected);
}

enum operation { COUNT, SAVE_BUFFER, SAVE_FILE, REJECT_BUFFER_EARLY, REJECT_BUFFER_LATE, REJECT_FILE_LATE };
static const char *operation_name[] = {"size", "save-buffer", "save-file", "reject-buffer-early",
                                       "reject-buffer-late", "reject-file-late"};

static void check_preserved(uint32_t percent, int tft, enum operation op, const char *path,
                            const uint8_t *valid, size_t valid_size) {
    snprintf(scenario, sizeof(scenario), "%s speed=%u lcd=%s", operation_name[op], percent, tft ? "TFT" : "fallback");
    ++cases;
    gp32_t *g = fixture(percent, tft, 0u), *control = fixture(percent, tft, 0u);
    uint8_t *before = NULL, *saved = NULL;
    if (!g || !control) goto done;
    size_t size = 0u;
    before = capture_live(g, &size);
    saved = malloc(size);
    CHECK(before && saved, "allocate live and output images");
    if (!before || !saved) goto done;
    gp32_status_t status = GP32_OK;
    switch (op) {
    case COUNT: CHECK(gp32_state_size(g) == size, "count is unchanged"); break;
    case SAVE_BUFFER:
        for (unsigned repeat = 0; repeat < 3u; ++repeat) {
            status = gp32_save_state_data(g, saved, size);
            CHECK(status == GP32_OK, "repeat buffer save succeeds");
            expect_live(g, before, size, "buffer save leaves exact live state");
        }
        break;
    case SAVE_FILE: status = gp32_save_state(g, path); break;
    case REJECT_BUFFER_EARLY: status = gp32_load_state_data(g, valid, 1u); break;
    case REJECT_BUFFER_LATE: status = gp32_load_state_data(g, valid, valid_size - 1u); break;
    case REJECT_FILE_LATE:
        CHECK(write_file(path, valid, valid_size - 1u), "write truncated file");
        status = gp32_load_state(g, path);
        break;
    }
    CHECK(status == (op >= REJECT_BUFFER_EARLY ? GP32_ERR_IO : GP32_OK), "operation status");
    CHECK(gp32_get_cpu_speed_percent(g) == percent &&
          s3c2400_run_clock_hz(g->soc) == s3c2400_run_clock_hz(control->soc), "original speed and derived clock restored");
    expect_live(g, before, size, "operation leaves exact live state");
    compare_execution(g, control);
done:
    free(before); free(saved); gp32_destroy(g); gp32_destroy(control);
}

static void check_loaded(uint32_t source_speed, uint32_t target_speed, int tft, int file, const char *path) {
    snprintf(scenario, sizeof(scenario), "load-%s speed=%u->%u lcd=%s", file ? "file" : "buffer",
             source_speed, target_speed, tft ? "TFT" : "fallback");
    ++cases;
    gp32_t *source = fixture(source_speed, tft, 17u);
    gp32_t *g = fixture(target_speed, tft, 0u);
    gp32_t *control = gp32_create(NULL);
    uint8_t *saved = NULL, *expected = NULL;
    if (!source || !g || !control) { CHECK(0, "create load machines"); goto done; }
    gp32_set_jit(control, 0);
    size_t size = gp32_state_size(source), expected_size = 0u;
    saved = malloc(size);
    CHECK(saved && gp32_save_state_data(source, saved, size) == GP32_OK, "save cross-speed source");
    if (!saved) goto done;
    /* Reference: accept the incoming nominal phases at 100%, then perform a
     * normal user speed change. Its pristine old phases deliberately differ
     * from the target's, catching accidental restoration of either machine's
     * pre-load phases after successful parsing. */
    CHECK(gp32_load_state_data(control, saved, size) == GP32_OK, "load nominal reference");
    CHECK(gp32_set_cpu_speed_percent(control, target_speed) == GP32_OK, "scale reference to target speed");
    if (file) {
        CHECK(write_file(path, saved, size), "write cross-speed state");
        CHECK(gp32_load_state(g, path) == GP32_OK, "accept cross-speed file");
    } else CHECK(gp32_load_state_data(g, saved, size) == GP32_OK, "accept cross-speed buffer");
    CHECK(gp32_get_cpu_speed_percent(g) == target_speed, "successful load keeps host speed");
    CHECK(arm920t_get_reg(g->cpu, 4u) == 17u, "incoming CPU state accepted");
    expected = capture_live(control, &expected_size);
    CHECK(expected != NULL, "capture loaded reference");
    if (expected) expect_live(g, expected, expected_size, "incoming phases retained at target speed");
    compare_execution(g, control);
done:
    free(saved); free(expected); gp32_destroy(source); gp32_destroy(g); gp32_destroy(control);
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    const uint32_t speeds[] = {100u, 133u, 150u};
    const uint32_t loads[][2] = {{100u, 150u}, {150u, 100u}, {150u, 133u}, {133u, 150u}};
    for (int tft = 0; tft < 2; ++tft) {
        snprintf(scenario, sizeof(scenario), "nominal rejection source lcd=%d", tft);
        gp32_t *source = fixture(100u, tft, 17u);
        size_t size = source ? gp32_state_size(source) : 0u;
        uint8_t *valid = malloc(size);
        CHECK(source && valid && gp32_save_state_data(source, valid, size) == GP32_OK, "create valid rejection source");
        if (source && valid) {
            for (unsigned speed = 0; speed < GP32_ARRAY_COUNT(speeds); ++speed)
                for (unsigned op = COUNT; op <= REJECT_FILE_LATE; ++op)
                    check_preserved(speeds[speed], tft, (enum operation)op, argv[1], valid, size);
        }
        free(valid); gp32_destroy(source);
        for (unsigned pair = 0; pair < GP32_ARRAY_COUNT(loads); ++pair)
            for (int file = 0; file < 2; ++file)
                check_loaded(loads[pair][0], loads[pair][1], tft, file, argv[1]);
    }
    (void)remove(argv[1]);
    printf("gp32_state_speed cases=%u failures=%u\n", cases, failures);
    return failures ? 1 : 0;
}
