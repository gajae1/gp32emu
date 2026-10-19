/* Built by bench_audio_resampler.py with two independently compiled versions
 * of the resampler. This is a developer comparator, not part of the core. */
#define _POSIX_C_SOURCE 200809L
#include "audio/gp32_audio_resampler.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DECLARE(prefix) \
    void prefix##_init(gp32_audio_resampler_t *); \
    void prefix##_reset(gp32_audio_resampler_t *); \
    void prefix##_mark_gap(gp32_audio_resampler_t *, uint32_t); \
    void prefix##_mark_gap_from_silence(gp32_audio_resampler_t *, uint32_t); \
    size_t prefix##_process(gp32_audio_resampler_t *, const int16_t *, size_t, \
                            uint32_t, uint32_t, int32_t, int16_t *, size_t); \
    size_t prefix##_copy(gp32_audio_resampler_t *, const int16_t *, size_t, \
                         uint32_t, int16_t *); \
    size_t prefix##_output_frames(const gp32_audio_resampler_t *, size_t, \
                                  uint32_t, uint32_t, int32_t)
DECLARE(baseline);
DECLARE(candidate);

typedef size_t (*process_fn)(gp32_audio_resampler_t *, const int16_t *, size_t,
                             uint32_t, uint32_t, int32_t, int16_t *, size_t);

static uint32_t random_state = UINT32_C(0x12345678);
static int16_t input[2048u * 2u], output[2][16384u * 2u];

static uint32_t random_u32(void) {
    random_state = random_state * UINT32_C(1664525) + UINT32_C(1013904223);
    return random_state;
}

static int check_streams(void) {
    static const uint32_t rates[][2] = {
        {11025, 44100}, {22050, 44100}, {23144, 44100}, {32000, 48000},
        {44100, 48000}, {48000, 44100}, {96000, 8000}, {44100, 44100}
    };
    static const size_t blocks[] = {1, 2, 3, 4, 5, 31, 63, 64, 65, 66, 127, 512, 2048};
    static const int32_t adjustments[] = {-50000, -1000, 0, 1000, 50000};
    gp32_audio_resampler_t a, b;
    baseline_init(&a);
    candidate_init(&b);
    uint64_t frames_checked = 0;
    for (unsigned trial = 0; trial < 1024u; ++trial) {
        unsigned rate = (trial / 17u) % (sizeof(rates) / sizeof(rates[0]));
        uint32_t sr = rates[rate][0], dr = rates[rate][1];
        size_t frames = blocks[random_u32() % (sizeof(blocks) / sizeof(blocks[0]))];
        int32_t ppm = adjustments[random_u32() % (sizeof(adjustments) / sizeof(adjustments[0]))];
        size_t cap = trial % 5u ? 16384u : 1u + random_u32() % 127u;
        for (size_t i = 0; i < frames * 2u; ++i)
            input[i] = i % 7u ? (int16_t)(random_u32() >> 16) : (i & 1u ? INT16_MIN : INT16_MAX);
        if (trial % 53u == 0u) {
            baseline_mark_gap(&a, dr);
            candidate_mark_gap(&b, dr);
        } else if (trial % 67u == 0u) {
            baseline_mark_gap_from_silence(&a, dr);
            candidate_mark_gap_from_silence(&b, dr);
        } else if (trial % 79u == 0u) {
            baseline_reset(&a);
            candidate_reset(&b);
        }
        if (baseline_output_frames(&a, frames, sr, dr, ppm) !=
            candidate_output_frames(&b, frames, sr, dr, ppm)) {
            fprintf(stderr, "FAIL: output predictor at stream block %u\n", trial);
            return 0;
        }
        memset(output, 0x5a, sizeof(output));
        size_t na, nb;
        if (trial % 29u == 0u) {
            na = baseline_copy(&a, input, frames, sr, output[0]);
            nb = candidate_copy(&b, input, frames, sr, output[1]);
        } else {
            na = baseline_process(&a, input, frames, sr, dr, ppm, output[0], cap);
            nb = candidate_process(&b, input, frames, sr, dr, ppm, output[1], cap);
        }
        if (na != nb || memcmp(output[0], output[1], sizeof(output[0])) || memcmp(&a, &b, sizeof(a))) {
            fprintf(stderr, "FAIL: PCM/count/state at stream block %u (rate %u/%u, input %zu, cap %zu)\n",
                    trial, sr, dr, frames, cap);
            return 0;
        }
        frames_checked += na;
    }
    fprintf(stderr, "PASS: 1024 stream blocks, %" PRIu64 " PCM frames and all resampler states identical\n",
            frames_checked);
    return 1;
}

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void benchmark(unsigned which, uint32_t src_rate, uint32_t dst_rate,
                       size_t block, unsigned iterations, unsigned round) {
    static const process_fn process[] = {baseline_process, candidate_process};
    gp32_audio_resampler_t r;
    baseline_init(&r);
    process[which](&r, input, block, src_rate, dst_rate, 0, output[0], 16384u);
    uint64_t frames = 0, checksum = 0;
    double start = now_seconds();
    for (unsigned i = 0; i < iterations; ++i) {
        size_t count = process[which](&r, input, block, src_rate, dst_rate, 0, output[0], 16384u);
        frames += count;
        if (count) checksum += (uint16_t)output[0][(i % count) * 2u];
    }
    double elapsed = now_seconds() - start;
    printf("%s,%u,%u,%zu,%u,%.3f,%" PRIu64 ",%" PRIu64 "\n",
            which ? "candidate" : "baseline", src_rate, dst_rate, block, round,
            elapsed * 1e9 / (double)frames, frames, checksum);
}

int main(int argc, char **argv) {
    unsigned rounds = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 10) : 7u;
    unsigned frames_per_round = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 250000u;
    if (!check_streams()) return EXIT_FAILURE;
    if (!rounds) return EXIT_SUCCESS;
    static const uint32_t rates[][2] = {{11025, 44100}, {23144, 44100}, {48000, 44100}};
    static const size_t blocks[] = {1, 32, 128, 512, 2048};
    for (size_t i = 0; i < sizeof(input) / sizeof(input[0]); ++i)
        input[i] = (int16_t)(random_u32() >> 16);
    puts("variant,source,dest,block,round,ns_per_frame,frames,checksum");
    for (unsigned round = 0; round < rounds; ++round) {
        for (unsigned rate = 0; rate < sizeof(rates) / sizeof(rates[0]); ++rate) {
            for (unsigned block = 0; block < sizeof(blocks) / sizeof(blocks[0]); ++block) {
                unsigned iterations = (unsigned)((double)frames_per_round * rates[rate][0] /
                                      ((double)blocks[block] * rates[rate][1]));
                if (iterations < 32u) iterations = 32u;
                /* Alternate the adjacent baseline/candidate order each round. */
                for (unsigned run = 0; run < 2u; ++run)
                    benchmark((round + run) & 1u, rates[rate][0], rates[rate][1],
                              blocks[block], iterations, round);
            }
        }
    }
    return EXIT_SUCCESS;
}
