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

/* Cross-device consistency oracle. The established one-cycle bus order is
 * LCD, IIS, then PWM; IIS at cycle 500 therefore observes the timer after
 * cycle 499 (751 from a 1000 count at /2). These exact values preserve the
 * emulator policy and do not assert silicon arbitration or full-state identity:
 * the host render-generation counter still coalesces quiet multi-frame ticks.
 * All CPU code and the tiny IRQ vector below are synthetic test instructions.
 */
#define RAM 0x0c000000u
#define PWM 0x15100000u
#define IRQ 0x14400000u
#define DMA 0x14600000u
#define IIS 0x15508000u
#define LCD 0x14a00000u

enum scenario {
    READ_TIMER, ACK_TIMER, STOP_IIS, CHANGE_IIS_RATE, CHANGE_PCLK,
    READ_TIMER_PCM, READ_LCD, WRITE_VRAM, PWM_START_IIS, PWM_STOP_IIS,
    SAME_CYCLE_IRQ, WRITE_CODE, UNMASK_IRQ, MUTED_PCM_CHANGE, RELOAD_MMIO_SOURCE,
    SCENARIOS
};
static const char *const names[] = {
    "IIS DMA reads PWM count", "IIS DMA acknowledges earlier PWM IRQ",
    "IIS DMA stops IIS", "IIS DMA changes IIS prescaler", "IIS DMA changes PCLK",
    "fast IIS DMA reads PWM as PCM", "IIS DMA reads LCD scan position",
    "IIS DMA changes VRAM before scanout", "PWM DMA starts IIS", "PWM DMA stops IIS",
    "IIS/PWM same-cycle request ordering", "IIS DMA changes running CPU code",
    "IIS DMA unmasks pending CPU IRQ", "muted RAM PCM changes before partial pair",
    "RAM IIS DMA reloads an MMIO source"
};
typedef struct observation {
    uint32_t words[2], timer, irq, dma_count, dma_src, dma_dst, dma_on;
    uint32_t iis_control, iis_psr, pclk, pixel, cpu_r7, cpu_pc;
    uint64_t pcm_frames;
    int16_t pcm[8];
} observation_t;

