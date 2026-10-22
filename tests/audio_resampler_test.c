#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif

/* Check the FIR against its mathematical definition, independently of the
 * contiguous/history split and the order used by an optimized kernel. */
#include "../src/audio/gp32_audio_resampler.c"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#include <unistd.h>
#endif

static uint32_t random_state = UINT32_C(0x5eeda11d);

static int16_t random_sample(void) {
    random_state = random_state * UINT32_C(1664525) + UINT32_C(1013904223);
    return (int16_t)(random_state >> 16);
}

static void reference_gather(const gp32_audio_resampler_t *r, const int16_t *src,
                             int have_prev, size_t n, unsigned phase,
                             int64_t *left, int64_t *right) {
    *left = *right = 0;
    for (size_t tap = 0; tap < GP32_AUDIO_POLY_TAPS; ++tap) {
        int16_t l, rr;
        if (tap > n) {
            size_t back = tap - n;
            l = r->poly_hist_l[GP32_AUDIO_POLY_HISTORY - back];
            rr = r->poly_hist_r[GP32_AUDIO_POLY_HISTORY - back];
        } else {
            size_t index = n - tap;
            if (have_prev && !index) {
                l = r->prev_l;
                rr = r->prev_r;
            } else {
                index -= have_prev ? 1u : 0u;
                l = src[index * 2u];
                rr = src[index * 2u + 1u];
            }
        }
        *left += (int64_t)r->poly_coef[phase][tap] * l;
        *right += (int64_t)r->poly_coef[phase][tap] * rr;
    }
}

static int check_kernel(gp32_audio_resampler_t *r, int16_t *storage,
                         unsigned pattern, uint64_t *checks) {
    /* Every source alignment, coefficient phase and FIR/history boundary.
     * n=64 and higher also check omission of the exactly-zero last tap. */
    for (unsigned alignment = 0; alignment < 8u; ++alignment) {
        const int16_t *src = storage + alignment;
        for (unsigned prev = 0; prev < 2u; ++prev) {
            for (unsigned phase = 0; phase < GP32_AUDIO_POLY_PHASES; ++phase) {
                for (size_t at = 0; at < 84u; ++at) {
                    size_t n = at < 80u ? at : 128u * (at - 79u) - 1u;
                    int64_t want_l, want_r, got_l, got_r;
                    reference_gather(r, src, (int)prev, n, phase, &want_l, &want_r);
                    poly_gather(r, src, (int)prev, n, (uint32_t)phase << 24,
                                &got_l, &got_r);
                    if (got_l != want_l || got_r != want_r) {
                        fprintf(stderr, "FIR pattern=%u alignment=%u prev=%u phase=%u n=%zu: "
                            "got=%" PRId64 "/%" PRId64 " want=%" PRId64 "/%" PRId64 "\n",
                            pattern, alignment, prev, phase, n, got_l, got_r, want_l, want_r);
                        return 0;
                    }
                    ++*checks;
                }
            }
        }
    }
    return 1;
}

/* A vector gather may only read real source frames, including at the two
 * ends of an allocation. Put the exact input against each inaccessible page;
 * every vector-sized prefix/tail and the history split are exercised. A
 * read-only source also catches stores accidentally introduced by a kernel. */
static int check_guarded_source(gp32_audio_resampler_t *r, uint64_t *checks) {
#if defined(__unix__) || defined(__APPLE__)
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size < 2048) return 0;
    size_t page = (size_t)page_size;
    void *mapping = mmap(NULL, 3u * page, PROT_NONE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) return 0;
    int16_t *middle = (int16_t *)((unsigned char *)mapping + page);
    if (mprotect(middle, page, PROT_READ | PROT_WRITE)) {
        munmap(mapping, 3u * page);
        return 0;
    }
    for (size_t i = 0; i < page / sizeof(*middle); ++i)
        middle[i] = i & 1u ? INT16_MIN : random_sample();
    if (mprotect(middle, page, PROT_READ)) {
        munmap(mapping, 3u * page);
        return 0;
    }
    int ok = 1;
    for (unsigned side = 0; side < 2u && ok; ++side) {
        for (size_t test = 1u; test <= 84u && ok; ++test) {
            size_t frames = test <= 80u ? test : 128u * (test - 80u);
            const int16_t *src = side ? middle + page / sizeof(*middle) - frames * 2u : middle;
            for (unsigned prev = 0; prev < 2u && ok; ++prev) {
                size_t n = frames - (prev ? 0u : 1u);
                for (unsigned phase = 0; phase < GP32_AUDIO_POLY_PHASES; ++phase) {
                    int64_t want_l, want_r, got_l, got_r;
                    reference_gather(r, src, (int)prev, n, phase, &want_l, &want_r);
                    poly_gather(r, src, (int)prev, n, (uint32_t)phase << 24,
                                &got_l, &got_r);
                    if (got_l != want_l || got_r != want_r) {
                        fprintf(stderr, "guarded FIR side=%u frames=%zu prev=%u phase=%u mismatch\n",
                                side, frames, prev, phase);
                        ok = 0;
                        break;
                    }
                    ++*checks;
                }
            }
        }
    }
    if (munmap(mapping, 3u * page)) ok = 0;
    return ok;
#else
    (void)r;
    (void)checks;
    return 1;
#endif
}

/* The lookup must remain bit-exact with the original numerical kernel,
 * including the mirrored rows' first-maximum normalization and rates whose
 * multiply/divide rounds the nominal upsampling cutoff by one bit. */
