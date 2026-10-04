/*
 * s3c2400_render_test.c - focused scanout exactness + timing harness.
 *
 * Drives s3c2400_render_lcd() through the public MMIO/register API for a set
 * of LCD configurations (16/8/4/2/1-bpp, tight and wrapped page widths, odd
 * widths), prints an FNV-1a-64 hash of the RGBA framebuffer plus the mean
 * render time. The hash is the exactness gate: it must stay identical before
 * and after a scanout optimization. The timing is the measurement gate.
 *
 * Build (mingw/msvc-style portable C11):
 *   gcc -O2 -std=c11 -I src -I include tests/s3c2400_render_test.c \
 *       build-<x>/libgp32emu.a -o render_test.exe
 */
#ifndef _WIN32
#define _POSIX_C_SOURCE 199309L
#endif

#include "s3c2400.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <time.h>
#endif

#define RAM_BASE 0x0c000000u

static double now_seconds(void) {
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER counter;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#endif
}

static uint64_t hash_bytes(const void *data, size_t len) {
    uint64_t h = 14695981039346656037ULL;
    const uint8_t *p = (const uint8_t *)data;
    while (len--) {
        h ^= (uint64_t)*p++;
        h *= 1099511628211ULL;
    }
    return h;
}

/* Deterministic VRAM pattern; independent of any host memory layout. */
static void fill_vram(uint8_t *vram, size_t bytes) {
    uint32_t x = 0x12345678u;
    for (size_t i = 0; i < bytes; ++i) {
        x = x * 1664525u + 1013904223u;
        vram[i] = (uint8_t)(x >> 24);
    }
}

typedef struct lcd_case {
    const char *name;
    uint32_t bppmode;      /* BPPMODE field of LCDCON1 */
    uint32_t w, h;         /* visible size, pixels */
    uint32_t pagewidth;    /* LCDSADDR3 PAGEWIDTH, halfwords */
    uint32_t offsize;      /* LCDSADDR3 OFFSIZE, halfwords */
    uint32_t src_delta;    /* bytes per line added by offsize */
} lcd_case_t;

static const lcd_case_t k_cases[] = {
    { "16bpp-tight",  0x0cu, 240, 320, 240, 0,  0 },
    { "16bpp-wrapped",0x0cu, 240, 320, 240, 16, 32 },
    { "16bpp-narrow", 0x0cu, 120, 320, 120, 0,  0 },
    { "8bpp-tight",   0x0bu, 240, 320, 120, 0,  0 },
    { "8bpp-odd-width",0x0bu, 239, 320, 120, 0,  0 },
    { "4bpp-tight",   0x0au, 240, 320, 60,  0,  0 },
    { "2bpp-tight",   0x09u, 240, 320, 30,  0,  0 },
    { "1bpp-tight",   0x08u, 240, 320, 15,  0,  0 },
    /* 240 pixels ends inside a 32-pixel word and takes the fallback.
     * A 224-pixel row also exercises the contiguous 1-bpp palette path. */
    { "1bpp-aligned", 0x08u, 224, 320, 14,  0,  0 },
};

static const uint32_t k_frame_bytes = 240u * 320u * 4u;

int main(int argc, char **argv) {
    uint64_t iterations = 200;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--iterations") && i + 1 < argc) iterations = strtoull(argv[++i], NULL, 0);
    }
    if (!iterations) iterations = 1;

    s3c2400_t *s = s3c2400_create(8u * 1024u * 1024u);
    if (!s) { fprintf(stderr, "s3c2400_create failed\n"); return 1; }
    s3c2400_reset(s);

    /* Program the LCD palette the same way guest software does. */
    for (uint32_t i = 0; i < 256u; ++i) {
        uint32_t color = ((i * 37u) & 0x1fu) << 11 | ((255u - i) & 0x1fu) << 6 | ((i * 11u) & 0x1fu) << 1 | (i & 1u);
        s3c2400_write32(s, 0x14a00400u + i * 4u, color & 0xffffu);
    }

    uint8_t *vram = (uint8_t *)malloc(k_frame_bytes + 0x10000u);
    if (!vram) { s3c2400_destroy(s); return 1; }
    fill_vram(vram, k_frame_bytes + 0x10000u);

    const uint32_t base = RAM_BASE + 0x1000u;
    int failures = 0;
    for (size_t c = 0; c < sizeof(k_cases) / sizeof(k_cases[0]); ++c) {
        const lcd_case_t *cfg = &k_cases[c];
        uint64_t per_line = (uint64_t)cfg->pagewidth * 2u + (uint64_t)cfg->offsize * 2u;
        uint64_t span = (uint64_t)cfg->h * per_line + (uint64_t)cfg->pagewidth * 2u;
        (void)cfg->src_delta;
        if (span > k_frame_bytes + 0x10000u) span = k_frame_bytes + 0x10000u;
        if (span < cfg->pagewidth * 2u + 4u) span = cfg->pagewidth * 2u + 4u;

        uint8_t *img = (uint8_t *)malloc((size_t)span);
        if (!img) { failures++; break; }
        memcpy(img, vram, (size_t)span);
        char err[128];
        if (!s3c2400_load_ram_image(s, base, img, (size_t)span, err, sizeof(err))) {
            fprintf(stderr, "load_ram_image(%s): %s\n", cfg->name, err);
            free(img); failures++; break;
        }
        free(img);

        /* Register writes never start the scanout; LCDCON1 does, so write it
         * last and re-write it on each timed iteration. */
        /* Register byte offsets: LCDCON1..5 = 0x00/0x04/0x08/0x0c/0x10,
         * LCDSADDR1..3 = 0x14/0x18/0x1c. */
        s3c2400_write32(s, 0x14a00004u, ((cfg->h - 1u) & 0x3ffu) << 14);
        s3c2400_write32(s, 0x14a00008u, ((cfg->w - 1u) & 0x7ffu) << 8);
        s3c2400_write32(s, 0x14a00014u, (base >> 1) & 0x7ffffffu);
        s3c2400_write32(s, 0x14a00018u, (uint32_t)(((uint64_t)base + span) >> 1));
        s3c2400_write32(s, 0x14a0001cu, (cfg->offsize << 11) | (cfg->pagewidth & 0x7ffu));
        s3c2400_write32(s, 0x14a00000u, (cfg->bppmode << 1) | 1u);

        s3c2400_render_lcd(s);
        const uint32_t *fb = s3c2400_framebuffer(s, NULL, NULL, NULL, NULL);
        if (!fb) { fprintf(stderr, "%s: no framebuffer\n", cfg->name); failures++; break; }
        uint64_t hash = hash_bytes(fb, 240u * 320u * sizeof(uint32_t));
        uint32_t nonzero = 0;
        for (uint32_t i = 0; i < 240u * 320u; ++i) if (fb[i] & 0x00ffffffu) nonzero++;

        double t0 = now_seconds();
        for (uint64_t i = 0; i < iterations; ++i) s3c2400_render_lcd(s);
        double t1 = now_seconds();

        const uint32_t *fb2 = s3c2400_framebuffer(s, NULL, NULL, NULL, NULL);
        uint64_t hash2 = hash_bytes(fb2, 240u * 320u * sizeof(uint32_t));
        if (hash2 != hash) failures++;

        printf("case=%s hash=%016" PRIx64 " hash_repeat=%016" PRIx64 " nonzero=%u ns_per_render=%.1f\n",
               cfg->name, hash, hash2, nonzero, (t1 - t0) * 1e9 / (double)iterations);
    }

    free(vram);
    s3c2400_destroy(s);
    return failures ? 1 : 0;
}
