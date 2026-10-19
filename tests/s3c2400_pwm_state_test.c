/* The v16 / GP32SO10 PWM extension must preserve live, buffered and zero
 * states independently, and reject every invalid/truncated tail before the
 * SoC commits RAM, SmartMedia, audio or IRQ changes. */
#include "../src/s3c2400.c"
#include "pwm_legacy_soc9_fixture.h"

#define PWM_TAIL_BYTES 92u
static unsigned cases, failures;
#define CHECK(c, msg) do { if (!(c)) { \
    if (failures < 64u) fprintf(stderr, "FAIL: %s (%d)\n", msg, __LINE__); \
    ++failures; \
} } while (0)

static uint8_t *capture(s3c2400_t *s, size_t *size) {
    FILE *f = tmpfile();
    *size = 0u;
    if (!f) return NULL;
    int ok = s3c2400_state_save(s, f);
    long end = ftell(f);
    uint8_t *data = ok && end > 0 ? malloc((size_t)end) : NULL;
    if (data) {
        rewind(f);
        if (fread(data, 1u, (size_t)end, f) == (size_t)end) *size = (size_t)end;
        else { free(data); data = NULL; }
    }
    fclose(f);
    return data;
}

static int restore(s3c2400_t *s, const uint8_t *data, size_t size) {
    FILE *f = tmpfile();
    if (!f) return 0;
    int ok = fwrite(data, 1u, size, f) == size;
    rewind(f);
    if (ok) ok = s3c2400_state_load(s, f);
    fclose(f);
    return ok;
}

static void equal_capture(s3c2400_t *s, const uint8_t *wanted, size_t wanted_size, const char *what) {
    size_t size;
    uint8_t *data = capture(s, &size);
    CHECK(data && size == wanted_size && !memcmp(data, wanted, size), what);
    free(data);
}

static s3c2400_t *fixture(unsigned speed, unsigned tag) {
    s3c2400_t *s = s3c2400_create(4096u);
    if (!s) return NULL;
    s3c2400_write32(s, 0x14800004u, 0u);
    s3c2400_write32(s, 0x14800014u, 0u);
    for (unsigned t = 0; t < 5u; ++t)
        s3c2400_write32(s, 0x1510000cu + t*12u, 11u + t*7u);
    s3c2400_write32(s, 0x15100008u, 0x7bbb0bu);
    s3c2400_write32(s, 0x15100008u, 0x599909u);
    s3c2400_tick(s, 7u);
    s3c2400_write32(s, 0x15100008u, 0x599909u & ~0x10100u); /* Stop 1 and 3. */
    s3c2400_write32(s, 0x1510000cu, 3u);
    s3c2400_write32(s, 0x1510003cu, 3u);
    s3c2400_write32(s, 0x15100008u, (0x599909u & ~0x10100u) | 0x200002u);
    s3c2400_write32(s, 0x15100008u, 0x599909u & ~0x10100u);
    s3c2400_write32(s, 0x1510000cu, 73u);
    s3c2400_write32(s, 0x1510003cu, 79u);
    s3c2400_tick(s, 5u); /* Retained half edge: both timers reach zero. */
    s3c2400_write32(s, 0x1510000cu, 83u);
    s3c2400_write32(s, 0x1510003cu, 89u);
    s3c2400_write32(s, 0x15100008u, 0x599909u & ~0x10101u); /* Stop 0 in zero phase. */
    CHECK(s3c2400_set_cpu_speed_percent(s, speed), "fixture speed");
    s3c2400_tick(s, 1u); /* Timer4 remains in zero phase at 100 and 150%. */
    s3c2400_write32(s, 0x0c000100u, 0x13579bdfu + tag);
    uint8_t card[2112u * 4u];
    memset(card, (int)(0x31u + tag), sizeof(card));
    CHECK(s3c2400_load_smartmedia_buffer(s, card, sizeof(card), NULL, 0u), "fixture card");
    s3c2400_audio_append_u8_mono(s, (const uint8_t[]){(uint8_t)(120u + tag)}, 1u, 44100u);
    return s;
}

