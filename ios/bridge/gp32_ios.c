/*
 * gp32emu iOS bridge implementation. Plain C11, no Apple framework here, so a
 * host without an iOS SDK can still syntax-check it (see ios/README.md).
 *
 * Every gp32_* call in this file runs on the single owner queue the ObjC
 * wrapper provides (ios/app/GP32Emulator.m). The only entry point the audio
 * thread may call is gp32_ios_audio_sink_read().
 *
 * GP32/S3C2400 model: BSD-3-Clause, derived from MAME's Game Park GP32 driver.
 * See licenses/ and ios/CREDITS.md. No BIOS or game image ships with this
 * source or with a bundle built from it.
 */

#include "gp32_ios.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio/gp32_audio_resampler.h"
#include "smc_direct.h"

/* Source frames handed to the resampler per call. The worst case output for a
 * chunk at the slowest guest IIS rate (11025 Hz) is 4x + a couple of carry
 * frames, so the fixed scratch below always fits the kernel's bound and the
 * delivery path never allocates. */
#define GP32_IOS_AUDIO_IN_CHUNK 512u
#define GP32_IOS_AUDIO_SCRATCH_FRAMES 4096u

/* Host-side boot decision, mirroring the libretro frontend. */
typedef enum ios_boot {
    IOS_BOOT_BIOS = 0,
    IOS_BOOT_DIRECT = 1,       /* extract the executable, mount no card */
    IOS_BOOT_DIRECT_MEDIA = 2  /* extract the executable, keep the card mounted */
} ios_boot_t;

typedef enum ios_ext {
    IOS_EXT_SMC = 0,
    IOS_EXT_FXE = 1,
    IOS_EXT_FPK = 2
} ios_ext_t;

struct gp32_ios_emulator {
    gp32_t *core;
    gp32_ios_audio_sink_t *sink;
    gp32_log_fn log;
    void *log_user;

    char bios_path[GP32_IOS_PATH_MAX];
    char game_path[GP32_IOS_PATH_MAX];
    char game_label[128];
    char card_save_path[GP32_IOS_PATH_MAX]; /* set while a writable card is mounted */
    char state_path[GP32_IOS_PATH_MAX];
    gp32_ios_boot_mode_t boot_mode;

    int have_game;
    int restored_card_progress;
    int card_save_reported;
    uint32_t buttons;
    uint64_t frame_counter;
    uint32_t last_underruns;

    gp32_audio_resampler_t resampler;
    int16_t scratch[GP32_IOS_AUDIO_SCRATCH_FRAMES * 2u];
    uint32_t staged[GP32_IOS_FRAME_PIXELS];

    char error[256];
};

/* ---- Small helpers ---- */

static void set_error(gp32_ios_emulator_t *e, const char *message) {
    if (!e) return;
    snprintf(e->error, sizeof(e->error), "%s", (message && message[0]) ? message : "unknown error");
}

static void set_core_error(gp32_ios_emulator_t *e) {
    const char *detail = (e && e->core) ? gp32_get_error(e->core) : NULL;
    set_error(e, (detail && detail[0]) ? detail : "unknown error");
}

static void log_line(gp32_ios_emulator_t *e, const char *message) {
    if (e && e->log && message) e->log(e->log_user, message);
}

static int copy_text(char *dst, size_t cap, const char *src) {
    if (!dst || !cap) return 0;
    if (!src) { dst[0] = '\0'; return 1; }
    size_t len = strlen(src);
    if (len >= cap) return 0;
    memcpy(dst, src, len + 1u);
    return 1;
}

static int path_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static int ext_is(const char *path, const char *ext) {
    size_t path_len = strlen(path), ext_len = strlen(ext);
    if (path_len < ext_len) return 0;
    const char *tail = path + (path_len - ext_len);
    for (size_t i = 0; i < ext_len; ++i) {
        if (tolower((unsigned char)tail[i]) != tolower((unsigned char)ext[i])) return 0;
    }
    return 1;
}

static const char *path_basename(const char *path) {
    const char *base = path;
    for (const char *p = path; p && *p; ++p) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    return base && base[0] ? base : "gp32-content";
}

