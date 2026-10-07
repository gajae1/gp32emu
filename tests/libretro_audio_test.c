/* Exercises the real libretro run/delivery/lifecycle code with scripted PCM.
 * Only CPU stepping and the core PCM source are substituted; resampling,
 * callbacks, reset, state loading, and the silent-core check use real code.
 * Link this translation unit with gp32emu, not a second copy of libretro.c.
 */
#include <stdio.h>
#include <math.h>
static unsigned state_file_opens;
static FILE *counted_fopen(const char *path, const char *mode) {
    ++state_file_opens;
    return fopen(path, mode);
}
#define fopen counted_fopen
#define gp32_run_frame test_run_frame
#define gp32_get_audio test_get_audio
#define gp32_consume_audio test_consume_audio
#define gp32_clear_audio test_clear_audio
#define gp32_get_framebuffer test_get_framebuffer
#define gp32_get_lcd_frame_period test_get_lcd_frame_period
#ifndef LIBRETRO_SOURCE
#define LIBRETRO_SOURCE "../src/libretro/libretro.c"
#endif
#include LIBRETRO_SOURCE
#undef fopen
#undef gp32_run_frame
#undef gp32_get_audio
#undef gp32_consume_audio
#undef gp32_clear_audio
#undef gp32_get_framebuffer
#undef gp32_get_lcd_frame_period

gp32_status_t gp32_run_frame(gp32_t *);
gp32_status_t gp32_get_audio(gp32_t *, gp32_audio_desc_t *);
gp32_status_t gp32_consume_audio(gp32_t *, uint64_t);
gp32_status_t gp32_clear_audio(gp32_t *);
gp32_status_t gp32_get_framebuffer(gp32_t *, gp32_framebuffer_desc_t *);
int gp32_get_lcd_frame_period(const gp32_t *, uint32_t *, uint32_t *);

#define CAPTURE_FRAMES 32768u
static int16_t input_pcm[4096u * 2u];
static int16_t captured[CAPTURE_FRAMES * 2u];
static int16_t expected[CAPTURE_FRAMES * 2u];
static gp32_audio_desc_t scripted_audio;
static gp32_audio_desc_t scripted_next_audio;
static gp32_framebuffer_desc_t scripted_fb;
static int scripted = 1;
static size_t captured_frames, allowance, per_call, callback_calls;
static int failures;
static uint32_t scripted_panel_period;
static double frontend_fps;

int test_get_lcd_frame_period(const gp32_t *g, uint32_t *ns, uint32_t *frac) {
    if (!scripted_panel_period) return gp32_get_lcd_frame_period(g, ns, frac);
    *ns = scripted_panel_period;
    *frac = 0;
    return 1;
}

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s (line %d)\n", message, __LINE__); \
        ++failures; \
    } \
} while (0)

gp32_status_t test_run_frame(gp32_t *g) {
    return scripted ? GP32_OK : gp32_run_frame(g);
}

gp32_status_t test_get_audio(gp32_t *g, gp32_audio_desc_t *out) {
    if (!scripted) return gp32_get_audio(g, out);
    *out = scripted_audio;
    return GP32_OK;
}

gp32_status_t test_clear_audio(gp32_t *g) {
    if (!scripted) return gp32_clear_audio(g);
    /* Make retaining a borrowed source pointer observably wrong. */
    memset(input_pcm, 0x5a, sizeof(input_pcm));
    scripted_audio.frame_count = 0;
    scripted_next_audio.frame_count = 0;
    return GP32_OK;
}

gp32_status_t test_consume_audio(gp32_t *g, uint64_t frames) {
    if (!scripted) return gp32_consume_audio(g, frames);
    if (frames > scripted_audio.frame_count) return GP32_ERR_INVALID_ARGUMENT;
    /* The oversized-source fixture lives in separate read-only storage. */
    uintptr_t ptr = (uintptr_t)scripted_audio.samples_s16_interleaved;
    if (ptr >= (uintptr_t)input_pcm && ptr < (uintptr_t)(input_pcm + 8192u)) {
        size_t offset = (ptr - (uintptr_t)input_pcm) / 4u;
        size_t poison = frames < 4096u - offset ? (size_t)frames : 4096u - offset;
        memset(input_pcm + offset * 2u, 0x5a, poison * 4u);
    }
    if (frames < scripted_audio.frame_count)
        scripted_audio.samples_s16_interleaved += (size_t)frames * 2u;
    scripted_audio.frame_count -= frames;
    if (!scripted_audio.frame_count) {
        scripted_audio = scripted_next_audio;
        memset(&scripted_next_audio, 0, sizeof(scripted_next_audio));
    }
    return GP32_OK;
}

gp32_status_t test_get_framebuffer(gp32_t *g, gp32_framebuffer_desc_t *out) {
    if (!scripted) return gp32_get_framebuffer(g, out);
    *out = scripted_fb;
    return GP32_OK;
}

static size_t capture_batch(const int16_t *data, size_t frames) {
    if (++callback_calls > CAPTURE_FRAMES * 2u) {
        fprintf(stderr, "FAIL: callback retried without progress\n");
        exit(2);
    }
    size_t accepted = frames < allowance ? frames : allowance;
    if (accepted > per_call) accepted = per_call;
    if (accepted > CAPTURE_FRAMES - captured_frames) {
        fprintf(stderr, "FAIL: capture overflow\n");
        exit(2);
    }
    memcpy(captured + captured_frames * 2u, data, accepted * 2u * sizeof(int16_t));
    captured_frames += accepted;
    allowance -= accepted;
    return accepted;
}

