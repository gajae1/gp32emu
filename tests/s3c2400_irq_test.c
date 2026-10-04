/* Peripheral routing must select the ARM exception bank, not just its vector.
 * S3C2400 manual ch.14: FIQ bypasses INTPND/INTOFFSET arbitration. */
#include "s3c2400.h"
#include <stdio.h>

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

static unsigned enter(arm920t_t *c, uint32_t masks) {
    arm920t_set_cpsr(c, 0x13u | masks);
    arm920t_set_reg(c, 15u, 0x0c000000u);
    CHECK(arm920t_run(c, 1u) == 1u);
    return arm920t_get_cpsr(c) & 31u;
}

static void check_routing(int jit) {
    s3c2400_t *s = s3c2400_create(8u*1024u*1024u);
    CHECK(s != NULL); if (!s) return;
    arm_bus_t bus = s3c2400_get_bus(s);
    arm920t_t *c = arm920t_create(&bus);
    CHECK(c != NULL); if (!c) { s3c2400_destroy(s); return; }
    s3c2400_set_irq_sink(s, c);
    arm920t_set_jit(c, jit);
    uint8_t bios[32] = {0}; char err[128];
    gp32_st32le(bios + 0x18u, 0xe3a06012u); /* IRQ marker */
    gp32_st32le(bios + 0x1cu, 0xe3a07011u); /* FIQ marker */
    CHECK(s3c2400_load_bios_buffer(s, bios, sizeof(bios), err, sizeof(err)));
    s3c2400_write32(s, 0x0c000000u, 0xeafffffeu);
    s3c2400_write32(s, 0x14800004u, 0u);
    s3c2400_write32(s, 0x14800014u, 0u);
    const uint32_t fiq = 1u << 10, irq = 1u << 11;
    s3c2400_write32(s, 0x14400004u, fiq);
    s3c2400_write32(s, 0x14400008u, UINT32_MAX);
    s3c2400_write32(s, 0x1510000cu, 9u);
    s3c2400_write32(s, 0x15100008u, 1u);
    s3c2400_tick(s, 20u);
    CHECK(s3c2400_read32(s, 0x14400000u) == fiq);
    CHECK(enter(c, 0) == 0x13u); /* Controller mask retains pending source. */
    s3c2400_write32(s, 0x14400008u, ~fiq);
    CHECK(s3c2400_read32(s, 0x14400010u) == 0u);
    CHECK(s3c2400_read32(s, 0x14400014u) == 0u);
    CHECK(enter(c, 0x40u) == 0x13u); /* CPSR.F masks only FIQ. */
    CHECK(enter(c, 0x80u) == 0x11u); /* CPSR.I does not mask FIQ. */
    CHECK(arm920t_get_pc(c) == 0x20u && arm920t_get_reg(c, 7u) == 0x11u);

    /* Pending source changes route immediately; the old CPU line must drop. */
    s3c2400_write32(s, 0x14400004u, 0u);
    CHECK(enter(c, 0u) == 0x12u);
    CHECK(arm920t_get_pc(c) == 0x1cu && arm920t_get_reg(c, 6u) == 0x12u);
    CHECK(s3c2400_read32(s, 0x14400010u) == fiq);
    s3c2400_write32(s, 0x14400000u, fiq);
    s3c2400_write32(s, 0x14400010u, fiq);
    CHECK(enter(c, 0u) == 0x13u);

    /* Simultaneous requests retain the IRQ's arbitration registers while
     * FIQ takes CPU priority. Acknowledge FIQ without losing the IRQ. */
    s3c2400_write32(s, 0x14400004u, fiq);
    s3c2400_write32(s, 0x14400008u, ~(fiq | irq));
    s3c2400_write32(s, 0x15100018u, 9u);
    s3c2400_write32(s, 0x15100008u, 0x101u);
    s3c2400_tick(s, 20u);
    CHECK(s3c2400_read32(s, 0x14400010u) == irq);
    CHECK(s3c2400_read32(s, 0x14400014u) == 11u);
    CHECK(enter(c, 0u) == 0x11u);
    CHECK(enter(c, 0x40u) == 0x12u);
    s3c2400_write32(s, 0x14400008u, ~irq);
    CHECK(enter(c, 0u) == 0x12u);
    s3c2400_write32(s, 0x14400008u, ~(fiq | irq));
    s3c2400_write32(s, 0x14400000u, fiq);
    CHECK(enter(c, 0u) == 0x12u);
    s3c2400_reset(s);
    s3c2400_write32(s, 0x0c000000u, 0xeafffffeu);
    CHECK(enter(c, 0u) == 0x13u);
    arm920t_destroy(c); s3c2400_destroy(s);
}

int main(void) {
    check_routing(0); check_routing(1);
    if (failures) return 1;
    puts("PASS: peripheral IRQ/FIQ routing, masks, priority and acknowledgement");
    return 0;
}
