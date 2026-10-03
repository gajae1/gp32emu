/* Exercise the real U8 PCM conversion sites, including private HLE mixers.
 * Run with -fsanitize=shift to catch signed shifts below the U8 midpoint. */
#include "../src/gp32.c"

static const uint8_t input[] = {0, 1, 127, 128, 129, 254, 255};
static const int16_t expected[] = {-32768, -32512, -256, 0, 256, 32256, 32512};

static gp32_t *sdk_stream_fixture(uint32_t half_samples) {
    gp32_t *g = gp32_create(NULL);
    if (!g) return NULL;
    const uint32_t mixer = GP32_RAM_BASE + 0x4000u;
    const uint32_t cursor = mixer - 0x100u;
    const uint32_t buffer = GP32_RAM_BASE + 0x8000u;
    const uint32_t object = GP32_RAM_BASE + 0xa000u;
    const uint32_t fill = GP32_RAM_BASE + 0xc000u;
    const uint32_t code[] = {
        0xe5903000u, 0xe2833001u, 0xe5803000u, 0xe5813000u, 0xe12fff1eu,
    }; /* Increment object word, copy it into the released half, BX lr. */
    g->direct_fxe_mode = 1;
    direct_install_stubs(g);
    gp32_set_jit(g, 1);
    s3c2400_write32(g->soc, 0x14800004u, 0x3000u);
    s3c2400_write32(g->soc, 0x14800014u, 0u);
    g->direct_hle_sdk_sndmixer_addr = mixer;
    g->direct_hle_sdk_rate = 44100u;
    s3c2400_write32(g->soc, mixer, buffer);
    s3c2400_write32(g->soc, mixer + 4u, buffer);
    s3c2400_write32(g->soc, mixer + 8u, half_samples * 2u);
    s3c2400_write32(g->soc, mixer + 12u, half_samples * 2u);
    s3c2400_write32(g->soc, mixer + 16u, 1u);
    s3c2400_write32(g->soc, cursor, mixer + 4u);
    s3c2400_write32(g->soc, cursor + 8u, 1u); /* two bytes per sample */
    s3c2400_write32(g->soc, cursor + 12u, buffer);
    s3c2400_write32(g->soc, cursor + 24u, half_samples);
    s3c2400_write32(g->soc, cursor + 28u, object);
    s3c2400_write32(g->soc, cursor + 32u, fill);
    s3c2400_write32(g->soc, object, 0x80008000u);
    for (unsigned i = 0; i < half_samples * 2u; ++i) s3c2400_write16(g->soc, buffer + i * 2u, 0x8000u);
    for (unsigned i = 0; i < GP32_ARRAY_COUNT(code); ++i)
        s3c2400_write32(g->soc, fill + i * 4u, code[i]);
    return g;
}

static int check_sdk_refill_slicing(uint32_t half_samples) {
    gp32_t *batch = sdk_stream_fixture(half_samples), *split = sdk_stream_fixture(half_samples);
    if (!batch || !split) { gp32_destroy(batch); gp32_destroy(split); return 0; }
    uint32_t clock = direct_run_clock_hz(batch);
    direct_sdk_sound_tick(batch, clock / 100u);
    for (unsigned i = 0; i < 100u; ++i) direct_sdk_sound_tick(split, clock / 10000u);
    uint64_t a_frames = 0, b_frames = 0;
    uint32_t a_rate = 0, b_rate = 0;
    const int16_t *a = s3c2400_audio_samples(batch->soc, &a_frames, &a_rate);
    const int16_t *b = s3c2400_audio_samples(split->soc, &b_frames, &b_rate);
    int ok = clock == 66000000u && a && b && a_frames == 441u && b_frames == a_frames &&
        a_rate == 44100u && b_rate == a_rate &&
        !memcmp(a, b, (size_t)a_frames * 2u * sizeof(*a)) &&
        a[half_samples * 4u] == 1 && a[half_samples * 6u] == 2 &&
        s3c2400_debug_read32(batch->soc, GP32_RAM_BASE + 0xa000u) == 0x80008000u + 441u / half_samples &&
        s3c2400_debug_read32(split->soc, GP32_RAM_BASE + 0xa000u) == 0x80008000u + 441u / half_samples;
    if (!ok) fprintf(stderr, "FAIL: SDK refill must follow buffer progress independent of cycle slicing\n");
    gp32_destroy(batch);
    gp32_destroy(split);
    return ok;
}

