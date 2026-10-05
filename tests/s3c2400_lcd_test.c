/* Exact TFT scan timing through the existing public SoC API.
 *
 * Oracle: Samsung S3C2400 ch.15, TFT VCLK = HCLK / (2*(CLKVAL+1));
 * each porch/sync/active field encodes its length minus one. LCDCON5
 * VSTATUS[20:19]/HSTATUS[18:17] are read-only: sync/back/active/front =
 * 0/1/2/3. ENVID rising starts at sync origin (the integration contract).
 * LINECNT is checked ONLY during active video; blanking readback is not
 * assumed. Period checks compare the last active count with the next
 * active reload. No renderer count, private state, state version or layout.
 *
 * Scope excludes STN, CLKVAL=0 and live timing-register latch semantics.
 * All arithmetic and tick loops are bounded to a few synthetic frames.
 */
#include "s3c2400.h"
#include <inttypes.h>

#define MPLLCON_ADDR 0x14800004u
#define CLKDIVN_ADDR 0x14800014u
#define LCDCON1_ADDR 0x14a00000u
#define LCDCON2_ADDR 0x14a00004u
#define LCDCON3_ADDR 0x14a00008u
#define LCDCON4_ADDR 0x14a0000cu
#define LCDCON5_ADDR 0x14a00010u
#define STATUS_MASK (15u << 17)
#define LCD_CONTROL 0x702u

typedef struct lcd_geometry {
    uint32_t cv, hs, hb, ha, hf, vs, vb, va, vf;
    uint64_t pixel, line, frame, active_begin, active_end;
} lcd_geometry_t;

static lcd_geometry_t geometry(uint32_t cv) {
    const uint32_t c2 = 0x0e3bc102u, c3 = 0x0078ef01u, c4 = 0x19u;
    lcd_geometry_t g;
    g.cv = cv;
    g.hs = (c4 & 255u) + 1u;
    g.hb = ((c3 >> 19) & 127u) + 1u;
    g.ha = ((c3 >> 8) & 2047u) + 1u;
    g.hf = (c3 & 255u) + 1u;
    g.vs = (c2 & 63u) + 1u;
    g.vb = ((c2 >> 24) & 255u) + 1u;
    g.va = ((c2 >> 14) & 1023u) + 1u;
    g.vf = ((c2 >> 6) & 255u) + 1u;
    g.pixel = 2u * (cv + 1u);
    g.line = g.pixel * (g.hs + g.hb + g.ha + g.hf);
    g.frame = g.line * (g.vs + g.vb + g.va + g.vf);
    g.active_begin = g.line * (g.vs + g.vb);
    g.active_end = g.active_begin + g.line * g.va;
    return g;
}

static uint64_t ceil_ratio(uint64_t numerator, uint64_t denominator) {
    return numerator / denominator + (numerator % denominator != 0u);
}

static uint32_t linecnt(s3c2400_t *s) {
    return (s3c2400_read32(s, LCDCON1_ADDR) >> 18) & 1023u;
}

static unsigned interval_status(uint64_t pos, uint64_t sync,
                                uint64_t back, uint64_t active) {
    if (pos < sync) return 0u;
    if (pos < sync + back) return 1u;
    if (pos < sync + back + active) return 2u;
    return 3u;
}

/* hclk_elapsed is floor(exact elapsed HCLK), never a rounded frame period.
 * Test inputs fit comfortably in uint64_t, including RUN * HCLK products. */