/* ---- Audio delivery ---- */

void gp32_ios_audio_sink_init(gp32_ios_audio_sink_t *s) {
    if (!s) return;
    memset(s->samples, 0, sizeof(s->samples));
    atomic_store_explicit(&s->head, 0u, memory_order_relaxed);
    atomic_store_explicit(&s->tail, 0u, memory_order_relaxed);
    atomic_store_explicit(&s->flush_request, 0u, memory_order_relaxed);
    atomic_store_explicit(&s->underrun_events, 0u, memory_order_relaxed);
    atomic_store_explicit(&s->dropped_frames, 0u, memory_order_relaxed);
    atomic_store_explicit(&s->layout_mismatch_events, 0u, memory_order_relaxed);
}

void gp32_ios_audio_sink_request_flush(gp32_ios_audio_sink_t *s) {
    if (!s) return;
    /* Only a request: tail belongs to the consumer, which applies the discard
     * at its next read, so this stays correct while a callback is running. */
    atomic_store_explicit(&s->flush_request, 1u, memory_order_release);
}

void gp32_ios_audio_sink_write(gp32_ios_audio_sink_t *s, const int16_t *frames,
                               size_t count, int *dropped) {
    if (dropped) *dropped = 0;
    if (!s || !frames || !count) return;

    /* Producer side only: head is owned here, tail is sampled, never written.
     * Occupancy comes from the index difference, not from comparing head and
     * tail values, so a full ring is unambiguous. */
    uint32_t head = atomic_load_explicit(&s->head, memory_order_relaxed);
    uint32_t tail = atomic_load_explicit(&s->tail, memory_order_acquire);
    size_t used = (size_t)(head - tail);
    size_t free_frames = (size_t)GP32_IOS_AUDIO_RING_FRAMES - used;
    size_t accept = (count < free_frames) ? count : free_frames;

    if (accept < count) {
        /* Drop the newest frames: the retained queue stays continuous with what
         * the device has already heard, and the consumer's tail is never moved
         * by the producer. A stalled device costs the newest audio, never
         * memory and never a race on tail. */
        atomic_fetch_add_explicit(&s->dropped_frames, (uint32_t)(count - accept), memory_order_relaxed);
        if (dropped) *dropped = 1;
    }
    if (!accept) return;

    size_t index = (size_t)(head % GP32_IOS_AUDIO_RING_FRAMES);
    size_t first = accept;
    if (index + accept > GP32_IOS_AUDIO_RING_FRAMES) first = GP32_IOS_AUDIO_RING_FRAMES - index;
    memcpy(&s->samples[index * 2u], frames, first * 2u * sizeof(int16_t));
    if (accept > first)
        memcpy(&s->samples[0], frames + first * 2u, (accept - first) * 2u * sizeof(int16_t));

    atomic_store_explicit(&s->head, head + (uint32_t)accept, memory_order_release);
}

size_t gp32_ios_audio_sink_read(gp32_ios_audio_sink_t *s, int16_t *dst, size_t count) {
    if (!s || !dst || !count) return 0;

    /* Consumer-owned flush: the producer only raises the request, so the tail
     * index is never written by two threads. Anything produced after the head
     * observed here is kept. */
    if (atomic_exchange_explicit(&s->flush_request, 0u, memory_order_acq_rel)) {
        uint32_t flushed_head = atomic_load_explicit(&s->head, memory_order_acquire);
        atomic_store_explicit(&s->tail, flushed_head, memory_order_release);
    }

    uint32_t tail = atomic_load_explicit(&s->tail, memory_order_relaxed);
    uint32_t head = atomic_load_explicit(&s->head, memory_order_acquire);
    size_t available = (size_t)(head - tail);
    size_t take = (count < available) ? count : available;

    if (take) {
        size_t index = (size_t)(tail % GP32_IOS_AUDIO_RING_FRAMES);
        size_t first = take;
        if (index + take > GP32_IOS_AUDIO_RING_FRAMES) first = GP32_IOS_AUDIO_RING_FRAMES - index;
        memcpy(dst, &s->samples[index * 2u], first * 2u * sizeof(int16_t));
        if (take > first)
            memcpy(dst + first * 2u, &s->samples[0], (take - first) * 2u * sizeof(int16_t));
        atomic_store_explicit(&s->tail, tail + (uint32_t)take, memory_order_release);
    }
    if (take < count) {
        memset(dst + take * 2u, 0, (count - take) * 2u * sizeof(int16_t));
        atomic_fetch_add_explicit(&s->underrun_events, 1u, memory_order_relaxed);
    }
    return take;
}

