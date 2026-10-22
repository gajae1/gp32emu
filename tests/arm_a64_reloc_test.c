/* Standalone A64 instruction-placement tests; no A64 execution is required.
 * Build with -DA64_RELOC_TEST_LEGACY to expose the baseline scanner's partial
 * destination writes on a late rejection.  --failure-atomicity selects that
 * negative control without the rest of the placement contract.
 */
#include <inttypes.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "a64_reloc_reference.h"

#ifdef A64_RELOC_TEST_LEGACY
enum { A64_RELOC_B26 = 0, A64_RELOC_B19 = 1, A64_RELOC_B14 = 2 };
typedef struct { uint16_t at_kind, target; } a64_reloc_t;
static int a64_place(uint32_t *hot_dst, uint32_t *cold_dst, uint32_t *src,
                     size_t hot, size_t n, const a64_reloc_t *reloc,
                     unsigned count) {
    (void)reloc; (void)count;
    return a64_reloc_reference_place_legacy(hot_dst, cold_dst, src, hot, n);
}
#else
#ifndef A64_RELOC_TEST_SOURCE
#define A64_RELOC_TEST_SOURCE "../src/arm920t_jit_a64_reloc.h"
#endif
#include A64_RELOC_TEST_SOURCE
#endif

#define TEST_NOP UINT32_C(0xd503201f)
#define TEST_SENTINEL UINT32_C(0xa5a5a5a5)
#define TEST_MAX_WORDS 16384u

static unsigned failures, cases_run;
static char current_case[128];

static void expect(int condition, const char *what, uint64_t got, uint64_t want) {
    if (condition) return;
    if (failures++ < 24u)
        fprintf(stderr, "FAIL[%s] %s: got=%#" PRIx64 " want=%#" PRIx64 "\n",
                current_case, what, got, want);
}

/* Decode by architectural instruction families, independently of the
 * candidate's relocation records and implementation helpers. */
static unsigned branch_bits(uint32_t word, unsigned *shift) {
    unsigned top6 = word >> 26;
    if (top6 == 5u || top6 == 37u) { *shift = 0u; return 26u; }
    switch (word >> 24) {
    case 0x54u:
        if (word & 16u) return 0u;
        *shift = 5u; return 19u;
    case 0x34u: case 0x35u: case 0xb4u: case 0xb5u:
        *shift = 5u; return 19u;
    case 0x36u: case 0x37u: case 0xb6u: case 0xb7u:
        *shift = 5u; return 14u;
    default: return 0u;
    }
}

static int64_t branch_delta(uint32_t word, unsigned bits, unsigned shift) {
    uint32_t modulus = UINT32_C(1) << bits;
    int64_t raw = (word >> shift) & (modulus - 1u);
    return raw >= (int64_t)(modulus / 2u) ? raw - modulus : raw;
}

static uint32_t branch_word(uint32_t base, unsigned bits, int64_t delta) {
    unsigned shift = bits == 26u ? 0u : 5u;
    uint32_t field = (UINT32_C(1) << bits) - 1u;
    return (base & ~(field << shift)) |
           (((uint32_t)(uint64_t)delta & field) << shift);
}

static a64_reloc_t record(size_t at, unsigned kind, size_t target) {
    a64_reloc_t r = {(uint16_t)(at | ((size_t)kind << 14)), (uint16_t)target};
    return r;
}

static size_t physical_word(size_t logical, size_t hot,
                            size_t hot_at, size_t cold_at) {
    return logical < hot ? hot_at + logical : cold_at + logical - hot;
}

static int in_destination(size_t index, size_t hot, size_t n,
                           size_t hot_at, size_t cold_at) {
    return (index >= hot_at && index - hot_at < hot) ||
           (index >= cold_at && index - cold_at < n - hot);
}

/* Calculate targets from arena word positions, not by adjusting a shared
 * gap formula.  The one-past-end target belongs to the cold span. */
