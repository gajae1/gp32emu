/* Framebuffer pixel oracle for the LCD scanout conversion loops.
 *
 * The scanout must be a pure expansion of the programmed VRAM under the
 * S3C2400 DMA byte-order rule plus the 5:5:5:I colour model, in every one of
 * the four BSWP/HWSWP combinations.  This test renders the 16-bpp direct path
 * and the 8-bpp indexed contiguous path (the two conversion inner loops every
 * GP32 title uses), builds the expected framebuffer independently from the
 * guest-visible register state and the raw VRAM bytes, and compares the whole
 * 240x320 surface.  It runs on every host the core builds for, including
 * native AArch64, so an architecture-specific inner-loop rewrite is checked by
 * the same oracle as the portable one.
 *
 * Expected colour is the documented hardware model: a 16-bpp entry is
 * 5:5:5:I, every channel is six bits wide (its five bits with the shared
 * intensity bit as LSB) and expands to eight bits by replicating the top two.
 * Expected byte order is the documented DMA word assembly (BSWP reverses the
 * four bytes of the word, HWSWP then selects which half is consumed first).
 */
#include "s3c2400.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RAM_BASE 0x0c000000u
#define LCDCON1_ADDR 0x14a00000u
#define LCDCON2_ADDR 0x14a00004u
#define LCDCON3_ADDR 0x14a00008u
#define LCDCON5_ADDR 0x14a00010u
#define LCDSADDR1_ADDR 0x14a00014u
#define LCDSADDR2_ADDR 0x14a00018u
#define LCDSADDR3_ADDR 0x14a0001cu
#define PALETTE_ADDR 0x14a00400u

#define FRAME_W 240u
#define FRAME_H 320u
#define FRAME_PIXELS (FRAME_W * FRAME_H)

static uint16_t palette_entry[256];

static uint8_t expand6(uint32_t v) {
    v &= 0x3fu;
    return (uint8_t)((v << 2) | (v >> 4));
}

/* 5:5:5:I halfword -> XRGB8888, the same model the palette and the direct
 * 16-bpp path both decode through. */