/* Resample one borrowed guest span into the bounded sink. The span is consumed
 * by the caller only after this returns, so a refused write keeps the samples
 * for the next call. Split into fixed chunks: the resampler keeps its phase and
 * kernel history across calls, so the split is not audible as a seam. */
static void push_audio_span(gp32_ios_emulator_t *e, const gp32_audio_desc_t *aud) {
    if (!e->sink || !aud->samples_s16_interleaved || !aud->frame_count) return;
    uint32_t src_rate = aud->sample_rate_hz ? aud->sample_rate_hz : GP32_IOS_AUDIO_RATE;
    size_t chunk_cap = (size_t)(GP32_IOS_AUDIO_SCRATCH_FRAMES - 4u) * src_rate / GP32_IOS_AUDIO_RATE;
    if (chunk_cap > GP32_IOS_AUDIO_IN_CHUNK) chunk_cap = GP32_IOS_AUDIO_IN_CHUNK;
    if (!chunk_cap) chunk_cap = 1u;

    size_t total = (size_t)aud->frame_count;
    for (size_t offset = 0; offset < total;) {
        size_t chunk = total - offset;
        if (chunk > chunk_cap) chunk = chunk_cap;
        /* Observe starvation before generating the next output, not after
         * draining a whole host frame. Otherwise the first recovered span
         * steps away from silence and only the following frame gets a ramp. */
        uint32_t underruns = atomic_load_explicit(&e->sink->underrun_events, memory_order_relaxed);
        if (underruns != e->last_underruns) {
            e->last_underruns = underruns;
            gp32_audio_resampler_mark_gap_from_silence(&e->resampler, GP32_IOS_AUDIO_RATE);
        }
        size_t got = gp32_audio_resampler_process(&e->resampler,
                                                  aud->samples_s16_interleaved + offset * 2u,
                                                  chunk, src_rate, GP32_IOS_AUDIO_RATE, 0,
                                                  e->scratch, GP32_IOS_AUDIO_SCRATCH_FRAMES);
        if (got) {
            int dropped = 0;
            gp32_ios_audio_sink_write(e->sink, e->scratch, got, &dropped);
            /* Dropping the newest frames breaks continuity after the retained
             * queue: ramp the next output instead of stepping to it. */
            if (dropped) gp32_audio_resampler_mark_gap(&e->resampler, GP32_IOS_AUDIO_RATE);
        }
        offset += chunk;
    }
}

/* Release every queued guest PCM span. The core hands out one uniform-rate
 * span at a time; each is consumed only after its output is retained. */
static void drain_guest_audio(gp32_ios_emulator_t *e) {
    if (!e || !e->core) return;
    gp32_audio_desc_t aud;
    if (gp32_get_audio(e->core, &aud) != GP32_OK) return;
    while (aud.frame_count) {
        push_audio_span(e, &aud);
        if (gp32_consume_audio(e->core, aud.frame_count) != GP32_OK) return;
        if (gp32_get_audio(e->core, &aud) != GP32_OK) return;
    }
}

/* gp32_set_host_pump hook: runs between guest run slices, so a frame that
 * overruns the display refresh still hands already-produced PCM to the ring
 * while it executes instead of only at the frame boundary. */
static void ios_audio_pump(gp32_t *core, void *user) {
    (void)core;
    drain_guest_audio((gp32_ios_emulator_t *)user);
}

/* ---- Framebuffer staging ---- */

static uint32_t force_xrgb(uint32_t p) {
    return 0xff000000u | (p & 0x00ffffffu);
}

