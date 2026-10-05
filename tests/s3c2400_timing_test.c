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

static int check_iis_fractional_rate(s3c2400_t *s, FILE *state) {
    /* 132 MHz PLL / half HCLK gives 48 MHz effective CPU and 66 MHz PCLK.
     * PCLK / (6 * 256) advertises 42,968 Hz, not an integer CPU period. */
    const uint32_t runclk = 48000000u, rate = 42968u;
    for (unsigned split = 0; split < 3u; ++split) {
        s3c2400_reset(s);
        s3c2400_write32(s, 0x14800004u, 0xe000u);
        s3c2400_write32(s, 0x14800014u, 2u);
        s3c2400_write32(s, 0x15508008u, 5u << 5);
        s3c2400_write32(s, 0x14600040u, 0x0c000000u);
        s3c2400_write32(s, 0x14600044u, 0x35508010u);
        s3c2400_write32(s, 0x14600048u, 0x10800008u);
        s3c2400_write32(s, 0x14600058u, 2u);
        s3c2400_write32(s, 0x15508000u, 1u);
        if (s3c2400_run_clock_hz(s) != runclk) return 0;
        uint64_t total = 0;
        for (uint32_t elapsed = 0; elapsed < runclk;) {
            uint32_t n = split ? 32749u : runclk;
            if (split == 2u && !elapsed) n = 100u;
            if (n > runclk - elapsed) n = runclk - elapsed;
            s3c2400_tick(s, n);
            uint64_t frames; uint32_t tag;
            (void)s3c2400_audio_samples(s, &frames, &tag);
            total += frames;
            s3c2400_audio_clear(s);
            elapsed += n;
            if (frames && tag != rate) return 0; /* an empty queue has no new rate tag */
            if (split && elapsed == (split == 2u ? 100u : 32749u)) {
                rewind(state);
                if (!s3c2400_state_save(s, state)) return 0;
                s3c2400_tick(s, 12345u);
                rewind(state);
                /* Skipping the prefix exposes the unchanged legacy body.
                 * At 100 cycles its cycle-unit phase is exact. */
                if (split == 2u && fseek(state, 8L, SEEK_SET)) return 0;
                if (!s3c2400_state_load(s, state)) return 0;
            }
        }
        printf("iis_one_second split=%u frames=%" PRIu64 " rate=%u\n", split, total, rate);
        if (total != rate) { fputs("FAIL: IIS generated duration differs from sample-rate tag\n", stderr); return 0; }
    }
    return 1;
}