static int expect_phase(s3c2400_t *s, const lcd_geometry_t *g,
                        uint64_t hclk_elapsed, const char *where) {
    uint64_t phase = hclk_elapsed % g->frame;
    uint64_t row = phase / g->line, col = phase % g->line;
    unsigned v = interval_status(row, g->vs, g->vb, g->va);
    unsigned h = interval_status(col, g->pixel * g->hs,
                                 g->pixel * g->hb, g->pixel * g->ha);
    uint32_t want_status = (v << 19) | (h << 17);
    uint32_t c1 = s3c2400_read32(s, LCDCON1_ADDR);
    uint32_t c5 = s3c2400_read32(s, LCDCON5_ADDR);
    if (s3c2400_debug_read32(s, LCDCON1_ADDR) != c1 ||
        s3c2400_debug_read32(s, LCDCON5_ADDR) != c5 ||
        (c5 & STATUS_MASK) != want_status || (c5 & 0x1fffu) != LCD_CONTROL) {
        fprintf(stderr, "FAIL: %s CV=%u HCLK=%" PRIu64
                " LCDCON5=%08x want V/H=%u/%u (MMIO/debug must agree)\n",
                where, g->cv, hclk_elapsed, c5, v, h);
        return 0;
    }
    if (v == 2u) {
        uint32_t want_line = g->va - 1u - (uint32_t)(row - g->vs - g->vb);
        if (((c1 >> 18) & 1023u) != want_line) {
            fprintf(stderr, "FAIL: %s CV=%u HCLK=%" PRIu64
                    " active LINECNT=%u want=%u\n", where, g->cv,
                    hclk_elapsed, (c1 >> 18) & 1023u, want_line);
            return 0;
        }
    }
    return 1;
}

static int expect_clocks(s3c2400_t *s, uint32_t f, uint32_t h, uint32_t run) {
    if (s3c2400_fclk_hz(s) == f && s3c2400_hclk_hz(s) == h &&
        s3c2400_run_clock_hz(s) == run) return 1;
    fprintf(stderr, "FAIL: fixture FCLK/HCLK/RUN=%u/%u/%u want=%u/%u/%u\n",
            s3c2400_fclk_hz(s), s3c2400_hclk_hz(s),
            s3c2400_run_clock_hz(s), f, h, run);
    return 0;
}

static int configure(s3c2400_t *s, uint32_t cv, int divided) {
    s3c2400_reset(s);
    s3c2400_write32(s, MPLLCON_ADDR, divided ? 0xe000u : 0x2000u);
    s3c2400_write32(s, CLKDIVN_ADDR, divided ? 2u : 0u);
    s3c2400_write32(s, LCDCON1_ADDR, 0u);
    s3c2400_write32(s, LCDCON2_ADDR, 0x0e3bc102u);
    s3c2400_write32(s, LCDCON3_ADDR, 0x0078ef01u);
    s3c2400_write32(s, LCDCON4_ADDR, 0x19u);
    s3c2400_write32(s, LCDCON5_ADDR, LCD_CONTROL);
    s3c2400_write32(s, LCDCON1_ADDR, (cv << 8) | 0x79u);
    return divided ? expect_clocks(s, 132000000u, 66000000u, 48000000u) :
                     expect_clocks(s, 60000000u, 60000000u, 60000000u);
}

static void tick_to(s3c2400_t *s, uint64_t *elapsed, uint64_t target) {
    /* Every caller supplies increasing targets below 2^32 RUN cycles. */
    s3c2400_tick(s, (uint32_t)(target - *elapsed));
    *elapsed = target;
}

static int check_period(s3c2400_t *s, uint32_t cv) {
    lcd_geometry_t g = geometry(cv);
    uint64_t elapsed = 0, last_reload;
    if (!configure(s, cv, 0)) return 0;
    /* Independent fixture cross-check: H=26+16+240+2, V=3+15+240+5. */
    if (g.frame != (cv == 5u ? UINT64_C(896304) : UINT64_C(1792608))) {
        fputs("FAIL: LCD test fixture arithmetic\n", stderr);
        return 0;
    }
    tick_to(s, &elapsed, g.active_begin);
    if (!expect_phase(s, &g, elapsed, "first active reload")) return 0;
    last_reload = elapsed;
    for (unsigned frame = 0; frame < 3u; ++frame) {
        tick_to(s, &elapsed, frame * g.frame + g.active_end - 1u);
        if (!expect_phase(s, &g, elapsed, "last active line")) return 0;
        uint32_t last_active = linecnt(s);
        uint64_t next = (frame + 1u) * g.frame + g.active_begin;
        tick_to(s, &elapsed, next - 1u);
        if (!expect_phase(s, &g, elapsed, "before next active reload")) return 0;
        tick_to(s, &elapsed, next);
        if (!expect_phase(s, &g, elapsed, "next active reload")) return 0;
        uint32_t reload = linecnt(s);
        if (reload <= last_active || elapsed - last_reload != g.frame) {
            fprintf(stderr, "FAIL: CV=%u LINECNT active reload %u->%u"
                    " interval=%" PRIu64 " want=%" PRIu64 "\n",
                    cv, last_active, reload, elapsed - last_reload, g.frame);
            return 0;
        }
        last_reload = elapsed;
    }
    printf("PASS: CV=%u three active LINECNT reloads, period=%" PRIu64 " HCLK/RUN\n",
           cv, g.frame);
    return 1;
}