/* Present the core's LCD memory as a fixed 320x240 landscape RGBA8888 frame.
 * Mirrors the libretro presenter: direct copy for the landscape panel, a
 * 90-degree counter-clockwise rotation for the raw 240x320 portrait memory the
 * S3C2400 programs, and an integer scale/crop fallback for unusual settings. */
static int stage_frame(const gp32_framebuffer_desc_t *fb, uint32_t *dst) {
    if (!fb || !dst || !fb->pixels_rgba8888 || !fb->width || !fb->height ||
        fb->stride_pixels < fb->width) return 0;
    const uint32_t *src = fb->pixels_rgba8888;
    size_t stride = fb->stride_pixels;

    if (fb->width == GP32_IOS_FRAME_WIDTH && fb->height == GP32_IOS_FRAME_HEIGHT) {
        for (uint32_t y = 0; y < GP32_IOS_FRAME_HEIGHT; ++y) {
            const uint32_t *row = src + (size_t)y * stride;
            uint32_t *out = dst + (size_t)y * GP32_IOS_FRAME_WIDTH;
            for (uint32_t x = 0; x < GP32_IOS_FRAME_WIDTH; ++x) out[x] = force_xrgb(row[x]);
        }
        return 1;
    }

    if (fb->width == GP32_IOS_FRAME_HEIGHT && fb->height == GP32_IOS_FRAME_WIDTH) {
        /* Cache-friendly tiling: a full output row reads 320 different source
         * rows, so reuse those lines in 8x8 blocks before advancing. */
        for (uint32_t by = 0; by < GP32_IOS_FRAME_HEIGHT; by += 8u) {
            for (uint32_t bx = 0; bx < GP32_IOS_FRAME_WIDTH; bx += 8u) {
                for (uint32_t y = by; y < by + 8u; ++y) {
                    uint32_t sx = GP32_IOS_FRAME_HEIGHT - 1u - y;
                    uint32_t *out = dst + (size_t)y * GP32_IOS_FRAME_WIDTH;
                    for (uint32_t x = bx; x < bx + 8u; ++x)
                        out[x] = force_xrgb(src[(size_t)x * stride + sx]);
                }
            }
        }
        return 1;
    }

    for (uint32_t y = 0; y < GP32_IOS_FRAME_HEIGHT; ++y) {
        uint32_t sy = (uint32_t)(((uint64_t)y * fb->height) / GP32_IOS_FRAME_HEIGHT);
        if (sy >= fb->height) sy = fb->height - 1u;
        const uint32_t *row = src + (size_t)sy * stride;
        uint32_t *out = dst + (size_t)y * GP32_IOS_FRAME_WIDTH;
        for (uint32_t x = 0; x < GP32_IOS_FRAME_WIDTH; ++x) {
            uint32_t sx = (uint32_t)(((uint64_t)x * fb->width) / GP32_IOS_FRAME_WIDTH);
            if (sx >= fb->width) sx = fb->width - 1u;
            out[x] = force_xrgb(row[sx]);
        }
    }
    return 1;
}

/* ---- Lifecycle ---- */

gp32_ios_emulator_t *gp32_ios_emulator_create(void) {
    gp32_ios_emulator_t *e = (gp32_ios_emulator_t *)calloc(1, sizeof(*e));
    if (!e) return NULL;
    gp32_audio_resampler_init(&e->resampler);
    return e;
}

static int release_core(gp32_ios_emulator_t *e, int persist) {
    if (!e || !e->core) return 1;
    if (persist && e->card_save_path[0] &&
        gp32_save_card_progress(e->core, e->card_save_path) != GP32_OK) {
        char message[384];
        snprintf(message, sizeof(message), "gp32emu ios: card progress save failed: %s", gp32_get_error(e->core));
        log_line(e, message);
        set_core_error(e);
        return 0;
    }
    gp32_clear_audio(e->core);
    gp32_destroy(e->core);
    e->core = NULL;
    e->have_game = 0;
    e->restored_card_progress = 0;
    e->card_save_reported = 0;
    e->buttons = 0;
    e->frame_counter = 0;
    return 1;
}

