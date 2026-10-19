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

/* Samsung S3C2400 manual pp.8-10 and 8-13:
 * https://datasheets.chipdb.org/Samsung/S3C2400.pdf
 * ON_OFF enables requests; it is not a request by itself. Every accepted
 * SW_TRIG starts one service and clears SW_TRIG, including while ON remains
 * set between the atomic transfers of a single-service operation. */
static void trigger_write(s3c2400_t *s, uint32_t addr, unsigned access) {
    if (access == 0u) s3c2400_write8(s, addr, 3u);
    else if (access == 1u) s3c2400_write16(s, addr, 3u);
    else s3c2400_write32(s, addr, 3u);
}

static int trigger_case(s3c2400_t *s, unsigned ch, unsigned dsz,
                        unsigned access, unsigned reload) {
    const uint32_t dma = 0x14600000u + ch * 32u;
    const uint32_t src = BASE + 0x200u, dst = BASE + 0x1800u;
    const uint32_t width = 1u << dsz, count = 3u;
    const uint32_t dcon = (1u << 28) | (!reload << 22) | (dsz << 20) | count;
    const uint32_t irq = 1u << (17u + ch);
    int failures = 0;
    char error[128];
    s3c2400_reset(s);
    if (!s3c2400_load_ram_image(s, BASE, initial, RAM_BYTES, error, sizeof(error))) return 1;
    memcpy(expected, initial, RAM_BYTES);
    s3c2400_write32(s, 0x14400008u, UINT32_MAX);
    s3c2400_write32(s, dma, src);
    s3c2400_write32(s, dma + 4u, dst);
    s3c2400_write32(s, dma + 8u, dcon);
    s3c2400_write32(s, dma + 24u, 2u); /* Arm without requesting. */
    if (memcmp(s3c2400_ram_data(s), expected, RAM_BYTES) ||
        s3c2400_read32(s, 0x14400000u) != 0u) {
        fprintf(stderr, "FAIL: DMA%u dsz=%u enable without SW_TRIG transferred data\n", ch, dsz);
        ++failures;
    }

    /* Isolate repeated requests from the enable-only failure: restart with
     * ON|SW_TRIG, the first request that the old implementation did handle. */
    s3c2400_write32(s, dma + 24u, 4u);
    if (!s3c2400_load_ram_image(s, BASE, initial, RAM_BYTES, error, sizeof(error))) return 1;
    s3c2400_write32(s, 0x14400000u, UINT32_MAX);
    for (unsigned request = 0; request < (reload ? 6u : 3u); ++request) {
        const uint32_t unit = request % count;
        memcpy(expected + dst - BASE + unit * width,
               initial + src - BASE + unit * width, width);
        trigger_write(s, dma + 24u, access);
        uint32_t left = count - unit - 1u;
        uint32_t next = unit + 1u;
        const uint32_t pending = left ? 0u : irq;
        const uint32_t on = left || reload ? 2u : 0u;
        if (!left && reload) { left = count; next = 0u; }
        const uint32_t actual[] = {
            s3c2400_read32(s, dma + 12u), s3c2400_read32(s, dma + 16u),
            s3c2400_read32(s, dma + 20u), s3c2400_read32(s, dma + 24u),
            s3c2400_read32(s, 0x14400000u)
        };
        const uint32_t want[] = {left, src + next * width, dst + next * width, on, pending};
        if (memcmp(actual, want, sizeof(want)) ||
            memcmp(s3c2400_ram_data(s), expected, RAM_BYTES)) {
            fprintf(stderr, "FAIL: DMA%u dsz=%u access=%u reload=%u request=%u count=%u/%u trigger=%u/%u\n",
                    ch, dsz, access, reload, request + 1u, actual[0], left, actual[3], on);
            ++failures;
        }
        s3c2400_write32(s, 0x14400000u, irq);
        if (request == 0u && access == 2u) {
            FILE *state = tmpfile();
            if (!state) { ++failures; break; }
            int ok = s3c2400_state_save(s, state);
            rewind(state);
            ok = ok && s3c2400_state_load(s, state);
            fclose(state);
            if (!ok) { fputs("FAIL: DMA request state round trip\n", stderr); ++failures; break; }
        }
        if (request == 0u) s3c2400_write32(s, dma + 24u, 2u); /* No new request. */
    }

    /* Software triggers have no effect while the channel is off, or when
     * a hardware source is selected. No hardware source fires in this case. */
    memcpy(expected, s3c2400_ram_data(s), RAM_BYTES);
    s3c2400_write32(s, dma + 24u, 4u);
    s3c2400_write32(s, 0x14400000u, UINT32_MAX);
    s3c2400_write32(s, dma + 24u, 1u);
    if (memcmp(s3c2400_ram_data(s), expected, RAM_BYTES) ||
        s3c2400_read32(s, 0x14400000u) != 0u) ++failures;
    s3c2400_write32(s, dma + 8u, dcon | (1u << 23));
    trigger_write(s, dma + 24u, access);
    if (memcmp(s3c2400_ram_data(s), expected, RAM_BYTES) ||
        s3c2400_read32(s, 0x14400000u) != 0u) ++failures;
    s3c2400_write32(s, dma + 8u, dcon);
    s3c2400_write32(s, dma + 24u, 7u); /* STOP takes precedence over ON|SW_TRIG. */
    if (memcmp(s3c2400_ram_data(s), expected, RAM_BYTES) ||
        s3c2400_read32(s, 0x14400000u) != 0u ||
        s3c2400_read32(s, dma + 12u) != 0u ||
        s3c2400_read32(s, dma + 16u) != 0u ||
        s3c2400_read32(s, dma + 20u) != 0u ||
        (s3c2400_read32(s, dma + 24u) & 2u)) {
        fputs("FAIL: STOP did not suppress a software DMA request\n", stderr);
        ++failures;
    }
    return failures;
}

