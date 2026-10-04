/* Exercises the real libretro run/delivery/lifecycle code with scripted PCM.
 * Only CPU stepping and the core PCM source are substituted; resampling,
 * callbacks, reset, state loading, and the silent-core check use real code.
 * Link this translation unit with gp32emu, not a second copy of libretro.c.
 */
#include <stdio.h>
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

gp32_status_t gp32_run_frame(gp32_t *);
gp32_status_t gp32_get_audio(gp32_t *, gp32_audio_desc_t *);
gp32_status_t gp32_consume_audio(gp32_t *, uint64_t);
gp32_status_t gp32_clear_audio(gp32_t *);
gp32_status_t gp32_get_framebuffer(gp32_t *, gp32_framebuffer_desc_t *);

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
     * Subsequent output timestamps are spaced half a new interval apart. */
    const int16_t want[] = {1166, -1167, 1666, -1667, 2166, -2167, 2666, -2667};
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
    feed(4, 11025, -2000);
    size_t count = captured_frames;
    memcpy(expected, captured, count * 2u * sizeof(int16_t));
    start_case();
    feed(4, 11025, 2000);
    captured_frames = 0;
    allowance = 0;
    feed(0, 11025, 0);
    allowance = SIZE_MAX;
    feed(4, 11025, -2000);
    CHECK(captured_frames == 735u + count, "rejected silence must be retained before resumed audio");
    all_zero = captured_frames >= 735u;
    for (size_t i = 0; i < 735u * 2u && all_zero; ++i) if (captured[i]) all_zero = 0;
    CHECK(all_zero, "silence must precede resumed PCM");
    CHECK(captured_frames == 735u + count &&
          !memcmp(expected, captured + 735u * 2u, count * 2u * sizeof(int16_t)),
          "resampling must not interpolate across an idle audio gap");

    start_case();
    feed(1, 11025, 100);
    CHECK(captured_frames == 0, "a resampler awaiting input must not insert silence");
    feed(1, 11025, 200);
    CHECK(captured_frames == 4, "short active blocks must not be padded or stretched");
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
    size_t got = fread(again, 1, size, file);
    int tail = fgetc(file);
    fclose(file);
    CHECK(got == size && tail == EOF && !memcmp(state, again, size),
          "memory serialization must match the existing file format byte for byte");

    CHECK(!retro_unserialize(state, 8u), "truncated state header must fail");
    CHECK(!retro_unserialize(state, size - 1u), "truncated state body must fail");
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
            if (captured[at] != sample || captured[at + 1u] != -sample) ordered = 0;
        }
    CHECK(ordered, "all retained stereo PCM must survive a recovered frontend in order");
    CHECK(audio_pending_frames == 0 && audio_resample_cap <= 11025,
          "draining and appending in one run must retain the bounded queue");
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
    CHECK(captured_frames == count && !memcmp(expected, captured, count * 2u * sizeof(int16_t)),
          "partial recovery must not repeat accepted PCM or drop extra retained frames");
}

int main(int argc, char **argv) {
    test_partial_and_blocked();
    test_resampled_and_sample_callback();
    test_mixed_spans();
    test_source_rate_phase();
    test_silence_and_gap();
    test_sustained_backpressure();
    test_partial_overflow_recovery();
    test_oversized_block_recovery();
    test_video_dupe();
    test_portrait_video();
    test_lifecycle(argc > 1 ? argv[1] : "libretro_audio_test.tmp");
    test_state_buffers(argc > 1 ? argv[1] : "libretro_audio_test.tmp");
    retro_deinit();
    if (failures) {
        fprintf(stderr, "libretro audio: %d failures\n", failures);
        return 1;
    }
    puts("PASS: partial/zero acceptance, exact PCM/resampling, silence, fallback, lifecycle");
    return 0;
}
