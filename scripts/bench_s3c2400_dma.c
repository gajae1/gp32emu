/* Synchronous RAM DMA cost, using the public SoC bus with a real CPU sink.
 * No guest instructions, device clocks, video or audio run during the sample.
 * Build each comparison against its own Release library, for example:
 *   cc -O3 -DNDEBUG -std=c2x -Isrc -Iinclude scripts/bench_s3c2400_dma.c \
 *      build/libgp32emu.a -lm -pthread -o bench_s3c2400_dma
 * Arguments: transfer width (0=byte, 1=halfword, 2=word), bytes, iterations.
 * Record the library's compiler, flags and GP32EMU_CPU_PROFILE setting along
 * with the measurements; this fixture does not infer those build options. */
#define _POSIX_C_SOURCE 200809L
#include "s3c2400.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DMA 0x14600000u
#define SOURCE 0x0c010000u
#define DESTINATION 0x0c200000u

static int completed(s3c2400_t *s, uint32_t bytes) {
    return s3c2400_read32(s, DMA + 12u) == 0u &&
           s3c2400_read32(s, DMA + 16u) == SOURCE + bytes &&
           s3c2400_read32(s, DMA + 20u) == DESTINATION + bytes &&
           s3c2400_read32(s, DMA + 24u) == 0u;
}

int main(int argc, char **argv) {
    if (argc != 4) return 2;
    unsigned dsz = (unsigned)strtoul(argv[1], NULL, 0);
    uint32_t bytes = (uint32_t)strtoul(argv[2], NULL, 0);
    uint32_t iterations = (uint32_t)strtoul(argv[3], NULL, 0);
    if (dsz > 2u || !bytes || (bytes & ((1u << dsz) - 1u)) ||
        (bytes >> dsz) > 0xfffffu || bytes > 0x100000u || !iterations) return 2;
    s3c2400_t *s = s3c2400_create(4u * 1024u * 1024u);
    if (!s) return 2;
    arm_bus_t bus = s3c2400_get_bus(s);
    arm920t_t *cpu = arm920t_create(&bus);
    if (!cpu) return 2;
    s3c2400_set_irq_sink(s, cpu);
    uint8_t *data = malloc(bytes);
    if (!data) return 2;
    for (uint32_t i = 0; i < bytes; ++i) data[i] = (uint8_t)(i * 73u + (i >> 9) + 7u);
    char error[128];
    if (!s3c2400_load_ram_image(s, SOURCE, data, bytes, error, sizeof(error))) return 2;
    s3c2400_write32(s, DMA, SOURCE);
    s3c2400_write32(s, DMA + 4u, DESTINATION);
    /* Whole-service software DMA with reload disabled. Completion clears ON;
     * each write of ON plus the software-request bit starts another complete
     * transfer with the programmed original source, destination and count. */
    s3c2400_write32(s, DMA + 8u, 0x04400000u | (dsz << 20) | (bytes >> dsz));
    for (unsigned i = 0; i < 64u; ++i) s3c2400_write32(s, DMA + 24u, 3u);
    if (!completed(s, bytes)) return 1;
    struct timespec start, end;
    if (clock_gettime(CLOCK_MONOTONIC, &start)) return 2;
    for (uint32_t i = 0; i < iterations; ++i) s3c2400_write32(s, DMA + 24u, 3u);
    if (clock_gettime(CLOCK_MONOTONIC, &end)) return 2;
    double ns = (double)(end.tv_sec - start.tv_sec) * 1e9 + (double)(end.tv_nsec - start.tv_nsec);
    const uint8_t *result = s3c2400_ram_data(s) + 0x200000u;
    if (!completed(s, bytes) || memcmp(result, data, bytes)) {
        fputs("FAIL: DMA payload or completion registers changed\n", stderr);
        return 1;
    }
    uint32_t hash = 2166136261u;
    for (uint32_t i = 0; i < bytes; ++i) hash = (hash ^ result[i]) * 16777619u;
    printf("dsz=%u bytes=%u iterations=%u ns_per_transfer=%.3f ns_per_byte=%.6f hash=%08x "
           "fclk=%u pclk=%u runclk=%u speed_percent=%u\n",
           dsz, bytes, iterations, ns / iterations, ns / iterations / bytes, hash,
           s3c2400_fclk_hz(s), s3c2400_pclk_hz(s), s3c2400_run_clock_hz(s),
           s3c2400_cpu_speed_percent(s));
    free(data);
    arm920t_destroy(cpu);
    s3c2400_destroy(s);
    return 0;
}
