/*
 * compat_probe.c - input-driven compatibility sweep harness.
 *
 * Drives the machine exactly like bench_core.c: same warmup/measured split,
 * same per-frame input script, same framebuffer/audio consumption, and the
 * same identity JSON on stdout. On top of that it adds two opt-in side
 * channels used by the sweep:
 *
 *   --log FILE         every core log line, prefixed with the frame that
 *                      produced it (unmapped MMIO writes, undefined
 *                      instruction and abort vector entries)
 *   --frames-csv FILE  one row per measured frame: frame,pc,cpsr,video_hash
 *                      so static pictures and stalled PCs can be found
 *                      offline without re-running the game
 *   --dump-at LIST     comma-separated measured frame numbers to save as PPM
 *   --dump-prefix P    file prefix for the PPM dumps (<P>.<frame>.ppm)
 *   --label NAME       echoed into both files for post-mortem bookkeeping
 *
 * The framebuffer is read once per measured frame, exactly like the
 * benchmark: gp32_get_framebuffer applies the GP32 shadow repair fixups, so
 * reading it during warmup would change the guest-visible result.
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

#define FNV64_OFFSET 14695981039346656037ULL
#define FNV64_PRIME 1099511628211ULL

static FILE *g_log_file;
static FILE *g_csv_file;
static uint64_t g_frame;
static uint64_t g_log_lines;
static uint64_t g_dump_at[16];
static unsigned g_dump_count;
static const char *g_dump_prefix;

static void log_sink(void *user, const char *message) {
    (void)user;
    if (!g_log_file || !message) return;
    fprintf(g_log_file, "%" PRIu64 ",%s\n", g_frame, message);
    ++g_log_lines;
    if ((g_log_lines & 0xffu) == 0u) fflush(g_log_file);
}

static uint64_t fnv1a64_update(uint64_t h, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    while (len--) {
        h ^= (uint64_t)*p++;
        h *= FNV64_PRIME;
    }
    return h;
}

static uint32_t bios_auto_start_buttons_for_frame(uint64_t frame) {
    return ((frame >= 620u && frame < 660u) ||
            (frame >= 1450u && frame < 1490u) ||
            (frame >= 1800u && frame < 1840u) ||
            (frame >= 2200u && frame < 2240u)) ? GP32_BUTTON_A : 0u;
}

static uint64_t hash_framebuffer(uint64_t h, const gp32_framebuffer_desc_t *fb) {
    if (!fb->pixels_rgba8888 || !fb->width || !fb->height) return h;
    for (uint32_t y = 0; y < fb->height; ++y) {
        const uint8_t *row = (const uint8_t *)(fb->pixels_rgba8888 + (size_t)y * fb->stride_pixels);
        h = fnv1a64_update(h, row, (size_t)fb->width * sizeof(uint32_t));
    }
    return h;
}

/* Save one measured frame as binary PPM so a sweep can show where a game
 * actually is. Only called on measured frames, after the same single
 * gp32_get_framebuffer call the benchmark makes. */