static int check_cpu_speed_keeps_peripheral_time(s3c2400_t *s) {
    /* The CPU-speed option grants more guest instructions per emulated second
     * but must keep IIS pitch/duration and PWM timer periods in real time, also
     * across a speed change in the middle of playback. */
    const uint32_t rate = 42968u;
    s3c2400_reset(s);
    s3c2400_write32(s, 0x14800004u, 0xe000u);
    s3c2400_write32(s, 0x14800014u, 2u);
    s3c2400_write32(s, 0x15508008u, 5u << 5);
    s3c2400_write32(s, 0x14600040u, 0x0c000000u);
    s3c2400_write32(s, 0x14600044u, 0x35508010u);
    s3c2400_write32(s, 0x14600048u, 0x10800008u);
    s3c2400_write32(s, 0x14600058u, 2u);
    s3c2400_write32(s, 0x15508000u, 1u);
    /* PWM timer 4: prescaler 0, divider 1/2, count 33000 -> 1 kHz at 66 MHz PCLK. */
    s3c2400_write32(s, 0x1510003cu, 33000u - 1u);
    s3c2400_write32(s, 0x15100008u, 0x00600000u); /* timer 4: auto-reload + manual update */
    s3c2400_write32(s, 0x15100008u, 0x00500000u); /* timer 4: auto-reload + start */
    uint64_t total = 0;
    unsigned timer_irqs = 0;
    for (unsigned half = 0; half < 2u; ++half) {
        if (!s3c2400_set_cpu_speed_percent(s, half ? 200u : 100u)) return 0;
        uint32_t runclk = s3c2400_run_clock_hz(s);
        if (runclk != (half ? 96000000u : 48000000u)) return 0;
        for (uint32_t elapsed = 0; elapsed < runclk / 2u;) {
            uint32_t n = 4000u;
            s3c2400_tick(s, n);
            elapsed += n;
            uint64_t frames; uint32_t tag;
            (void)s3c2400_audio_samples(s, &frames, &tag);
            if (frames && tag != rate) return 0;
            total += frames;
            s3c2400_audio_clear(s);
            if (s3c2400_read32(s, 0x14400000u) & (1u << 14)) {
                ++timer_irqs;
                s3c2400_write32(s, 0x14400000u, 1u << 14);
            }
        }
    }
    if (!s3c2400_set_cpu_speed_percent(s, 100u) || s3c2400_set_cpu_speed_percent(s, 10u)) return 0;
    printf("cpu_speed frames=%" PRIu64 " rate=%u timer_irqs=%u\n", total, rate, timer_irqs);
    if (total + 1u < rate || total > rate + 1u) { fputs("FAIL: CPU speed changed IIS duration\n", stderr); return 0; }
    if (timer_irqs + 2u < 1000u || timer_irqs > 1000u + 2u) { fputs("FAIL: CPU speed changed PWM period\n", stderr); return 0; }
    return 1;
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

static int check_lcd_scanline_poll(s3c2400_t *s) {
    /* Wait for an exact scanline, as opposed to merely checking vblank.
     * Frame-aligned large slices must not repeatedly skip the target. */
    const uint32_t code[] = {
        0xe5902000u, 0xe1a02922u, 0xe1520001u, 0x1afffffbu,
        0xe3a04001u, 0xeafffffeu,
    }; /* LDR; LSR #18; CMP r2,r1; BNE; MOV r4,#1; B . */
    const uint32_t phases[] = {0u, 10000u, 799999u};
    arm_bus_t bus = s3c2400_get_bus(s);
    arm920t_t *cpu = arm920t_create(&bus);
    if (!cpu) return 0;
    s3c2400_set_irq_sink(s, cpu);
    int ok = 1;
    for (unsigned jit = 0; jit < 2u && ok; ++jit) {
        for (unsigned k = 0; k < GP32_ARRAY_COUNT(phases) && ok; ++k) {
            s3c2400_reset(s);
            arm920t_reset(cpu, 0x0c000000u);
            arm920t_set_jit(cpu, (int)jit);
            for (unsigned i = 0; i < GP32_ARRAY_COUNT(code); ++i)
                s3c2400_write32(s, 0x0c000000u + i * 4u, code[i]);
            arm920t_set_reg(cpu, 0, 0x14a00000u);
            arm920t_set_reg(cpu, 1, 8u);
            s3c2400_write32(s, 0x14800004u, 0u); /* 48 MHz. */
            s3c2400_write32(s, 0x14800014u, 0u);
            s3c2400_write32(s, 0x14a00004u, 319u << 14);
            s3c2400_write32(s, 0x14a00000u, 1u);
            s3c2400_tick(s, phases[k]);
            for (unsigned frame = 0; frame < 2u && ok; ++frame) {
                uint32_t remaining = 800000u;
                while (remaining) {
                    uint32_t budget = remaining > 32768u ? 32768u : remaining;
                    uint32_t done = s3c2400_run_cpu(s, budget);
                    if (!done || done > budget) { ok = 0; break; }
                    remaining -= done;
                }
            }
            if (arm920t_get_reg(cpu, 4) != 1u) {
                fprintf(stderr, "FAIL: missed LCD scanline (jit=%u phase=%u)\n", jit, phases[k]);
                ok = 0;
            }
        }
    }
    s3c2400_set_irq_sink(s, NULL);
    arm920t_destroy(cpu);
    return ok;
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
                   check_iis_fifo_restart(s) && check_lcd_scanline_poll(s) && check_iis_fractional_rate(s, state) &&
                   check_cpu_speed_keeps_peripheral_time(s);
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