static void capture_sample(int16_t left, int16_t right) {
    int16_t stereo[2] = {left, right};
    (void)capture_batch(stereo, 1);
}

static void start_case(void);

/* Video capture for the frame-duplication path. */
static uint32_t video_pixels[GP32_W * GP32_H];
static int video_calls, video_null_calls;
static const void *video_last_data;
static unsigned video_last_w, video_last_h;
static size_t video_last_pitch;

static void record_video(const void *data, unsigned w, unsigned h, size_t pitch) {
    ++video_calls;
    if (!data) ++video_null_calls;
    video_last_data = data;
    video_last_w = w;
    video_last_h = h;
    video_last_pitch = pitch;
}

static bool env_can_dupe;
static bool env_var_update;
static const char *env_lcd_persistence;

static bool test_environ(unsigned cmd, void *data) {
    switch (cmd) {
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
        frontend_fps = ((struct retro_system_av_info *)data)->timing.fps;
        return true;
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
        *(bool *)data = env_can_dupe;
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *(bool *)data = env_var_update;
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        struct retro_variable *v = (struct retro_variable *)data;
        if (v && v->key && !strcmp(v->key, "gp32emu_lcd_persistence") && env_lcd_persistence) {
            v->value = env_lcd_persistence;
            return true;
        }
        return false;
    }
    default:
        return false;
    }
}

static void video_case(bool can_dupe_ok, uint64_t counter) {
    env_can_dupe = can_dupe_ok;
    retro_set_environment(test_environ);
    retro_set_video_refresh(record_video);
    start_case();
    scripted_fb.pixels_rgba8888 = video_pixels;
    scripted_fb.width = GP32_W;
    scripted_fb.height = GP32_H;
    scripted_fb.stride_pixels = GP32_W;
    scripted_fb.frame_counter = counter;
    video_calls = video_null_calls = 0;
    video_last_data = NULL;
    video_last_w = video_last_h = 0;
    video_last_pitch = 0;
}

/* The core may only send a NULL frame when the frontend answered true to
 * RETRO_ENVIRONMENT_GET_CAN_DUPE; otherwise it must resend the pixels it
 * presented last, including the effect buffer when a filter ran. */
static void test_video_dupe(void) {
    video_case(true, 100u);
    retro_run();
    CHECK(video_calls == 1 && video_last_data && !video_null_calls, "first frame must push pixels");
    retro_run();
    CHECK(video_calls == 2 && video_null_calls == 1,
          "unchanged frame must be a NULL duplicate when CAN_DUPE is true");

    scripted_fb.frame_counter = 101u;
    retro_run();
    CHECK(video_calls == 3 && video_null_calls == 1 && video_last_data,
          "a new frame must stage and push real pixels");

    retro_reset();
    scripted_fb.frame_counter = 101u;
    retro_run();
    CHECK(video_calls == 4 && video_null_calls == 1 && video_last_data,
          "reset must void frame identity so a repeated counter is re-staged");

    video_case(false, 200u);
    retro_run();
    CHECK(video_calls == 1 && video_last_data, "first frame must push pixels without CAN_DUPE");
    const void *first = video_last_data;
    retro_run();
    CHECK(video_null_calls == 0, "a frontend without CAN_DUPE must never receive NULL");
    CHECK(video_calls == 2 && video_last_data == first, "duplicate must resend the same buffer");
    CHECK(video_last_w == GP32_W && video_last_h == GP32_H && video_last_pitch == GP32_W * sizeof(uint32_t),
          "resent duplicate must carry full geometry");

    video_case(false, 300u);
    env_lcd_persistence = "enabled";
    env_var_update = true;
    retro_run();
    CHECK(video_calls == 1 && video_last_data && video_last_data != (const void *)frame_rgb,
          "an active effect pass must present the effect buffer");
    const void *effect_ptr = video_last_data;
    env_lcd_persistence = "disabled";
    env_var_update = true;
    retro_run();
    CHECK(video_calls == 2 && video_null_calls == 0,
          "a stopped effect pass must not leak a NULL duplicate");
    CHECK(video_last_data == effect_ptr,
          "the duplicate must resend the last effect buffer, not frame_rgb");

    env_var_update = false;
    env_lcd_persistence = NULL;
    gp32_video_effects_set(&effects, 0, 0);
    use_lcd_persistence = use_frame_interpolation = 0;
    retro_set_video_refresh(NULL);
    retro_set_environment(NULL);
}

/* Run-ahead and rewind serialize, run ahead, restore and run the same frames
 * again. The replay has to reproduce the duplicate decision, not only the
 * pixels: a state that forgets which frame the frontend already has turns a
 * NULL duplicate back into a fresh push. */
static void test_state_video_identity(void) {
    video_case(true, 500u);
    retro_run();
    CHECK(video_calls == 1 && !video_null_calls, "video state case must present its first frame");
    size_t size = retro_serialize_size();
    uint8_t *state = size ? (uint8_t *)malloc(size) : NULL;
    if (!state || !retro_serialize(state, size)) exit(2);

    retro_run();
    CHECK(video_null_calls == 1, "an unchanged counter must duplicate in the uninterrupted run");

    CHECK(retro_unserialize(state, size), "state must load for a video replay");
    retro_run();
    CHECK(video_calls == 3 && video_null_calls == 2,
          "the replayed frame must keep the duplicate decision of the saved state");

    free(state);
    retro_set_video_refresh(NULL);
    retro_set_environment(NULL);
}

