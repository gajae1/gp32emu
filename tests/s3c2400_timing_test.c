/* Replay changing PLL/LCD timing, frame boundaries and state restore through
 * the public peripheral API. Compare the trace hash against the prior core. */
#include "s3c2400.h"
#include <inttypes.h>

static uint64_t hash = UINT64_C(14695981039346656037);
static void record(uint32_t value) {
    for (unsigned i = 0; i < 4u; ++i) {
        hash ^= (value >> (8u * i)) & 255u;
        hash *= UINT64_C(1099511628211);
    }
}

static void observe(s3c2400_t *s, uint32_t cycles) {
    s3c2400_tick(s, cycles);
    record(s3c2400_read32(s, 0x14a00000u));
    record(s3c2400_read32(s, 0x14a00000u));
    uint64_t frames;
    (void)s3c2400_framebuffer(s, NULL, NULL, NULL, &frames);
    record((uint32_t)frames);
    record(s3c2400_run_clock_hz(s));
}

int main(void) {
    static const uint32_t clocks[] = {0x0005c080u, 0x0007d042u, 0x00048032u};
    static const uint32_t steps[] = {1u, 1023u, 32768u, 999999u, 7u, 800000u};
    s3c2400_t *s = s3c2400_create(8u * 1024u * 1024u);
    FILE *state = tmpfile();
    if (!s || !state) return 2;
    for (unsigned k = 0; k < 12u; ++k) {
        s3c2400_write32(s, 0x14800004u, clocks[k % 3u]);
        s3c2400_write32(s, 0x14800014u, k % 4u);
        s3c2400_write32(s, 0x14a00004u, ((k & 1u) ? 319u : 239u) << 14);
        s3c2400_write32(s, 0x14a00000u, (12u << 1) | 1u);
        for (unsigned i = 0; i < sizeof(steps) / sizeof(steps[0]); ++i) observe(s, steps[i]);
        rewind(state);
        if (!s3c2400_state_save(s, state)) return 2;
        s3c2400_write32(s, 0x14800004u, clocks[(k + 1u) % 3u]);
        s3c2400_write32(s, 0x14a00004u, 17u << 14);
        observe(s, 123456u);
        rewind(state);
        if (!s3c2400_state_load(s, state)) return 2;
        observe(s, 1u);
        if ((k % 3u) == 0u) { s3c2400_reset(s); observe(s, 31u); }
    }
    fclose(state);
    printf("lcd_timing_trace=%016" PRIx64 "\n", hash);
    /* Recorded by running this replay against the previous core, before the
     * derived timing cache was introduced. */
    if (hash != UINT64_C(0x47779e5037cd7b27)) { s3c2400_destroy(s); return 1; }
    /* IIS byte-DMA pacing. Each 8-bit DMA unit writes IISFIF once, and the
     * register write pushes one 16-bit FIFO entry, so a stereo frame consumes
     * two units. With an 8-unit auto-reloading block the terminal-count IRQ
     * must land on the fourth frame period (500 cycles each at 96 kHz/48 MHz),
     * not the second. */
    s3c2400_reset(s);
    s3c2400_write32(s, 0x14600040u, 0x0c000000u); /* DISRC2: RAM, incrementing */
    s3c2400_write32(s, 0x14600044u, 0x35508010u); /* DIDST2: IISFIF, fixed */
    s3c2400_write32(s, 0x14600048u, 0x10800008u); /* DCON2: IRQ|HW req|IIS, byte, tc=8 */
    s3c2400_write32(s, 0x14600058u, 2u);          /* DMASKTRIG2: channel on */
    s3c2400_write32(s, 0x15508000u, 1u);          /* IISCON: start */
    unsigned dma2_irqs = 0;
    for (unsigned i = 0; i < 4u; ++i) {
        s3c2400_tick(s, 500u);
        if (s3c2400_read32(s, 0x14400000u) & 0x00080000u) {
            ++dma2_irqs;
            s3c2400_write32(s, 0x14400000u, 0x00080000u);
        }
    }
    uint64_t iis_frames = 0;
    (void)s3c2400_audio_samples(s, &iis_frames, NULL);
    s3c2400_destroy(s);
    if (iis_frames != 4u || dma2_irqs != 1u) {
        fprintf(stderr, "FAIL: IIS byte DMA frames=%" PRIu64 " dma2_irqs=%u (want 4/1)\n",
                iis_frames, dma2_irqs);
        return 1;
    }
    return 0;
}
