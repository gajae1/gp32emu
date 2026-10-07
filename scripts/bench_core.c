/*
 * bench_core.c - deterministic GP32 core benchmark harness.
 *
 * Runs a fixed warmup (auto A pulses like headless_main, unless an input
 * script is supplied), then times a fixed number of frames. Every frame
 * consumes the framebuffer and audio queue; measured frames fold pixels and
 * PCM into FNV-1a 64 hashes so runs can be compared for exactness.
 *
 * Output is a single JSON object on stdout. Exit 0 on success, 1 on runtime
 * failure, 2 on usage errors.
 *
 * --frame-times adds a frame_times object describing the measured frames'
 * host-time distribution (percentiles, tail counts, slowest frames).
 *
 * Every measured frame also folds the panel's own frame counter into
 * lcd_frames/lcd_repeat_frames/lcd_unpresented_frames, so a title whose
 * panel period differs from the 1/60 s frame interval shows up as repeated
 * presents instead of only as a hash change.
 *
 * Portable C11: QueryPerformanceCounter on Windows, CLOCK_MONOTONIC elsewhere.
 * Depends only on libgp32emu (gp32emu/gp32.h) and src/input_script.h.
 */
#ifndef _WIN32
#define _POSIX_C_SOURCE 199309L
#endif

#include "gp32emu/gp32.h"
#include "smc_direct.h"
#include "input_script.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <time.h>
#endif

#define FNV64_OFFSET 14695981039346656037ULL
#define FNV64_PRIME 1099511628211ULL

static uint64_t fnv1a64_update(uint64_t h, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    while (len--) {
        h ^= (uint64_t)*p++;
        h *= FNV64_PRIME;
    }
    return h;
}

static double now_seconds(void) {
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER counter;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#endif
}

/* Raw high-resolution ticks for per-frame sampling: QPC ticks on Windows,
 * CLOCK_MONOTONIC nanoseconds elsewhere. Kept apart from now_seconds() so the
 * default elapsed/fps reporting path stays exactly as it was. */
static uint64_t now_ticks(void) {
#ifdef _WIN32
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return (uint64_t)counter.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
}

static double ticks_per_ms(void) {
#ifdef _WIN32
    static LARGE_INTEGER freq;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    return (double)freq.QuadPart / 1000.0;
#else
    return 1000000.0; /* CLOCK_MONOTONIC reports nanoseconds */
#endif
}

/* Same confirm-pulse schedule as headless_main's BIOS auto-start. */
static uint32_t bios_auto_start_buttons_for_frame(uint64_t frame) {
    return ((frame >= 620u && frame < 660u) ||
            (frame >= 1450u && frame < 1490u) ||
            (frame >= 1800u && frame < 1840u) ||
            (frame >= 2200u && frame < 2240u)) ? GP32_BUTTON_A : 0u;
}

/* Legacy cycle budgets retained for comparisons with pre-frame-time results. */
static uint32_t frame_cycles_for(gp32_t *g, uint64_t *accum) {
    uint32_t run_hz = gp32_get_run_clock_hz(g);
    if (!run_hz) run_hz = 66000000u;
    *accum += run_hz;
    uint32_t frame_cycles = (uint32_t)(*accum / 60u);
    *accum -= (uint64_t)frame_cycles * 60u;
    return frame_cycles ? frame_cycles : 1u;
}

static uint64_t hash_framebuffer(uint64_t h, const gp32_framebuffer_desc_t *fb) {
    if (!fb->pixels_rgba8888 || !fb->width || !fb->height) return h;
    for (uint32_t y = 0; y < fb->height; ++y) {
        const uint8_t *row = (const uint8_t *)(fb->pixels_rgba8888 + (size_t)y * fb->stride_pixels);
        h = fnv1a64_update(h, row, (size_t)fb->width * sizeof(uint32_t));
    }
    return h;
}

static uint64_t hash_audio(uint64_t h, const gp32_audio_desc_t *aud) {
    if (!aud->samples_s16_interleaved || !aud->frame_count) return h;
    return fnv1a64_update(h, aud->samples_s16_interleaved,
                          (size_t)aud->frame_count * 2u * sizeof(int16_t));
}