static int expected_words(uint32_t *out, const uint32_t *src, size_t hot,
                           size_t n, size_t hot_at, size_t cold_at) {
    for (size_t i = 0; i < n; ++i) {
        out[i] = src[i];
        unsigned shift = 0u, bits = branch_bits(src[i], &shift);
        if (!bits) continue;
        int64_t target = (int64_t)i + branch_delta(src[i], bits, shift);
        if (target < 0 || target > (int64_t)n) return 0;
        int64_t from = (int64_t)physical_word(i, hot, hot_at, cold_at);
        int64_t to = (int64_t)physical_word((size_t)target, hot, hot_at, cold_at);
        int64_t delta = to - from, bound = INT64_C(1) << (bits - 1u);
        if (delta < -bound || delta >= bound) return 0;
        if ((i < hot) != ((size_t)target < hot))
            out[i] = branch_word(src[i], bits, delta);
    }
    return 1;
}

static void run_case(const char *name, const uint32_t *src, size_t hot,
                     size_t n, const a64_reloc_t *reloc, unsigned count,
                     uint32_t *arena, size_t arena_words,
                     size_t hot_at, size_t cold_at, int want_success) {
    snprintf(current_case, sizeof(current_case), "%s", name);
    ++cases_run;
    if (hot > n || !hot_at || !cold_at || hot_at + hot >= arena_words ||
        cold_at + n - hot >= arena_words ||
        (hot && n > hot && hot_at < cold_at + n - hot && cold_at < hot_at + hot)) {
        expect(0, "invalid test geometry", hot, n);
        return;
    }
    size_t bytes = (n ? n : 1u) * sizeof(uint32_t);
    uint32_t *scratch = malloc(bytes), *expected = malloc(bytes);
    if (!scratch || !expected) { fputs("test allocation failed\n", stderr); exit(2); }
    if (n) memcpy(scratch, src, n * sizeof(*src));
    if (want_success && !expected_words(expected, src, hot, n, hot_at, cold_at)) {
        expect(0, "independent target calculation accepts fixture", 0u, 1u);
        free(expected);
        free(scratch);
        return;
    }
    for (size_t i = 0; i < n; ++i)
        arena[physical_word(i, hot, hot_at, cold_at)] = TEST_SENTINEL;
    size_t guards[] = {hot_at - 1u, hot_at + hot,
                       cold_at - 1u, cold_at + n - hot};
    for (unsigned k = 0; k < 4u; ++k)
        if (!in_destination(guards[k], hot, n, hot_at, cold_at))
            arena[guards[k]] = TEST_SENTINEL;

    int got = a64_place(arena + hot_at, arena + cold_at, scratch,
                         hot, n, reloc, count);
    expect(!!got == !!want_success, "placement result", !!got, !!want_success);
    for (size_t i = 0; i < n; ++i) {
        uint32_t word = arena[physical_word(i, hot, hot_at, cold_at)];
        uint32_t want = want_success ? expected[i] : TEST_SENTINEL;
        expect(word == want, want_success ? "placed word" : "rejected destination unchanged",
               word, want);
        if (want_success && got) {
            unsigned shift = 0u, bits = branch_bits(src[i], &shift);
            if (bits) {
                int64_t logical_target = (int64_t)i + branch_delta(src[i], bits, shift);
                int64_t actual_target = (int64_t)physical_word(i, hot, hot_at, cold_at) +
                                        branch_delta(word, bits, shift);
                int64_t wanted_target = (int64_t)physical_word((size_t)logical_target,
                                                               hot, hot_at, cold_at);
                expect(actual_target == wanted_target, "architectural branch target",
                       (uint64_t)actual_target, (uint64_t)wanted_target);
            }
        }
    }
    for (unsigned k = 0; k < 4u; ++k)
        if (!in_destination(guards[k], hot, n, hot_at, cold_at))
            expect(arena[guards[k]] == TEST_SENTINEL, "destination guard",
                   arena[guards[k]], TEST_SENTINEL);

    /* Also compare ordinary-size successful cases to the fixed old scanner.
     * The target arithmetic above remains the primary independent oracle. */
    if (want_success && arena_words <= 65536u) {
        uint32_t *old = malloc(arena_words * sizeof(*old));
        if (!old) exit(2);
        expect(a64_reloc_reference_place_legacy(old + hot_at, old + cold_at,
                                                 src, hot, n),
               "baseline accepts valid fixture", 0u, 1u);
        for (size_t i = 0; i < n; ++i)
            expect(old[physical_word(i, hot, hot_at, cold_at)] == expected[i],
                   "baseline and independent expectation agree",
                   old[physical_word(i, hot, hot_at, cold_at)], expected[i]);
        free(old);
    }
    free(expected);
    free(scratch);
}