static int check_hle_pcm_clock_domains(void) {
    /* Equal wall time must yield the same stream under different CPU/bus
     * ratios, including the high-PLL effective instruction-budget clock. */
    const uint32_t pll[] = {0x3000u, 0x3000u, 0xe000u};
    const uint32_t div[] = {0u, 2u, 2u};
    const uint32_t run_hz[] = {66000000u, 33000000u, 48000000u};
    uint8_t sef[8u + 512u] = {0};
    for (unsigned i = 8u; i < sizeof(sef); ++i) sef[i] = (uint8_t)i;
    fpk_asset_t asset = {0};
    asset.data = sef;
    asset.size = sizeof(sef);
    int16_t reference[441u * 2u];
    for (unsigned k = 0; k < GP32_ARRAY_COUNT(pll); ++k) {
        gp32_t *g = gp32_create(NULL);
        if (!g) return 0;
        g->direct_fxe_mode = 1;
        s3c2400_write32(g->soc, 0x14800004u, pll[k]);
        s3c2400_write32(g->soc, 0x14800014u, div[k]);
        g->direct_hle_audio_asset = &asset;
        g->direct_hle_audio_size = 512u;
        g->direct_hle_audio_rate = 22050u;
        const uint32_t source = GP32_RAM_BASE + 0x1000u;
        s3c2400_write32(g->soc, source, 0x40ff8000u);
        g->direct_hle_pcm_ch[0].active = 1u;
        g->direct_hle_pcm_ch[0].src_addr = source;
        g->direct_hle_pcm_ch[0].size_bytes = 4u;
        g->direct_hle_pcm_ch[0].bits = 8u;
        g->direct_hle_pcm_ch[0].rate = 11025u;
        g->direct_hle_pcm_ch[0].repeat = 1u;
        uint32_t budget = direct_run_clock_hz(g) / 100u;
        g->direct_vblank_wait_cycles = budget;
        gp32_status_t status = gp32_run_cycles(g, budget);
        uint64_t frames = 0;
        uint32_t rate = 0;
        const int16_t *pcm = s3c2400_audio_samples(g->soc, &frames, &rate);
        int ok = status == GP32_OK && direct_run_clock_hz(g) == run_hz[k] &&
            pcm && frames == 441u && rate == 44100u;
        if (ok && k == 0u) memcpy(reference, pcm, sizeof(reference));
        else if (ok) ok = !memcmp(reference, pcm, sizeof(reference));
        if (!ok) fprintf(stderr, "FAIL: HLE PCM clock ratio %u: frames=%llu rate=%u\n",
                         k, (unsigned long long)frames, rate);
        g->direct_hle_audio_asset = NULL;
        gp32_destroy(g);
        if (!ok) return 0;
    }
    return 1;
}

int main(void) {
    if (!check_hle_pcm_clock_domains()) return 1;
    if (!check_sdk_refill_slicing(32u) || !check_sdk_refill_slicing(64u) ||
        !check_sdk_refill_slicing(70u)) return 1;
    gp32_t *g = gp32_create(NULL);
    if (!g) return 2;
    s3c2400_audio_clear(g->soc);
    s3c2400_audio_append_u8_mono(g->soc, input, GP32_ARRAY_COUNT(input), 11025u);
    uint64_t frames = 0;
    uint32_t rate = 0;
    const int16_t *pcm = s3c2400_audio_samples(g->soc, &frames, &rate);
    if (!pcm || frames != GP32_ARRAY_COUNT(input) || rate != 11025u) return 1;
    for (size_t i = 0; i < GP32_ARRAY_COUNT(input); ++i)
        if (pcm[i * 2u] != expected[i] || pcm[i * 2u + 1u] != expected[i]) return 1;

    uint8_t sef[8u + sizeof(input)] = {0};
    memcpy(sef + 8u, input, sizeof(input));
    fpk_asset_t asset = {0};
    asset.data = sef;
    asset.size = sizeof(sef);
    g->direct_hle_audio_asset = &asset;
    g->direct_hle_audio_size = sizeof(input);
    g->direct_hle_audio_rate = 11025u;
    for (size_t i = 0; i < GP32_ARRAY_COUNT(input); ++i) {
        int32_t left = 0, right = 0;
        if (!direct_hle_sef_sample(g, 11025u, &left, &right) ||
            left != expected[i] || right != expected[i]) return 1;
    }
    g->direct_hle_audio_asset = NULL;

    for (unsigned stereo = 0; stereo <= 1u; ++stereo) {
        for (size_t i = 0; i < GP32_ARRAY_COUNT(input); ++i) {
            s3c2400_write8(g->soc, GP32_RAM_BASE + 0x1000u + (uint32_t)i * (1u + stereo), input[i]);
            if (stereo)
                s3c2400_write8(g->soc, GP32_RAM_BASE + 0x1001u + (uint32_t)i * 2u, input[GP32_ARRAY_COUNT(input) - 1u - i]);
        }
        memset(&g->direct_hle_pcm_ch[0], 0, sizeof(g->direct_hle_pcm_ch[0]));
        g->direct_hle_pcm_ch[0].active = 1u;
        g->direct_hle_pcm_ch[0].src_addr = GP32_RAM_BASE + 0x1000u;
        g->direct_hle_pcm_ch[0].size_bytes = sizeof(input) * (1u + stereo);
        g->direct_hle_pcm_ch[0].bits = 8u;
        g->direct_hle_pcm_ch[0].stereo = stereo;
        g->direct_hle_pcm_ch[0].rate = 11025u;
        for (size_t i = 0; i < GP32_ARRAY_COUNT(input); ++i) {
            int32_t left = 0, right = 0;
            int16_t want_right = expected[stereo ? GP32_ARRAY_COUNT(input) - 1u - i : i];
            if (!direct_pcm_channel_sample(g, 0u, 11025u, &left, &right) ||
                left != expected[i] || right != want_right) return 1;
        }
    }
    gp32_destroy(g);
    puts("PASS: U8 PCM endpoints, midpoint and channel polarity (SoC/SEF/mono/stereo)");
    return 0;
}
