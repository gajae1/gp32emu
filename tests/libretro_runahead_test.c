/* RetroArch run-ahead and rewind serialize at every displayed frame, run the
 * hidden frames, restore the captured state and replay the same inputs; a
 * second core instance loads the state instead. The replay only reproduces the
 * uninterrupted run while everything this file carries *between* retro_run
 * calls travels in the state: the delivery section (LRST) with the queued and
 * resampled audio, the idle-silence budget, and the duplicate-frame decision.
 *
 * This test replays a scripted A/V timeline from a captured state, compares
 * every frame's delivered video and audio against the uninterrupted run, and
 * requires the state captured after the replay to be byte-identical. It also
 * pins retro_serialize_size across the session and checks that repeated
 * rollbacks never make the SmartMedia autosave rewrite the card file.
 *
 * Only CPU stepping and the core PCM source are substituted; savestate
 * save/load, resampling, callbacks and the card autosave poll are real code.
 * Everything is synthetic, so no copyrighted fixture is needed.
 * Link this translation unit with gp32emu, not a second copy of libretro.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#define make_dir(p) _mkdir(p)
#else
#include <sys/stat.h>
#define make_dir(p) mkdir((p), 0755)
#endif

#ifndef LIBRETRO_SOURCE
#define LIBRETRO_SOURCE "../src/libretro/libretro.c"
#endif
#define gp32_run_frame test_run_frame
#define gp32_get_audio test_get_audio
#define gp32_consume_audio test_consume_audio
#define gp32_get_framebuffer test_get_framebuffer
#include LIBRETRO_SOURCE
#undef gp32_run_frame
#undef gp32_get_audio
#undef gp32_consume_audio
#undef gp32_get_framebuffer

gp32_status_t gp32_run_frame(gp32_t *);
gp32_status_t gp32_get_audio(gp32_t *, gp32_audio_desc_t *);
gp32_status_t gp32_consume_audio(gp32_t *, uint64_t);
gp32_status_t gp32_get_framebuffer(gp32_t *, gp32_framebuffer_desc_t *);

static int failures;
#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s (line %d)\n", message, __LINE__); \
        ++failures; \
    } \
} while (0)

/* Scripted timeline. Frame shapes cycle through resampler rate changes, two
 * kinds of idle frame, and blocks the frontend only partially accepts, so a
 * queue residue survives frame boundaries. The frame counter repeats every
 * third step while the pixels still change: exactly the case where equality of
 * the counter alone cannot prove the presented pixels are unchanged. */
#define WARMUP_FRAMES 12u
#define REPLAY_FRAMES 24u
#define PCM_FRAMES 4096u
#define AUDIO_PER_CALL 700u
#define AUDIO_ALLOWANCE 2500u
#define CARD_BYTES (528u * 128u)

static int16_t input_pcm[PCM_FRAMES * 2u];
static gp32_audio_desc_t scripted_span;
static gp32_framebuffer_desc_t scripted_fb;
static uint32_t video_pixels[GP32_W * GP32_H];
static size_t audio_per_call = AUDIO_PER_CALL;

static uint64_t frame_hash;
static size_t audio_calls, audio_frames, audio_allowance;
static size_t total_video_nulls, total_audio_calls;

static unsigned script_span_frames(unsigned step) {
    static const unsigned shape[6] = {512u, 2048u, 0u, 257u, 0u, 1024u};
    return shape[step % 6u];
}

static uint32_t script_rate(unsigned step) {
    static const uint32_t rates[6] = {44100u, 22050u, 44100u, 32000u, 22050u, 44100u};
    return rates[step % 6u];
}

static uint64_t script_counter(unsigned step) { return 1000u + step - step / 3u; }

static void apply_script(unsigned step) {
    uint32_t color = 0xff000000u | ((step * 2654435761u) & 0x00ffffffu);
    for (size_t i = 0; i < GP32_W * GP32_H; ++i) video_pixels[i] = color;
    scripted_fb = (gp32_framebuffer_desc_t){
        .pixels_rgba8888 = video_pixels,
        .width = GP32_W,
        .height = GP32_H,
        .stride_pixels = GP32_W,
        .frame_counter = script_counter(step)
    };
    unsigned frames = script_span_frames(step);
    for (unsigned j = 0; j < frames; ++j) {
        input_pcm[j * 2u] = (int16_t)((step * 7919u + j * 131u) & 0xffffu);
        input_pcm[j * 2u + 1u] = (int16_t)(0x8000u ^ ((step * 104729u + j * 17u) & 0xffffu));
    }
    scripted_span = (gp32_audio_desc_t){input_pcm, frames, script_rate(step)};
}