static void test_failure_atomicity(void) {
    uint32_t arena[16416], src[8];
    for (unsigned i = 0; i < 8u; ++i) src[i] = TEST_NOP;
    /* Three valid hot words precede an unrepresentable hot-to-cold TBZ. */
    src[3] = branch_word(UINT32_C(0x36280011), 14u, 3);
    a64_reloc_t reloc = record(3u, A64_RELOC_B14, 6u);
    run_case("late-range-symbolic", src, 4u, 8u, &reloc, 1u,
             arena, 16416u, 8u, 16384u, 0);
    run_case("late-range-scanner", src, 4u, 8u, NULL, UINT_MAX,
             arena, 16416u, 8u, 16384u, 0);
    src[3] = TEST_NOP;
    /* The historical scanner has already copied the whole hot span and two
     * cold words before discovering this impossible logical target. */
    src[6] = branch_word(UINT32_C(0x14000000), 26u, 3);
    run_case("late-invalid-target-scanner", src, 4u, 8u, NULL, UINT_MAX,
             arena, 16416u, 8u, 24u, 0);
}

typedef struct branch_variant {
    const char *name;
    uint32_t opcode;
    unsigned bits, kind;
} branch_variant_t;

/* Register, condition, bit-number, link, nonzero and width bits deliberately
 * vary.  Word comparisons preserve every bit outside the immediate field. */
static const branch_variant_t variants[] = {
    {"B",       UINT32_C(0x14000000), 26u, A64_RELOC_B26},
    {"BL",      UINT32_C(0x94000000), 26u, A64_RELOC_B26},
    {"B.EQ",    UINT32_C(0x54000000), 19u, A64_RELOC_B19},
    {"B.LE",    UINT32_C(0x5400000d), 19u, A64_RELOC_B19},
    {"CBZ.W",   UINT32_C(0x34000003), 19u, A64_RELOC_B19},
    {"CBNZ.W",  UINT32_C(0x3500001d), 19u, A64_RELOC_B19},
    {"CBZ.X",   UINT32_C(0xb400001f), 19u, A64_RELOC_B19},
    {"CBNZ.X",  UINT32_C(0xb500000e), 19u, A64_RELOC_B19},
    {"TBZ.W",   UINT32_C(0x3628001f), 14u, A64_RELOC_B14},
    {"TBNZ.W",  UINT32_C(0x37f8000b), 14u, A64_RELOC_B14},
    {"TBZ.X",   UINT32_C(0xb618000a), 14u, A64_RELOC_B14},
    {"TBNZ.X",  UINT32_C(0xb7f80012), 14u, A64_RELOC_B14},
};
#define VARIANT_COUNT (sizeof(variants) / sizeof(variants[0]))

static void test_branch_families(void) {
    uint32_t arena[96], src[8];
    /* Both crossing directions, split target, one-past-end target, last-word
     * source, and same-span branches in each part. */
    static const size_t positions[][2] = {
        {1u, 6u}, {6u, 1u}, {1u, 4u}, {6u, 4u},
        {3u, 8u}, {7u, 8u}, {7u, 0u}, {0u, 3u}, {4u, 7u}
    };
    static const size_t placements[][2] = {{8u, 24u}, {24u, 8u}, {8u, 12u}};
    char name[112];
    for (size_t v = 0; v < VARIANT_COUNT; ++v)
        for (unsigned p = 0; p < sizeof(positions) / sizeof(positions[0]); ++p)
            for (unsigned layout = 0; layout < 3u; ++layout)
                for (unsigned fallback = 0; fallback < 2u; ++fallback) {
                    for (unsigned i = 0; i < 8u; ++i) src[i] = TEST_NOP;
                    size_t at = positions[p][0], target = positions[p][1];
                    src[at] = branch_word(variants[v].opcode, variants[v].bits,
                                          (int64_t)target - (int64_t)at);
                    a64_reloc_t reloc = record(at, variants[v].kind, target);
                    snprintf(name, sizeof(name), "%s-at%zu-to%zu-layout%u-%s",
                             variants[v].name, at, target, layout,
                             fallback ? "scanner" : "symbolic");
                    run_case(name, src, 4u, 8u, fallback ? NULL : &reloc,
                             fallback ? UINT_MAX : 1u, arena, 96u,
                             placements[layout][0], placements[layout][1], 1);
                }
}

