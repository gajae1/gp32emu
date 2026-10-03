/* Exercise the real U8 PCM conversion sites, including private HLE mixers.
 * Run with -fsanitize=shift to catch signed shifts below the U8 midpoint. */
#include "../src/gp32.c"

static const uint8_t input[] = {0, 1, 127, 128, 129, 254, 255};
static const int16_t expected[] = {-32768, -32512, -256, 0, 256, 32256, 32512};

int main(void) {
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
