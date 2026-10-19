/* Exercise the portable iOS boundary with a synthetic core, without UIKit,
 * firmware, game data or a physical audio device. */
#include "../ios/bridge/gp32_ios.c"

static int failures;
#define CHECK(c, message) do { if (!(c)) { \
    fprintf(stderr, "FAIL: %s\n", message); ++failures; } } while (0)

static void queue_tone(gp32_ios_emulator_t *e, int16_t level) {
    int16_t samples[512u * 2u];
    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i)
        samples[i] = level;
    gp32_audio_desc_t span = {0};
    span.samples_s16_interleaved = samples;
    span.frame_count = 512u;
    span.sample_rate_hz = GP32_IOS_AUDIO_RATE;
    push_audio_span(e, &span);
}

static void audio_transition(unsigned action, const char *state_path) {
    gp32_ios_audio_sink_t sink;
    gp32_ios_audio_sink_init(&sink);
    gp32_ios_emulator_t *e = gp32_ios_emulator_create();
    CHECK(e != NULL, "allocate bridge");
    if (!e) return;
    e->core = gp32_create(NULL);
    CHECK(e->core != NULL, "create synthetic core");
    if (!e->core) { gp32_ios_emulator_destroy(e); return; }
    gp32_set_jit(e->core, 0);
    e->sink = &sink;
    e->have_game = 1;
    CHECK(copy_text(e->state_path, sizeof(e->state_path), state_path), "state path");
    if (action == 2u) CHECK(gp32_ios_emulator_save_state(e), "save synthetic state");

    queue_tone(e, 16000);
    gp32_ios_emulator_set_buttons(e, GP32_BUTTON_A);
    CHECK(!(gp32_debug_read32(e->core, 0x1560000cu) & 0x4000u), "A reaches the core");
    if (action == 0u) gp32_ios_emulator_clear_audio(e);
    else if (action == 1u) {
        CHECK(gp32_ios_emulator_reset(e), "reset succeeds");
        CHECK(gp32_debug_read32(e->core, 0x1560000cu) & 0x4000u,
              "reset releases input in the core as well as the wrapper");
    } else CHECK(gp32_ios_emulator_load_state(e), "restore synthetic state");

    int16_t output[512u * 2u];
    CHECK(gp32_ios_audio_sink_read(&sink, output, 1u) == 0u &&
          output[0] == 0 && output[1] == 0, "transition discards pre-transition PCM");
    queue_tone(e, -16000);
    size_t got = gp32_ios_audio_sink_read(&sink, output, 512u);
    CHECK(got > 256u, "new audio reaches the consumer");
    CHECK(abs(output[0]) < 2000 && abs(output[1]) < 2000,
          "resumed audio ramps from silence, not a stale sample");
    if (got > 256u)
        CHECK(output[2u * (got - 1u)] < -15000, "ramp settles to the new signal");
    gp32_ios_emulator_destroy(e);
    if (action == 2u) CHECK(remove(state_path) == 0, "remove synthetic state");
}

static void audio_underrun_recovery(void) {
    gp32_ios_audio_sink_t sink;
    gp32_ios_audio_sink_init(&sink);
    gp32_ios_emulator_t *e = gp32_ios_emulator_create();
    CHECK(e != NULL, "allocate underrun bridge");
    if (!e) return;
    e->sink = &sink;
    int16_t output[512u * 2u];
    queue_tone(e, 16000);
    (void)gp32_ios_audio_sink_read(&sink, output, 512u);
    CHECK(gp32_ios_audio_sink_read(&sink, output, 1u) == 0u,
          "consumer exhausts the queue before recovery");
    queue_tone(e, -16000);
    size_t got = gp32_ios_audio_sink_read(&sink, output, 512u);
    CHECK(got > 256u, "underrun recovery supplies fresh PCM");
    CHECK(abs(output[0]) < 2000 && abs(output[1]) < 2000,
          "first recovered span ramps from silence immediately");
    if (got > 256u)
        CHECK(output[2u * (got - 1u)] < -15000, "recovery ramp settles");
    gp32_ios_emulator_destroy(e);
}

/* Slot paths are supplied per operation. A rejected path must not redirect the
 * legacy state path or disturb the live core behind it. */