static void dump_framebuffer(const gp32_framebuffer_desc_t *fb) {
    if (!g_dump_prefix || !fb->pixels_rgba8888) return;
    char name[512];
    snprintf(name, sizeof(name), "%s.%06" PRIu64 ".ppm", g_dump_prefix, g_frame);
    FILE *f = fopen(name, "wb");
    if (!f) { fprintf(stderr, "dump %s failed\n", name); return; }
    fprintf(f, "P6\n%u %u\n255\n", fb->width, fb->height);
    for (uint32_t y = 0; y < fb->height; ++y) {
        const uint8_t *row = (const uint8_t *)(fb->pixels_rgba8888 + (size_t)y * fb->stride_pixels);
        for (uint32_t x = 0; x < fb->width; ++x) {
            uint8_t rgb[3] = { row[x * 4u + 0u], row[x * 4u + 1u], row[x * 4u + 2u] };
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
}

static uint64_t hash_audio(uint64_t h, const gp32_audio_desc_t *aud) {
    if (!aud->samples_s16_interleaved || !aud->frame_count) return h;
    return fnv1a64_update(h, aud->samples_s16_interleaved,
                          (size_t)aud->frame_count * 2u * sizeof(int16_t));
}

static int usage(const char *argv0) {
    fprintf(stderr,
        "usage: %s --bios bios.bin --smc game.smc [--warmup N=2400] [--frames N=600] [--jit]\n"
        "       [--input-script script.txt] [--log FILE] [--frames-csv FILE] [--label NAME]\n"
        "       [--dump-at N,N,..] [--dump-prefix P] [--force-bios]\n"
        "Same deterministic drive and identity JSON as gp32_bench, plus frame-stamped core log\n"
        "lines and a per-frame PC/video hash trace for compatibility sweeps.\n",
        argv0);
    return 2;
}

int main(int argc, char **argv) {
    const char *bios = NULL, *smc = NULL, *input_script_path = NULL;
    const char *log_path = NULL, *csv_path = NULL, *label = NULL;
    uint64_t warmup = 2400, frames = 600;
    int jit = 0, force_bios = 0;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--bios") && i + 1 < argc) bios = argv[++i];
        else if (!strcmp(argv[i], "--smc") && i + 1 < argc) smc = argv[++i];
        else if (!strcmp(argv[i], "--warmup") && i + 1 < argc) warmup = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--input-script") && i + 1 < argc) input_script_path = argv[++i];
        else if (!strcmp(argv[i], "--log") && i + 1 < argc) log_path = argv[++i];
        else if (!strcmp(argv[i], "--frames-csv") && i + 1 < argc) csv_path = argv[++i];
        else if (!strcmp(argv[i], "--label") && i + 1 < argc) label = argv[++i];
        else if (!strcmp(argv[i], "--dump-at") && i + 1 < argc) {
            char *list = argv[++i];
            for (char *tok = strtok(list, ","); tok && g_dump_count < 16u; tok = strtok(NULL, ",")) {
                g_dump_at[g_dump_count++] = strtoull(tok, NULL, 0);
            }
        }
        else if (!strcmp(argv[i], "--dump-prefix") && i + 1 < argc) g_dump_prefix = argv[++i];
        else if (!strcmp(argv[i], "--jit")) jit = 1;
        else if (!strcmp(argv[i], "--force-bios")) force_bios = 1;
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(argv[0]); return 0; }
        else { fprintf(stderr, "unknown or incomplete option: %s\n", argv[i]); return usage(argv[0]); }
    }
    if (!bios || !smc) { fprintf(stderr, "--bios and --smc are required\n"); return usage(argv[0]); }

    if (log_path) {
        g_log_file = fopen(log_path, "wb");
        if (!g_log_file) { fprintf(stderr, "open %s failed\n", log_path); return 1; }
        fprintf(g_log_file, "# compat_probe log label=%s smc=%s jit=%d warmup=%" PRIu64 " frames=%" PRIu64 "\n",
                label ? label : "-", smc, jit, warmup, frames);
    }
    if (csv_path) {
        g_csv_file = fopen(csv_path, "wb");
        if (!g_csv_file) { fprintf(stderr, "open %s failed\n", csv_path); return 1; }
        fprintf(g_csv_file, "# frame,pc,cpsr,video_hash\n");
    }

    gp32_input_script_t *script = NULL;
    if (input_script_path) {
        char err[256] = {0};
        if (!gp32_input_script_load(input_script_path, &script, err, sizeof(err))) {
            fprintf(stderr, "input script load failed: %s\n", err[0] ? err : "unknown error");
            if (g_log_file) fclose(g_log_file);
            if (g_csv_file) fclose(g_csv_file);
            return 1;
        }
    }

    gp32_options_t opt;
    memset(&opt, 0, sizeof(opt));
    opt.bios_path = bios;
    opt.smartmedia_path = smc;
    if (g_log_file) opt.log = log_sink;

    if (!force_bios) {
        char layout_exe[260], layout_err[256];
        smc_card_launch_layout_t layout = smc_direct_classify_file(smc, layout_exe, sizeof(layout_exe), layout_err, sizeof(layout_err));
        if (layout == SMC_CARD_LAYOUT_DIRECT_ONLY) {
            fprintf(stderr, "boot path: direct SmartMedia (starts from %s; the retail BIOS launcher boots GAME\\ cards only)\n",
                    layout_exe[0] ? layout_exe : "a freeware layout");
            opt.bios_path = NULL;
        }
    }
    gp32_t *g = gp32_create(&opt);
    if (!g) {
        fprintf(stderr, "gp32_create/load failed (bios=%s smc=%s)\n", bios, smc);
        gp32_input_script_destroy(script);
        if (g_log_file) fclose(g_log_file);
        if (g_csv_file) fclose(g_csv_file);
        return 1;
    }
    if (log_path) gp32_set_diag_log(g, log_sink, NULL);
    if (jit && gp32_set_jit(g, 1) != GP32_OK) {
        fprintf(stderr, "gp32_set_jit failed: %s\n", gp32_get_error(g));
        gp32_input_script_destroy(script); gp32_destroy(g);
        if (g_log_file) fclose(g_log_file);
        if (g_csv_file) fclose(g_csv_file);
        return 1;
    }

    const uint64_t total = warmup + frames;
    uint64_t script_frame = 0, audio_frames = 0, measured = 0;
    uint64_t video_hash = FNV64_OFFSET, audio_hash = FNV64_OFFSET;
    int timed = 0;
    gp32_status_t st = GP32_OK;

    for (uint64_t frame = 0; frame < total; ++frame) {
        uint32_t buttons;
        if (script) buttons = gp32_input_script_frame(script, script_frame++);
        else buttons = (opt.bios_path ? bios_auto_start_buttons_for_frame(frame) : 0u);
        gp32_set_buttons(g, buttons);

        if (!timed && frame >= warmup) timed = 1;
        g_frame = frame;

        st = gp32_run_frame(g);
        if (st != GP32_OK) break;

        if (timed) {
            gp32_framebuffer_desc_t fb;
            uint32_t pc_now = gp32_get_pc(g), cpsr_now = gp32_get_cpsr(g);
            uint64_t fb_hash = 0;
            if (gp32_get_framebuffer(g, &fb) == GP32_OK) {
                fb_hash = hash_framebuffer(FNV64_OFFSET, &fb);
                video_hash = hash_framebuffer(video_hash, &fb);
                for (unsigned i = 0; i < g_dump_count; ++i) {
                    if (g_dump_at[i] == frame) dump_framebuffer(&fb);
                }
            }
            if (g_csv_file) fprintf(g_csv_file, "%" PRIu64 ",%08" PRIx32 ",%08" PRIx32 ",%016" PRIx64 "\n",
                                    frame, pc_now, cpsr_now, fb_hash);
            gp32_audio_desc_t aud;
            while (gp32_get_audio(g, &aud) == GP32_OK && aud.frame_count) {
                audio_hash = hash_audio(audio_hash, &aud);
                audio_frames += aud.frame_count;
                if (gp32_consume_audio(g, aud.frame_count) != GP32_OK) break;
            }
            ++measured;
        }
        gp32_clear_audio(g);
    }

    if (st != GP32_OK) {
        fprintf(stderr, "run failed at frame %" PRIu64 ": %s\n", g_frame, gp32_get_error(g));
        gp32_input_script_destroy(script); gp32_destroy(g);
        if (g_log_file) fclose(g_log_file);
        if (g_csv_file) fclose(g_csv_file);
        return 1;
    }

    printf("{\"frames\":%" PRIu64 ",\"warmup\":%" PRIu64 ",\"cycles\":%" PRIu64
           ",\"pc\":\"0x%08" PRIx32 "\",\"cpsr\":\"0x%08" PRIx32 "\""
           ",\"clock\":%" PRIu32 ",\"audio_frames\":%" PRIu64
           ",\"video_hash\":\"%016" PRIx64 "\",\"audio_hash\":\"%016" PRIx64 "\""
           ",\"jit\":%d}\n",
           measured, warmup, gp32_get_cycles(g),
           gp32_get_pc(g), gp32_get_cpsr(g), gp32_get_run_clock_hz(g),
           audio_frames, video_hash, audio_hash, jit);

    if (g_log_file) {
        fprintf(stderr, "compat_probe: log_lines=%" PRIu64 "\n", g_log_lines);
        fclose(g_log_file);
        g_log_file = NULL;
    }
    if (g_csv_file) fclose(g_csv_file);
    gp32_input_script_destroy(script);
    gp32_destroy(g);
    return 0;
}