static int check_status_edges(s3c2400_t *s) {
    lcd_geometry_t g = geometry(5u);
    const uint64_t edges[] = {
        g.pixel * g.hs, g.pixel * (g.hs + g.hb),
        g.pixel * (g.hs + g.hb + g.ha), g.line,
        g.line * g.vs, g.active_begin, g.active_end, g.frame
    };
    uint64_t elapsed = 0;
    if (!configure(s, g.cv, 0) || !expect_phase(s, &g, 0u, "ENVID sync origin")) return 0;
    for (unsigned i = 0; i < GP32_ARRAY_COUNT(edges); ++i) {
        tick_to(s, &elapsed, edges[i] - 1u);
        if (!expect_phase(s, &g, elapsed, "one cycle before status edge")) return 0;
        tick_to(s, &elapsed, edges[i]);
        if (!expect_phase(s, &g, elapsed, "at status edge")) return 0;
        /* Attempt to forge every status bit without changing LCD controls.
         * This write must neither replace live readback nor rephase scanout. */
        uint32_t c5 = s3c2400_read32(s, LCDCON5_ADDR);
        s3c2400_write32(s, LCDCON5_ADDR, c5 ^ STATUS_MASK);
        if (!expect_phase(s, &g, elapsed, "read-only status write")) return 0;
        tick_to(s, &elapsed, edges[i] + 1u);
        if (!expect_phase(s, &g, elapsed, "one cycle after status edge")) return 0;
    }
    s3c2400_write32(s, LCDCON1_ADDR, (g.cv << 8) | 0x78u);
    s3c2400_tick(s, 137u);
    s3c2400_write32(s, LCDCON1_ADDR, (g.cv << 8) | 0x79u);
    if (!expect_phase(s, &g, 0u, "ENVID re-enable sync origin")) return 0;
    puts("PASS: exact TFT status edges, read-only status and ENVID origin");
    return 1;
}

static void tick_split(s3c2400_t *s, uint32_t cycles) {
    static const uint32_t pieces[] = {1u, 7u, 113u, 4093u};
    unsigned i = 0;
    while (cycles) {
        uint32_t n = pieces[i++ % GP32_ARRAY_COUNT(pieces)];
        if (n > cycles) n = cycles;
        s3c2400_tick(s, n);
        cycles -= n;
    }
}

