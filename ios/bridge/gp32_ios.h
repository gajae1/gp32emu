/*
 * gp32emu iOS bridge: one emulator instance, one owning thread.
 *
 * This file is plain C11 with no Apple framework dependency, so a host
 * without an iOS SDK can still syntax-check it (see ios/README.md). It is the
 * only place the UIKit frontend touches the core: all gp32_* calls happen on
 * the single owner queue the app owns (ios/app/GP32Emulator.m), never on the
 * main thread and never on the audio render thread.
 *
 * Threading contract
 *   - Every function takes gp32_ios_emulator_t * and is owner-queue-only,
 *     except gp32_ios_audio_sink_read(), which the real-time audio callback
 *     calls. The sink is a fixed-size single-producer/single-consumer ring
 *     with atomic indices and no allocation, lock or core call.
 *   - gp32_ios_emulator_frame_pixels() stays valid until the next
 *     gp32_ios_emulator_run_frame() call, so a frontend copies or uploads it
 *     before starting the next frame.
 *
 * GP32/S3C2400 model: BSD-3-Clause, derived from MAME's Game Park GP32 driver.
 * See licenses/ and ios/CREDITS.md. No BIOS or game image is part of this
 * source or of a built bundle.
 */

#ifndef GP32EMU_IOS_BRIDGE_H
#define GP32EMU_IOS_BRIDGE_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "gp32emu/gp32.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Native landscape LCD. The core also exposes the raw 240x320 portrait
 * framebuffer; the bridge rotates that to this fixed size. */
#define GP32_IOS_FRAME_WIDTH 320u
#define GP32_IOS_FRAME_HEIGHT 240u
#define GP32_IOS_FRAME_PIXELS (GP32_IOS_FRAME_WIDTH * GP32_IOS_FRAME_HEIGHT)

/* Audio delivery rate. The source rate follows the guest IIS programming. */
#define GP32_IOS_AUDIO_RATE 44100u

/* Bounded stereo ring: 4096 frames is about 93 ms at 44100 Hz. The producer
 * never moves the consumer's tail: when the ring is full the newest frames are
 * dropped, so a stalled consumer costs audio continuity, never memory and
 * never a producer/consumer race on the same index. */
#define GP32_IOS_AUDIO_RING_FRAMES 4096u

#define GP32_IOS_PATH_MAX 1024u

/* Single-producer/single-consumer interleaved stereo ring. Owned by the app
 * (embedded in GP32Audio) and outlives every audio callback that can read it.
 * head is written only by the producer (owner queue), tail only by the
 * consumer (audio thread), and the two share no other mutable index. */
typedef struct gp32_ios_audio_sink {
    int16_t samples[GP32_IOS_AUDIO_RING_FRAMES * 2u];
    _Atomic uint32_t head;                /* producer frames written: owner queue */
    _Atomic uint32_t tail;                /* consumer frames played: audio thread */
    _Atomic uint32_t flush_request;       /* owner queue sets, audio thread applies */
    _Atomic uint32_t underrun_events;     /* audio thread: ring ran dry */
    _Atomic uint32_t dropped_frames;      /* owner queue: new frames that did not fit */
    _Atomic uint32_t layout_mismatch_events; /* audio thread: unexpected buffer layout */
} gp32_ios_audio_sink_t;

typedef struct gp32_ios_emulator gp32_ios_emulator_t;

typedef enum gp32_ios_boot_mode {
    /* BIOS when a BIOS image is set and the card is not a freeware layout,
     * otherwise the host-side direct SmartMedia boot. */
    GP32_IOS_BOOT_AUTO = 0,
    /* Retail firmware boots and the user starts the game from its menu. */
    GP32_IOS_BOOT_BIOS = 1,
    /* Host-side direct boot: the executable is extracted from the card while
     * the card itself stays mounted for the game's own asset reads. */
    GP32_IOS_BOOT_DIRECT = 2
} gp32_ios_boot_mode_t;

typedef struct gp32_ios_config {
    const char *bios_path;       /* optional imported BIOS image */
    const char *game_path;       /* required imported card/.fxe/.fpk copy */
    const char *game_label;      /* optional display name for the direct boot */
    const char *card_save_path;  /* optional persisted card-progress file */
    const char *state_path;      /* optional save-state file */
    gp32_ios_audio_sink_t *audio_sink; /* optional; NULL discards guest audio */
    gp32_ios_boot_mode_t boot_mode;
    gp32_log_fn log;             /* optional core diagnostics */
    void *log_user;
} gp32_ios_config_t;