gp32_status_t test_run_frame(gp32_t *g) { (void)g; return GP32_OK; }

gp32_status_t test_get_audio(gp32_t *g, gp32_audio_desc_t *out) {
    (void)g;
    *out = scripted_span;
    return GP32_OK;
}

gp32_status_t test_consume_audio(gp32_t *g, uint64_t frames) {
    (void)g;
    if (frames > scripted_span.frame_count) return GP32_ERR_INVALID_ARGUMENT;
    scripted_span.samples_s16_interleaved += (size_t)frames * 2u;
    scripted_span.frame_count -= frames;
    return GP32_OK;
}

gp32_status_t test_get_framebuffer(gp32_t *g, gp32_framebuffer_desc_t *out) {
    (void)g;
    *out = scripted_fb;
    return GP32_OK;
}

static uint64_t hash_bytes(uint64_t h, const void *data, size_t bytes) {
    const uint8_t *p = (const uint8_t *)data;
    while (bytes--) { h ^= *p++; h *= 1099511628211ull; }
    return h;
}

/* Content, not pointer identity: a resent duplicate must hash like the frame
 * it repeats. */
static void record_video(const void *data, unsigned width, unsigned height, size_t pitch) {
    uint8_t kind = data ? 2u : 1u;
    frame_hash = hash_bytes(frame_hash, &kind, 1u);
    if (!data) { ++total_video_nulls; return; }
    uint32_t dims[3] = {width, height, (uint32_t)pitch};
    frame_hash = hash_bytes(frame_hash, dims, sizeof(dims));
    frame_hash = hash_bytes(frame_hash, data, (size_t)width * height * sizeof(uint32_t));
}

/* Only part of each frame's output is accepted, so the queue keeps a residue
 * at the frame boundary; the accepted count is hashed as well. */
static size_t capture_batch(const int16_t *data, size_t frames) {
    size_t accepted = frames < audio_per_call ? frames : audio_per_call;
    if (accepted > audio_allowance) accepted = audio_allowance;
    audio_allowance -= accepted;
    ++audio_calls;
    audio_frames += accepted;
    frame_hash = hash_bytes(frame_hash, &accepted, sizeof(accepted));
    frame_hash = hash_bytes(frame_hash, data, accepted * 2u * sizeof(int16_t));
    return accepted;
}

static bool test_environ(unsigned cmd, void *data) {
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
        *(bool *)data = true;
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *(bool *)data = false;
        return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_VARIABLES:
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
        return true;
    default:
        return false;
    }
}

static void start_core(void) {
    retro_deinit();
    memset(content_path, 0, sizeof(content_path));
    memset(smartmedia_save_path, 0, sizeof(smartmedia_save_path));
    retro_set_environment(test_environ);
    retro_init();
    retro_set_video_refresh(record_video);
    retro_set_audio_sample_batch(capture_batch);
    audio_allowance = AUDIO_ALLOWANCE;
    emu = gp32_create(NULL);
    if (!emu) { fprintf(stderr, "HOST-FATAL: cannot create the emulator\n"); exit(2); }
}

static size_t run_scripted_frame(unsigned step) {
    apply_script(step);
    frame_hash = 1469598103934665603ull;
    audio_allowance = AUDIO_ALLOWANCE;
    audio_calls = audio_frames = 0;
    retro_run();
    total_audio_calls += audio_calls;
    return (size_t)audio_frames;
}

static uint64_t frame_delivery_hash(void) { return frame_hash; }

/* (a) and (b): replay from a captured state must reproduce every delivered
 * frame and the bytes of the state that follows them. */