static void test_portrait_video(void) {
    uint32_t *raw = malloc((243u * 320u + 1u) * sizeof(*raw));
    uint32_t *out = malloc((320u * 240u + 2u) * sizeof(*out));
    if (!raw || !out) exit(2);
    for (unsigned stride = 240u; stride <= 243u; stride += 3u) {
        memset(raw, 0x5a, (243u * 320u + 1u) * sizeof(*raw));
        for (unsigned y = 0; y < 320u; ++y)
            for (unsigned x = 0; x < 240u; ++x)
                raw[1u + y * stride + x] = (y << 16) | (x << 8) | ((x ^ y) & 0xffu);
        gp32_framebuffer_desc_t fb = {
            .pixels_rgba8888 = raw + 1, .width = 240, .height = 320,
            .stride_pixels = stride
        };
        out[0] = out[320u * 240u + 1u] = 0x12345678u;
        CHECK(stage_frame_320x240(&fb, out + 1), "portrait frame must stage");
        int exact = 1;
        /* Scatter source coordinates into the expected CCW destination. */
        for (unsigned y = 0; y < 320u; ++y)
            for (unsigned x = 0; x < 240u; ++x)
                if (out[1u + (239u - x) * 320u + y] !=
                    (raw[1u + y * stride + x] | 0xff000000u)) exact = 0;
        CHECK(exact, "portrait rotation must preserve every RGB pixel and force opaque alpha");
        CHECK(out[0] == 0x12345678u && out[320u * 240u + 1u] == 0x12345678u,
              "portrait rotation must preserve destination guards");
    }
    free(raw);
    free(out);
}

static void start_case(void) {
    retro_deinit();
    retro_init();
    emu = gp32_create(NULL);
    if (!emu) exit(2);
    scripted = 1;
    scripted_panel_period = 0;
    captured_frames = callback_calls = 0;
    allowance = per_call = SIZE_MAX;
    memset(&scripted_audio, 0, sizeof(scripted_audio));
    memset(&scripted_next_audio, 0, sizeof(scripted_next_audio));
    retro_set_audio_sample_batch(capture_batch);
    retro_set_audio_sample(capture_sample);
}

static void feed(size_t frames, uint32_t rate, int seed) {
    if (frames > 4096u) exit(2);
    for (size_t i = 0; i < frames; ++i) {
        input_pcm[i * 2u] = (int16_t)(seed + (int)i);
        input_pcm[i * 2u + 1u] = (int16_t)(-seed - (int)i);
    }
    scripted_audio.samples_s16_interleaved = input_pcm;
    scripted_audio.frame_count = frames;
    scripted_audio.sample_rate_hz = rate;
    callback_calls = 0;
    retro_run();
}

static void test_partial_and_blocked(void) {
    start_case();
    per_call = 3;
    feed(11, 44100, 100);
    CHECK(captured_frames == 11, "partial acceptance must deliver the entire block");

    start_case();
    feed(11, 44100, 100);
    feed(2048, 44100, 1000);
    feed(7, 44100, 7000);
    size_t count = captured_frames;
    memcpy(expected, captured, count * 2u * sizeof(int16_t));

    start_case();
    allowance = 3;
    feed(11, 44100, 100);
    CHECK(captured_frames == 3, "only accepted frames count as delivered");
    CHECK(callback_calls <= 2, "zero acceptance must end retries this run");
    feed(2048, 44100, 1000);
    CHECK(captured_frames == 3, "a blocked frontend must return promptly");
    allowance = SIZE_MAX;
    per_call = 17;
    feed(7, 44100, 7000);
    CHECK(captured_frames == count, "blocked frames must survive subsequent runs");
    CHECK(captured_frames == count && !memcmp(expected, captured, count * 2u * sizeof(int16_t)),
          "PCM must remain exact, ordered and independent of the source buffer");
}

static void test_resampled_and_sample_callback(void) {
    start_case();
    feed(64, 11025, 100);
    feed(1500, 11025, 1000);
    feed(257, 48000, 7000);
    feed(1, 44100, 9000);
    size_t count = captured_frames;
    memcpy(expected, captured, count * 2u * sizeof(int16_t));

    start_case();
    allowance = 5;
    feed(64, 11025, 100);
    feed(1500, 11025, 1000);
    feed(257, 48000, 7000);
    /* The fallback must also drain any earlier batch-callback backlog. */
    allowance = SIZE_MAX;
    retro_set_audio_sample_batch(NULL);
    feed(1, 44100, 9000);
    CHECK(captured_frames == count && !memcmp(expected, captured, count * 2u * sizeof(int16_t)),
          "resampled PCM/rate changes must be bit-exact under backpressure and fallback");
}

static void test_mixed_spans(void) {
    start_case();
    feed(11, 11025, 100);
    feed(17, 22050, 1000);
    size_t count = captured_frames;
    memcpy(expected, captured, count * 4u);

    start_case();
    for (unsigned i = 0; i < 28; ++i) {
        int v = i < 11 ? 100 + (int)i : 1000 + (int)i - 11;
        input_pcm[i * 2u] = (int16_t)v;
        input_pcm[i * 2u + 1u] = (int16_t)-v;
    }
    scripted_audio = (gp32_audio_desc_t){input_pcm, 11, 11025};
    scripted_next_audio = (gp32_audio_desc_t){input_pcm + 22, 17, 22050};
    allowance = 3;
    retro_run();
    CHECK(!scripted_audio.frame_count && !scripted_next_audio.frame_count,
          "one run must retain both rate spans before releasing borrowed PCM");
    CHECK(captured_frames == 3, "mixed spans respect frontend backpressure");
    allowance = SIZE_MAX;
    flush_audio();
    CHECK(captured_frames == count && !memcmp(expected, captured, count * 4u),
          "mixed spans match separate deliveries without loss, replay or padding");
}

