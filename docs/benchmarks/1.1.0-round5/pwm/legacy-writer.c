/* Compile with the exact pre-change s3c2400.c/header, not the new model.
 * Emits a real GP32SOC9 image for the checked-in sparse legacy fixture. */
#include "s3c2400.h"
#include <stdio.h>

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    s3c2400_t *s = s3c2400_create(4096u);
    if (!s) return 2;
    s3c2400_write32(s, 0x14800004u, 0u);
    s3c2400_write32(s, 0x14800014u, 0u);
    for (unsigned t = 0; t < 5u; ++t)
        s3c2400_write32(s, 0x1510000cu + t * 12u, t == 4u ? 9u : 100u + 17u*t);
    s3c2400_write32(s, 0x15100008u, 0x599909u);
    s3c2400_tick(s, 31u);
    s3c2400_write32(s, 0x1510000cu, 200u);
    s3c2400_write32(s, 0x15100024u, 0u);
    s3c2400_write32(s, 0x15100008u, 0x599909u & ~0x10100u);
    for (unsigned t = 0; t < 5u; ++t)
        printf("timer%u old_visible=%u\n", t, s3c2400_read32(s,
               t == 4u ? 0x15100040u : 0x15100014u + t*12u));
    FILE *f = fopen(argv[1], "wb");
    int ok = f && s3c2400_state_save(s, f);
    if (f && fclose(f)) ok = 0;
    s3c2400_destroy(s);
    return ok ? 0 : 1;
}
