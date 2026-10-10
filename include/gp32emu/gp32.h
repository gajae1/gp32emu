/*
 * gp32emu - standalone headless GP32 emulator front-end API
 * C11, no external runtime dependencies.
 *
 * The GP32/S3C2400 hardware model is derived from MAME's Game Park GP32
 * driver by Tim Schuerewegen (BSD-3-Clause). See licenses/.
 */
#ifndef GP32EMU_GP32_H
#define GP32EMU_GP32_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gp32 gp32_t;
typedef struct gp32_framebuffer gp32_framebuffer_t;


typedef enum gp32_status {
    GP32_OK = 0,
    GP32_ERR_INVALID_ARGUMENT = -1,
    GP32_ERR_NO_MEMORY = -2,
    GP32_ERR_IO = -3,
    GP32_ERR_BAD_IMAGE = -4,
    GP32_ERR_CPU_FAULT = -5
} gp32_status_t;

typedef enum gp32_button {
    GP32_BUTTON_A      = 1u << 0,
    GP32_BUTTON_B      = 1u << 1,
    GP32_BUTTON_L      = 1u << 2,
    GP32_BUTTON_R      = 1u << 3,
    GP32_BUTTON_START  = 1u << 4,
    GP32_BUTTON_SELECT = 1u << 5,
    GP32_BUTTON_UP     = 1u << 6,
    GP32_BUTTON_DOWN   = 1u << 7,
    GP32_BUTTON_LEFT   = 1u << 8,
    GP32_BUTTON_RIGHT  = 1u << 9
} gp32_button_t;

typedef void (*gp32_log_fn)(void *user, const char *message);
/* Opt-in diagnostic sink for exception vector entries a compatibility sweep
 * needs (undefined instruction, aborts). Independent of the trace callback,
 * so it can be enabled without a per-instruction trace; a NULL sink keeps the
 * default behaviour identical. Install with gp32_set_diag_log after create. */
typedef void (*gp32_diag_fn)(void *user, const char *message);

/* Optional host hook invoked between guest run slices while a frame is being
 * executed (gp32_run_frame and gp32_run_cycles). A frontend installs one to
 * hand already-produced PCM to its own audio sink before the frame boundary:
 * a guest frame that takes longer than real time then keeps the frontend FIFO
 * fed while it runs instead of leaving it dry until the whole frame retires.
 * The hook only inspects host-side data (for example gp32_get_audio /
 * gp32_consume_audio); it must not re-enter the emulator or read guest state.
 * A NULL hook (the default) leaves execution unchanged. */
typedef void (*gp32_host_pump_fn)(gp32_t *gp32, void *user);

typedef struct gp32_options {
    const char *bios_path;         /* optional; can be loaded later */
    const char *smartmedia_path;   /* optional; can be loaded later */
    size_t ram_size;               /* 0 => GP32 default 8 MiB */
    int enable_trace;              /* non-zero => CPU trace to log callback */
    gp32_log_fn log;
    void *log_user;
} gp32_options_t;

typedef struct gp32_framebuffer_desc {
    const uint32_t *pixels_rgba8888;
    uint32_t width;
    uint32_t height;
    uint32_t stride_pixels;
    uint64_t frame_counter;
} gp32_framebuffer_desc_t;

typedef struct gp32_audio_desc {
    const int16_t *samples_s16_interleaved; /* stereo L,R,L,R at sample_rate_hz */
    uint64_t frame_count;                  /* stereo frames, not int16_t sample count */
    uint32_t sample_rate_hz;
} gp32_audio_desc_t;

gp32_t *gp32_create(const gp32_options_t *options);
void gp32_destroy(gp32_t *gp32);

gp32_status_t gp32_load_bios(gp32_t *gp32, const char *path);
gp32_status_t gp32_load_smartmedia(gp32_t *gp32, const char *path);
/* Buffer loaders are used by the WASM frontend and by hosts that do not expose a filesystem. */
gp32_status_t gp32_load_bios_data(gp32_t *gp32, const void *data, size_t size);
gp32_status_t gp32_load_smartmedia_data(gp32_t *gp32, const void *data, size_t size);
/* Savestate SmartMedia base and the persisted card that supersedes it.
 *
 * A state saved by this core stores the NAND pages that differ from the image
 * the frontend passes as content, not the whole card, so it is smaller but only
 * reconstructs over that same image. A host that mounts a persisted card image
 * over immutable content must therefore set the base from
 * the content and mount the persisted image with the over-base loaders: the
 * state base then stays the content, which every later session passes again,
 * and a state saved in one session loads in any later session of the same game.
 * Without a base, or once the card no longer shares its shape, states carry the
 * whole image (the pre-v0013 payload) and still load everywhere. */