static void dma_config(s3c2400_t *s, unsigned ch, uint32_t src, uint32_t dst, uint32_t dcon) {
    uint32_t r = DMA + ch * 32u;
    s3c2400_write32(s, r, src);
    s3c2400_write32(s, r + 4u, dst);
    s3c2400_write32(s, r + 8u, dcon);
    s3c2400_write32(s, r + 24u, 2u);
}
static void timer_start(s3c2400_t *s, uint32_t count) {
    s3c2400_write32(s, PWM + 12u, count);
    s3c2400_write32(s, PWM + 8u, 2u);
    s3c2400_write32(s, PWM + 8u, 1u);
}
static void lcd_setup(s3c2400_t *s, unsigned visible, unsigned width) {
    s3c2400_write32(s, LCD + 4u, (visible - 1u) << 14);
    s3c2400_write32(s, LCD + 8u, (width - 1u) << 8);
    s3c2400_write32(s, LCD + 12u, 0u);
    s3c2400_write32(s, LCD + 20u, (RAM + 0x200u) >> 1);
    s3c2400_write32(s, LCD + 24u, (RAM + 0x204u) >> 1);
    s3c2400_write32(s, LCD + 28u, 2u);
    s3c2400_write32(s, LCD, (1u << 8) | (3u << 5) | (12u << 1) | 1u);
}
static int run(enum scenario which, unsigned quantum, int cpu_mode, observation_t *out) {
    s3c2400_t *s = s3c2400_create(16384u);
    if (!s) return 0;
    arm920t_t *cpu = NULL;
    if (cpu_mode >= 0) {
        arm_bus_t bus = s3c2400_get_bus(s);
        cpu = arm920t_create(&bus);
        if (!cpu) { s3c2400_destroy(s); return 0; }
        s3c2400_set_irq_sink(s, cpu);
        arm920t_set_jit(cpu, cpu_mode);
        s3c2400_write32(s, RAM, 0xeafffffeu); /* B .; no MMIO or guest stores. */
        arm920t_set_reg(cpu, 15u, RAM);
    }
    s3c2400_set_audio_idle(s, 0);
    s3c2400_write32(s, 0x14800004u, 0u);
    s3c2400_write32(s, 0x14800014u, 0u);
    s3c2400_write32(s, RAM + 0x100u, 0u);
    s3c2400_write32(s, RAM + 0x104u, 0x11223344u);
    uint32_t cycles = 1000u;
    switch (which) {
    case READ_TIMER:
        timer_start(s, 1000u);
        dma_config(s, 2, (PWM + 20u) | (1u << 29), RAM + 0x1000u, 0x00e00002u);
        break;
    case ACK_TIMER:
        timer_start(s, 100u);
        s3c2400_write32(s, RAM + 0x100u, 1u << 10);
        dma_config(s, 2, RAM + 0x100u, IRQ | (1u << 29), 0x00e00001u);
        break;
    case STOP_IIS:
        dma_config(s, 2, (RAM + 0x100u) | (1u << 29), IIS | (1u << 29), 0x00e00008u);
        break;
    case CHANGE_IIS_RATE:
        s3c2400_write32(s, RAM + 0x100u, 1u << 5);
        dma_config(s, 2, (RAM + 0x100u) | (1u << 29), (IIS + 8u) | (1u << 29), 0x00e00008u);
        break;
    case CHANGE_PCLK:
        timer_start(s, 1000u);
        s3c2400_write32(s, RAM + 0x100u, 1u);
        dma_config(s, 2, (RAM + 0x100u) | (1u << 29), 0x34800014u, 0x00e00008u);
        break;
    case READ_TIMER_PCM:
        timer_start(s, 1000u);
        dma_config(s, 2, (PWM + 20u) | (1u << 29), (IIS + 16u) | (1u << 29), 0x00e00002u);
        break;
    case READ_LCD:
        lcd_setup(s, 64u, 8u); /* line=44 HCLK; 67 lines/frame. */
        dma_config(s, 2, LCD | (1u << 29), RAM + 0x1000u, 0x00e00002u);
        break;
    case WRITE_VRAM:
        lcd_setup(s, 3u, 39u); /* line=168 HCLK; six lines/frame: boundary=1008. */
        s3c2400_write32(s, RAM + 0x100u, 0x7c007c00u);
        dma_config(s, 2, RAM + 0x100u, RAM + 0x200u, 0x00e00001u);
        cycles = 1008u;
        break;
    case PWM_START_IIS:
    case PWM_STOP_IIS:
        timer_start(s, 100u); /* first/only DMA request at cycle 200. */
        s3c2400_write32(s, PWM + 4u, 1u << 20);
        s3c2400_write32(s, RAM + 0x100u, which == PWM_START_IIS);
        dma_config(s, 0, RAM + 0x100u, IIS | (1u << 29), 0x03e00001u);
        dma_config(s, 2, (RAM + 0x104u) | (1u << 29), (IIS + 16u) | (1u << 29), 0x00e00008u);
        break;
    case SAME_CYCLE_IRQ:
        timer_start(s, 250u); /* terminal at the first IIS frame's cycle 500. */
        dma_config(s, 2, IRQ | (1u << 29), RAM + 0x1000u, 0x00e00002u);
        break;
    case WRITE_CODE:
        s3c2400_write32(s, RAM + 0x100u, 0xe3a0702au); /* MOV r7,#42. */
        s3c2400_write32(s, RAM + 4u, 0xeafffffeu); /* B . after the changed instruction. */
        dma_config(s, 2, RAM + 0x100u, RAM, 0x00e00001u);
        break;
    case UNMASK_IRQ: {
        uint8_t bios[32] = {0};
        gp32_st32le(bios + 0x18u, 0xe51ff004u); /* LDR pc,[pc,#-4]. */
        gp32_st32le(bios + 0x1cu, RAM + 0x2000u);
        char error[128];
        if (!s3c2400_load_bios_buffer(s, bios, sizeof(bios), error, sizeof(error))) return 0;
        const uint32_t handler[] = {
            0xe2877001u, /* ADD r7,r7,#1. */
            0xe5889000u, /* STR r9,[r8]: acknowledge SRCPND. */
            0xe5889010u, /* STR r9,[r8,#16]: acknowledge INTPND. */
            0xe25ef004u, /* SUBS pc,lr,#4. */
        };
        for (unsigned i = 0; i < GP32_ARRAY_COUNT(handler); ++i)
            s3c2400_write32(s, RAM + 0x2000u + i * 4u, handler[i]);
        s3c2400_write32(s, IRQ + 8u, UINT32_MAX);
        timer_start(s, 1u);
        s3c2400_tick(s, 2u); /* Already pending but still masked. */
        s3c2400_write32(s, RAM + 0x100u, ~(1u << 10));
        dma_config(s, 2, RAM + 0x100u, (IRQ + 8u) | (1u << 29), 0x00e00001u);
        if (cpu) {
            arm920t_set_cpsr(cpu, 0x53u);
            arm920t_set_reg(cpu, 8u, IRQ);
            arm920t_set_reg(cpu, 9u, 1u << 10);
        }
        break;
    }
    case MUTED_PCM_CHANGE:
        timer_start(s, 375u); /* DMA at cycle 750, between IIS frames. */
        s3c2400_write32(s, PWM + 4u, 1u << 20);
        s3c2400_write32(s, RAM + 0x100u, 0x33334444u);
        s3c2400_write32(s, RAM + 0x108u, 0xaaaabbbbu);
        dma_config(s, 0, RAM + 0x100u, RAM + 0x108u, 0x03e00001u);
        dma_config(s, 2, RAM + 0x104u, (IIS + 16u) | (1u << 29), 0x00d00003u);
        s3c2400_audio_set_volume(s, 63u);
        break;
    case RELOAD_MMIO_SOURCE:
        timer_start(s, 2000u);
        dma_config(s, 2, (RAM + 0x104u) | (1u << 29), (IIS + 16u) | (1u << 29), 0x00a00002u);
        /* Change the reload source without replacing the current RAM cursor. */
        s3c2400_write32(s, DMA + 0x40u, (PWM + 20u) | (1u << 29));
        cycles = 2000u;
        break;
    default: return 0;
    }
    if (which != PWM_START_IIS) s3c2400_write32(s, IIS, 1u);
    for (unsigned remaining = cycles; remaining;) {
        unsigned step = quantum < remaining ? quantum : remaining;
        uint32_t done = step;
        if (cpu) done = s3c2400_run_cpu(s, step);
        else s3c2400_tick(s, step);
        if (!done || done > step) return 0;
        remaining -= done;
    }
    if (which == MUTED_PCM_CHANGE) {
        s3c2400_audio_set_volume(s, 0u);
        s3c2400_write16(s, IIS + 16u, 0x1234u);
    }
    memset(out, 0, sizeof(*out));
    out->words[0] = s3c2400_read32(s, RAM + 0x1000u);
    out->words[1] = s3c2400_read32(s, RAM + 0x1004u);
    out->timer = s3c2400_read32(s, PWM + 20u);
    out->irq = s3c2400_read32(s, IRQ);
    out->dma_count = s3c2400_read32(s, DMA + 0x4cu);
    out->dma_src = s3c2400_read32(s, DMA + 0x50u);
    out->dma_dst = s3c2400_read32(s, DMA + 0x54u);
    out->dma_on = s3c2400_read32(s, DMA + 0x58u);
    out->iis_control = s3c2400_read32(s, IIS);
    out->iis_psr = s3c2400_read32(s, IIS + 8u);
    out->pclk = s3c2400_pclk_hz(s);
    if (cpu) {
        out->cpu_r7 = arm920t_get_reg(cpu, 7u);
        out->cpu_pc = arm920t_get_reg(cpu, 15u);
    }
    const int16_t *pcm = s3c2400_audio_samples(s, &out->pcm_frames, NULL);
    if (out->pcm_frames > 4u) return 0;
    if (out->pcm_frames) memcpy(out->pcm, pcm, (size_t)out->pcm_frames * 4u);
    if (which == WRITE_VRAM) out->pixel = s3c2400_framebuffer(s, NULL, NULL, NULL, NULL)[0];
    s3c2400_set_irq_sink(s, NULL);
    arm920t_destroy(cpu);
    s3c2400_destroy(s);
    return 1;
}
static void print_observation(const char *kind, const observation_t *o) {
    fprintf(stderr, "  %s reads=[%08x,%08x] count=%u srcpnd=%08x dma=[%u,%08x,%08x,%u] IIS=[%u,%u] PCLK=%u pixel=%08x CPU=[%u,%08x] PCM=%" PRIu64 " [%d,%d,%d,%d,%d,%d,%d,%d]\n",
        kind, o->words[0], o->words[1], o->timer, o->irq, o->dma_count, o->dma_src, o->dma_dst, o->dma_on,
        o->iis_control, o->iis_psr, o->pclk, o->pixel, o->cpu_r7, o->cpu_pc, o->pcm_frames,
        o->pcm[0], o->pcm[1], o->pcm[2], o->pcm[3], o->pcm[4], o->pcm[5], o->pcm[6], o->pcm[7]);
}
static int event_expected(enum scenario which, int mode, const observation_t *o) {
    switch (which) {
    case READ_TIMER: return o->words[0] == 751u && o->words[1] == 501u && o->timer == 500u;
    case ACK_TIMER: return o->irq == 0u && o->timer == 0u;
    case STOP_IIS: return o->iis_control == 0u && o->dma_count == 7u;
    case CHANGE_IIS_RATE: return o->iis_psr == 32u && o->dma_count == 7u;
    case CHANGE_PCLK: return o->pclk == 24000000u && o->timer == 626u && o->dma_count == 7u;
    case READ_TIMER_PCM: return o->pcm_frames == 2u && o->pcm[1] == 751 && o->pcm[3] == 501;
    case READ_LCD: return o->words[0] == 0x00d80179u && o->words[1] == 0x00ac0179u;
    case WRITE_VRAM: return o->pixel == 0xff798200u;
    case PWM_START_IIS: return o->pcm_frames == 1u && o->dma_count == 7u;
    case PWM_STOP_IIS: return o->pcm_frames == 0u && o->dma_count == 8u;
    case SAME_CYCLE_IRQ: return o->words[0] == 0u && o->words[1] == (1u << 10);
    case WRITE_CODE: return mode < 0 || (o->cpu_r7 == 42u && o->cpu_pc == RAM + 4u);
    case UNMASK_IRQ: return mode < 0 || (o->cpu_r7 == 1u && o->irq == 0u);
    case MUTED_PCM_CHANGE: return o->pcm_frames == 2u && o->pcm[0] == 0 && o->pcm[1] == 0 &&
                                 o->pcm[2] == 0x4444 && o->pcm[3] == 0x1234;
    case RELOAD_MMIO_SOURCE: return o->pcm_frames == 4u && o->pcm[5] == 1251 && o->pcm[7] == 1001;
    default: return 0;
    }
}

