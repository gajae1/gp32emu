/* A one-shot IIS DMA handler must refill before later sample periods, even
 * when the host asks for a large CPU slice. Compare to one-cycle scheduling. */
#include "s3c2400.h"
#include <stdio.h>
#include <string.h>

typedef struct result { uint32_t regs[16], cpsr; uint64_t frames; int16_t pcm[128]; } result_t;
typedef struct scenario { uint32_t dcon, prescaler, phase, cycles, frames, irqs; } scenario_t;

static int run(unsigned quantum, int jit, const scenario_t *v, result_t *out) {
    s3c2400_t *s = s3c2400_create(8u * 1024u * 1024u);
    if (!s) return 0;
    arm_bus_t bus = s3c2400_get_bus(s);
    arm920t_t *c = arm920t_create(&bus);
    if (!c) { s3c2400_destroy(s); return 0; }
    s3c2400_set_irq_sink(s, c);
    arm920t_set_jit(c, jit);
    uint8_t bios[32] = {0};
    gp32_st32le(bios + 0x18u, 0xe51ff004u); /* ldr pc,[pc,#-4] */
    gp32_st32le(bios + 0x1cu, 0x0c001000u);
    char error[128];
    int ok = s3c2400_load_bios_buffer(s, bios, sizeof(bios), error, sizeof(error));
    if (!ok) { fprintf(stderr, "%s\n", error); goto end; }
    s3c2400_write32(s, 0x0c000000u, 0xeafffffeu);
    const uint32_t handler[] = {
        0xe5801018u, /* str r1,[r0,#24]: restart DMA2 */
        0xe5823000u, /* str r3,[r2]: acknowledge SRCPND */
        0xe5823010u, /* str r3,[r2,#16]: acknowledge INTPND */
        0xe2855001u, /* add r5,r5,#1 */
        0xe25ef004u, /* subs pc,lr,#4 */
    };
    for (unsigned i = 0; i < GP32_ARRAY_COUNT(handler); ++i)
        s3c2400_write32(s, 0x0c001000u + 4u*i, handler[i]);
    for (unsigned i = 0; i < 16u; ++i)
        s3c2400_write16(s, 0x0c002000u + 2u*i, (uint16_t)(100u + i));
    s3c2400_write32(s, 0x14800004u, 0u); /* RUN/PCLK 48 MHz */
    s3c2400_write32(s, 0x14800014u, 0u);
    s3c2400_write32(s, 0x14600040u, 0x0c002000u);
    s3c2400_write32(s, 0x14600044u, 0x35508010u);
    s3c2400_write32(s, 0x14600048u, v->dcon);
    s3c2400_write32(s, 0x14600058u, 2u);
    s3c2400_write32(s, 0x15508008u, v->prescaler);
    s3c2400_write32(s, 0x15508000u, 1u); /* model rate ceiling 96 kHz */
    s3c2400_tick(s, v->phase);
    s3c2400_write32(s, 0x14400008u, ~(1u << 19));
    arm920t_set_cpsr(c, 0x53u);
    arm920t_set_reg(c, 0u, 0x14600040u);
    arm920t_set_reg(c, 1u, 2u);
    arm920t_set_reg(c, 2u, 0x14400000u);
    arm920t_set_reg(c, 3u, 1u << 19);
    arm920t_set_reg(c, 15u, 0x0c000000u);
    for (unsigned remaining = v->cycles; remaining;) {
        unsigned step = remaining < quantum ? remaining : quantum;
        uint32_t done = s3c2400_run_cpu(s, step);
        if (!done || done > step) { ok = 0; goto end; }
        remaining -= done;
    }
    for (unsigned i = 0; i < 16u; ++i) out->regs[i] = arm920t_get_reg(c, i);
    out->cpsr = arm920t_get_cpsr(c);
    const int16_t *pcm = s3c2400_audio_samples(s, &out->frames, NULL);
    if (out->frames > GP32_ARRAY_COUNT(out->pcm) / 2u) { ok = 0; goto end; }
    memcpy(out->pcm, pcm, (size_t)out->frames * 2u * sizeof(*pcm));
end:
    arm920t_destroy(c); s3c2400_destroy(s); return ok;
}

int main(void) {
    const scenario_t cases[] = {
        {0x10d00008u, 0u, 0u, 5000u, 10u, 2u}, /* halfword, one-shot */
        {0x10e00008u, 0u, 0u, 5000u, 10u, 1u}, /* word, one-shot */
        {0x10d00009u, 6u << 5, 123u, 18000u, 9u, 2u}, /* fractional period, odd count */
        {0x14d00008u, 0u, 0u, 5000u, 40u, 9u}, /* whole-service mode */
        {0x10900009u, 0u, 0u, 5000u, 10u, 2u}, /* auto-reload: IRQs after 9 and 18 units */
    };
    for (int jit = 0; jit <= 1; ++jit) {
      for (unsigned i = 0; i < GP32_ARRAY_COUNT(cases); ++i) {
        const scenario_t *v = &cases[i];
        result_t fine = {0}, coarse = {0};
        if (!run(1u, jit, v, &fine) || !run(32768u, jit, v, &coarse)) return 2;
        if (fine.frames != v->frames || fine.regs[5] != v->irqs || memcmp(&fine, &coarse, sizeof(fine))) {
            fprintf(stderr, "FAIL jit=%d case=%u: fine/coarse frames=%llu/%llu IRQs=%u/%u\n",
                    jit, i, (unsigned long long)fine.frames, (unsigned long long)coarse.frames,
                    fine.regs[5], coarse.regs[5]);
            return 1;
        }
      }
    }
    puts("PASS: IIS DMA refill IRQ independent of CPU slice size");
    return 0;
}
