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
 * Portable C11: QueryPerformanceCounter on Windows, CLOCK_MONOTONIC elsewhere.
 * Depends only on libgp32emu (gp32emu/gp32.h) and src/input_script.h.
 */
#ifndef _WIN32
#define _POSIX_C_SOURCE 199309L
#endif

#include "gp32emu/gp32.h"
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

/* Same confirm-pulse schedule as headless_main's BIOS auto-start. */
static uint32_t bios_auto_start_buttons_for_frame(uint64_t frame) {
    return ((frame >= 620u && frame < 660u) ||
            (frame >= 1450u && frame < 1490u) ||
            (frame >= 1800u && frame < 1840u) ||
            (frame >= 2200u && frame < 2240u)) ? GP32_BUTTON_A : 0u;
}

/* One emulated frame: dynamic budget = run clock / 60 with carry. */
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

static int usage(const char *argv0) {
    fprintf(stderr,
        "usage: %s --bios bios.bin --smc game.smc [--state file] [--warmup N=2400] [--frames N=600] [--jit] [--input-script script.txt] [--cpu-profile]\n"
        "Times --frames frames after --warmup warmup frames. BIOS+SMC runs without an input script get the same\n"
        "auto A pulses as headless_main. Prints one JSON object: fps, elapsed, cycles, pc, cpsr, clock,\n"
        "audio_frames, video/audio FNV-1a-64 hashes. --cpu-profile resets CPU workload counters at the\n"
        "warmup boundary and appends a cpu_profile object (requires a GP32EMU_CPU_PROFILE build).\n",
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
    int jit = 0, cpu_profile = 0;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--bios") && i + 1 < argc) bios = argv[++i];
        else if (!strcmp(argv[i], "--smc") && i + 1 < argc) smc = argv[++i];
        else if (!strcmp(argv[i], "--state") && i + 1 < argc) state_path = argv[++i];
        else if (!strcmp(argv[i], "--warmup") && i + 1 < argc) warmup = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--jit")) jit = 1;
        else if (!strcmp(argv[i], "--cpu-profile")) cpu_profile = 1;
        else if (!strcmp(argv[i], "--input-script") && i + 1 < argc) input_script_path = argv[++i];
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(argv[0]); return 0; }
        else { fprintf(stderr, "unknown or incomplete option: %s\n", argv[i]); return usage(argv[0]); }
    }
    if (!bios || !smc) { fprintf(stderr, "--bios and --smc are required\n"); return usage(argv[0]); }

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

    const uint64_t total = warmup + frames;
    uint64_t cycle_accum = 0, script_frame = 0;
    uint64_t video_hash = FNV64_OFFSET, audio_hash = FNV64_OFFSET;
    uint64_t audio_frames = 0, measured = 0;
    double t0 = 0.0, elapsed = 0.0;
    int timed = 0;
    gp32_status_t st = GP32_OK;

    for (uint64_t frame = 0; frame < total; ++frame) {
        uint32_t buttons;
        if (script) {
            buttons = gp32_input_script_frame(script, script_frame++);
        } else {
            buttons = state_path ? 0u : bios_auto_start_buttons_for_frame(frame);
        }
        gp32_set_buttons(g, buttons);

        if (!timed && frame >= warmup) { timed = 1; t0 = now_seconds(); if (cpu_profile) gp32_reset_cpu_profile(g); }

        uint32_t n = frame_cycles_for(g, &cycle_accum);
        st = gp32_run_cycles(g, n);
        if (st != GP32_OK) break;

        if (timed) {
            gp32_framebuffer_desc_t fb;
            if (gp32_get_framebuffer(g, &fb) == GP32_OK) video_hash = hash_framebuffer(video_hash, &fb);
            gp32_audio_desc_t aud;
            if (gp32_get_audio(g, &aud) == GP32_OK) {
                audio_hash = hash_audio(audio_hash, &aud);
                audio_frames += aud.frame_count;
            }
            ++measured;
        }
        /* Drain the audio queue every frame like a real frontend. */
        gp32_clear_audio(g);
    }
    if (timed) elapsed = now_seconds() - t0;

    if (st != GP32_OK) {
        fprintf(stderr, "run failed: %s\n", gp32_get_error(g));
        gp32_input_script_destroy(script); gp32_destroy(g); return 1;
    }

    double fps = elapsed > 0.0 ? (double)measured / elapsed : 0.0;
    printf("{\"fps\":%.3f,\"elapsed\":%.6f,\"frames\":%" PRIu64
           ",\"warmup\":%" PRIu64 ",\"cycles\":%" PRIu64
           ",\"pc\":\"0x%08" PRIx32 "\",\"cpsr\":\"0x%08" PRIx32 "\""
           ",\"clock\":%" PRIu32 ",\"audio_frames\":%" PRIu64
           ",\"video_hash\":\"%016" PRIx64 "\",\"audio_hash\":\"%016" PRIx64 "\""
           ",\"jit\":%d",
           fps, elapsed, measured, warmup, gp32_get_cycles(g),
           gp32_get_pc(g), gp32_get_cpsr(g), gp32_get_run_clock_hz(g),
           audio_frames, video_hash, audio_hash, jit);
    if (cpu_profile) {
        gp32_cpu_profile_t p;
        if (gp32_get_cpu_profile(g, &p) == GP32_OK) print_cpu_profile_json(&p);
        else printf(",\"cpu_profile\":{\"supported\":0}");
    }
    printf("}\n");

    gp32_input_script_destroy(script);
    gp32_destroy(g);
    return 0;
}