static void test_source_rate_phase(void) {
    gp32_audio_resampler_t r;
    gp32_audio_resampler_init(&r);
    const int16_t first[] = {0, 0, 1000, -1000};
    const int16_t second[] = {2000, -2000, 3000, -3000};
    int16_t out[80];
    size_t n = gp32_audio_resampler_process(&r, first, 2, 72000, 48000, 0, out, 40);
    CHECK(n == 1 && out[0] == 0, "first output precedes source rate switch");
    n = gp32_audio_resampler_process(&r, second, 2, 24000, 48000, 0, out, 40);
    /* Half of a 72-kHz interval is one sixth of a 24-kHz interval.
     * Subsequent output timestamps are spaced half a new interval apart.
     * The anti-imaging kernel trails the sample grid by its 32-source-frame
     * group delay, so these outputs still reconstruct the primed (zero) head
     * of the stream; an independent Q15 model of this kernel produces exactly
     * these values.  The count pins the wall-time phase. */
    const int16_t want[] = {0, 0, 0, 0, 0, 0, 0, 0};
    CHECK(n == 4 && !memcmp(out, want, sizeof(want)),
          "rate switch preserves wall-time phase and the previous sample");
}

static void test_silence_and_gap(void) {
    start_case();
    scripted = 0;
    /* A newly reset real core has no active audio; do not fake the descriptor. */
    retro_run();
    retro_set_audio_sample_batch(NULL);
    retro_run();
    CHECK(captured_frames == 1470, "two silent video frames must deliver 1470 stereo frames");
    int all_zero = 1;
    for (size_t i = 0; i < captured_frames * 2u; ++i) if (captured[i]) all_zero = 0;
    CHECK(all_zero, "silent-core output must be zero in both callback modes");

    start_case();
    feed(1, 11025, 100);
    CHECK(captured_frames == 0, "a resampler awaiting input must not insert silence");
    feed(1, 11025, 200);
    CHECK(captured_frames == 4, "short active blocks must not be padded or stretched");
}

static void constant_audio(size_t frames, uint32_t rate, int16_t value) {
    for (size_t i = 0; i < frames; ++i) {
        input_pcm[i * 2u] = value;
        input_pcm[i * 2u + 1u] = (int16_t)-value;
    }
    scripted_audio = (gp32_audio_desc_t){input_pcm, frames, rate};
    callback_calls = 0;
    retro_run();
}

static void test_idle_boundaries(void) {
    /* Independent 4:1 sample grid: two constants supply one interval;
     * floor(11025/60)=183 zero samples supply 183 more intervals.  The
     * anti-imaging kernel rings for 32 source frames on either side of a
     * level step, but it never shifts a level: outside that window a constant
     * input reproduces the constant within the one-LSB Q15 ripple, and a
     * silence gap reproduces exact zero.  Values were checked against an
     * independent Q15 model of this kernel. */
    start_case();
    allowance = 2;
    constant_audio(2, 11025, -4000);
    constant_audio(0, 11025, 0);
    constant_audio(2, 11025, 8000);
    CHECK(captured_frames == 2 && audio_pending_frames == 742,
          "idle carry and fractional counts survive a blocked callback");
    allowance = SIZE_MAX;
    per_call = 7;
    flush_audio();
    CHECK(captured_frames == 744, "idle emits 732 frames, not forced 735");
    int levels = captured_frames == 744;
    for (size_t i = 0; i < 16 && levels; ++i)
        if (captured[i * 2u] < -4001 || captured[i * 2u] > -3999 ||
            captured[i * 2u + 1u] != -captured[i * 2u]) levels = 0;
    for (size_t i = 260; i < 741 && levels; ++i)
        if (captured[i * 2u] != 0 || captured[i * 2u + 1u] != 0) levels = 0;
    /* The following 8-kHz block is shorter than the kernel's group delay, so
     * only its silent leading edge can appear in this capture. */
    for (size_t i = 741; i < 744 && levels; ++i)
        if (captured[i * 2u] < -2 || captured[i * 2u] > 2 ||
            captured[i * 2u + 1u] != -captured[i * 2u]) levels = 0;
    CHECK(levels, "idle boundaries keep constant and silent levels without spurious samples");
    retro_set_audio_sample_batch(NULL);
    constant_audio(0, 11025, 0);
    CHECK(captured_frames == 1480, "next idle retains the three-quarter source fraction");

    start_case();
    constant_audio(2, 44100, -4000);
    constant_audio(0, 11025, 0);
    CHECK(captured_frames == 733, "direct copy retains last sample and one output tick across a rate change");
    int resumed = captured_frames == 733;
    for (size_t i = 0; i < 16 && resumed; ++i)
        if (captured[i * 2u] < -4001 || captured[i * 2u] > -3999 ||
            captured[i * 2u + 1u] != -captured[i * 2u]) resumed = 0;
    CHECK(resumed, "a direct copy leaves the resampler primed so a later rate change has no hole or jump");
}