static void check_roundtrip(void) {
    const unsigned speeds[] = {100u, 150u};
    const uint32_t steps[] = {1u, 1u, 3u, 13u, 149u, 503u};
    for (unsigned i = 0; i < GP32_ARRAY_COUNT(speeds); ++i) {
        ++cases;
        s3c2400_t *source = fixture(speeds[i], 0u), *target = fixture(speeds[i], 9u);
        CHECK(source && target, "roundtrip instances");
        if (!source || !target) { s3c2400_destroy(source); s3c2400_destroy(target); continue; }
        s3c2400_tick(target, 17u); /* Different live/zero state before the load. */
        size_t size;
        uint8_t *data = capture(source, &size);
        CHECK(data != NULL, "capture live/buffer/zero fixture");
        if (!data) { s3c2400_destroy(source); s3c2400_destroy(target); continue; }
        CHECK(!memcmp(data, "GP32SO10", 8u), "explicit new standalone magic");
        CHECK(size >= PWM_TAIL_BYTES && !memcmp(data + size - PWM_TAIL_BYTES, "PWM1", 4u),
              "explicit PWM extension tag");
        CHECK(restore(target, data, size), "load current PWM extension");
        equal_capture(target, data, size, "roundtrip preserves full SoC bytes");
        for (unsigned step = 0; step < GP32_ARRAY_COUNT(steps); ++step) {
            if (step == 1u) {
                s3c2400_write32(source, 0x15100008u, 0x599909u);
                s3c2400_write32(target, 0x15100008u, 0x599909u);
            }
            s3c2400_tick(source, steps[step]);
            s3c2400_tick(target, steps[step]);
            CHECK(!memcmp(source->pwm_count, target->pwm_count, sizeof(source->pwm_count)),
                  "restored live counts match continued execution");
            CHECK(!memcmp(source->pwm_accum, target->pwm_accum, sizeof(source->pwm_accum)) &&
                  source->pwm_reload_pending == target->pwm_reload_pending &&
                  !memcmp(source->pwm_reload, target->pwm_reload, sizeof(source->pwm_reload)),
                  "restored fractional and pending reload phases match");
        }
        free(data);
        data = capture(source, &size);
        CHECK(data != NULL, "capture continued reference");
        if (data) equal_capture(target, data, size, "continued restored SoC equals original");
        free(data);
        s3c2400_destroy(source); s3c2400_destroy(target);
    }
}

static void reject(s3c2400_t *s, const uint8_t *bad, size_t bad_size,
                   const uint8_t *live, size_t live_size, const char *what) {
    ++cases;
    CHECK(!restore(s, bad, bad_size), what);
    equal_capture(s, live, live_size, "rejected PWM tail leaves complete SoC unchanged");
}

