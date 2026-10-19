/* A caller's composite input can share card storage that the guest commit
 * rewrites. Delivery PCM and video must own their pending bytes until their
 * later commit, and failure to reserve them must preserve the entire state. */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include "../src/gp32.c"
#include "../src/s3c2400.c"
#include "../src/smartmedia.c"

static unsigned delivery_fail_at, delivery_calls;
static int delivery_trace;
static void *delivery_owned[8];
static void *owned_delivery_malloc(size_t bytes) {
    if (delivery_trace && ++delivery_calls == delivery_fail_at) return NULL;
    void *p = malloc(bytes);
    if (p && delivery_trace) {
        unsigned i;
        for (i = 0; i < 8u && delivery_owned[i]; ++i) {}
        if (i == 8u) abort();
        delivery_owned[i] = p;
    }
    return p;
}
static void owned_delivery_free(void *p) {
    for (unsigned i = 0; i < 8u; ++i)
        if (delivery_owned[i] == p) delivery_owned[i] = NULL;
    free(p);
}
#define malloc owned_delivery_malloc
#define free owned_delivery_free
#include "../src/libretro/libretro.c"
#undef malloc
#undef free

static unsigned checks, failures;
#define CHECK(c, msg) do { ++checks; if (!(c)) { \
    fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); ++failures; \
} } while (0)
static int any_owned(void) {
    for (unsigned i = 0; i < 8u; ++i) if (delivery_owned[i]) return 1;
    return 0;
}

int main(void) {
    const size_t card_bytes = 32768u * 528u;
    retro_init();
    emu = gp32_create(NULL);
    if (!emu) return 2;
    uint8_t *card = malloc(card_bytes);
    if (!card) return 2;
    memset(card, 0xa5, card_bytes);
    CHECK(gp32_load_smartmedia_data(emu, card, card_bytes) == GP32_OK, "mount synthetic card");
    free(card);
    audio_resample_cap = audio_pending_frames = 16u;
    audio_resample_buf = malloc(audio_resample_cap * 4u);
    if (!audio_resample_buf || !effects_ready || !effects.prev_raw || !effects.prev_lcd) return 2;
    int16_t wanted_audio[32];
    for (unsigned i = 0; i < 32u; ++i) wanted_audio[i] = audio_resample_buf[i] = (int16_t)(100 + i * 37u);
    effects.have_prev_raw = effects.have_prev_lcd = 1;
    for (size_t i = 0; i < GP32_VIDEO_EFFECTS_PIXELS; ++i) {
        effects.prev_raw[i] = 0x00123456u ^ (uint32_t)i;
        effects.prev_lcd[i] = 0x00765432u ^ (uint32_t)(i * 7u);
    }
    last_video_frame = 42u; have_last_video = 1; last_video_ptr = effect_rgb;
    size_t size = retro_serialize_size();
    uint8_t *saved = malloc(size), *expected = malloc(size), *got = malloc(size);
    if (!saved || !expected || !got || size > card_bytes) return 2;
    CHECK(retro_serialize(saved, size), "capture composite state");
    CHECK(retro_unserialize(saved, size), "external-buffer control restore");
    CHECK(retro_serialize(expected, size), "capture normalized control state");

    smc_t *smc = emu->soc->smc;
    memcpy(smc->data, saved, size);
    smc_diff_refresh(smc, 0u, (uint32_t)((size + smc->page_total_size - 1u) / smc->page_total_size));
    unsigned changed_pages = smc->diff_pages;
    CHECK(retro_unserialize(smc->data, size), "restore composite from live card storage");
    CHECK(audio_pending_frames == 16u && !memcmp(wanted_audio, audio_resample_buf, sizeof(wanted_audio)),
          "delivery PCM survives card restoration over its input");
    int raw_ok = 1, lcd_ok = 1;
    for (size_t i = 0; i < GP32_VIDEO_EFFECTS_PIXELS; ++i) {
        raw_ok &= effects.prev_raw[i] == (0x00123456u ^ (uint32_t)i);
        lcd_ok &= effects.prev_lcd[i] == (0x00765432u ^ (uint32_t)(i * 7u));
    }
    CHECK(raw_ok && effects.have_prev_raw, "raw frame history survives card restoration");
    CHECK(lcd_ok && effects.have_prev_lcd, "LCD frame history survives card restoration");
    CHECK(retro_serialize(got, size) && !memcmp(expected, got, size),
          "shared-input restore equals external-buffer restore byte for byte");

    /* Queue growth and private payload ownership are two distinct fallible
     * reservations. Reject each one before committing either guest or tail. */
    for (unsigned fail = 1; fail <= 2u; ++fail) {
        CHECK(retro_unserialize(saved, size), "reset allocation-failure fixture");
        CHECK(gp32_set_buttons(emu, GP32_BUTTON_A) == GP32_OK, "distinct live guest input");
        audio_resample_cap = audio_pending_frames = 8u;
        audio_resample_buf[0] = -1234;
        effects.prev_raw[0] ^= 0xabcdefu;
        effects.prev_lcd[0] ^= 0xfedcbau;
        CHECK(retro_serialize(got, size), "capture complete live state before reservation failure");
        uint8_t *before = malloc(size);
        if (!before) return 2;
        memcpy(before, got, size);
        delivery_calls = 0; delivery_fail_at = fail; delivery_trace = 1;
        int ok = retro_unserialize(saved, size);
        delivery_trace = 0;
        CHECK(!ok, "each delivery reservation failure rejects the whole load");
        CHECK(!any_owned(), "rejection frees every private delivery reservation");
        CHECK(retro_serialize(got, size) && !memcmp(before, got, size),
              "delivery reservation failure preserves guest, queued PCM and both histories");
        CHECK(retro_unserialize(saved, size), "valid retry after reservation failure");
        CHECK(retro_serialize(got, size) && !memcmp(expected, got, size), "valid retry restores exact composite state");
        free(before);
    }
    /* With no PCM or histories present, there is no payload to reserve. */
    audio_pending_frames = 0;
    effects.have_prev_raw = effects.have_prev_lcd = 0;
    CHECK(retro_serialize(saved, size), "capture empty delivery payload");
    delivery_calls = 0; delivery_fail_at = 1; delivery_trace = 1;
    int ok = retro_unserialize(saved, size);
    delivery_trace = 0;
    CHECK(ok && !delivery_calls, "empty delivery payload needs no allocation");
    free(saved); free(expected); free(got);
    gp32_destroy(emu); emu = NULL; retro_deinit();
    CHECK(!any_owned(), "all delivery-owned storage released after teardown");
    printf("libretro_state_ownership: %u checks, %u failures; state_bytes=%zu changed_card_pages=%u\n",
           checks, failures, size, changed_pages);
    return failures ? 1 : 0;
}