static void test_signed_boundaries(void) {
    /* A B26 displacement reaches 128 MiB.  Reserve one ordinary allocation
     * and touch only the two short destination spans; no executable mapping
     * or instruction execution is needed, on either x64 or A64. */
    const size_t arena_words = (UINT32_C(1) << 25) + 64u;
    uint32_t *arena = malloc(arena_words * sizeof(*arena));
    if (!arena) { fputs("boundary allocation failed\n", stderr); exit(2); }
    uint32_t src[8];
    char name[112];
    for (size_t v = 0; v < VARIANT_COUNT; ++v) {
        int64_t bound = INT64_C(1) << (variants[v].bits - 1u);
        int64_t displacements[] = {bound - 1, -bound, bound, -bound - 1};
        for (unsigned source_cold = 0; source_cold < 2u; ++source_cold)
            for (unsigned edge = 0; edge < 4u; ++edge)
                for (unsigned fallback = 0; fallback < 2u; ++fallback) {
                    int64_t delta = displacements[edge];
                    /* at=1,target=5 => delta=cold_at-hot_at;
                     * at=6,target=2 => delta=hot_at-cold_at. */
                    size_t at = source_cold ? 6u : 1u;
                    size_t target = source_cold ? 2u : 5u;
                    int64_t distance = source_cold ? -delta : delta;
                    size_t hot_at = distance < 0 ? 8u + (size_t)-distance : 8u;
                    size_t cold_at = distance > 0 ? 8u + (size_t)distance : 8u;
                    for (unsigned i = 0; i < 8u; ++i) src[i] = TEST_NOP;
                    src[at] = branch_word(variants[v].opcode, variants[v].bits,
                                          (int64_t)target - (int64_t)at);
                    a64_reloc_t reloc = record(at, variants[v].kind, target);
                    snprintf(name, sizeof(name), "%s-boundary-%" PRId64 "-%s-%s",
                             variants[v].name, delta, source_cold ? "cold" : "hot",
                             fallback ? "scanner" : "symbolic");
                    run_case(name, src, 4u, 8u, fallback ? NULL : &reloc,
                             fallback ? UINT_MAX : 1u, arena, arena_words,
                             hot_at, cold_at, edge < 2u);
                }
    }
    free(arena);
}

static void test_invalid_metadata(void) {
    uint32_t arena[64], src[8];
    for (unsigned i = 0; i < 8u; ++i) src[i] = TEST_NOP;
    src[1] = branch_word(UINT32_C(0x5400000d), 19u, 5);
    src[6] = branch_word(UINT32_C(0x94000000), 26u, -6);
    a64_reloc_t reloc[] = {record(1u, A64_RELOC_B19, 6u),
                           record(6u, A64_RELOC_B26, 9u)};
    run_case("metadata-late-target-after-end", src, 4u, 8u, reloc, 2u,
             arena, 64u, 8u, 24u, 0);
    reloc[1] = record(8u, A64_RELOC_B26, 0u);
    run_case("metadata-source-at-end", src, 4u, 8u, reloc, 2u,
             arena, 64u, 8u, 24u, 0);
    reloc[1] = record(16383u, A64_RELOC_B26, 0u);
    run_case("metadata-source-max-outside-block", src, 4u, 8u, reloc, 2u,
             arena, 64u, 8u, 24u, 0);
    reloc[1] = record(6u, 3u, 0u);
    run_case("metadata-reserved-kind", src, 4u, 8u, reloc, 2u,
             arena, 64u, 8u, 24u, 0);
    reloc[1] = record(6u, A64_RELOC_B26, UINT16_MAX);
    run_case("metadata-target-max-outside-block", src, 4u, 8u, reloc, 2u,
             arena, 64u, 8u, 24u, 0);

    /* Invalid global geometry must reject before reading source or copying
     * either span.  Source is deliberately short for the oversized case. */
    const size_t bad_hot[] = {9u, 4u}, bad_n[] = {8u, TEST_MAX_WORDS + 1u};
    for (unsigned k = 0; k < 2u; ++k)
        for (unsigned fallback = 0; fallback < 2u; ++fallback) {
            snprintf(current_case, sizeof(current_case), "%s",
                     k ? "oversized-block" : "split-after-block");
            ++cases_run;
            for (unsigned i = 0; i < 64u; ++i) arena[i] = TEST_SENTINEL;
            int result = a64_place(arena + 8u, arena + 24u, src,
                                   bad_hot[k], bad_n[k], NULL,
                                   fallback ? UINT_MAX : 0u);
            expect(!result, "invalid geometry rejected", result, 0u);
            for (unsigned i = 0; i < 64u; ++i)
                expect(arena[i] == TEST_SENTINEL, "invalid geometry leaves destinations",
                       arena[i], TEST_SENTINEL);
        }
}