static void test_rollback_replay(void) {
    start_core();
    for (unsigned step = 0; step < WARMUP_FRAMES; ++step) (void)run_scripted_frame(step);

    size_t size = retro_serialize_size();
    CHECK(size > 0, "retro_serialize_size must be known once content is loaded");
    uint8_t *s0 = (uint8_t *)malloc(size ? size : 1u);
    uint8_t *s1 = (uint8_t *)malloc(size ? size : 1u);
    uint8_t *s2 = (uint8_t *)malloc(size ? size : 1u);
    if (!s0 || !s1 || !s2) { fprintf(stderr, "HOST-FATAL: out of memory\n"); exit(2); }
    CHECK(retro_serialize(s0, size), "capture the state run-ahead restores");

    uint64_t delivered[REPLAY_FRAMES];
    unsigned size_changes = 0;
    for (unsigned i = 0; i < REPLAY_FRAMES; ++i) {
        (void)run_scripted_frame(WARMUP_FRAMES + i);
        delivered[i] = frame_delivery_hash();
        if (retro_serialize_size() != size) ++size_changes;
    }
    CHECK(retro_serialize(s1, size), "capture the state after the run-ahead frames");
    CHECK(retro_unserialize(s0, size), "restore the state run-ahead captured");
    if (retro_serialize_size() != size) ++size_changes;

    unsigned mismatches = 0;
    for (unsigned i = 0; i < REPLAY_FRAMES; ++i) {
        (void)run_scripted_frame(WARMUP_FRAMES + i);
        if (frame_delivery_hash() != delivered[i]) ++mismatches;
        if (retro_serialize_size() != size) ++size_changes;
    }
    CHECK(mismatches == 0, "every replayed frame must deliver identical video and audio");
    CHECK(retro_serialize(s2, size), "capture the replay's final state");
    CHECK(!memcmp(s1, s2, size), "the replayed state must be byte-identical");
    CHECK(retro_unserialize(s1, size) && retro_serialize(s2, size) && !memcmp(s1, s2, size),
          "serialize/unserialize round trip must reproduce identical bytes");
    CHECK(size_changes == 0, "retro_serialize_size must stay constant during a session");
    /* Guard against a timeline that stopped exercising the delivery state. */
    CHECK(total_video_nulls >= 2u, "the timeline must contain duplicate frames");
    CHECK(total_audio_calls >= REPLAY_FRAMES, "the timeline must deliver audio over several calls");

    printf("rollback replay: %s (%u/%u frames differ, %zu-byte state, %zu video dupes, %zu audio calls)\n",
           mismatches ? "FAIL" : "PASS", mismatches, REPLAY_FRAMES, size, total_video_nulls, total_audio_calls);
    free(s0); free(s1); free(s2);
    retro_deinit();
}

/* (c): run-ahead restores a state on every displayed frame, which marks the
 * card dirty again each time. The autosave poll must coalesce that (and exit
 * saves still flush), or a handheld's SD card is written every frame. */
static void test_card_autosave_gate(const char *dir) {
    start_core();
    uint8_t *image = (uint8_t *)malloc(CARD_BYTES);
    if (!image) { fprintf(stderr, "HOST-FATAL: out of memory\n"); exit(2); }
    memset(image, 0xff, CARD_BYTES);
    CHECK(gp32_load_smartmedia_over_base_data(emu, image, CARD_BYTES) == GP32_OK,
          "mount a synthetic SmartMedia card");
    char save_path[4096];
    join_path(save_path, sizeof(save_path), dir, "runahead.gp32.sav");
    remove(save_path);
    snprintf(smartmedia_save_path, sizeof(smartmedia_save_path), "%s", save_path);

    size_t size = retro_serialize_size();
    uint8_t *state = (uint8_t *)malloc(size ? size : 1u);
    if (!state) { fprintf(stderr, "HOST-FATAL: out of memory\n"); exit(2); }
    /* RetroArch single-instance run-ahead: sample the frame, capture a state,
     * run hidden frames from it, then restore it. */
    int cycles_ok = 1;
    for (unsigned i = 0; i < REPLAY_FRAMES && cycles_ok; ++i) {
        (void)run_scripted_frame(i);
        cycles_ok = retro_serialize(state, size) && run_scripted_frame(i) >= 0 &&
                    retro_unserialize(state, size);
        (void)run_scripted_frame(i);
    }
    CHECK(cycles_ok, "run-ahead cycles (run, capture, run, restore) must all succeed");
    CHECK(!file_exists(save_path), "repeated rollbacks must not write the card save file");
    /* Positive control: this same dirtied card still flushes on an explicit
     * save, so the file's absence above comes from the coalescing gate. */
    CHECK(gp32_save_card_progress(emu, save_path) == GP32_OK,
          "explicit card progress save must succeed");
    CHECK(file_exists(save_path), "explicit card progress save must write the file");
    remove(save_path);

    free(state);
    free(image);
    retro_deinit();
}

int main(int argc, char **argv) {
    const char *root = argc > 1 ? argv[1] : "libretro_runahead_tmp";
    make_dir(root);
    test_rollback_replay();
    test_card_autosave_gate(root);
    printf("Run-ahead replay: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