static void check_transaction(void) {
    s3c2400_t *source = fixture(100u, 0u), *target = fixture(150u, 9u);
    CHECK(source && target, "transaction instances");
    if (!source || !target) { s3c2400_destroy(source); s3c2400_destroy(target); return; }
    size_t size = 0u, live_size = 0u;
    uint8_t *data = capture(source, &size), *live = capture(target, &live_size);
    uint8_t *bad = data ? malloc(size) : NULL;
    CHECK(data && live && bad, "transaction buffers");
    if (!data || !live || !bad) goto end;
    CHECK(size >= PWM_TAIL_BYTES && !memcmp(data + size - PWM_TAIL_BYTES, "PWM1", 4u), "new tail is present");
    if (size < PWM_TAIL_BYTES || memcmp(data + size - PWM_TAIL_BYTES, "PWM1", 4u)) goto end;
    for (unsigned cut = 1u; cut <= PWM_TAIL_BYTES; ++cut)
        reject(target, data, size - cut, live, live_size, "every PWM-tail truncation is rejected");
    for (unsigned field = 0; field < 8u; ++field) {
        memcpy(bad, data, size);
        uint8_t *tail = bad + size - PWM_TAIL_BYTES;
        if (field == 0u) tail[0] = 'X';
        if (field == 1u) gp32_st32le(tail + 4u, 0u); /* zero phase-clock denominator */
        if (field == 2u) gp32_st32le(tail + 8u, 32u); /* nonexistent pending timer */
        if (field == 3u) gp32_st32le(tail + 12u, 65536u); /* non-16-bit live count */
        if (field == 4u) gp32_st32le(tail + 16u, 65536u); /* non-16-bit latch */
        if (field == 5u) gp32_st32le(tail + 12u, 1u); /* pending load with nonzero live count */
        if (field == 6u) gp32_st32le(tail + 8u, 0u); /* nonzero latch without pending flag */
        if (field == 7u) { /* one-past the largest valid fraction */
            gp32_st32le(tail + 20u, gp32_ld32le(tail + 4u) * 2u);
            gp32_st32le(tail + 24u, 0u);
        }
        reject(target, bad, size, live, live_size, "invalid explicit PWM field is rejected");
    }
    /* Different host speed: convert the fraction, retain live/latch counts.
     * The equivalent control loads at the incoming speed, then changes speed. */
    s3c2400_t *control = s3c2400_create(4096u);
    CHECK(control != NULL, "cross-speed reference");
    if (control) {
        CHECK(restore(control, data, size) && s3c2400_set_cpu_speed_percent(control, 150u),
              "prepare cross-speed reference");
        CHECK(restore(target, data, size), "standalone cross-speed restore");
        s3c2400_tick(control, 117u);
        s3c2400_tick(target, 117u);
        size_t control_size;
        uint8_t *expected = capture(control, &control_size);
        CHECK(expected != NULL, "capture cross-speed reference");
        /* Other preexisting standalone phase sections use target-clock
         * semantics. This check is specifically the new PWM fraction. */
        CHECK(!memcmp(control->pwm_count, target->pwm_count, sizeof(control->pwm_count)) &&
              !memcmp(control->pwm_accum, target->pwm_accum, sizeof(control->pwm_accum)) &&
              !memcmp(control->pwm_reload, target->pwm_reload, sizeof(control->pwm_reload)),
              "PWM load fraction is independent of target host speed");
        free(expected);
        s3c2400_destroy(control);
    }
end:
    free(data); free(live); free(bad);
    s3c2400_destroy(source); s3c2400_destroy(target);
}

static void check_legacy(void) {
    uint32_t endian = 1u;
    if (sizeof(size_t) != 8u || *(uint8_t *)&endian != 1u) {
        puts("SKIP: frozen GP32SOC9 fixture requires its original little-endian LP64 ABI");
        return;
    }
    ++cases;
    uint8_t *data = calloc(1u, legacy_pwm_size);
    s3c2400_t *s = s3c2400_create(4096u);
    CHECK(data && s, "legacy fixture allocation");
    if (!data || !s) { free(data); s3c2400_destroy(s); return; }
    for (unsigned i = 0; i < GP32_ARRAY_COUNT(legacy_pwm_fills); ++i)
        memset(data + legacy_pwm_fills[i].offset, legacy_pwm_fills[i].value, legacy_pwm_fills[i].length);
    for (unsigned i = 0; i < GP32_ARRAY_COUNT(legacy_pwm_runs); ++i)
        memcpy(data + legacy_pwm_runs[i].offset, legacy_pwm_runs[i].data, legacy_pwm_runs[i].length);
    CHECK(restore(s, data, legacy_pwm_size), "actual pre-change GP32SOC9 image loads");
    const uint32_t expected[5] = {185u, 117u, 65521u, 151u, 4u};
    for (unsigned t = 0; t < 5u; ++t)
        CHECK(s3c2400_read32(s, t == 4u ? 0x15100040u : 0x15100014u + t*12u) == expected[t],
              "migration preserves the old observable counter where representable");
    CHECK(s3c2400_read32(s, 0x1510000cu) == 200u && s3c2400_read32(s, 0x15100024u) == 0u,
          "migration preserves distinct legacy buffer registers");
    size_t size;
    uint8_t *converted = capture(s, &size);
    CHECK(converted != NULL, "save migrated state in the new format");
    if (converted) {
        CHECK(restore(s, converted, size), "migrated state reloads with explicit live fields");
        s3c2400_tick(s, 1u);
        CHECK(s3c2400_read32(s, 0x15100014u) == 184u && s3c2400_read32(s, 0x15100040u) == 3u,
              "legacy running sub-tick fractions continue");
        s3c2400_write32(s, 0x15100008u, 0x599909u);
        s3c2400_tick(s, 2u);
        CHECK(s3c2400_read32(s, 0x15100020u) == 116u && s3c2400_read32(s, 0x15100038u) == 150u,
              "legacy stopped counters resume from their old observed values");
        free(converted);
    }
    free(data); s3c2400_destroy(s);
}