int gp32_ios_emulator_unload(gp32_ios_emulator_t *e) {
    if (!e || !release_core(e, 1)) return 0;
    if (e->sink) gp32_ios_audio_sink_request_flush(e->sink);
    gp32_audio_resampler_mark_gap_from_silence(&e->resampler, GP32_IOS_AUDIO_RATE);
    memset(e->staged, 0, sizeof(e->staged));
    e->card_save_path[0] = '\0';
    e->error[0] = '\0';
    return 1;
}

void gp32_ios_emulator_destroy(gp32_ios_emulator_t *e) {
    if (!e) return;
    /* Final object destruction cannot retain a retryable session. */
    if (!release_core(e, 1)) release_core(e, 0);
    free(e);
}

int gp32_ios_emulator_has_game(const gp32_ios_emulator_t *e) {
    return e && e->core && e->have_game;
}

const char *gp32_ios_emulator_error(const gp32_ios_emulator_t *e) {
    return e ? e->error : "no emulator";
}

/* Restore a persisted card that supersedes the immutable imported image. A
 * present but unreadable, damaged or mismatched save aborts the load instead of
 * silently replacing the player's progress with a fresh card. */
static int restore_card_progress(gp32_ios_emulator_t *e) {
    if (!e->card_save_path[0]) return 0;
    FILE *f = fopen(e->card_save_path, "rb");
    if (!f) {
        int saved_errno = errno;
        if (saved_errno == ENOENT) {
            /* Nothing to restore yet: the first session creates the file on its
             * first save. */
            return 0;
        }
        /* The progress file exists but cannot be opened: report it and abort the
         * load instead of booting a fresh card that would replace it. */
        char message[384];
        snprintf(message, sizeof(message),
                 "gp32emu ios: cannot open card progress (%s), load aborted: %s",
                 strerror(saved_errno), e->card_save_path);
        log_line(e, message);
        set_error(e, "card save exists but cannot be opened; load aborted to protect progress");
        return -1;
    }
    fclose(f);
    if (gp32_load_card_progress(e->core, e->card_save_path) != GP32_OK) {
        char message[384];
        snprintf(message, sizeof(message),
                 "gp32emu ios: cannot restore card progress, load aborted: %s",
                 gp32_get_error(e->core));
        log_line(e, message);
        set_error(e, "card save is unreadable or does not match this card; load aborted to protect progress");
        return -1;
    }
    e->restored_card_progress = 1;
    return 1;
}

static int load_standalone(gp32_ios_emulator_t *e, ios_ext_t ext) {
    if (ext == IOS_EXT_FXE) return gp32_load_fxe(e->core, e->game_path) == GP32_OK;
    return gp32_load_fpk(e->core, e->game_path) == GP32_OK;
}

/* Returns 1 loaded, 0 failed without touching persisted progress, -1 aborted
 * because restoring progress failed. */
static int load_smartmedia(gp32_ios_emulator_t *e, ios_boot_t boot) {
    const char *label = e->game_label[0] ? e->game_label : path_basename(e->game_path);
    if (boot == IOS_BOOT_DIRECT) {
        /* Extracted boot without a card slot: this shape has no writable NAND
         * to persist, so the card save path is not used for the session. */
        e->card_save_path[0] = '\0';
        return gp32_load_smartmedia_direct(e->core, e->game_path) == GP32_OK;
    }
    if (gp32_load_smartmedia(e->core, e->game_path) != GP32_OK) return 0;
    int restored = restore_card_progress(e);
    if (restored < 0) return -1;
    if (boot == IOS_BOOT_DIRECT_MEDIA)
        return gp32_boot_mounted_smartmedia(e->core, label) == GP32_OK;
    return 1;
}

static gp32_t *create_core(gp32_ios_emulator_t *e, const char *bios_path) {
    gp32_options_t opt;
    memset(&opt, 0, sizeof(opt));
    opt.bios_path = (bios_path && bios_path[0]) ? bios_path : NULL;
    /* Content is mounted explicitly below, so the card slot starts empty. */
    opt.smartmedia_path = NULL;
    opt.log = e->log;
    opt.log_user = e->log_user;
    return gp32_create(&opt);
}