/* DMASKTRIG.STOP (manual p.8-13) ends the current atomic transfer, clears
 * the current registers, and prevents the remaining whole service. These
 * cases use TSZ=0: the current emulator does not implement four-unit bursts. */
static int stop_case(s3c2400_t *s, unsigned ch, unsigned dsz,
                     unsigned whole, unsigned reload, unsigned iis_request) {
    const uint32_t dma = 0x14600000u + ch * 32u;
    const uint32_t src = BASE + 0x200u, width = 1u << dsz;
    const uint32_t irq = 1u << (17u + ch);
    int failures = 0;
    s3c2400_reset(s);
    s3c2400_write32(s, 0x14400008u, UINT32_MAX);
    for (unsigned i = 0; i < 3u; ++i) {
        uint32_t value = i ? i * 0x11111111u : 4u;
        if (dsz == 0u) s3c2400_write8(s, src + i * width, (uint8_t)value);
        else if (dsz == 1u) s3c2400_write16(s, src + i * width, (uint16_t)value);
        else s3c2400_write32(s, src + i * width, value);
    }
    s3c2400_write32(s, dma, src);
    s3c2400_write32(s, dma + 4u, dma + 24u);
    s3c2400_write32(s, dma + 8u, (1u << 28) | (whole << 26) |
                       (iis_request << 23) | (!reload << 22) | (dsz << 20) | 3u);
    s3c2400_write32(s, dma + 24u, iis_request ? 2u : 3u);
    if (iis_request) {
        s3c2400_write32(s, 0x14800004u, 0u);
        s3c2400_write32(s, 0x14800014u, 0u);
        s3c2400_write32(s, 0x15508000u, 1u);
        s3c2400_tick(s, 1000u); /* STOP on the first IIS unit, at cycle 500. */
    }
    uint32_t actual[] = {
        s3c2400_read32(s, dma + 12u), s3c2400_read32(s, dma + 16u),
        s3c2400_read32(s, dma + 20u), s3c2400_read32(s, dma + 24u),
        s3c2400_read32(s, 0x14400000u)
    };
    const uint32_t want[] = {0u, 0u, 0u, 4u, 0u};
    if (memcmp(actual, want, sizeof(want))) {
        fprintf(stderr, "FAIL: DMA%u self STOP dsz=%u whole=%u reload=%u IIS=%u count=%u src=%08x dst=%08x mask=%08x irq=%08x\n",
                ch, dsz, whole, reload, iis_request, actual[0], actual[1], actual[2], actual[3], actual[4]);
        ++failures;
    }
    /* A fresh operation after cancellation still owns its current registers
     * and emits its ordinary terminal IRQ. The cancellation is not sticky. */
    s3c2400_write32(s, dma + 24u, 4u);
    s3c2400_write32(s, 0x14400000u, UINT32_MAX);
    s3c2400_write32(s, src, 0x1234a5e7u);
    s3c2400_write32(s, dma, src);
    s3c2400_write32(s, dma + 4u, BASE + 0x1800u);
    s3c2400_write32(s, dma + 8u, (1u << 28) | (1u << 22) | (dsz << 20) | 1u);
    s3c2400_write32(s, dma + 24u, 3u);
    uint32_t mask = dsz == 0u ? 0xffu : dsz == 1u ? 0xffffu : UINT32_MAX;
    if (s3c2400_read32(s, BASE + 0x1800u) != (0x1234a5e7u & mask) ||
        s3c2400_read32(s, dma + 12u) != 0u ||
        s3c2400_read32(s, dma + 16u) != src + width ||
        s3c2400_read32(s, dma + 20u) != BASE + 0x1800u + width ||
        s3c2400_read32(s, dma + 24u) != 0u ||
        s3c2400_read32(s, 0x14400000u) != irq) {
        fprintf(stderr, "FAIL: DMA%u fresh operation after self STOP dsz=%u\n", ch, dsz);
        ++failures;
    }
    return failures;
}