static int check_fractional_state(s3c2400_t *whole, s3c2400_t *split,
                                  s3c2400_t *restored) {
    lcd_geometry_t g = geometry(5u);
    const uint64_t hpoints[] = {
        g.pixel * g.hs, g.pixel * (g.hs + g.hb), g.line,
        g.active_begin, g.active_begin + g.line, g.active_end,
        g.frame, g.frame + g.pixel * g.hs, 2u * g.frame,
        3u * g.frame + g.active_begin
    };
    uint64_t elapsed = 1u;
    if (!configure(whole, g.cv, 1) || !configure(split, g.cv, 1) ||
        !configure(restored, 11u, 0)) return 0;
    s3c2400_tick(whole, 1u);
    s3c2400_tick(split, 1u);
    /* One RUN cycle = 11/8 HCLK; save a nonzero 3/8 HCLK remainder.
     * A dropped remainder shifts the first HSTATUS edge at RUN=227. */
    FILE *state = tmpfile();
    if (!state) { fputs("FAIL: tmpfile for LCD phase state\n", stderr); return 0; }
    int loaded = s3c2400_state_save(whole, state);
    /* Force different clocks, geometry and cached readback before loading. */
    s3c2400_tick(restored, 123457u);
    (void)s3c2400_read32(restored, LCDCON1_ADDR);
    (void)s3c2400_read32(restored, LCDCON5_ADDR);
    if (loaded && fseek(state, 0L, SEEK_SET) == 0)
        loaded = s3c2400_state_load(restored, state);
    else loaded = 0;
    fclose(state);
    if (!loaded) { fputs("FAIL: public LCD phase state roundtrip\n", stderr); return 0; }
    if (!expect_clocks(restored, 132000000u, 66000000u, 48000000u) ||
        !expect_phase(restored, &g, 1u, "restored fractional prefix")) return 0;
    for (unsigned i = 0; i < GP32_ARRAY_COUNT(hpoints); ++i) {
        /* Ceiling applies to the absolute edge, never independently per frame. */
        uint64_t edge = ceil_ratio(hpoints[i] * 48000000u, 66000000u);
        for (unsigned at = 0; at < 2u; ++at) {
            uint64_t target = edge - (at == 0u ? 1u : 0u);
            uint32_t delta = (uint32_t)(target - elapsed);
            s3c2400_tick(whole, delta);
            tick_split(split, delta);
            s3c2400_tick(restored, delta);
            elapsed = target;
            uint64_t hphase = elapsed * 66000000u / 48000000u;
            if (!expect_phase(whole, &g, hphase, "whole fractional tick") ||
                !expect_phase(split, &g, hphase, "split fractional tick") ||
                !expect_phase(restored, &g, hphase, "restored fractional tick")) return 0;
            /* Blanking LINECNT semantics stay unspecified, but partitioning
             * and save/load must still produce identical public readback. */
            uint32_t c1 = s3c2400_read32(whole, LCDCON1_ADDR);
            uint32_t c5 = s3c2400_read32(whole, LCDCON5_ADDR);
            if (s3c2400_read32(split, LCDCON1_ADDR) != c1 ||
                s3c2400_read32(restored, LCDCON1_ADDR) != c1 ||
                s3c2400_read32(split, LCDCON5_ADDR) != c5 ||
                s3c2400_read32(restored, LCDCON5_ADDR) != c5) {
                fprintf(stderr, "FAIL: split/state LCD readback differs at RUN=%" PRIu64 "\n", elapsed);
                return 0;
            }
        }
    }
    puts("PASS: HCLK66/RUN48 whole/split ticks and fractional save/load phase");
    return 1;
}

static int check_clock_phase(s3c2400_t *s) {
    lcd_geometry_t g = geometry(5u);
    if (!configure(s, g.cv, 1)) return 0;
    s3c2400_tick(s, 1u); /* 11/8 HCLK. */
    s3c2400_write32(s, CLKDIVN_ADDR, 0u); /* HCLK=RUN=132 MHz. */
    if (!expect_clocks(s, 132000000u, 132000000u, 132000000u) ||
        !expect_phase(s, &g, 1u, "divider write preserves HCLK phase")) return 0;
    s3c2400_write32(s, MPLLCON_ADDR, 0x2000u); /* HCLK=RUN=60 MHz. */
    if (!expect_clocks(s, 60000000u, 60000000u, 60000000u) ||
        !expect_phase(s, &g, 1u, "PLL write preserves HCLK phase")) return 0;
    s3c2400_tick(s, 11u); /* Now 99/8 HCLK, including the original fraction. */
    s3c2400_write32(s, MPLLCON_ADDR, 0xe000u);
    if (!expect_phase(s, &g, 12u, "PLL return preserves HCLK phase")) return 0;
    s3c2400_write32(s, CLKDIVN_ADDR, 2u);
    if (!expect_clocks(s, 132000000u, 66000000u, 48000000u) ||
        !expect_phase(s, &g, 12u, "divider return preserves HCLK phase")) return 0;
    /* Exact remaining RUN to HSTATUS sync->back = ceil((312*8-99)/11)
     * = 218. Discarding the 3/8 fraction delays this edge by one cycle. */
    uint64_t edge = g.pixel * g.hs;
    uint32_t remaining = (uint32_t)ceil_ratio(edge * 8u - 99u, 11u);
    s3c2400_tick(s, remaining - 1u);
    if (!expect_phase(s, &g, (99u + (remaining - 1u) * 11u) / 8u,
                      "before clock-preserved fractional edge")) return 0;
    s3c2400_tick(s, 1u);
    if (!expect_phase(s, &g, (99u + remaining * 11u) / 8u,
                      "at clock-preserved fractional edge")) return 0;

    /* Also switch rates during active video at an exactly integral HCLK
     * phase, and observe the next active LINECNT reload after the switch. */
    if (!configure(s, g.cv, 1)) return 0;
    uint64_t prefix = ((g.active_end - g.line / 2u) / 11u) * 8u;
    uint64_t phase = prefix * 11u / 8u;
    s3c2400_tick(s, (uint32_t)prefix);
    if (!expect_phase(s, &g, phase, "active prefix before clock change")) return 0;
    s3c2400_write32(s, CLKDIVN_ADDR, 0u);
    if (!expect_phase(s, &g, phase, "active divider preserves scan position")) return 0;
    s3c2400_write32(s, MPLLCON_ADDR, 0x2000u);
    if (!expect_clocks(s, 60000000u, 60000000u, 60000000u) ||
        !expect_phase(s, &g, phase, "active PLL preserves scan position")) return 0;
    uint32_t last_active = linecnt(s);
    uint64_t next = g.frame + g.active_begin;
    s3c2400_tick(s, (uint32_t)(next - phase - 1u));
    if (!expect_phase(s, &g, next - 1u, "before clock-changed active reload")) return 0;
    s3c2400_tick(s, 1u);
    if (!expect_phase(s, &g, next, "at clock-changed active reload") ||
        linecnt(s) <= last_active) return 0;
    puts("PASS: clock writes preserve integral and fractional HCLK scan phase");
    return 1;
}

