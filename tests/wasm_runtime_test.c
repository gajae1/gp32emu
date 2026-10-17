/* Compiled with the real freestanding WASM libc, not the host's libm. */
#include "audio/gp32_audio_resampler.h"
#include "smartmedia.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static gp32_audio_resampler_t resampler;
static int16_t input[1024u * 2u];
static int16_t output[8192u * 2u];

__attribute__((export_name("test_resample_constant")))
uint32_t test_resample_constant(uint32_t src_rate, uint32_t dst_rate, int level) {
    for (unsigned i = 0; i < 1024u * 2u; ++i) input[i] = (int16_t)level;
    gp32_audio_resampler_init(&resampler);
    return (uint32_t)gp32_audio_resampler_process(&resampler, input, 1024u,
                                                 src_rate, dst_rate, 0,
                                                 output, 8192u);
}

__attribute__((export_name("test_audio_ptr")))
uint32_t test_audio_ptr(void) { return (uint32_t)(uintptr_t)output; }

__attribute__((export_name("test_coefficient_sum")))
int32_t test_coefficient_sum(uint32_t phase) {
    int32_t sum = 0;
    for (unsigned i = 0; i < GP32_AUDIO_POLY_TAPS; ++i)
        sum += resampler.poly_coef[phase % GP32_AUDIO_POLY_PHASES][i];
    return sum;
}

__attribute__((export_name("test_smc_host_time_ms")))
uint64_t test_smc_host_time_ms(void) { return smc_host_time_ms(); }

extern uint32_t __pcfx_wasm_heap_used(void);
extern void pcfx_wasm_vfs_clear(void);
extern uint32_t pcfx_wasm_vfs_count(void);
extern uint32_t pcfx_wasm_vfs_add_file(uint32_t path, uint32_t path_len,
                                        uint32_t data, uint32_t size);

#define CHECK(c) do { if (!(c)) return __LINE__; } while (0)

/* Each call has its own WASM instance. Returning a source line keeps failures
 * readable without depending on the freestanding printf stubs. */
__attribute__((export_name("test_vfs_growing_writes")))
uint32_t test_vfs_growing_writes(void) {
    uint8_t block[4096], saved[4096];
    uint32_t initial = __pcfx_wasm_heap_used();
    for (unsigned cycle = 0; cycle < 6u; ++cycle) {
        FILE *f = fopen("/growing", "wb");
        CHECK(f);
        for (unsigned i = 0; i < 128u; ++i) {
            memset(block, (int)i, sizeof(block));
            CHECK(fwrite(block, 1u, sizeof(block), f) == sizeof(block));
        }
        CHECK(fclose(f) == 0);
        f = fopen("/growing", "rb");
        CHECK(f);
        for (unsigned i = 0; i < 128u; ++i) {
            memset(block, (int)i, sizeof(block));
            CHECK(fread(saved, 1u, sizeof(saved), f) == sizeof(saved));
            CHECK(memcmp(block, saved, sizeof(block)) == 0);
        }
        CHECK(fclose(f) == 0 && remove("/growing") == 0);
        CHECK(__pcfx_wasm_heap_used() == initial);
    }
    return 0;
}

__attribute__((export_name("test_vfs_close_full")))
uint32_t test_vfs_close_full(void) {
    static uint8_t borrowed = 0x5au;
    uint32_t initial = __pcfx_wasm_heap_used();
    char path[32];
    for (unsigned i = 0; i < 128u; ++i) {
        snprintf(path, sizeof(path), "/entry%u", i);
        CHECK(pcfx_wasm_vfs_add_file((uint32_t)(uintptr_t)path, (uint32_t)strlen(path),
                                    (uint32_t)(uintptr_t)&borrowed, 1u));
    }
    FILE *reader = fopen("/entry0", "rb");
    FILE *writer = fopen("/overflow", "wb");
    CHECK(reader && writer);
    CHECK(fwrite(&borrowed, 1u, 1u, writer) == 1u);
    CHECK(fclose(writer) == EOF && errno == ENOSPC);
    uint8_t byte = 0;
    CHECK(fread(&byte, 1u, 1u, reader) == 1u && byte == borrowed);
    CHECK(fclose(reader) == 0 && pcfx_wasm_vfs_count() == 128u);
    pcfx_wasm_vfs_clear();
    CHECK(__pcfx_wasm_heap_used() == initial);
    return 0;
}

__attribute__((export_name("test_vfs_clear_ownership")))
uint32_t test_vfs_clear_ownership(void) {
    uint32_t initial = __pcfx_wasm_heap_used();
    uint8_t *borrowed = malloc(64u);
    CHECK(borrowed);
    memset(borrowed, 0x5a, 64u);
    uint32_t borrowed_heap = __pcfx_wasm_heap_used();
    static const char path[] = "/borrowed";
    CHECK(pcfx_wasm_vfs_add_file((uint32_t)(uintptr_t)path, sizeof(path) - 1u,
                                (uint32_t)(uintptr_t)borrowed, 64u));
    FILE *owned = fopen("/owned", "wb");
    CHECK(owned && fwrite(borrowed, 1u, 1u, owned) == 1u && fclose(owned) == 0);
    FILE *reader = fopen("/owned", "rb");
    FILE *pending = fopen("/pending", "wb");
    CHECK(reader && pending && fwrite(borrowed, 1u, 1u, pending) == 1u);
    /* Clear invalidates every open handle. Readers borrow VFS data, while an
     * unpublished writer still owns its buffer and must release it here. */
    pcfx_wasm_vfs_clear();
    CHECK(pcfx_wasm_vfs_count() == 0u && __pcfx_wasm_heap_used() == borrowed_heap);
    uint8_t *other = malloc(64u);
    CHECK(other && other != borrowed && borrowed[0] == 0x5au);
    free(other);
    free(borrowed);
    CHECK(__pcfx_wasm_heap_used() == initial);
    return 0;
}

__attribute__((export_name("test_vfs_growth_failure")))
uint32_t test_vfs_growth_failure(void) {
    uint32_t initial = __pcfx_wasm_heap_used();
    uint8_t byte = 0x5au, saved = 0;
    FILE *f = fopen("/failed_growth", "wb");
    CHECK(f && fwrite(&byte, 1u, 1u, f) == 1u);
    /* Exceeds the module's 256 MiB limit. Allocation must fail before the
     * source is read, and the previously written byte must remain owned. */
    CHECK(fwrite(&byte, 1u, 300u * 1024u * 1024u, f) == 0u);
    CHECK(errno == ENOMEM && ferror(f));
    CHECK(fclose(f) == 0);
    f = fopen("/failed_growth", "rb");
    CHECK(f && fread(&saved, 1u, 1u, f) == 1u && saved == byte);
    CHECK(fclose(f) == 0 && remove("/failed_growth") == 0);
    CHECK(__pcfx_wasm_heap_used() == initial);
    return 0;
}