/*
 * Optional per-frame host-time distribution (--frame-times).
 *
 * frame_ms[] holds one host-time sample per measured frame. A sample covers the
 * same work as the default elapsed timer: the guest frame plus this harness's
 * own framebuffer/audio consumption, so the samples reproduce elapsed. The
 * slowest list keeps the frames that matter for micro-stutter; when
 * --cpu-profile is also set each entry carries that frame's counter deltas so
 * an outlier can be attributed to JIT compile bursts, cache invalidation,
 * media polling or plain guest work.
 */
#define FRAME_TIME_SLOWEST 10
#define FRAME_TIME_OVER1_MS 16.67
#define FRAME_TIME_OVER2_MS 33.3

typedef struct frame_counters {
    uint64_t cycles;
    uint64_t jit_blocks_compiled;
    uint64_t jit_native_compiled;
    uint64_t jit_misses;
    uint64_t jit_invalidations;
    uint64_t jit_code_full_events;
    uint64_t native_block_calls;
    uint64_t helper_interp_ops;
    uint64_t poll_skip_events;
    uint64_t slow_bail_single_nonram;
} frame_counters_t;

static frame_counters_t frame_counters_snapshot(const gp32_t *g) {
    gp32_cpu_profile_t p;
    frame_counters_t c;
    memset(&p, 0, sizeof(p));
    gp32_get_cpu_profile(g, &p);
    c.cycles = gp32_get_cycles(g);
    c.jit_blocks_compiled = p.jit_blocks_compiled;
    c.jit_native_compiled = p.jit_native_compiled;
    c.jit_misses = p.jit_misses;
    c.jit_invalidations = p.jit_invalidations;
    c.jit_code_full_events = p.jit_code_full_events;
    c.native_block_calls = p.native_block_calls;
    c.helper_interp_ops = p.helper_interp_ops;
    c.poll_skip_events = p.poll_skip_events;
    c.slow_bail_single_nonram = p.slow_bail_single_nonram;
    return c;
}

static frame_counters_t frame_counters_delta(frame_counters_t after, frame_counters_t before) {
    frame_counters_t d;
    d.cycles = after.cycles - before.cycles;
    d.jit_blocks_compiled = after.jit_blocks_compiled - before.jit_blocks_compiled;
    d.jit_native_compiled = after.jit_native_compiled - before.jit_native_compiled;
    d.jit_misses = after.jit_misses - before.jit_misses;
    d.jit_invalidations = after.jit_invalidations - before.jit_invalidations;
    d.jit_code_full_events = after.jit_code_full_events - before.jit_code_full_events;
    d.native_block_calls = after.native_block_calls - before.native_block_calls;
    d.helper_interp_ops = after.helper_interp_ops - before.helper_interp_ops;
    d.poll_skip_events = after.poll_skip_events - before.poll_skip_events;
    d.slow_bail_single_nonram = after.slow_bail_single_nonram - before.slow_bail_single_nonram;
    return d;
}