/* BIOS-style VSTATUS wait: LDR LCDCON5, isolate VSTATUS, spin while the panel
 * is in active video. The status word is run-invariant (the scan phase only
 * advances after arm920t_run returned) and the read itself bounds the run at
 * the next status edge, so a proven side-effect-free repetition may retire the
 * iterations up to that edge. The reference bus withholds the stable-read
 * certificate and must execute every iteration with identical r[0..15], CPSR
 * and consumed cycles at every run boundary. */
static int check_vstatus_poll_fast_forward(void) {
    static const uint32_t code[] = {
        0xe59c2010u, /* LDR r2,[r12,#16]  LCDCON5 VSTATUS/HSTATUS */
        0xe1a029a2u, /* MOV r2,r2,LSR #19  VSTATUS */
        0xe3520002u, /* CMP r2,#2 */
        0x0afffffbu, /* BEQ start         spin while VSTATUS is active */
        0xe3a04001u, /* MOV r4,#1         left active video */
        0xeafffffeu,
    };
    lcd_geometry_t g = geometry(5u);
    s3c2400_t *fast = s3c2400_create(2u * 1024u * 1024u);
    s3c2400_t *ref = s3c2400_create(2u * 1024u * 1024u);
    if (!fast || !ref) {
        fprintf(stderr, "FAIL: VSTATUS poll SoC allocation\n");
        s3c2400_destroy(fast); s3c2400_destroy(ref);
        return 0;
    }
    arm_bus_t bus_fast = s3c2400_get_bus(fast);
    arm_bus_t bus_ref = s3c2400_get_bus(ref);
    bus_ref.is_stable_read32 = NULL;
    arm920t_t *cpu_fast = arm920t_create(&bus_fast);
    arm920t_t *cpu_ref = arm920t_create(&bus_ref);
    int ok = cpu_fast != NULL && cpu_ref != NULL;
    if (ok) {
        s3c2400_set_irq_sink(fast, cpu_fast);
        s3c2400_set_irq_sink(ref, cpu_ref);
    }
    /* Three lines into active video: VSTATUS is 2, so the loop spins until the
     * status edge that ends the active region, then parks on MOV/B self. */
    const uint64_t start = g.active_begin + 3u * g.line;
    for (unsigned jit = 0; jit < 2u && ok; ++jit) {
        if (!configure(fast, g.cv, 0) || !configure(ref, g.cv, 0)) { ok = 0; break; }
        arm920t_reset(cpu_fast, 0x0c000000u);
        arm920t_reset(cpu_ref, 0x0c000000u);
        arm920t_set_jit(cpu_fast, (int)jit);
        arm920t_set_jit(cpu_ref, (int)jit);
        for (unsigned i = 0; i < GP32_ARRAY_COUNT(code); ++i) {
            s3c2400_write32(fast, 0x0c000000u + i * 4u, code[i]);
            s3c2400_write32(ref, 0x0c000000u + i * 4u, code[i]);
        }
        arm920t_set_reg(cpu_fast, 12, LCDCON1_ADDR);
        arm920t_set_reg(cpu_ref, 12, LCDCON1_ADDR);
        uint64_t elapsed_fast = 0, elapsed_ref = 0;
        tick_to(fast, &elapsed_fast, start);
        tick_to(ref, &elapsed_ref, start);
        uint32_t cycles = 0;
        /* One active region is ~240 lines; a deadline-truncated run covers
         * less than a line, so allow well over one chunk per line. */
        for (unsigned step = 0; step < 4096u; ++step) {
            uint32_t done_fast = s3c2400_run_cpu(fast, 2048u);
            uint32_t done_ref = s3c2400_run_cpu(ref, 2048u);
            if (done_fast != done_ref) {
                fprintf(stderr, "FAIL: VSTATUS poll run jit=%u step=%u fast=%u ref=%u\n",
                        jit, step, done_fast, done_ref);
                ok = 0;
                break;
            }
            cycles += done_fast;
            for (unsigned reg = 0; reg < 16u && ok; ++reg)
                if (arm920t_get_reg(cpu_fast, reg) != arm920t_get_reg(cpu_ref, reg)) {
                    fprintf(stderr, "FAIL: VSTATUS poll r%u jit=%u step=%u fast=%08x ref=%08x\n",
                            reg, jit, step, arm920t_get_reg(cpu_fast, reg),
                            arm920t_get_reg(cpu_ref, reg));
                    ok = 0;
                }
            if (ok && arm920t_get_cpsr(cpu_fast) != arm920t_get_cpsr(cpu_ref)) {
                fprintf(stderr, "FAIL: VSTATUS poll CPSR jit=%u step=%u\n", jit, step);
                ok = 0;
            }
            if (ok && arm920t_get_reg(cpu_fast, 4) == 1u) break;
        }
        if (ok && arm920t_get_reg(cpu_fast, 4) != 1u) {
            fprintf(stderr, "FAIL: VSTATUS poll never left active video jit=%u cycles=%u\n",
                    jit, cycles);
            ok = 0;
        }
        /* The wait spans most of active video; an early exit would mean the
         * status edge was missed, an infinite one would mean it never came. */
        if (ok && (cycles < (uint32_t)(100u * g.line) ||
                   cycles > (uint32_t)(g.va * g.line))) {
            fprintf(stderr, "FAIL: VSTATUS poll cycles=%u outside one active region\n", cycles);
            ok = 0;
        }
        /* Only the certified bus may omit the spin: the reference executes
         * every iteration. The wait spans over 100 lines, so a certified
         * fast-forward must retire far more than the trailing self loop. */
        gp32_cpu_profile_t pf, pr;
        arm920t_get_cpu_profile(cpu_fast, &pf);
        arm920t_get_cpu_profile(cpu_ref, &pr);
        if (pf.supported && pf.poll_skipped_insns < 200000u) {
            fprintf(stderr, "FAIL: VSTATUS poll did not fast-forward jit=%u "
                    "skipped=%" PRIu64 " reference=%" PRIu64 "\n",
                    jit, pf.poll_skipped_insns, pr.poll_skipped_insns);
            ok = 0;
        }
    }
    if (ok) puts("PASS: interpreter/JIT VSTATUS poll fast-forward is equivalent");
    s3c2400_set_irq_sink(fast, NULL);
    s3c2400_set_irq_sink(ref, NULL);
    arm920t_destroy(cpu_fast);
    arm920t_destroy(cpu_ref);
    s3c2400_destroy(fast);
    s3c2400_destroy(ref);
    return ok;
}