/* ---- Lifecycle (owner queue) ---- */

gp32_ios_emulator_t *gp32_ios_emulator_create(void);
/* Persists pending card changes, then releases the core. On save failure,
 * returns 0 and preserves the entire live session for retry; error() explains. */
int gp32_ios_emulator_unload(gp32_ios_emulator_t *e);
void gp32_ios_emulator_destroy(gp32_ios_emulator_t *e);

/* Loads content into a released (or fresh) emulator. Returns 1 on success.
 * An existing but unreadable or mismatched card-progress file aborts the load
 * so a save is never silently replaced by a fresh card. */
int gp32_ios_emulator_load(gp32_ios_emulator_t *e, const gp32_ios_config_t *cfg);
int gp32_ios_emulator_has_game(const gp32_ios_emulator_t *e);
const char *gp32_ios_emulator_error(const gp32_ios_emulator_t *e);

/* ---- Execution (owner queue) ---- */

/* One 60 Hz emulated interval. Returns 1 when the interval completed. */
int gp32_ios_emulator_run_frame(gp32_ios_emulator_t *e);
void gp32_ios_emulator_set_buttons(gp32_ios_emulator_t *e, uint32_t button_mask);
void gp32_ios_emulator_release_input(gp32_ios_emulator_t *e);
/* Successful reset releases input and discards host audio from before reset. */
int gp32_ios_emulator_reset(gp32_ios_emulator_t *e);
/* Experimental developer JIT handshake; retains interpreter on failure. */
int gp32_ios_emulator_prepare_jit(gp32_ios_emulator_t *e, gp32_ios_jit_region_t *region);
int gp32_ios_emulator_activate_jit(gp32_ios_emulator_t *e);
int gp32_ios_emulator_jit_enabled(const gp32_ios_emulator_t *e);
int gp32_ios_emulator_activate_jit_universal(gp32_ios_emulator_t *e);

/* Framebuffer, rotated to 320x240 landscape RGBA8888 (0xAARRGGBB words, so the
 * bytes CoreGraphics reads as 32-bit little-endian BGRA). */
const uint32_t *gp32_ios_emulator_frame_pixels(const gp32_ios_emulator_t *e);
uint64_t gp32_ios_emulator_frame_counter(const gp32_ios_emulator_t *e);

/* ---- Persistence (owner queue) ---- */

int gp32_ios_emulator_save_card(gp32_ios_emulator_t *e);
int gp32_ios_emulator_save_state(gp32_ios_emulator_t *e);
/* Successful restore also discards the previous host audio stream. */
int gp32_ios_emulator_load_state(gp32_ios_emulator_t *e);
/* Explicit-path variants validate the path before entering the core and never
 * modify the configured state path. A failed load retains the core's existing
 * read-before-commit behavior; a failed save leaves the live game untouched. */
int gp32_ios_emulator_save_state_at_path(gp32_ios_emulator_t *e, const char *path);
int gp32_ios_emulator_load_state_at_path(gp32_ios_emulator_t *e, const char *path);
size_t gp32_ios_emulator_state_size(const gp32_ios_emulator_t *e);
void gp32_ios_emulator_clear_audio(gp32_ios_emulator_t *e);

/* ---- Audio sink (init/request_flush/write: owner queue; read: audio thread) ---- */

/* Zeroes the whole ring and both indices. Only legal while the consumer cannot
 * run (construction, or after the engine has been torn down): a live callback
 * must never have its tail reset underneath it. */
void gp32_ios_audio_sink_init(gp32_ios_audio_sink_t *s);

/* Producer-side only. Asks the consumer to discard everything queued up to the
 * head it observes at its next read. The consumer owns tail, so this is safe
 * while a render callback runs; content (re)load, state restore and audio
 * resume use it so no audio from before the break can be played. */
void gp32_ios_audio_sink_request_flush(gp32_ios_audio_sink_t *s);

/* Always accepts, never blocks and never touches tail: if the ring is full the
 * newest frames are dropped and counted, reported through dropped (optional). */
void gp32_ios_audio_sink_write(gp32_ios_audio_sink_t *s, const int16_t *frames, size_t count, int *dropped);
/* Fills count interleaved stereo frames, padding with silence on underrun.
 * Returns the frames that came from the ring. Lock-, allocation- and
 * core-call-free: safe for a real-time audio callback. */
size_t gp32_ios_audio_sink_read(gp32_ios_audio_sink_t *s, int16_t *dst, size_t count);

#ifdef __cplusplus
}
#endif

#endif /* GP32EMU_IOS_BRIDGE_H */
