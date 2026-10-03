/*
 * s3c2400_lut_thread_test.c - focused race harness for the shared 16-bpp LUT.
 *
 * The global 256 KB color16_lut is built on the first s3c2400_create(). To
 * prove concurrent creation cannot publish a partially-filled table, N
 * threads are released from a start gate at once and each creates its own
 * core, renders the same deterministic 16-bpp frame, and hashes the result.
 * Every thread must match a single-threaded reference hash computed after the
 * race. A torn/zeroed table entry shows up as a wrong (usually black) pixel.
 *
 * The LUT is a per-process global, so race coverage comes from running the
 * binary in a loop (each process re-initializes the state).
 *
 * Build (mingw/msvc-style portable C11):
 *   gcc -O2 -std=c11 -I src -I include tests/s3c2400_lut_thread_test.c \
 *       build-<x>/libgp32emu.a -o lut_thread_test.exe
 */
#include "s3c2400.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif

#define RAM_BASE 0x0c000000u
#define THREADS 8

static atomic_int g_go;

static uint64_t hash_bytes(const void *data, size_t len) {
    uint64_t h = 14695981039346656037ULL;
    const uint8_t *p = (const uint8_t *)data;
    while (len--) {
        h ^= (uint64_t)*p++;
        h *= 1099511628211ULL;
    }
    return h;
}

static void fill_vram(uint8_t *vram, size_t bytes) {
    uint32_t x = 0x12345678u;
    for (size_t i = 0; i < bytes; ++i) {
        x = x * 1664525u + 1013904223u;
        vram[i] = (uint8_t)(x >> 24);
    }
}

/* Deterministic 240x320 16-bpp frame; identical for every caller. */
static int build_and_render(uint64_t *hash, uint32_t *nonzero) {
    const size_t span = 240u * 320u * 2u;
    s3c2400_t *s = s3c2400_create(8u * 1024u * 1024u);
    if (!s) return 0;
    s3c2400_reset(s);

    for (uint32_t i = 0; i < 256u; ++i) {
        uint32_t color = ((i * 37u) & 0x1fu) << 11 | ((255u - i) & 0x1fu) << 6 | ((i * 11u) & 0x1fu) << 1 | (i & 1u);
        s3c2400_write32(s, 0x14a00400u + i * 4u, color & 0xffffu);
    }

    uint8_t *img = (uint8_t *)malloc(span);
    if (!img) { s3c2400_destroy(s); return 0; }
    fill_vram(img, span);

    const uint32_t base = RAM_BASE + 0x1000u;
    char err[128];
    if (!s3c2400_load_ram_image(s, base, img, span, err, sizeof(err))) {
        fprintf(stderr, "load_ram_image: %s\n", err);
        free(img); s3c2400_destroy(s); return 0;
    }
    free(img);

    s3c2400_write32(s, 0x14a00004u, (uint32_t)((320u - 1u) & 0x3ffu) << 14);
    s3c2400_write32(s, 0x14a00008u, (uint32_t)((240u - 1u) & 0x7ffu) << 8);
    s3c2400_write32(s, 0x14a00014u, (base >> 1) & 0x7ffffffu);
    s3c2400_write32(s, 0x14a00018u, (uint32_t)(((uint64_t)base + span) >> 1));
    s3c2400_write32(s, 0x14a0001cu, 240u & 0x7ffu);
    s3c2400_write32(s, 0x14a00000u, (0x0cu << 1) | 1u);

    s3c2400_render_lcd(s);
    const uint32_t *fb = s3c2400_framebuffer(s, NULL, NULL, NULL, NULL);
    if (!fb) { s3c2400_destroy(s); return 0; }

    *hash = hash_bytes(fb, 240u * 320u * sizeof(uint32_t));
    uint32_t nz = 0;
    for (uint32_t i = 0; i < 240u * 320u; ++i) if (fb[i] & 0x00ffffffu) nz++;
    *nonzero = nz;
    s3c2400_destroy(s);
    return 1;
}

typedef struct { uint64_t hash; uint32_t nonzero; int ok; } result_t;

#ifdef _WIN32
static DWORD WINAPI thread_entry(LPVOID arg) {
    result_t *r = (result_t *)arg;
    while (!atomic_load_explicit(&g_go, memory_order_acquire)) { }
    r->ok = build_and_render(&r->hash, &r->nonzero);
    return 0;
}
#else
static void *thread_entry(void *arg) {
    result_t *r = (result_t *)arg;
    while (!atomic_load_explicit(&g_go, memory_order_acquire)) { }
    r->ok = build_and_render(&r->hash, &r->nonzero);
    return NULL;
}
#endif

int main(void) {
    result_t results[THREADS];
    memset(results, 0, sizeof(results));

#ifdef _WIN32
    HANDLE threads[THREADS];
    for (int i = 0; i < THREADS; ++i)
        threads[i] = CreateThread(NULL, 0, thread_entry, &results[i], 0, NULL);
    atomic_store_explicit(&g_go, 1, memory_order_release);
    WaitForMultipleObjects(THREADS, threads, TRUE, INFINITE);
    for (int i = 0; i < THREADS; ++i) CloseHandle(threads[i]);
#else
    pthread_t threads[THREADS];
    for (int i = 0; i < THREADS; ++i) pthread_create(&threads[i], NULL, thread_entry, &results[i]);
    atomic_store_explicit(&g_go, 1, memory_order_release);
    for (int i = 0; i < THREADS; ++i) pthread_join(threads[i], NULL);
#endif

    uint64_t ref_hash = 0;
    uint32_t ref_nonzero = 0;
    if (!build_and_render(&ref_hash, &ref_nonzero)) {
        fprintf(stderr, "reference render failed\n");
        return 1;
    }

    int failures = 0;
    for (int i = 0; i < THREADS; ++i) {
        if (!results[i].ok) { fprintf(stderr, "thread %d create/render failed\n", i); failures++; continue; }
        if (results[i].hash != ref_hash || results[i].nonzero != ref_nonzero) {
            fprintf(stderr, "thread %d mismatch hash=%016" PRIx64 " expected=%016" PRIx64
                            " nonzero=%u expected=%u\n",
                    i, results[i].hash, ref_hash, results[i].nonzero, ref_nonzero);
            failures++;
        }
    }

    if (failures) {
        printf("FAIL threads=%d failures=%d\n", THREADS, failures);
        return 1;
    }
    printf("OK threads=%d hash=%016" PRIx64 " nonzero=%u\n", THREADS, ref_hash, ref_nonzero);
    return 0;
}