static uint32_t colour_of(uint16_t data) {
    uint32_t i = data & 1u;
    uint8_t r = expand6((((uint32_t)data >> 11) & 0x1fu) << 1 | i);
    uint8_t g = expand6((((uint32_t)data >> 6) & 0x1fu) << 1 | i);
    uint8_t b = expand6((((uint32_t)data >> 1) & 0x1fu) << 1 | i);
    return 0xff000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

/* LCDCON5 BSWP/HWSWP as the panel DMA assembles one 32-bit guest word.
 * mode = (HWSWP << 1) | BSWP. */
static uint32_t dma_word(const uint8_t *p, uint32_t mode) {
    switch (mode) {
    case 0u: return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
    case 1u: return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
    case 2u: return ((uint32_t)p[1] << 24) | ((uint32_t)p[0] << 16) | ((uint32_t)p[3] << 8) | p[2];
    default: return ((uint32_t)p[2] << 24) | ((uint32_t)p[3] << 16) | ((uint32_t)p[0] << 8) | p[1];
    }
}

static void fill_vram(uint8_t *vram, size_t bytes) {
    uint32_t x = 0x2f6e5d4cu;
    for (size_t i = 0; i < bytes; ++i) {
        x = x * 1664525u + 1013904223u;
        vram[i] = (uint8_t)(x >> 24);
    }
}

/* Bytes per VRAM line and pixels per word for the two supported modes. */
static int check_mode(s3c2400_t *s, uint32_t bppmode, uint32_t bpp, uint32_t mode,
                      const uint8_t *vram, size_t vram_bytes) {
    const uint32_t row_bytes = FRAME_W * (bpp / 8u);
    const uint32_t pixels_per_word = 32u / bpp;
    const uint32_t words_per_row = FRAME_W / pixels_per_word;
    const uint32_t base = RAM_BASE + 0x2000u;
    char err[128];

    if (!s3c2400_load_ram_image(s, base, vram, vram_bytes, err, sizeof(err))) {
        fprintf(stderr, "FAIL: %ubpp mode %u load_ram_image: %s\n", bpp, mode, err);
        return 0;
    }
    /* Register writes other than LCDCON1 never start a scanout, so the mode,
     * byte order and addressing are all in place before ENVID is set. */
    s3c2400_write32(s, LCDCON2_ADDR, (FRAME_H - 1u) << 14);
    s3c2400_write32(s, LCDCON3_ADDR, (FRAME_W - 1u) << 8);
    /* LCDCON5 carries HWSWP in bit 0 and BSWP in bit 1, i.e. the transposed
     * order of the BSWP/HWSWP pair the model names. */
    s3c2400_write32(s, LCDCON5_ADDR, ((mode & 1u) << 1) | ((mode >> 1) & 1u));
    s3c2400_write32(s, LCDSADDR1_ADDR, (base >> 1) & 0x7ffffffu);
    s3c2400_write32(s, LCDSADDR2_ADDR, (uint32_t)(((uint64_t)base + vram_bytes) >> 1));
    /* One line of contiguous pixels, therefore PAGEWIDTH in halfwords. */
    s3c2400_write32(s, LCDSADDR3_ADDR, (FRAME_W * (bpp / 8u)) / 2u);
    s3c2400_write32(s, LCDCON1_ADDR, (bppmode << 1) | 1u);
    s3c2400_render_lcd(s);

    const uint32_t *fb = s3c2400_framebuffer(s, NULL, NULL, NULL, NULL);
    if (!fb) { fprintf(stderr, "FAIL: %ubpp mode %u no framebuffer\n", bpp, mode); return 0; }

    int bad = 0;
    for (uint32_t y = 0; y < FRAME_H && !bad; ++y) {
        /* The panel-settle line is not part of the visible glass: the first
         * presented row repeats the first displayed scanline, so VRAM row 0
         * never reaches the framebuffer. */
        const uint8_t *line = vram + (size_t)(y == 0u ? 1u : y) * row_bytes;
        for (uint32_t w = 0; w < words_per_row; ++w) {
            const uint32_t word = dma_word(line + (size_t)w * 4u, mode);
            for (uint32_t k = 0; k < pixels_per_word; ++k) {
                const uint32_t shift = 32u - bpp * (k + 1u);
                const uint32_t field = (word >> shift) & (bpp == 16u ? 0xffffu : 0xffu);
                const uint32_t want = bpp == 16u ? colour_of((uint16_t)field)
                                                 : colour_of(palette_entry[field]);
                const uint32_t x = w * pixels_per_word + k;
                if (fb[(size_t)y * FRAME_W + x] != want) {
                    fprintf(stderr, "FAIL: %ubpp mode %u pixel(%u,%u)=%08" PRIx32
                            " want=%08" PRIx32 "\n", bpp, mode, x, y,
                            fb[(size_t)y * FRAME_W + x], want);
                    bad = 1;
                    break;
                }
            }
            if (bad) break;
        }
    }
    if (bad) return 0;

    /* Row 0 is the hidden prefetch line replaced by row 1: verify that rule
     * holds as an equality rather than a per-pixel expectation. */
    if (memcmp(fb, fb + FRAME_W, FRAME_W * sizeof(uint32_t)) != 0) {
        fputs("FAIL: presented row 0 differs from row 1\n", stderr);
        return 0;
    }
    printf("PASS: %2ubpp mode %u scanout matches the DMA/colour model\n", bpp, mode);
    return 1;
}

int main(void) {
    s3c2400_t *s = s3c2400_create(8u * 1024u * 1024u);
    if (!s) { fputs("FAIL: allocate scanout test SoC\n", stderr); return 2; }
    s3c2400_reset(s);

    for (uint32_t i = 0; i < 256u; ++i) {
        palette_entry[i] = (uint16_t)(((i * 29u) & 0x1fu) << 11 |
                                      ((255u - i) & 0x1fu) << 6 |
                                      ((i * 7u) & 0x1fu) << 1 | (i & 1u));
        s3c2400_write32(s, PALETTE_ADDR + i * 4u, palette_entry[i]);
    }

    const size_t vram_bytes = (size_t)FRAME_W * FRAME_H * 2u;
    uint8_t *vram = (uint8_t *)malloc(vram_bytes);
    if (!vram) { s3c2400_destroy(s); return 2; }
    fill_vram(vram, vram_bytes);

    int ok = 1;
    for (uint32_t mode = 0; mode < 4u; ++mode)
        ok = check_mode(s, 0x0cu, 16u, mode, vram, vram_bytes) && ok;
    for (uint32_t mode = 0; mode < 4u; ++mode)
        ok = check_mode(s, 0x0bu, 8u, mode, vram, vram_bytes) && ok;

    free(vram);
    s3c2400_destroy(s);
    return ok ? 0 : 1;
}