gp32_status_t gp32_set_smartmedia_state_base(gp32_t *gp32, const void *data, size_t size);
gp32_status_t gp32_set_smartmedia_state_base_file(gp32_t *gp32, const char *path);
gp32_status_t gp32_load_smartmedia_over_base(gp32_t *gp32, const char *path);
gp32_status_t gp32_load_smartmedia_over_base_data(gp32_t *gp32, const void *data, size_t size);
/* BIOSless direct loader for retail SmartMedia images: extracts the first commercial/homebrew executable from FAT and installs file HLE for sibling assets. */
gp32_status_t gp32_load_smartmedia_direct(gp32_t *gp32, const char *path);
gp32_status_t gp32_load_smartmedia_direct_data(gp32_t *gp32, const void *data, size_t size, const char *label);
gp32_status_t gp32_load_fxe(gp32_t *gp32, const char *path);
gp32_status_t gp32_load_fxe_data(gp32_t *gp32, const void *data, size_t size, const char *label);
gp32_status_t gp32_load_fpk(gp32_t *gp32, const char *path);
gp32_status_t gp32_load_fpk_data(gp32_t *gp32, const void *data, size_t size, const char *label);
gp32_status_t gp32_save_smartmedia(gp32_t *gp32, const char *path);
/* Persistent page changes over the mounted original card. Legacy full card
 * saves are accepted on load. A delta must match the exact mounted base. */
gp32_status_t gp32_save_card_progress(gp32_t *gp32, const char *path);
/* Call once per host frame with a stable save path. Reports a failed automatic
 * save once until recovery. Explicit save/destroy waits for outstanding work. */
gp32_status_t gp32_poll_card_progress(gp32_t *gp32, const char *path);
gp32_status_t gp32_load_card_progress(gp32_t *gp32, const char *path);
gp32_status_t gp32_boot_mounted_smartmedia(gp32_t *gp32, const char *label);
gp32_status_t gp32_save_state(gp32_t *gp32, const char *path);
gp32_status_t gp32_load_state(gp32_t *gp32, const char *path);
/* Exact current payload size (including queued PCM), or 0 on failure.
 * Memory states use the same byte format as path states. A short save buffer
 * is rejected before writing; a larger buffer has its trailing bytes zeroed.
 * Loads accept trailing bytes and retain the path loader's error semantics. */
size_t gp32_state_size(const gp32_t *gp32);
gp32_status_t gp32_save_state_data(gp32_t *gp32, void *data, size_t size);
gp32_status_t gp32_load_state_data(gp32_t *gp32, const void *data, size_t size);
/* Same load, and reports the exact guest-section length at the front of the
 * buffer. Frontends that append their own section after the guest payload use
 * it to find where that section starts; trailing bytes stay tolerated. */
gp32_status_t gp32_load_state_data_ex(gp32_t *gp32, const void *data, size_t size, size_t *consumed);

gp32_status_t gp32_reset(gp32_t *gp32);
/* Explicit CPU-cycle budget shared by foreground and HLE callback execution,
 * independent of host frame pacing. Unfinished callbacks/display waits remain pending.
 * Callback watchdog/stall/unsupported-task faults return GP32_ERR_CPU_FAULT
 * until reset or a nonfault state load; gp32_get_error describes the fault. */
gp32_status_t gp32_run_cycles(gp32_t *gp32, uint32_t cycles);
/* Advance one 60 Hz interval of emulated time, including guest clock changes
 * and callback execution. A callback can remain pending across frames.
 * Pacing carry, callbacks and guest display waits are restored by savestates. */
gp32_status_t gp32_run_frame(gp32_t *gp32);
gp32_status_t gp32_set_jit(gp32_t *gp32, int enabled);
void gp32_set_diag_log(gp32_t *gp32, gp32_diag_fn fn, void *user);
/* Install the host pump; see gp32_host_pump_fn. Independent of savestates. */
void gp32_set_host_pump(gp32_t *gp32, gp32_host_pump_fn fn, void *user);
/* Optional guest CPU speed, 50..400 percent of the clock the game programs
 * (default 100). Only instruction throughput changes; audio pitch, timers and
 * LCD refresh keep real time. Savestates are portable between speeds. */
gp32_status_t gp32_set_cpu_speed_percent(gp32_t *gp32, uint32_t percent);
uint32_t gp32_get_cpu_speed_percent(const gp32_t *gp32);
/* Runtime game compatibility fixes (default on): Astonishia Story R's ADPCM
 * nibble order and Pinball Dreams' startup DAC mute. Card images are never
 * modified. Saved RAM retains each fix's current mode. */
gp32_status_t gp32_set_game_fixes(gp32_t *gp32, int enabled);
/* Shorten silent loading pauses (default on): while a game stops audio,
 * freezes the screen and reads the card in bulk, run its CPU at 4x speed.
 * Audio pitch, timers and LCD refresh keep real time. */
