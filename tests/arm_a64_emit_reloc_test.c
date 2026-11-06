/* Check every real A64 emitter placement before running the CPU differential
 * fixtures. The reference receives the original contiguous instruction stream
 * and an independently allocated destination with the same hot/cold gap.
 * Missing relocation records therefore change observable final code bytes,
 * even when a particular guest fixture does not execute the affected edge.
 *
 * --reloc-negative-control deliberately drops one recorded relocation. It is
 * expected to exit nonzero and defaults to the small --bx-native fixture.
 * This hook exists only in this test translation unit.
 */
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif

#include <inttypes.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if (defined(__aarch64__) || defined(_M_ARM64)) && !defined(__AARCH64EB__) && \
    (!defined(GP32EMU_DISABLE_JIT) || !GP32EMU_DISABLE_JIT)

/* The production header's include guard keeps the implementation below from
 * being included a second time by arm920t.c. Only its name is substituted;
 * production code calls the observing wrapper defined in this file. */
#define a64_place a64_emit_candidate_place
#include "../src/arm920t_jit_a64_reloc.h"
#undef a64_place
#include "a64_reloc_reference.h"

static uint64_t emit_placements, emit_symbolic, emit_scanner, emit_records;
static uint64_t emit_successes, emit_rejections, emit_mismatches;
static unsigned emit_max_records;
static int emit_negative_control, emit_negative_injected;
static uint32_t *emit_original, *emit_mirror;
static size_t emit_original_cap, emit_mirror_cap;

static void emit_test_storage(uint32_t **storage, size_t *capacity, size_t words) {
    if (words <= *capacity) return;
    if (words > SIZE_MAX / sizeof(**storage)) {
        fputs("FAIL: A64 relocation fixture storage size overflow\n", stderr);
        exit(2);
    }
    uint32_t *larger = realloc(*storage, words * sizeof(**storage));
    if (!larger) {
        fputs("FAIL: A64 relocation fixture allocation\n", stderr);
        exit(2);
    }
    *storage = larger;
    *capacity = words;
}

static void emit_reloc_mismatch(const char *reason, size_t hot, size_t n,
                                size_t gap, unsigned count) {
    if (emit_mismatches < 12u)
        fprintf(stderr,
                "FAIL: emitted A64 relocation #%" PRIu64 ": %s "
                "hot=%zu words=%zu gap=%zu records=%u\n",
                emit_placements, reason, hot, n, gap, count);
    ++emit_mismatches;
}

static int a64_place(uint32_t *hot_dst, uint32_t *cold_dst, uint32_t *src,
                     size_t hot, size_t n, const a64_reloc_t *relocs,
                     unsigned count) {
    ++emit_placements;
    if (count == UINT_MAX) ++emit_scanner;
    else {
        ++emit_symbolic;
        emit_records += count;
        if (count > emit_max_records) emit_max_records = count;
    }

    /* Production places hot before cold inside one arena chunk. Checking the
     * envelope also prevents a malformed placement from sizing test storage. */
    uintptr_t hot_address = (uintptr_t)hot_dst;
    uintptr_t cold_address = (uintptr_t)cold_dst;
    if (hot > n || cold_address < hot_address ||
        (cold_address - hot_address) % sizeof(*src)) {
        emit_reloc_mismatch("invalid destination layout", hot, n, 0, count);
        return 0;
    }
    size_t gap = (size_t)((cold_address - hot_address) / sizeof(*src));
    if (gap < hot || gap > SIZE_MAX - (n - hot)) {
        emit_reloc_mismatch("overlapping destination spans", hot, n, gap, count);
        return 0;
    }
    emit_test_storage(&emit_original, &emit_original_cap, n);
    emit_test_storage(&emit_mirror, &emit_mirror_cap, gap + n - hot);
    memcpy(emit_original, src, n * sizeof(*src));

    int expected = a64_reloc_reference_place_legacy(
        emit_mirror, emit_mirror + gap, emit_original, hot, n);
    unsigned candidate_count = count;
    if (emit_negative_control && !emit_negative_injected && count && count != UINT_MAX) {
        --candidate_count;
        emit_negative_injected = 1;
    }
    int actual = a64_emit_candidate_place(
        hot_dst, cold_dst, src, hot, n, relocs, candidate_count);

    if (!!actual != !!expected) {
        emit_reloc_mismatch("success differs from reference", hot, n, gap, count);
        return 0;
    }
    if (!actual) {
        ++emit_rejections;
        return 0;
    }
    if (memcmp(hot_dst, emit_mirror, hot * sizeof(*src)) ||
        memcmp(cold_dst, emit_mirror + gap, (n - hot) * sizeof(*src))) {
        emit_reloc_mismatch("final code bytes differ from reference", hot, n, gap, count);
        /* Never execute code already known to contain a wrong branch. */
        return 0;
    }
    ++emit_successes;
    return 1;
}