static int cmp_double_asc(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

/* Nearest-rank percentile over an ascending array. */
static double percentile_ms(const double *sorted, size_t count, uint64_t num, uint64_t den) {
    if (!count) return 0.0;
    uint64_t rank = ((uint64_t)count * num + den - 1u) / den;
    if (rank < 1u) rank = 1u;
    if (rank > count) rank = count;
    return sorted[rank - 1u];
}

static void print_frame_counters_json(const frame_counters_t *c) {
    printf(",\"cycles\":%" PRIu64
           ",\"jit_blocks_compiled\":%" PRIu64
           ",\"jit_native_compiled\":%" PRIu64
           ",\"jit_misses\":%" PRIu64
           ",\"jit_invalidations\":%" PRIu64
           ",\"jit_code_full_events\":%" PRIu64
           ",\"native_block_calls\":%" PRIu64
           ",\"helper_interp_ops\":%" PRIu64
           ",\"poll_skip_events\":%" PRIu64
           ",\"slow_bail_single_nonram\":%" PRIu64,
           c->cycles, c->jit_blocks_compiled, c->jit_native_compiled, c->jit_misses,
           c->jit_invalidations, c->jit_code_full_events, c->native_block_calls,
           c->helper_interp_ops, c->poll_skip_events, c->slow_bail_single_nonram);
}

static void print_frame_times_json(double *frame_ms, size_t count,
                                   const frame_counters_t *counters) {
    size_t over1 = 0, over2 = 0, top_n = 0;
    size_t top_idx[FRAME_TIME_SLOWEST];
    double top_ms[FRAME_TIME_SLOWEST];

    for (size_t i = 0; i < count; ++i) {
        double v = frame_ms[i];
        if (v > FRAME_TIME_OVER1_MS) ++over1;
        if (v > FRAME_TIME_OVER2_MS) ++over2;
        if (top_n == FRAME_TIME_SLOWEST && !(v > top_ms[FRAME_TIME_SLOWEST - 1])) continue;
        size_t pos = top_n < FRAME_TIME_SLOWEST ? top_n : FRAME_TIME_SLOWEST - 1;
        while (pos > 0 && top_ms[pos - 1] < v) {
            top_ms[pos] = top_ms[pos - 1];
            top_idx[pos] = top_idx[pos - 1];
            --pos;
        }
        top_ms[pos] = v;
        top_idx[pos] = i;
        if (top_n < FRAME_TIME_SLOWEST) ++top_n;
    }
    if (count > 1) qsort(frame_ms, count, sizeof *frame_ms, cmp_double_asc);

    printf(",\"frame_times\":{\"count\":%zu"
           ",\"p50_ms\":%.3f,\"p90_ms\":%.3f,\"p99_ms\":%.3f,\"p99_9_ms\":%.3f,\"max_ms\":%.3f"
           ",\"over_16_67_ms\":%zu,\"over_33_3_ms\":%zu,\"slowest\":[",
           count,
           percentile_ms(frame_ms, count, 50, 100), percentile_ms(frame_ms, count, 90, 100),
           percentile_ms(frame_ms, count, 99, 100), percentile_ms(frame_ms, count, 999, 1000),
           count ? frame_ms[count - 1] : 0.0,
           over1, over2);
    for (size_t i = 0; i < top_n; ++i) {
        printf("%s{\"index\":%zu,\"ms\":%.3f", i ? "," : "", top_idx[i], top_ms[i]);
        if (counters) print_frame_counters_json(&counters[top_idx[i]]);
        printf("}");
    }
    printf("]}");
}

static int usage(const char *argv0) {
    fprintf(stderr,
        "usage: %s --bios bios.bin --smc game.smc [--state file] [--warmup N=2400] [--frames N=600] [--jit] [--input-script script.txt] [--cpu-profile] [--frame-times] [--legacy-cycle-frames] [--force-bios] [--force-direct]\n"
        "Times --frames frames after --warmup warmup frames. BIOS+SMC runs without an input script get the same\n"
        "auto A pulses as headless_main. Prints one JSON object: fps, elapsed, cycles (absolute counter,\n"
        "warmup included), cycles_measured (counter delta over the measured frames only), pc, cpsr, clock,\n"
        "audio_frames, lcd_frames/lcd_repeat_frames/lcd_unpresented_frames, video/audio FNV-1a-64 hashes.\n"
        "--cpu-profile resets CPU workload counters at the\n"
        "warmup boundary and appends a cpu_profile object (requires a GP32EMU_CPU_PROFILE build).\n"
        "A card whose only executable sits outside GAME\\ (the freeware GPMM\\ layout) cannot boot through\n"
        "the retail BIOS, so it is loaded through the direct SmartMedia boot instead; --state replays and\n"
        "--force-bios keep the BIOS path. --force-direct boots any classified card through the host-side direct loader.\n"
        "--frame-times times each measured frame with the high-resolution host clock and appends a\n"
        "frame_times object: count, p50/p90/p99/p99.9/max milliseconds (nearest rank), how many frames\n"
        "exceeded 16.67 ms and 33.3 ms, and the 10 slowest frames. With --cpu-profile as well, every\n"
        "slowest frame also carries that frame's delta of cycles, jit_blocks_compiled,\n"
        "jit_native_compiled, jit_misses, jit_invalidations, jit_code_full_events, native_block_calls,\n"
        "helper_interp_ops, poll_skip_events and slow_bail_single_nonram. Default output is unchanged\n"
        "without the option.\n"
        "Default pacing matches frontends; --legacy-cycle-frames replays the former clock/60 budget.\n",
        argv0);
    return 2;
}

static void print_cpu_profile_json(const gp32_cpu_profile_t *p) {
    static const char *op_kind_names[GP32_CPU_PROFILE_OP_KINDS] = {
        "interp", "data", "psr", "mul", "swp", "half",
        "single_dt", "block_dt", "branch", "swi", "coproc", "undefined"
    };
    printf(",\"cpu_profile\":{\"supported\":%" PRIu32 ",\"native_backend\":%" PRIu32
           ",\"interp_arm_insns\":%" PRIu64 ",\"interp_thumb_insns\":%" PRIu64
           ",\"block_interp_arm_insns\":%" PRIu64
           ",\"native_block_calls\":%" PRIu64 ",\"native_arm_insns\":%" PRIu64
           ",\"native_bail_calls\":%" PRIu64
           ",\"poll_skip_events\":%" PRIu64 ",\"poll_skipped_insns\":%" PRIu64
           ",\"jit_hits\":%" PRIu64 ",\"jit_misses\":%" PRIu64
           ",\"jit_fallbacks\":%" PRIu64
           ",\"jit_blocks_compiled\":%" PRIu64 ",\"jit_block_conflicts\":%" PRIu64
           ",\"jit_translate_failures\":%" PRIu64
           ",\"jit_native_compiled\":%" PRIu64 ",\"jit_native_failed\":%" PRIu64
           ",\"jit_code_full_events\":%" PRIu64 ",\"jit_code_alloc_failures\":%" PRIu64
           ",\"jit_invalidations\":%" PRIu64
           ",\"jit_inv_reset\":%" PRIu64 ",\"jit_inv_jit_disable\":%" PRIu64
           ",\"jit_inv_api_flush\":%" PRIu64 ",\"jit_inv_state_load\":%" PRIu64
           ",\"jit_inv_cp15_mmu\":%" PRIu64 ",\"jit_inv_cp15_cache\":%" PRIu64
           ",\"jit_inv_code_recycle\":%" PRIu64
           ",\"helper_ld_word\":%" PRIu64 ",\"helper_ld_byte\":%" PRIu64
           ",\"helper_ld_sbyte\":%" PRIu64 ",\"helper_ld_half\":%" PRIu64
           ",\"helper_ld_shalf\":%" PRIu64 ",\"helper_st_word\":%" PRIu64
           ",\"helper_st_byte\":%" PRIu64 ",\"helper_st_half\":%" PRIu64
           ",\"helper_write_pc\":%" PRIu64 ",\"helper_interp_ops\":%" PRIu64
           ",\"helper_op_kinds\":{",
           p->supported, p->native_backend,
           p->interp_arm_insns, p->interp_thumb_insns, p->block_interp_arm_insns,
           p->native_block_calls, p->native_arm_insns, p->native_bail_calls,
           p->poll_skip_events, p->poll_skipped_insns,
           p->jit_hits, p->jit_misses, p->jit_fallbacks,
           p->jit_blocks_compiled, p->jit_block_conflicts, p->jit_translate_failures,
           p->jit_native_compiled, p->jit_native_failed,
           p->jit_code_full_events, p->jit_code_alloc_failures,
           p->jit_invalidations,
           p->jit_inv_reset, p->jit_inv_jit_disable, p->jit_inv_api_flush,
           p->jit_inv_state_load, p->jit_inv_cp15_mmu, p->jit_inv_cp15_cache,
           p->jit_inv_code_recycle,
           p->helper_ld_word, p->helper_ld_byte, p->helper_ld_sbyte,
           p->helper_ld_half, p->helper_ld_shalf,
           p->helper_st_word, p->helper_st_byte, p->helper_st_half,
           p->helper_write_pc, p->helper_interp_ops);
    for (unsigned i = 0; i < GP32_CPU_PROFILE_OP_KINDS; ++i)
        printf("%s\"%s\":%" PRIu64, i ? "," : "", op_kind_names[i], p->helper_op_kinds[i]);
    printf("},\"jit_code_size\":%" PRIu64 ",\"jit_code_used\":%" PRIu64
           ",\"jit_code_highwater\":%" PRIu64 ",\"jit_block_capacity\":%" PRIu64
           ",\"slow_gate_data_regshift\":%" PRIu64
           ",\"slow_gate_data_r15flags\":%" PRIu64
           ",\"slow_gate_mul\":%" PRIu64
           ",\"slow_gate_block_shape\":%" PRIu64
           ",\"slow_gate_single_shape\":%" PRIu64
           ",\"slow_bail_block_unaligned\":%" PRIu64
           ",\"slow_bail_block_xpage\":%" PRIu64
           ",\"slow_bail_block_tinypage\":%" PRIu64
           ",\"slow_bail_block_nonram\":%" PRIu64
           ",\"slow_bail_block_pcodd\":%" PRIu64
           ",\"slow_bail_block_other\":%" PRIu64
           ",\"slow_bail_single_nonram\":%" PRIu64
           ",\"slow_bail_single_tlbmiss\":%" PRIu64
           ",\"slow_bail_single_other\":%" PRIu64
           ",\"slow_bail_other\":%" PRIu64
           ",\"slow_bail_block_tlbmiss\":%" PRIu64,
           p->jit_code_size, p->jit_code_used, p->jit_code_highwater, p->jit_block_capacity,
           p->slow_gate_data_regshift, p->slow_gate_data_r15flags, p->slow_gate_mul,
           p->slow_gate_block_shape, p->slow_gate_single_shape,
           p->slow_bail_block_unaligned, p->slow_bail_block_xpage,
           p->slow_bail_block_tinypage, p->slow_bail_block_nonram,
           p->slow_bail_block_pcodd, p->slow_bail_block_other,
           p->slow_bail_single_nonram, p->slow_bail_single_tlbmiss,
           p->slow_bail_single_other, p->slow_bail_other,
           p->slow_bail_block_tlbmiss);
    printf(",\"single_nonram_address_overflow\":%" PRIu64 ",\"single_nonram_regions\":{",
           p->single_nonram_address_overflow);
    unsigned printed = 0;
    for (unsigned i = 0; i < 256; ++i) if (p->single_nonram_regions[i])
        printf("%s\"0x%02x\":%" PRIu64, printed++ ? "," : "", i, p->single_nonram_regions[i]);
    printf("},\"single_nonram_addresses\":[");
    printed = 0;
    for (unsigned i = 0; i < GP32_CPU_PROFILE_MEMORY_SLOTS; ++i) {
        const gp32_memory_profile_t *a = &p->single_nonram_addresses[i];
        if (!(a->reads | a->writes)) continue;
        printf("%s{\"address\":\"0x%08" PRIx32 "\",\"first_pc\":\"0x%08" PRIx32
               "\",\"reads\":%" PRIu64 ",\"writes\":%" PRIu64 "}",
               printed++ ? "," : "", a->physical_address, a->first_pc, a->reads, a->writes);
    }
    printf("]}");
}

int main(int argc, char **argv) {
    const char *bios = NULL, *smc = NULL, *state_path = NULL, *input_script_path = NULL;
    uint64_t warmup = 2400, frames = 600;
    int jit = 0, cpu_profile = 0, legacy_cycle_frames = 0, frame_times = 0, force_bios = 0, force_direct = 0;
    int frame_times_raw = 0;
    unsigned cpu_speed = 100u;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--bios") && i + 1 < argc) bios = argv[++i];
        else if (!strcmp(argv[i], "--smc") && i + 1 < argc) smc = argv[++i];
        else if (!strcmp(argv[i], "--state") && i + 1 < argc) state_path = argv[++i];
        else if (!strcmp(argv[i], "--warmup") && i + 1 < argc) warmup = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--jit")) jit = 1;
        else if (!strcmp(argv[i], "--legacy-cycle-frames")) legacy_cycle_frames = 1;
        else if (!strcmp(argv[i], "--cpu-profile")) cpu_profile = 1;
        else if (!strcmp(argv[i], "--frame-times")) frame_times = 1;
        else if (!strcmp(argv[i], "--frame-times-raw")) frame_times = frame_times_raw = 1;
        else if (!strcmp(argv[i], "--force-bios")) force_bios = 1;
        else if (!strcmp(argv[i], "--force-direct")) force_direct = 1;
        else if (!strcmp(argv[i], "--cpu-speed") && i + 1 < argc) cpu_speed = (unsigned)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--input-script") && i + 1 < argc) input_script_path = argv[++i];
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(argv[0]); return 0; }
        else { fprintf(stderr, "unknown or incomplete option: %s\n", argv[i]); return usage(argv[0]); }
    }
    if (!bios || !smc) { fprintf(stderr, "--bios and --smc are required\n"); return usage(argv[0]); }

    double *frame_ms = NULL;
    frame_counters_t *frame_ctr = NULL;
    if (frame_times && frames) {
        if (frames > (uint64_t)SIZE_MAX / sizeof(double)) {
            fprintf(stderr, "--frame-times: frame count too large\n");
            return 1;
        }
        frame_ms = (double *)calloc((size_t)frames, sizeof *frame_ms);
        if (cpu_profile) frame_ctr = (frame_counters_t *)calloc((size_t)frames, sizeof *frame_ctr);
        if (!frame_ms || (cpu_profile && !frame_ctr)) {
            fprintf(stderr, "--frame-times: sample allocation failed\n");
            free(frame_ms);
            free(frame_ctr);
            return 1;
        }
    }

    gp32_input_script_t *script = NULL;
    if (input_script_path) {
        char err[256] = {0};
        if (!gp32_input_script_load(input_script_path, &script, err, sizeof(err))) {
            fprintf(stderr, "input script load failed: %s\n", err[0] ? err : "unknown error");
            return 1;
        }
    }

    gp32_options_t opt;
    memset(&opt, 0, sizeof(opt));
    opt.bios_path = bios;
    opt.smartmedia_path = smc;

    /* The retail BIOS launcher starts GAME\\ cards only. A card whose executable
     * lives elsewhere (freeware GPMM\\) stays on the DATA LOADING screen when the
     * BIOS boots it, so the bench boots it the way its frontend would. --state
     * replays a recorded machine and --force-bios measure the BIOS path itself. */
    if (!force_bios && !state_path) {
        char layout_exe[260], layout_err[256];
        smc_card_launch_layout_t layout = smc_direct_classify_file(smc, layout_exe, sizeof(layout_exe), layout_err, sizeof(layout_err));
        if (layout == SMC_CARD_LAYOUT_DIRECT_ONLY || (force_direct && layout != SMC_CARD_LAYOUT_NONE)) {
            fprintf(stderr, "boot path: direct SmartMedia (starts from %s; %s)\n",
                    layout_exe[0] ? layout_exe : "a freeware layout",
                    force_direct && layout != SMC_CARD_LAYOUT_DIRECT_ONLY
                        ? "forced by --force-direct"
                        : "the retail BIOS launcher boots GAME\\ cards only");
            opt.bios_path = NULL;
        }
    }
    gp32_t *g = gp32_create(&opt);
    if (!g) { fprintf(stderr, "gp32_create/load failed (bios=%s smc=%s)\n", bios, smc); gp32_input_script_destroy(script); return 1; }
    if (jit && gp32_set_jit(g, 1) != GP32_OK) {
        fprintf(stderr, "gp32_set_jit failed: %s\n", gp32_get_error(g));
        gp32_input_script_destroy(script); gp32_destroy(g); return 1;
    }
    if (state_path && gp32_load_state(g, state_path) != GP32_OK) {
        fprintf(stderr, "gp32_load_state failed: %s\n", gp32_get_error(g));
        gp32_input_script_destroy(script); gp32_destroy(g); return 1;
    }
    if (cpu_speed != 100u && gp32_set_cpu_speed_percent(g, cpu_speed) != GP32_OK) {
        fprintf(stderr, "gp32_set_cpu_speed_percent failed: %s\n", gp32_get_error(g));
        gp32_input_script_destroy(script); gp32_destroy(g); return 1;
    }

    const uint64_t total = warmup + frames;
    uint64_t cycle_accum = 0, script_frame = 0;
    uint64_t video_hash = FNV64_OFFSET, audio_hash = FNV64_OFFSET;
    uint64_t audio_frames = 0, measured = 0;
    /* Playback-buffer model for a real-time frontend: each measured frame
     * drains rate/60 source samples and produced PCM refills it. The credit
     * uses 1/60-sample units so fractional rates accumulate exactly, starts
     * with one frame of slack, and re-arms after a dry frame. */
    uint64_t audio_underrun_risk = 0;
    int64_t audio_credit_q = 0;
    uint32_t audio_play_rate = 0;
    /* Panel cadence: a frontend presents the newest LCD frame once per host
     * frame, so a zero counter delta repeats the previous picture and a delta
     * above one loses every panel frame but the last. */
    uint64_t lcd_frames = 0, lcd_repeat_frames = 0, lcd_unpresented_frames = 0, lcd_counter = 0;
    int lcd_have_counter = 0;
    double t0 = 0.0, elapsed = 0.0;
    uint64_t frame_ticks0 = 0, cycles_at_warmup = 0;
    frame_counters_t frame_ctr_before;
    int timed = 0;
    gp32_status_t st = GP32_OK;

    memset(&frame_ctr_before, 0, sizeof(frame_ctr_before));

    for (uint64_t frame = 0; frame < total; ++frame) {
        uint32_t buttons;
        if (script) {
            buttons = gp32_input_script_frame(script, script_frame++);
        } else {
            /* The auto A pulses exist to leave the BIOS menu and its card
             * loading screen; a direct-booted card, like a state replay, has
             * neither and must see the input sequence it measured before. */
            buttons = (state_path || !opt.bios_path) ? 0u : bios_auto_start_buttons_for_frame(frame);
        }
        gp32_set_buttons(g, buttons);

        if (!timed && frame >= warmup) {
            timed = 1;
            cycles_at_warmup = gp32_get_cycles(g);
            t0 = now_seconds();
            if (cpu_profile) gp32_reset_cpu_profile(g);
        }
        if (timed && frame_times) {
            frame_ticks0 = now_ticks();
            if (frame_ctr) frame_ctr_before = frame_counters_snapshot(g);
        }

        st = legacy_cycle_frames ? gp32_run_cycles(g, frame_cycles_for(g, &cycle_accum))
                                 : gp32_run_frame(g);
        if (st != GP32_OK) break;

        if (timed) {
            gp32_framebuffer_desc_t fb;
            if (gp32_get_framebuffer(g, &fb) == GP32_OK) {
                video_hash = hash_framebuffer(video_hash, &fb);
                if (lcd_have_counter) {
                    uint64_t delta = fb.frame_counter - lcd_counter;
                    lcd_frames += delta;
                    if (!delta) ++lcd_repeat_frames;
                    else if (delta > 1u) lcd_unpresented_frames += delta - 1u;
                }
                lcd_counter = fb.frame_counter;
                lcd_have_counter = 1;
            }
            gp32_audio_desc_t aud;
            uint64_t produced = 0;
            while (gp32_get_audio(g, &aud) == GP32_OK && aud.frame_count) {
                audio_hash = hash_audio(audio_hash, &aud);
                audio_frames += aud.frame_count;
                produced += aud.frame_count;
                if (gp32_consume_audio(g, aud.frame_count) != GP32_OK) break;
            }
            if (aud.sample_rate_hz) {
                if (!audio_play_rate) audio_credit_q = aud.sample_rate_hz;
                audio_play_rate = aud.sample_rate_hz;
            }
            if (audio_play_rate) {
                audio_credit_q += (int64_t)produced * 60;
                audio_credit_q -= audio_play_rate;
                if (audio_credit_q < 0) {
                    ++audio_underrun_risk;
                    audio_credit_q = audio_play_rate;
                }
            }
            ++measured;
        }
        /* Drain the audio queue every frame like a real frontend. */
        gp32_clear_audio(g);
        if (timed && frame_ms) {
            size_t slot = (size_t)measured - 1u;
            frame_ms[slot] = (double)(now_ticks() - frame_ticks0) / ticks_per_ms();
            if (frame_ctr)
                frame_ctr[slot] = frame_counters_delta(frame_counters_snapshot(g), frame_ctr_before);
        }
    }
    if (timed) elapsed = now_seconds() - t0;

    if (st != GP32_OK) {
        fprintf(stderr, "run failed: %s\n", gp32_get_error(g));
        gp32_input_script_destroy(script); gp32_destroy(g); return 1;
    }

    double fps = elapsed > 0.0 ? (double)measured / elapsed : 0.0;
    /* The top-level "cycles" field is the absolute counter and therefore
     * includes every warmup frame; cycles_measured is that counter's delta
     * across the measured window, so guest time derives from exactly the
     * frames fps/elapsed describe without arithmetic on warmup. */
    uint64_t cycles_end = gp32_get_cycles(g);
    uint64_t cycles_measured = timed ? cycles_end - cycles_at_warmup : 0;
    printf("{\"fps\":%.3f,\"elapsed\":%.6f,\"frames\":%" PRIu64
           ",\"warmup\":%" PRIu64 ",\"cycles\":%" PRIu64
           ",\"cycles_measured\":%" PRIu64
           ",\"pc\":\"0x%08" PRIx32 "\",\"cpsr\":\"0x%08" PRIx32 "\""
           ",\"clock\":%" PRIu32 ",\"audio_frames\":%" PRIu64
           ",\"audio_underrun_risk\":%" PRIu64
           ",\"lcd_frames\":%" PRIu64 ",\"lcd_repeat_frames\":%" PRIu64
           ",\"lcd_unpresented_frames\":%" PRIu64
           ",\"video_hash\":\"%016" PRIx64 "\",\"audio_hash\":\"%016" PRIx64 "\""
           ",\"jit\":%d",
           fps, elapsed, measured, warmup, cycles_end, cycles_measured,
           gp32_get_pc(g), gp32_get_cpsr(g), gp32_get_run_clock_hz(g),
           audio_frames, audio_underrun_risk, lcd_frames, lcd_repeat_frames,
           lcd_unpresented_frames, video_hash, audio_hash, jit);
    printf(",\"frame_pacing\":\"%s\"", legacy_cycle_frames ? "legacy_cycles" : "time");
    if (frame_times && frame_times_raw) {
        printf(",\"frame_ms\":[");
        for (uint64_t i = 0; i < measured; ++i) printf("%s%.3f", i ? "," : "", frame_ms[i]);
        printf("]");
        if (frame_ctr) {
            printf(",\"frame_counters\":[");
            for (uint64_t i = 0; i < measured; ++i) {
                printf("%s{\"index\":%" PRIu64, i ? "," : "", i);
                print_frame_counters_json(&frame_ctr[i]);
                printf("}");
            }
            printf("]");
        }
    }
    if (frame_times) print_frame_times_json(frame_ms, (size_t)measured, frame_ctr);
    if (cpu_profile) {
        gp32_cpu_profile_t p;
        if (gp32_get_cpu_profile(g, &p) == GP32_OK) print_cpu_profile_json(&p);
        else printf(",\"cpu_profile\":{\"supported\":0}");
    }
    printf("}\n");

    free(frame_ms);
    free(frame_ctr);
    gp32_input_script_destroy(script);
    gp32_destroy(g);
    return 0;
}