/* Synchronous MMIO may trigger a second DMA. It can STOP the outer channel,
 * then re-enable it before the outer bus write returns. The old operation
 * has still been cancelled; it must leave the freshly loaded cursors alone.
 * This checks emulator reentrancy, not simultaneous silicon arbitration. */
static int nested_stop_case(s3c2400_t *s, unsigned whole,
                            unsigned reenable, unsigned cancel_outer) {
    const uint32_t dma = 0x14600000u, child = dma + 32u;
    const uint32_t src = BASE + 0x200u, command = BASE + 0x280u;
    const uint32_t stop = (cancel_outer ? dma : dma + 3u * 32u) + 24u;
    s3c2400_reset(s);
    s3c2400_write32(s, 0x14400008u, UINT32_MAX);
    s3c2400_write32(s, src, 3u); /* First outer unit triggers DMA1. */
    s3c2400_write32(s, src + 4u, 0x11111111u);
    s3c2400_write32(s, src + 8u, 0x22222222u);
    s3c2400_write32(s, command, 4u);
    s3c2400_write32(s, command + 4u, 2u);
    s3c2400_write32(s, child, command);
    s3c2400_write32(s, child + 4u, stop | (1u << 29));
    s3c2400_write32(s, child + 8u, 0x04600000u | (1u + reenable));
    s3c2400_write32(s, dma, src);
    s3c2400_write32(s, dma + 4u, child + 24u);
    s3c2400_write32(s, dma + 8u, (1u << 28) | (whole << 26) | 0x00600003u);
    s3c2400_write32(s, dma + 24u, 3u);
    const uint32_t moved = whole ? 3u : 1u;
    uint32_t want[] = {3u - moved, src + 4u * moved,
                       child + 24u + 4u * moved, whole ? 0u : 2u,
                       whole ? 1u << 17 : 0u,
                       whole ? 0x22222222u : 0u};
    if (cancel_outer) {
        want[0] = reenable ? 3u : 0u;
        want[1] = reenable ? src : 0u;
        want[2] = reenable ? child + 24u : 0u;
        want[3] = reenable ? 2u : 4u;
        want[4] = want[5] = 0u;
    }
    const uint32_t actual[] = {
        s3c2400_read32(s, dma + 12u), s3c2400_read32(s, dma + 16u),
        s3c2400_read32(s, dma + 20u), s3c2400_read32(s, dma + 24u),
        s3c2400_read32(s, 0x14400000u), s3c2400_read32(s, dma + 64u)
    };
    /* The child finishes independently even though it stopped DMA0/DMA3. */
    if (memcmp(actual, want, sizeof(want)) ||
        s3c2400_read32(s, child + 12u) != 0u ||
        s3c2400_read32(s, child + 16u) != command + 4u * (1u + reenable) ||
        s3c2400_read32(s, child + 20u) != stop ||
        s3c2400_read32(s, child + 24u) != 0u) {
        fprintf(stderr, "FAIL: nested STOP whole=%u reenable=%u cancel_outer=%u\n", whole, reenable, cancel_outer);
        for (unsigned i = 0; i < GP32_ARRAY_COUNT(want); ++i)
            if (actual[i] != want[i]) fprintf(stderr, " register %u=%08x expected %08x\n", i, actual[i], want[i]);
        return 1;
    }
    return 0;
}

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
    unsigned trigger_cases = 0, trigger_failures = 0;
    for (unsigned channel = 0; channel < 4u; ++channel)
        for (unsigned dsz = 0; dsz < 3u; ++dsz)
            for (unsigned access = 0; access < 3u; ++access)
                for (unsigned reload = 0; reload < 2u; ++reload) {
                    ++trigger_cases;
                    trigger_failures += trigger_case(s, channel, dsz, access, reload);
                }
    unsigned stop_cases = 0, stop_failures = 0;
    for (unsigned channel = 0; channel < 4u; ++channel)
        for (unsigned dsz = 0; dsz < 3u; ++dsz)
            for (unsigned whole = 0; whole < 2u; ++whole)
                for (unsigned reload = 0; reload < 2u; ++reload) {
                    ++stop_cases;
                    stop_failures += stop_case(s, channel, dsz, whole, reload, 0u);
                    if (channel == 2u) {
                        ++stop_cases;
                        stop_failures += stop_case(s, channel, dsz, whole, reload, 1u);
                    }
                }
    for (unsigned whole = 0; whole < 2u; ++whole)
        for (unsigned reenable = 0; reenable < 2u; ++reenable)
            for (unsigned cancel = 0; cancel < 2u; ++cancel) {
                ++stop_cases;
                stop_failures += nested_stop_case(s, whole, reenable, cancel);
            }
    s3c2400_destroy(s);
    printf("DMA oracle cases=%u failures=%u\n", cases, failures);
    printf("DMA software-trigger scenarios=%u assertion_failures=%u\n", trigger_cases, trigger_failures);
    printf("DMA in-flight STOP scenarios=%u assertion_failures=%u\n", stop_cases, stop_failures);
    return failures || trigger_failures || stop_failures ? 1 : 0;
}
