/* Bounded library exploration: JSONL workload windows, screenshots and state.
 * Timings from parallel/profile runs are diagnostics, not speed acceptance.
 * No game data is bundled. Each invocation needs its own output directory. */
#include "gp32emu/gp32.h"
#include "input_script.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t capture(gp32_t *g, unsigned frame, int write_image) {
    gp32_framebuffer_desc_t fb;
    if (gp32_get_framebuffer(g, &fb) != GP32_OK) return 0;
    uint64_t hash = 14695981039346656037ULL;
    FILE *out = NULL;
    if (write_image) {
        char name[48]; snprintf(name, sizeof(name), "frame-%06u.ppm", frame);
        out = fopen(name, "wb");
        if (!out) { perror(name); exit(1); }
        /* GP32 scanout is portrait; rotate counterclockwise for inspection. */
        fprintf(out, "P6\n%u %u\n255\n", fb.height, fb.width);
    }
    for (unsigned y = 0; y < fb.width; y++) {
        for (unsigned x = 0; x < fb.height; x++) {
            uint32_t p = fb.pixels_rgba8888[x * fb.stride_pixels + fb.width - 1 - y];
            unsigned char rgb[3] = {(unsigned char)(p >> 16), (unsigned char)(p >> 8), (unsigned char)p};
            for (unsigned j = 0; j < 3; j++) hash = (hash ^ rgb[j]) * 1099511628211ULL;
            if (out && fwrite(rgb, 1, 3, out) != 3) { perror("capture write"); exit(1); }
        }
    }
    if (out && fclose(out)) { perror("capture close"); exit(1); }
    return hash;
}

int main(int argc, char **argv) {
    if (argc != 6) { fprintf(stderr, "usage: library_probe BIOS SMC FRAMES STATE-or-- INPUT-or--\n"); return 2; }
    char *end; unsigned long n = strtoul(argv[3], &end, 10);
    if (*end || !n || n > 36000) return 2;
    gp32_options_t opt = {0}; opt.bios_path = argv[1]; opt.smartmedia_path = argv[2];
    gp32_t *g = gp32_create(&opt); if (!g) return 1;
    gp32_input_script_t *script = NULL; char error[256] = {0};
    int state = strcmp(argv[4], "-") != 0;
    if (gp32_set_jit(g, 1) != GP32_OK ||
        (state && gp32_load_state(g, argv[4]) != GP32_OK)) goto failed;
    if (strcmp(argv[5], "-") && !gp32_input_script_load(argv[5], &script, error, sizeof(error))) {
        fprintf(stderr, "%s\n", error); goto failed;
    }
    uint64_t audio = 0, nonzero = 0, changes = 0, prior = 0, audio_hash = 14695981039346656037ULL;
    gp32_reset_cpu_profile(g);
    for (unsigned i = 0; i < n; i++) {
        uint32_t buttons = script ? gp32_input_script_frame(script, i) :
            (!state && ((i >= 620 && i < 660) || (i >= 1450 && i < 1490) ||
                        (i >= 1800 && i < 1840) || (i >= 2200 && i < 2240)) ? GP32_BUTTON_A : 0);
        gp32_set_buttons(g, buttons);
        if (gp32_run_frame(g) != GP32_OK) goto failed;
        int boundary = (i + 1) % 300 == 0 || i + 1 == n;
        uint64_t hash = capture(g, i + 1, boundary);
        changes += i != 0 && hash != prior; prior = hash;
        gp32_audio_desc_t a;
        while (gp32_get_audio(g, &a) == GP32_OK && a.frame_count) {
            audio += a.frame_count;
            for (uint64_t j = 0; j < a.frame_count * 2; j++) {
                uint16_t sample = (uint16_t)a.samples_s16_interleaved[j];
                nonzero += sample != 0;
                audio_hash = (audio_hash ^ (sample & 255)) * 1099511628211ULL;
                audio_hash = (audio_hash ^ (sample >> 8)) * 1099511628211ULL;
            }
            if (gp32_consume_audio(g, a.frame_count) != GP32_OK) goto failed;
        }
        if (boundary) {
            gp32_cpu_profile_t p = {0}; gp32_get_cpu_profile(g, &p);
            printf("{\"frame\":%u,\"pc\":\"%08x\",\"clock\":%u,\"cycles\":%" PRIu64
                   ",\"video_hash\":\"%016" PRIx64 "\",\"audio_hash\":\"%016" PRIx64
                   "\",\"audio_frames\":%" PRIu64 ",\"nonzero_samples\":%" PRIu64
                   ",\"video_changes\":%" PRIu64 ",\"profile_supported\":%u,\"native\":%" PRIu64
                   ",\"portable\":%" PRIu64 ",\"interp_arm\":%" PRIu64 ",\"interp_thumb\":%" PRIu64
                   ",\"native_failed\":%" PRIu64 ",\"code_full\":%" PRIu64 ",\"invalidations\":%" PRIu64 "}\n",
                   i + 1, gp32_get_pc(g), gp32_get_run_clock_hz(g), gp32_get_cycles(g), hash, audio_hash,
                   audio, nonzero, changes, p.supported, p.native_arm_insns, p.block_interp_arm_insns,
                   p.interp_arm_insns, p.interp_thumb_insns, p.jit_native_failed, p.jit_code_full_events, p.jit_invalidations);
            fflush(stdout); gp32_reset_cpu_profile(g);
            audio = nonzero = changes = 0; audio_hash = 14695981039346656037ULL;
        }
    }
    if (gp32_save_state(g, "end.state") != GP32_OK) goto failed;
    gp32_input_script_destroy(script); gp32_destroy(g); return 0;
failed:
    fprintf(stderr, "probe failed: %s\n", gp32_get_error(g));
    gp32_input_script_destroy(script); gp32_destroy(g); return 1;
}