static int check_cpu_status_poll(s3c2400_t *s) {
    /* Read LINECNT first, then wait for horizontal active video. The second
     * read must narrow the pending line deadline to the earlier status edge. */
    const uint32_t code[] = {
        0xe5905000u, /* LDR r5,[r0]: LINECNT */
        0xe5902010u, /* LDR r2,[r0,#16]: LCDCON5 */
        0xe0022003u, /* AND r2,r2,r3 */
        0xe1520001u, /* CMP r2,r1 */
        0x1afffffau, /* BNE start */
        0xe3a04001u, /* MOV r4,#1 */
        0xeafffffeu,
    };
    arm_bus_t bus = s3c2400_get_bus(s);
    arm920t_t *cpu = arm920t_create(&bus);
    if (!cpu) return 0;
    s3c2400_set_irq_sink(s, cpu);
    int ok = 1;
    for (unsigned jit = 0; jit < 2u && ok; ++jit) {
        if (!configure(s, 5u, 0)) { ok = 0; break; }
        arm920t_reset(cpu, 0x0c000000u);
        arm920t_set_jit(cpu, (int)jit);
        for (unsigned i = 0; i < GP32_ARRAY_COUNT(code); ++i)
            s3c2400_write32(s, 0x0c000000u + i * 4u, code[i]);
        arm920t_set_reg(cpu, 0, LCDCON1_ADDR);
        arm920t_set_reg(cpu, 1, 2u << 17);
        arm920t_set_reg(cpu, 3, 3u << 17);
        uint32_t first = s3c2400_run_cpu(s, 32768u);
        /* HSync is 26 pixels * 12 HCLK. Allow one instruction of overshoot;
         * ending at the 3408-cycle line boundary misses this observation. */
        if (first < 312u || first > 328u) {
            fprintf(stderr, "FAIL: mixed LCD deadline jit=%u first=%u\n", jit, first);
            ok = 0; break;
        }
        uint32_t elapsed = first;
        while (elapsed < 1000u && !arm920t_get_reg(cpu, 4)) {
            uint32_t done = s3c2400_run_cpu(s, 1000u - elapsed);
            if (!done || done > 1000u - elapsed) { ok = 0; break; }
            elapsed += done;
        }
        if (arm920t_get_reg(cpu, 4) != 1u) {
            fprintf(stderr, "FAIL: TFT active-status poll jit=%u\n", jit);
            ok = 0;
        }
    }
    s3c2400_set_irq_sink(s, NULL);
    arm920t_destroy(cpu);
    if (ok) puts("PASS: interpreter/JIT mixed LINECNT and HSTATUS polling");
    return ok;
}

