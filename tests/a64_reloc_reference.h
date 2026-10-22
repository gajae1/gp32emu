/* Baseline placement scanner, retained only as an independent test oracle.
 * Extracted from arm920t_jit_a64.inc at
 * 0b64950558c3719868c00f7fe73643c254bfddba.  Unlike the candidate placement
 * contract, this historical scanner can write part of a destination before
 * rejecting a later branch.  Call it only with private destination storage.
 */
#ifndef GP32_TEST_A64_RELOC_REFERENCE_H
#define GP32_TEST_A64_RELOC_REFERENCE_H

#include <stddef.h>
#include <stdint.h>

static int a64_reloc_reference_place_legacy(uint32_t *hot_dst,
                                           uint32_t *cold_dst,
                                           const uint32_t *src,
                                           size_t hot, size_t n) {
    intptr_t gap = ((intptr_t)cold_dst - (intptr_t)hot_dst) / 4 - (intptr_t)hot;
    for (unsigned part = 0; part < 2u; ++part) {
        size_t first = part ? hot : 0u, end = part ? n : hot;
        uint32_t *dst = part ? cold_dst : hot_dst;
        for (size_t i = first; i < end; ++i) {
            uint32_t w = src[i];
            if ((w & 0x1c000000u) == 0x14000000u) {
                uint32_t mask;
                unsigned bits;
                if ((w & 0x7c000000u) == 0x14000000u) {
                    bits = 26; mask = 0x03ffffffu;
                } else if ((w & 0xff000010u) == 0x54000000u ||
                           (w & 0x7e000000u) == 0x34000000u) {
                    bits = 19; mask = 0x7ffffu << 5;
                } else if ((w & 0x7e000000u) == 0x36000000u) {
                    bits = 14; mask = 0x3fffu << 5;
                } else {
                    bits = 0; mask = 0;
                }
                if (bits) {
                    unsigned shift = bits == 26 ? 0u : 5u;
                    int64_t imm = (int64_t)((w & mask) >> shift);
                    if (imm & ((int64_t)1 << (bits - 1)))
                        imm -= (int64_t)1 << bits;
                    int64_t t = (int64_t)i + imm;
                    if (t < 0 || t > (int64_t)n) return 0;
                    if ((i < hot) != ((size_t)t < hot)) {
                        int64_t d = imm + (part ? -gap : gap);
                        if (d >= ((int64_t)1 << (bits - 1)) ||
                            d < -((int64_t)1 << (bits - 1))) return 0;
                        w = (w & ~mask) | (((uint32_t)d << shift) & mask);
                    }
                }
            }
            dst[i - first] = w;
        }
    }
    return 1;
}

#endif
