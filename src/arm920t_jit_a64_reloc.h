/* Private, host-independent placement for the A64 emitter.
 *
 * The emitter knows branch labels before encoding their immediates. Retain
 * only edges crossing the hot/cold split instead of rediscovering those labels
 * from every branch at placement. Source words are disposable scratch: on a
 * rejected relocation some may already have changed, but neither destination
 * span has been written. Never retry placement with that changed scratch.
 */
#ifndef GP32EMU_ARM920T_JIT_A64_RELOC_H
#define GP32EMU_ARM920T_JIT_A64_RELOC_H

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { A64_RELOC_B26 = 0, A64_RELOC_B19, A64_RELOC_B14 };
enum { A64_RELOC_INDEX_BITS = 14, A64_RELOC_MAX_WORDS = 1u << 14 };
typedef struct a64_reloc {
    uint16_t at_kind; /* low 14 bits: source word; high 2 bits: immediate kind */
    uint16_t target;  /* may equal n, including n == A64_RELOC_MAX_WORDS */
} a64_reloc_t;
_Static_assert(sizeof(a64_reloc_t) == 4u, "compact split relocation");

static inline int a64_reloc_word(uint32_t word, unsigned kind, int64_t delta,
                                  uint32_t *out) {
    if (kind > A64_RELOC_B14) return 0;
    unsigned bits = kind == A64_RELOC_B26 ? 26u : kind == A64_RELOC_B19 ? 19u : 14u;
    unsigned shift = kind == A64_RELOC_B26 ? 0u : 5u;
    int64_t limit = INT64_C(1) << (bits - 1u);
    if (delta < -limit || delta >= limit) return 0;
    uint32_t mask = ((UINT32_C(1) << bits) - 1u) << shift;
    *out = (word & ~mask) | (((uint32_t)delta << shift) & mask);
    return 1;
}

/* The bounded record buffer can overflow for large cold chains. Preserve
 * native coverage by scanning the ORIGINAL contiguous scratch in that case.
 * Each word is visited once; earlier rewritten words are never decoded again.
 * External runtime placeholders have a zero immediate and stay in their span.
 */
static int a64_reloc_scan(uint32_t *src, size_t hot, size_t n, int64_t gap) {
    for (size_t i = 0; i < n; ++i) {
        uint32_t w = src[i];
        if ((w & 0x1c000000u) != 0x14000000u) continue;
        unsigned kind, bits, shift;
        if ((w & 0x7c000000u) == 0x14000000u) {
            kind = A64_RELOC_B26; bits = 26u; shift = 0u;
        } else if ((w & 0xff000010u) == 0x54000000u ||
                   (w & 0x7e000000u) == 0x34000000u) {
            kind = A64_RELOC_B19; bits = 19u; shift = 5u;
        } else if ((w & 0x7e000000u) == 0x36000000u) {
            kind = A64_RELOC_B14; bits = 14u; shift = 5u;
        } else continue;
        uint32_t mask = (UINT32_C(1) << bits) - 1u;
        int64_t imm = (int64_t)((w >> shift) & mask);
        if (imm & (INT64_C(1) << (bits - 1u))) imm -= INT64_C(1) << bits;
        int64_t target = (int64_t)i + imm;
        if (target < 0 || target > (int64_t)n) return 0;
        if ((i < hot) == ((size_t)target < hot)) continue;
        int64_t delta = imm + (i < hot ? gap : -gap);
        if (!a64_reloc_word(w, kind, delta, &src[i])) return 0;
    }
    return 1;
}

/* Both destinations are aligned, disjoint spans within one arena; src and the
 * initialized record prefix are separate private storage. All addresses here
 * are from the RW view. The RX view has the same relative layout.
 *
 * UINT_MAX selects the overflow scanner BEFORE any scratch is rewritten.
 * Otherwise only the supplied crossing records need work. The encoding kind
 * is a trusted emitter property, not inferred again from the instruction.
 */
static int a64_place(uint32_t *hot_dst, uint32_t *cold_dst, uint32_t *src,
                     size_t hot, size_t n, const a64_reloc_t *reloc, unsigned count) {
    if (hot > n || n > A64_RELOC_MAX_WORDS) return 0;
    int64_t gap = ((intptr_t)cold_dst - (intptr_t)hot_dst) / 4 - (int64_t)hot;
    if (count == UINT_MAX) {
        if (!a64_reloc_scan(src, hot, n, gap)) return 0;
    } else {
        for (unsigned k = 0; k < count; ++k) {
            size_t at = reloc[k].at_kind & (A64_RELOC_MAX_WORDS - 1u);
            size_t target = reloc[k].target;
            unsigned kind = reloc[k].at_kind >> A64_RELOC_INDEX_BITS;
            if (at >= n || target > n || kind > A64_RELOC_B14) return 0;
            if ((at < hot) == (target < hot)) continue;
            int64_t delta = (int64_t)target - (int64_t)at + (at < hot ? gap : -gap);
            if (!a64_reloc_word(src[at], kind, delta, &src[at])) return 0;
        }
    }
    /* No failure is possible after the first arena write. Scratch is now in
     * the final split layout; do not decode or relocate it a second time. */
    memcpy(hot_dst, src, hot * sizeof(*src));
    memcpy(cold_dst, src + hot, (n - hot) * sizeof(*src));
    return 1;
}

#endif /* GP32EMU_ARM920T_JIT_A64_RELOC_H */