static int check_soc_event_order(void) {
    const unsigned quantums[] = {499u, 500u, 501u, 32768u};
    unsigned cases = 0u, failures = 0u;
    for (int mode = -1; mode <= 1; ++mode) {
        for (enum scenario which = 0; which < SCENARIOS; ++which) {
            observation_t fine, coarse;
            if (!run(which, 1u, mode, &fine)) return 0;
            if (!event_expected(which, mode, &fine)) {
                fprintf(stderr, "FAIL: one-cycle oracle mode=%d case=%s\n", mode, names[which]);
                print_observation("fine", &fine);
                ++failures;
            }
            for (unsigned q = 0; q < GP32_ARRAY_COUNT(quantums); ++q) {
                ++cases;
                if (!run(which, quantums[q], mode, &coarse)) return 0;
                if (memcmp(&fine, &coarse, sizeof(fine))) {
                    fprintf(stderr, "FAIL: event order mode=%d quantum=%u case=%s\n",
                            mode, quantums[q], names[which]);
                    print_observation("fine  ", &fine);
                    print_observation("coarse", &coarse);
                    ++failures;
                }
            }
        }
    }
    /* A pending timer IRQ has no repeated bus effect. The common scheduler
     * must preserve arithmetic advancement instead of walking 2^32 cycles. */
    s3c2400_t *s = s3c2400_create(4096u);
    if (!s) return 0;
    s3c2400_write32(s, 0x14800004u, 0u);
    s3c2400_write32(s, 0x14800014u, 0u);
    s3c2400_write32(s, PWM + 12u, 1u);
    s3c2400_write32(s, PWM + 8u, 10u);
    s3c2400_write32(s, PWM + 8u, 9u);
    s3c2400_tick(s, UINT32_MAX);
    if (s3c2400_read32(s, PWM + 20u) != 0u || s3c2400_read32(s, IRQ) != (1u << 10)) ++failures;
    s3c2400_tick(s, 1u);
    if (s3c2400_read32(s, PWM + 20u) != 1u) ++failures;
    /* Maximum TFT geometry has 8,808,038,400 HCLK/frame. At RUN=3.156 GHz
     * its frame*RUN numerator exceeds uint64_t, even though this UINT32_MAX
     * interval advances only 2,147,483,647 HCLK. An armed PWM DMA requires the
     * frame deadline to be considered; the bounded-distance guard must win. */
    s3c2400_reset(s);
    s3c2400_write32(s, 0x14800004u, 0xff000u);
    s3c2400_write32(s, 0x14800014u, 0u);
    if (!s3c2400_set_cpu_speed_percent(s, 200u)) ++failures;
    s3c2400_write32(s, LCD + 4u, UINT32_MAX);
    s3c2400_write32(s, LCD + 8u, UINT32_MAX);
    s3c2400_write32(s, LCD + 12u, UINT32_MAX);
    s3c2400_write32(s, LCD, (1023u << 8) | (3u << 5) | (12u << 1) | 1u);
    s3c2400_write32(s, RAM + 0x100u, 0xdeadbeefu);
    timer_start(s, 1u);
    s3c2400_write32(s, PWM + 4u, 1u << 20);
    dma_config(s, 0u, RAM + 0x100u, RAM + 0x200u, 0x03e00001u);
    s3c2400_tick(s, UINT32_MAX);
    s3c2400_clock_snapshot_t phase;
    s3c2400_clock_snapshot_capture(s, &phase);
    if (phase.lcd_line_accum != UINT32_MAX / 2u || phase.lcd_hclk_remainder != 1578000000u ||
        s3c2400_read32(s, RAM + 0x200u) != 0xdeadbeefu ||
        (s3c2400_read32(s, LCD) >> 18) != 953u) ++failures;
    s3c2400_destroy(s);
    printf("soc_event_order cases=%u failures=%u maximum_tick=checked\n", cases, failures);
    return failures == 0u;
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
                   check_cpu_speed_keeps_peripheral_time(s) && check_soc_event_order();
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