static void test_idle_rate_counts(void) {
    const uint32_t rates[] = {11025, 11035, 32000, 48000, 96000};
    for (size_t r = 0; r < sizeof(rates) / sizeof(rates[0]); ++r) {
        start_case();
        size_t total = 0;
        for (unsigned frame = 0; frame < 60; ++frame) {
            captured_frames = 0;
            constant_audio(0, rates[r], 0);
            total += captured_frames;
        }
        /* One second provides rate samples, hence rate-1 intervals on a
         * fresh interpolator. Rational ceiling is independent of its Q32 code. */
        size_t want = ((uint64_t)(rates[r] - 1u) * 44100u + rates[r] - 1u) / rates[r];
        CHECK(total == want, "one-second idle count follows source intervals and fractional budget");
    }
    start_case();
    constant_audio(2, 11025, 0);
    constant_audio(0, 11025, 0);
    constant_audio(0, 22050, 0);
    /* 0.75/11025 seconds of debt becomes 1.5 source samples at 22050;
     * the next idle supplies floor(367.5+1.5)=369 intervals, 738 outputs. */
    CHECK(captured_frames == 1474, "idle source-rate switch preserves fractional wall time");
    retro_reset();
    captured_frames = 0;
    constant_audio(0, 22050, 0);
    CHECK(captured_frames == 732, "reset clears idle time debt and interpolation history");
}

static void test_lifecycle(const char *state_path) {
    start_case();
    allowance = 0;
    feed(4, 11025, 3000);
    retro_reset();
    allowance = SIZE_MAX;
    feed(2, 44100, -100);
    CHECK(captured_frames == 2 && captured[0] == -100, "reset must discard old audio");

    allowance = 0;
    feed(4, 11025, 3000);
    retro_unload_game();
    emu = gp32_create(NULL);
    if (!emu) exit(2);
    captured_frames = 0;
    allowance = SIZE_MAX;
    feed(2, 44100, -200);
    CHECK(captured_frames == 2 && captured[0] == -200, "unload must discard old audio");

    unsigned opens_before = state_file_opens;
    size_t state_size = retro_serialize_size();
    CHECK(state_file_opens == opens_before, "state size query must not open files");
    void *state = malloc(state_size);
    if (!state_size || !state || !retro_serialize(state, state_size)) exit(2);
    CHECK(!retro_serialize(state, state_size - 1), "short state buffer must fail instead of truncating");
    CHECK(state_file_opens == opens_before, "state serialization must not open files");
    allowance = 0;
    feed(4, 11025, 3000);
    CHECK(retro_unserialize(state, state_size), "valid state must load");
    captured_frames = 0;
    allowance = SIZE_MAX;
    feed(2, 44100, -300);
    CHECK(captured_frames == 2 && captured[0] == -300, "state load must discard old audio");

    start_case();
    feed(4, 11025, 3000);
    feed(4, 11025, 4000);
    size_t count = captured_frames;
    memcpy(expected, captured, count * 2u * sizeof(int16_t));
    start_case();
    allowance = 0;
    feed(4, 11025, 3000);
    memset(state, 0, state_size < 16u ? state_size : 16u);
    CHECK(!retro_unserialize(state, state_size), "invalid state must fail to load");
    allowance = SIZE_MAX;
    feed(4, 11025, 4000);
    CHECK(captured_frames == count && !memcmp(expected, captured, count * 2u * sizeof(int16_t)),
          "failed state load must preserve pending PCM and resampler continuity");
    free(state);
    remove(state_path);
}

static void test_state_buffers(const char *state_path) {
    start_case();
    uint8_t card[528u * 128u];
    memset(card, 0xff, sizeof(card));
    card[71] = 0x42;
    CHECK(gp32_load_smartmedia_data(emu, card, sizeof(card)) == GP32_OK,
          "synthetic NAND image must load for state roundtrip");
    unsigned opens_before = state_file_opens;
    size_t size = retro_serialize_size();
    uint8_t *state = (uint8_t *)malloc(size + 32u);
    uint8_t *again = (uint8_t *)malloc(size);
    if (!size || !state || !again) exit(2);
    memset(state, 0xa5, size + 32u);
    CHECK(retro_serialize(state, size + 16u), "oversized state buffer must be accepted");
    int padded = 1, guard = 1;
    for (size_t i = size; i < size + 16u; ++i) if (state[i]) padded = 0;
    for (size_t i = size + 16u; i < size + 32u; ++i) if (state[i] != 0xa5) guard = 0;
    CHECK(padded && guard, "serialize must zero padding and preserve buffer guard");
    CHECK(!retro_serialize(state, size - 1u), "short buffer must reject the complete state");
    CHECK(retro_serialize(again, size) && !memcmp(state, again, size),
          "repeated serialization must be deterministic");
    CHECK(state_file_opens == opens_before, "size and buffer save must use no frontend files");

    CHECK(gp32_save_state(emu, state_path) == GP32_OK, "file state must save");
    FILE *file = fopen(state_path, "rb");
    if (!file) exit(2);
    size_t guest = gp32_state_size(emu);
    size_t got = fread(again, 1, guest, file);
    int tail = fgetc(file);
    fclose(file);
    CHECK(got == guest && tail == EOF && !memcmp(state, again, guest),
          "memory serialization must keep the file's guest payload byte for byte");
    CHECK(size > guest && !memcmp(state + guest, "LRST", 4u),
          "memory serialization must append the versioned delivery section");

    CHECK(!retro_unserialize(state, 8u), "truncated state header must fail");
    /* The reported size is an upper bound; cut inside the delivery body. */
    CHECK(!retro_unserialize(state, guest + 32u), "truncated state body must fail");
    gp32_reset(emu);
    CHECK(retro_unserialize(state, size + 16u), "padded complete state must load");
    CHECK(retro_serialize(again, size) && !memcmp(state, again, size),
          "NAND and machine state must survive memory roundtrip");
    CHECK(state_file_opens == opens_before, "memory load must use no frontend files");
    CHECK(gp32_load_state(emu, state_path) == GP32_OK, "file wrapper must still load");
    free(state);
    free(again);
    remove(state_path);
}

