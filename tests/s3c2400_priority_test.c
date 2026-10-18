/* Samsung S3C2400 manual pp.14-3--14-5, 14-12--14-13:
 * https://datasheets.chipdb.org/Samsung/S3C2400.pdf
 * Inject source requests inside the SoC to cover interrupt producers that
 * the emulator does not yet implement. PRIORITY is write-only in hardware;
 * internal state inspection here tests arbitration/serialization, not a
 * promise about hardware register readback. */
#include "../src/s3c2400.c"
#include <stdio.h>

static unsigned failures, cases;

/* Independent, literal manual permutations and interrupt wiring. Unwired
 * endpoint inputs and the reserved source 16 never enter a test request. */
static const unsigned order[4][6] = {
    {0, 1, 2, 3, 4, 5}, {0, 2, 3, 4, 1, 5},
    {0, 3, 4, 1, 2, 5}, {0, 4, 1, 2, 3, 5}
};
static const int source[6][6] = {
    {-1, 0, 1, 2, 3, -1}, {4, 5, 6, 7, 8, 9},
    {10, 11, 12, 13, 14, 15}, {-1, 17, 18, 19, 20, 21},
    {22, 23, 24, 25, 26, 27}, {-1, 28, 29, 30, 31, -1}
};
static const unsigned representative[6] = {0, 4, 10, 17, 22, 28};

static void expect(int ok, const char *where, unsigned arb, unsigned sel,
                    unsigned mode, unsigned mask, unsigned step) {
    if (ok) return;
    if (failures < 16u)
        fprintf(stderr, "FAIL: %s arb=%u sel=%u mode=%u mask=%02x step=%u\n",
                where, arb, sel, mode, mask, step);
    ++failures;
}

static unsigned winner(unsigned inputs, unsigned selection) {
    for (unsigned i = 0; i < 6u; ++i)
        if (inputs & (1u << order[selection][i])) return order[selection][i];
    return 6u;
}

static void prepare(s3c2400_t *s, uint32_t pending, uint32_t priority,
                     uint32_t masked, uint32_t fiq) {
    memset(s->irq, 0, sizeof(s->irq));
    s->irq[2] = UINT32_MAX;
    s3c2400_write32(s, 0x1440000cu, priority);
    s3c2400_write32(s, 0x14400004u, fiq);
    s->irq[0] = pending;
    s3c2400_write32(s, 0x14400008u, masked);
}

static void arbiter_cases(s3c2400_t *s) {
    for (unsigned arb = 0; arb < 7u; ++arb) {
        unsigned connected = 63u;
        if (arb < 6u) {
            connected = 0u;
            for (unsigned req = 0; req < 6u; ++req)
                if (source[arb][req] >= 0) connected |= 1u << req;
        }
        for (unsigned sel = 0; sel < 4u; ++sel)
            for (unsigned mode = 0; mode < 2u; ++mode)
                for (unsigned mask = 1u; mask < 64u; ++mask) {
                    if (mask & ~connected) continue;
                    uint32_t pending = 0u;
                    for (unsigned req = 0; req < 6u; ++req) if (mask & (1u << req))
                        pending |= 1u << (arb == 6u ? representative[req] : (unsigned)source[arb][req]);
                    const unsigned shift = 7u + arb * 2u;
                    uint32_t priority = (sel << shift) | (mode << arb);
                    unsigned selection = sel;
                    prepare(s, pending, priority, 0u, 0u);
                    ++cases;
                    for (unsigned step = 0; step < 6u; ++step) {
                        unsigned req = winner(mask, selection);
                        unsigned irq = arb == 6u ? representative[req] : (unsigned)source[arb][req];
                        if (mode && req >= 1u && req <= 4u) selection = req % 4u;
                        priority = (priority & ~(3u << shift)) | (selection << shift);
                        expect(s3c2400_read32(s, 0x14400010u) == (1u << irq) &&
                               s3c2400_read32(s, 0x14400014u) == irq,
                               "selected source", arb, sel, mode, mask, step);
                        expect(s->irq[3] == priority, "rotation state", arb, sel, mode, mask, step);
                        /* Rechecking a held selection does not service it
                         * again. A still-asserted source may be selected
                         * again only after acknowledging INTPND. */
                        check_irq(s);
                        expect(s->irq[3] == priority, "held selection rotated twice", arb, sel, mode, mask, step);
                        s3c2400_write32(s, 0x14400010u, UINT32_MAX);
                    }
                }
    }
}

