/* Replay changing PLL/LCD timing, frame boundaries and state restore through
 * the public peripheral API, including phase continuity across clock changes. */
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

static int check_iis_clock_phase(s3c2400_t *s, FILE *state) {
    s3c2400_reset(s);
    s3c2400_write32(s, 0x14800004u, 0u); /* FCLK 48 MHz. */
    s3c2400_write32(s, 0x14800014u, 0u);
    s3c2400_write32(s, 0x14600040u, 0x0c000000u);
    s3c2400_write32(s, 0x14600044u, 0x35508010u);
    s3c2400_write32(s, 0x14600048u, 0x10800008u);
    s3c2400_write32(s, 0x14600058u, 2u);
    s3c2400_write32(s, 0x15508000u, 1u);
    s3c2400_tick(s, 250u); /* Half a frame at the model's 96 kHz ceiling. */
    for (unsigned step = 0; step < 2u; ++step) {
        s3c2400_write32(s, 0x14800014u, step ? 0u : 3u);
        if (!step) {
            rewind(state);
            if (!s3c2400_state_save(s, state)) return 0;
            s3c2400_tick(s, 10000u);
            rewind(state);
            if (!s3c2400_state_load(s, state)) return 0;
        }
        uint32_t remaining = step ? 250u : 256u;
        s3c2400_tick(s, remaining - 1u);
        uint64_t frames = 0;
        uint32_t rate = 0;
        (void)s3c2400_audio_samples(s, &frames, &rate);
        if (frames != 0u) goto fail;
        s3c2400_tick(s, 1u);
        (void)s3c2400_audio_samples(s, &frames, &rate);
        if (frames != 1u || rate != (step ? 96000u : 46875u)) goto fail;
        s3c2400_audio_clear(s);
        if (!step) s3c2400_tick(s, 256u);
    }
    return 1;
fail:
    fputs("FAIL: IIS clock change shifts sample boundary\n", stderr);
    return 0;
}

static uint64_t frame_count(s3c2400_t *s) {
    uint64_t frames;
    (void)s3c2400_framebuffer(s, NULL, NULL, NULL, &frames);
    return frames;
}

static int check_iis_fifo_restart(s3c2400_t *s) {
    /* SDK stop disables TX FIFO to discard pending data. A completed stereo
     * frame stays available to the host, but a lone old channel must not be
     * paired with the first channel of the next playback. */
    s3c2400_reset(s);
    s3c2400_write32(s, 0x1550800cu, 0xa00u);
    s3c2400_write16(s, 0x15508010u, 1000u);
    s3c2400_write16(s, 0x15508010u, 2000u);
    s3c2400_write16(s, 0x15508010u, 3000u);
    s3c2400_write8(s, 0x1550800du, 0u); /* Masked TX-disable write. */
    s3c2400_write32(s, 0x1550800cu, 0xa00u);
    s3c2400_write16(s, 0x15508010u, 4000u);
    s3c2400_write16(s, 0x15508010u, 5000u);
    uint64_t frames = 0;
    const int16_t *pcm = s3c2400_audio_samples(s, &frames, NULL);
    if (!pcm || frames != 2u || pcm[0] != 1000 || pcm[1] != 2000 ||
        pcm[2] != 4000 || pcm[3] != 5000) {
        fputs("FAIL: IIS FIFO disable leaks a pending channel into new playback\n", stderr);
        return 0;
    }
    s3c2400_audio_clear(s);
    s3c2400_write16(s, 0x15508010u, 6000u);
    s3c2400_write32(s, 0x1550800cu, 0xa00u); /* Same enable must retain it. */
    s3c2400_write8(s, 0x1550800cu, 0u); /* Unrelated lane must retain it. */
    s3c2400_write16(s, 0x15508010u, 7000u);
    pcm = s3c2400_audio_samples(s, &frames, NULL);
    if (!pcm || frames != 1u || pcm[0] != 6000 || pcm[1] != 7000) {
        fputs("FAIL: IIS enabled FIFO loses a pending channel\n", stderr);
        return 0;
    }
    return 1;
}

static int check_lcd_clock_phase(s3c2400_t *s, FILE *state) {
    s3c2400_reset(s);
    s3c2400_write32(s, 0x14800004u, 0u); /* 48 MHz, 800,000 cycles per frame. */
    s3c2400_write32(s, 0x14800014u, 0u);
    s3c2400_write32(s, 0x14a00004u, 239u << 14);
    s3c2400_write32(s, 0x14a00000u, (12u << 1) | 1u);
    /* A long-running old state must preserve its current half-frame, without
     * mistaking completed frames for pending display work after a change. */
    s3c2400_tick(s, 4000400000u);
    uint32_t line = s3c2400_read32(s, 0x14a00000u);
    uint64_t frames = frame_count(s);
    for (unsigned step = 0; step < 2u; ++step) {
        s3c2400_write32(s, 0x14800014u, step ? 0u : 2u);
        if (s3c2400_read32(s, 0x14a00000u) != line || frame_count(s) != frames) goto fail;
        rewind(state);
        if (!s3c2400_state_save(s, state)) return 0;
        s3c2400_tick(s, 999999u);
        rewind(state);
        if (!s3c2400_state_load(s, state)) return 0;
        uint32_t half_frame = step ? 400000u : 200000u;
        s3c2400_tick(s, half_frame - 1u);
        if (frame_count(s) != frames) goto fail;
        s3c2400_tick(s, 1u);
        if (frame_count(s) != ++frames) goto fail;
        if (!step) s3c2400_tick(s, half_frame);
    }
    return 1;
fail:
    fputs("FAIL: LCD clock change jumps scan position or moves frame boundary\n", stderr);
    return 0;
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
    int phase_ok = check_lcd_clock_phase(s, state) && check_iis_clock_phase(s, state) &&
                   check_iis_fifo_restart(s);
    fclose(state);
    if (!phase_ok) { s3c2400_destroy(s); return 1; }
    printf("lcd_timing_trace=%016" PRIx64 "\n", hash);
    /* The former 47779e5037cd7b27 trace reinterpreted elapsed history on clock
     * changes. This phase-preserving trace was independently derived in the
     * frame-fraction domain, including cycle quantization and state restore. */
    if (hash != UINT64_C(0xdb468039216226ad)) { s3c2400_destroy(s); return 1; }
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