/* The same guarantee for the delivery queue: rewind and run-ahead replay the
 * frames after a state, so the resampler phase and history that a load used to
 * clear have to come back for the replayed PCM to match the uninterrupted one. */
static void test_state_replay_continuity(void) {
    start_case();
    feed(4, 11025, 3000);
    feed(3, 22050, 500);
    size_t size = retro_serialize_size();
    uint8_t *state = size ? (uint8_t *)malloc(size) : NULL;
    if (!state || !retro_serialize(state, size)) exit(2);

    size_t base = captured_frames;
    feed(5, 44100, 900);
    feed(2, 11025, 1300);
    size_t count = captured_frames - base;
    memcpy(expected, captured + base * 2u, count * 2u * sizeof(int16_t));

    CHECK(retro_unserialize(state, size), "state must load for an audio replay");
    captured_frames = base;
    feed(5, 44100, 900);
    feed(2, 11025, 1300);
    CHECK(captured_frames == base + count &&
          !memcmp(expected, captured + base * 2u, count * 2u * sizeof(int16_t)),
          "replayed PCM must match the uninterrupted run after a state load");
    free(state);
}

static void test_sustained_backpressure(void) {
    start_case();
    allowance = 0;
    for (int i = 0; i < 120; ++i) feed(735, 44100, i * 100);
    CHECK(captured_frames == 0, "blocked frontend must receive no audio");
    CHECK(audio_resample_cap <= 11025, "two seconds of refusal must not grow the audio allocation beyond 250ms");
    allowance = SIZE_MAX;
    feed(735, 44100, 12000);
    CHECK(captured_frames == 11760, "recovery must drain retained audio before admitting a new block");
    int ordered = captured_frames == 11760;
    for (size_t block = 0; block < 16u && ordered; ++block)
        for (size_t frame = 0; frame < 735u; ++frame) {
            int16_t sample = (int16_t)(10500 + block * 100u + frame);
            size_t at = (block * 735u + frame) * 2u;
            if (block == 0u && frame < 44u) {
                if (captured[at] <= 0 || captured[at] > sample ||
                    captured[at + 1u] != -captured[at]) ordered = 0;
            } else if (captured[at] != sample || captured[at + 1u] != -sample) ordered = 0;
        }
    CHECK(ordered, "retained PCM stays exact after the one-millisecond recovery prefix");
    CHECK(audio_pending_frames == 0 && audio_resample_cap <= 11025,
          "draining and appending in one run must retain the bounded queue");
}

static void test_resampled_queue_boundary(void) {
    /* A real rate change leaves a fractional phase at 1:1. The blocked
     * stream must match an immediately accepted stream while it still fits. */
    start_case();
    feed(2u, 32000u, 1000);
    for (unsigned i = 0; i < 3u; ++i) feed(3400u, 44100u, 2000 + (int)i * 4000);
    feed(800u, 44100u, 16000);
    size_t count = captured_frames;
    memcpy(expected, captured, count * 2u * sizeof(int16_t));
    start_case();
    feed(2u, 32000u, 1000);
    allowance = 0;
    for (unsigned i = 0; i < 3u; ++i) feed(3400u, 44100u, 2000 + (int)i * 4000);
    feed(800u, 44100u, 16000);
    CHECK(audio_pending_frames == 11000u && !audio_gap_left,
          "resampler capacity slack must not evict PCM that fits the queue");
    allowance = SIZE_MAX;
    flush_audio();
    CHECK(captured_frames == count && !memcmp(captured, expected, count * 2u * sizeof(int16_t)),
          "near-limit resampled backpressure preserves every PCM sample");

    /* This complete upsampled span fits, even though the conservative
     * allocation estimate exceeds the queue limit. Use the actual converter
     * as the independent source of the expected stream. */
    start_case();
    for (size_t i = 0; i < 1995u; ++i) {
        input_pcm[i * 2u] = (int16_t)(1000 + i);
        input_pcm[i * 2u + 1u] = (int16_t)(-1000 - (int)i);
    }
    gp32_audio_resampler_t reference;
    gp32_audio_resampler_init(&reference);
    count = gp32_audio_resampler_process(&reference, input_pcm, 1995u, 8000u,
                                          44100u, 0, expected, CAPTURE_FRAMES);
    CHECK(count > 10000u && count <= GP32_AUDIO_QUEUE_LIMIT, "upsampled boundary fixture fits");
    feed(1995u, 8000u, 1000);
    CHECK(captured_frames == count && !memcmp(captured, expected, count * 2u * sizeof(int16_t)),
          "a fitting upsampled span must not be discarded as oversized");

    /* Changing from 44.1 to 96 kHz can consume a source frame before another
     * output is due. A full queue must not lose data to reserve unused room. */
    start_case();
    allowance = 0;
    for (unsigned i = 0; i < 15u; ++i) feed(735u, 44100u, 1000);
    memcpy(expected, audio_resample_buf, GP32_AUDIO_QUEUE_LIMIT * 2u * sizeof(int16_t));
    feed(1u, 96000u, 2000);
    CHECK(audio_pending_frames == GP32_AUDIO_QUEUE_LIMIT && !audio_gap_left &&
          !memcmp(audio_resample_buf, expected, GP32_AUDIO_QUEUE_LIMIT * 2u * sizeof(int16_t)),
          "a zero-output rate transition must preserve the full queue");
}