static void test_empty_spans_and_nonbranches(void) {
    uint32_t arena[64];
    const uint32_t nonbranches[] = {
        TEST_NOP, UINT32_C(0x54000010), UINT32_C(0xd65f03c0),
        UINT32_C(0xd63f0200), UINT32_C(0x91000400), UINT32_C(0x90000001),
        UINT32_C(0xaa0103e2), UINT32_C(0xf9400263)
    };
    for (unsigned fallback = 0; fallback < 2u; ++fallback) {
        unsigned count = fallback ? UINT_MAX : 0u;
        run_case("empty-block", nonbranches, 0u, 0u, NULL, count,
                 arena, 64u, 8u, 24u, 1);
        run_case("empty-hot-span", nonbranches, 0u, 8u, NULL, count,
                 arena, 64u, 8u, 24u, 1);
        run_case("empty-cold-span", nonbranches, 8u, 8u, NULL, count,
                 arena, 64u, 8u, 24u, 1);
        run_case("nonbranch-bit-patterns", nonbranches, 4u, 8u, NULL, count,
                 arena, 64u, 8u, 24u, 1);
    }
    uint32_t src[8];
    for (unsigned i = 0; i < 8u; ++i) src[i] = TEST_NOP;
    src[7] = branch_word(UINT32_C(0x14000000), 26u, 1);
    a64_reloc_t reloc = record(7u, A64_RELOC_B26, 8u);
    run_case("empty-cold-one-past-target", src, 8u, 8u, &reloc, 1u,
             arena, 64u, 8u, 24u, 1);
    run_case("empty-cold-one-past-target-scanner", src, 8u, 8u, NULL, UINT_MAX,
             arena, 64u, 8u, 24u, 1);
}

static void test_many_relocations(void) {
    enum { N = 1200, HOT = 600, ARENA_WORDS = 4096 };
    uint32_t *src = malloc(N * sizeof(*src));
    uint32_t *arena = malloc(ARENA_WORDS * sizeof(*arena));
    a64_reloc_t *reloc = malloc(N * sizeof(*reloc));
    if (!src || !arena || !reloc) exit(2);
    for (unsigned i = 0; i < N; ++i) {
        size_t target = i < HOT ? i + HOT : i - HOT;
        const branch_variant_t *v = &variants[i % VARIANT_COUNT];
        src[i] = branch_word(v->opcode, v->bits, (int64_t)target - i);
        reloc[i] = record(i, v->kind, target);
    }
    /* More than the old FIX_CAP=384 crossing edges exercise the explicit
     * bounded-ledger overflow scanner route. */
    run_case("1200-crossing-edges-symbolic", src, HOT, N, reloc, N,
             arena, ARENA_WORDS, 8u, 1000u, 1);
    run_case("1200-crossing-edges-forced-overflow", src, HOT, N, NULL, UINT_MAX,
             arena, ARENA_WORDS, 8u, 1000u, 1);
    src[N - 1] = branch_word(UINT32_C(0x14000000), 26u, 2);
    run_case("overflow-scanner-late-invalid-target", src, HOT, N, NULL, UINT_MAX,
             arena, ARENA_WORDS, 8u, 1000u, 0);
    free(reloc); free(arena); free(src);
}