static void explicit_state_paths(const char *base) {
    char legacy_path[GP32_IOS_PATH_MAX];
    char slot_path[GP32_IOS_PATH_MAX];
    char missing_path[GP32_IOS_PATH_MAX];
    char oversized_path[GP32_IOS_PATH_MAX + 1u];
    snprintf(legacy_path, sizeof(legacy_path), "%s.legacy.gp32st", base);
    snprintf(slot_path, sizeof(slot_path), "%s.slot02.gp32st", base);
    snprintf(missing_path, sizeof(missing_path), "%s.missing.gp32st", base);
    remove(slot_path);
    remove(missing_path);
    memset(oversized_path, 'x', sizeof(oversized_path) - 1u);
    oversized_path[sizeof(oversized_path) - 1u] = '\0';

    gp32_ios_audio_sink_t sink;
    gp32_ios_audio_sink_init(&sink);
    gp32_ios_emulator_t *e = gp32_ios_emulator_create();
    CHECK(e != NULL, "allocate explicit-path bridge");
    if (!e) return;
    e->core = gp32_create(NULL);
    CHECK(e->core != NULL, "create explicit-path core");
    if (!e->core) { gp32_ios_emulator_destroy(e); return; }
    gp32_set_jit(e->core, 0);
    e->sink = &sink;
    e->have_game = 1;
    CHECK(copy_text(e->state_path, sizeof(e->state_path), legacy_path),
          "set legacy state path");
    gp32_ios_emulator_set_buttons(e, GP32_BUTTON_A);
    gp32_t *session = e->core;
    uint32_t buttons_before = gp32_debug_read32(e->core, 0x1560000cu);

    CHECK(!gp32_ios_emulator_load_state_at_path(e, oversized_path),
          "oversized explicit state path is rejected");
    CHECK(e->core == session &&
          gp32_debug_read32(e->core, 0x1560000cu) == buttons_before,
          "rejected oversized path keeps the live core");
    CHECK(strcmp(e->state_path, legacy_path) == 0,
          "rejected oversized path keeps the legacy path");

    CHECK(!gp32_ios_emulator_load_state_at_path(e, missing_path),
          "missing explicit state path is rejected");
    CHECK(e->core == session &&
          gp32_debug_read32(e->core, 0x1560000cu) == buttons_before,
          "failed explicit load keeps the live core");
    CHECK(strcmp(e->state_path, legacy_path) == 0,
          "failed explicit load keeps the legacy path");

    CHECK(gp32_ios_emulator_save_state_at_path(e, slot_path),
          "save through an explicit slot path");
    CHECK(strcmp(e->state_path, legacy_path) == 0,
          "successful explicit save keeps the legacy path");
    CHECK(gp32_ios_emulator_load_state_at_path(e, slot_path),
          "load through the same explicit slot path");
    FILE *saved = fopen(slot_path, "rb");
    CHECK(saved != NULL, "explicit slot save exists");
    if (saved) {
        CHECK(fseek(saved, 0, SEEK_END) == 0 && ftell(saved) > 0,
              "explicit slot save is non-empty");
        fclose(saved);
    }
    CHECK(remove(slot_path) == 0, "remove explicit slot state");
    gp32_ios_emulator_destroy(e);
}

/* A regular file in place of the save directory fails on every host, even
 * when tests run as root. Keep the modified card alive until saving can retry. */
static void failed_card_unload(const char *path) {
    size_t bytes = 528u * 8192u;
    uint8_t *image = malloc(bytes);
    gp32_ios_emulator_t *e = gp32_ios_emulator_create();
    CHECK(image && e, "allocate card fixture");
    if (!image || !e) { free(image); gp32_ios_emulator_destroy(e); return; }
    memset(image, 0xff, bytes);
    e->core = gp32_create(NULL);
    CHECK(e->core != NULL, "create card core");
    if (!e->core) { free(image); gp32_ios_emulator_destroy(e); return; }
    CHECK(gp32_load_smartmedia_data(e->core, image, bytes) == GP32_OK, "mount base card");
    image[528u * 3u + 7u] = 0x42u;
    CHECK(gp32_load_smartmedia_over_base_data(e->core, image, bytes) == GP32_OK,
          "restore changed card over immutable base");
    e->have_game = 1;
    FILE *blocker = fopen(path, "wb");
    CHECK(blocker != NULL, "create save-directory obstruction");
    if (!blocker) { free(image); gp32_ios_emulator_destroy(e); return; }
    fclose(blocker);
    snprintf(e->card_save_path, sizeof(e->card_save_path), "%s/progress.sav", path);
    CHECK(gp32_save_card_progress(e->core, e->card_save_path) != GP32_OK,
          "fixture really fails persistence");
    gp32_t *original = e->core;
    CHECK(!gp32_ios_emulator_unload(e), "unload reports failed persistence");
    CHECK(e->core == original && gp32_ios_emulator_has_game(e),
          "failed unload retains the live game and unsaved card");
    CHECK(gp32_ios_emulator_error(e)[0] != '\0', "failed unload exposes save error");
    CHECK(remove(path) == 0, "remove save-directory obstruction");
    if (e->core) {
        CHECK(copy_text(e->card_save_path, sizeof(e->card_save_path), path), "retry path");
        CHECK(gp32_ios_emulator_unload(e), "unload retry succeeds");
        CHECK(!gp32_ios_emulator_has_game(e), "successful retry releases game");
        gp32_t *restored = gp32_create(NULL);
        CHECK(restored != NULL, "create save verification core");
        if (restored) {
            memset(image, 0xff, bytes);
            CHECK(gp32_load_smartmedia_data(restored, image, bytes) == GP32_OK &&
                  gp32_load_card_progress(restored, path) == GP32_OK,
                  "retry creates readable card progress");
            CHECK(remove(path) == 0, "remove verified progress file");
            CHECK(gp32_save_smartmedia(restored, path) == GP32_OK, "dump restored card");
            FILE *card = fopen(path, "rb");
            CHECK(card != NULL, "open restored card");
            if (card) {
                CHECK(fseek(card, 528u * 3u + 7u, SEEK_SET) == 0 && fgetc(card) == 0x42u,
                      "retry preserves actual unsaved NAND byte");
                fclose(card);
            }
            gp32_destroy(restored);
            CHECK(remove(path) == 0, "remove restored card fixture");
        }
    }
    gp32_ios_emulator_destroy(e);
    free(image);
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    for (unsigned action = 0; action < 3u; ++action)
        audio_transition(action, argv[1]);
    audio_underrun_recovery();
    explicit_state_paths(argv[1]);
    failed_card_unload(argv[1]);
    if (failures) return 1;
    puts("PASS: iOS audio continuity, reset input release, explicit state paths and failed-save preservation");
    return 0;
}