gp32_status_t gp32_set_fast_loading(gp32_t *gp32, int enabled);
/* Overrides HLE playback rate for raw SEF PCM. 0 restores SDK-derived auto rate. */
gp32_status_t gp32_set_hle_sef_rate(gp32_t *gp32, uint32_t sample_rate_hz);

gp32_status_t gp32_set_buttons(gp32_t *gp32, uint32_t gp32_button_mask);
gp32_status_t gp32_get_framebuffer(gp32_t *gp32, gp32_framebuffer_desc_t *out_desc);
/* Borrow the first uniform-rate span. Consume it before requesting the next.
 * Mutating the emulator invalidates borrowed pointers; clear discards all spans. */
gp32_status_t gp32_get_audio(gp32_t *gp32, gp32_audio_desc_t *out_desc);
gp32_status_t gp32_consume_audio(gp32_t *gp32, uint64_t frames);
gp32_status_t gp32_clear_audio(gp32_t *gp32);
/* Frame period of the live TFT panel programming as period_ns +
 * period_frac/2^20 ns, the same clock LINECNT and VSTATUS observe. Returns 0
 * when no panel frame clock can be derived (ENVID off, STN mode, zero
 * divider, or a period outside the 5..500 Hz sanity window), and the caller
 * keeps its previous timing. Frontends use it to advertise the rate the
 * hardware actually shows; it never changes emulated time. */
int gp32_get_lcd_frame_period(const gp32_t *gp32, uint32_t *period_ns, uint32_t *period_frac);

uint32_t gp32_get_pc(const gp32_t *gp32);
uint32_t gp32_get_cpu_reg(const gp32_t *gp32, unsigned reg);
uint32_t gp32_get_cpsr(const gp32_t *gp32);
uint32_t gp32_get_cp15(const gp32_t *gp32, unsigned reg);
uint32_t gp32_debug_read32(gp32_t *gp32, uint32_t addr);
uint64_t gp32_get_cycles(const gp32_t *gp32);
uint32_t gp32_get_fclk_hz(const gp32_t *gp32);
/* Effective instruction budget clock used by frontends for real-time pacing. */
uint32_t gp32_get_run_clock_hz(const gp32_t *gp32);
uint64_t gp32_get_jit_hits(const gp32_t *gp32);
uint64_t gp32_get_jit_misses(const gp32_t *gp32);
uint64_t gp32_get_jit_fallbacks(const gp32_t *gp32);

/*
 * Optional CPU workload profile (GP32EMU_CPU_PROFILE build, off by default).
 * Counters attribute guest work to the interpreter, cached-block bytecode
 * loop and native JIT paths, and record JIT compile/invalidation pressure.
 * supported==0 when the core was built without profiling; all counters then
 * read zero. Profiling state is transient and never enters saved states.
 */
#define GP32_CPU_PROFILE_OP_KINDS 12u /* interp,data,psr,mul,swp,half,single_dt,block_dt,branch,swi,coproc,undefined */
#define GP32_CPU_PROFILE_MEMORY_SLOTS 64u
typedef struct gp32_memory_profile {
    uint32_t physical_address;
    uint32_t first_pc;
    uint64_t reads, writes;
} gp32_memory_profile_t;