static void boundary_cases(s3c2400_t *s) {
    const uint32_t timer1 = 1u << 11, timer2 = 1u << 12, timer4 = 1u << 14;
    const uint32_t timer_mode = 1u << 2;
    ++cases;
    prepare(s, timer1 | timer2 | timer4, timer_mode, UINT32_MAX, 0u);
    expect(s->irq[4] == 0u && s->irq[3] == timer_mode,
           "masked sources do not arbitrate", 2u, 0u, 1u, 0x16u, 0u);
    s3c2400_write32(s, 0x14400008u, 0u);
    uint32_t rotated = timer_mode | (1u << 11);
    expect(s->irq[4] == timer1 && s->irq[3] == rotated,
           "unmask selects and rotates once", 2u, 0u, 1u, 0x16u, 0u);
    s3c2400_write32(s, 0x14400000u, timer1);
    s3c2400_write32(s, 0x14400010u, timer2);
    s3c2400_write32(s, 0x14400008u, UINT32_MAX);
    expect(s->irq[4] == timer1 && s->irq[5] == 11u && s->irq[3] == rotated,
           "source clear, unrelated ack and mask retain selection", 2u, 0u, 1u, 0x16u, 1u);
    FILE *state = tmpfile();
    int ok = state != NULL;
    if (state) {
        ok = s3c2400_state_save(s, state);
        rewind(state);
        ok = ok && s3c2400_state_load(s, state);
        fclose(state);
    }
    expect(ok && s->irq[4] == timer1 && s->irq[3] == rotated,
           "restore retains selected source and rotation", 2u, 0u, 1u, 0x16u, 1u);

    /* Rotation belongs to accepting the source into INTPND. A guest's later
     * explicit priority setting applies to the next arbitration; acknowledging
     * the held source must not overwrite that setting with an extra rotation. */
    s3c2400_write32(s, 0x1440000cu, timer_mode | (3u << 11));
    s3c2400_write32(s, 0x14400008u, 0u);
    expect(s->irq[4] == timer1, "priority write retains selected source", 2u, 3u, 1u, 0x14u, 2u);
    s3c2400_write32(s, 0x14400010u, timer1);
    expect(s->irq[4] == timer4 && s->irq[3] == timer_mode,
           "explicit priority survives prior acknowledgement", 2u, 3u, 1u, 0x14u, 3u);
    s3c2400_write32(s, 0x14400000u, timer4);
    s3c2400_write32(s, 0x14400010u, timer4);
    expect(s->irq[4] == timer2 && s->irq[3] == (timer_mode | (2u << 11)),
           "remaining source follows new rotation", 2u, 0u, 1u, 0x4u, 4u);
    s3c2400_write32(s, 0x14400000u, timer2);
    s3c2400_write32(s, 0x14400010u, timer2);
    expect(s->irq[4] == 0u && s->irq[5] == 0u,
           "empty controller clears offset", 2u, 2u, 1u, 0u, 5u);

    ++cases;
    prepare(s, timer1, 0x7fu, 0u, timer1);
    expect(s->irq[4] == 0u && s->irq[3] == 0x7fu,
           "FIQ bypasses all arbiters", 6u, 0u, 1u, 0u, 0u);
    request_irq(s, 12u);
    const uint32_t fiq_priority = 0x7fu | (2u << 11) | (2u << 19);
    expect(s->irq[4] == timer2 && s->irq[3] == fiq_priority,
           "only IRQ winner rotates both levels", 6u, 0u, 1u, 4u, 1u);
    request_irq(s, 11u);
    expect(s->irq[4] == timer2 && s->irq[3] == fiq_priority,
           "FIQ arrival preserves held IRQ rotation", 6u, 2u, 1u, 4u, 2u);

    ++cases;
    prepare(s, timer1 | (1u << 17) | (1u << 23), 0x7fu | (1u << 19), 0u, 0u);
    const uint32_t first_path = 0x7fu | (1u << 11) | (2u << 19);
    expect(s->irq[4] == timer1 && s->irq[3] == first_path,
           "losing groups do not rotate", 6u, 1u, 1u, 0x1cu, 0u);
    s3c2400_write32(s, 0x14400000u, timer1);
    s3c2400_write32(s, 0x14400010u, timer1);
    const uint32_t second_path = 0x7fu | (1u << 11) | (1u << 13) | (3u << 19);
    expect(s->irq[4] == (1u << 17) && s->irq[3] == second_path,
           "next group rotates only its winning path", 6u, 2u, 1u, 0x18u, 1u);

    ++cases;
    prepare(s, timer1 | timer2, 0u, 0u, 0u); /* Legacy fixed priority value. */
    expect(s->irq[4] == timer1 && s->irq[3] == 0u,
           "explicit legacy fixed priority remains fixed", 2u, 0u, 0u, 6u, 0u);
    s3c2400_reset(s);
    expect(s->irq[3] == 0x7fu, "hardware reset enables all rotation modes", 6u, 0u, 1u, 0u, 0u);
}

int main(void) {
    s3c2400_t *s = s3c2400_create(4096u);
    if (!s) return 2;
    arm_bus_t bus = s3c2400_get_bus(s);
    arm920t_t *cpu = arm920t_create(&bus);
    if (!cpu) { s3c2400_destroy(s); return 2; }
    s3c2400_set_irq_sink(s, cpu);
    arbiter_cases(s);
    boundary_cases(s);
    arm920t_destroy(cpu);
    s3c2400_destroy(s);
    printf("IRQ priority scenarios=%u assertion_failures=%u\n", cases, failures);
    return failures ? 1 : 0;
}