static void test_oversized_block_recovery(void) {
    static const int16_t oversized[12000u * 2u] = {0};
    start_case();
    allowance = 0;
    feed(735u, 44100u, 1000);
    CHECK(captured_frames == 0u, "oversized recovery fixture queues blocked PCM");
    allowance = SIZE_MAX;
    per_call = 17u;
    scripted_audio = (gp32_audio_desc_t){oversized, 12000u, 44100u};
    retro_run();
    CHECK(captured_frames == 735u, "oversized block must drain recoverable backlog before discard");
    int exact = captured_frames == 735u;
    for (size_t i = 0; i < captured_frames && exact; ++i)
        if (captured[i * 2u] != 1000 + (int)i || captured[i * 2u + 1u] != -1000 - (int)i)
            exact = 0;
    CHECK(exact, "oversized recovery preserves accepted PCM order and values");
    CHECK(audio_pending_frames == 0u && scripted_audio.frame_count == 0u,
          "oversized source is discarded without retaining excessive latency");
}

static void test_discard_gap_recovery(void) {
    static const int16_t oversized[12000u * 2u] = {0};
    for (unsigned scenario = 0; scenario < 4u; ++scenario) {
        unsigned oversized_case = scenario & 1u, rediscard = scenario >= 2u;
        start_case();
        feed(1u, 44100u, 16000);
        allowance = 0;
        if (oversized_case) {
            scripted_audio = (gp32_audio_desc_t){oversized, 12000u, 44100u};
            retro_run();
            feed(96u, 44100u, -16000);
        } else {
            for (unsigned i = 0; i < 16u; ++i) feed(735u, 44100u, -16000);
        }
        size_t retained = oversized_case ? 96u : 11025u;
        CHECK(captured_frames == 1u && audio_pending_frames == retained,
              "gap fixture discards source without delivering blocked PCM");
        /* Advance across partial acceptances with zero-acceptance retries.
         * A retry must not advance or apply the recovery transition twice. */
        allowance = 7u;
        per_call = 3u;
        flush_audio();
        CHECK(captured_frames == 8u, "partial gap recovery accepts exactly seven frames");
        flush_audio();
        CHECK(captured_frames == 8u, "zero acceptance leaves the gap pending");
        if (rediscard) {
            if (oversized_case) {
                scripted_audio = (gp32_audio_desc_t){oversized, 12000u, 44100u};
                retro_run();
                feed(96u, 44100u, -16000);
            } else feed(735u, 44100u, -16000);
            CHECK(captured_frames == 8u && audio_pending_frames == retained,
                  "another discard restarts from the partially accepted recovery endpoint");
        }
        allowance = SIZE_MAX;
        if (scenario == 3u) retro_set_audio_sample_batch(NULL);
        flush_audio();
        size_t gap_start = rediscard ? 8u : 1u;
        CHECK(captured_frames == retained + gap_start && !audio_pending_frames,
              "gap recovery preserves the retained frame count");
        int bounded = 1, exact_tail = 1;
        for (size_t i = 1; i < captured_frames; ++i) {
            for (unsigned ch = 0; ch < 2u; ++ch) {
                int step = (int)captured[i * 2u + ch] - captured[(i - 1u) * 2u + ch];
                if (step < -1000 || step > 1000) bounded = 0;
                if (i >= gap_start + 44u) {
                    int raw = -16000 + (int)((i - gap_start) % 735u);
                    if (captured[i * 2u + ch] != (ch ? -raw : raw)) exact_tail = 0;
                }
            }
        }
        CHECK(bounded, "forced-discard recovery must not emit the 32000-step discontinuity");
        CHECK(exact_tail, "only the short recovery prefix may change retained PCM");
        CHECK(captured[(captured_frames - 1u) * 2u] ==
                  -16000 + (int)((retained - 1u) % 735u),
              "recovery reaches the original waveform");
    }
}

static void test_partial_overflow_recovery(void) {
    /* After accepting 300 frames, a full queue can retain only the last
     * 14 old blocks plus the incoming block. Capture that stream independently. */
    start_case();
    feed(300, 44100, 10500);
    for (int i = 106; i <= 120; ++i) feed(735, 44100, i * 100);
    feed(1, 44100, 13000);
    size_t count = captured_frames;
    memcpy(expected, captured, count * 2u * sizeof(int16_t));

    start_case();
    allowance = 0;
    for (int i = 0; i < 120; ++i) feed(735, 44100, i * 100);
    allowance = 300;
    feed(735, 44100, 12000);
    CHECK(captured_frames == 300 && audio_pending_frames == 11025,
          "partial recovery must drain accepted PCM and bound the remainder");
    allowance = SIZE_MAX;
    feed(1, 44100, 13000);
    int exact = captured_frames == count;
    for (size_t i = 0; i < count && exact; ++i) {
        int recovery = i < 44u || (i >= 300u && i < 344u);
        for (unsigned ch = 0; ch < 2u; ++ch) {
            int actual = captured[i * 2u + ch], raw = expected[i * 2u + ch];
            int from = i < 44u ? 0 : expected[299u * 2u + ch];
            int low = from < raw ? from : raw, high = from > raw ? from : raw;
            if (recovery ? (actual < low || actual > high) : actual != raw) exact = 0;
        }
    }
    CHECK(exact, "partial recovery preserves every retained frame outside the two gap prefixes");
}

