/* Muted DMA must retain raw FIFO samples and bus/DMA side effects. Compare
 * the batched path with actual individual IIS FIFO writes, then unmute. */
#include "../src/s3c2400.c"

static s3c2400_t *machine(unsigned idx) {
    s3c2400_t *s = s3c2400_create(4096u);
    if (!s) return NULL;
    for (unsigned i = 0; i < 1024u; ++i)
        gp32_st32le(s->ram + i * 4u, 0x80ff1357u + i * 0x10203u);
    audio_append_stereo(s, 111, -222, 44100u);
    s->iis_fifo[0] = 0x1357u;
    s->iis_fifo[1] = 0x2468u;
    s->iis_fifo_index = idx;
    s3c2400_audio_set_volume(s, 63u);
    return s;
}

static int same_audio(s3c2400_t *a, s3c2400_t *b) {
    return a->iis_fifo_index == b->iis_fifo_index &&
        !memcmp(a->iis_fifo, b->iis_fifo, sizeof(a->iis_fifo)) &&
        a->audio_frames == b->audio_frames &&
        a->audio_sample_rate_hz == b->audio_sample_rate_hz &&
        !memcmp(a->audio, b->audio, (size_t)a->audio_frames * 4u);
}

static int one(unsigned dsz, unsigned idx, unsigned fixed, unsigned n, uint32_t src) {
    s3c2400_t *a = machine(idx), *b = machine(idx);
    if (!a || !b) {
        s3c2400_destroy(a); s3c2400_destroy(b);
        return 0;
    }
    uint32_t *r = a->dma + 16;
    unsigned step = dsz == 1u ? 2u : 4u;
    unsigned partial = n & 1u;
    r[0] = src | (fixed << 29);
    r[1] = 0x35508010u;
    r[2] = 0x10400000u | (dsz << 20) | (n + partial);
    r[3] = n + partial;
    r[4] = src;
    r[5] = 0x15508010u;
    r[6] = 2u;
    uint32_t retired = dma_iis_fast_trigger_count(a, r, n);
    for (unsigned i = 0; i < n; ++i) {
        uint32_t at = src + (fixed ? 0u : i * step);
        if (dsz == 1u) iis_fifo_write16(b, s3c2400_read16(b, at));
        else {
            uint32_t v = s3c2400_read32(b, at);
            iis_fifo_write16(b, (uint16_t)(v >> 16));
            iis_fifo_write16(b, (uint16_t)v);
        }
    }
    int ok = retired == n && same_audio(a, b) &&
        r[3] == partial && r[4] == src + (fixed ? 0u : n * step) &&
        r[5] == 0x15508010u && ((r[6] & 2u) != 0u) == (partial != 0u) &&
        ((a->irq[0] & (1u << 19)) != 0u) == (partial == 0u);
    /* A muted partial pair must retain its original sample when sound returns.
     * Older queued PCM must also survive the bulk zero-fill unchanged. */
    s3c2400_audio_set_volume(a, 0u);
    s3c2400_audio_set_volume(b, 0u);
    iis_fifo_write16(a, 0x7fffu); iis_fifo_write16(b, 0x7fffu);
    iis_fifo_write16(a, 0x8000u); iis_fifo_write16(b, 0x8000u);
    ok &= same_audio(a, b) && a->audio[0] == 111 && a->audio[1] == -222;
    if (!ok) fprintf(stderr, "FAIL dsz=%u idx=%u fixed=%u count=%u src=%08x\n",
                     dsz, idx, fixed, n, src);
    s3c2400_destroy(a); s3c2400_destroy(b);
    return ok;
}

int main(void) {
    unsigned cases = 0u, failures = 0u;
    for (unsigned dsz = 1u; dsz <= 2u; ++dsz)
        for (unsigned idx = 0u; idx <= 1u; ++idx) {
            for (unsigned fixed = 0u; fixed <= 1u; ++fixed)
                for (unsigned n = 1u; n <= 5u; ++n) {
                    ++cases; failures += !one(dsz, idx, fixed, n, 0x0c000100u);
                }
            ++cases; failures += !one(dsz, idx, 0u, 3u, 0x0c000ffeu);
            ++cases; failures += !one(dsz, idx, 1u, 3u, 0x1560000cu);
        }
    printf("cases=%u failures=%u\n", cases, failures);
    return failures ? 1 : 0;
}
