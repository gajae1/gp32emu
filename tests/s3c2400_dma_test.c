/* RAM DMA must preserve per-unit read-before-write ordering and the public
 * transfer registers. This byte oracle deliberately includes overlapping
 * spans and RAM edges, where a bulk copy must keep the ordinary bus path. */
#include "s3c2400.h"
#include <stdio.h>
#include <string.h>

#define BASE 0x0c000000u
#define RAM_BYTES 16384u

typedef struct layout {
    uint32_t src, dst;
    unsigned fixed_src, fixed_dst;
} layout_t;

static uint8_t initial[RAM_BYTES], expected[RAM_BYTES];

static int one(s3c2400_t *s, unsigned channel, unsigned dsz,
               const layout_t *layout, uint32_t count, unsigned mode) {
    s3c2400_reset(s);
    char error[128];
    if (!s3c2400_load_ram_image(s, BASE, initial, sizeof(initial), error, sizeof(error))) return 0;
    memcpy(expected, initial, sizeof(expected));
    const uint32_t step = 1u << dsz;
    const uint32_t width = dsz < 2u ? step : 4u;
    const unsigned whole = mode & 1u, stop = (mode >> 1) & 1u;
    const uint32_t moved = whole ? count : (count != 0u);
    uint32_t src = layout->src, dst = layout->dst;
    for (uint32_t i = 0; i < moved; ++i) {
        /* A unit is read completely before any of its bytes are written.
         * Forward overlap can therefore propagate earlier units, including
         * an overlap smaller than the transfer width. memmove is not an
         * equivalent model for this case. Unmapped RAM-neighbour reads are
         * all ones, and their writes are dropped by the SoC bus. */
        uint8_t value[4];
        for (uint32_t b = 0; b < width; ++b)
            value[b] = src + b >= BASE && src + b - BASE < RAM_BYTES
                     ? expected[src + b - BASE] : 0xffu;
        for (uint32_t b = 0; b < width; ++b)
            if (dst + b >= BASE && dst + b - BASE < RAM_BYTES)
                expected[dst + b - BASE] = value[b];
        if (!layout->fixed_src) src += step;
        if (!layout->fixed_dst) dst += step;
    }
    uint32_t left = count - moved;
    const uint32_t irq = left ? 0u : 1u << (17u + channel);
    const uint32_t enabled = !left && stop ? 0u : 2u;
    if (!left && !stop) { left = count; src = layout->src; dst = layout->dst; }

    const uint32_t dma = 0x14600000u + channel * 32u;
    s3c2400_write32(s, 0x14400008u, UINT32_MAX);
    s3c2400_write32(s, dma, layout->src | (layout->fixed_src << 29));
    s3c2400_write32(s, dma + 4u, layout->dst | (layout->fixed_dst << 29));
    s3c2400_write32(s, dma + 8u, (1u << 28) | (whole << 26) | (stop << 22) | (dsz << 20) | count);
    s3c2400_write32(s, dma + 24u, 3u); /* ON plus software request. */
    const uint32_t actual[] = {
        s3c2400_read32(s, dma + 12u), s3c2400_read32(s, dma + 16u),
        s3c2400_read32(s, dma + 20u), s3c2400_read32(s, dma + 24u),
        s3c2400_read32(s, 0x14400000u)
    };
    const uint32_t want[] = {left, src, dst, enabled, irq};
    if (memcmp(actual, want, sizeof(want)) ||
        memcmp(s3c2400_ram_data(s), expected, sizeof(expected))) {
        fprintf(stderr, "FAIL: DMA%u dsz=%u src=%08x/%u dst=%08x/%u count=%u mode=%u\n",
                channel, dsz, layout->src, layout->fixed_src, layout->dst, layout->fixed_dst, count, mode);
        for (unsigned r = 0; r < GP32_ARRAY_COUNT(want); ++r)
            if (actual[r] != want[r]) fprintf(stderr, " register %u=%08x expected %08x\n", r, actual[r], want[r]);
        const uint8_t *ram = s3c2400_ram_data(s);
        for (unsigned i = 0; i < RAM_BYTES; ++i) if (ram[i] != expected[i]) {
            fprintf(stderr, " RAM[%u]=%02x expected %02x\n", i, ram[i], expected[i]);
            break;
        }
        return 0;
    }
    return 1;
}

int main(void) {
    static const layout_t layouts[] = {
        {BASE + 0x200u, BASE + 0x1800u, 0, 0}, /* disjoint */
        {BASE + 0x201u, BASE + 0x1803u, 0, 0}, /* unaligned */
        {BASE + 0x500u, BASE + 0x504u, 0, 0}, /* forward overlap */
        {BASE + 0x504u, BASE + 0x500u, 0, 0}, /* backward overlap */
        {BASE + 0x500u, BASE + 0x501u, 0, 0}, /* partial-unit overlap */
        {BASE + 0x500u, BASE + 0x500u, 0, 0}, /* identical spans */
        {BASE + 0x201u, BASE + 0x1803u, 1, 0},
        {BASE + 0x201u, BASE + 0x1803u, 0, 1},
        {BASE + 0x201u, BASE + 0x1803u, 1, 1},
        {BASE + RAM_BYTES - 3u, BASE + 0x1800u, 0, 0},
        {BASE + 0x200u, BASE + RAM_BYTES - 3u, 0, 0},
        {BASE - 2u, BASE + 0x1800u, 0, 0},
        {BASE + 0x200u, BASE - 2u, 0, 0},
    };
    static const uint32_t counts[] = {0u, 1u, 2u, 17u, 257u};
    uint32_t random = 0x13579bdfu;
    for (unsigned i = 0; i < RAM_BYTES; ++i) {
        random = random * 1664525u + 1013904223u;
        initial[i] = (uint8_t)(random >> 24);
    }
    s3c2400_t *s = s3c2400_create(RAM_BYTES);
    if (!s) return 2;
    unsigned cases = 0, failures = 0;
    for (unsigned channel = 0; channel < 4; ++channel)
        /* DSZ=3 is reserved: keep its existing fallback behavior too. */
        for (unsigned dsz = 0; dsz < 4; ++dsz)
            for (unsigned layout = 0; layout < GP32_ARRAY_COUNT(layouts); ++layout)
                for (unsigned n = 0; n < GP32_ARRAY_COUNT(counts); ++n)
                    for (unsigned mode = 0; mode < 4; ++mode) {
                        ++cases;
                        failures += !one(s, channel, dsz, &layouts[layout], counts[n], mode);
                    }
    s3c2400_destroy(s);
    printf("DMA oracle cases=%u failures=%u\n", cases, failures);
    return failures ? 1 : 0;
}
