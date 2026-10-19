#include "s3c2400.h"
#include <stdio.h>
#include <string.h>

static void report(s3c2400_t *s, const char *where) {
    printf("%s: SRCPND=%08x INTPND=%08x INTOFFSET=%u\n", where,
           s3c2400_read32(s, 0x14400000u), s3c2400_read32(s, 0x14400010u),
           s3c2400_read32(s, 0x14400014u));
}
int main(int argc, char **argv) {
    if (argc != 3) return 2;
    s3c2400_t *s = s3c2400_create(4096u);
    arm_bus_t bus = s3c2400_get_bus(s);
    arm920t_t *c = arm920t_create(&bus);
    s3c2400_set_irq_sink(s, c);
    int ok = 1;
    if (!strcmp(argv[1], "write")) {
        s3c2400_write32(s, 0x14800004u, 0u);
        s3c2400_write32(s, 0x14800014u, 0u);
        s3c2400_write32(s, 0x14400008u, ~((1u << 10) | (1u << 11)));
        s3c2400_write32(s, 0x15100018u, 9u);
        s3c2400_write32(s, 0x15100008u, 0x100u);
        s3c2400_tick(s, 20u);
        s3c2400_write32(s, 0x1510000cu, 9u);
        s3c2400_write32(s, 0x15100008u, 1u);
        s3c2400_tick(s, 20u);
        report(s, "legacy saved selection");
        FILE *f = fopen(argv[2], "wb");
        ok = f && s3c2400_state_save(s, f);
        if (f) fclose(f);
    } else {
        FILE *f = fopen(argv[2], "rb");
        ok = f && s3c2400_state_load(s, f);
        if (f) fclose(f);
        if (!ok) return 2;
        report(s, "fixed core restored selection");
        for (unsigned i = 0; i < 3; ++i) {
            uint32_t selected = s3c2400_read32(s, 0x14400014u);
            printf("ISR acknowledges source %u\n", selected);
            s3c2400_write32(s, 0x14400000u, 1u << selected);
            s3c2400_write32(s, 0x14400010u, 1u << selected);
            report(s, "after acknowledgement");
        }
        ok = s3c2400_read32(s, 0x14400010u) == 0u &&
             s3c2400_read32(s, 0x14400000u) == 0u;
    }
    arm920t_destroy(c); s3c2400_destroy(s);
    return ok ? 0 : 1;
}