typedef struct gp32_cpu_profile {
    uint32_t supported;          /* 1 when built with GP32EMU_CPU_PROFILE */
    uint32_t native_backend;     /* 0 none, 1 x64, 2 aarch64 */
    /* Interpreted guest instruction work by executor path. */
    uint64_t interp_arm_insns;       /* ARM insns single-stepped by exec_arm */
    uint64_t interp_thumb_insns;     /* Thumb insns single-stepped by exec_thumb */
    uint64_t block_interp_arm_insns; /* ARM ops run by the portable cached-block interpreter */
    /* Native (host-JIT) execution. */
    uint64_t native_block_calls;     /* generated block entries */
    uint64_t native_arm_insns;       /* guest ARM insns retired by native code */
    uint64_t native_bail_calls;      /* native calls that retired zero insns */
    /* Idle-poll fixed-point repeat skipping. */
    uint64_t poll_skip_events;
    uint64_t poll_skipped_insns;
    /* Block-cache / JIT pipeline. */
    uint64_t jit_hits;
    uint64_t jit_misses;
    uint64_t jit_fallbacks;          /* unclassified ops reaching the exact interpreter */
    uint64_t jit_blocks_compiled;
    uint64_t jit_block_conflicts;    /* translate evicted a live same-generation block at a different PC */
    uint64_t jit_translate_failures;
    uint64_t jit_native_compiled;
    uint64_t jit_native_failed;      /* includes code-cache-full on backends that cannot count it directly */
    uint64_t jit_code_full_events;   /* executable cache reserve failure */
    uint64_t jit_code_alloc_failures;
    /* Invalidation causes. */
    uint64_t jit_invalidations;
    uint64_t jit_inv_reset;
    uint64_t jit_inv_jit_disable;
    uint64_t jit_inv_api_flush;
    uint64_t jit_inv_state_load;
    uint64_t jit_inv_cp15_mmu;       /* control/TTB/TLB maintenance writes */
    uint64_t jit_inv_cp15_cache;     /* icache/all/prefetch cache ops */
    uint64_t jit_inv_code_recycle;   /* executable arena reuse */
    /* Calls from generated native code into C helpers, by helper kind. */
    uint64_t helper_ld_word;
    uint64_t helper_ld_byte;
    uint64_t helper_ld_sbyte;
    uint64_t helper_ld_half;
    uint64_t helper_ld_shalf;
    uint64_t helper_st_word;
    uint64_t helper_st_byte;
    uint64_t helper_st_half;
    uint64_t helper_write_pc;
    uint64_t helper_interp_ops;      /* classified-op helper calls (slow insns inside native blocks) */
    uint64_t helper_op_kinds[GP32_CPU_PROFILE_OP_KINDS]; /* guest op-kind split of helper_interp_ops */
    /* Executable code-cache occupancy at snapshot time. */
    uint64_t jit_code_size;
    uint64_t jit_code_used;
    uint64_t jit_code_highwater;
    uint64_t jit_block_capacity;
    /* Why slow helpers ran: compile-time shape gates vs runtime guards.
     * AArch64 only, recomputed from pre-op state; TLB probes never fill. */
    uint64_t slow_gate_data_regshift; /* regshift with unsupported PC operands */
    uint64_t slow_gate_data_r15flags; /* data: S-flag write to r15 */
    uint64_t slow_gate_mul;           /* mul: PC operand */
    uint64_t slow_gate_block_shape;   /* block: S bit/PC base/no fastmem/etc. */
    uint64_t slow_gate_single_shape;  /* single: PC writeback or no fastmem */
    uint64_t slow_bail_block_unaligned;
    uint64_t slow_bail_block_xpage;   /* reserved: crossing alone no longer bails */
    uint64_t slow_bail_block_tinypage;/* TLB mask < 4 KiB */
    uint64_t slow_bail_block_nonram;  /* phys span outside direct RAM window */
    uint64_t slow_bail_block_pcodd;   /* popped PC is odd (Thumb target) */
    uint64_t slow_bail_block_other;   /* eligible shape, unexplained bail */
    uint64_t slow_bail_single_nonram; /* phys outside RAM window (MMIO etc.) */
    uint64_t slow_bail_single_tlbmiss;/* MMU on and no live TLB entry */
    uint64_t slow_bail_single_other;
    uint64_t slow_bail_other;         /* kinds without native emitters */
    uint64_t slow_bail_block_tlbmiss; /* either fragment has no live TLB entry */
    /* Non-RAM single transfers: all counts by physical top byte, plus the
     * first 64 distinct addresses. Overflow is explicit, not sampled away. */
    uint64_t single_nonram_regions[256];
    gp32_memory_profile_t single_nonram_addresses[GP32_CPU_PROFILE_MEMORY_SLOTS];
    uint64_t single_nonram_address_overflow;
    /* Native returns to a different guest PC, by the exiting opcode kind.
     * The last slot covers native-loop blocks whose exact exit instruction
     * cannot be inferred from the returned instruction count alone. Ready
     * means the next cached block passes dispatch guards, not that chaining
     * is proven safe or profitable. Counted poll sources are never ready. */
    uint64_t native_cross_exits[GP32_CPU_PROFILE_OP_KINDS + 1u];
    uint64_t native_dispatch_ready_exits[GP32_CPU_PROFILE_OP_KINDS + 1u];
    /* Last-target prediction per compiled source block. Cold observations
     * have no prior target; ready hits also pass the cached successor guards.
     * These counters observe exits only, without fetching or chaining code. */
    uint64_t native_exit_predictions;
    uint64_t native_exit_prediction_hits;
    uint64_t native_exit_prediction_ready_hits;
    uint64_t native_exit_prediction_cold;
    uint64_t native_exit_prediction_second_hits;
    uint64_t native_exit_prediction_second_ready_hits;
} gp32_cpu_profile_t;

gp32_status_t gp32_get_cpu_profile(const gp32_t *gp32, gp32_cpu_profile_t *out);
gp32_status_t gp32_reset_cpu_profile(gp32_t *gp32);

const char *gp32_get_error(const gp32_t *gp32);

#ifdef __cplusplus
}
#endif

#endif /* GP32EMU_GP32_H */