/* A frontend schedules retro_run at the advertised rate, not the LCD rate.
 * Every run advances 1/60 second and must supply the matching audio duration,
 * including when a game reprograms its independent LCD divider. */
static void test_frontend_audio_clock(void) {
    const uint32_t periods[] = {20000000u, 16891892u, 11261261u};
    for (unsigned k = 0; k < GP32_ARRAY_COUNT(periods); ++k) {
        start_case();
        retro_set_environment(test_environ);
        struct retro_system_av_info info;
        retro_get_system_av_info(&info);
        frontend_fps = info.timing.fps;
        scripted_panel_period = periods[k];
        feed(735u, 44100u, 0); /* let a timing update reach the frontend */
        captured_frames = 0;
        double frontend_seconds = 0.0;
        for (unsigned frame = 0; frame < 12u; ++frame) {
            frontend_seconds += 1.0 / frontend_fps;
            feed(735u, 44100u, 0);
        }
        double audio_seconds = (double)captured_frames / info.timing.sample_rate;
        fprintf(stderr, "frontend-clock panel_ns=%u advertised=%.3f audio=%.6f wall=%.6f\n",
                periods[k], frontend_fps, audio_seconds, frontend_seconds);
        CHECK(fabs(audio_seconds - frontend_seconds) < 1.0 / 44100.0,
              "advertised run cadence must match produced audio duration");
        scripted_panel_period = 0;
        retro_set_environment(NULL);
    }
}

/* A valid full-scale signal can exceed a 32-bit FIR accumulator before the
 * final s16 clip. Exercise both contiguous taps and carried history. */
static void test_resampler_peak_clipping(void) {
    gp32_audio_resampler_t r;
    int16_t zero[4] = {0}, out[2], src[132];
    for (unsigned history = 0; history < 2; ++history) {
        gp32_audio_resampler_init(&r);
        gp32_audio_resampler_process(&r, zero, 2, 23144, 44100, 0, out, 1);
        unsigned phase = 0;
        int largest = 0;
        for (unsigned p = 0; p < GP32_AUDIO_POLY_PHASES; ++p) {
            int magnitude = 0;
            for (unsigned i = 0; i < GP32_AUDIO_POLY_TAPS; ++i) {
                int c = r.poly_coef[p][i];
                magnitude += c < 0 ? -c : c;
            }
            if (magnitude > largest) { largest = magnitude; phase = p; }
        }
        memset(src, 0, sizeof(src));
        int64_t sum_l = 0, sum_r = 0;
        for (unsigned i = 0; i < GP32_AUDIO_POLY_TAPS; ++i) {
            int c = r.poly_coef[phase][i];
            int16_t l = c < 0 ? INT16_MIN : INT16_MAX;
            int16_t rr = c < 0 ? INT16_MAX : INT16_MIN;
            sum_l += (int64_t)c * l;
            sum_r += (int64_t)c * rr;
            if (!history) { src[(64u - i) * 2u] = l; src[(64u - i) * 2u + 1u] = rr; }
            else if (!i) { r.prev_l = l; r.prev_r = rr; }
            else { r.poly_hist_l[64u - i] = l; r.poly_hist_r[64u - i] = rr; }
        }
        CHECK(sum_l > INT32_MAX && sum_r < INT32_MIN, "peak fixture crosses both accumulator limits");
        r.have_prev = history != 0;
        r.phase_q32 = ((uint64_t)(history ? 0u : 64u) << 32) | ((uint64_t)phase << 24);
        CHECK(gp32_audio_resampler_process(&r, src, history ? 1u : 66u,
                  23144, 44100, 0, out, 1) == 1u, "peak fixture emits a sample");
        CHECK(out[0] == INT16_MAX && out[1] == INT16_MIN,
              "FIR peaks saturate without wrapping to the opposite polarity");
    }
}

int main(int argc, char **argv) {
    test_resampler_peak_clipping();
    test_frontend_audio_clock();
    test_partial_and_blocked();
    test_resampled_and_sample_callback();
    test_mixed_spans();
    test_source_rate_phase();
    test_silence_and_gap();
    test_idle_boundaries();
    test_idle_rate_counts();
    test_sustained_backpressure();
    test_partial_overflow_recovery();
    test_resampled_queue_boundary();
    test_oversized_block_recovery();
    test_discard_gap_recovery();
    test_video_dupe();
    test_state_video_identity();
    test_portrait_video();
    test_lifecycle(argc > 1 ? argv[1] : "libretro_audio_test.tmp");
    test_state_buffers(argc > 1 ? argv[1] : "libretro_audio_test.tmp");
    test_state_replay_continuity();
    retro_deinit();
    if (failures) {
        fprintf(stderr, "libretro audio: %d failures\n", failures);
        return 1;
    }
    puts("PASS: partial/zero acceptance, exact PCM/resampling, silence, fallback, lifecycle");
    return 0;
}