static void test_maximum_indices(void) {
    const size_t n = TEST_MAX_WORDS, hot = TEST_MAX_WORDS / 2u;
    const size_t arena_words = 2u * TEST_MAX_WORDS + 64u;
    uint32_t *src = malloc(n * sizeof(*src));
    uint32_t *arena = malloc(arena_words * sizeof(*arena));
    if (!src || !arena) exit(2);
    for (size_t i = 0; i < n; ++i) src[i] = TEST_NOP;
    src[0] = branch_word(UINT32_C(0x94000000), 26u, (int64_t)n);
    src[hot - 1u] = branch_word(UINT32_C(0x37f8000b), 14u, 1);
    src[hot] = branch_word(UINT32_C(0xb618000a), 14u, -1);
    src[n - 1u] = branch_word(UINT32_C(0x3500001d), 19u, -(int64_t)(n - 1u));
    a64_reloc_t reloc[] = {
        record(0u, A64_RELOC_B26, n), record(hot - 1u, A64_RELOC_B14, hot),
        record(hot, A64_RELOC_B14, hot - 1u), record(n - 1u, A64_RELOC_B19, 0u)
    };
    run_case("maximum-word-indices", src, hot, n, reloc, 4u,
             arena, arena_words, 8u, hot + 24u, 1);
    run_case("maximum-word-indices-scanner", src, hot, n, NULL, UINT_MAX,
             arena, arena_words, 8u, hot + 24u, 1);
    run_case("maximum-all-cold", src, 0u, n, reloc, 4u,
             arena, arena_words, 8u, 24u, 1);
    run_case("maximum-all-hot", src, n, n, reloc, 4u,
             arena, arena_words, 8u, n + 24u, 1);
    free(arena); free(src);
}

static uint32_t random_state = UINT32_C(0x92c0a64d);
static uint32_t random_word(void) {
    uint32_t x = random_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return random_state = x;
}

static void test_random_splits(void) {
    enum { MAX_N = 256, ARENA_WORDS = 9000, RANDOM_CASES = 2000 };
    uint32_t src[MAX_N], arena[ARENA_WORDS];
    a64_reloc_t reloc[MAX_N];
    char name[80];
    for (unsigned iteration = 0; iteration < RANDOM_CASES; ++iteration) {
        size_t n = 1u + random_word() % MAX_N;
        size_t hot = random_word() % (n + 1u);
        size_t separation = random_word() % 4096u;
        unsigned reverse = random_word() & 1u;
        size_t hot_at = reverse ? 8u + (n - hot) + separation : 8u;
        size_t cold_at = reverse ? 8u : 8u + hot + separation;
        unsigned count = 0;
        for (size_t i = 0; i < n; ++i) {
            if ((random_word() & 3u) != 0u) {
                src[i] = TEST_NOP ^ ((random_word() & 7u) << 5);
                continue;
            }
            const branch_variant_t *v = &variants[random_word() % VARIANT_COUNT];
            size_t target = random_word() % (n + 1u);
            src[i] = branch_word(v->opcode, v->bits, (int64_t)target - (int64_t)i);
            reloc[count++] = record(i, v->kind, target);
        }
        for (unsigned fallback = 0; fallback < 2u; ++fallback) {
            snprintf(name, sizeof(name), "seed92c0a64d-random%u-%s", iteration,
                     fallback ? "scanner" : "symbolic");
            run_case(name, src, hot, n, fallback ? NULL : reloc,
                     fallback ? UINT_MAX : count, arena, ARENA_WORDS,
                     hot_at, cold_at, 1);
        }
    }
}

int main(int argc, char **argv) {
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "--failure-atomicity"))) {
        fputs("usage: arm_a64_reloc_test [--failure-atomicity]\n", stderr);
        return 2;
    }
    test_failure_atomicity();
    if (argc == 1) {
        test_branch_families();
        test_signed_boundaries();
        test_invalid_metadata();
        test_empty_spans_and_nonbranches();
        test_many_relocations();
        test_maximum_indices();
        test_random_splits();
    }
    printf("A64 relocation: cases=%u failures=%u\n", cases_run, failures);
    return failures ? 1 : 0;
}