#define main a64_emit_differential_main
#include "arm_jit_test.c"
#undef main

/* The full guest fixtures exercise ordinary blocks. Reach the record limit
 * deliberately through the real emitter entry points as well: both a forward
 * patch and a cold backward branch must overflow to the scanner without
 * overwriting the record array or losing a branch. These words are compared,
 * never executed. */
static int emit_capacity_cases(void) {
    int prior_failures = failures;
    uint64_t prior_symbolic = emit_symbolic, prior_scanner = emit_scanner;
    unsigned cases = 0;
    current_case = "a64-reloc-recorder-capacity";
    for (unsigned backwards = 0; backwards < 2u; ++backwards) {
        for (unsigned overflow = 0; overflow < 2u; ++overflow) {
            uint32_t scratch[A64_RELOC_MAX + 2u];
            uint32_t arena[4096u + A64_RELOC_MAX + 2u];
            struct {
                uint32_t before;
                a64_reloc_t records[A64_RELOC_MAX];
                uint32_t after;
            } guarded;
            memset(&guarded, 0xa5, sizeof(guarded));
            a64_emit_t e = {0};
            e.code = scratch;
            e.cap = sizeof(scratch) / sizeof(scratch[0]);
            e.reloc = guarded.records;
            unsigned edges = A64_RELOC_MAX + overflow;
            if (backwards) {
                a64_u32(&e, 0xd503201fu); /* hot NOP */
                e.hot_end = e.pos;
                for (unsigned i = 0; i < edges; ++i) a64_branch_hot(&e, 0);
            } else {
                for (unsigned i = 0; i < edges; ++i) (void)a64_jump(&e, 1u);
                e.hot_end = e.pos;
                a64_u32(&e, 0xd503201fu); /* cold NOP */
                for (unsigned i = 0; i < edges; ++i) a64_patch_cold(&e, i, e.hot_end);
            }
            unsigned expected_count = overflow ? UINT_MAX : A64_RELOC_MAX;
            CHECK(!e.fail, "capacity fixture unexpectedly failed emission");
            CHECK(e.reloc_count == expected_count,
                  "recorder must fill its last slot before selecting overflow");
            CHECK(guarded.before == UINT32_C(0xa5a5a5a5) &&
                  guarded.after == UINT32_C(0xa5a5a5a5),
                  "relocation recorder wrote outside its bounded array");
            if (!e.fail && e.reloc_count == expected_count) {
                CHECK(a64_place(arena, arena + 4096u, scratch, e.hot_end, e.pos,
                                guarded.records, e.reloc_count),
                      "capacity fixture final bytes differ from reference");
            }
            ++cases;
        }
    }
    CHECK(emit_symbolic - prior_symbolic == 2u && emit_scanner - prior_scanner == 2u,
          "capacity cases did not exercise both placement modes");
    printf("A64 recorder capacity: cases=%u capacity=%u symbolic=%" PRIu64
           " scanner=%" PRIu64 "\n", cases, A64_RELOC_MAX,
           emit_symbolic - prior_symbolic, emit_scanner - prior_scanner);
    return failures == prior_failures;
}

int main(int argc, char **argv) {
    char *negative_args[] = {argv[0], "--bx-native", NULL};
    if (argc > 1 && !strcmp(argv[1], "--reloc-negative-control")) {
        emit_negative_control = 1;
        --argc;
        ++argv;
        if (argc == 1) {
            argc = 2;
            argv = negative_args;
        }
    }

    int status = a64_emit_differential_main(argc, argv);
    if (!emit_placements || !emit_symbolic || !emit_records || !emit_successes) {
        fputs("FAIL: A64 emitter relocation coverage was not exercised\n", stderr);
        status = 1;
    }
    if (!emit_capacity_cases()) status = 1;
    if (emit_negative_control && !emit_negative_injected) {
        fputs("FAIL: A64 relocation negative control was not injected\n", stderr);
        status = 1;
    }
    if (emit_mismatches) status = 1;
    printf("A64 emitted relocation: placements=%" PRIu64 " symbolic=%" PRIu64
           " scanner=%" PRIu64 " records=%" PRIu64 " max_records=%u"
           " successful=%" PRIu64 " rejected=%" PRIu64 " mismatches=%" PRIu64
           " negative_injected=%d\n",
           emit_placements, emit_symbolic, emit_scanner, emit_records,
           emit_max_records, emit_successes, emit_rejections, emit_mismatches,
           emit_negative_injected);
    free(emit_original);
    free(emit_mirror);
    return status;
}

#else

int main(void) {
    puts("SKIP: A64 emitter relocation integration requires a native A64 backend");
    return 0;
}

#endif