static int check_tables(void) {
    static const uint32_t rates[][2] = {
        {11025,44100}, {22050,44100}, {23144,44100}, {32000,48000},
        {44100,48000}, {48000,44100}, {96000,8000}, {1,UINT32_MAX},
        {UINT32_MAX,1}, {UINT32_MAX,UINT32_MAX}, {8535,44100}, {8543,44100}
    };
    gp32_audio_resampler_t actual, expected;
    /* Exhaust every representable cutoff accepted by the lookup, not just
     * a selection of rates that happen to round to its center. */
    double edge = 0x1.eb851eb851eb6p-2;
    for (unsigned k = 0; k < 5u; ++k, edge = nextafter(edge, INFINITY)) {
        poly_build_table_math(&expected, edge);
        if (memcmp(expected.poly_coef, poly_upsampling_coef,
                   sizeof(expected.poly_coef))) {
            fprintf(stderr, "rounded cutoff %a differs from lookup\n", edge);
            return 0;
        }
    }
    for (unsigned k = 0; k < sizeof(rates) / sizeof(rates[0]); ++k) {
        memset(&actual, 0x5a, sizeof(actual));
        memcpy(&expected, &actual, sizeof(expected));
        poly_build_table_math(&expected, poly_cutoff(rates[k][0], rates[k][1]));
        expected.poly_src_rate = rates[k][0];
        expected.poly_dst_rate = rates[k][1];
        poly_build_table(&actual, rates[k][0], rates[k][1]);
        if (memcmp(&actual, &expected, sizeof(actual))) {
            fprintf(stderr, "coefficient/history mismatch at %u -> %u\n",
                    rates[k][0], rates[k][1]);
            return 0;
        }
    }
    return 1;
}

/* Regenerate src/audio/gp32_audio_kernel.inc with --emit-table. No runtime
 * state, build-host endianness or floating-point values are embedded. */
static int emit_table(void) {
    gp32_audio_resampler_t r;
    poly_build_table_math(&r, 0.480);
    puts("/* Generated by audio_resampler_test --emit-table.\n"
         " * Kaiser beta=6, cutoff=0.480, 256 phases, 65 taps; Q15 DC-normalized. */");
    puts("/* Keep this large immutable table after executable code in ELF images.\n"
         " * It is made read-only with the other RELRO data by the dynamic loader. */\n"
         "#if defined(__ELF__) && (defined(__GNUC__) || defined(__clang__))\n"
         "__attribute__((section(\".data.rel.ro.gp32_audio_kernel\")))\n"
         "#endif");
    puts("static const int16_t poly_upsampling_coef[256][65] = {");
    for (unsigned p = 0; p < GP32_AUDIO_POLY_PHASES; ++p) {
        fputs("    {", stdout);
        for (unsigned i = 0; i < GP32_AUDIO_POLY_TAPS; ++i)
            printf("%s%d", i ? "," : "", r.poly_coef[p][i]);
        puts("},");
    }
    puts("};");
    return ferror(stdout) ? EXIT_FAILURE : EXIT_SUCCESS;
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--emit-table")) return emit_table();
    if (argc != 1 || !check_tables()) return EXIT_FAILURE;
    static const uint32_t rates[][2] = {
        {11025, 44100}, {22050, 44100}, {23144, 44100}, {32000, 48000},
        {44100, 48000}, {48000, 44100}, {96000, 8000}
    };
    gp32_audio_resampler_t r;
    int16_t storage[512u * 2u + 8u];
    uint64_t checks = 0;
    gp32_audio_resampler_init(&r);
    for (size_t i = 0; i < sizeof(storage) / sizeof(storage[0]); ++i)
        storage[i] = i % 7u ? random_sample() : (i & 1u ? INT16_MIN : INT16_MAX);
    for (unsigned i = 0; i < GP32_AUDIO_POLY_HISTORY; ++i) {
        r.poly_hist_l[i] = random_sample();
        r.poly_hist_r[i] = random_sample();
    }
    r.prev_l = INT16_MIN;
    r.prev_r = INT16_MAX;
    for (unsigned rate = 0; rate < sizeof(rates) / sizeof(rates[0]); ++rate) {
        poly_build_table(&r, rates[rate][0], rates[rate][1]);
        for (unsigned phase = 0; phase < GP32_AUDIO_POLY_PHASES; ++phase) {
            if (r.poly_coef[phase][GP32_AUDIO_POLY_HISTORY] != 0) {
                fputs("FIR last tap must be zero at every supported phase\n", stderr);
                return EXIT_FAILURE;
            }
        }
        if (!check_kernel(&r, storage, rate, &checks)) return EXIT_FAILURE;
    }
    /* Exercise multiplication and accumulation limits even beyond the
     * generated filter's lobes. Two -32768 products must not wrap at 32 bits. */
    for (unsigned phase = 0; phase < GP32_AUDIO_POLY_PHASES; ++phase) {
        for (unsigned tap = 0; tap < GP32_AUDIO_POLY_HISTORY; ++tap)
            r.poly_coef[phase][tap] = phase & 1u ? INT16_MIN : random_sample();
        r.poly_coef[phase][GP32_AUDIO_POLY_HISTORY] = 0;
    }
    for (size_t i = 0; i < sizeof(storage) / sizeof(storage[0]); ++i) storage[i] = INT16_MIN;
    for (unsigned i = 0; i < GP32_AUDIO_POLY_HISTORY; ++i)
        r.poly_hist_l[i] = r.poly_hist_r[i] = INT16_MIN;
    r.prev_l = r.prev_r = INT16_MIN;
    if (!check_kernel(&r, storage, 7u, &checks)) return EXIT_FAILURE;
    uint64_t guarded_checks = 0;
    if (!check_guarded_source(&r, &guarded_checks)) return EXIT_FAILURE;
    printf("PASS: %" PRIu64 " FIR sums and %" PRIu64 " protected-source sums match independent signed-64-bit reference\n",
           checks, guarded_checks);
    return EXIT_SUCCESS;
}