int gp32_ios_emulator_load(gp32_ios_emulator_t *e, const gp32_ios_config_t *cfg) {
    if (!e) return 0;
    if (!cfg || !cfg->game_path || !cfg->game_path[0]) {
        set_error(e, "no game image selected");
        return 0;
    }
    if (e->core) {
        set_error(e, "content is already loaded");
        return 0;
    }
    e->error[0] = '\0';
    e->sink = cfg->audio_sink;
    e->log = cfg->log;
    e->log_user = cfg->log_user;
    e->have_game = 0;
    e->restored_card_progress = 0;
    e->card_save_reported = 0;
    e->buttons = 0;
    e->frame_counter = 0;
    memset(e->staged, 0, sizeof(e->staged));
    gp32_audio_resampler_mark_gap_from_silence(&e->resampler, GP32_IOS_AUDIO_RATE);
    if (e->sink) {
        /* Never reset shared indices here: the audio callback may be running.
         * The consumer applies this discard at its next read. */
        gp32_ios_audio_sink_request_flush(e->sink);
        e->last_underruns = atomic_load_explicit(&e->sink->underrun_events, memory_order_relaxed);
    }

    if (!copy_text(e->bios_path, sizeof(e->bios_path), cfg->bios_path) ||
        !copy_text(e->game_path, sizeof(e->game_path), cfg->game_path) ||
        !copy_text(e->card_save_path, sizeof(e->card_save_path), cfg->card_save_path) ||
        !copy_text(e->state_path, sizeof(e->state_path), cfg->state_path) ||
        !copy_text(e->game_label, sizeof(e->game_label), cfg->game_label)) {
        set_error(e, "path is too long for this build");
        return 0;
    }
    e->boot_mode = cfg->boot_mode;

    ios_ext_t ext = IOS_EXT_SMC;
    if (ext_is(e->game_path, ".fxe")) ext = IOS_EXT_FXE;
    else if (ext_is(e->game_path, ".fpk")) ext = IOS_EXT_FPK;

    int have_bios = e->bios_path[0] && path_exists(e->bios_path);
    if (e->bios_path[0] && !have_bios)
        log_line(e, "gp32emu ios: the imported BIOS image could not be read; using the host-side boot path");

    /* The retail launcher boots a card only from its top-level GAME\ folder. A
     * card whose executable lives elsewhere (the freeware GPMM\ layout) hangs
     * on the firmware's card-scan error path, so it needs the host-side direct
     * boot with the card still mounted: the game reads its own assets through
     * the card slot and only runs when it is present. */
    smc_card_launch_layout_t layout = SMC_CARD_LAYOUT_NONE;
    char layout_exe[260] = {0};
    if (ext == IOS_EXT_SMC && e->boot_mode != GP32_IOS_BOOT_BIOS) {
        char layout_err[192] = {0};
        layout = smc_direct_classify_file(e->game_path, layout_exe, sizeof(layout_exe),
                                         layout_err, sizeof(layout_err));
        if (layout == SMC_CARD_LAYOUT_DIRECT_ONLY)
            log_line(e, "gp32emu ios: freeware card layout, using the direct boot with the card mounted");
        else if (layout == SMC_CARD_LAYOUT_NONE)
            log_line(e, "gp32emu ios: no launcher-compatible executable found; keeping the configured boot path");
    }

    int boot;
    if (ext != IOS_EXT_SMC) {
        boot = (have_bios && e->boot_mode != GP32_IOS_BOOT_DIRECT) ? IOS_BOOT_BIOS : IOS_BOOT_DIRECT;
    } else if (e->boot_mode == GP32_IOS_BOOT_BIOS) {
        if (!have_bios) {
            set_error(e, "a BIOS boot was requested but the BIOS image is missing or unreadable");
            return 0;
        }
        boot = IOS_BOOT_BIOS;
    } else if (e->boot_mode == GP32_IOS_BOOT_DIRECT) {
        boot = (layout == SMC_CARD_LAYOUT_DIRECT_ONLY) ? IOS_BOOT_DIRECT_MEDIA : IOS_BOOT_DIRECT;
    } else if (layout == SMC_CARD_LAYOUT_DIRECT_ONLY) {
        boot = IOS_BOOT_DIRECT_MEDIA;
    } else {
        boot = have_bios ? IOS_BOOT_BIOS : IOS_BOOT_DIRECT;
    }

    for (;;) {
        e->core = create_core(e, boot == IOS_BOOT_BIOS ? e->bios_path : NULL);
        if (!e->core) {
            set_error(e, "the emulator core could not be created (out of memory or unreadable BIOS)");
            return 0;
        }

        int loaded = (ext == IOS_EXT_SMC) ? load_smartmedia(e, boot) : load_standalone(e, ext);
        if (loaded > 0) {
            /* Start safely with the interpreter, including developer builds.
             * JIT activation requires a separate owner-queue handshake. */
            gp32_set_jit(e->core, 0);
            gp32_set_cpu_speed_percent(e->core, 100);
            gp32_set_game_fixes(e->core, 1);
            gp32_set_fast_loading(e->core, 1);
            gp32_set_host_pump(e->core, ios_audio_pump, e);
            e->have_game = 1;
            return 1;
        }

        int allowed_fallback = (loaded == 0) && ext == IOS_EXT_SMC &&
                                boot == IOS_BOOT_BIOS && e->boot_mode != GP32_IOS_BOOT_BIOS;
        if (loaded < 0) {
            /* Progress protection fired; the error already names the reason. */
        } else if (allowed_fallback) {
            log_line(e, "gp32emu ios: the BIOS boot did not start this card, retrying the host-side boot path");
        } else {
            set_core_error(e);
        }
        release_core(e, 0);
        if (!allowed_fallback) return 0;
        boot = (layout == SMC_CARD_LAYOUT_DIRECT_ONLY) ? IOS_BOOT_DIRECT_MEDIA : IOS_BOOT_DIRECT;
    }
}