static int check_long_slice(s3c2400_t *s) {
    lcd_geometry_t g = geometry(5u);
    for (unsigned divided = 0; divided < 2u; ++divided) {
        if (!configure(s, g.cv, divided)) return 0;
        s3c2400_tick(s, UINT32_MAX);
        uint64_t hclk = divided ? (uint64_t)UINT32_MAX * 11u / 8u : UINT32_MAX;
        if (!expect_phase(s, &g, hclk, "multi-frame maximum cycle slice")) return 0;
        s3c2400_tick(s, 1u);
        hclk = divided ? ((uint64_t)UINT32_MAX + 1u) * 11u / 8u : (uint64_t)UINT32_MAX + 1u;
        if (!expect_phase(s, &g, hclk, "fraction after maximum cycle slice")) return 0;
    }
    return 1;
}

int main(void) {
    s3c2400_t *a = s3c2400_create(8u * 1024u * 1024u);
    s3c2400_t *b = s3c2400_create(8u * 1024u * 1024u);
    s3c2400_t *c = s3c2400_create(8u * 1024u * 1024u);
    if (!a || !b || !c) {
        s3c2400_destroy(a); s3c2400_destroy(b); s3c2400_destroy(c);
        fputs("FAIL: allocate LCD test SoCs\n", stderr);
        return 2;
    }
    int ok = check_period(a, 5u);
    ok = check_period(a, 11u) && ok;
    ok = check_status_edges(a) && ok;
    ok = check_fractional_state(a, b, c) && ok;
    ok = check_clock_phase(a) && ok;
    ok = check_vstatus_poll_fast_forward() && ok;
    ok = check_cpu_status_poll(a) && ok;
    ok = check_long_slice(a) && ok;
    s3c2400_destroy(a); s3c2400_destroy(b); s3c2400_destroy(c);
    return ok ? 0 : 1;
}
