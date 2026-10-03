/* Synthetic compatibility fixtures for the existing Blue Angelo shadow repair.
 * Include the core to exercise its private repair entry points without a ROM.
 * GP32_SOURCE can select a saved pre-change source for the same fixtures. */
#ifndef GP32_SOURCE
#define GP32_SOURCE "../src/gp32.c"
#endif
#include GP32_SOURCE

static int failures;
#define CHECK(c, msg) do { if (!(c)) { fprintf(stderr, "FAIL: %s (%d)\n", msg, __LINE__); ++failures; } } while (0)
#define LUT (GP32_RAM_BASE + 0x747200u)
#define FB (GP32_RAM_BASE + 0x1000u)

static void seed_luts(gp32_t *g, int paired) {
    for (uint32_t i = 0; i < 256u; ++i) {
        s3c2400_write8(g->soc, LUT - 0x130u + i, paired ? (uint8_t)(0x36u + i % 10u) : 0u);
        s3c2400_write8(g->soc, LUT + i, (uint8_t)(0x71u + i % 7u));
    }
    s3c2400_write8(g->soc, LUT + 255u, 0x78u);
}

static void surface(gp32_t *g, uint32_t addr, uint32_t w, uint32_t h, uint32_t mode) {
    s3c2400_write32(g->soc, 0x14a00004u, (h - 1u) << 14);
    s3c2400_write32(g->soc, 0x14a00008u, (w - 1u) << 8);
    s3c2400_write32(g->soc, 0x14a00014u, addr >> 1);
    s3c2400_write32(g->soc, 0x14a00000u, (mode << 1) | 1u);
}

static void pixels(gp32_t *g, uint32_t addr, const uint8_t *values, unsigned n) {
    for (unsigned i = 0; i < n; ++i) s3c2400_write8(g->soc, addr + i, values[i]);
}

static void check_pixels(gp32_t *g, uint32_t addr, const uint8_t *values, unsigned n) {
    for (unsigned i = 0; i < n; ++i) CHECK(s3c2400_read8(g->soc, addr + i) == values[i], "expected pixel");
}

int main(void) {
    gp32_t *g = gp32_create(NULL);
    if (!g) return 2;
    const uint8_t isolated[] = {0x71, 0x78, 0, 0x78};
    const uint8_t repaired[] = {0x71, 0x77, 0, 0x78};
    seed_luts(g, 0);
    surface(g, FB, 4, 1, 0x0b);
    pixels(g, FB, isolated, sizeof(isolated));
    direct_fix_gp32_additive_blend_shadow_endpoint(g);
    direct_fix_gp32_additive_blend_shadow_pixels(g);
    CHECK(s3c2400_read8(g->soc, LUT + 255u) == 0x78, "unpaired table is untouched");
    check_pixels(g, FB, isolated, sizeof(isolated));

    seed_luts(g, 1);
    gp32_framebuffer_desc_t fb;
    CHECK(gp32_get_framebuffer(g, &fb) == GP32_OK && fb.pixels_rgba8888, "public framebuffer path");
    CHECK(s3c2400_read8(g->soc, LUT + 255u) == 0x77, "paired LUT endpoint repaired");
    check_pixels(g, FB, repaired, sizeof(repaired));

    /* Left-to-right propagation completes in one pass; right-to-left advances
     * one pixel per pass and must stop after the existing eight-pass limit. */
    uint8_t chain[12];
    memset(chain, 0x78, sizeof(chain)); chain[0] = 0x71;
    surface(g, FB, 12, 1, 0x0b); pixels(g, FB, chain, sizeof(chain));
    direct_fix_gp32_additive_blend_shadow_pixels(g);
    memset(chain + 1, 0x77, sizeof(chain) - 1); check_pixels(g, FB, chain, sizeof(chain));
    memset(chain, 0x78, sizeof(chain)); chain[11] = 0x71;
    pixels(g, FB, chain, sizeof(chain)); direct_fix_gp32_additive_blend_shadow_pixels(g);
    memset(chain + 3, 0x77, 8); check_pixels(g, FB, chain, sizeof(chain));

    /* Diagonal corner adjacency counts; a row boundary is not adjacency. */
    const uint8_t corner[] = {0x71, 0, 0, 0x78};
    const uint8_t corner_done[] = {0x71, 0, 0, 0x77};
    surface(g, FB, 2, 2, 0x0b); pixels(g, FB, corner, sizeof(corner));
    direct_fix_gp32_additive_blend_shadow_pixels(g); check_pixels(g, FB, corner_done, sizeof(corner_done));
    const uint8_t row_edge[] = {0, 0, 0x71, 0x78, 0, 0};
    surface(g, FB, 3, 2, 0x0b); pixels(g, FB, row_edge, sizeof(row_edge));
    direct_fix_gp32_additive_blend_shadow_pixels(g); check_pixels(g, FB, row_edge, sizeof(row_edge));

    surface(g, FB, 4, 1, 0x0c); pixels(g, FB, isolated, sizeof(isolated));
    direct_fix_gp32_additive_blend_shadow_pixels(g); check_pixels(g, FB, isolated, sizeof(isolated));
    uint32_t tail = GP32_RAM_BASE + 8u * 1024u * 1024u - 2u;
    surface(g, tail, 4, 1, 0x0b); pixels(g, tail, isolated, 2);
    direct_fix_gp32_additive_blend_shadow_pixels(g); check_pixels(g, tail, isolated, 2);
    gp32_destroy(g);

    /* The original byte reader sees 0xff in an incomplete final RAM word.
     * Preserve that quirk, but still repair the earlier valid-word pixels. */
    gp32_options_t options = {0}; options.ram_size = 8u * 1024u * 1024u + 2u;
    g = gp32_create(&options); if (!g) return 2;
    seed_luts(g, 1);
    tail = GP32_RAM_BASE + 8u * 1024u * 1024u - 4u;
    const uint8_t odd[] = {0x71, 0x78, 0, 0, 0x71, 0x78};
    const uint8_t odd_done[] = {0x71, 0x77, 0, 0, 0x71, 0x78};
    surface(g, tail, 6, 1, 0x0b); pixels(g, tail, odd, sizeof(odd));
    direct_fix_gp32_additive_blend_shadow_pixels(g); check_pixels(g, tail, odd_done, sizeof(odd_done));
    gp32_destroy(g);
    printf("gp32_shadow: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