/* ---- Execution ---- */

int gp32_ios_emulator_run_frame(gp32_ios_emulator_t *e) {
    if (!e || !e->core || !e->have_game) return 0;
    if (gp32_run_frame(e->core) != GP32_OK) {
        set_core_error(e);
        return 0;
    }

    gp32_framebuffer_desc_t fb;
    if (gp32_get_framebuffer(e->core, &fb) == GP32_OK) {
        stage_frame(&fb, e->staged);
        e->frame_counter = fb.frame_counter;
    }
    drain_guest_audio(e);

    /* One poll per host frame keeps automatic card saves bounded and lets the
     * core report a failed save once until it recovers. */
    if (e->card_save_path[0]) {
        if (gp32_poll_card_progress(e->core, e->card_save_path) != GP32_OK) {
            char message[384];
            snprintf(message, sizeof(message), "gp32emu ios: automatic card save failed: %s",
                     gp32_get_error(e->core));
            log_line(e, message);
            if (!e->card_save_reported) {
                e->card_save_reported = 1;
                set_error(e, "automatic card save failed; check free space and try a manual save");
            }
        } else {
            e->card_save_reported = 0;
        }
    }
    return 1;
}

void gp32_ios_emulator_set_buttons(gp32_ios_emulator_t *e, uint32_t button_mask) {
    if (!e || !e->core) return;
    e->buttons = button_mask;
    gp32_set_buttons(e->core, button_mask);
}

void gp32_ios_emulator_release_input(gp32_ios_emulator_t *e) {
    /* Pause and background transitions use this so a key or touch that is not
     * followed by an up event can never leave a guest button held down. */
    gp32_ios_emulator_set_buttons(e, 0u);
}

int gp32_ios_emulator_prepare_jit(gp32_ios_emulator_t *e, gp32_ios_jit_region_t *region) {
    return gp32_prepare_ios_jit(e ? e->core : NULL, region);
}

int gp32_ios_emulator_activate_jit(gp32_ios_emulator_t *e) {
    return gp32_activate_ios_jit(e ? e->core : NULL);
}
int gp32_ios_emulator_jit_enabled(const gp32_ios_emulator_t *e) {
    return gp32_get_jit_enabled(e ? e->core : NULL);
}
int gp32_ios_emulator_activate_jit_universal(gp32_ios_emulator_t *e) {
    return gp32_activate_ios_jit_universal(e ? e->core : NULL);
}

