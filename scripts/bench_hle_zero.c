/* Compile both variants of gp32.c against the same other core objects.
 * Repeated direct-HLE RAM clears; no firmware, instruction execution or FPS. */
#ifndef GP32_CORE_UNDER_TEST
#define GP32_CORE_UNDER_TEST "../src/gp32.c"
#endif
#include GP32_CORE_UNDER_TEST
#include <time.h>

static uint64_t now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) exit(2);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}
static uint32_t number(const char *s) {
    char *end;
    errno = 0;
    unsigned long n = strtoul(s, &end, 10);
    if (errno || *end || n > UINT32_MAX) exit(2);
    return (uint32_t)n;
}
int main(int argc, char **argv) {
    if (argc != 4) { fprintf(stderr, "usage: %s bytes iterations working-spans\n", argv[0]); return 2; }
    uint32_t bytes = number(argv[1]), iterations = number(argv[2]), working = number(argv[3]);
    if (!bytes || !iterations || iterations > 1000000000u ||
        !working || working > 8u || (working & (working - 1u))) return 2;
    gp32_t *g = gp32_create(NULL);
    if (!g) return 2;
    uint32_t ram_size = (uint32_t)s3c2400_ram_size(g->soc);
    uint32_t stride = ram_size / working;
    if (bytes > stride - 128u) { gp32_destroy(g); return 2; }
    arm_bus_t bus = s3c2400_get_bus(g->soc);
    uint8_t *ram = bus.fastmem(bus.user, GP32_RAM_BASE, ram_size, 1);
    if (!ram) { gp32_destroy(g); return 2; }
    memset(ram, 0xa5, ram_size);
    for (unsigned i = 0; i < 64u; ++i)
        direct_zero_if_ram(g, GP32_RAM_BASE + 64u + (i & (working - 1u)) * stride, bytes);
    uint64_t start = now_ns();
    for (uint32_t i = 0; i < iterations; ++i)
        direct_zero_if_ram(g, GP32_RAM_BASE + 64u + (i & (working - 1u)) * stride, bytes);
    uint64_t elapsed = now_ns() - start;
    /* Inspect every byte after timing, including guards and untouched gaps. */
    for (uint32_t i = 0; i < ram_size; ++i) {
        uint32_t in_span = i % stride;
        uint8_t expected = in_span >= 64u && in_span - 64u < bytes ? 0u : 0xa5u;
        if (ram[i] != expected) {
            fprintf(stderr, "RAM mismatch at %08x: %02x != %02x\n", GP32_RAM_BASE + i, ram[i], expected);
            gp32_destroy(g);
            return 1;
        }
    }
    if (arm920t_get_cycles(g->cpu) != 0u) { gp32_destroy(g); return 1; }
    printf("{\"bytes\":%u,\"iterations\":%u,\"working_spans\":%u,\"elapsed_ns\":%" PRIu64
           ",\"ns_per_zero\":%.9f,\"verified_bytes\":%u,\"guest_cycles\":0}\n",
           bytes, iterations, working, elapsed, (double)elapsed / iterations, ram_size);
    gp32_destroy(g);
    return 0;
}