static void check_phase_bounds(void) {
    ++cases;
    s3c2400_t *source = fixture(100u, 0u), *target = fixture(150u, 9u);
    CHECK(source && target, "maximum fraction instances");
    if (!source || !target) { s3c2400_destroy(source); s3c2400_destroy(target); return; }
    size_t size = 0u, live_size = 0u;
    uint8_t *data = capture(source, &size), *live = NULL;
    CHECK(data && size > PWM_TAIL_BYTES + 8u, "maximum fraction input");
    if (!data || size <= PWM_TAIL_BYTES + 8u) goto done;
    s3c2400_state_image_t *body = (void *)(data + 8u);
    body->pwm[0] = 0xffffu; /* Both prescalers are 256. */
    body->pwm[1] = 0x33333u; /* Each input divider is sixteen. */
    uint8_t *tail = data + size - PWM_TAIL_BYTES;
    uint64_t largest = (uint64_t)UINT32_MAX * 4096u - 1u;
    gp32_st32le(tail + 4u, UINT32_MAX);
    for (unsigned t = 0u; t < 5u; ++t) {
        gp32_st32le(tail + 20u + t * 16u, (uint32_t)largest);
        gp32_st32le(tail + 24u + t * 16u, (uint32_t)(largest >> 32u));
    }
    state_io_t io = state_io_reader(data + 8u, size - 8u);
    s3c2400_pwm_phase_t phase = {0};
    CHECK(s3c2400_state_load_io_checked(target, &io, 1, 1, 1, 1, 1, 1,
          0u, 0u, 0u, SMC_STATE_FORMAT_V14, &phase), "largest bounded source fraction accepted");
    CHECK(phase.clock_hz == UINT32_MAX, "checked load returns original source denominator");
    for (unsigned t = 0u; t < 5u; ++t) {
        CHECK(phase.accum[t] == largest, "checked load returns original source numerator");
        /* floor((clock*4096 - 1) * target / clock) = target*4096 - 1.
         * This source numerator times target would overflow a direct product. */
        CHECK(target->pwm_accum[t] == (uint64_t)72000000u * 4096u - 1u,
              "large fraction rescales without overflow or loss of whole ticks");
    }
    live = capture(target, &live_size);
    CHECK(live != NULL, "capture maximum fraction target");
    if (!live) goto done;
    ++cases;
    ++largest; /* Exactly one timer tick is outside the canonical fraction. */
    gp32_st32le(tail + 20u, (uint32_t)largest);
    gp32_st32le(tail + 24u, (uint32_t)(largest >> 32u));
    memset(&phase, 0xa5, sizeof(phase));
    uint8_t untouched[sizeof(phase)];
    memcpy(untouched, &phase, sizeof(phase));
    io = state_io_reader(data + 8u, size - 8u);
    CHECK(!s3c2400_state_load_io_checked(target, &io, 1, 1, 1, 1, 1, 1,
          0u, 0u, 0u, SMC_STATE_FORMAT_V14, &phase), "one-past maximum source fraction rejected");
    CHECK(!memcmp(&phase, untouched, sizeof(phase)), "rejected load never publishes a partial phase output");
    equal_capture(target, live, live_size, "large invalid fraction leaves complete live SoC unchanged");
done:
    free(data); free(live); s3c2400_destroy(source); s3c2400_destroy(target);
}

int main(void) {
    check_roundtrip();
    check_transaction();
    check_legacy();
    check_phase_bounds();
    printf("PWM state scenarios=%u assertion_failures=%u\n", cases, failures);
    return failures ? 1 : 0;
}