int gp32_ios_emulator_reset(gp32_ios_emulator_t *e) {
    if (!e || !e->core) return 0;
    if (gp32_reset(e->core) != GP32_OK) {
        set_core_error(e);
        return 0;
    }
    /* The core preserves host-owned buttons across a hardware reset. Release
     * them through its input API as well as the frontend's cached mask. */
    gp32_ios_emulator_release_input(e);
    gp32_ios_emulator_clear_audio(e);
    e->error[0] = '\0';
    return 1;
}

const uint32_t *gp32_ios_emulator_frame_pixels(const gp32_ios_emulator_t *e) {
    return e ? e->staged : NULL;
}

uint64_t gp32_ios_emulator_frame_counter(const gp32_ios_emulator_t *e) {
    return e ? e->frame_counter : 0u;
}

/* ---- Persistence ---- */

int gp32_ios_emulator_save_card(gp32_ios_emulator_t *e) {
    if (!e || !e->core || !e->card_save_path[0]) return 0;
    if (gp32_save_card_progress(e->core, e->card_save_path) != GP32_OK) {
        set_core_error(e);
        return 0;
    }
    e->error[0] = '\0';
    return 1;
}

int gp32_ios_emulator_save_state(gp32_ios_emulator_t *e) {
    if (!e || !e->core || !e->state_path[0]) return 0;
    if (gp32_save_state(e->core, e->state_path) != GP32_OK) {
        set_core_error(e);
        return 0;
    }
    e->error[0] = '\0';
    return 1;
}

/* The caller's string is validated in bridge-owned storage before any core
 * call. In particular, a bad explicit path cannot overwrite e->state_path or
 * leave the current game backed by a half-applied state load. */
static int explicit_state_path(gp32_ios_emulator_t *e, const char *path,
                               char target[GP32_IOS_PATH_MAX]) {
    if (!e || !e->core) return 0;
    if (!copy_text(target, GP32_IOS_PATH_MAX, path)) {
        set_error(e, "save-state path is too long for this build");
        return 0;
    }
    if (!target[0]) {
        set_error(e, "save-state path is unavailable");
        return 0;
    }
    return 1;
}

int gp32_ios_emulator_save_state_at_path(gp32_ios_emulator_t *e, const char *path) {
    char target[GP32_IOS_PATH_MAX];
    if (!explicit_state_path(e, path, target)) return 0;
    if (gp32_save_state(e->core, target) != GP32_OK) {
        set_core_error(e);
        return 0;
    }
    e->error[0] = '\0';
    return 1;
}

int gp32_ios_emulator_load_state(gp32_ios_emulator_t *e) {
    if (!e || !e->core || !e->state_path[0]) return 0;
    if (gp32_load_state(e->core, e->state_path) != GP32_OK) {
        set_core_error(e);
        return 0;
    }
    gp32_ios_emulator_clear_audio(e);
    e->error[0] = '\0';
    return 1;
}

int gp32_ios_emulator_load_state_at_path(gp32_ios_emulator_t *e, const char *path) {
    char target[GP32_IOS_PATH_MAX];
    if (!explicit_state_path(e, path, target)) return 0;
    if (gp32_load_state(e->core, target) != GP32_OK) {
        set_core_error(e);
        return 0;
    }
    gp32_ios_emulator_clear_audio(e);
    e->error[0] = '\0';
    return 1;
}

size_t gp32_ios_emulator_state_size(const gp32_ios_emulator_t *e) {
    if (!e || !e->core) return 0;
    return gp32_state_size(e->core);
}

void gp32_ios_emulator_clear_audio(gp32_ios_emulator_t *e) {
    if (!e || !e->core) return;
    gp32_clear_audio(e->core);
    /* The consumer discards queued audio and outputs silence. The new stream
     * must start from that level, not a sample from before the transition. */
    gp32_audio_resampler_mark_gap_from_silence(&e->resampler, GP32_IOS_AUDIO_RATE);
    if (e->sink) gp32_ios_audio_sink_request_flush(e->sink);
}
