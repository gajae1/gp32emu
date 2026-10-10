#include "gp32emu/gp32.h"
#include "common.h"
#include "arm920t.h"
#include "s3c2400.h"
#include "fxe.h"
#include "fpk.h"
#include "smc_direct.h"
#include "save_atomic.h"

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#define GP32_HOST_SSE2 1
#define GP32_HOST_NEON 0
#elif (defined(__ARM_NEON) || defined(__ARM_NEON__)) && \
      defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#include <arm_neon.h>
#define GP32_HOST_SSE2 0
#define GP32_HOST_NEON 1
#else
#define GP32_HOST_SSE2 0
#define GP32_HOST_NEON 0
#endif

#define GP32_RAM_BASE 0x0c000000u
#define ARM_MODE_SVC 0x13u
#define ARM_I_FLAG 0x00000080u
#define ARM_F_FLAG 0x00000040u
#define ARM_T_FLAG 0x00000020u
#define GP32_DIRECT_GPOS_TIMER_COUNT 4u
#define GP32_DIRECT_PCM_CHANNELS 4u

typedef struct direct_timer_due {
    uint32_t callback, tps, fires, epoch;
} direct_timer_due_t;

enum { DIRECT_CB_NONE, DIRECT_CB_REFILL, DIRECT_CB_TIMER, DIRECT_CB_FAULT };
enum { DIRECT_TICK_IDLE, DIRECT_TICK_AFTER_REFILL, DIRECT_TICK_TIMER_CALLS };
enum { DIRECT_CB_TIMEOUT = 1, DIRECT_CB_STALLED, DIRECT_CB_TASK_SWITCH, DIRECT_CB_HOST_DISPLAY_WAIT };

typedef struct direct_callback_tail {
    uint32_t owner, fault_owner, fault_reason, fn;
    uint32_t ack_addr, ack_value;
    uint64_t deadline_ns;
    arm920t_register_context_t foreground;
    uint32_t sdk_task, suspended;
} direct_callback_tail_t;

typedef struct direct_hle_tick_tail {
    uint32_t phase, clock, volume;
    uint32_t sdk_frames_left, sdk_rate;
    uint32_t timer_slot, calls_left;
    direct_timer_due_t due[GP32_DIRECT_GPOS_TIMER_COUNT];
} direct_hle_tick_tail_t;

typedef struct gp32_elapsed_time {
    uint64_t nanoseconds;
    uint32_t remainder;
    uint32_t clock_hz;
} gp32_elapsed_time_t;

typedef struct gp32_frame_time {
    uint64_t deadline_ns;
    uint32_t remainder; /* fractional nanoseconds, denominator 60 */
    uint32_t valid;
} gp32_frame_time_t;

struct gp32 {
    s3c2400_t *soc;
    arm920t_t *cpu;
    gp32_log_fn log;
    void *log_user;
    /* Host delivery hook; never part of the machine state. */
    gp32_host_pump_fn host_pump;
    void *host_pump_user;
    char error[256];
    gp32_elapsed_time_t elapsed;
    gp32_frame_time_t frame_time;
    /* Dispatch-only; settled before returning to a public run/save boundary. */
    uint32_t direct_hle_pending_volume; /* bit 8 marks a pending six-bit value */
    int direct_cpu_running;
    int direct_fxe_mode;
    uint8_t adpcm_fix_disabled;  /* host game-fixes option; not serialized */
    uint8_t adpcm_fix_patched;   /* derived from RAM on every run/load */
    /* Fast loading: host policy and per-frame detector, never serialized. */
    uint8_t fast_load_disabled;
    uint8_t fast_load_active;
    uint8_t fast_load_streak;
    uint8_t fast_load_hash_valid;
    uint8_t fast_load_recent_audio;
    uint64_t fast_load_mark;
    uint64_t fast_load_hash;
    uint32_t direct_fxe_entry;
    uint32_t direct_fxe_stack;
    uint32_t direct_fxe_fb_addr;
    uint32_t direct_fxe_image_end;
    uint32_t direct_fxe_lcd_surface[4];
    uint32_t direct_fxe_surface_checksum[4];
    uint32_t direct_fxe_bpp;
    uint32_t direct_fxe_palette_addr;
    uint32_t direct_fxe_palette_initialized;
    uint32_t direct_fxe_lcd_explicit;
    uint32_t direct_fxe_lcd_enabled;
    char direct_fxe_title[33];
    char direct_smc_executable_path[260];
    char direct_smc_game_dir[260];
    fxe_image_t direct_reset_image;
    uint32_t direct_reset_image_valid;
    uint32_t direct_reset_scan_file_hle;
    uint32_t direct_reset_init_smc_gpio;
    /* Set when the loaded guest asks the firmware to restart the machine (SWI 4)
       or lands on the ROM's reset/fault vector; consumed at the frame boundary
       so the restart happens with the CPU idle. */
    uint32_t direct_reboot_pending;
    fpk_asset_t *direct_fpk_assets;
    size_t direct_fpk_asset_count;
    struct {
        const fpk_asset_t *asset;
        size_t pos;
        int used;
    } direct_fpk_handles[32];
    uint32_t direct_hle_file_open_addr;
    uint32_t direct_hle_file_read_addr[2];
    uint32_t direct_hle_file_close_addr;
    uint32_t direct_hle_file_size_addr;
    uint32_t direct_hle_file_seek_addr;
    uint32_t direct_hle_pathbuf_addr;
    uint32_t direct_hle_sound_dispatch_addr;
    uint32_t direct_hle_sound_table_addr;
    uint32_t direct_hle_sound_play_addr;
    uint32_t direct_hle_sound_state_addr;
    uint32_t direct_hle_pcm_env_addr;
    uint32_t direct_hle_pcm_init_addr;
    uint32_t direct_hle_pcm_play_addr;
    uint32_t direct_hle_pcm_stop_addr;
    uint32_t direct_hle_pcm_remove_addr;
    uint32_t direct_hle_pcm_lock_addr;
    uint32_t direct_hle_pcm_only_kill_addr;
    uint32_t direct_hle_pcm_initialized;
    uint32_t direct_hle_pcm_sr;
    uint32_t direct_hle_pcm_bit_count;
    uint32_t direct_hle_pcm_rate;
    uint32_t direct_hle_pcm_stereo;
    uint32_t direct_hle_pcm_bits;
    uint32_t direct_hle_pcm_active;
    uint32_t direct_hle_pcm_src_addr;
    uint32_t direct_hle_pcm_size_bytes;
    uint32_t direct_hle_pcm_pos_bytes;
    uint32_t direct_hle_pcm_repeat;
    uint64_t direct_hle_pcm_accum;
    struct {
        uint32_t active;
        uint32_t src_addr;
        uint32_t size_bytes;
        uint32_t pos_bytes;
        uint32_t repeat;
        uint32_t rate;
        uint32_t stereo;
        uint32_t bits;
        uint64_t accum;
    } direct_hle_pcm_ch[GP32_DIRECT_PCM_CHANNELS];
    uint32_t direct_hle_sdk_sndmixedbuf_addr;
    uint32_t direct_hle_sdk_sndsrcexist_addr;
    uint32_t direct_hle_sdk_pcm_workidx_addr;
    uint32_t direct_hle_sdk_sndmixer_addr;
    uint32_t direct_hle_sdk_mixbuf0_addr;
    uint32_t direct_hle_sdk_mixbuf1_addr;
    uint32_t direct_hle_sdk_mixbuf_bytes;
    uint32_t direct_hle_sdk_rate;
    uint64_t direct_hle_sdk_accum;
    uint64_t direct_hle_sdk_last_submit_cycle;
    uint64_t direct_hle_sdk_timer_accum;
    uint32_t direct_hle_sdk_submitted_frames;
    uint32_t direct_hle_sdk_timer_table_addr; /* Legacy state field; no runtime lookup. */
    struct {
        uint32_t configured;
        uint32_t enabled;
        uint32_t callback;
        uint32_t tps;
        uint32_t max_exec_tick;
        uint64_t accum;
    } direct_hle_gpos_timer[GP32_DIRECT_GPOS_TIMER_COUNT];
    /* Lifetimes also belong to a suspended tick and the v10 continuation. */
    uint32_t direct_hle_gpos_timer_epoch[GP32_DIRECT_GPOS_TIMER_COUNT];
    uint32_t direct_hle_gpos_timers_enabled;
    uint32_t direct_hle_gpos_task_first;
    uint32_t direct_hle_gpos_task_last;
    uint32_t direct_hle_gpos_scheduler_callback;
    uint32_t direct_hle_callback_returned;
    uint32_t direct_hle_callback_running;
    direct_callback_tail_t direct_callback;
    direct_hle_tick_tail_t direct_tick;
    uint32_t direct_hle_audio_rate_override;
    uint32_t direct_hle_audio_last_auto_rate;
    const fpk_asset_t *direct_hle_audio_asset;
    uint32_t direct_hle_audio_pos;
    uint32_t direct_hle_audio_size;
    uint32_t direct_hle_audio_rate;
    uint64_t direct_hle_audio_accum;
    struct {
        const fpk_asset_t *asset;
        uint32_t copied;
        uint32_t tries;
    } direct_hle_asset_autoload[16];
    gp32_frame_time_t direct_vblank_time; /* next unreserved panel-rate deadline */
    int direct_vblank_wait_requested; /* dispatch only: valid/invalid surface */
};

static void seterr(gp32_t *g, const char *fmt, ...) {
    if (!g) return;
    va_list ap; va_start(ap, fmt); vsnprintf(g->error, sizeof(g->error), fmt, ap); va_end(ap);
}
static void direct_callback_error(gp32_t *g) {
    const direct_callback_tail_t *cb = &g->direct_callback;
    const char *reason = cb->fault_reason == DIRECT_CB_TIMEOUT ? "emulated-time watchdog" :
        cb->fault_reason == DIRECT_CB_STALLED ? "halted or zero-progress CPU" :
        cb->fault_reason == DIRECT_CB_TASK_SWITCH ? "SDK task-switch fault retained from older continuation state" :
        "display-wait fault retained from older continuation state";
    seterr(g, "HLE callback fault: %s; owner=%u fn=%08x pc=%08x sp=%08x ns=%" PRIu64,
           reason, cb->fault_owner, cb->fn, arm920t_get_pc(g->cpu),
           arm920t_get_reg(g->cpu, 13), g->elapsed.nanoseconds);
}
static void direct_callback_fault(gp32_t *g, uint32_t reason) {
    if (!g || (g->direct_callback.owner != DIRECT_CB_REFILL &&
               g->direct_callback.owner != DIRECT_CB_TIMER)) return;
    g->direct_callback.fault_owner = g->direct_callback.owner;
    g->direct_callback.fault_reason = reason;
    g->direct_callback.owner = DIRECT_CB_FAULT;
    g->direct_hle_callback_returned = 0u;
    g->direct_hle_callback_running = 0u;
    arm920t_stop_run(g->cpu);
    direct_callback_error(g);
}
static void bridge_log(void *user, const char *line) {
    gp32_t *g = (gp32_t *)user;
    if (g && g->log) g->log(g->log_user, line);
}

static int direct_ram_range(const gp32_t *g, uint32_t addr, uint32_t len) {
    if (!g || !g->soc || addr < GP32_RAM_BASE) return 0;
    uint32_t end = GP32_RAM_BASE + (uint32_t)s3c2400_ram_size(g->soc);
    if (addr > end) return 0;
    return len <= end - addr;
}
static void direct_write32_if_ram(gp32_t *g, uint32_t addr, uint32_t value) {
    if (direct_ram_range(g, addr, 4u)) s3c2400_write32(g->soc, addr, value);
}
static void direct_write8_if_ram(gp32_t *g, uint32_t addr, uint8_t value) {
    if (direct_ram_range(g, addr, 1u)) s3c2400_write8(g->soc, addr, value);
}
static void direct_write_fw_arg(gp32_t *g, uint32_t addr, uint32_t value) {
    if ((addr & 3u) == 0u) direct_write32_if_ram(g, addr, value);
    else direct_write8_if_ram(g, addr, (uint8_t)value);
}
static uint32_t direct_hle_work_base(const gp32_t *g) {
    uint32_t ram_size = (g && g->soc) ? (uint32_t)s3c2400_ram_size(g->soc) : 0u;
    /* Keep direct-FXE firmware work memory out of the application stack.
       GPSDK scheduler stacks commonly live at the top of RAM; aliasing the
       firmware HLE stubs there can corrupt saved task contexts. */
    if (ram_size >= 0x00800000u) return GP32_RAM_BASE + 0x007d0000u;
    if (ram_size >= 0x00400000u) return GP32_RAM_BASE + ram_size - 0x30000u;
    return GP32_RAM_BASE + 0x100u;
}
static uint32_t direct_stub_addr(const gp32_t *g) {
    return direct_hle_work_base(g);
}
static uint32_t direct_ret_stub_addr(const gp32_t *g) { return direct_stub_addr(g) + 16u; }
static uint32_t direct_callback_return_stub_addr(const gp32_t *g) { return direct_stub_addr(g) + 24u; }
static uint32_t direct_wait_swi_addr(const gp32_t *g) { return direct_stub_addr(g) + 0x2cu; }
static uint32_t direct_time_mirror_addr(const gp32_t *g) { return direct_stub_addr(g) + 0x74u; }
static uint32_t direct_app_arg_addr(const gp32_t *g) { return direct_stub_addr(g) + 0x900u; }
static uint32_t direct_smc_cb_base_addr(const gp32_t *g) { return direct_stub_addr(g) + 0xb00u; }

static void direct_install_stubs(gp32_t *g) {
    uint32_t a = direct_stub_addr(g);
    if (!direct_ram_range(g, a, 0xacu)) return;
    /* GPSDK init stores the firmware display callback returned by SWI #0x0b
       selector 0 into both GpSurfaceSet and GpSurfaceFlip.  The callback is
       passed a GPDRAWSURFACE and makes that surface visible; it must not rewrite
       the application-owned descriptor.  Keep the descriptor intact and bounce
       through a private direct-mode SWI so the C-side HLE can install the LCD
       base address from ptgpds->ptbuffer. */
    s3c2400_write32(g->soc, a + 0u, 0xea000006u); /* b wait body */
    s3c2400_write32(g->soc, a + 4u, 0xe12fff1eu); /* bx lr */
    s3c2400_write32(g->soc, a + 8u, 0xe12fff1eu);
    s3c2400_write32(g->soc, a + 12u, 0xe12fff1eu);
    /* Generic firmware callback slot: return 0. */
    s3c2400_write32(g->soc, a + 16u, 0xe3a00000u); /* mov r0,#0 */
    s3c2400_write32(g->soc, a + 20u, 0xe12fff1eu); /* bx lr */
    s3c2400_write32(g->soc, a + 24u, 0xef070020u); /* private callback-return trap */
    s3c2400_write32(g->soc, a + 28u, 0xeafffffeu); /* b . */
    /* Each invocation owns r1:r2 and its stack frame. A normal IRQ/FIQ or a
     * suspended HLE callback preserves that deadline just like other guest
     * registers. Never borrow a shared deadline from RAM in the polling loop.
     * SUBS/SBCS tests the sign of the modular 64-bit time difference, including
     * both the low-word carry and the full uint64 wrap. Waits are < 2^63 ns. */
    const uint32_t wait[] = {
        0xe92d400eu, /* push {r1-r3,lr} */
        0xe10f3000u, /* mrs r3,cpsr */
        0xe92d1008u, /* push {r3,r12}: flags and scratch */
        0xef000011u, /* display and arm this call after settling SWI time */
        0xe59f3038u, /* ldr r3,mirror pointer @ +0x70 */
        0xe593c004u, /* +34: ldr r12,[r3,#4], high */
        0xe5930000u, /* ldr r0,[r3], low */
        0xe593e004u, /* ldr lr,[r3,#4], high again */
        0xe15c000eu, /* cmp r12,lr: reject a torn pair */
        0x1afffffau, /* bne +34 */
        0xe0500001u, /* subs r0,r0,r1 */
        0xe0dcc002u, /* sbcs r12,r12,r2 */
        0x4afffff7u, /* bmi +34 */
        0xe3a00001u, /* mov r0,#1 */
        0xe8bd1008u, /* pop {r3,r12} */
        0xe128f003u, /* msr cpsr_f,r3 */
        0xe8bd400eu, /* pop {r1-r3,lr} */
        0xe12fff1eu, /* bx lr (also returns to Thumb) */
    };
    for (unsigned i = 0; i < GP32_ARRAY_COUNT(wait); ++i)
        s3c2400_write32(g->soc, a + 0x20u + i * 4u, wait[i]);
    s3c2400_write32(g->soc, a + 0x70u, direct_time_mirror_addr(g));
    /* v2-v10 host-idle migration only. The loader builds a guest stack frame
     * with the interrupted scratch registers/PC/CPSR. It executes the same
     * ordinary polling instructions, then restores that frame via an origin-
     * checked service. There is no host idle-cycle advancement. */
    const uint32_t legacy_wait[] = {
        0xe51f3018u, /* ldr r3,mirror pointer @ +0x70 */
        0xe593c004u, 0xe5930000u, 0xe593e004u, 0xe15c000eu, 0x1afffffau,
        0xe0500001u, 0xe0dcc002u, 0x4afffff7u,
        0xef070021u, /* +a4: restore migrated guest frame */
        0xeafffffeu,
    };
    for (unsigned i = 0; i < GP32_ARRAY_COUNT(legacy_wait); ++i)
        s3c2400_write32(g->soc, a + 0x80u + i * 4u, legacy_wait[i]);
}
/* The firmware clock service (retail ROM 0x200c) as the launch path (ROM
 * 0x20ec-0x210c), the reinit service (0x6b3c-0x6b5c) and SWI 0x0d (ROM 0x1398)
 * call it. All three pass the LOCKTIME word of the default clock table (ROM
 * 0x1090; SWI 0x0d loads it at 0x13ac) with their own MPLLCON and CLKDIVN. It
 * writes CLKDIVN, LOCKTIME and MPLLCON in that order (0x204c-0x2054), stores the
 * {FCLK, HCLK, PCLK} block SWI 0x0b selector 4 returns (0x2064-0x2094), then sets
 * the REFRESH counter to 2049 - (HCLK / 1024 * 156) / 10000 (0x2098-0x20d0:
 * 0x5fd at the BIOS 33.9 MHz). The SoC model switches clocks atomically, so the
 * ROM's PLL settling step (0x2024-0x203c) has no equivalent, and the block's
 * FCLK comes from the PLL register where the ROM copies the caller's word. */
#define DIRECT_FW_LOCKTIME 0x007d07d0u
static void direct_publish_fw_clocks(gp32_t *g);
static void direct_apply_fw_clock(gp32_t *g, uint32_t mpllcon, uint32_t clkdivn) {
    s3c2400_write32(g->soc, 0x14800014u, clkdivn & 3u);
    s3c2400_write32(g->soc, 0x14800000u, DIRECT_FW_LOCKTIME);
    s3c2400_write32(g->soc, 0x14800004u, mpllcon);
    uint32_t drop = (uint32_t)(((uint64_t)(s3c2400_hclk_hz(g->soc) >> 10) * 156u) / 10000u);
    uint32_t refresh = s3c2400_debug_read32(g->soc, 0x14000024u);
    s3c2400_write32(g->soc, 0x14000024u, (refresh & ~0x7ffu) | ((0x801u - drop) & 0x7ffu));
    direct_publish_fw_clocks(g);
}

/* The default clock table at ROM 0x1090, which the launch path and the reinit
 * service hand to the clock service: CLKCON 0x4768, then MPLLCON 0x69032 with
 * CLKDIVN 3 (FCLK 67.8 MHz, HCLK 33.9 MHz, PCLK 16.95 MHz). */
static void direct_apply_fw_default_clock(gp32_t *g) {
    s3c2400_write32(g->soc, 0x1480000cu, 0x00004768u);
    direct_apply_fw_clock(g, 0x00069032u, 3u);
}

/* Register state the retail firmware leaves at the first instruction of a
 * title.  Read back at game entry in BIOS boots of ten cards: Her Knights,
 * Blue Angelo, Dungeon & Guarder EU, Little Wizard EU, Funny Soccer, Holeman,
 * Super Plusha, Tales of Windy Land and Topy Topy Gogo under BIOS 1.6.6, and
 * Pinball Dreams under the 2003-05-21 BIOS (BIOS 1.6.6 never launches it).
 * Every value below was identical on all of them.  ROM addresses are BIOS 1.6.6.
 * Not reproduced: card-dependent leftovers (pending interrupts, DMA pointers,
 * GPBDAT readback, phase counters), the BIOS tick (timers 0, 1 and 4 running
 * with ROM ISRs that direct mode does not have), CP15/MMU and the codec
 * latches (status 0x10, VC 0, control 0x80), which change no modelled output.
 * Every write goes through the register model, so rates derived from the
 * clock tree follow it. */
static void direct_init_soc_handoff(gp32_t *g) {
    if (!g || !g->soc) return;
    /* Memory controller: BWSCON through MRSRB7, reset code ROM 0x1b8 (literals
     * 0x2ac/0x2b0); the REFRESH counter bits come from the clock below. */
    static const uint32_t memcon[13] = {
        0x11111110u, 0x00000400u, 0x00000f00u, 0x11111110u, 0x11111110u, 0x11111110u, 0x11111110u,
        0x00018000u, 0x00018000u, 0x00890543u, 0x00000016u, 0x00000020u, 0x00000020u,
    };
    for (unsigned i = 0; i < GP32_ARRAY_COUNT(memcon); ++i)
        s3c2400_write32(g->soc, 0x14000000u + i * 4u, memcon[i]);
    /* Clock tree: UPLLCON from the reset literal at ROM 0x2bc (stm at 0x1d4),
     * CLKSLOW never written, CLKCON and the PLL/divider from the default
     * clock table at ROM 0x1090 that the launch path (0x20ec-0x210c) feeds to
     * the clock service: FCLK 67.8 MHz, HCLK 33.9 MHz, PCLK 16.95 MHz. */
    s3c2400_write32(g->soc, 0x14800008u, 0x00058042u);
    s3c2400_write32(g->soc, 0x14800010u, 0x00000000u);
    direct_apply_fw_default_clock(g);
    /* Interrupt controller (int_init, ROM 0x15e0): INTMOD 0, PRIORITY
     * 0x18002b (0x1608), and the sources the ROM unmasks for its own ISRs:
     * EINT2 (0x164c), TIMER0 (0x1684), TIMER4 (0x16b8), USBD (0x16f0); DMA3
     * follows through the SWI 9 service (0x1260-0x1284). */
    s3c2400_write32(g->soc, 0x14400004u, 0x00000000u);
    s3c2400_write32(g->soc, 0x1440000cu, 0x0018002bu);
    s3c2400_write32(g->soc, 0x14400008u, 0xfdefbbfbu);
    /* PWM prescaler 1 = 0x63 and MUX4 = 3 from the timer-4 init (ROM 0x3c2c,
     * 0x3c3c); prescaler 0 = 0x33 and MUX0/1 = 3 are the menu's leftovers. */
    s3c2400_write32(g->soc, 0x15100000u, 0x00006333u);
    s3c2400_write32(g->soc, 0x15100004u, 0x00030033u);
    /* IIS: the launch-path stop (ROM 0x7804) leaves IISCON 0x0e and IISFCON
     * 0xa00 (0x7818, 0x7824); the sound routine at ROM 0x3dcc leaves IISMOD
     * 0x89 (IIS format, 16 bit, 256fs) and IISPSR 0xa5 (0x3de4, 0x3df4):
     * 11,035 Hz at the BIOS PCLK. */
    s3c2400_write32(g->soc, 0x15508008u, 0x000000a5u);
    s3c2400_write32(g->soc, 0x15508004u, 0x00000089u);
    s3c2400_write32(g->soc, 0x15508000u, 0x0000000eu);
    s3c2400_write32(g->soc, 0x1550800cu, 0x00000a00u);
    /* GPIO: port init ROM 0x1704, SmartMedia pins 0x66b4 (GPBCON low half
     * 0x5555, GPDCON 0x15000, GPECON 0x560), L3 pins 0x5530 (GPEDAT idle
     * level 0x600 with MODE and CLOCK high, GPEUP 0xe00, GPECON 0x540000) and
     * EXTINT 0x700 (0x1668). GPDDAT 0x3c0 is the SmartMedia driver's
     * deselected level.  GPEDAT goes before GPECON so the L3 decoder never
     * sees the idle level as an edge. */
    static const uint32_t gpio[][2] = {
        {0x15600000u, 0x000004ffu}, {0x15600008u, 0x00005555u}, {0x15600010u, 0x00000000u},
        {0x15600014u, 0xaaaaaaaau}, {0x1560001cu, 0x0000ffffu}, {0x15600020u, 0x000150aau},
        {0x15600024u, 0x000003c0u}, {0x15600028u, 0x0000000fu}, {0x15600030u, 0x000006c8u},
        {0x1560002cu, 0x00540560u}, {0x15600034u, 0x00000e00u}, {0x15600038u, 0x00000aaau},
        {0x15600040u, 0x0000003fu}, {0x15600044u, 0x00003caau}, {0x1560004cu, 0x0000006fu},
        {0x15600058u, 0x00000700u},
    };
    for (unsigned i = 0; i < GP32_ARRAY_COUNT(gpio); ++i)
        s3c2400_write32(g->soc, gpio[i][0], gpio[i][1]);
}

/* SmartMedia loads also park the data bus high; the control pins already hold
 * the firmware's idle levels from direct_init_soc_handoff. */
static void direct_init_smc_gpio(gp32_t *g) {
    if (!g || !g->soc) return;
    s3c2400_write32(g->soc, 0x1560000cu, 0x000000ffu);
}

static void direct_install_smc_callbacks(gp32_t *g) {
    static const uint32_t code[] = {
        0xe92d4000u,0xe3a01015u,0xe1a01401u,0xe3811060u,0xe1a01801u,0xe591200cu,0xe3a030ffu,0xe0022403u,
        0xe1822000u,0xe581200cu,0xe5912030u,0xe3c22008u,0xe5812030u,0xe3822008u,0xe5812030u,0xe8bd8000u,
        0xe92d4000u,0xe3a01015u,0xe1a01401u,0xe3811060u,0xe1a01801u,0xe5912024u,0xe3a03001u,0xe1c22403u,
        0xe5812024u,0xe591000cu,0xe20000ffu,0xe1822403u,0xe5812024u,0xe8bd8000u,
        0xe92d4000u,0xe3a01015u,0xe1a01401u,0xe3811060u,0xe1a01801u,0xe3a02002u,0xe1a02402u,0xe5910030u,
        0xe3100004u,0x1a000002u,0xe5910024u,0xe1100002u,0x0afffff9u,0xe8bd8000u,
        0xe92d4000u,0xe3a01015u,0xe1a01401u,0xe3811060u,0xe1a01801u,0xe3a020ffu,0xe1822402u,0xe5910008u,
        0xe1c00002u,0xe5810008u,0xe8bd8000u,
        0xe92d4000u,0xe3a01015u,0xe1a01401u,0xe3811060u,0xe1a01801u,0xe3a020ffu,0xe1822402u,0xe5910008u,
        0xe1c00002u,0xe3a02055u,0xe1822402u,0xe1800002u,0xe5810008u,0xe8bd8000u,
        0xe92d4000u,0xe3a01015u,0xe1a01401u,0xe3811060u,0xe1a01801u,0xe5910030u,0xe3800010u,0xe5810030u,
        0xe8bd8000u,
        0xe92d4000u,0xe3a01015u,0xe1a01401u,0xe3811060u,0xe1a01801u,0xe5910030u,0xe3c00010u,0xe5810030u,
        0xe8bd8000u,
        0xe92d4000u,0xe3a01015u,0xe1a01401u,0xe3811060u,0xe1a01801u,0xe5910030u,0xe3800020u,0xe5810030u,
        0xe8bd8000u,
        0xe92d4000u,0xe3a01015u,0xe1a01401u,0xe3811060u,0xe1a01801u,0xe5910030u,0xe3c00020u,0xe5810030u,
        0xe8bd8000u,
        0xe92d4000u,0xe3a01015u,0xe1a01401u,0xe3811060u,0xe1a01801u,0xe5910024u,0xe3c00080u,0xe5810024u,
        0xe8bd8000u,
        0xe92d4000u,0xe3a01015u,0xe1a01401u,0xe3811060u,0xe1a01801u,0xe5910024u,0xe3800080u,0xe5810024u,
        0xe8bd8000u,
        0xe92d4000u,0xe3a01015u,0xe1a01401u,0xe3811060u,0xe1a01801u,0xe5910024u,0xe3800040u,0xe5810024u,
        0xe8bd8000u,
        0xe92d4000u,0xe3a01015u,0xe1a01401u,0xe3811060u,0xe1a01801u,0xe5910024u,0xe3c00040u,0xe5810024u,
        0xe8bd8000u
    };
    uint32_t base = direct_smc_cb_base_addr(g);
    if (!direct_ram_range(g, base, (uint32_t)sizeof(code))) return;
    for (size_t i = 0; i < sizeof(code) / sizeof(code[0]); ++i) s3c2400_write32(g->soc, base + (uint32_t)i * 4u, code[i]);
}

static uint32_t direct_fwinfo_addr(const gp32_t *g) {
    return direct_hle_work_base(g) + 0x10000u;
}

static uint32_t direct_fw_tick_addr(const gp32_t *g) {
    return direct_fwinfo_addr(g) + 0x20u;
}

static uint32_t direct_pcm_cursor_addr(const gp32_t *g) {
    return direct_fwinfo_addr(g) + 0x80u;
}

static void direct_update_fw_tick(gp32_t *g);
static void direct_schedule_vblank_wait(gp32_t *g);
/* Modular deadlines stay meaningful within half the uint64 time range. */
static int direct_time_pending(uint64_t deadline, uint64_t now) {
    uint64_t delta = deadline - now;
    return delta && delta < (UINT64_C(1) << 63);
}
static void direct_hle_audio_tick(gp32_t *g, uint32_t cycles, uint32_t clock);
static void direct_hle_gpos_timer_prepare(gp32_t *g, uint32_t cycles, uint32_t clock,
                                        direct_timer_due_t pending[GP32_DIRECT_GPOS_TIMER_COUNT]);
static void direct_hle_gpos_timer_dispatch(gp32_t *g);
static void direct_hle_tick_pump(gp32_t *g);
static gp32_status_t gp32_load_fxe_image_internal(gp32_t *g, fxe_image_t *img, int update_reset_image, int scan_file_hle, int init_smc_gpio, int preserve_hle_options);
static int direct_handle_swi_gpos_timer(gp32_t *g, arm920t_t *cpu, uint32_t pc);
static void direct_tick_sdk_task_sleepers(gp32_t *g, uint32_t first_task, uint32_t last_task, uint32_t ticks);
static int direct_resume_ready_sdk_task(gp32_t *g, uint32_t pc, uint32_t first_task, uint32_t last_task);
static int direct_task_record_plausible(gp32_t *g, uint32_t task_addr);

static uint32_t direct_firmware_clock_hz(const gp32_t *g) {
    uint32_t hz = 0u;
    if (g && g->soc) hz = s3c2400_fclk_hz(g->soc);
    return hz ? hz : 66000000u;
}

static uint32_t direct_run_clock_hz(const gp32_t *g) {
    uint32_t hz = 0u;
    if (g && g->soc) hz = s3c2400_run_clock_hz(g->soc);
    return hz ? hz : 66000000u;
}

static uint32_t direct_elapsed_ms(const gp32_t *g) {
    return g ? (uint32_t)(g->elapsed.nanoseconds / 1000000u) : 0u;
}

static void direct_account_elapsed(gp32_t *g, uint32_t cycles, uint32_t clock) {
    if (!cycles) return;
    /* Carry sub-nanosecond phase across slices. A clock change only converts
     * that fraction; it must never rescale already elapsed whole time. */
    if (g->elapsed.clock_hz && g->elapsed.clock_hz != clock)
        g->elapsed.remainder = (uint32_t)((uint64_t)g->elapsed.remainder * clock / g->elapsed.clock_hz);
    uint64_t scaled = (uint64_t)cycles * 1000000000u + g->elapsed.remainder;
    g->elapsed.nanoseconds += scaled / clock;
    g->elapsed.remainder = (uint32_t)(scaled % clock);
    g->elapsed.clock_hz = clock;
}

static uint32_t direct_run_cpu(gp32_t *g, uint32_t cycles, uint32_t clock) {
    int was_running = g->direct_cpu_running;
    g->direct_cpu_running = 1;
    uint32_t done = s3c2400_run_cpu(g->soc, cycles);
    g->direct_cpu_running = was_running;
    direct_account_elapsed(g, done, clock);
    direct_schedule_vblank_wait(g);
    /* A refill/timer callback is already outside the foreground audio prefix.
       Commit its command after hardware time, before its next instruction. */
    if (g->direct_hle_callback_running && g->direct_hle_pending_volume) {
        uint32_t volume = g->direct_hle_pending_volume;
        g->direct_hle_pending_volume = 0;
        s3c2400_audio_set_volume(g->soc, volume);
    }
    return done;
}

static void direct_hle_tick(gp32_t *g, uint32_t cycles, uint32_t clock) {
    /* Every consumer receives the rate at which these cycles elapsed, even
     * if a timer/refill callback changes the clock before mixing finishes. */
    /* Mix the elapsed prefix before either the foreground command or a timer
       callback can change its gain. SDK refills remain at sample boundaries
       inside audio_tick; their commands precede this foreground command. */
    if (!g || !cycles || g->direct_tick.clock || g->direct_callback.owner) return;
    direct_hle_tick_tail_t *tail = &g->direct_tick;
    tail->clock = clock;
    tail->volume = g->direct_hle_pending_volume;
    g->direct_hle_pending_volume = 0;
    /* A refill may start or replace a timer. Capture the old lifetimes before
       it runs so new timers cannot inherit this already elapsed interval. */
    direct_hle_gpos_timer_prepare(g, cycles, clock, tail->due);
    direct_hle_audio_tick(g, cycles, clock);
    if (!g->direct_callback.owner) direct_hle_tick_pump(g);
}

static uint32_t direct_key_port_value(gp32_t *g, uint32_t value) {
    if (!g || !g->soc) return value;
    if (value >= 0x15600000u && value <= 0x1560005bu) return s3c2400_read32(g->soc, value & ~3u);
    return value;
}

static void direct_request_vblank_wait(gp32_t *g, int valid_surface) {
    g->direct_vblank_wait_requested = valid_surface ? 1 : 2;
    arm920t_stop_run(g->cpu);
}

/* The guest takes LINECNT/VSTATUS from the LCD controller as its frame
 * boundary, so the direct-mode vblank deadline must run at the rate the panel
 * is actually programmed to instead of a fixed 60 Hz. gp32_frame_time_t
 * already carries the period numerator over a denominator-60 residue, so a
 * panel period of ns + frac/2^20 maps onto that accumulator with no wire
 * change. Returns the legacy 1e9/60 ns numerator when no TFT frame clock can
 * be derived, which keeps BIOS/STN guests on the exact 60 Hz pacing. */
static uint64_t direct_vblank_period60(gp32_t *g) {
    uint32_t ns = 0u, frac = 0u;
    if (g && g->soc && s3c2400_lcd_frame_period(g->soc, &ns, &frac)) {
        uint64_t scaled = ((uint64_t)ns << 20) | frac;
        uint64_t period60 = (scaled * 60u + (1u << 19)) >> 20;
        if (period60 >= 60u) return period60;
    }
    return 1000000000u;
}

static void direct_advance_vblank_deadline(gp32_t *g, uint64_t period60) {
    gp32_frame_time_t *v = &g->direct_vblank_time;
    uint64_t acc = period60 + v->remainder;
    v->deadline_ns += acc / 60u;
    v->remainder = (uint32_t)(acc % 60u);
}

static void direct_schedule_vblank_wait(gp32_t *g) {
    if (!g || !g->cpu || !g->direct_vblank_wait_requested) return;
    uint64_t deadline = g->elapsed.nanoseconds;
    if (g->direct_vblank_wait_requested == 1) {
        gp32_frame_time_t *v = &g->direct_vblank_time;
        uint64_t period60 = direct_vblank_period60(g);
        /* A missed reservation rebases to a full interval from emulated now.
         * Host suspension advances no time. Clock writes do not rescale it. */
        if (!v->valid || !direct_time_pending(v->deadline_ns, deadline)) {
            v->deadline_ns = deadline;
            v->remainder = 0u;
            v->valid = 1u;
            direct_advance_vblank_deadline(g, period60);
        }
        deadline = v->deadline_ns;
        direct_advance_vblank_deadline(g, period60);
    }
    arm920t_set_reg(g->cpu, 1u, (uint32_t)deadline);
    arm920t_set_reg(g->cpu, 2u, (uint32_t)(deadline >> 32));
    g->direct_vblank_wait_requested = 0;
}

static void direct_update_fw_tick(gp32_t *g) {
    if (!g || !g->direct_fxe_mode) return;
    direct_write32_if_ram(g, direct_fw_tick_addr(g), direct_elapsed_ms(g));
    /* CPU/hardware settlement is synchronous: no guest runs between these
     * writes. A high/low/high guest read can span run boundaries, so it retries
     * if the high word changed. Each bounded slice advances < 2^32 ns. */
    uint32_t mirror = direct_time_mirror_addr(g);
    direct_write32_if_ram(g, mirror, (uint32_t)g->elapsed.nanoseconds);
    direct_write32_if_ram(g, mirror + 4u, (uint32_t)(g->elapsed.nanoseconds >> 32));
}

static void direct_migrate_vblank_wait(gp32_t *g, uint64_t cycles) {
    uint32_t clock = direct_run_clock_hz(g);
    uint64_t phase = g->elapsed.remainder;
    if (g->elapsed.clock_hz && g->elapsed.clock_hz != clock)
        phase = phase * clock / g->elapsed.clock_hz;
    /* Legacy cycle waits have no historical deadline clock. Convert the
     * outstanding cycles once at the restored run clock (legacy policy). */
    uint64_t ns = (cycles / clock) * 1000000000u +
        ((cycles % clock) * 1000000000u + phase) / clock;
    uint64_t deadline = g->elapsed.nanoseconds + ns;
    uint32_t sp = arm920t_get_reg(g->cpu, 13u) - 32u;
    const unsigned regs[] = {0u, 1u, 2u, 3u, 12u, 14u, 15u};
    for (unsigned i = 0; i < GP32_ARRAY_COUNT(regs); ++i)
        direct_write32_if_ram(g, sp + i * 4u, arm920t_get_reg(g->cpu, regs[i]));
    uint32_t cpsr = arm920t_get_cpsr(g->cpu);
    direct_write32_if_ram(g, sp + 28u, cpsr);
    arm920t_set_cpsr(g->cpu, cpsr & ~ARM_T_FLAG);
    arm920t_set_reg(g->cpu, 13u, sp);
    arm920t_set_reg(g->cpu, 1u, (uint32_t)deadline);
    arm920t_set_reg(g->cpu, 2u, (uint32_t)(deadline >> 32));
    arm920t_set_reg(g->cpu, 15u, direct_stub_addr(g) + 0x80u);
    g->direct_vblank_time = (gp32_frame_time_t){deadline, 0u, 1u};
    direct_advance_vblank_deadline(g, direct_vblank_period60(g));
}

/* SWI 0x0b selector 4 returns the firmware's clock block {FCLK, HCLK, PCLK}
 * in Hz (ROM 0x1340 returns its address, 0x2064-0x2094 fill it). */
static void direct_publish_fw_clocks(gp32_t *g) {
    uint32_t info = direct_fwinfo_addr(g);
    uint32_t fclk = direct_firmware_clock_hz(g);
    direct_write32_if_ram(g, info + 0u, fclk);
    direct_write32_if_ram(g, info + 4u, g->soc ? s3c2400_hclk_hz(g->soc) : fclk);
    direct_write32_if_ram(g, info + 8u, g->soc ? s3c2400_pclk_hz(g->soc) : fclk);
}

static void direct_sync_fwinfo(gp32_t *g) {
    direct_publish_fw_clocks(g);
    direct_update_fw_tick(g);
}

static void direct_copy_bytes_if_ram(gp32_t *g, uint32_t dst, const uint8_t *src, uint32_t len) {
    if (!g || !src || !direct_ram_range(g, dst, len)) return;
    for (uint32_t i = 0; i < len; ++i) s3c2400_write8(g->soc, dst + i, src[i]);
}

static void direct_zero_if_ram(gp32_t *g, uint32_t dst, uint32_t len) {
    if (!g || !direct_ram_range(g, dst, len)) return;
    for (uint32_t i = 0; i < len; ++i) s3c2400_write8(g->soc, dst + i, 0u);
}

static void direct_write_cstr_if_ram(gp32_t *g, uint32_t dst, const char *s) {
    if (!g || !s) return;
    size_t n = strlen(s) + 1u;
    if (n > 512u) n = 512u;
    if (!direct_ram_range(g, dst, (uint32_t)n)) return;
    for (size_t i = 0; i < n; ++i) s3c2400_write8(g->soc, dst + (uint32_t)i, (uint8_t)s[i]);
}

static void direct_apply_gxb_scatterload(gp32_t *g, const fxe_image_t *img) {
    if (!g || !img || !img->payload || img->payload_size < 0x20u) return;
    uint32_t first = gp32_ld32le(img->payload + 0u);
    uint32_t rom_start = gp32_ld32le(img->payload + 4u);
    uint32_t ro_limit = gp32_ld32le(img->payload + 8u);
    uint32_t rw_base = gp32_ld32le(img->payload + 12u);
    uint32_t zi_limit = gp32_ld32le(img->payload + 16u);
    uint32_t rw_limit = gp32_ld32le(img->payload + 20u);
    if ((first & 0x0f000000u) != 0x0a000000u) return;
    if (rom_start != img->load_addr || ro_limit < rom_start) return;
    if (rw_base < GP32_RAM_BASE || rw_limit < rw_base || zi_limit < rw_limit) return;
    uint32_t ro_size = ro_limit - rom_start;
    uint32_t rw_size = rw_limit - rw_base;
    uint32_t zi_size = zi_limit - rw_limit;
    if (rw_size && (uint64_t)ro_size + (uint64_t)rw_size <= img->payload_size) {
        direct_copy_bytes_if_ram(g, rw_base, img->payload + ro_size, rw_size);
    }
    if (zi_size) direct_zero_if_ram(g, rw_limit, zi_size);
}


static void direct_update_stub_framebuffer(gp32_t *g) {
    GP32_UNUSED(g);
}

static uint32_t direct_default_surface_addr(uint32_t page) {
    /* Keep direct-mode LCD buffers high in RAM, away from the loaded image,
       BSS, heap and SDK task stacks.  Four 240x320 8-bpp pages fit below the
       callback stub area at the top of the 8 MiB GP32 RAM map. */
    return GP32_RAM_BASE + 0x00780000u + (page & 3u) * (240u * 320u);
}

static uint32_t direct_surface_addr_for_bpp(const gp32_t *g, uint32_t page) {
    if (g && g->direct_fxe_bpp == 16u) return GP32_RAM_BASE + 0x00780000u + (page & 1u) * (240u * 320u * 2u);
    return direct_default_surface_addr(page);
}

static uint32_t direct_palette_hw_mirror_addr(const gp32_t *g) { return direct_stub_addr(g) + 0x100u; }
static uint32_t direct_palette_sw_addr(const gp32_t *g) { return direct_stub_addr(g) + 0x500u; }

static uint32_t direct_pack_logpal_entry(uint32_t r, uint32_t gr, uint32_t b, uint32_t flags) {
    uint32_t v = ((r & 0xf8u) << 8) | ((gr & 0xf8u) << 3) | ((b & 0xfbu) >> 2);
    if (flags) v |= 1u;
    return v & 0xffffu;
}

static uint32_t direct_sdk_palette_to_lcd(uint32_t value) {
    /* GPSDK GP_PALETTEENTRY matches the S3C2400 TFT 5:5:5:I palette word. */
    return value & 0xffffu;
}

static uint32_t direct_lcd_palette_to_sdk(uint32_t value) {
    return value & 0xffffu;
}

static void direct_write_palette_entry(gp32_t *g, uint32_t index, uint32_t value) {
    if (!g || index >= 256u) return;
    value &= 0xffffu;
    uint32_t lcd = direct_sdk_palette_to_lcd(value);
    s3c2400_write16(g->soc, 0x14a00400u + index * 4u, (uint16_t)lcd);
    uint32_t hw = direct_palette_hw_mirror_addr(g);
    if (direct_ram_range(g, hw + index * 4u, 4u)) s3c2400_write32(g->soc, hw + index * 4u, lcd);
}

static void direct_copy_palette32_to_lcd(gp32_t *g, uint32_t src_addr) {
    if (!g || !direct_ram_range(g, src_addr, 256u * 4u)) return;
    for (uint32_t i = 0; i < 256u; ++i) {
        uint32_t v = s3c2400_debug_read32(g->soc, src_addr + i * 4u) & 0xffffu;
        direct_write_palette_entry(g, i, v);
    }
    g->direct_fxe_palette_initialized = 1u;
}

static int direct_palette_looks_like_logpal(gp32_t *g, uint32_t pal_addr) {
    if (!g || !direct_ram_range(g, pal_addr, 16u * 4u)) return 0;
    unsigned nonzero_flags = 0, plausible_low = 0;
    for (uint32_t i = 0; i < 16u; ++i) {
        uint32_t v = s3c2400_debug_read32(g->soc, pal_addr + i * 4u);
        uint32_t flags = (v >> 24) & 0xffu;
        uint32_t r = v & 0xffu, gr = (v >> 8) & 0xffu, b = (v >> 16) & 0xffu;
        if (flags) nonzero_flags++;
        if (r <= 0xffu && gr <= 0xffu && b <= 0xffu) plausible_low++;
    }
    /* GP_LOGPALENTRY has byte order R,G,B,flags.  A nonzero flag byte in an
       application palette is a strong signal that the pointer is not an array
       of packed 5:5:5:1 words. */
    return plausible_low == 16u && nonzero_flags >= 8u;
}

static void direct_copy_logpal_to_lcd(gp32_t *g, uint32_t pal_addr) {
    if (!g || !direct_ram_range(g, pal_addr, 256u * 4u)) return;
    uint32_t sw = direct_palette_sw_addr(g);
    for (uint32_t i = 0; i < 256u; ++i) {
        uint32_t raw = s3c2400_debug_read32(g->soc, pal_addr + i * 4u);
        uint32_t v = direct_pack_logpal_entry(raw & 0xffu, (raw >> 8) & 0xffu, (raw >> 16) & 0xffu, (raw >> 24) & 0xffu);
        direct_write_palette_entry(g, i, v);
        if (direct_ram_range(g, sw + i * 4u, 4u)) s3c2400_write32(g->soc, sw + i * 4u, v);
    }
    g->direct_fxe_palette_addr = sw;
    g->direct_fxe_palette_initialized = 1u;
}

static void direct_copy_lcd_palette_to_ram(gp32_t *g, uint32_t dst_addr) {
    if (!g || !direct_ram_range(g, dst_addr, 256u * 4u)) return;
    for (uint32_t i = 0; i < 256u; ++i) {
        uint32_t v = s3c2400_debug_read32(g->soc, 0x14a00400u + i * 4u) & 0xffffu;
        s3c2400_write32(g->soc, dst_addr + i * 4u, direct_lcd_palette_to_sdk(v));
    }
}

static int direct_packed_palette_plausible(gp32_t *g, uint32_t pal_addr) {
    if (!g || !direct_ram_range(g, pal_addr, 256u * 4u)) return 0;
    unsigned high_clear = 0, varied = 0, pointer_like = 0;
    uint32_t first = s3c2400_debug_read32(g->soc, pal_addr + 0u) & 0xffffu;
    for (uint32_t i = 0; i < 256u; ++i) {
        uint32_t v = s3c2400_debug_read32(g->soc, pal_addr + i * 4u);
        if ((v & 0xffff0000u) == 0u) high_clear++;
        if ((v & 0xffffu) != first) varied++;
        if ((v & 0xff000000u) == GP32_RAM_BASE || ((v & 0xff000000u) == 0x0c000000u)) pointer_like++;
    }
    return varied >= 16u && high_clear >= 224u && pointer_like < 8u;
}

static void direct_realize_software_palette_from(gp32_t *g, uint32_t pal_addr, int allow_direct_pointer) {
    if (!g) return;
    uint32_t sw = 0u;

    /* GPSDK exposes two distinct palette paths.  GpPaletteRealize and the
       synchronous/asynchronous realize helpers pass stale scratch registers
       through SWI #0x16; commercial code must therefore realize the firmware
       logical palette returned by the palette-address BIOS service, not any
       incidental RAM value left in r2/r3.  A few direct-HLE loaders did pass a
       literal 256-entry palette pointer, so keep that path only when the
       candidate looks like packed 5:5:5:1 palette data rather than a heap object
       or callback table. */
    if (allow_direct_pointer && direct_packed_palette_plausible(g, pal_addr)) sw = pal_addr;
    else sw = g->direct_fxe_palette_addr;
    if (!direct_ram_range(g, sw, 256u * 4u)) sw = direct_palette_sw_addr(g);
    if (direct_ram_range(g, sw, 256u * 4u)) {
        direct_copy_palette32_to_lcd(g, sw);
        g->direct_fxe_palette_addr = sw;
    }
}


static void direct_fill_default_palette(gp32_t *g) {
    if (!g) return;
    /* GP32 8-bpp examples frequently start with a NULL palette and later update
       individual entries.  Provide a deterministic RGB 3:3:2 palette instead
       of leaving the LCD palette black.  Entries are expanded to the S3C2400's
       5:5:5 packed palette format used by the renderer. */
    uint32_t sw = direct_palette_sw_addr(g);
    for (uint32_t i = 0; i < 256u; ++i) {
        uint32_t r3 = (i >> 5) & 7u;
        uint32_t g3 = (i >> 2) & 7u;
        uint32_t b2 = i & 3u;
        uint32_t r5 = (r3 << 2) | (r3 >> 1);
        uint32_t g5 = (g3 << 2) | (g3 >> 1);
        uint32_t b5 = (b2 << 3) | (b2 << 1) | (b2 >> 1);
        uint32_t v = (r5 << 11) | (g5 << 6) | (b5 << 1);
        direct_write_palette_entry(g, i, v);
        if (direct_ram_range(g, sw + i * 4u, 4u)) s3c2400_write32(g->soc, sw + i * 4u, v);
    }
    g->direct_fxe_palette_addr = sw;
    g->direct_fxe_palette_initialized = 1u;
}

static uint32_t direct_read_surface_bpp(gp32_t *g, uint32_t surf) {
    if (!direct_ram_range(g, surf, 28u)) return 0u;
    /* Official GPDRAWSURFACE offset +4 is bufflag, not bpp.  Older HLE builds
       abused that word as a bpp field; accept it only for backward-compatible
       hand-made surfaces, otherwise use the active graphics mode selected via
       GpGraphicModeSet. */
    uint32_t maybe_bpp = s3c2400_debug_read32(g->soc, surf + 4u);
    if (maybe_bpp == 8u || maybe_bpp == 16u) return maybe_bpp;
    return (g->direct_fxe_bpp == 16u) ? 16u : 8u;
}

static uint32_t direct_read_surface_buffer(gp32_t *g, uint32_t surf) {
    if (!direct_ram_range(g, surf, 28u)) return 0u;
    uint32_t fb = s3c2400_debug_read32(g->soc, surf + 0u);
    uint32_t bpp = direct_read_surface_bpp(g, surf);
    uint32_t bw = s3c2400_debug_read32(g->soc, surf + 8u);
    uint32_t bh = s3c2400_debug_read32(g->soc, surf + 12u);
    if ((bpp == 8u || bpp == 16u) && bw >= 240u && bw <= 320u && bh >= 240u && bh <= 320u &&
        direct_ram_range(g, fb, 240u * 320u * (bpp == 16u ? 2u : 1u))) {
        return fb;
    }
    return 0u;
}

static void direct_set_lcd_8bpp(gp32_t *g, uint32_t fb_addr, uint32_t pal_addr, int follow_clock);

static void direct_fill_lcd_surface(gp32_t *g, uint32_t surf, uint32_t page) {
    if (!direct_ram_range(g, surf, 28u)) return;
    uint32_t fb = direct_surface_addr_for_bpp(g, page);
    g->direct_fxe_lcd_surface[page & 3u] = fb;
    /* Official GPDRAWSURFACE layout from gpgraphic.h:
       ptbuffer, bpp, buf_w, buf_h, ox, oy, o_buffer.  The buffer itself is the
       GP32 portrait LCD memory, while SDK drawing routines expose a 320x240
       landscape coordinate system.  Commercial SDK games request 16-bpp before
       asking for surfaces, so preserve that mode here instead of forcing an
       8-bpp descriptor. */
    direct_write32_if_ram(g, surf + 0u, fb);
    direct_write32_if_ram(g, surf + 4u, (g->direct_fxe_bpp == 16u) ? 16u : 8u);
    direct_write32_if_ram(g, surf + 8u, 320u);
    direct_write32_if_ram(g, surf + 12u, 240u);
    direct_write32_if_ram(g, surf + 16u, 0u);
    direct_write32_if_ram(g, surf + 20u, 0u);
    direct_write32_if_ram(g, surf + 24u, fb);
}



typedef struct direct_surface_stats {
    uint32_t checksum;
    uint32_t nonzero;
    uint32_t different_from_first;
    uint32_t transitions;
} direct_surface_stats_t;

static direct_surface_stats_t direct_surface_stats(gp32_t *g, uint32_t addr) {
    direct_surface_stats_t st;
    memset(&st, 0, sizeof(st));
    st.checksum = 2166136261u;
    if (!g || !direct_ram_range(g, addr, 240u * 320u)) return st;
    uint8_t first = 0, prev = 0;
    for (uint32_t off = 0; off < 240u * 320u; ++off) {
        uint32_t ba = (addr + off) & ~3u;
        uint32_t bw = s3c2400_debug_read32(g->soc, ba);
        uint8_t v = (uint8_t)(bw >> (((addr + off) & 3u) * 8u));
        if (off == 0u) first = v;
        else if (v != prev) st.transitions++;
        if (v) st.nonzero++;
        if (v != first) st.different_from_first++;
        st.checksum ^= v;
        st.checksum *= 16777619u;
        prev = v;
    }
    st.checksum ^= st.nonzero * 2654435761u;
    st.checksum ^= st.different_from_first * 2246822519u;
    st.checksum ^= st.transitions * 3266489917u;
    return st;
}

static uint32_t direct_surface_score(const direct_surface_stats_t *st) {
    if (!st || st->different_from_first == 0u) return 0u;
    /* A fully cleared white or black page has a large nonzero count but no
       picture content.  Score actual image structure instead. */
    uint32_t score = st->different_from_first + st->transitions * 4u;
    if (score < st->different_from_first) score = UINT32_MAX;
    return score;
}

static void direct_select_visible_surface(gp32_t *g) {
    if (!g || !g->direct_fxe_mode || g->direct_fxe_bpp != 8u) return;
    if (g->direct_fxe_lcd_explicit) return;
    uint32_t cur_score = 0u;
    if (g->direct_fxe_fb_addr) {
        direct_surface_stats_t cur = direct_surface_stats(g, g->direct_fxe_fb_addr);
        cur_score = direct_surface_score(&cur);
    }

    unsigned best = 4u;
    uint32_t best_score = 0u;
    int best_changed = 0;
    int prefer_changed = (cur_score == 0u);
    for (unsigned i = 0; i < 4u; ++i) {
        uint32_t addr = g->direct_fxe_lcd_surface[i];
        direct_surface_stats_t st = direct_surface_stats(g, addr);
        uint32_t score = direct_surface_score(&st);
        int changed = (st.checksum != g->direct_fxe_surface_checksum[i]);
        g->direct_fxe_surface_checksum[i] = st.checksum;
        if (score == 0u) continue;
        int take = 0;
        if (best == 4u) {
            take = 1;
        } else if (prefer_changed) {
            /* If the current LCD page is blank, use the best changed page, but
               do not let a low-content scratch/transition page win merely
               because it changed later. */
            if (changed != best_changed) take = changed;
            else if (score > best_score) take = 1;
        } else if (score > best_score) {
            /* Once a coherent LCD page is visible, choose by content quality
               rather than by recency. Some GPSDK programs update work pages during blits;
               choosing every changed page caused partially composed frames. */
            take = 1;
        }
        if (take) {
            best = i;
            best_score = score;
            best_changed = changed;
        }
    }
    if (best != 4u && g->direct_fxe_lcd_surface[best] != g->direct_fxe_fb_addr) {
        direct_set_lcd_8bpp(g, g->direct_fxe_lcd_surface[best], 0u, 0);
    }
}

/*
 * LCD controller state of the retail firmware.  Every graphics-mode switch in
 * the BIOS (ROM 0x1804) programs the panel from the same words: ROM
 * 0x1868-0x18ec composes and writes LCDCON1-5 (CLKVAL comes from the current
 * clock through ROM 0x1fd4 and is 3 at the BIOS's own clock) and ROM
 * 0x1a48-0x1a58 clears ENVID, writes LCDSADDR1-3 and sets ENVID again.  A title
 * starts with them in place.  The 8 bpp words were read back at the first
 * instruction of the game in BIOS boots (BIOS 1.6.6: Her Knights, Blue Angelo,
 * Little Wizard; 2003-05-21 BIOS: Pinball Dreams).  The 16 bpp words are the
 * ones the same routine writes for the BIOS's own 16 bpp screens (BPP code 0xc,
 * LCDCON5 low nibble 1); they were not read back at a 16 bpp game.
 *
 *             8 bpp       16 bpp
 *   LCDCON1   0x377       0x379       TFT, CLKVAL 3, ENVID; LINECNT reads back live
 *   LCDCON2   0x014fc081  same        VBPD 1, 320 lines, VFPD 2, VSPW 1
 *   LCDCON3   0x0030ef02  same        HBPD 6, 240 pixels, HFPD 2
 *   LCDCON4   0x00000004  same        HSPW 4
 *   LCDCON5   0x702       0x701       BSWP / HWSWP; the status bits are read-only
 *   LCDSADDR3 0x78        0xf0        halfwords per line
 *
 * Direct mode used to program a synthetic non-TFT panel (CLKVAL 0, no porches)
 * instead.  Titles read these registers for themselves: Pinball Dreams waits on
 * LCDCON1 LINECNT before it calls any display service and derives its TIMER4
 * frame period from LCDCON1-4, so a panel that is off or timed differently from
 * the retail one leaves it spinning in its palette-fade loop.
 */
#define DIRECT_LCDCON2_RETAIL       0x014fc081u
#define DIRECT_LCDCON3_RETAIL       0x0030ef02u
#define DIRECT_LCDCON4_RETAIL       0x00000004u
#define DIRECT_LCDCON5_RETAIL_8BPP  0x00000702u

/* CLKVAL (ROM 0x1fd4): the HCLK of the firmware's clock block divided by
 * 10,006,200, rounded up from a quarter, minus one - 3 at the BIOS's 33.9 MHz,
 * 4 at 48 MHz, 5 at 59.25 MHz.  Only a graphics-mode switch (ROM 0x1804, reached
 * by GpGraphicModeSet and the reinit service) recomputes it.  Every other display
 * service leaves the divider in LCDCON1 alone, so a title that changed the clock
 * after its last mode switch keeps the old one: BIOS boots show 59.2 Hz panels at
 * 59.25 MHz for titles that switch modes after the clock change (Again,
 * Astonishia Story R, Tomak) and 88.8 Hz for the ones that do not (Dungeon &
 * Guarder, Kimchiman, Woody & Kunta).  A live LCDCON1 that is not a TFT setup
 * with a divider (a fresh load, a guest's STN or CLKVAL 0 program) takes the
 * computed value. */
static uint32_t direct_lcdcon1(const gp32_t *g, uint32_t bppmode, int follow_clock) {
    uint32_t live = s3c2400_debug_read32(g->soc, 0x14a00000u);
    uint32_t clkval = GP32_BITS(live, 17, 8);
    if (follow_clock || GP32_BITS(live, 6, 5) != 3u || clkval == 0u) {
        const uint32_t unit = 10006200u;
        uint32_t hclk = s3c2400_hclk_hz(g->soc);
        uint32_t n = hclk / unit + (hclk % unit >= unit / 4u ? 1u : 0u);
        clkval = n > 1u ? n - 1u : 1u; /* the ROM's CLKVAL 0 is outside the TFT range */
    }
    return 1u | (bppmode << 1) | (3u << 5) | (clkval << 8);
}

static void direct_write_lcd_timing(gp32_t *g) {
    s3c2400_write32(g->soc, 0x14a00004u, DIRECT_LCDCON2_RETAIL);
    s3c2400_write32(g->soc, 0x14a00008u, DIRECT_LCDCON3_RETAIL);
    s3c2400_write32(g->soc, 0x14a0000cu, DIRECT_LCDCON4_RETAIL);
}

static void direct_set_lcd_16bpp(gp32_t *g, uint32_t fb_addr, int follow_clock) {
    if (!g || !direct_ram_range(g, fb_addr, 240u * 320u * 2u)) return;
    g->direct_fxe_fb_addr = fb_addr;
    direct_update_stub_framebuffer(g);
    uint32_t start = fb_addr >> 1;
    uint32_t end = (fb_addr + 240u * 320u * 2u) >> 1;
    direct_write_lcd_timing(g);
    /* BIOS/GPSDK 16-bpp surfaces use LCDCON5 HWSWP.  Without it, the LCD DMA
       consumes each 32-bit word in the wrong halfword order.  Latin/UI shapes
       can still look mostly plausible, but Hangul glyphs become visually
       substituted or scrambled compared with a real BIOS boot. */
    s3c2400_write32(g->soc, 0x14a00010u, 0x00000701u);
    s3c2400_write32(g->soc, 0x14a00014u, start);
    s3c2400_write32(g->soc, 0x14a00018u, end & 0x001fffffu);
    s3c2400_write32(g->soc, 0x14a0001cu, 240u);
    g->direct_fxe_bpp = 16u;
    s3c2400_write32(g->soc, 0x14a00000u, direct_lcdcon1(g, 0x0cu, follow_clock));
}

static void direct_set_lcd_8bpp(gp32_t *g, uint32_t fb_addr, uint32_t pal_addr, int follow_clock) {
    /* Direct-loaded GPSDK programs do not have the BIOS display service behind
       SWI #0x16.  This installs an 8-bit TFT LCD view over the app's active
       portrait framebuffer and copies the caller-supplied 256-entry GP32
       palette into the S3C2400 LCD palette registers. */
    if (!g || !direct_ram_range(g, fb_addr, 240u * 320u)) return;
    g->direct_fxe_fb_addr = fb_addr;
    direct_update_stub_framebuffer(g);
    uint32_t start = fb_addr >> 1;
    uint32_t end = (fb_addr + 240u * 320u) >> 1;
    direct_write_lcd_timing(g);
    s3c2400_write32(g->soc, 0x14a00010u, DIRECT_LCDCON5_RETAIL_8BPP);
    s3c2400_write32(g->soc, 0x14a00014u, start);
    s3c2400_write32(g->soc, 0x14a00018u, end & 0x001fffffu);
    s3c2400_write32(g->soc, 0x14a0001cu, 120u);
    g->direct_fxe_bpp = 8u;
    if (direct_ram_range(g, pal_addr, 256u * 4u)) {
        uint32_t sw = direct_palette_sw_addr(g);
        if (direct_palette_looks_like_logpal(g, pal_addr)) {
            direct_copy_logpal_to_lcd(g, pal_addr);
        } else {
            g->direct_fxe_palette_addr = sw;
            for (uint32_t i = 0; i < 256u; ++i) {
                uint32_t v = s3c2400_debug_read32(g->soc, pal_addr + i * 4u) & 0xffffu;
                direct_write_palette_entry(g, i, v);
                if (direct_ram_range(g, sw + i * 4u, 4u)) s3c2400_write32(g->soc, sw + i * 4u, v);
            }
            g->direct_fxe_palette_initialized = 1u;
        }
    } else if (direct_ram_range(g, pal_addr, 256u * 2u)) {
        uint32_t sw = direct_palette_sw_addr(g);
        g->direct_fxe_palette_addr = sw;
        for (uint32_t i = 0; i < 256u; ++i) {
            uint32_t a = pal_addr + i * 2u;
            uint32_t raw = s3c2400_debug_read32(g->soc, a & ~3u);
            uint32_t v = ((a & 2u) ? (raw >> 16) : raw) & 0xffffu;
            direct_write_palette_entry(g, i, v);
            if (direct_ram_range(g, sw + i * 4u, 4u)) s3c2400_write32(g->soc, sw + i * 4u, v);
        }
        g->direct_fxe_palette_initialized = 1u;
    } else if (!g->direct_fxe_palette_initialized) {
        direct_fill_default_palette(g);
    }
    s3c2400_write32(g->soc, 0x14a00000u, direct_lcdcon1(g, 0x0bu, follow_clock));
}

/* The state the firmware leaves for a title it has just started: 8 bpp over the
 * default surface with the panel enabled.  Only controller registers change;
 * the HLE's own mode bookkeeping stays "not selected yet" until the title asks. */
static void direct_init_lcd_handoff(gp32_t *g) {
    uint32_t fb = direct_default_surface_addr(0u);
    direct_write_lcd_timing(g);
    s3c2400_write32(g->soc, 0x14a00010u, DIRECT_LCDCON5_RETAIL_8BPP);
    s3c2400_write32(g->soc, 0x14a00014u, fb >> 1);
    s3c2400_write32(g->soc, 0x14a00018u, ((fb + 240u * 320u) >> 1) & 0x001fffffu);
    s3c2400_write32(g->soc, 0x14a0001cu, 120u);
    s3c2400_write32(g->soc, 0x14a00000u, direct_lcdcon1(g, 0x0bu, 1));
}

static void direct_fast_load_reset(gp32_t *g) {
    g->fast_load_active = g->fast_load_streak = g->fast_load_hash_valid = 0u;
    g->fast_load_recent_audio = 0u;
    g->fast_load_mark = s3c2400_smc_bytes_read(g->soc);
    g->fast_load_hash = 0u;
}

static void direct_reset_hle_runtime(gp32_t *g, int preserve_hle_options) {
    if (!g) return;
    direct_fast_load_reset(g);
    memset(&g->direct_callback, 0, sizeof(g->direct_callback));
    memset(&g->direct_tick, 0, sizeof(g->direct_tick));
    memset(&g->direct_vblank_time, 0, sizeof(g->direct_vblank_time));
    g->direct_vblank_wait_requested = 0;
    memset(g->direct_hle_gpos_timer_epoch, 0, sizeof(g->direct_hle_gpos_timer_epoch));
    g->direct_hle_pending_volume = 0;
    g->direct_cpu_running = 0;
    uint32_t saved_rate_override = preserve_hle_options ? g->direct_hle_audio_rate_override : 0u;
    memset(g->direct_fpk_handles, 0, sizeof(g->direct_fpk_handles));
    g->direct_hle_file_open_addr = 0;
    g->direct_hle_file_read_addr[0] = 0;
    g->direct_hle_file_read_addr[1] = 0;
    g->direct_hle_file_close_addr = 0;
    g->direct_hle_file_size_addr = 0;
    g->direct_hle_file_seek_addr = 0;
    g->direct_hle_pathbuf_addr = 0;
    g->direct_hle_sound_dispatch_addr = 0;
    g->direct_hle_sound_table_addr = 0;
    g->direct_hle_sound_play_addr = 0;
    g->direct_hle_sound_state_addr = 0;
    g->direct_hle_pcm_env_addr = 0;
    g->direct_hle_pcm_init_addr = 0;
    g->direct_hle_pcm_play_addr = 0;
    g->direct_hle_pcm_stop_addr = 0;
    g->direct_hle_pcm_remove_addr = 0;
    g->direct_hle_pcm_lock_addr = 0;
    g->direct_hle_pcm_only_kill_addr = 0;
    g->direct_hle_pcm_initialized = 0;
    g->direct_hle_pcm_sr = 0;
    g->direct_hle_pcm_bit_count = 0;
    g->direct_hle_pcm_rate = 0;
    g->direct_hle_pcm_stereo = 0;
    g->direct_hle_pcm_bits = 16;
    g->direct_hle_pcm_active = 0;
    g->direct_hle_pcm_src_addr = 0;
    g->direct_hle_pcm_size_bytes = 0;
    g->direct_hle_pcm_pos_bytes = 0;
    g->direct_hle_pcm_repeat = 0;
    g->direct_hle_pcm_accum = 0;
    memset(g->direct_hle_pcm_ch, 0, sizeof(g->direct_hle_pcm_ch));
    g->direct_hle_sdk_sndmixedbuf_addr = 0;
    g->direct_hle_sdk_sndsrcexist_addr = 0;
    g->direct_hle_sdk_pcm_workidx_addr = 0;
    g->direct_hle_sdk_sndmixer_addr = 0;
    g->direct_hle_sdk_mixbuf0_addr = 0;
    g->direct_hle_sdk_mixbuf1_addr = 0;
    g->direct_hle_sdk_mixbuf_bytes = 0;
    g->direct_hle_sdk_rate = 0;
    g->direct_hle_sdk_accum = 0;
    g->direct_hle_sdk_last_submit_cycle = 0;
    g->direct_hle_sdk_timer_accum = 0;
    g->direct_hle_sdk_submitted_frames = 0;
    g->direct_hle_sdk_timer_table_addr = 0;
    memset(g->direct_hle_gpos_timer, 0, sizeof(g->direct_hle_gpos_timer));
    g->direct_hle_gpos_timers_enabled = 0;
    g->direct_hle_gpos_task_first = 0;
    g->direct_hle_gpos_task_last = 0;
    g->direct_hle_gpos_scheduler_callback = 0;
    g->direct_hle_callback_returned = 0;
    g->direct_hle_callback_running = 0;
    g->direct_hle_audio_rate_override = saved_rate_override;
    g->direct_hle_audio_last_auto_rate = 0;
    g->direct_hle_audio_asset = NULL;
    g->direct_hle_audio_pos = 0;
    g->direct_hle_audio_size = 0;
    g->direct_hle_audio_rate = 0;
    g->direct_hle_audio_accum = 0;
    memset(g->direct_hle_asset_autoload, 0, sizeof(g->direct_hle_asset_autoload));
}

static void direct_clear_reset_image(gp32_t *g) {
    if (!g) return;
    free(g->direct_reset_image.payload);
    memset(&g->direct_reset_image, 0, sizeof(g->direct_reset_image));
    g->direct_reset_image_valid = 0;
    g->direct_reset_scan_file_hle = 0;
    g->direct_reset_init_smc_gpio = 0;
}

static int direct_store_reset_image(gp32_t *g, const fxe_image_t *img, int scan_file_hle, int init_smc_gpio) {
    if (!g || !img || !img->payload || !img->payload_size) return 0;
    uint8_t *copy = (uint8_t *)malloc(img->payload_size);
    if (!copy) return 0;
    memcpy(copy, img->payload, img->payload_size);
    direct_clear_reset_image(g);
    g->direct_reset_image = *img;
    g->direct_reset_image.payload = copy;
    g->direct_reset_image_valid = 1u;
    g->direct_reset_scan_file_hle = scan_file_hle ? 1u : 0u;
    g->direct_reset_init_smc_gpio = init_smc_gpio ? 1u : 0u;
    return 1;
}

static void direct_clear_fpk_assets(gp32_t *g) {
    if (!g) return;
    for (size_t i = 0; i < g->direct_fpk_asset_count; ++i) free(g->direct_fpk_assets[i].data);
    free(g->direct_fpk_assets);
    g->direct_fpk_assets = NULL;
    g->direct_fpk_asset_count = 0;
    direct_reset_hle_runtime(g, 0);
}

static int direct_read_cstr(gp32_t *g, uint32_t addr, char *out, size_t out_len) {
    if (!out || out_len == 0) return 0;
    out[0] = '\0';
    if (!direct_ram_range(g, addr, 1u)) return 0;
    size_t n = 0;
    while (n + 1u < out_len && direct_ram_range(g, addr + (uint32_t)n, 1u)) {
        uint32_t ba = (addr + (uint32_t)n) & ~3u;
        uint32_t bw = s3c2400_debug_read32(g->soc, ba);
        uint8_t c = (uint8_t)(bw >> (((addr + (uint32_t)n) & 3u) * 8u));
        if (!c) { out[n] = '\0'; return n != 0; }
        if (c < 0x20u || c > 0x7eu) break;
        out[n++] = (char)c;
    }
    out[n] = '\0';
    return n != 0;
}

static void direct_norm_path(const char *in, char *out, size_t out_len) {
    if (!out || out_len == 0) return;
    out[0] = '\0';
    if (!in) return;
    size_t j = 0;
    const char *s = in;
    if ((s[0] == 'g' || s[0] == 'G') && (s[1] == 'p' || s[1] == 'P') && s[2] == ':') s += 3;
    while (*s == '\\' || *s == '/') ++s;
    for (; *s && j + 1u < out_len; ++s) {
        char c = *s;
        if (c == '\\') c = '/';
        out[j++] = (char)tolower((unsigned char)c);
    }
    while (j && out[j - 1u] == '/') --j;
    out[j] = '\0';
}

static const fpk_asset_t *direct_find_fpk_asset(gp32_t *g, const char *path) {
    if (!g || !path || !path[0]) return NULL;
    char q[320];
    direct_norm_path(path, q, sizeof(q));
    if (!q[0]) return NULL;

    int q_has_dir = strchr(q, '/') != NULL;
    size_t lq = strlen(q);
    for (size_t i = 0; i < g->direct_fpk_asset_count; ++i) {
        char p[320];
        direct_norm_path(g->direct_fpk_assets[i].path, p, sizeof(p));
        if (!strcmp(p, q)) return &g->direct_fpk_assets[i];

        size_t lp = strlen(p);
        if (q_has_dir) {
            if (lp >= lq && !strcmp(p + lp - lq, q) && (lp == lq || p[lp - lq - 1u] == '/')) return &g->direct_fpk_assets[i];
            if (lq >= lp && !strcmp(q + lq - lp, p) && (lq == lp || q[lq - lp - 1u] == '/')) return &g->direct_fpk_assets[i];
        } else {
            const char *base = strrchr(p, '/');
            base = base ? base + 1 : p;
            if (!strcmp(base, q)) return &g->direct_fpk_assets[i];
        }
    }
    return NULL;
}


static int direct_trace_enabled(void) {
    static int initialized = 0;
    static int enabled = 0;
    if (!initialized) {
        enabled = getenv("GP32_DIRECT_TRACE") ? 1 : 0;
        initialized = 1;
    }
    return enabled;
}

static void direct_trace_file_path(gp32_t *g, const char *op, uint32_t path_addr, const fpk_asset_t *a) {
    if (!direct_trace_enabled()) return;
    char path[320] = {0};
    if (path_addr) direct_read_cstr(g, path_addr, path, sizeof(path));
    fprintf(stderr, "[direct-hle] %s pc=%08x lr=%08x path_addr=%08x path='%s' asset='%s' size=%zu\n",
            op ? op : "file", g && g->cpu ? arm920t_get_pc(g->cpu) : 0u, g && g->cpu ? arm920t_get_reg(g->cpu, 14) : 0u,
            path_addr, path, a ? a->path : "", a ? a->size : (size_t)0u);
}

static void direct_trace_file_io(gp32_t *g, const char *op, uint32_t h, uint32_t dst, uint32_t want, size_t pos, size_t n, const fpk_asset_t *a, uint32_t st) {
    if (!direct_trace_enabled()) return;
    fprintf(stderr, "[direct-hle] %s pc=%08x lr=%08x h=%u dst=%08x want=%u pos=%zu n=%zu st=%u asset='%s'\n",
            op ? op : "io", g && g->cpu ? arm920t_get_pc(g->cpu) : 0u, g && g->cpu ? arm920t_get_reg(g->cpu, 14) : 0u,
            h, dst, want, pos, n, st, a ? a->path : "");
}


static int direct_ascii_equal_nocase(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        ++a; ++b;
    }
    return *a == '\0' && *b == '\0';
}

static void direct_asset_basename_lower(const fpk_asset_t *a, char *out, size_t out_len) {
    if (!out || !out_len) return;
    out[0] = '\0';
    if (!a || !a->path[0]) return;
    const char *base = a->path;
    for (const char *p = a->path; *p; ++p) if (*p == '/' || *p == '\\') base = p + 1;
    size_t j = 0;
    while (base[j] && j + 1u < out_len) {
        out[j] = (char)tolower((unsigned char)base[j]);
        ++j;
    }
    out[j] = '\0';
}

static int direct_asset_is_gpg(const fpk_asset_t *a) {
    if (!a || !a->data || a->size <= 8u) return 0;
    char name[64];
    direct_asset_basename_lower(a, name, sizeof(name));
    size_t n = strlen(name);
    return n > 4u && strcmp(name + n - 4u, ".gpg") == 0 &&
           a->data[0] == 'g' && a->data[1] == 'p' && a->data[2] == 'g' && a->data[3] == ' ';
}

static void direct_note_asset_autoload(gp32_t *g, const fpk_asset_t *a) {
    if (!g || !direct_asset_is_gpg(a)) return;
    for (size_t i = 0; i < GP32_ARRAY_COUNT(g->direct_hle_asset_autoload); ++i) {
        if (g->direct_hle_asset_autoload[i].asset == a) return;
    }
    for (size_t i = 0; i < GP32_ARRAY_COUNT(g->direct_hle_asset_autoload); ++i) {
        if (!g->direct_hle_asset_autoload[i].asset || g->direct_hle_asset_autoload[i].copied) {
            g->direct_hle_asset_autoload[i].asset = a;
            g->direct_hle_asset_autoload[i].copied = 0u;
            g->direct_hle_asset_autoload[i].tries = 0u;
            return;
        }
    }
}

static int direct_buffer_looks_unloaded(gp32_t *g, uint32_t buf, uint32_t len) {
    if (!g || !len || !direct_ram_range(g, buf, len)) return 0;
    uint32_t sample = len < 256u ? len : 256u;
    uint32_t zero = 0u, ff = 0u;
    for (uint32_t i = 0; i < sample; ++i) {
        uint32_t ba = (buf + i) & ~3u;
        uint32_t bw = s3c2400_debug_read32(g->soc, ba);
        uint8_t c = (uint8_t)(bw >> (((buf + i) & 3u) * 8u));
        if (c == 0u) ++zero;
        if (c == 0xffu) ++ff;
    }
    return zero > (sample * 3u) / 4u || ff > (sample * 3u) / 4u;
}

static int direct_try_autoload_gpg_asset(gp32_t *g, const fpk_asset_t *a) {
    if (!g || !direct_asset_is_gpg(a)) return 0;
    char basename[64];
    direct_asset_basename_lower(a, basename, sizeof(basename));
    if (!basename[0]) return 0;
    uint32_t start = GP32_RAM_BASE;
    uint32_t end = g->direct_fxe_image_end;
    uint32_t ram_end = GP32_RAM_BASE + (uint32_t)s3c2400_ram_size(g->soc);
    if (end <= start || end > ram_end) end = ram_end;
    uint32_t payload_len = (uint32_t)(a->size - 8u);
    for (uint32_t saddr = start; saddr + 8u < end; ++saddr) {
        char lit[64];
        if (!direct_read_cstr(g, saddr, lit, sizeof(lit))) continue;
        if (!direct_ascii_equal_nocase(lit, basename)) continue;
        uint32_t back = (saddr > 0x28u) ? (saddr - 0x28u) : start;
        if (back < start) back = start;
        for (uint32_t p = saddr; p >= back + 4u; p -= 4u) {
            uint32_t slot_addr = s3c2400_debug_read32(g->soc, p - 4u);
            if (!direct_ram_range(g, slot_addr, 4u)) continue;
            uint32_t dst = s3c2400_debug_read32(g->soc, slot_addr);
            if (dst <= slot_addr || dst < end) break;
            if (!direct_ram_range(g, dst, payload_len)) break;
            if (!direct_buffer_looks_unloaded(g, dst, payload_len)) break;
            direct_copy_bytes_if_ram(g, dst, a->data + 8u, payload_len);
            if (direct_trace_enabled()) {
                fprintf(stderr, "[direct-hle] autoload/gpg asset='%s' literal=%08x slot=%08x dst=%08x n=%u\n",
                        a->path, saddr, slot_addr, dst, payload_len);
            }
            return 1;
        }
        saddr += (uint32_t)strlen(lit);
    }
    return 0;
}

static void direct_process_asset_autoload(gp32_t *g) {
    if (!g || !g->direct_fxe_mode || !g->direct_fpk_asset_count) return;
    for (size_t i = 0; i < GP32_ARRAY_COUNT(g->direct_hle_asset_autoload); ++i) {
        if (!g->direct_hle_asset_autoload[i].asset || g->direct_hle_asset_autoload[i].copied) continue;
        if (direct_try_autoload_gpg_asset(g, g->direct_hle_asset_autoload[i].asset)) {
            g->direct_hle_asset_autoload[i].copied = 1u;
        } else if (++g->direct_hle_asset_autoload[i].tries > 4096u) {
            g->direct_hle_asset_autoload[i].asset = NULL;
            g->direct_hle_asset_autoload[i].copied = 0u;
            g->direct_hle_asset_autoload[i].tries = 0u;
        }
    }
}

static const fpk_asset_t *direct_fpk_asset_from_cpu_path(gp32_t *g, uint32_t path_addr) {
    char path[320];

    /* Prefer the live string argument supplied by the intercepted open/size
       call.  The scanned SDK wrapper path-buffer pointer is only a fallback:
       some retail wrappers reuse one global filename buffer, and using it
       first can bind a new request to the previous filename.  Dyhard exposes
       this with title.pal: the stale buffer still names title.spr, causing the
       palette load to read sprite pixels as palette entries. */
    if (path_addr && direct_read_cstr(g, path_addr, path, sizeof(path))) {
        const fpk_asset_t *a = direct_find_fpk_asset(g, path);
        if (a) return a;
        for (uint32_t off = 0x20u; off <= 0x400u; off += 4u) {
            if (direct_read_cstr(g, path_addr + off, path, sizeof(path))) {
                a = direct_find_fpk_asset(g, path);
                if (a) return a;
            }
        }
    }
    if (g->direct_hle_pathbuf_addr && direct_read_cstr(g, g->direct_hle_pathbuf_addr, path, sizeof(path))) {
        const fpk_asset_t *a = direct_find_fpk_asset(g, path);
        if (a) return a;
    }
    return NULL;
}

static int direct_fpk_asset_is_sef(const fpk_asset_t *a, uint32_t *payload_size) {
    if (!a || a->size < 8u || !a->data) return 0;
    if (a->data[0] != 's' || a->data[1] != 'e' || a->data[2] != 'f' || a->data[3] != ' ') return 0;
    uint32_t n = gp32_ld32le(a->data + 4u);
    if (n > a->size - 8u) n = (uint32_t)(a->size - 8u);
    if (!n) return 0;
    if (payload_size) *payload_size = n;
    return 1;
}

static uint32_t direct_nearest_standard_audio_rate(uint32_t rate) {
    static const uint32_t rates[] = { 8000u, 11025u, 16000u, 22050u, 32000u, 44100u };
    uint32_t best = rates[0];
    uint32_t best_delta = rate > best ? rate - best : best - rate;
    for (size_t i = 1u; i < sizeof(rates) / sizeof(rates[0]); ++i) {
        uint32_t r = rates[i];
        uint32_t delta = rate > r ? rate - r : r - rate;
        if (delta < best_delta) {
            best = r;
            best_delta = delta;
        }
    }
    return best;
}

static uint32_t direct_hle_sef_playback_rate(gp32_t *g) {
    if (!g) return 22050u;
    if (g->direct_hle_audio_rate_override >= 4000u && g->direct_hle_audio_rate_override <= 192000u) return g->direct_hle_audio_rate_override;

    /* GPSDK's stream helper feeds SEF data through GpPcmPlay() as raw unsigned
       8-bit mono PCM.  The SEF header only carries "sef " + payload length;
       the playback rate comes from the SDK stream ring-buffer byte rate.
       OMG reads 1024 bytes every 50 ms, i.e. 20480 8-bit samples/s, which
       snaps to the GP32's 22050 Hz PCM mode. */
    if (g->soc && g->direct_hle_sound_state_addr && direct_ram_range(g, g->direct_hle_sound_state_addr, 0x58u)) {
        uint32_t ring_bytes = s3c2400_debug_read32(g->soc, g->direct_hle_sound_state_addr + 0x14u);
        uint32_t period_ms = s3c2400_debug_read32(g->soc, g->direct_hle_sound_state_addr + 0x54u);
        if (ring_bytes >= 128u && ring_bytes <= 65536u && period_ms >= 1u && period_ms <= 1000u) {
            uint64_t samples_per_sec = ((uint64_t)ring_bytes * 1000u + (uint64_t)period_ms / 2u) / (uint64_t)period_ms;
            if (samples_per_sec >= 4000u && samples_per_sec <= 96000u) {
                uint32_t rate = direct_nearest_standard_audio_rate((uint32_t)samples_per_sec);
                g->direct_hle_audio_last_auto_rate = rate;
                return rate;
            }
        }
    }

    return g->direct_hle_audio_last_auto_rate ? g->direct_hle_audio_last_auto_rate : 22050u;
}

static int direct_play_sef_asset(gp32_t *g, const fpk_asset_t *a) {
    uint32_t n = 0;
    if (!g || !g->soc || !direct_fpk_asset_is_sef(a, &n)) return 0;
    if (g->direct_hle_audio_asset == a && g->direct_hle_audio_pos < g->direct_hle_audio_size) return 1;
    g->direct_hle_audio_asset = a;
    g->direct_hle_audio_pos = 0;
    g->direct_hle_audio_size = n;
    g->direct_hle_audio_rate = direct_hle_sef_playback_rate(g);
    g->direct_hle_audio_accum = 0;
    return 1;
}

static int direct_play_sef_path(gp32_t *g, uint32_t path_addr) {
    char path[320];
    if (!g || !path_addr || !direct_read_cstr(g, path_addr, path, sizeof(path))) return 0;
    return direct_play_sef_asset(g, direct_find_fpk_asset(g, path));
}

static void direct_stop_sef(gp32_t *g) {
    if (!g) return;
    g->direct_hle_audio_asset = NULL;
    g->direct_hle_audio_pos = 0;
    g->direct_hle_audio_size = 0;
    g->direct_hle_audio_accum = 0;
}

static uint8_t direct_read8_if_ram(gp32_t *g, uint32_t addr) {
    if (!direct_ram_range(g, addr, 1u)) return 0u;
    uint32_t bw = s3c2400_debug_read32(g->soc, addr & ~3u);
    return (uint8_t)(bw >> ((addr & 3u) * 8u));
}


static int direct_is_gp32_shadow_ramp_context(gp32_t *g, uint32_t addr, uint8_t lo, uint8_t hi) {
    if (!direct_ram_range(g, addr, 256u)) return 0;
    unsigned in_range = 0u;
    unsigned top = 0u;
    unsigned transitions = 0u;
    uint8_t prev = direct_read8_if_ram(g, addr);
    for (uint32_t i = 0; i < 256u; ++i) {
        uint8_t v = direct_read8_if_ram(g, addr + i);
        if (v >= lo && v <= hi) in_range++;
        if (v == hi) top++;
        if (i && v != prev) transitions++;
        prev = v;
    }
    return in_range >= 248u && top <= 8u && transitions >= 8u;
}

static int direct_blue_angelo_shadow_lut_present(gp32_t *g) {
    if (!g) return 0;
    uint32_t lut = GP32_RAM_BASE + 0x747200u;
    if (!direct_is_gp32_shadow_ramp_context(g, lut - 0x130u, 0x36u, 0x40u)) return 0;
    if (!direct_is_gp32_shadow_ramp_context(g, lut, 0x71u, 0x78u)) return 0;
    return 1;
}

static int direct_shadow_index(uint8_t v) {
    return v >= 0x71u && v <= 0x77u;
}

static int direct_surface_has_shadow_neighbour(gp32_t *g, const uint8_t *pixels, uint32_t fb_addr, uint32_t width, uint32_t height, uint32_t x, uint32_t y) {
    for (int dy = -1; dy <= 1; ++dy) {
        int yy = (int)y + dy;
        if (yy < 0 || yy >= (int)height) continue;
        for (int dx = -1; dx <= 1; ++dx) {
            int xx = (int)x + dx;
            if ((dx == 0 && dy == 0) || xx < 0 || xx >= (int)width) continue;
            uint32_t offset = (uint32_t)yy * width + (uint32_t)xx;
            uint8_t v = pixels ? pixels[offset] : direct_read8_if_ram(g, fb_addr + offset);
            if (direct_shadow_index(v)) return 1;
        }
    }
    return 0;
}

static void direct_fix_gp32_additive_blend_shadow_pixels(gp32_t *g) {
    /*
     * Repair already-composited Blue Angelo additive shadow pixels.  The LUT
     * endpoint fix below prevents newly drawn shadow from selecting palette
     * slot 0x78, but savestates and frame snapshots can already contain 0x78
     * in the active 8-bpp surface.  Do not globally remap yellow: only when the
     * Blue Angelo paired shadow LUT is present, walk the active 240x320 8-bpp
     * LCD surface and convert 0x78 pixels that are connected to the purple
     * shadow ramp (0x71..0x77).  Ordinary yellow HUD/moon artwork remains 0x78
     * because it is not part of that shadow-ramp component.
     */
    if (!g || !direct_blue_angelo_shadow_lut_present(g)) return;
    uint32_t lcdcon1 = s3c2400_debug_read32(g->soc, 0x14a00000u);
    uint32_t bppmode = GP32_BITS(lcdcon1, 4, 1);
    if (bppmode != 0x0bu) return;
    uint32_t lcdcon2 = s3c2400_debug_read32(g->soc, 0x14a00004u);
    uint32_t lcdcon3 = s3c2400_debug_read32(g->soc, 0x14a00008u);
    uint32_t height = GP32_BITS(lcdcon2, 23, 14) + 1u;
    uint32_t width = GP32_BITS(lcdcon3, 18, 8) + 1u;
    if (width == 0u || height == 0u || width > 240u || height > 320u) return;
    uint32_t fb_addr = s3c2400_debug_read32(g->soc, 0x14a00014u) << 1;
    if (!direct_ram_range(g, fb_addr, width * height)) return;
    /* The complete surface is ordinary RAM. Resolve it once for this call;
     * state loading can replace RAM, so do not cache the pointer. Keep the
     * ordered, in-place eight-pass repair and all LUT/LCD guards unchanged.
     * The old byte reader uses aligned 32-bit loads. Retain it if a custom
     * RAM size ends partway through the surface's last aligned word. */
    arm_bus_t bus = s3c2400_get_bus(g->soc);
    uint32_t span = ((fb_addr + width * height + 3u) & ~3u) - fb_addr;
    uint8_t *pixels = bus.fastmem(bus.user, fb_addr, span, 1);

    for (unsigned pass = 0; pass < 8u; ++pass) {
        uint32_t changed = 0u;
        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
                uint32_t offset = y * width + x;
                uint8_t v = pixels ? pixels[offset] : direct_read8_if_ram(g, fb_addr + offset);
                if (v != 0x78u) continue;
                if (!direct_surface_has_shadow_neighbour(g, pixels, fb_addr, width, height, x, y)) continue;
                if (pixels) pixels[offset] = 0x77u;
                else direct_write8_if_ram(g, fb_addr + offset, 0x77u);
                changed++;
            }
        }
        if (!changed) break;
    }
}

static void direct_fix_gp32_additive_blend_shadow_endpoint(gp32_t *g) {
    /*
     * Additive-blend shadow-LUT compatibility guard.  v55 applied this data-shaped Blue
     * Angelo endpoint correction to a single broad byte-table shape.  That made
     * unrelated BIOS/game runtime data at the same RAM address eligible for
     * mutation and caused colour regressions outside the shadow test case.
     *
     * Do not key this to the current boot path: savestates and BIOS-launched
     * gameplay can legitimately contain the same table.  Instead, require the
     * neighbouring luminance/shadow table pair used by the same routine before
     * touching RAM.  The LCD palette/register model is left untouched; only the
     * runtime blend lookup endpoint is corrected when the paired LUTs prove this
     * is the additive-blend shadow table.
     */
    if (!g) return;
    uint32_t lut = GP32_RAM_BASE + 0x747200u;
    if (!direct_blue_angelo_shadow_lut_present(g)) return;
    for (uint32_t i = 0; i < 256u; ++i) {
        if (direct_read8_if_ram(g, lut + i) == 0x78u) direct_write8_if_ram(g, lut + i, 0x77u);
    }
}

static uint32_t direct_read32_if_ram(gp32_t *g, uint32_t addr) {
    if (!direct_ram_range(g, addr, 4u)) return 0u;
    return s3c2400_debug_read32(g->soc, addr);
}

/* Pinball's captured startup sequence writes VC=63, then starts its IIS mixer
 * without a later volume restore, even with MUSIC ON. The modeled DAC correctly
 * mutes VC=63. This optional compatibility fix changes only the exact resident
 * initialization routine, retaining the game's software SFX/music controls.
 * It is not a claim about unmeasured original GP32 DAC/board behavior.
 *
 * RAM records installation. A previously initialized state needs the matching
 * codec tuple repaired once; subsequent guest volume/mute writes are respected.
 * Neither the card nor queued PCM is rewritten. */
#define GP32_PINBALL_FIX_FUNC 0x0c22a464u
#define GP32_PINBALL_FIX_PC   0x0c22a4e8u
#define GP32_PINBALL_FIX_ORIG 0xe3a0003fu /* mov r0, #63 */
#define GP32_PINBALL_FIX_NEW  0xe3a00000u /* mov r0, #0 */

static const uint32_t direct_pinball_fix_code[] = {
    0xe1a0c00du, 0xe92dd830u, 0xe59f40f8u, 0xe3a02a02u, 0xe3a01000u, 0xe59f50f0u,
    0xe1a00004u, 0xe24cb004u, 0xeb000cbbu, 0xe59f10e4u, 0xe3a0c004u, 0xe585c000u,
    0xe5910000u, 0xe59fc0d8u, 0xe3c02c0eu, 0xe382ec06u, 0xe581e000u, 0xe59c0000u,
    0xe241e004u, 0xe3803c0eu, 0xe58c3000u, 0xe59ec000u, 0xe3a00016u, 0xe3cc273fu,
    0xe382c715u, 0xe58ec000u, 0xebffff4eu, 0xe3a01001u, 0xe3a00008u, 0xebffff8bu,
    0xe3a00014u, 0xebffff49u, 0xe3a01001u, 0xe3a0003fu, 0xebffff86u, 0xe3a00014u,
    0xebffff44u, 0xe3a00090u, 0xe3a01001u, 0xebffff81u, 0xe59fc070u, 0xe59f3070u,
    0xe3a00042u, 0xe3a01026u, 0xe5830000u, 0xe58c1000u, 0xe59f1060u, 0xe2802057u,
    0xe2430004u, 0xe3a03c0au, 0xe5802000u, 0xe5813000u, 0xe59c2000u, 0xe59f3048u,
    0xe3820001u, 0xe58c0000u, 0xe59fc040u, 0xe59f1040u, 0xe59f2040u, 0xe28c0004u,
    0xe58c3000u, 0xe5814000u, 0xe5802000u, 0xe3a00002u, 0xe5850000u, 0xe91ba830u,
    0x0c432d92u, 0x14600058u, 0x15600030u, 0x15600034u, 0x15508000u, 0x15508008u,
    0x1550800cu, 0x75508010u, 0x14600044u, 0x14600040u, 0x50a00800u,
};

static void direct_pinball_fix_update(gp32_t *g, int flush) {
    if (!g || !g->cpu) return;
    uint32_t word = direct_read32_if_ram(g, GP32_PINBALL_FIX_PC);
    int disabling = g->adpcm_fix_disabled && word == GP32_PINBALL_FIX_NEW;
    int installing = !g->adpcm_fix_disabled && word == GP32_PINBALL_FIX_ORIG;
    if (!disabling && !installing) return;
    uint32_t count = (uint32_t)GP32_ARRAY_COUNT(direct_pinball_fix_code);
    if (!direct_ram_range(g, GP32_PINBALL_FIX_FUNC, count * 4u)) return;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t addr = GP32_PINBALL_FIX_FUNC + i * 4u;
        if (addr != GP32_PINBALL_FIX_PC &&
            direct_read32_if_ram(g, addr) != direct_pinball_fix_code[i]) return;
    }
    direct_write32_if_ram(g, GP32_PINBALL_FIX_PC,
                         installing ? GP32_PINBALL_FIX_NEW : GP32_PINBALL_FIX_ORIG);
    if (installing)
        (void)s3c2400_audio_replace_codec_volume(g->soc, 0x08u, 0x90u, 63u, 0u);
    if (flush) arm920t_flush_jit(g->cpu);
}

/*
 * Astonishia Story R's IMA-ADPCM decoder (0x0c0112a4) chooses the low or high
 * nibble from the parity of the stream-wide sample counter (r7) instead of the
 * position inside the 505-sample block. Every odd block therefore decodes with
 * swapped nibbles, which is the ~12 clicks/s heard on the title screen; real
 * hardware runs the same code. When this exact routine is resident, its
 * "tst r7, #1" is replaced by a host trap that tests the in-block parity. The
 * card image is never touched.
 *
 * Both parities read one byte per two samples, but they read at different
 * samples inside an odd block. Switching in the middle of a block is safe only
 * at an odd in-block position, where both have consumed the same bytes. The
 * trap word records which parity the block in progress uses, so it is carried
 * by savestates and RAM stays the only source of truth.
 */
#define GP32_ADPCM_FIX_FUNC 0x0c0112a4u
#define GP32_ADPCM_FIX_PC   0x0c011364u
#define GP32_ADPCM_FIX_TST  0xe3170001u  /* tst r7, #1 */
#define GP32_ADPCM_FIX_ORIG 0xef47a0d0u  /* trap, block uses the game's parity */
#define GP32_ADPCM_FIX_NEW  0xef47a0d1u  /* trap, block uses in-block parity */

static const uint32_t direct_adpcm_fix_code[] = {
    0xe92d4ff0u, 0xe24dd008u, 0xe1a05000u, 0xe3a00000u, 0xe5cd0000u, 0xe5917010u,
    0xe1a04001u, 0xe087a002u, 0xe157000au, 0xe591901cu, 0xe3a06000u, 0xe3a08000u,
    0xaa00005bu, 0xe1570009u, 0x1a000016u, 0xe5940018u, 0xe28d1004u, 0xe0809007u,
    0xe5940014u, 0xe3a02004u, 0xe1590000u, 0x31a00009u, 0xe1a09000u, 0xe5940000u,
    0xebffc278u, 0xe1dd00f4u, 0xe51f1764u, 0xe2877001u, 0xe2806902u, 0xe1a00806u,
    0xe1a00840u, 0xe0c500b2u, 0xe5911004u, 0xe3510000u, 0x10c500b2u, 0xe5dd0006u,
    0xe1a08180u, 0xea000004u, 0xe5d40020u, 0xe5946024u, 0xe5cd0000u, 0xe594002cu,
    0xe1a08180u, 0xe159000au, 0xa1a0b00au, 0xb1a0b009u, 0xe157000bu, 0xaa000036u,
    0xe3170001u, 0x05dd0000u, 0x01a00220u, 0x05cd0000u, 0x0a000003u, 0xe1a0100du,
    0xe3a02001u, 0xe5940000u, 0xebffc258u, 0xe5dd0000u, 0xe51fc0fcu, 0xe51f37e8u,
    0xe2002007u, 0xe1820008u, 0xe1a01080u, 0xe19c00f1u, 0xe5933004u, 0xe3530000u,
    0x0a000011u, 0xe5dd3000u, 0xe3130008u, 0xe18820c2u, 0xe1a02082u, 0xe19c20f2u,
    0xe08220c0u, 0x0a000004u, 0xe04620c2u, 0xe3520000u, 0xb3a02000u, 0xe0c520b2u,
    0xea000005u, 0xe3a03801u, 0xe2433001u, 0xe08620c2u, 0xe1520003u, 0xc1a02003u,
    0xe0c520b2u, 0xe5dd2000u, 0xe3120008u, 0x0a000003u, 0xe0466000u, 0xe3560000u,
    0xb3a06000u, 0xea000004u, 0xe3a03801u, 0xe2433001u, 0xe0866000u, 0xe1560003u,
    0xc1a06003u, 0xe51f0194u, 0xe0c560b2u, 0xe2877001u, 0xe157000bu, 0xe19080f1u,
    0xbaffffc8u, 0xe157000au, 0xbaffffa3u, 0xe5847010u, 0xe5dd0000u, 0xe5c40020u,
    0xe1a001c8u, 0xe584002cu, 0xe5846024u, 0xe584901cu, 0xe28dd008u, 0xe8bd8ff0u,
};

static void direct_update_swi_hook(gp32_t *g);

static int direct_adpcm_fix_is_trap(uint32_t w) {
    return w == GP32_ADPCM_FIX_ORIG || w == GP32_ADPCM_FIX_NEW;
}

static int direct_adpcm_fix_resident(gp32_t *g) {
    const uint32_t n = (uint32_t)(sizeof(direct_adpcm_fix_code) / sizeof(direct_adpcm_fix_code[0]));
    if (!direct_ram_range(g, GP32_ADPCM_FIX_FUNC, n * 4u)) return 0;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t addr = GP32_ADPCM_FIX_FUNC + i * 4u;
        uint32_t w = s3c2400_debug_read32(g->soc, addr);
        if (addr == GP32_ADPCM_FIX_PC ? (w != GP32_ADPCM_FIX_TST && !direct_adpcm_fix_is_trap(w))
                                      : w != direct_adpcm_fix_code[i]) return 0;
    }
    return 1;
}

/* Cheap per-run check: one word read unless the routine has just appeared.
 * A freshly installed trap starts in the game's parity because the block in
 * progress was decoded by the original instruction. With fixes disabled the
 * trap switches back at a safe position, then the original word returns.
 * Pass flush = 0 only when the JIT cache is known to be empty. */
static void direct_adpcm_fix_update(gp32_t *g, int flush) {
    if (!g || !g->cpu) return;
    uint32_t w = direct_read32_if_ram(g, GP32_ADPCM_FIX_PC);
    int patched = direct_adpcm_fix_is_trap(w);
    if (w == GP32_ADPCM_FIX_ORIG && g->adpcm_fix_disabled) {
        direct_write32_if_ram(g, GP32_ADPCM_FIX_PC, GP32_ADPCM_FIX_TST);
        if (flush) arm920t_flush_jit(g->cpu);
        patched = 0;
    } else if (w == GP32_ADPCM_FIX_TST && !g->adpcm_fix_disabled && direct_adpcm_fix_resident(g)) {
        direct_write32_if_ram(g, GP32_ADPCM_FIX_PC, GP32_ADPCM_FIX_ORIG);
        if (flush) arm920t_flush_jit(g->cpu);
        patched = 1;
    }
    if (patched != g->adpcm_fix_patched) {
        g->adpcm_fix_patched = (uint8_t)patched;
        direct_update_swi_hook(g);
    }
}

static int direct_adpcm_fix_swi(gp32_t *g, arm920t_t *cpu, uint32_t imm, uint32_t pc) {
    if (pc != GP32_ADPCM_FIX_PC || (imm | 1u) != (GP32_ADPCM_FIX_NEW & 0x00ffffffu)) return 0;
    /* The JIT may hold the decoded word; RAM holds the current mode. */
    uint32_t word = direct_read32_if_ram(g, GP32_ADPCM_FIX_PC);
    if (!direct_adpcm_fix_is_trap(word)) return 0;
    uint32_t state = arm920t_get_reg(cpu, 4);
    uint32_t pos = arm920t_get_reg(cpu, 7);
    uint32_t end = arm920t_get_reg(cpu, 9);  /* next header, clipped to the stream length */
    uint32_t spb = direct_read32_if_ram(g, state + 0x18u);
    uint32_t total = direct_read32_if_ram(g, state + 0x14u);
    uint32_t start = end - spb;
    if (spb && end == total) {
        uint32_t rem = total % spb;  /* a short final block starts at total - rem */
        if (rem && pos >= total - rem) start = total - rem;
    }
    uint32_t local = pos - start;
    int use_new = word == GP32_ADPCM_FIX_NEW;
    if (use_new == (int)g->adpcm_fix_disabled && ((local & 1u) || !(start & 1u))) {
        use_new = !use_new;
        direct_write32_if_ram(g, GP32_ADPCM_FIX_PC, use_new ? GP32_ADPCM_FIX_NEW : GP32_ADPCM_FIX_ORIG);
    }
    uint32_t parity = use_new ? local : pos;
    uint32_t cpsr = arm920t_get_cpsr(cpu) & ~0xc0000000u;  /* TST #1: N clear, C/V kept */
    if (!(parity & 1u)) cpsr |= 0x40000000u;
    arm920t_set_cpsr(cpu, cpsr);
    return 1;
}


static uint16_t direct_read_u16_if_ram(gp32_t *g, uint32_t addr) {
    if (direct_ram_range(g, addr, 2u)) return s3c2400_read16(g->soc, addr);
    /* Preserve the zero-filled missing byte when a sample straddles RAM. */
    uint16_t lo = direct_read8_if_ram(g, addr + 0u);
    uint16_t hi = direct_read8_if_ram(g, addr + 1u);
    return (uint16_t)(lo | (uint16_t)(hi << 8));
}

static int16_t direct_read_s16_if_ram(gp32_t *g, uint32_t addr) {
    return (int16_t)direct_read_u16_if_ram(g, addr);
}

static uint32_t direct_pcm_rate_from_sr(uint32_t sr) {
    switch (sr) {
    case 0u: case 1u: return 11025u; /* PCM_M11 / PCM_S11 */
    case 2u: case 3u: return 22050u; /* PCM_M22 / PCM_S22 */
    case 4u: case 5u: return 44100u; /* PCM_M44 / PCM_S44 */
    default: return 11025u;
    }
}

static uint32_t direct_pcm_stereo_from_sr(uint32_t sr) {
    return (sr == 1u || sr == 3u || sr == 5u) ? 1u : 0u;
}

static uint32_t direct_pcm_cursor_addr_channel(const gp32_t *g, uint32_t ch) {
    return direct_pcm_cursor_addr(g) + ch * 4u;
}

static void direct_pcm_sync_legacy(gp32_t *g) {
    if (!g) return;
    uint32_t first = GP32_DIRECT_PCM_CHANNELS;
    for (uint32_t ch = 0; ch < GP32_DIRECT_PCM_CHANNELS; ++ch) {
        if (g->direct_hle_pcm_ch[ch].active) { first = ch; break; }
    }
    if (first < GP32_DIRECT_PCM_CHANNELS) {
        g->direct_hle_pcm_active = 1u;
        g->direct_hle_pcm_src_addr = g->direct_hle_pcm_ch[first].src_addr;
        g->direct_hle_pcm_size_bytes = g->direct_hle_pcm_ch[first].size_bytes;
        g->direct_hle_pcm_pos_bytes = g->direct_hle_pcm_ch[first].pos_bytes;
        g->direct_hle_pcm_repeat = g->direct_hle_pcm_ch[first].repeat;
    } else {
        g->direct_hle_pcm_active = 0u;
        g->direct_hle_pcm_src_addr = 0u;
        g->direct_hle_pcm_size_bytes = 0u;
        g->direct_hle_pcm_pos_bytes = 0u;
        g->direct_hle_pcm_repeat = 0u;
    }
}

static void direct_pcm_update_cursor(gp32_t *g) {
    if (!g) return;
    for (uint32_t ch = 0; ch < GP32_DIRECT_PCM_CHANNELS; ++ch) {
        uint32_t cursor = g->direct_hle_pcm_ch[ch].active ?
            (g->direct_hle_pcm_ch[ch].src_addr + g->direct_hle_pcm_ch[ch].pos_bytes) : 0u;
        direct_write32_if_ram(g, direct_pcm_cursor_addr_channel(g, ch), cursor);
    }
    direct_pcm_sync_legacy(g);
}

static void direct_stop_pcm_channel(gp32_t *g, uint32_t ch) {
    if (!g || ch >= GP32_DIRECT_PCM_CHANNELS) return;
    memset(&g->direct_hle_pcm_ch[ch], 0, sizeof(g->direct_hle_pcm_ch[ch]));
    direct_write32_if_ram(g, direct_pcm_cursor_addr_channel(g, ch), 0u);
    direct_pcm_sync_legacy(g);
}

static void direct_stop_pcm(gp32_t *g) {
    if (!g) return;
    for (uint32_t ch = 0; ch < GP32_DIRECT_PCM_CHANNELS; ++ch) direct_stop_pcm_channel(g, ch);
    g->direct_hle_pcm_accum = 0;
    direct_pcm_update_cursor(g);
}

static void direct_stop_pcm_src(gp32_t *g, uint32_t src_addr) {
    if (!g) return;
    if (!src_addr) { direct_stop_pcm(g); return; }
    for (uint32_t ch = 0; ch < GP32_DIRECT_PCM_CHANNELS; ++ch) {
        if (g->direct_hle_pcm_ch[ch].active && g->direct_hle_pcm_ch[ch].src_addr == src_addr) {
            direct_stop_pcm_channel(g, ch);
        }
    }
    direct_pcm_update_cursor(g);
}

static int direct_start_pcm(gp32_t *g, uint32_t src_addr, uint32_t size_bytes, uint32_t repeat, uint32_t *out_ch) {
    if (out_ch) *out_ch = 0xffffffffu;
    if (!g || !g->soc || !src_addr || size_bytes < 1u || !direct_ram_range(g, src_addr, size_bytes)) return 0;
    if (!g->direct_hle_pcm_rate) g->direct_hle_pcm_rate = direct_pcm_rate_from_sr(g->direct_hle_pcm_sr);
    if (!g->direct_hle_pcm_bits) g->direct_hle_pcm_bits = 16u;

    uint32_t ch = GP32_DIRECT_PCM_CHANNELS;
    for (uint32_t i = 0; i < GP32_DIRECT_PCM_CHANNELS; ++i) {
        if (g->direct_hle_pcm_ch[i].active && g->direct_hle_pcm_ch[i].src_addr == src_addr) { ch = i; break; }
    }
    if (ch == GP32_DIRECT_PCM_CHANNELS) {
        for (uint32_t i = 0; i < GP32_DIRECT_PCM_CHANNELS; ++i) {
            if (!g->direct_hle_pcm_ch[i].active) { ch = i; break; }
        }
    }
    if (ch == GP32_DIRECT_PCM_CHANNELS) return 0;

    g->direct_hle_pcm_ch[ch].active = 1u;
    g->direct_hle_pcm_ch[ch].src_addr = src_addr;
    g->direct_hle_pcm_ch[ch].size_bytes = size_bytes;
    g->direct_hle_pcm_ch[ch].pos_bytes = 0u;
    g->direct_hle_pcm_ch[ch].repeat = repeat ? 1u : 0u;
    g->direct_hle_pcm_ch[ch].rate = g->direct_hle_pcm_rate ? g->direct_hle_pcm_rate : direct_pcm_rate_from_sr(g->direct_hle_pcm_sr);
    g->direct_hle_pcm_ch[ch].stereo = g->direct_hle_pcm_stereo;
    g->direct_hle_pcm_ch[ch].bits = g->direct_hle_pcm_bits ? g->direct_hle_pcm_bits : 16u;
    g->direct_hle_pcm_ch[ch].accum = 0u;
    direct_write32_if_ram(g, direct_pcm_cursor_addr_channel(g, ch), src_addr);
    direct_pcm_sync_legacy(g);
    if (out_ch) *out_ch = ch;
    return 1;
}

static uint32_t direct_pcm_channel_frame_bytes(const gp32_t *g, uint32_t ch) {
    if (!g || ch >= GP32_DIRECT_PCM_CHANNELS) return 0u;
    uint32_t b = (g->direct_hle_pcm_ch[ch].bits == 8u) ? 1u : 2u;
    if (g->direct_hle_pcm_ch[ch].stereo) b *= 2u;
    return b;
}

static int direct_pcm_channel_sample(gp32_t *g, uint32_t ch, uint32_t out_rate, int32_t *left, int32_t *right) {
    if (!g || ch >= GP32_DIRECT_PCM_CHANNELS || !left || !right) return 0;
    if (!g->direct_hle_pcm_ch[ch].active) return 0;
    uint32_t frame_bytes = direct_pcm_channel_frame_bytes(g, ch);
    if (!frame_bytes || g->direct_hle_pcm_ch[ch].size_bytes < frame_bytes) { direct_stop_pcm_channel(g, ch); return 0; }
    if (g->direct_hle_pcm_ch[ch].pos_bytes + frame_bytes > g->direct_hle_pcm_ch[ch].size_bytes) {
        if (g->direct_hle_pcm_ch[ch].repeat) g->direct_hle_pcm_ch[ch].pos_bytes = 0u;
        else { direct_stop_pcm_channel(g, ch); return 0; }
    }

    uint32_t addr = g->direct_hle_pcm_ch[ch].src_addr + g->direct_hle_pcm_ch[ch].pos_bytes;
    int16_t l = 0, r = 0;
    if (g->direct_hle_pcm_ch[ch].bits == 8u) {
        l = (int16_t)(((int)direct_read8_if_ram(g, addr) - 128) * 256);
        if (g->direct_hle_pcm_ch[ch].stereo) r = (int16_t)(((int)direct_read8_if_ram(g, addr + 1u) - 128) * 256);
        else r = l;
    } else {
        l = (int16_t)((int32_t)direct_read_u16_if_ram(g, addr) - 32768);
        if (g->direct_hle_pcm_ch[ch].stereo) r = (int16_t)((int32_t)direct_read_u16_if_ram(g, addr + 2u) - 32768);
        else r = l;
    }
    *left += l;
    *right += r;

    uint32_t rate = g->direct_hle_pcm_ch[ch].rate ? g->direct_hle_pcm_ch[ch].rate : 11025u;
    g->direct_hle_pcm_ch[ch].accum += (uint64_t)rate;
    uint32_t adv = (uint32_t)(g->direct_hle_pcm_ch[ch].accum / (uint64_t)out_rate);
    g->direct_hle_pcm_ch[ch].accum %= (uint64_t)out_rate;
    if (!adv) return 1;
    uint64_t new_pos = (uint64_t)g->direct_hle_pcm_ch[ch].pos_bytes + (uint64_t)adv * (uint64_t)frame_bytes;
    if (new_pos >= g->direct_hle_pcm_ch[ch].size_bytes) {
        if (g->direct_hle_pcm_ch[ch].repeat) new_pos %= g->direct_hle_pcm_ch[ch].size_bytes;
        else { direct_stop_pcm_channel(g, ch); return 1; }
    }
    g->direct_hle_pcm_ch[ch].pos_bytes = (uint32_t)new_pos;
    direct_write32_if_ram(g, direct_pcm_cursor_addr_channel(g, ch), g->direct_hle_pcm_ch[ch].src_addr + g->direct_hle_pcm_ch[ch].pos_bytes);
    direct_pcm_sync_legacy(g);
    return 1;
}

static int direct_hle_pcm_any_active(gp32_t *g) {
    if (!g) return 0;
    for (uint32_t ch = 0; ch < GP32_DIRECT_PCM_CHANNELS; ++ch) if (g->direct_hle_pcm_ch[ch].active) return 1;
    return 0;
}

static int direct_hle_sef_sample(gp32_t *g, uint32_t out_rate, int32_t *left, int32_t *right) {
    if (!g || !left || !right || !g->direct_hle_audio_asset || !g->direct_hle_audio_rate) return 0;
    if (g->direct_hle_audio_pos >= g->direct_hle_audio_size) { direct_stop_sef(g); return 0; }
    uint32_t off = 8u + g->direct_hle_audio_pos;
    const uint8_t *d = g->direct_hle_audio_asset->data;
    int16_t s = (int16_t)(((int)d[off] - 128) * 256);
    *left += s;
    *right += s;

    uint32_t rate = g->direct_hle_audio_rate ? g->direct_hle_audio_rate : 11025u;
    g->direct_hle_audio_accum += (uint64_t)rate;
    uint32_t adv = (uint32_t)(g->direct_hle_audio_accum / (uint64_t)out_rate);
    g->direct_hle_audio_accum %= (uint64_t)out_rate;
    if (adv) {
        uint64_t new_pos = (uint64_t)g->direct_hle_audio_pos + (uint64_t)adv;
        if (new_pos >= g->direct_hle_audio_size) direct_stop_sef(g);
        else g->direct_hle_audio_pos = (uint32_t)new_pos;
    }
    return 1;
}

static int16_t direct_mix_clamp_i16(int32_t v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

static uint32_t direct_hle_mix_output_rate(gp32_t *g) {
    /* Keep the direct-HLE mixer at one output rate for the whole generated
       stream.  Individual GPSDK PCM sources can be 11/22/44 kHz and the SEF
       stream path is normally 22 kHz; switching the emulator's appended sample
       rate while a WAV/frontend buffer is already open makes earlier samples
       play at the wrong speed.  Mix every HLE stream into 44.1 kHz and resample
       each active source into that domain. */
    GP32_UNUSED(g);
    return 44100u;
}

static void direct_hle_pcm_tick(gp32_t *g, uint32_t cycles, uint32_t clock) {
    if (!g || !g->soc || !cycles) return;
    if (!g->direct_hle_audio_asset && !direct_hle_pcm_any_active(g)) return;
    uint32_t out_rate = direct_hle_mix_output_rate(g);
    /* cycles arrive in the same effective instruction-budget domain used
     * by gp32_run_cycles, not the PLL frequency reported to firmware. */
    uint64_t scaled = g->direct_hle_pcm_accum + (uint64_t)cycles * (uint64_t)out_rate;
    uint32_t frames = (uint32_t)(scaled / (uint64_t)clock);
    g->direct_hle_pcm_accum = scaled % (uint64_t)clock;
    if (!frames) return;
    for (uint32_t i = 0; i < frames; ++i) {
        int32_t left = 0, right = 0;
        int active = 0;
        if (direct_hle_sef_sample(g, out_rate, &left, &right)) active = 1;
        for (uint32_t ch = 0; ch < GP32_DIRECT_PCM_CHANNELS; ++ch) {
            if (direct_pcm_channel_sample(g, ch, out_rate, &left, &right)) active = 1;
        }
        if (active) s3c2400_audio_append_s16_stereo(g->soc, direct_mix_clamp_i16(left), direct_mix_clamp_i16(right), out_rate);
    }
    direct_pcm_update_cursor(g);
}


static uint32_t direct_sdk_guess_rate_from_word(uint32_t v) {
    if (v == 11025u || v == 0x2b11u) return 11025u;
    if (v == 22050u || v == 0x5622u) return 22050u;
    if (v == 44100u || v == 0xac44u) return 44100u;
    return 0u;
}

static void direct_sdk_update_srcexist(gp32_t *g) {
    if (!g || !g->direct_hle_sdk_sndmixer_addr || !g->direct_hle_sdk_sndsrcexist_addr) return;
    uint32_t active = 0u;
    for (uint32_t ch = 0; ch < 4u; ++ch) {
        uint32_t e = g->direct_hle_sdk_sndmixer_addr + ch * 20u;
        if (direct_read32_if_ram(g, e + 4u) && direct_read32_if_ram(g, e + 12u)) { active = 1u; break; }
        if (direct_read32_if_ram(g, e + 0u) && direct_read32_if_ram(g, e + 8u)) { active = 1u; break; }
    }
    direct_write32_if_ram(g, g->direct_hle_sdk_sndsrcexist_addr, active);
}

static uint32_t direct_sdk_sound_buffer_base(gp32_t *g, uint32_t bytes) {
    if (!g || !g->soc || !bytes || bytes > 0x10000u) return 0u;
    /* Like the BIOS, keep mixer storage in firmware-owned high RAM. The
       caller only supplies a two-pointer output table: memory beside that
       table (or beyond the packed image) can be live BSS/heap. Reserve two
       maximum-size buffers immediately below the direct-mode LCD pages. */
    uint32_t base = direct_default_surface_addr(0) - 0x20000u;
    if (!direct_ram_range(g, base, 0x20000u) || g->direct_fxe_image_end > base) return 0u;
    const fxe_image_t *img = &g->direct_reset_image;
    if (img->payload && img->payload_size >= 0x20u) {
        uint32_t first = gp32_ld32le(img->payload);
        uint32_t ro_start = gp32_ld32le(img->payload + 4u);
        uint32_t ro_end = gp32_ld32le(img->payload + 8u);
        uint32_t rw_start = gp32_ld32le(img->payload + 12u);
        uint32_t zi_end = gp32_ld32le(img->payload + 16u);
        uint32_t rw_end = gp32_ld32le(img->payload + 20u);
        if ((first & 0x0f000000u) == 0x0a000000u && ro_start == img->load_addr &&
            ro_end >= ro_start && rw_start >= GP32_RAM_BASE && rw_end >= rw_start &&
            zi_end >= rw_end && zi_end > base) return 0u;
    }
    return base;
}

static void direct_sdk_sound_alloc_buffers(gp32_t *g, uint32_t table_addr, uint32_t bytes, uint32_t state_addr) {
    if (!g || !table_addr) return;
    if (bytes < 16u) bytes = 0x180u;
    if (bytes > 0x10000u) bytes = 0x10000u;
    uint32_t base = direct_sdk_sound_buffer_base(g, bytes);
    if (!base) return;
    uint32_t buf0 = base;
    uint32_t buf1 = (base + bytes + 255u) & ~255u;
    direct_zero_if_ram(g, buf0, bytes);
    direct_zero_if_ram(g, buf1, bytes);
    direct_write32_if_ram(g, table_addr + 0u, buf0);
    direct_write32_if_ram(g, table_addr + 4u, buf1);
    g->direct_hle_sdk_sndmixedbuf_addr = table_addr;
    g->direct_hle_sdk_mixbuf0_addr = buf0;
    g->direct_hle_sdk_mixbuf1_addr = buf1;
    g->direct_hle_sdk_mixbuf_bytes = bytes;
    if (state_addr && direct_ram_range(g, state_addr, 4u)) {
        g->direct_hle_sdk_sndsrcexist_addr = state_addr;
        g->direct_hle_sdk_pcm_workidx_addr = state_addr + 4u;
        if (state_addr >= 0x50u) g->direct_hle_sdk_sndmixer_addr = state_addr - 0x50u;
    }
    if (g->direct_hle_sdk_pcm_workidx_addr) direct_write32_if_ram(g, g->direct_hle_sdk_pcm_workidx_addr, 0u);
    direct_sdk_update_srcexist(g);
}

static void direct_sdk_submit_pcm_buffer(gp32_t *g, uint32_t buf, uint32_t bytes, uint32_t rate) {
    if (!g || !g->soc || !buf || bytes < 2u || !direct_ram_range(g, buf, bytes)) return;
    if (!rate) rate = g->direct_hle_sdk_rate ? g->direct_hle_sdk_rate : 44100u;
    if (rate < 4000u || rate > 96000u) rate = 44100u;
    uint32_t frames = bytes / 2u;
    for (uint32_t i = 0; i < frames; ++i) {
        int16_t s = direct_read_s16_if_ram(g, buf + i * 2u);
        s3c2400_audio_append_s16_stereo(g->soc, s, s, rate);
    }
    g->direct_hle_sdk_rate = rate;
    g->direct_hle_sdk_submitted_frames += frames;
    if (g->cpu) g->direct_hle_sdk_last_submit_cycle = arm920t_get_cycles(g->cpu);
}

static int direct_sdk_sound_mix_one(gp32_t *g, const uint8_t *mixer, int16_t *out) {
    if (!g || !out || !g->direct_hle_sdk_sndmixer_addr) return 0;
    int32_t acc = 0;
    uint32_t active = 0u;
    for (uint32_t ch = 0; ch < 4u; ++ch) {
        uint32_t e = g->direct_hle_sdk_sndmixer_addr + ch * 20u;
        const uint8_t *p = mixer ? mixer + ch * 20u : NULL;
        uint32_t src0 = p ? gp32_ld32le(p + 0u) : direct_read32_if_ram(g, e + 0u);
        uint32_t cur = p ? gp32_ld32le(p + 4u) : direct_read32_if_ram(g, e + 4u);
        uint32_t reload = p ? gp32_ld32le(p + 8u) : direct_read32_if_ram(g, e + 8u);
        uint32_t remain = p ? gp32_ld32le(p + 12u) : direct_read32_if_ram(g, e + 12u);
        uint32_t repeat = p ? gp32_ld32le(p + 16u) : direct_read32_if_ram(g, e + 16u);
        if (!cur || !remain) {
            if (src0 && reload) {
                cur = src0;
                remain = reload;
            } else {
                continue;
            }
        }
        if (!direct_ram_range(g, cur, 2u)) {
            direct_write32_if_ram(g, e + 0u, 0u);
            direct_write32_if_ram(g, e + 4u, 0u);
            direct_write32_if_ram(g, e + 12u, 0u);
            continue;
        }
        acc += (int32_t)direct_read_u16_if_ram(g, cur) - 32768;
        cur += 2u;
        remain--;
        if (!remain) {
            if (repeat && src0 && reload) {
                cur = src0;
                remain = reload;
            } else {
                direct_write32_if_ram(g, e + 0u, 0u);
                cur = 0u;
            }
        }
        direct_write32_if_ram(g, e + 4u, cur);
        direct_write32_if_ram(g, e + 12u, remain);
        active++;
    }
    if (!active) {
        direct_sdk_update_srcexist(g);
        return 0;
    }
    int32_t mixed = acc / (int32_t)active;
    if (mixed < -32768) mixed = -32768;
    if (mixed > 32767) mixed = 32767;
    *out = (int16_t)mixed;
    direct_sdk_update_srcexist(g);
    return 1;
}


/* Install the guest context only. gp32_run pays for all callback execution
 * from its caller's budget; exhaustion is suspension, never completion. */
static int direct_begin_guest_function3(gp32_t *g, uint32_t owner, uint32_t fn,
                                        uint32_t r0, uint32_t r1, uint32_t r2) {
    if (!g || !g->cpu || g->direct_callback.owner ||
        (owner != DIRECT_CB_REFILL && owner != DIRECT_CB_TIMER) ||
        (!(fn & 1u) && (fn & 3u)) || !direct_ram_range(g, fn & ~1u, 4u)) return 0;
    uint32_t cb_stack = direct_stub_addr(g) + 0x1f00u;
    if (!direct_ram_range(g, cb_stack - 0x300u, 0x300u)) return 0;
    direct_callback_tail_t *cb = &g->direct_callback;
    arm920t_get_register_context(g->cpu, &cb->foreground);
    cb->owner = owner;
    cb->fn = fn;
    cb->deadline_ns = g->elapsed.nanoseconds + 1000000000u;
    g->direct_hle_callback_running = 1u;
    g->direct_hle_callback_returned = 0u;
    arm920t_set_cpsr(g->cpu, ARM_MODE_SVC | ARM_I_FLAG | ARM_F_FLAG |
                    ((fn & 1u) ? ARM_T_FLAG : 0u));
    arm920t_set_reg(g->cpu, 0, r0);
    arm920t_set_reg(g->cpu, 1, r1);
    arm920t_set_reg(g->cpu, 2, r2);
    arm920t_set_reg(g->cpu, 3, 0u);
    arm920t_set_reg(g->cpu, 13, cb_stack);
    arm920t_set_reg(g->cpu, 14, direct_callback_return_stub_addr(g));
    arm920t_set_reg(g->cpu, 15, fn & ~1u);
    return 1;
}

static void direct_gpos_timer_reset(gp32_t *g) {
    if (!g) return;
    for (uint32_t i = 0; i < GP32_DIRECT_GPOS_TIMER_COUNT; ++i)
        ++g->direct_hle_gpos_timer_epoch[i];
    memset(g->direct_hle_gpos_timer, 0, sizeof(g->direct_hle_gpos_timer));
    g->direct_hle_gpos_timers_enabled = 0u;
    g->direct_hle_gpos_task_first = 0u;
    g->direct_hle_gpos_task_last = 0u;
    g->direct_hle_gpos_scheduler_callback = 0u;
}

static int direct_gpos_task_table_bounds_valid(gp32_t *g, uint32_t first, uint32_t last_after) {
    if (!g || last_after < 0x34u) return 0;
    if (last_after <= first || last_after - first > 16u * 0x34u) return 0;
    if (((last_after - first) % 0x34u) != 0u) return 0;
    if (!direct_ram_range(g, first, 0x34u) || !direct_ram_range(g, last_after - 0x34u, 0x34u)) return 0;
    return direct_task_record_plausible(g, first) && direct_task_record_plausible(g, last_after - 0x34u);
}

static void direct_gpos_neutralize_internal_timer_tasks(gp32_t *g) {
    if (!g || !g->direct_hle_gpos_task_first || !g->direct_hle_gpos_task_last || !g->direct_hle_gpos_scheduler_callback) return;
    uint32_t cb = g->direct_hle_gpos_scheduler_callback & ~1u;
    uint32_t lo = cb + 0x280u;
    uint32_t hi = cb + 0x330u;
    for (uint32_t t = g->direct_hle_gpos_task_first; t <= g->direct_hle_gpos_task_last; t += 0x34u) {
        if (!direct_task_record_plausible(g, t)) continue;
        uint32_t entry = s3c2400_debug_read32(g->soc, t + 0x30u) & ~1u;
        if (entry >= lo && entry < hi) {
            /* The resident GPOS timer-process threads are entered from the
               firmware timer IRQ scheduler.  Direct-FXE HLE does not construct
               that IRQ return frame, so these firmware contexts must remain
               host-emulated rather than being restored as normal game threads. */
            direct_write32_if_ram(g, t + 0x14u, 8u);
            direct_write32_if_ram(g, t + 0x20u, 0u);
            direct_write32_if_ram(g, t + 0x24u, 0xffffffffu);
        }
    }
}

static void direct_gpos_timer_note_scheduler_bounds(gp32_t *g, uint32_t callback) {
    if (!g || !callback) return;
    uint32_t cb = callback & ~1u;
    if (!direct_ram_range(g, cb, 4u)) return;
    for (uint32_t off = 0x180u; off <= 0x580u; off += 4u) {
        uint32_t first = direct_read32_if_ram(g, cb + off + 0x10u);
        uint32_t last_after = direct_read32_if_ram(g, cb + off + 0x14u);
        if (!direct_gpos_task_table_bounds_valid(g, first, last_after)) continue;
        g->direct_hle_gpos_task_first = first;
        g->direct_hle_gpos_task_last = last_after - 0x34u;
        g->direct_hle_gpos_scheduler_callback = cb;
        direct_gpos_neutralize_internal_timer_tasks(g);
        return;
    }
}

static int direct_gpos_task_is_internal_timer(gp32_t *g, uint32_t task_addr) {
    if (!g || !g->direct_hle_gpos_scheduler_callback || !direct_ram_range(g, task_addr, 0x34u)) return 0;
    uint32_t entry = s3c2400_debug_read32(g->soc, task_addr + 0x30u) & ~1u;
    uint32_t cb = g->direct_hle_gpos_scheduler_callback & ~1u;
    return entry >= cb + 0x280u && entry < cb + 0x330u;
}

static int direct_try_emulate_gpos_counter_callback(gp32_t *g, uint32_t callback, uint32_t fires) {
    if (!g || !fires) return 0;
    uint32_t cb = callback & ~1u;
    for (uint32_t off = 0x180u; off <= 0x380u; off += 4u) {
        uint32_t sub_addr = direct_read32_if_ram(g, cb + off + 0u);
        uint32_t threshold_addr = direct_read32_if_ram(g, cb + off + 4u);
        uint32_t tick_addr = direct_read32_if_ram(g, cb + off + 8u);
        if (!direct_ram_range(g, sub_addr, 4u) || !direct_ram_range(g, threshold_addr, 4u) || !direct_ram_range(g, tick_addr, 4u)) continue;
        uint32_t threshold = direct_read32_if_ram(g, threshold_addr);
        if (threshold == 0u || threshold > 1000000u) continue;
        uint64_t limit = (uint64_t)threshold * 2ull;
        uint64_t sub = direct_read32_if_ram(g, sub_addr);
        uint64_t total = sub + (uint64_t)fires;
        uint32_t ticks = direct_read32_if_ram(g, tick_addr);
        ticks += (uint32_t)(total / limit);
        sub = total % limit;
        direct_write32_if_ram(g, sub_addr, (uint32_t)sub);
        direct_write32_if_ram(g, tick_addr, ticks);
        return 1;
    }
    return 0;
}

static int direct_gpos_callback_is_scheduler(gp32_t *g, uint32_t callback) {
    if (!g || !callback) return 0;
    direct_gpos_timer_note_scheduler_bounds(g, callback);
    return g->direct_hle_gpos_scheduler_callback == (callback & ~1u);
}

static void direct_hle_gpos_timer_prepare(gp32_t *g, uint32_t cycles, uint32_t clock,
                                        direct_timer_due_t pending[GP32_DIRECT_GPOS_TIMER_COUNT]) {
    if (!g || !g->direct_fxe_mode || !g->direct_hle_gpos_timers_enabled || !cycles) return;
    /* Settle elapsed time for every slot before guest callbacks can start or
     * reconfigure another timer. New timers must not inherit pre-start time. */
    for (uint32_t i = 0; i < GP32_DIRECT_GPOS_TIMER_COUNT; ++i) {
        uint32_t cb = g->direct_hle_gpos_timer[i].callback;
        uint32_t tps = g->direct_hle_gpos_timer[i].tps;
        if (!g->direct_hle_gpos_timer[i].configured || !g->direct_hle_gpos_timer[i].enabled || !cb || !tps) continue;
        pending[i].callback = cb;
        pending[i].tps = tps;
        pending[i].epoch = g->direct_hle_gpos_timer_epoch[i];
        if (tps > 200000u) tps = 200000u;
        uint64_t scaled = g->direct_hle_gpos_timer[i].accum + (uint64_t)cycles * (uint64_t)tps;
        pending[i].fires = (uint32_t)(scaled / (uint64_t)clock);
        g->direct_hle_gpos_timer[i].accum = scaled % (uint64_t)clock;
    }
}

static int direct_timer_due_current(const gp32_t *g, uint32_t i) {
    const direct_timer_due_t *due = &g->direct_tick.due[i];
    return due->fires && g->direct_hle_gpos_timers_enabled &&
        g->direct_hle_gpos_timer[i].configured && g->direct_hle_gpos_timer[i].enabled &&
        g->direct_hle_gpos_timer_epoch[i] == due->epoch &&
        g->direct_hle_gpos_timer[i].callback == due->callback &&
        g->direct_hle_gpos_timer[i].tps == due->tps;
}

static void direct_hle_gpos_timer_dispatch(gp32_t *g) {
    direct_hle_tick_tail_t *tail = &g->direct_tick;
    int continuing = tail->phase == DIRECT_TICK_TIMER_CALLS;
    for (uint32_t i = tail->timer_slot; i < GP32_DIRECT_GPOS_TIMER_COUNT; ++i) {
        const direct_timer_due_t *due = &tail->due[i];
        uint32_t cb = due->callback, tps = due->tps, fires = due->fires;
        if (!continuing) {
            tail->calls_left = 0u;
            if (!direct_timer_due_current(g, i)) continue;
            if (direct_gpos_callback_is_scheduler(g, cb)) {
                direct_gpos_neutralize_internal_timer_tasks(g);
                direct_tick_sdk_task_sleepers(g, g->direct_hle_gpos_task_first,
                                              g->direct_hle_gpos_task_last, fires);
                continue;
            }
            if (direct_try_emulate_gpos_counter_callback(g, cb, fires)) continue;
            if (tps > 1000u || !direct_ram_range(g, cb & ~1u, 4u)) continue;
            tail->calls_left = fires > 8u ? 8u : fires;
        }
        continuing = 0;
        while (tail->calls_left) {
            if (!direct_timer_due_current(g, i)) break;
            --tail->calls_left;
            tail->timer_slot = i;
            tail->phase = DIRECT_TICK_TIMER_CALLS;
            if (direct_begin_guest_function3(g, DIRECT_CB_TIMER, cb, 0u, 0u, 0u)) return;
        }
    }
}


static int direct_handle_swi_gpos_timer(gp32_t *g, arm920t_t *cpu, uint32_t pc) {
    if (!g || !cpu) return 0;
    uint32_t cmdp = arm920t_get_reg(cpu, 0);
    if (!direct_ram_range(g, cmdp, 4u)) {
        arm920t_set_reg(cpu, 0, 0u);
        return 1;
    }
    uint32_t cmd = direct_read32_if_ram(g, cmdp + 0u);
    /* Some GPSDK/GPOS task code invokes SWI #0x13 from an IRQ-return style
       wrapper.  In direct-FXE HLE the SWI itself is handled inline, so the
       guest's following MOVS PC,LR/SPSR path is not a real exception return.
       Emulate the wrapper epilogue and continue at its saved LR instead. */
    if (cmd == 2u && direct_ram_range(g, cmdp, 32u)) {
        uint32_t selector = direct_read32_if_ram(g, cmdp + 4u);
        uint32_t saved_spsr = direct_read32_if_ram(g, cmdp + 8u);
        if (selector == 1u && ((saved_spsr & 0x1fu) == ARM_MODE_SVC || (saved_spsr & 0x1fu) == 0x10u || (saved_spsr & 0x1fu) == 0x1fu)) {
            uint32_t saved_lr = direct_read32_if_ram(g, cmdp + 28u);
            if (direct_ram_range(g, saved_lr & ~1u, 4u)) {
                arm920t_set_reg(cpu, 0, direct_read32_if_ram(g, cmdp + 12u));
                arm920t_set_reg(cpu, 1, direct_read32_if_ram(g, cmdp + 16u));
                arm920t_set_reg(cpu, 2, direct_read32_if_ram(g, cmdp + 20u));
                arm920t_set_reg(cpu, 3, direct_read32_if_ram(g, cmdp + 24u));
                arm920t_set_reg(cpu, 13, cmdp + 32u);
                arm920t_set_reg(cpu, 14, saved_lr);
                arm920t_set_cpsr(cpu, saved_spsr);
                arm920t_set_reg(cpu, 15, saved_lr & ~1u);
                (void)pc;
                return 1;
            }
        }
    }
    switch (cmd) {
    case 0u:
        direct_gpos_timer_reset(g);
        break;
    case 1u: {
        uint32_t idx = direct_read32_if_ram(g, cmdp + 4u);
        uint32_t cb = direct_read32_if_ram(g, cmdp + 8u);
        uint32_t tps = direct_read32_if_ram(g, cmdp + 12u);
        uint32_t max_exec = direct_read32_if_ram(g, cmdp + 16u);
        if (idx < GP32_DIRECT_GPOS_TIMER_COUNT && direct_ram_range(g, cb & ~1u, 4u) && tps) {
            ++g->direct_hle_gpos_timer_epoch[idx];
            g->direct_hle_gpos_timer[idx].configured = 1u;
            g->direct_hle_gpos_timer[idx].enabled = 0u;
            g->direct_hle_gpos_timer[idx].callback = cb;
            g->direct_hle_gpos_timer[idx].tps = tps;
            g->direct_hle_gpos_timer[idx].max_exec_tick = max_exec;
            g->direct_hle_gpos_timer[idx].accum = 0u;
            direct_gpos_timer_note_scheduler_bounds(g, cb);
        }
        break;
    }
    case 2u: {
        uint32_t arg = direct_read32_if_ram(g, cmdp + 4u);
        if (arg < GP32_DIRECT_GPOS_TIMER_COUNT && g->direct_hle_gpos_timer[arg].configured) {
            g->direct_hle_gpos_timer[arg].enabled = 1u;
        } else {
            for (uint32_t i = 0; i < GP32_DIRECT_GPOS_TIMER_COUNT; ++i) {
                if (g->direct_hle_gpos_timer[i].configured) g->direct_hle_gpos_timer[i].enabled = 1u;
            }
        }
        g->direct_hle_gpos_timers_enabled = 1u;
        direct_gpos_neutralize_internal_timer_tasks(g);
        break;
    }
    case 3u: {
        uint32_t idx = direct_read32_if_ram(g, cmdp + 4u);
        if (idx < GP32_DIRECT_GPOS_TIMER_COUNT) {
            ++g->direct_hle_gpos_timer_epoch[idx];
            g->direct_hle_gpos_timer[idx].enabled = 0u;
        }
        break;
    }
    case 4u: {
        uint32_t idx = direct_read32_if_ram(g, cmdp + 4u);
        if (idx < GP32_DIRECT_GPOS_TIMER_COUNT && g->direct_hle_gpos_timer[idx].configured) {
            g->direct_hle_gpos_timer[idx].enabled = 1u;
            g->direct_hle_gpos_timers_enabled = 1u;
            direct_gpos_neutralize_internal_timer_tasks(g);
        }
        break;
    }
    case 5u: {
        uint32_t idx = direct_read32_if_ram(g, cmdp + 4u);
        if (idx < GP32_DIRECT_GPOS_TIMER_COUNT) {
            ++g->direct_hle_gpos_timer_epoch[idx];
            memset(&g->direct_hle_gpos_timer[idx], 0, sizeof(g->direct_hle_gpos_timer[idx]));
        }
        break;
    }
    default:
        break;
    }
    arm920t_set_reg(cpu, 0, 0u);
    return 1;
}

/* Refill a released half and bound the next mix span by its cursor edge.
 * Without a recognized stream, retain the ordinary 64-sample poll cadence. */
static uint32_t direct_sdk_pcm_refill_tick(gp32_t *g, int allow_refill) {
    if (!g || !g->direct_hle_sdk_sndmixer_addr) return 64u;
    uint32_t entry = g->direct_hle_sdk_sndmixer_addr;
    uint32_t cursor_ptr_addr = 0u;
    uint32_t start = (entry > 0x1000u) ? entry - 0x1000u : GP32_RAM_BASE;
    uint32_t end = entry + 0x1000u;
    uint32_t ram_end = GP32_RAM_BASE + (uint32_t)s3c2400_ram_size(g->soc);
    if (end > ram_end) end = ram_end;
    /* Resolve this read-only RAM window once, retaining the guarded reader
     * for windows crossing a RAM boundary. No pointer survives this search
     * or the guest callback below. */
    const uint8_t *scan = NULL;
    if (end >= start && direct_ram_range(g, start, end - start)) {
        arm_bus_t bus = s3c2400_get_bus(g->soc);
        scan = bus.fastmem(bus.user, start, end - start, 0);
    }
    for (uint32_t a = start; a + 0x24u < end; a += 4u) {
        uint32_t value = scan ? gp32_ld32le(scan + (a - start)) : direct_read32_if_ram(g, a);
        if (value != entry + 4u) continue;
        uint32_t base = direct_read32_if_ram(g, a + 0x0cu);
        uint32_t block = a + 0x14u;
        uint32_t obj = direct_read32_if_ram(g, block + 8u);
        uint32_t fill = direct_read32_if_ram(g, block + 12u);
        if (direct_ram_range(g, base, 2u) && direct_ram_range(g, obj, 4u) && direct_ram_range(g, fill & ~1u, 4u)) {
            cursor_ptr_addr = a;
            break;
        }
    }
    if (!cursor_ptr_addr) return 64u;
    uint32_t last_half_addr = cursor_ptr_addr - 4u;
    uint32_t shift = direct_read32_if_ram(g, cursor_ptr_addr + 8u);
    uint32_t base = direct_read32_if_ram(g, cursor_ptr_addr + 0x0cu);
    uint32_t block = cursor_ptr_addr + 0x14u;
    uint32_t half_units = direct_read32_if_ram(g, block + 4u);
    uint32_t obj = direct_read32_if_ram(g, block + 8u);
    uint32_t fill = direct_read32_if_ram(g, block + 12u);
    uint32_t cur = direct_read32_if_ram(g, entry + 4u);
    if (!base || !cur || !half_units || shift > 4u || !direct_ram_range(g, fill & ~1u, 4u)) return 64u;
    if (half_units > (0x20000u >> shift)) return 64u;
    uint32_t half_bytes = half_units << shift;
    if (!half_bytes || half_bytes > 0x20000u || cur < base ||
        (uint64_t)cur - base >= (uint64_t)half_bytes * 2u) return 64u;
    uint32_t current_half = ((cur - base) >= half_bytes) ? 1u : 0u;
    uint32_t previous_half = direct_read32_if_ram(g, last_half_addr) & 1u;
    if (current_half == previous_half) {
        uint32_t bytes_left = half_bytes - ((cur - base) % half_bytes);
        uint32_t samples_left = (bytes_left + 1u) / 2u;
        return samples_left < 64u ? samples_left : 64u;
    }
    if (!allow_refill) return 64u;
    uint32_t dst = base + (previous_half ? half_bytes : 0u);
    if (!direct_ram_range(g, dst, half_bytes)) return 64u;
    uint32_t old_phase = g->direct_tick.phase;
    g->direct_tick.phase = DIRECT_TICK_AFTER_REFILL;
    if (!direct_begin_guest_function3(g, DIRECT_CB_REFILL, fill, obj, dst, half_bytes)) {
        g->direct_tick.phase = old_phase;
        return 64u;
    }
    g->direct_callback.ack_addr = last_half_addr;
    g->direct_callback.ack_value = current_half;
    return 0u; /* PENDING: do not acknowledge or mix until the real return. */
}

static void direct_sdk_sound_mix(gp32_t *g, uint32_t span) {
    direct_hle_tick_tail_t *tail = &g->direct_tick;
    uint32_t rate = tail->sdk_rate;
    /* Reacquire every host pointer after guest execution or a state load. */
    arm_bus_t bus = s3c2400_get_bus(g->soc);
    while (tail->sdk_frames_left) {
        uint32_t until_poll = 64u - (uint32_t)g->direct_hle_sdk_timer_accum;
        if (span > until_poll) span = until_poll;
        uint32_t until_event = span;
        if (span > tail->sdk_frames_left) span = tail->sdk_frames_left;
        /* Only the address is reused within this callback-free span. Read
         * live channel words each sample, retaining alias/write ordering and
         * the guarded fallback for a table crossing the RAM boundary. */
        const uint8_t *mixer = NULL;
        if (direct_ram_range(g, g->direct_hle_sdk_sndmixer_addr, 80u))
            mixer = bus.fastmem(bus.user, g->direct_hle_sdk_sndmixer_addr, 80u, 0);
        for (uint32_t i = 0; i < span; ++i) {
            int16_t sample = 0;
            if (!direct_sdk_sound_mix_one(g, mixer, &sample)) break;
            s3c2400_audio_append_s16_stereo(g->soc, sample, sample, rate);
        }
        tail->sdk_frames_left -= span;
        g->direct_hle_sdk_timer_accum += span;
        if (g->direct_hle_sdk_timer_accum == 64u) {
            g->direct_hle_sdk_timer_accum = 0u;
            /* GPOS owns timer dispatch. The old SDK-table scan only cached
             * an unused address; invoking those IRQ-context callbacks here
             * would corrupt foreground task state (for example AKA NOID). */
        }
        /* A short host slice must not retry a failed guest refill more often
         * than the next cursor/poll boundary. */
        span = direct_sdk_pcm_refill_tick(g, span == until_event);
        if (!span) return;
    }
}

static void direct_sdk_sound_tick(gp32_t *g, uint32_t cycles, uint32_t clock) {
    if (!g || !g->soc || !cycles || !g->direct_hle_sdk_sndmixer_addr) return;
    uint32_t rate = g->direct_hle_sdk_rate ? g->direct_hle_sdk_rate : 44100u;
    if (g->direct_hle_sdk_last_submit_cycle && g->cpu) {
        uint64_t now = arm920t_get_cycles(g->cpu);
        if (now >= g->direct_hle_sdk_last_submit_cycle && now - g->direct_hle_sdk_last_submit_cycle < (uint64_t)clock / 30u) return;
    }
    uint64_t scaled = g->direct_hle_sdk_accum + (uint64_t)cycles * (uint64_t)rate;
    g->direct_tick.sdk_frames_left = (uint32_t)(scaled / (uint64_t)clock);
    g->direct_tick.sdk_rate = rate;
    g->direct_hle_sdk_accum = scaled % (uint64_t)clock;
    if (!g->direct_tick.sdk_frames_left) return;
    g->direct_hle_sdk_timer_accum %= 64u;
    uint32_t span = direct_sdk_pcm_refill_tick(g, g->direct_hle_sdk_timer_accum == 0u);
    if (span) direct_sdk_sound_mix(g, span);
}

static int direct_handle_swi_set_sndbuffer(gp32_t *g, arm920t_t *cpu) {
    if (!g || !cpu) return 0;
    uint32_t table_addr = arm920t_get_reg(cpu, 0);
    uint32_t bytes = arm920t_get_reg(cpu, 1);
    uint32_t state_addr = arm920t_get_reg(cpu, 3);
    direct_sdk_sound_alloc_buffers(g, table_addr, bytes, state_addr);
    arm920t_set_reg(cpu, 0, 0u);
    return 1;
}

static int direct_handle_swi_iis(gp32_t *g, arm920t_t *cpu) {
    if (!g || !cpu) return 0;
    uint32_t cmdp = arm920t_get_reg(cpu, 0);
    uint32_t cmd = direct_read32_if_ram(g, cmdp + 0u);
    if (!cmd) cmd = arm920t_get_reg(cpu, 3);
    for (uint32_t i = 0; i < 10u && direct_ram_range(g, cmdp + i * 4u, 4u); ++i) {
        uint32_t rate = direct_sdk_guess_rate_from_word(direct_read32_if_ram(g, cmdp + i * 4u));
        if (rate) { g->direct_hle_sdk_rate = rate; break; }
    }
    switch (cmd & 0xffffu) {
    case 0x0180u: {
        uint32_t buf = direct_read32_if_ram(g, cmdp + 8u);
        uint32_t bytes = direct_read32_if_ram(g, cmdp + 16u);
        if (!buf) buf = arm920t_get_reg(cpu, 1);
        if (!bytes) bytes = arm920t_get_reg(cpu, 3);
        direct_sdk_submit_pcm_buffer(g, buf, bytes, g->direct_hle_sdk_rate);
        break;
    }
    case 0x1000u:
    case 0x8010u:
    case 0x4010u:
    case 0x2000u:
    case 0x0020u:
    case 0x0002u:
    default:
        break;
    }
    arm920t_set_reg(cpu, 0, 1u);
    return 1;
}

static void direct_hle_audio_tick(gp32_t *g, uint32_t cycles, uint32_t clock) {
    if (!g || !cycles) return;
    direct_hle_pcm_tick(g, cycles, clock);
    direct_sdk_sound_tick(g, cycles, clock);
}

/* Only the actual refill and timer consumer tails can be pending. Preparation
 * and prefix PCM mixing happen once, before the first callback is installed. */
static void direct_hle_tick_pump(gp32_t *g) {
    direct_hle_tick_tail_t *tail = &g->direct_tick;
    if (!tail->clock || g->direct_callback.owner) return;
    if (tail->phase == DIRECT_TICK_AFTER_REFILL) {
        direct_sdk_sound_mix(g, 1u);
        if (g->direct_callback.owner) return;
        tail->phase = DIRECT_TICK_IDLE;
    }
    if (tail->volume) {
        s3c2400_audio_set_volume(g->soc, tail->volume);
        tail->volume = 0u;
    }
    direct_hle_gpos_timer_dispatch(g);
    if (g->direct_callback.owner) return;
    uint32_t clock = tail->clock, next_clock = direct_run_clock_hz(g);
    if (next_clock != clock) {
        g->direct_hle_pcm_accum = g->direct_hle_pcm_accum * next_clock / clock;
        g->direct_hle_sdk_accum = g->direct_hle_sdk_accum * next_clock / clock;
        for (unsigned i = 0; i < GP32_DIRECT_GPOS_TIMER_COUNT; ++i)
            g->direct_hle_gpos_timer[i].accum = g->direct_hle_gpos_timer[i].accum * next_clock / clock;
    }
    memset(tail, 0, sizeof(*tail));
}

static uint32_t direct_alloc_fpk_handle(gp32_t *g, const fpk_asset_t *asset) {
    if (!g || !asset) return 0;
    for (uint32_t i = 1u; i < 32u; ++i) {
        if (!g->direct_fpk_handles[i].used) {
            g->direct_fpk_handles[i].used = 1;
            g->direct_fpk_handles[i].asset = asset;
            g->direct_fpk_handles[i].pos = 0;
            return i;
        }
    }
    return 0;
}

static const fpk_asset_t *direct_handle_asset(gp32_t *g, uint32_t h, size_t **posp) {
    uint32_t idx = h & 31u;
    if (!g || idx == 0u || idx >= 32u || !g->direct_fpk_handles[idx].used || (h >> 24u) != 0u) return NULL;
    if (posp) *posp = &g->direct_fpk_handles[idx].pos;
    return g->direct_fpk_handles[idx].asset;
}


static uint32_t direct_arm_branch_target(uint32_t pc, uint32_t insn) {
    int32_t imm = (int32_t)(insn & 0x00ffffffu);
    if (imm & 0x00800000) imm |= (int32_t)0xff000000u;
    return pc + 8u + ((uint32_t)imm << 2);
}

static int direct_arm_bl_to(uint32_t pc, uint32_t insn, uint32_t target) {
    return (insn & 0x0f000000u) == 0x0b000000u && direct_arm_branch_target(pc, insn) == target;
}

static int direct_arm_is_bl(uint32_t insn) {
    return (insn & 0x0f000000u) == 0x0b000000u;
}

static int direct_file_helper_looks_read(gp32_t *g, uint32_t target) {
    if (!g || !direct_ram_range(g, target, 0x18u)) return 0;
    uint32_t w0 = s3c2400_debug_read32(g->soc, target + 0x00u);
    uint32_t w1 = s3c2400_debug_read32(g->soc, target + 0x04u);
    uint32_t w2 = s3c2400_debug_read32(g->soc, target + 0x08u);
    uint32_t w3 = s3c2400_debug_read32(g->soc, target + 0x0cu);
    uint32_t w4 = s3c2400_debug_read32(g->soc, target + 0x10u);
    if (w0 == 0xe92d4fffu && w1 == 0xe24dd00cu && w2 == 0xe1a05001u &&
        w3 == 0xe1a04002u && w4 == 0xe3a0107fu) return 1;
    return 0;
}

static int direct_file_helper_looks_seek(gp32_t *g, uint32_t target) {
    if (!g || !direct_ram_range(g, target, 0x20u)) return 0;
    uint32_t w0 = s3c2400_debug_read32(g->soc, target + 0x00u);
    uint32_t w1 = s3c2400_debug_read32(g->soc, target + 0x04u);
    uint32_t w2 = s3c2400_debug_read32(g->soc, target + 0x08u);
    uint32_t w3 = s3c2400_debug_read32(g->soc, target + 0x0cu);
    uint32_t w4 = s3c2400_debug_read32(g->soc, target + 0x10u);
    uint32_t w5 = s3c2400_debug_read32(g->soc, target + 0x14u);
    /* Retail file seek helper variants keep handle in r0, seek mode in r1,
       signed offset in r2, and the out-position pointer in r3.  W.B.W. uses
       the second ordering below; treating it as read made asset loaders copy
       data to address 0x00000001 instead of seeking inside .PAK files. */
    if (w0 == 0xe92d4ff8u && w1 == 0xe1a05001u && w2 == 0xe1a04002u &&
        w3 == 0xe1a06003u && w4 == 0xe3a0107fu && (w5 & 0x0fffff00u) == 0x00017c00u) return 1;
    if (w0 == 0xe92d4ff8u && w1 == 0xe1a05001u && w2 == 0xe3a0107fu &&
        w3 == 0xe0017c40u && w4 == 0xe1a04002u && w5 == 0xe1a06003u) return 1;
    return 0;
}

/*
 * Samsung/Mirko SMFS SmartMedia library (gp_smc.a, built from the SDK's
 * lib.src/smfs plus the closed smf_*.o objects).  Homebrew that links this
 * prebuilt library reaches the card through its own wrapper family instead of
 * the GPSDK file API: every wrapper opens with "mov ip,sp" and a register-save
 * prologue, calls the card gate (a function that is exactly
 * "push {lr}; swi 0x11; pop {pc}"), then calls the closed Samsung driver entry
 * for its operation.  The emulator's card gate already answers "card present",
 * but the driver has no FAT volume to resolve, so every open/read/seek fails and
 * the title can never load its assets.  Matching the wrapper family
 * structurally - prologue, card gate call, driver entry shape - lets the
 * existing asset-backed HLE serve these entries too.  No link address or title
 * address is involved, so any gp_smc.a build is treated the same way.
 */
static int direct_smfs_card_gate(gp32_t *g, uint32_t target) {
    if (!direct_ram_range(g, target, 12u)) return 0;
    return s3c2400_debug_read32(g->soc, target + 0u) == 0xe92d4000u &&
           s3c2400_debug_read32(g->soc, target + 4u) == 0xef000011u &&
           s3c2400_debug_read32(g->soc, target + 8u) == 0xe8bd8000u;
}

/* Classify a Samsung driver entry as the HLE id of the wrapper that calls it.
   The heads are the compiled prologues of smOpenFile (read mode), smReadFile,
   smSeekFile and smCloseFile; the write-side entries keep distinct heads and
   are deliberately left alone because the HLE has no write service. */
static uint32_t direct_smfs_driver_hle_id(gp32_t *g, uint32_t target) {
    if (!direct_ram_range(g, target, 0x14u)) return 0u;
    uint32_t h0 = s3c2400_debug_read32(g->soc, target + 0x00u);
    uint32_t h1 = s3c2400_debug_read32(g->soc, target + 0x04u);
    uint32_t h2 = s3c2400_debug_read32(g->soc, target + 0x08u);
    uint32_t h3 = s3c2400_debug_read32(g->soc, target + 0x0cu);
    uint32_t h4 = s3c2400_debug_read32(g->soc, target + 0x10u);
    if (h0 != 0xe1a0c00du) return 0u;
    if (h1 == 0xe92dddf0u && h2 == 0xe24cb004u && h3 == 0xe24dd04cu && h4 == 0xe1a08001u) return 1u; /* open */
    if (h1 == 0xe92ddff0u && h2 == 0xe24cb004u && h3 == 0xe1a0c000u && h4 == 0xe1a00c4cu) return 2u; /* read */
    if (h1 == 0xe92ddff0u && h2 == 0xe1a0e000u && h3 == 0xe1a00c4eu && h4 == 0xe24cb004u) return 5u; /* seek */
    if (h1 == 0xe92dddf0u && h2 == 0xe1a01000u && h3 == 0xe1a03c41u && h4 == 0xe24cb004u) return 3u; /* close */
    return 0u;
}

static void direct_scan_file_hle_range(gp32_t *g, uint32_t start, uint32_t end) {
    if (!g || !g->direct_fpk_asset_count || !g->direct_fxe_mode) return;
    uint32_t ram_end = GP32_RAM_BASE + (uint32_t)s3c2400_ram_size(g->soc);
    if (start < GP32_RAM_BASE) start = GP32_RAM_BASE;
    if (end > ram_end) end = ram_end;
    if (end <= start) return;
    for (uint32_t a = start; a + 0x90u < end; a += 4u) {
        uint32_t w0 = s3c2400_debug_read32(g->soc, a + 0u);
        uint32_t w1 = s3c2400_debug_read32(g->soc, a + 4u);
        uint32_t w2 = s3c2400_debug_read32(g->soc, a + 8u);
        uint32_t w3 = s3c2400_debug_read32(g->soc, a + 12u);
        uint32_t w4 = s3c2400_debug_read32(g->soc, a + 16u);
        if (w0 == 0xe92d40f0u && w1 == 0xe1a06000u && w2 == 0xe1a05001u && w3 == 0xe1a04002u) {
            uint32_t bl = s3c2400_debug_read32(g->soc, a + 0x10u);
            uint32_t after_bl = s3c2400_debug_read32(g->soc, a + 0x14u);
            uint32_t cmp = s3c2400_debug_read32(g->soc, a + 0x18u);
            uint32_t ret_err2 = s3c2400_debug_read32(g->soc, a + 0x20u);
            int open_like = (after_bl & 0xffff0000u) == 0xe51f0000u && cmp == 0xe3500000u && ret_err2 == 0x03a00002u && direct_arm_bl_to(a + 0x10u, bl, GP32_RAM_BASE + 0x00000e64u);
            int legacy_open_like = (s3c2400_debug_read32(g->soc, a + 0x60u) == 0xe51f0224u);
            int retail_open_like = (w0 == 0xe92d40f0u && w1 == 0xe1a06000u && w2 == 0xe1a05001u && w3 == 0xe1a04002u &&
                                    (w4 & 0x0f000000u) == 0x0b000000u && s3c2400_debug_read32(g->soc, a + 0x28u) == 0xe1a00006u &&
                                    (direct_arm_bl_to(a + 0x60u, s3c2400_debug_read32(g->soc, a + 0x60u), GP32_RAM_BASE + 0x00109118u) ||
                                     direct_arm_bl_to(a + 0x60u, s3c2400_debug_read32(g->soc, a + 0x60u), GP32_RAM_BASE + 0x00109428u)));
            if ((open_like || legacy_open_like || retail_open_like) && g->direct_hle_file_open_addr != a) {
                if (!g->direct_hle_file_open_addr) g->direct_hle_file_open_addr = a;
                if (legacy_open_like) {
                    uint32_t lit = a + 0x60u + 8u - (s3c2400_debug_read32(g->soc, a + 0x60u) & 0xfffu);
                    if (direct_ram_range(g, lit, 4u)) g->direct_hle_pathbuf_addr = s3c2400_debug_read32(g->soc, lit);
                } else {
                    uint32_t lit = a + 0x14u + 8u - (after_bl & 0xfffu);
                    if (direct_ram_range(g, lit, 4u)) g->direct_hle_pathbuf_addr = s3c2400_debug_read32(g->soc, lit);
                }
                s3c2400_write32(g->soc, a, 0xef070001u);
            }
        }
        if ((w0 == 0xe92d40f0u && w1 == 0xe1a07000u && w2 == 0xe1a06001u && w3 == 0xe1a05002u && w4 == 0xe1a04003u)) {
            uint32_t body_pc = a + 0x54u;
            uint32_t body_bl = s3c2400_debug_read32(g->soc, body_pc);
            if (!direct_arm_is_bl(body_bl)) {
                body_pc = a + 0x58u;
                body_bl = s3c2400_debug_read32(g->soc, body_pc);
            }
            uint32_t body_target = direct_arm_is_bl(body_bl) ? direct_arm_branch_target(body_pc, body_bl) : 0u;
            int retail_seek_like = direct_arm_bl_to(body_pc, body_bl, GP32_RAM_BASE + 0x00109b68u) || direct_file_helper_looks_seek(g, body_target);
            int retail_write_like = direct_arm_bl_to(body_pc, body_bl, GP32_RAM_BASE + 0x00109964u);
            int retail_read_like = direct_file_helper_looks_read(g, body_target);
            if (retail_seek_like) {
                g->direct_hle_file_seek_addr = a;
                s3c2400_write32(g->soc, a, 0xef070005u);
            } else if (!retail_write_like && (retail_read_like || body_target == 0u || g->direct_hle_file_read_addr[0] == body_target || g->direct_hle_file_read_addr[1] == body_target)) {
                if (!g->direct_hle_file_read_addr[0]) {
                    g->direct_hle_file_read_addr[0] = a;
                } else if (g->direct_hle_file_read_addr[0] != a && !g->direct_hle_file_read_addr[1]) {
                    g->direct_hle_file_read_addr[1] = a;
                }
                s3c2400_write32(g->soc, a, 0xef070002u);
            }
        }
        /* Retail BIOS file layer.  Commercial GXE/GXC titles such as Princess Maker 2 often call the lower
           firmware file entry points directly after the BIOS has installed the card/FAT service table.  Direct
           BIOSless SMC loading has already extracted the FAT files into direct_fpk_assets, so patch these SDK
           entry points to the same asset-backed HLE used by the higher GPSDK wrappers. */
        if (w0 == 0xe92d4ff0u && w1 == 0xe24dd060u && w2 == 0xe1a05001u && w3 == 0xe1a06002u && w4 == 0xe1a07000u) {
            g->direct_hle_file_open_addr = a;
            s3c2400_write32(g->soc, a, 0xef070001u);
        }
        /* The SDK also has a separate existing-file open entry, with a smaller
           stack frame and swapped saved mode/output registers. Both entries
           use (path, mode, handle_out); recognize the device-validation prefix
           without depending on their link address or a title's wrapper. */
        if (w0 == 0xe92d4ff0u && w1 == 0xe24dd04cu && w2 == 0xe1a06001u && w3 == 0xe1a05002u && w4 == 0xe1a07000u &&
            direct_arm_is_bl(s3c2400_debug_read32(g->soc, a + 0x14u)) &&
            s3c2400_debug_read32(g->soc, a + 0x18u) == 0xe1a04000u &&
            s3c2400_debug_read32(g->soc, a + 0x1cu) == 0xe3500001u &&
            s3c2400_debug_read32(g->soc, a + 0x20u) == 0x23a0000bu) {
            g->direct_hle_file_open_addr = a;
            s3c2400_write32(g->soc, a, 0xef070001u);
        }
        if (w0 == 0xe92d4fffu && w1 == 0xe24dd00cu && w2 == 0xe1a05001u && w3 == 0xe3a0107fu && w4 == 0xe0016c40u) {
            if (!g->direct_hle_file_read_addr[0]) {
                g->direct_hle_file_read_addr[0] = a;
            } else if (g->direct_hle_file_read_addr[0] != a && !g->direct_hle_file_read_addr[1]) {
                g->direct_hle_file_read_addr[1] = a;
            }
            s3c2400_write32(g->soc, a, 0xef070002u);
        }
        if (w0 == 0xe92d41f0u && w1 == 0xe24dd040u && w2 == 0xe3a0107fu && w3 == 0xe0014c40u) {
            g->direct_hle_file_close_addr = a;
            s3c2400_write32(g->soc, a, 0xef070003u);
        }
        if (w0 == 0xe92d40f0u && w1 == 0xe24dd048u && w2 == 0xe1a05001u && w3 == 0xe1a06000u) {
            g->direct_hle_file_size_addr = a;
            s3c2400_write32(g->soc, a, 0xef070004u);
        }
        if (w0 == 0xe92d4010u && w1 == 0xe1a04000u) {
            uint32_t bl = s3c2400_debug_read32(g->soc, a + 0x08u);
            uint32_t after_bl = s3c2400_debug_read32(g->soc, a + 0x0cu);
            uint32_t cmp = s3c2400_debug_read32(g->soc, a + 0x10u);
            uint32_t err2 = s3c2400_debug_read32(g->soc, a + 0x18u);
            int close_like = (after_bl & 0xffff0000u) == 0xe51f0000u && cmp == 0xe3500000u && err2 == 0x03a00002u && direct_arm_bl_to(a + 0x08u, bl, GP32_RAM_BASE + 0x00000e64u);
            int legacy_close_like = s3c2400_debug_read32(g->soc, a + 0x40u) == 0xebffe409u;
            int retail_close_like = (w0 == 0xe92d4010u && w1 == 0xe1a04000u && (w2 & 0x0f000000u) == 0x0b000000u &&
                                     s3c2400_debug_read32(g->soc, a + 0x38u) == 0xe1a00004u &&
                                     (s3c2400_debug_read32(g->soc, a + 0x3cu) & 0x0f000000u) == 0x0b000000u);
            if ((close_like || legacy_close_like || retail_close_like) && g->direct_hle_file_close_addr != a) {
                if (!g->direct_hle_file_close_addr) g->direct_hle_file_close_addr = a;
                s3c2400_write32(g->soc, a, 0xef070003u);
            }
        }
        if (w0 == 0xe92d4010u && w1 == 0xe1a04001u && s3c2400_debug_read32(g->soc, a + 0x54u) == 0xebffe483u) {
            if (!g->direct_hle_file_size_addr) g->direct_hle_file_size_addr = a;
            s3c2400_write32(g->soc, a, 0xef070004u);
        }
        if (w0 == 0xe92d4010u && w1 == 0xe1a04001u && (w2 & 0x0f000000u) == 0x0b000000u &&
            s3c2400_debug_read32(g->soc, a + 0x48u) == 0xe1a01004u && (s3c2400_debug_read32(g->soc, a + 0x50u) & 0x0f000000u) == 0x0b000000u) {
            if (!g->direct_hle_file_size_addr) g->direct_hle_file_size_addr = a;
            s3c2400_write32(g->soc, a, 0xef070004u);
        }
        if (w0 == 0xe92d4010u && w1 == 0xe1a04001u && (w2 & 0x0f000000u) == 0x0b000000u &&
            s3c2400_debug_read32(g->soc, a + 0x40u) == 0xe1a01004u &&
            direct_arm_bl_to(a + 0x50u, s3c2400_debug_read32(g->soc, a + 0x50u), GP32_RAM_BASE + 0x0010a150u)) {
            g->direct_hle_file_size_addr = a;
            s3c2400_write32(g->soc, a, 0xef070004u);
        }
        if (w0 == 0xe92d40f0u && w1 == 0xe51f6b14u && w2 == 0xe3a07000u && w3 == 0xe5867040u) {
            s3c2400_write32(g->soc, a, 0xef070008u);
        }
        /* GPSDK PCM library.  BIOSless direct loading has no firmware IIS/PCM
           service behind the library's SWI wrappers, so patch the stable GPSDK
           PCM entry points and stream guest RAM directly into the emulator's
           audio sink.  This stays at the SDK ABI level instead of keying on a
           title-specific address. */
        if (!g->direct_hle_pcm_env_addr && w0 == 0xe59f302cu && w1 == 0xe593c000u && w2 == 0xe35c0000u && w3 == 0x03a00002u) {
            uint32_t lit = a + 8u + (w0 & 0xfffu);
            g->direct_hle_pcm_env_addr = direct_ram_range(g, lit, 4u) ? s3c2400_debug_read32(g->soc, lit) : 0u;
            s3c2400_write32(g->soc, a, 0xef070009u);
        }
        if (!g->direct_hle_pcm_play_addr && w0 == 0xe92d4ff0u && w1 == 0xe1a04000u && w2 == 0xe51f00d8u &&
            w3 == 0xe1a06001u && w4 == 0xe5900000u && s3c2400_debug_read32(g->soc, a + 0x14u) == 0xe1a05002u &&
            s3c2400_debug_read32(g->soc, a + 0x18u) == 0xe3500000u && s3c2400_debug_read32(g->soc, a + 0x1cu) == 0x03a00002u) {
            g->direct_hle_pcm_play_addr = a;
            s3c2400_write32(g->soc, a, 0xef07000au);
        }
        if (!g->direct_hle_pcm_init_addr && (w0 == 0xe92d43f0u || w0 == 0xe92d41f0u) &&
            w1 == 0xe24dd01cu && w2 == 0xe1a06000u && w3 == 0xe1a05001u &&
            w4 == 0xe3a07000u &&
            ((w0 == 0xe92d43f0u && s3c2400_debug_read32(g->soc, a + 0x24u) == 0xe3a00a01u) ||
             (w0 == 0xe92d41f0u && s3c2400_debug_read32(g->soc, a + 0x18u) == 0xe3a00a01u))) {
            /*
             * GP32 SDK PCM init has two register-save variants in commercial
             * stripped/self-loader GXBs.  Older HLE matched only the r4-r9/lr
             * prologue (E92D43F0).  Little Wizard, Rally Pop, Tanggle's Magic
             * Square, and Dooly use the r4-r8/lr variant (E92D41F0), whose
             * unpatched body falls through a partly packed jump-table path and
             * can jump into non-code before the actual game loop starts.  Treat
             * both as the same SDK ABI and service them through the PCM HLE.
             */
            g->direct_hle_pcm_init_addr = a;
            s3c2400_write32(g->soc, a, 0xef07000bu);
        }
        if (!g->direct_hle_pcm_stop_addr && w0 == 0xe92d41f0u && w1 == 0xe24dd01cu && w3 == 0xe51f8398u) {
            g->direct_hle_pcm_stop_addr = a;
            s3c2400_write32(g->soc, a, 0xef07000cu);
        }
        if (!g->direct_hle_pcm_remove_addr && w0 == 0xe3500000u && w1 == 0x01a0f00eu && w2 == 0xe92d4010u && w3 == 0xe1a04000u &&
            s3c2400_debug_read32(g->soc, a + 0x10u) == 0xebfbdf9du) {
            g->direct_hle_pcm_remove_addr = a;
            s3c2400_write32(g->soc, a, 0xef07000du);
        }
        if (!g->direct_hle_pcm_lock_addr && w0 == 0xe52de004u && w1 == 0xe51fc2a0u && w2 == 0xe3a03000u &&
            w3 == 0xe083e103u && w4 == 0xe79ce10eu) {
            g->direct_hle_pcm_lock_addr = a;
            s3c2400_write32(g->soc, a, 0xef07000eu);
        }
        if (!g->direct_hle_pcm_only_kill_addr && w0 == 0xe92d4070u && w1 == 0xe51f62ecu && w2 == 0xe1a04000u && w3 == 0xe3a05000u) {
            g->direct_hle_pcm_only_kill_addr = a;
            s3c2400_write32(g->soc, a, 0xef07000fu);
        }
        if (!g->direct_hle_sound_dispatch_addr && w0 == 0xe92d4030u && w1 == 0xe24dd040u && w2 == 0xe1a05000u && w3 == 0xe1a04001u && w4 == 0xe59f1054u &&
            s3c2400_debug_read32(g->soc, a + 0x14u) == 0xe1a0000du && s3c2400_debug_read32(g->soc, a + 0x18u) == 0xe3a02040u) {
            uint32_t literal = a + 0x10u + 8u + (w4 & 0xfffu);
            g->direct_hle_sound_dispatch_addr = a;
            if (direct_ram_range(g, literal, 4u)) g->direct_hle_sound_table_addr = s3c2400_debug_read32(g->soc, literal);
            s3c2400_write32(g->soc, a, 0xef070006u);
        }
        if (!g->direct_hle_sound_play_addr && w0 == 0xe92d40f8u && w1 == 0xe51f6018u && w2 == 0xe1a04000u && w3 == 0xe5960030u && w4 == 0xe1a05001u) {
            uint32_t state_lit = a + 12u - 0x18u;
            g->direct_hle_sound_play_addr = a;
            if (direct_ram_range(g, state_lit, 4u)) g->direct_hle_sound_state_addr = s3c2400_debug_read32(g->soc, state_lit);
            /* Keep this as a fallback for titles that call the GPSDK PCM path directly
               instead of going through the small sample-index dispatcher above. */
            if (!g->direct_hle_sound_dispatch_addr) s3c2400_write32(g->soc, a, 0xef070007u);
        }
        /* Samsung/Mirko SMFS wrapper discovery: prologue shape, then a BL to
           the SWI-0x11 card gate, then a BL to the closed Samsung driver entry.
           Patch the wrapper entry to the asset-backed HLE SWI for that op. */
        if (w0 == 0xe1a0c00du) {
            int prologue_like = 0;
            if (w1 == 0xe92dd870u)
                prologue_like = w2 == 0xe1a06002u && w3 == 0xe1a04000u && w4 == 0xe24cb004u &&
                    s3c2400_debug_read32(g->soc, a + 0x14u) == 0xe1a05001u;
            else if (w1 == 0xe92dd8f0u)
                prologue_like = w2 == 0xe1a06002u && w3 == 0xe1a07003u && w4 == 0xe24cb004u &&
                    s3c2400_debug_read32(g->soc, a + 0x14u) == 0xe1a04000u &&
                    s3c2400_debug_read32(g->soc, a + 0x18u) == 0xe1a05001u;
            else if (w1 == 0xe92dd810u)
                prologue_like = w2 == 0xe24cb004u && w3 == 0xe1a04000u;
            if (prologue_like) {
                int saw_gate = 0;
                uint32_t hle_id = 0u;
                for (uint32_t off = 0x10u; off <= 0x84u; off += 4u) {
                    uint32_t pc = a + off;
                    uint32_t insn = s3c2400_debug_read32(g->soc, pc);
                    if (!direct_arm_is_bl(insn)) continue;
                    uint32_t t = direct_arm_branch_target(pc, insn);
                    if (!saw_gate) {
                        if (direct_smfs_card_gate(g, t)) saw_gate = 1;
                        continue;
                    }
                    hle_id = direct_smfs_driver_hle_id(g, t);
                    if (hle_id) break;
                }
                if (saw_gate && hle_id) s3c2400_write32(g->soc, a, 0xef070000u | hle_id);
            }
        }
    }
}

/* Fingerprint the image extent the loader currently knows about.  A staged
   placement hands its own range to direct_scan_file_hle_range because that
   code did not exist in RAM when the load-time scan ran. */
static void direct_scan_file_hle(gp32_t *g) {
    if (!g) return;
    uint32_t start = GP32_RAM_BASE;
    uint32_t end = g->direct_fxe_image_end;
    uint32_t ram_end = GP32_RAM_BASE + (uint32_t)s3c2400_ram_size(g->soc);
    if (end <= start || end > ram_end) end = ram_end;
    direct_scan_file_hle_range(g, start, end);
}



static uint32_t direct_fpk_handle_seek(gp32_t *g, uint32_t h, uint32_t seek_mode, int32_t offset, uint32_t old_offset_addr) {
    size_t *posp = NULL;
    const fpk_asset_t *a = direct_handle_asset(g, h, &posp);
    if (!a || !posp) return 0x0bu;
    size_t old_pos = *posp;
    if (old_offset_addr) direct_write32_if_ram(g, old_offset_addr, (uint32_t)old_pos);
    int64_t base = 0;
    if (seek_mode == 0u) base = (int64_t)old_pos;       /* FROM_CURRENT */
    else if (seek_mode == 1u) base = 0;                 /* FROM_BEGIN */
    else if (seek_mode == 2u) base = (int64_t)a->size;  /* FROM_END */
    else return 0x26u;
    int64_t np = base + (int64_t)offset;
    if (np < 0) np = 0;
    if ((uint64_t)np > (uint64_t)a->size) np = (int64_t)a->size;
    *posp = (size_t)np;
    return 0u;
}


static void direct_write16_if_ram(gp32_t *g, uint32_t addr, uint16_t value) {
    direct_write8_if_ram(g, addr + 0u, (uint8_t)(value & 0xffu));
    direct_write8_if_ram(g, addr + 1u, (uint8_t)(value >> 8));
}

static int direct_asset_basename_83(const char *path, char name[11]) {
    if (!path || !name) return 0;
    const char *base = strrchr(path, '/');
    const char *base2 = strrchr(path, '\\');
    if (base2 && (!base || base2 > base)) base = base2;
    base = base ? base + 1 : path;
    memset(name, ' ', 11u);
    size_t n = 0;
    while (base[n] && base[n] != '.' && n < 8u) {
        unsigned char c = (unsigned char)base[n];
        if (c <= 0x20u || c == '/' || c == '\\') return 0;
        name[n] = (char)toupper(c);
        ++n;
    }
    if (base[n] && base[n] != '.') return 0;
    if (base[n] == '.') {
        ++n;
        size_t e = 0;
        while (base[n] && e < 3u) {
            unsigned char c = (unsigned char)base[n++];
            if (c <= 0x20u || c == '/' || c == '\\' || c == '.') return 0;
            name[8u + e++] = (char)toupper(c);
        }
        if (base[n]) return 0;
    }
    return name[0] != ' ';
}

static int direct_asset_path_has_prefix_file(const fpk_asset_t *a, const char *prefix) {
    if (!a || !prefix || !prefix[0]) return 0;
    char p[320];
    direct_norm_path(a->path, p, sizeof(p));
    size_t lp = strlen(prefix);
    if (strncmp(p, prefix, lp) != 0) return 0;
    const char *tail = p + lp;
    return tail[0] != '\0' && strchr(tail, '/') == NULL;
}

static uint32_t direct_arm_ldr_pc_literal_value(gp32_t *g, uint32_t insn_addr) {
    if (!g || !direct_ram_range(g, insn_addr, 4u)) return 0u;
    uint32_t insn = s3c2400_debug_read32(g->soc, insn_addr);
    if ((insn & 0x0e100000u) != 0x04100000u) return 0u;
    if (((insn >> 16) & 0x0fu) != 15u) return 0u;
    uint32_t imm = insn & 0xfffu;
    uint32_t addr = insn_addr + 8u;
    if (insn & 0x00800000u) addr += imm;
    else addr -= imm;
    if (!direct_ram_range(g, addr, 4u)) return 0u;
    return s3c2400_debug_read32(g->soc, addr);
}

static void direct_write_fat_entry(gp32_t *g, uint32_t addr, const fpk_asset_t *a, uint16_t fallback_cluster) {
    char name[11];
    if (!direct_asset_basename_83(a ? a->path : NULL, name)) return;
    for (uint32_t i = 0; i < 32u; ++i) direct_write8_if_ram(g, addr + i, 0u);
    for (uint32_t i = 0; i < 11u; ++i) direct_write8_if_ram(g, addr + i, (uint8_t)name[i]);
    direct_write8_if_ram(g, addr + 11u, a->attr ? a->attr : 0x20u);
    direct_write16_if_ram(g, addr + 26u, a->first_cluster ? a->first_cluster : fallback_cluster);
    direct_write32_if_ram(g, addr + 28u, (uint32_t)(a->size > 0xffffffffu ? 0xffffffffu : a->size));
}

static void direct_write_dot_entry(gp32_t *g, uint32_t addr, int parent) {
    for (uint32_t i = 0; i < 32u; ++i) direct_write8_if_ram(g, addr + i, 0u);
    direct_write8_if_ram(g, addr + 0u, '.');
    if (parent) direct_write8_if_ram(g, addr + 1u, '.');
    for (uint32_t i = parent ? 2u : 1u; i < 11u; ++i) direct_write8_if_ram(g, addr + i, ' ');
    direct_write8_if_ram(g, addr + 11u, 0x10u);
    direct_write16_if_ram(g, addr + 26u, parent ? 3u : 4u);
}

static unsigned direct_write_smc_dir_entries_limited(gp32_t *g, uint32_t addr, const char *prefix, uint16_t first_fallback_cluster, unsigned max_entries) {
    unsigned n = 0;
    if (!g || !prefix || !max_entries) return 0;
    for (size_t i = 0; i < g->direct_fpk_asset_count && n < max_entries; ++i) {
        const fpk_asset_t *a = &g->direct_fpk_assets[i];
        if (!direct_asset_path_has_prefix_file(a, prefix)) continue;
        direct_write_fat_entry(g, addr + n * 32u, a, (uint16_t)(first_fallback_cluster + n));
        ++n;
    }
    for (uint32_t i = 0; i < 32u; ++i) direct_write8_if_ram(g, addr + n * 32u + i, 0u);
    return n;
}

static unsigned direct_write_smc_dir_entries(gp32_t *g, uint32_t addr, const char *prefix, uint16_t first_fallback_cluster) {
    return direct_write_smc_dir_entries_limited(g, addr, prefix, first_fallback_cluster, 64u);
}

static unsigned direct_count_assets_in_prefix(gp32_t *g, const char *prefix) {
    unsigned n = 0;
    if (!g || !prefix || !prefix[0]) return 0;
    for (size_t i = 0; i < g->direct_fpk_asset_count; ++i) {
        if (direct_asset_path_has_prefix_file(&g->direct_fpk_assets[i], prefix)) ++n;
    }
    return n;
}

static void direct_smc_asset_prefixes(gp32_t *g, char *dat_prefix, size_t dat_len, char *game_prefix, size_t game_len) {
    if (dat_prefix && dat_len) dat_prefix[0] = '\0';
    if (game_prefix && game_len) game_prefix[0] = '\0';
    if (!g) return;

    char exe[320];
    const char *src = g->direct_smc_executable_path[0] ? g->direct_smc_executable_path : g->direct_smc_game_dir;
    direct_norm_path(src, exe, sizeof(exe));

    char parent[320];
    snprintf(parent, sizeof(parent), "%s", exe);
    char *slash = strrchr(parent, '/');
    char *base = parent;
    if (slash) {
        *slash = '\0';
        base = slash + 1;
    } else {
        parent[0] = '\0';
    }

    char stem[128] = {0};
    if (base && base[0]) {
        size_t n = 0;
        while (base[n] && base[n] != '.' && n + 1u < sizeof(stem)) { stem[n] = base[n]; ++n; }
        stem[n] = '\0';
    }

    char selected[360];
    if (parent[0]) snprintf(selected, sizeof(selected), "%s/", parent);
    else selected[0] = '\0';

    if (parent[0] && stem[0]) {
        char subdir[360];
        snprintf(subdir, sizeof(subdir), "%s/%s/", parent, stem);
        if (direct_count_assets_in_prefix(g, subdir) > 0u) snprintf(selected, sizeof(selected), "%s", subdir);
    }

    if (dat_prefix && dat_len) snprintf(dat_prefix, dat_len, "%sdat/", selected);
    if (game_prefix && game_len) snprintf(game_prefix, game_len, "%s", selected);
}

static int direct_find_retail_dir_pool(gp32_t *g, uint32_t start, uint32_t end, uint32_t *pool, uint32_t *head_ptr) {
    if (pool) *pool = 0u;
    if (head_ptr) *head_ptr = 0u;
    if (!g || start >= end) return 0;
    if (start < GP32_RAM_BASE) start = GP32_RAM_BASE;
    uint32_t ram_end = GP32_RAM_BASE + (uint32_t)s3c2400_ram_size(g->soc);
    if (end > ram_end) end = ram_end;
    for (uint32_t a = start; a + 0x64u < end; a += 4u) {
        uint32_t w0 = s3c2400_debug_read32(g->soc, a + 0x00u);
        uint32_t w1 = s3c2400_debug_read32(g->soc, a + 0x04u);
        uint32_t w2 = s3c2400_debug_read32(g->soc, a + 0x08u);
        uint32_t w3 = s3c2400_debug_read32(g->soc, a + 0x0cu);
        uint32_t w4 = s3c2400_debug_read32(g->soc, a + 0x10u);
        if (w0 == 0xe59f1034u && w1 == 0xe3a00000u && w2 == 0xe0802300u &&
            w3 == 0xe0813182u && w4 == 0xe2833f82u) {
            uint32_t p = direct_arm_ldr_pc_literal_value(g, a + 0x00u);
            uint32_t h = direct_arm_ldr_pc_literal_value(g, a + 0x30u);
            if (direct_ram_range(g, p, 0x4000u)) {
                if (pool) *pool = p;
                if (head_ptr) *head_ptr = h;
                return 1;
            }
        }
    }
    return 0;
}

static void direct_seed_retail_asset_fat(gp32_t *g, uint32_t fat_buf, uint32_t cluster_bytes) {
    if (!g || !fat_buf || !cluster_bytes || !direct_ram_range(g, fat_buf, 4096u)) return;
    uint32_t max_cluster = 0u;
    for (size_t i = 0; i < g->direct_fpk_asset_count; ++i) {
        const fpk_asset_t *a = &g->direct_fpk_assets[i];
        if (!a->first_cluster) continue;
        uint32_t clusters = (uint32_t)((a->size + cluster_bytes - 1u) / cluster_bytes);
        if (!clusters) clusters = 1u;
        uint32_t end = (uint32_t)a->first_cluster + clusters + 1u;
        if (end > max_cluster) max_cluster = end;
    }
    if (max_cluster < 1024u) max_cluster = 1024u;
    if (max_cluster > 8192u) max_cluster = 8192u;
    uint32_t bytes = max_cluster * 2u;
    if (!direct_ram_range(g, fat_buf, bytes)) return;
    for (uint32_t i = 0; i < max_cluster; ++i) direct_write16_if_ram(g, fat_buf + i * 2u, 0xffffu);
    for (size_t i = 0; i < g->direct_fpk_asset_count; ++i) {
        const fpk_asset_t *a = &g->direct_fpk_assets[i];
        if (!a->first_cluster) continue;
        uint32_t clusters = (uint32_t)((a->size + cluster_bytes - 1u) / cluster_bytes);
        if (!clusters) clusters = 1u;
        for (uint32_t c = 0; c < clusters; ++c) {
            uint32_t cl = (uint32_t)a->first_cluster + c;
            if (cl >= max_cluster) break;
            uint16_t next = (c + 1u < clusters) ? (uint16_t)(cl + 1u) : 0xffffu;
            direct_write16_if_ram(g, fat_buf + cl * 2u, next);
        }
    }
}

static int direct_retail_smc_init_hle(gp32_t *g, uint32_t init_pc) {
    if (!g || !g->direct_smc_game_dir[0]) return 0;

    /* Commercial GXE/GXC builds do not all use the same statically linked
       BIOS SmartMedia work area.  The older HLE seed used Princess Maker 2's
       addresses, which let that title enumerate assets but left Dooly Soccer's
       relocated callback table all zeros; its loader then reached rendering
       with uninitialised asset/surface pointers.  Decode the generic SDK init
       thunk literals at the patched call site and populate the title's own
       card-ready flag and callback tables.  Keep the historical work-area seed
       below as a compatibility fallback for builds whose init thunk does not
       expose those literals. */
    direct_install_stubs(g);
    direct_install_smc_callbacks(g);
    static const uint32_t cb_offsets[] = {
        0x000u, 0x040u, 0x078u, 0x0b0u, 0x0dcu, 0x114u, 0x138u,
        0x15cu, 0x180u, 0x1a4u, 0x1c8u, 0x1ecu, 0x210u
    };
    if (init_pc && direct_ram_range(g, init_pc, 0x140u)) {
        uint32_t state_from_lit = direct_arm_ldr_pc_literal_value(g, init_pc + 4u);
        if (direct_ram_range(g, state_from_lit, 0x44u)) {
            direct_write32_if_ram(g, state_from_lit + 0x40u, 1u);
        }
        uint32_t src_table = direct_arm_ldr_pc_literal_value(g, init_pc + 0x24u);
        uint32_t dst_table = direct_arm_ldr_pc_literal_value(g, init_pc + 0x34u);
        for (uint32_t i = 0; i < (uint32_t)(sizeof(cb_offsets) / sizeof(cb_offsets[0])); ++i) {
            uint32_t v = direct_smc_cb_base_addr(g) + cb_offsets[i];
            if (direct_ram_range(g, src_table + i * 4u, 4u)) direct_write32_if_ram(g, src_table + i * 4u, v);
            if (direct_ram_range(g, dst_table + i * 4u, 4u)) direct_write32_if_ram(g, dst_table + i * 4u, v);
        }
    }

    if (init_pc && direct_ram_range(g, init_pc, 0x140u)) {
        uint32_t state_from_lit = direct_arm_ldr_pc_literal_value(g, init_pc + 4u);
        uint32_t pool = 0u, head_ptr = 0u;
        uint32_t scan_end = g->direct_fxe_image_end;
        if (scan_end < init_pc + 0x4000u) scan_end = init_pc + 0x4000u;
        if (direct_find_retail_dir_pool(g, init_pc, scan_end, &pool, &head_ptr)) {
            char dat_prefix[360], game_prefix[360];
            direct_smc_asset_prefixes(g, dat_prefix, sizeof(dat_prefix), game_prefix, sizeof(game_prefix));
            unsigned n = direct_write_smc_dir_entries_limited(g, pool + 4u, dat_prefix, 5u, 16u);
            if (!n) n = direct_write_smc_dir_entries_limited(g, pool + 4u, game_prefix, 5u, 16u);
            direct_write32_if_ram(g, pool, pool + 4u + n * 32u + 4u);
            if (head_ptr && direct_ram_range(g, head_ptr, 4u)) direct_write32_if_ram(g, head_ptr, pool);
            if (direct_ram_range(g, state_from_lit, 0x58u)) {
                uint32_t fat_buf = (pool + 0x1b000u + 15u) & ~15u;
                if (!direct_ram_range(g, fat_buf, 0x4000u)) fat_buf = (pool + 0x8000u + 15u) & ~15u;
                direct_write32_if_ram(g, state_from_lit + 0x04u, fat_buf);
                direct_write32_if_ram(g, state_from_lit + 0x08u, 42u);
                direct_write32_if_ram(g, state_from_lit + 0x0cu, 3u);
                direct_write32_if_ram(g, state_from_lit + 0x10u, 48u);
                direct_write32_if_ram(g, state_from_lit + 0x14u, 16u);
                direct_write32_if_ram(g, state_from_lit + 0x18u, 64u);
                direct_write32_if_ram(g, state_from_lit + 0x1cu, 1000u);
                direct_write32_if_ram(g, state_from_lit + 0x24u, 10u);
                direct_write32_if_ram(g, state_from_lit + 0x2cu, 512u);
                direct_write32_if_ram(g, state_from_lit + 0x44u, pool);
                direct_write32_if_ram(g, state_from_lit + 0x48u, 1u);
                direct_write32_if_ram(g, state_from_lit + 0x4cu, 10u);
                direct_write32_if_ram(g, state_from_lit + 0x50u, 4u);
                direct_write32_if_ram(g, state_from_lit + 0x54u, 512u);
                direct_seed_retail_asset_fat(g, fat_buf, 0x4000u);
            }
        }
    }

    const uint32_t state = 0x0c12b0acu;
    const uint32_t geom = 0x0c12b0f4u;
    const uint32_t dir_meta = 0x0c131ed0u;
    const uint32_t dir_list = 0x0c131f20u;

    direct_write32_if_ram(g, state + 0x40u, 1u);
    direct_write32_if_ram(g, geom + 0u, 1u);
    direct_write32_if_ram(g, geom + 4u, 10u);
    direct_write32_if_ram(g, geom + 8u, 4u);
    direct_write32_if_ram(g, geom + 12u, 512u);

    {
        static const uint32_t cb_offsets[] = {
            0x000u, 0x040u, 0x078u, 0x0b0u, 0x0dcu, 0x114u, 0x138u,
            0x15cu, 0x180u, 0x1a4u, 0x1c8u, 0x1ecu, 0x210u
        };
        const uint32_t src_table = 0x0c131d98u;
        const uint32_t dst_table = 0x0c12b14cu;
        direct_install_stubs(g);
        direct_install_smc_callbacks(g);
        for (uint32_t i = 0; i < 16u; ++i) {
            uint32_t v = direct_ret_stub_addr(g);
            if (i < (uint32_t)(sizeof(cb_offsets) / sizeof(cb_offsets[0]))) v = direct_smc_cb_base_addr(g) + cb_offsets[i];
            if (i == 13u) v = 0x80u;
            if (i == 14u) v = 0u;
            if (i == 15u) v = 2u;
            direct_write32_if_ram(g, src_table + i * 4u, v);
            direct_write32_if_ram(g, dst_table + i * 4u, v);
        }
    }

    char dat_prefix[360];
    char game_prefix[360];
    direct_smc_asset_prefixes(g, dat_prefix, sizeof(dat_prefix), game_prefix, sizeof(game_prefix));

    direct_zero_if_ram(g, dir_meta, 0x280u);
    direct_write8_if_ram(g, dir_meta + 0u, 'g');
    direct_write8_if_ram(g, dir_meta + 1u, 'p');
    direct_write32_if_ram(g, dir_meta + 0x0cu, dir_meta + 0x214u);
    direct_write_dot_entry(g, dir_meta + 0x10u, 0);
    direct_write_dot_entry(g, dir_meta + 0x30u, 1);

    unsigned n = direct_write_smc_dir_entries(g, dir_list, dat_prefix, 5u);
    if (!n) n = direct_write_smc_dir_entries(g, dir_list, game_prefix, 5u);
    for (uint32_t i = 0; i < (n + 1u) * 32u; ++i) {
        uint8_t v = 0u;
        uint32_t ba = (dir_list + i) & ~3u;
        uint32_t bw = s3c2400_debug_read32(g->soc, ba);
        v = (uint8_t)(bw >> (((dir_list + i) & 3u) * 8u));
        direct_write8_if_ram(g, dir_meta + 0x50u + i, v);
    }
    return 1;
}

static int direct_file_hle_swi(gp32_t *g, arm920t_t *cpu, uint32_t id, uint32_t pc) {
    if (!g || !cpu) return 0;
    uint32_t lr = arm920t_get_reg(cpu, 14);
    if (id == 0x21u) {
        if (pc != direct_stub_addr(g) + 0xa4u) return 0;
        uint32_t sp = arm920t_get_reg(cpu, 13u), frame[8];
        if (!direct_ram_range(g, sp, sizeof(frame))) return 0;
        for (unsigned i = 0; i < GP32_ARRAY_COUNT(frame); ++i)
            frame[i] = s3c2400_debug_read32(g->soc, sp + i * 4u);
        if ((frame[7] & 31u) != (arm920t_get_cpsr(cpu) & 31u) ||
            (frame[6] & ((frame[7] & ARM_T_FLAG) ? 1u : 3u))) return 0;
        const unsigned regs[] = {0u, 1u, 2u, 3u, 12u, 14u, 15u};
        arm920t_set_cpsr(cpu, frame[7]);
        for (unsigned i = 0; i < GP32_ARRAY_COUNT(regs); ++i)
            arm920t_set_reg(cpu, regs[i], frame[i]);
        arm920t_set_reg(cpu, 13u, sp + 32u);
        arm920t_stop_run(cpu);
        return 1;
    }
    if (id == 0x20u) {
        /* Only our active callback's return stub may end the host call. A
         * matching guest SWI elsewhere must follow normal exception dispatch. */
        if ((g->direct_callback.owner != DIRECT_CB_REFILL && g->direct_callback.owner != DIRECT_CB_TIMER) ||
            !g->direct_hle_callback_running || g->direct_callback.suspended ||
            pc != direct_callback_return_stub_addr(g)) return 0;
        g->direct_hle_callback_returned = 1u;
        arm920t_set_reg(cpu, 15, lr);
        arm920t_stop_run(cpu);
        return 1;
    }
    if (!g->direct_fpk_asset_count) return 0;
    if (id == 1u) {
        const fpk_asset_t *a = direct_fpk_asset_from_cpu_path(g, arm920t_get_reg(cpu, 0));
        direct_trace_file_path(g, "open/swi", arm920t_get_reg(cpu, 0), a);
        if (!a) { arm920t_set_reg(cpu, 0, 0x24u); arm920t_set_reg(cpu, 15, lr); return 1; }
        uint32_t h = direct_alloc_fpk_handle(g, a);
        if (!h) { arm920t_set_reg(cpu, 0, 0x10u); arm920t_set_reg(cpu, 15, lr); return 1; }
        direct_write32_if_ram(g, arm920t_get_reg(cpu, 2), h);
        arm920t_set_reg(cpu, 0, 0u);
        arm920t_set_reg(cpu, 15, lr);
        return 1;
    }
    if (id == 2u) {
        size_t *posp = NULL;
        const fpk_asset_t *a = direct_handle_asset(g, arm920t_get_reg(cpu, 0), &posp);
        if (!a || !posp) { arm920t_set_reg(cpu, 0, 0x0bu); arm920t_set_reg(cpu, 15, lr); return 1; }
        uint32_t dst = arm920t_get_reg(cpu, 1);
        uint32_t want = arm920t_get_reg(cpu, 2);
        uint32_t out_count = arm920t_get_reg(cpu, 3);
        size_t avail = (*posp < a->size) ? (a->size - *posp) : 0u;
        size_t n = want < avail ? (size_t)want : avail;
        direct_trace_file_io(g, "read/swi", arm920t_get_reg(cpu, 0), dst, want, *posp, n, a, 0u);
        if (n && direct_ram_range(g, dst, (uint32_t)n)) {
            for (size_t i = 0; i < n; ++i) s3c2400_write8(g->soc, dst + (uint32_t)i, a->data[*posp + i]);
        }
        *posp += n;
        direct_write32_if_ram(g, out_count, (uint32_t)n);
        arm920t_set_reg(cpu, 0, 0u);
        arm920t_set_reg(cpu, 15, lr);
        return 1;
    }
    if (id == 3u) {
        uint32_t h = arm920t_get_reg(cpu, 0) & 31u;
        if (h < 32u) memset(&g->direct_fpk_handles[h], 0, sizeof(g->direct_fpk_handles[h]));
        arm920t_set_reg(cpu, 0, 0u);
        arm920t_set_reg(cpu, 15, lr);
        return 1;
    }
    if (id == 4u) {
        size_t *posp = NULL;
        const fpk_asset_t *a = direct_fpk_asset_from_cpu_path(g, arm920t_get_reg(cpu, 0));
        if (!a) a = direct_handle_asset(g, arm920t_get_reg(cpu, 0), &posp);
        direct_trace_file_path(g, "size/swi", arm920t_get_reg(cpu, 0), a);
        if (!a) { arm920t_set_reg(cpu, 0, 0x24u); arm920t_set_reg(cpu, 15, lr); return 1; }
        direct_note_asset_autoload(g, a);
        direct_process_asset_autoload(g);
        direct_write32_if_ram(g, arm920t_get_reg(cpu, 1), (uint32_t)a->size);
        arm920t_set_reg(cpu, 0, 0u);
        arm920t_set_reg(cpu, 15, lr);
        return 1;
    }
    if (id == 5u) {
        size_t *dbg_posp = NULL;
        const fpk_asset_t *dbg_a = direct_handle_asset(g, arm920t_get_reg(cpu, 0), &dbg_posp);
        uint32_t st = direct_fpk_handle_seek(g, arm920t_get_reg(cpu, 0), arm920t_get_reg(cpu, 1), (int32_t)arm920t_get_reg(cpu, 2), arm920t_get_reg(cpu, 3));
        direct_trace_file_io(g, "seek/swi", arm920t_get_reg(cpu, 0), arm920t_get_reg(cpu, 1), arm920t_get_reg(cpu, 2), dbg_posp ? *dbg_posp : 0u, 0u, dbg_a, st);
        arm920t_set_reg(cpu, 0, st);
        arm920t_set_reg(cpu, 15, lr);
        return 1;
    }
    if (id == 6u) {
        uint32_t sound_id = arm920t_get_reg(cpu, 0);
        uint32_t mode = arm920t_get_reg(cpu, 1);
        if (mode == 2u) direct_stop_sef(g);
        else if (g->direct_hle_sound_table_addr && sound_id < 64u && mode == 0u) {
            uint32_t path_addr = s3c2400_debug_read32(g->soc, g->direct_hle_sound_table_addr + sound_id * 4u);
            direct_play_sef_path(g, path_addr);
        }
        arm920t_set_reg(cpu, 0, 0u);
        arm920t_set_reg(cpu, 15, lr);
        return 1;
    }
    if (id == 7u) {
        direct_play_sef_path(g, arm920t_get_reg(cpu, 0));
        arm920t_set_reg(cpu, 0, 0u);
        arm920t_set_reg(cpu, 15, lr);
        return 1;
    }
    if (id == 8u) {
        direct_retail_smc_init_hle(g, pc);
        arm920t_set_reg(cpu, 0, 0u);
        arm920t_set_reg(cpu, 15, lr);
        return 1;
    }
    if (id == 9u) { /* GpPcmEnvGet(PCM_SR*, PCM_BIT*, int*) */
        if (!g->direct_hle_pcm_initialized) { arm920t_set_reg(cpu, 0, 2u); arm920t_set_reg(cpu, 15, lr); return 1; }
        direct_write8_if_ram(g, arm920t_get_reg(cpu, 0), (uint8_t)g->direct_hle_pcm_sr);
        direct_write8_if_ram(g, arm920t_get_reg(cpu, 1), (uint8_t)g->direct_hle_pcm_bit_count);
        direct_write32_if_ram(g, arm920t_get_reg(cpu, 2), g->direct_hle_pcm_rate);
        arm920t_set_reg(cpu, 0, 0u);
        arm920t_set_reg(cpu, 15, lr);
        return 1;
    }
    if (id == 10u) { /* GpPcmPlay(src, size, repeatflag) */
        if (!g->direct_hle_pcm_initialized) {
            g->direct_hle_pcm_initialized = 1u;
            g->direct_hle_pcm_sr = 0u;
            g->direct_hle_pcm_bit_count = 1u;
            g->direct_hle_pcm_rate = direct_pcm_rate_from_sr(0u);
            g->direct_hle_pcm_stereo = 0u;
            g->direct_hle_pcm_bits = 16u;
        }
        uint32_t ch = 0xffffffffu;
        int ok = direct_start_pcm(g, arm920t_get_reg(cpu, 0), arm920t_get_reg(cpu, 1), arm920t_get_reg(cpu, 2), &ch);
        GP32_UNUSED(ch);
        arm920t_set_reg(cpu, 0, ok ? 0u : 1u);
        arm920t_set_reg(cpu, 15, lr);
        return 1;
    }
    if (id == 11u) { /* GpPcmInit(sr, bit_count) */
        uint32_t sr = arm920t_get_reg(cpu, 0);
        uint32_t bit = arm920t_get_reg(cpu, 1);
        if (sr > 5u) sr = 0u;
        if (bit > 1u) bit = 1u;
        g->direct_hle_pcm_initialized = 1u;
        g->direct_hle_pcm_sr = sr;
        g->direct_hle_pcm_bit_count = bit;
        g->direct_hle_pcm_rate = direct_pcm_rate_from_sr(sr);
        g->direct_hle_pcm_stereo = direct_pcm_stereo_from_sr(sr);
        g->direct_hle_pcm_bits = bit ? 16u : 8u;
        g->direct_hle_pcm_accum = 0;
        direct_pcm_update_cursor(g);
        arm920t_set_reg(cpu, 0, g->direct_hle_pcm_rate);
        arm920t_set_reg(cpu, 15, lr);
        return 1;
    }
    if (id == 12u) { /* GpPcmStop() */
        direct_stop_pcm(g);
        arm920t_set_reg(cpu, 0, 0u);
        arm920t_set_reg(cpu, 15, lr);
        return 1;
    }
    if (id == 13u) { /* GpPcmRemove(src) */
        uint32_t src = arm920t_get_reg(cpu, 0);
        direct_stop_pcm_src(g, src);
        arm920t_set_reg(cpu, 0, 0u);
        arm920t_set_reg(cpu, 15, lr);
        return 1;
    }
    if (id == 14u) { /* GpPcmLock(src, &idx_buf, &addr_of_playing_buf) */
        uint32_t src = arm920t_get_reg(cpu, 0);
        uint32_t found = GP32_DIRECT_PCM_CHANNELS;
        for (uint32_t ch = 0; ch < GP32_DIRECT_PCM_CHANNELS; ++ch) {
            if (g->direct_hle_pcm_ch[ch].active && (!src || g->direct_hle_pcm_ch[ch].src_addr == src)) { found = ch; break; }
        }
        if (found < GP32_DIRECT_PCM_CHANNELS) {
            direct_pcm_update_cursor(g);
            direct_write32_if_ram(g, arm920t_get_reg(cpu, 1), found);
            direct_write32_if_ram(g, arm920t_get_reg(cpu, 2), direct_pcm_cursor_addr_channel(g, found));
            arm920t_set_reg(cpu, 0, 1u);
        } else {
            arm920t_set_reg(cpu, 0, 0u);
        }
        arm920t_set_reg(cpu, 15, lr);
        return 1;
    }
    if (id == 15u) { /* GpPcmOnlyKill(src) */
        uint32_t src = arm920t_get_reg(cpu, 0);
        direct_stop_pcm_src(g, src);
        arm920t_set_reg(cpu, 0, 0u);
        arm920t_set_reg(cpu, 15, lr);
        return 1;
    }
    return 0;
}

static int direct_try_file_hle(gp32_t *g) {
    if (!g || !g->cpu || !g->direct_fpk_asset_count) return 0;
    uint32_t pc = arm920t_get_pc(g->cpu);
    uint32_t lr = arm920t_get_reg(g->cpu, 14);
    if (pc == g->direct_hle_file_open_addr) {
        const fpk_asset_t *a = direct_fpk_asset_from_cpu_path(g, arm920t_get_reg(g->cpu, 0));
        direct_trace_file_path(g, "open/pc", arm920t_get_reg(g->cpu, 0), a);
        if (!a) { arm920t_set_reg(g->cpu, 0, 0x24u); arm920t_set_reg(g->cpu, 15, lr); return 1; }
        uint32_t h = direct_alloc_fpk_handle(g, a);
        if (!h) { arm920t_set_reg(g->cpu, 0, 0x10u); arm920t_set_reg(g->cpu, 15, lr); return 1; }
        uint32_t out = arm920t_get_reg(g->cpu, 2);
        direct_write32_if_ram(g, out, h);
        arm920t_set_reg(g->cpu, 0, 0u);
        arm920t_set_reg(g->cpu, 15, lr);
        return 1;
    }
    if (pc == g->direct_hle_file_seek_addr) {
        size_t *dbg_posp = NULL;
        const fpk_asset_t *dbg_a = direct_handle_asset(g, arm920t_get_reg(g->cpu, 0), &dbg_posp);
        uint32_t st = direct_fpk_handle_seek(g, arm920t_get_reg(g->cpu, 0), arm920t_get_reg(g->cpu, 1), (int32_t)arm920t_get_reg(g->cpu, 2), arm920t_get_reg(g->cpu, 3));
        direct_trace_file_io(g, "seek/pc", arm920t_get_reg(g->cpu, 0), arm920t_get_reg(g->cpu, 1), arm920t_get_reg(g->cpu, 2), dbg_posp ? *dbg_posp : 0u, 0u, dbg_a, st);
        arm920t_set_reg(g->cpu, 0, st);
        arm920t_set_reg(g->cpu, 15, lr);
        return 1;
    }
    if (pc == g->direct_hle_file_size_addr) {
        const fpk_asset_t *a = direct_fpk_asset_from_cpu_path(g, arm920t_get_reg(g->cpu, 0));
        direct_trace_file_path(g, "size/pc", arm920t_get_reg(g->cpu, 0), a);
        if (!a) { arm920t_set_reg(g->cpu, 0, 0x24u); arm920t_set_reg(g->cpu, 15, lr); return 1; }
        direct_note_asset_autoload(g, a);
        direct_process_asset_autoload(g);
        direct_write32_if_ram(g, arm920t_get_reg(g->cpu, 1), (uint32_t)a->size);
        arm920t_set_reg(g->cpu, 0, 0u);
        arm920t_set_reg(g->cpu, 15, lr);
        return 1;
    }
    if (pc == g->direct_hle_file_read_addr[0] || pc == g->direct_hle_file_read_addr[1]) {
        size_t *posp = NULL;
        const fpk_asset_t *a = direct_handle_asset(g, arm920t_get_reg(g->cpu, 0), &posp);
        if (!a || !posp) { arm920t_set_reg(g->cpu, 0, 0x0bu); arm920t_set_reg(g->cpu, 15, lr); return 1; }
        uint32_t dst = arm920t_get_reg(g->cpu, 1);
        uint32_t want = arm920t_get_reg(g->cpu, 2);
        uint32_t out_count = arm920t_get_reg(g->cpu, 3);
        size_t avail = (*posp < a->size) ? (a->size - *posp) : 0u;
        size_t n = want < avail ? (size_t)want : avail;
        direct_trace_file_io(g, "read/pc", arm920t_get_reg(g->cpu, 0), dst, want, *posp, n, a, 0u);
        if (n && direct_ram_range(g, dst, (uint32_t)n)) {
            for (size_t i = 0; i < n; ++i) s3c2400_write8(g->soc, dst + (uint32_t)i, a->data[*posp + i]);
        }
        *posp += n;
        direct_write32_if_ram(g, out_count, (uint32_t)n);
        arm920t_set_reg(g->cpu, 0, 0u);
        arm920t_set_reg(g->cpu, 15, lr);
        return 1;
    }
    if (pc == g->direct_hle_file_close_addr) {
        uint32_t h = arm920t_get_reg(g->cpu, 0) & 31u;
        if (h < 32u) memset(&g->direct_fpk_handles[h], 0, sizeof(g->direct_fpk_handles[h]));
        arm920t_set_reg(g->cpu, 0, 0u);
        arm920t_set_reg(g->cpu, 15, lr);
        return 1;
    }
    return 0;
}

static int direct_restore_saved_context(gp32_t *g, uint32_t task_addr);
static void direct_tick_sdk_task_sleepers(gp32_t *g, uint32_t first_task, uint32_t last_task, uint32_t ticks);
static int direct_resume_ready_sdk_task(gp32_t *g, uint32_t pc, uint32_t first_task, uint32_t last_task);

/* Firmware SWI #5 image hand-off (GPSDK packer CRT).
 *
 * A packer that cannot decompress in place decrunches its image into scratch
 * RAM and then asks the firmware to run it.  The hand-off argument points at
 * either the image itself or at a four-byte length word in front of it, and
 * the embedded GXB header declares the rom_start the image belongs at.  The
 * firmware therefore has to place the image at its declared rom_start before
 * entering it, exactly like the file loader does.
 *
 * Without that step the image stays at the scratch address while every
 * absolute address it contains still describes the relocated layout, so the
 * guest starts executing whatever the scratch address happened to hold and
 * leaves RAM.  Place it exactly as firmware ROM 0x64c8 (the routine the SWI 5
 * service at ROM 0x2298 calls) places it, in three steps:
 *   1. move the declared ROM window [hdr, hdr + (hdr[+8] - hdr[+4])) to hdr[+4],
 *   2. move the initialised RW data the decruncher left directly behind that
 *      window to hdr[+12], for hdr[+20] - hdr[+12] bytes,
 *   3. clear the declared ZI window [hdr[+20], hdr[+24]).
 * Steps 2 and 3 are what make a hand-off work at all: the image is still at its
 * scratch address when it arrives, so its .data and .bss exist nowhere else
 * yet.  Both overlapping moves follow memmove rules.  The field offsets are the
 * ones that firmware routine reads; they agree with the GXB headers in the
 * corpus, where hdr[+16] and hdr[+24] are always equal.  Returns 0 when the
 * header is absent or the declared window is not usable, in which case the
 * caller keeps the unrelocated image. */
static int direct_gxb_place_image(gp32_t *g, uint32_t hdr, uint32_t *out_hdr, uint32_t *out_end) {
    uint32_t ram_end = GP32_RAM_BASE + (uint32_t)s3c2400_ram_size(g->soc);
    if (out_end) *out_end = 0u;
    if (!direct_ram_range(g, hdr, 0x20u)) return 0;
    uint32_t rom_start = s3c2400_debug_read32(g->soc, hdr + 4u);
    uint32_t rom_end = s3c2400_debug_read32(g->soc, hdr + 8u);
    uint32_t rw_base = s3c2400_debug_read32(g->soc, hdr + 12u);
    uint32_t rw_limit = s3c2400_debug_read32(g->soc, hdr + 20u);
    uint32_t zi_limit = s3c2400_debug_read32(g->soc, hdr + 24u);
    if (rom_start == hdr) { *out_hdr = hdr; return 1; }
    if (rom_start < GP32_RAM_BASE || rom_start >= ram_end) return 0;
    if (rom_end <= rom_start || rom_end > ram_end) return 0;
    uint32_t size = rom_end - rom_start;
    if (!direct_ram_range(g, hdr, size)) return 0;
    if (rom_start < hdr) {
        for (uint32_t i = 0; i < size; ++i) s3c2400_write8(g->soc, rom_start + i, s3c2400_read8(g->soc, hdr + i));
    } else {
        for (uint32_t i = size; i-- > 0u; ) s3c2400_write8(g->soc, rom_start + i, s3c2400_read8(g->soc, hdr + i));
    }
    uint32_t rw_size = (rw_limit > rw_base) ? (rw_limit - rw_base) : 0u;
    uint32_t rw_src = hdr + size;
    if (rw_size && direct_ram_range(g, rw_src, rw_size) && direct_ram_range(g, rw_base, rw_size)) {
        if (rw_base <= rw_src || rw_base >= rw_src + rw_size) {
            for (uint32_t i = 0; i < rw_size; ++i) s3c2400_write8(g->soc, rw_base + i, s3c2400_read8(g->soc, rw_src + i));
        } else {
            for (uint32_t i = rw_size; i-- > 0u; ) s3c2400_write8(g->soc, rw_base + i, s3c2400_read8(g->soc, rw_src + i));
        }
    }
    if (zi_limit > rw_limit) direct_zero_if_ram(g, rw_limit, zi_limit - rw_limit);
    *out_hdr = rom_start;
    if (out_end) *out_end = rom_end;
    return 1;
}


/* Firmware selector 0xFF of the high SWI range: every commercial SDK CRT ends
 * its entry trampoline with "svc #0x1ff" (verified at 0x0C000190 in all 28
 * payloads).  Retail firmware v1.6.6 reaches it through the ROM dispatch at
 * 0x1004 -> 0x2888 -> 0x69d0, which uses imm24 & 0xff as the selector, so
 * 0x1FF selects the service implemented at ROM 0x6b2c.  That service:
 *   - re-installs the exception-mode stacks from the ROM table at 0x1058
 *     (0x2390; supervisor top 0x0C7AFF00),
 *   - reprograms the clock from the ROM table at 0x1090 (0x200c),
 *   - restores the default 8-bpp LCD mode and rebuilds the 5:5:5 palette
 *     (0x1804/0x23e4), clears the three launcher fields at 0x0C7B0C00,
 *     0x0C7B0D00 and 0x0C7B0E00 (0x6c04-0x6c20),
 *   - and finally branches to the continuation the caller published in
 *     FIQ-mode r12 (0x6c24-0x6c48) with IRQ and FIQ disabled in supervisor
 *     mode, leaving the pre-service CPSR in SPSR_svc.
 * The caller idiom (Astonishia Story R 0x0C000168): mask the requested mode
 * into r0/r1/r2, briefly enter FIQ mode to load r12_fiq with the instruction
 * after the SWI, return to supervisor mode and issue svc #0x1ff.
 *
 * Direct-FXE HLE previously declined 0x1FF, so the CPU took the ordinary SWI
 * exception into the zero-filled direct-mode vector page and walked memory
 * until it reached RAM again.  One deviation from the ROM routine is
 * deliberate: the guest's banked stacks stay untouched because direct mode has
 * no firmware stack table and the trampoline continues on its own frame.  The
 * clock goes back to the default table (67.8 / 33.9 MHz) as in the ROM
 * (0x6b3c-0x6b5c), and the mode switch that follows takes its LCD divider
 * from that clock. */
static int direct_handle_swi_reinit(gp32_t *g, arm920t_t *cpu) {
    arm920t_register_context_t ctx;
    arm920t_get_register_context(cpu, &ctx);
    uint32_t resume = ctx.bank_fiq[4]; /* r12_fiq is the firmware's continuation slot. */
    if (!resume) return 0;             /* No published continuation: ordinary SWI semantics. */
    uint32_t entry_cpsr = ctx.cpsr;
    direct_apply_fw_default_clock(g);
    /* Default display state: 8 bpp over surface page 0 with the standard palette. */
    g->direct_fxe_bpp = 8u;
    g->direct_fxe_lcd_enabled = 1u;
    g->direct_fxe_lcd_explicit = 0u;
    g->direct_fxe_fb_addr = 0u;
    direct_fill_default_palette(g);
    direct_set_lcd_8bpp(g, direct_default_surface_addr(0u), 0u, 1);
    /* Launcher fields the firmware clears before handing control back. */
    direct_write32_if_ram(g, 0x0c7b0c00u, 0u);
    direct_write32_if_ram(g, 0x0c7b0d00u, 0u);
    direct_write32_if_ram(g, 0x0c7b0e00u, 0u);
    ctx.spsr_svc = entry_cpsr;
    ctx.cpsr = (entry_cpsr & ~(0x1fu | 0xc0u)) | 0x13u | 0xc0u;
    ctx.r[15] = resume;
    arm920t_set_register_context(cpu, &ctx);
    return 1;
}

/*
 * Retail ROM 0x1b48, the SWI 0x0b selector-5 literal, is 0x0c79c000: the top
 * of the RAM the firmware hands to a title, below its own stacks and work
 * areas.  Direct mode answered with the host's top-of-RAM (0x0c800000), which
 * put a b2fxec stub's decrunch scratch and stack against the very top of the
 * part instead of leaving the firmware's margin.
 */
static uint32_t direct_fw_usable_ram_top(gp32_t *g) {
    uint32_t top = GP32_RAM_BASE + 0x0079c000u;
    uint32_t ram_end = GP32_RAM_BASE + (uint32_t)s3c2400_ram_size(g->soc);
    return top < ram_end ? top : ram_end;
}

static int direct_fxe_swi(void *user, arm920t_t *cpu, uint32_t imm, uint32_t pc, int thumb) {
    GP32_UNUSED(thumb);
    gp32_t *g = (gp32_t *)user;
    if (!g || !g->direct_fxe_mode || !cpu) return 0;
    if ((imm & 0xffff00u) == 0x070000u && direct_file_hle_swi(g, cpu, imm & 0xffu, pc)) return 1;
    if (direct_trace_enabled() && (imm == 0x08u || imm == 0x11u || imm == 0x13u || imm == 0x14u || imm == 0x16u)) {
        fprintf(stderr, "[direct-hle] swi imm=%03x pc=%08x lr=%08x r0=%08x r1=%08x r2=%08x r3=%08x\n",
                imm, pc, arm920t_get_reg(cpu, 14), arm920t_get_reg(cpu, 0), arm920t_get_reg(cpu, 1), arm920t_get_reg(cpu, 2), arm920t_get_reg(cpu, 3));
    }
    switch (imm) {
    case 0x104: { /* GPSDK _gp_dev_id_get: 16-byte device ID at [r0]. */
        static const uint8_t key[8] = {'S', 'A', 'N', 'G', 'H', 'Y', 'U', 'K'};
        uint32_t buffer = arm920t_get_reg(cpu, 0);
        /* Retail firmware 0x51f4 reads EEPROM offset 0x10 for 16 bytes and
         * XORs each byte with its repeating 8-byte key. r1 is not read. */
        for (uint32_t i = 0; i < 16u; ++i)
            s3c2400_write8(g->soc, buffer + i,
                           (uint8_t)(s3c2400_eeprom_read8(g->soc, 0x10u + i) ^ key[i & 7u]));
        arm920t_set_reg(cpu, 0, 0u);
        return 1;
    }
    case 0x105: { /* GPSDK _gp_e2prom_read / _gp_e2prom_write. */
        uint32_t offset = arm920t_get_reg(cpu, 0);
        uint32_t count = arm920t_get_reg(cpu, 1);
        uint32_t buffer = arm920t_get_reg(cpu, 2);
        int write = arm920t_get_reg(cpu, 3) != 0u;
        /* Retail firmware clamps signed-negative offsets to zero and exposes
         * only the first 4 KiB of the chip. Use subtraction to reject invalid
         * ranges without reproducing its signed addition overflow loophole. */
        if (offset & 0x80000000u) offset = 0u;
        if (!count || offset >= 0x1000u || count > 0x1000u - offset) {
            arm920t_set_reg(cpu, 0, 0x21u);
            return 1;
        }
        for (uint32_t i = 0; i < count; ++i) {
            if (write)
                s3c2400_eeprom_write8(g->soc, offset + i, s3c2400_read8(g->soc, buffer + i));
            else
                s3c2400_write8(g->soc, buffer + i, s3c2400_eeprom_read8(g->soc, offset + i));
        }
        arm920t_set_reg(cpu, 0, 0u);
        return 1;
    }
    case 0x08: { /* GPSDK LCD/surface service. */
        uint32_t a0 = arm920t_get_reg(cpu, 0);
        uint32_t a1 = arm920t_get_reg(cpu, 1);
        uint32_t selector = arm920t_get_reg(cpu, 2);
        switch (selector) {
        case 0u: { /* GpGraphicModeSet(gd_bpp, gp_pal) */
            uint32_t bpp = (a0 == 16u) ? 16u : 8u;
            g->direct_fxe_bpp = bpp;
            g->direct_fxe_lcd_enabled = 1u;
            if (bpp == 16u) direct_set_lcd_16bpp(g, g->direct_fxe_fb_addr ? g->direct_fxe_fb_addr : direct_default_surface_addr(0), 1);
            else direct_set_lcd_8bpp(g, g->direct_fxe_fb_addr ? g->direct_fxe_fb_addr : direct_default_surface_addr(0), a1, 1);
            arm920t_set_reg(cpu, 0, (bpp == 16u) ? 2u : 4u);
            return 1;
        }
        case 1u: { /* GpLcdSurfaceGet(&surface, idx) */
            uint32_t page = a1 & 3u;
            if (direct_ram_range(g, a0, 28u)) {
                direct_fill_lcd_surface(g, a0, page);
                if (g->direct_fxe_fb_addr == 0u || page == 0u) {
                    if (g->direct_fxe_bpp == 16u) direct_set_lcd_16bpp(g, direct_surface_addr_for_bpp(g, page), 0);
                    else direct_set_lcd_8bpp(g, direct_surface_addr_for_bpp(g, page), 0u, 0);
                }
            }
            arm920t_set_reg(cpu, 0, 0u);
            return 1;
        }
        case 2u: /* GpFlipModeSet */
            arm920t_set_reg(cpu, 0, 0u);
            return 1;
        case 3u: /* GpLcdEnable */
            g->direct_fxe_lcd_enabled = 1u;
            if (g->direct_fxe_fb_addr) {
                if (g->direct_fxe_bpp == 16u) direct_set_lcd_16bpp(g, g->direct_fxe_fb_addr, 0);
                else direct_set_lcd_8bpp(g, g->direct_fxe_fb_addr, 0u, 0);
            }
            arm920t_set_reg(cpu, 0, 0u);
            return 1;
        case 4u: /* GpLcdDisable */
            g->direct_fxe_lcd_enabled = 0u;
            s3c2400_write32(g->soc, 0x14a00000u, s3c2400_debug_read32(g->soc, 0x14a00000u) & ~1u);
            arm920t_set_reg(cpu, 0, 0u);
            return 1;
        case 5u: /* GpLcdStatusGet */
            arm920t_set_reg(cpu, 0, g->direct_fxe_lcd_enabled ? (0x80u | 0x40u | 0x20u) : 0u);
            return 1;
        case 6u: /* GpLcdLock */
            arm920t_set_reg(cpu, 0, g->direct_fxe_fb_addr ? g->direct_fxe_fb_addr : direct_default_surface_addr(0));
            return 1;
        case 7u: { /* GpLcdInfoGet(GPLCDINFO *p_info) */
            if (direct_ram_range(g, a0, 36u)) {
                uint32_t bpp = (g->direct_fxe_bpp == 16u) ? 16u : 8u;
                uint32_t buf_count = (bpp == 16u) ? 2u : 4u;
                uint32_t lcd_global = (0u << 0) | (buf_count << 8) | (bpp << 16) | ((g->direct_fxe_lcd_enabled ? (0x80u | 0x40u | 0x20u) : 0u) << 24);
                direct_write32_if_ram(g, a0 + 0u, lcd_global);
                direct_write32_if_ram(g, a0 + 4u, 240u * 320u * (bpp == 16u ? 2u : 1u));
                for (uint32_t i = 0; i < 4u; ++i) direct_write32_if_ram(g, a0 + 8u + i * 4u, direct_default_surface_addr(i));
                direct_write32_if_ram(g, a0 + 24u, 0x14a00400u);
                direct_write32_if_ram(g, a0 + 28u, g->direct_fxe_palette_addr);
            }
            arm920t_set_reg(cpu, 0, 0u);
            return 1;
        }
        default:
            arm920t_set_reg(cpu, 0, 0u);
            return 1;
        }
    }
    case 0x0e: /* GPSDK sound-buffer allocator used by GpPcmInit. */
        return direct_handle_swi_set_sndbuffer(g, cpu);
    case 0x17: /* GpControlVolume is void; preserve the caller's registers. */
        if (g->direct_cpu_running) {
            g->direct_hle_pending_volume = 0x100u | (arm920t_get_reg(cpu, 0) & 63u);
            arm920t_stop_run(cpu);
        } else {
            s3c2400_audio_set_volume(g->soc, arm920t_get_reg(cpu, 0));
        }
        return 1;
    case 0x13: /* GPSDK/GPOS timer and scheduler command-block service. */
        return direct_handle_swi_gpos_timer(g, cpu, pc);
    case 0x11: { /* Direct-mode display callback for GpSurfaceSet/GpSurfaceFlip. */
        /* The real SDK service is a card-detect query; r0 may contain an
           unrelated pointer. Only our installed trampoline is a display call. */
        if (pc != direct_wait_swi_addr(g)) {
            uint32_t present = g->direct_fpk_asset_count != 0u ||
                (s3c2400_read32(g->soc, 0x15600030u) & 4u) == 0u;
            arm920t_set_reg(cpu, 0, present);
            return 1;
        }
        uint32_t a0 = arm920t_get_reg(cpu, 0);
        uint32_t fb = direct_read_surface_buffer(g, a0);
        if (fb) {
            uint32_t bpp = s3c2400_debug_read32(g->soc, a0 + 4u);
            if (bpp == 16u) direct_set_lcd_16bpp(g, fb, 0);
            else direct_set_lcd_8bpp(g, fb, 0u, 0);
            g->direct_fxe_lcd_explicit = 1u;
        }
        direct_request_vblank_wait(g, fb != 0u);
        arm920t_set_reg(cpu, 0, 1u);
        return 1;
    }
    case 0x14: { /* GPSDK thread sleep on scheduler builds; IIS/audio on PCM builds. */
        uint32_t cmdp = arm920t_get_reg(cpu, 0);
        uint32_t cmd = direct_read32_if_ram(g, cmdp);
        if (direct_ram_range(g, cmdp, 4u) &&
            (cmd == 0x20u || cmd == 0x80u || cmd == 0x1000u || cmd == 0x2000u || cmd == 0x4010u)) {
            direct_tick_sdk_task_sleepers(g, arm920t_get_reg(cpu, 2), arm920t_get_reg(cpu, 3), 1u);
            if (direct_resume_ready_sdk_task(g, pc, arm920t_get_reg(cpu, 2), arm920t_get_reg(cpu, 3))) return 1;
        }
        return direct_handle_swi_iis(g, cpu);
    }
    case 0x0b: { /* Firmware memory/control helper used by GP32 SDK CRTs. */
        uint32_t selector = arm920t_get_reg(cpu, 0);
        uint32_t a1 = arm920t_get_reg(cpu, 1);
        uint32_t a2 = arm920t_get_reg(cpu, 2);
        uint32_t value = 0u;
        direct_sync_fwinfo(g);
        switch (selector) {
        case 0u:
            direct_install_stubs(g);
            value = direct_stub_addr(g);
            break;
        case 2u:
            /* Palette address query used by several GPSDK palette wrappers.
               The first output is the hardware/register palette and the second
               is the firmware logical/work palette.  Older HLE returned zero
               here, which made GpPaletteSelect copy a selected palette to NULL
               and left later GpPaletteRealize calls displaying the default or
               corrupted palette. */
            if (!g->direct_fxe_palette_initialized) direct_fill_default_palette(g);
            direct_write_fw_arg(g, a1, 0x14a00400u);
            direct_write_fw_arg(g, a2, direct_palette_sw_addr(g));
            g->direct_fxe_palette_addr = direct_palette_sw_addr(g);
            value = 0u;
            break;
        case 3u:
            /* BIOS callback-vector table request.  Several GPSDK builds ask
               firmware to populate a table of service callbacks, copy it into
               BSS, then call through selected entries.  In direct FXE mode the
               services are represented by a safe return-only ARM stub. */
            if (direct_ram_range(g, a1, 16u * 4u)) {
                static const uint32_t cb_offsets[] = { 0x000u, 0x040u, 0x078u, 0x0b0u, 0x0dcu, 0x114u, 0x138u, 0x15cu, 0x180u, 0x1a4u, 0x1c8u, 0x1ecu, 0x210u };
                direct_install_stubs(g);
                direct_install_smc_callbacks(g);
                for (uint32_t i = 0; i < 16u; ++i) {
                    uint32_t v = direct_ret_stub_addr(g);
                    if (i < (uint32_t)(sizeof(cb_offsets) / sizeof(cb_offsets[0]))) v = direct_smc_cb_base_addr(g) + cb_offsets[i];
                    direct_write32_if_ram(g, a1 + i * 4u, v);
                }
            }
            value = a1;
            break;
        case 4u:
            value = direct_fwinfo_addr(g);
            break;
        case 5u:
            value = direct_fw_usable_ram_top(g);
            break;
        case 6u:
            direct_update_fw_tick(g);
            value = direct_fw_tick_addr(g);
            break;
        case 8u:
            value = 0u;
            break;
        default:
            value = 0u;
            break;
        }
        arm920t_set_reg(cpu, 0, value);
        return 1;
    }
    case 0x0f: { /* b2fxec/commercial CRT inherited app-argument query. */
        uint32_t selector = arm920t_get_reg(cpu, 4);
        uint32_t argp = arm920t_get_reg(cpu, 0);
        if ((selector == 1u || selector == 0u) && g->direct_smc_game_dir[0]) {
            uint32_t dst = direct_app_arg_addr(g);
            direct_write_cstr_if_ram(g, dst, g->direct_smc_game_dir);
            /* Selector 1 (ROM 0x1458-0x1490) also stores the string length, at
             * most 0xff, through r0 when that is not null.  The SDK startup stub
             * reads it back as the count of its byte-copy loop; left at a stale
             * stack word the copy runs off the end of RAM into the SoC registers. */
            if (selector == 1u && direct_ram_range(g, argp, 4u)) {
                size_t len = strlen(g->direct_smc_game_dir);
                s3c2400_write32(g->soc, argp, (uint32_t)(len > 0xffu ? 0xffu : len));
            }
            arm920t_set_reg(cpu, 0, dst);
            return 1;
        }
        if ((selector == 2u || selector == 3u) && g->direct_smc_executable_path[0]) {
            uint32_t dst = direct_app_arg_addr(g) + 0x100u;
            direct_write_cstr_if_ram(g, dst, g->direct_smc_executable_path);
            arm920t_set_reg(cpu, 0, dst);
            return 1;
        }
        if (argp >= GP32_RAM_BASE && argp + 4u < GP32_RAM_BASE + s3c2400_ram_size(g->soc)) {
            s3c2400_write32(g->soc, argp, 0u);
            arm920t_set_reg(cpu, 0, argp);
        } else {
            arm920t_set_reg(cpu, 0, 0u);
        }
        return 1;
    }
    case 0x10: { /* GP32 SDK key helper: selector 0 returns GPIO key ports; selector 1 maps active-low GPIO bits to SDK key bits. */
        uint32_t selector = arm920t_get_reg(cpu, 0);
        if (selector == 0u) {
            arm920t_set_reg(cpu, 0, 0x1560000cu);
            arm920t_set_reg(cpu, 1, 0x15600030u);
            return 1;
        }
        if (selector == 1u) {
            uint32_t in0 = direct_key_port_value(g, arm920t_get_reg(cpu, 1));
            uint32_t in1 = direct_key_port_value(g, arm920t_get_reg(cpu, 2));
            /*
             * This is the low-level firmware virtual-key mapper used by the
             * GPSDK _VirtualKeyMap routine, not the public GpKeyGet() ABI.
             * The SDK routine calls SWI #0x10 and then MVN's R0 before
             * returning to the game, so the firmware value must remain
             * active-low: 1 bits mean released, 0 bits mean pressed.
             * Returning active-high here made GpKeyGet() become ~keys, which
             * looked like every control was held; AKA NOID then drifted left
             * and ran its menus/gameplay controls erratically.
             */
            uint32_t keys = 0u;
            if ((in0 & 0x0100u) == 0u) keys |= 0x001u; /* GPC_VK_LEFT */
            if ((in0 & 0x0200u) == 0u) keys |= 0x002u; /* GPC_VK_DOWN */
            if ((in0 & 0x0400u) == 0u) keys |= 0x004u; /* GPC_VK_RIGHT */
            if ((in0 & 0x0800u) == 0u) keys |= 0x008u; /* GPC_VK_UP */
            if ((in0 & 0x1000u) == 0u) keys |= 0x010u; /* GPC_VK_FL */
            if ((in0 & 0x2000u) == 0u) keys |= 0x020u; /* GPC_VK_FB */
            if ((in0 & 0x4000u) == 0u) keys |= 0x040u; /* GPC_VK_FA */
            if ((in0 & 0x8000u) == 0u) keys |= 0x080u; /* GPC_VK_FR */
            if ((in1 & 0x0040u) == 0u) keys |= 0x100u; /* GPC_VK_START */
            if ((in1 & 0x0080u) == 0u) keys |= 0x200u; /* GPC_VK_SELECT */
            arm920t_set_reg(cpu, 0, ~keys);
            return 1;
        }
        arm920t_set_reg(cpu, 0, 0xffffffffu);
        return 1;
    }
    case 0x15: /* Application argument query. */
        arm920t_set_reg(cpu, 0, 0u);
        arm920t_set_reg(cpu, 1, 0u);
        return 1;
    case 0x09: { /* Firmware interrupt-handler install (retail ROM 0x1260): store
                  * the handler in the fixed ISR table and unmask the source. The
                  * ROM's SWI wrapper restores r0/r1, so the caller sees them
                  * unchanged. */
        uint32_t source = arm920t_get_reg(cpu, 0) & 31u;
        uint32_t handler = arm920t_get_reg(cpu, 1);
        s3c2400_write32(g->soc, S3C2400_HLE_ISR_TABLE_ADDR + source * 4u, handler);
        s3c2400_write32(g->soc, 0x14400008u,
                        s3c2400_debug_read32(g->soc, 0x14400008u) & ~(1u << source));
        return 1;
    }
    case 0x0a: { /* Firmware interrupt-handler removal (retail ROM 0x1294): clear
                  * the table entry and mask the source again. */
        uint32_t source = arm920t_get_reg(cpu, 0) & 31u;
        s3c2400_write32(g->soc, S3C2400_HLE_ISR_TABLE_ADDR + source * 4u, 0u);
        s3c2400_write32(g->soc, 0x14400008u,
                        s3c2400_debug_read32(g->soc, 0x14400008u) | (1u << source));
        return 1;
    }
    case 0x12: /* Firmware exit.  Keep direct-loaded homebrew in a benign idle loop. */
        arm920t_set_reg(cpu, 15, pc);
        return 1;
    case 0x04: /* Firmware system boot (retail ROM 0x20d8): the service clears RAM,
                 * reloads the firmware and finally jumps to the entry the launcher
                 * published, so it never returns to the caller. Returning instead
                 * sent the caller's stale LR (GpMadMP3's "press A to reboot" CRT
                 * epilogue) into data. Direct mode has no firmware to re-enter, so
                 * the request restarts the loaded image at the frame boundary; the
                 * guest stays parked on the service call until then. */
        arm920t_set_reg(cpu, 15, pc);
        g->direct_reboot_pending = 1u; /* restarted at the frame boundary */
        return 1;
    case 0x1ff: /* Firmware high-range selector 0xFF: device state reset and
                 * continuation through FIQ-mode r12, implemented above. */
        return direct_handle_swi_reinit(g, cpu);
    case 0x16: { /* GPSDK graphics mode/palette helper. */
        uint32_t selector = arm920t_get_reg(cpu, 0);
        uint32_t arg1 = arm920t_get_reg(cpu, 1);
        uint32_t arg2 = arm920t_get_reg(cpu, 2);
        uint32_t arg3 = arm920t_get_reg(cpu, 3);
        switch (selector) {
        case 1u: /* palette realize / vblank-safe update */
        case 2u: /* fast palette update */
            direct_realize_software_palette_from(g, arg2, 1);
            arm920t_set_reg(cpu, 0, 1u);
            return 1;
        case 3u: /* palette operation accepted; keep current addresses stable. */
        case 4u:
            arm920t_set_reg(cpu, 0, 1u);
            return 1;
        case 5u: { /* swi_pal_addr_get(unsigned int **reg, unsigned int **log) */
            uint32_t sw = direct_palette_sw_addr(g);
            uint32_t hw = direct_palette_hw_mirror_addr(g);
            if (!g->direct_fxe_palette_initialized) direct_fill_default_palette(g);
            if (direct_ram_range(g, arg1, 4u)) direct_write32_if_ram(g, arg1, 0x14a00400u);
            if (direct_ram_range(g, arg2, 4u)) direct_write32_if_ram(g, arg2, sw);
            direct_copy_lcd_palette_to_ram(g, hw);
            g->direct_fxe_palette_addr = sw;
            arm920t_set_reg(cpu, 0, 1u);
            return 1;
        }
        case 6u:
            direct_set_lcd_8bpp(g, g->direct_fxe_fb_addr ? g->direct_fxe_fb_addr : direct_default_surface_addr(0), arg1, 0);
            arm920t_set_reg(cpu, 0, 1u);
            return 1;
        default:
            GP32_UNUSED(arg3);
            arm920t_set_reg(cpu, 0, 1u);
            return 1;
        }
    }
    case 0x05: { /* Execute a decrunched GXB image when a packer passes one; otherwise firmware display service no-op.
                  * The retail exec service (ROM 0x2298) also redoes the launch path before it starts the image:
                  * int_init 0x15e0, IIS stop 0x7804, default clock 0x22d0-0x22f0, pin tables and 8 bpp mode
                  * switch 0x2170. Direct mode does not, so a stage that changed the clock hands it on. */
        uint32_t target = arm920t_get_reg(cpu, 0);
        uint32_t stack = arm920t_get_reg(cpu, 1);
        if (direct_ram_range(g, target, 8u)) {
            uint32_t w0 = s3c2400_debug_read32(g->soc, target);
            uint32_t w1 = s3c2400_debug_read32(g->soc, target + 4u);
            int w0_branch = (w0 & 0x0f000000u) == 0x0a000000u;
            int w1_branch = (w1 & 0x0f000000u) == 0x0a000000u;
            if (w0_branch || w1_branch) {
                if (!w0_branch) target += 4u;
                uint32_t placed = target;
                uint32_t placed_end = 0u;
                if (direct_gxb_place_image(g, target, &placed, &placed_end)) {
                    /* The declared windows (ROM, RW and ZI) were just rewritten
                       under the block cache, so drop every translated block. */
                    arm920t_flush_jit(cpu);
                    /* A staged image carries its own copy of the CRT and of any
                       prebuilt libraries; the load-time fingerprint scan ran
                       before that code existed, so scan the placed range now. */
                    if (placed_end > placed) direct_scan_file_hle_range(g, placed, placed_end);
                    target = placed;
                }
                /* An image only inherits the caller's stack when that value can
                   actually hold one: aligned, with room to grow down inside RAM.
                   A packer hands over a scratch address such as rom_start | 1,
                   and adopting it puts every later frame below the SDRAM window. */
                if ((stack & 3u) == 0u && stack >= GP32_RAM_BASE + 0x100u && direct_ram_range(g, stack, 4u))
                    arm920t_set_reg(cpu, 13, stack);
                arm920t_set_reg(cpu, 15, target);
                return 1;
            }
        }
        arm920t_set_reg(cpu, 0, 0u);
        return 1;
    }
    case 0x0d: { /* Firmware clock service (retail ROM 0x1104 -> 0x1398 -> clock service
                  * 0x200c): r0 points at {FCLK Hz, MPLLCON, CLKDIVN}. Direct mode takes
                  * FCLK from MPLLCON (titles pass the matching value), and the ROM
                  * epilogue (0x1250) restores r0 and r1, so the caller sees every
                  * register unchanged. */
        uint32_t params = arm920t_get_reg(cpu, 0);
        if (direct_ram_range(g, params, 12u))
            direct_apply_fw_clock(g, s3c2400_debug_read32(g->soc, params + 4u),
                                  s3c2400_debug_read32(g->soc, params + 8u));
        return 1;
    }
    default:
        if (imm == 0x02u || imm == 0x07u ||
            imm == 0x0eu ||
            (imm >= 0x100u && imm <= 0x120u)) {
            arm920t_set_reg(cpu, 0, 0u);
            return 1;
        }
        return 0;
    }
}

static int direct_swi_hook(void *user, arm920t_t *cpu, uint32_t imm, uint32_t pc, int thumb) {
    gp32_t *g = (gp32_t *)user;
    if (!thumb && g && g->adpcm_fix_patched && direct_adpcm_fix_swi(g, cpu, imm, pc)) return 1;
    return direct_fxe_swi(user, cpu, imm, pc, thumb);
}

/* The hook implements direct-FXE services and the ADPCM trap. Otherwise BIOS
 * mode uses the CPU's ordinary SWI exception path without a callback. Keep
 * this derived pointer synchronized with both flags. */
static void direct_update_swi_hook(gp32_t *g) {
    arm920t_set_swi_handler(g->cpu, (g->direct_fxe_mode || g->adpcm_fix_patched) ? direct_swi_hook : NULL, g);
}

static void direct_set_fxe_mode(gp32_t *g, uint32_t mode) {
    g->direct_fxe_mode = mode;
    direct_update_swi_hook(g);
    s3c2400_set_audio_idle(g->soc, !mode);
}

gp32_t *gp32_create(const gp32_options_t *opt) {
    gp32_t *g = (gp32_t *)calloc(1, sizeof(*g));
    if (!g) return NULL;
    if (opt) { g->log = opt->log; g->log_user = opt->log_user; }
    g->soc = s3c2400_create(opt ? opt->ram_size : 0);
    if (!g->soc) { gp32_destroy(g); return NULL; }
    s3c2400_set_log(g->soc, bridge_log, g);
    arm_bus_t bus = s3c2400_get_bus(g->soc);
    g->cpu = arm920t_create(&bus);
    if (!g->cpu) { gp32_destroy(g); return NULL; }
    size_t live_read_count = 0;
    const arm_live_read32_t *live_reads = s3c2400_live_read32(g->soc, &live_read_count);
    if (!arm920t_set_live_read32(g->cpu, live_reads, live_read_count)) { gp32_destroy(g); return NULL; }
    s3c2400_set_irq_sink(g->soc, g->cpu);
    if (opt && opt->enable_trace) arm920t_set_trace(g->cpu, 1, bridge_log, g);
#if defined(GP32EMU_WASM)
    int direct_smc = 0;
    (void)direct_smc;
    /* WASM hosts load BIOS/media from JavaScript-provided memory buffers.
     * Avoid pulling filesystem/ZIP path loaders into the freestanding build. */
    gp32_reset(g);
#else
    int direct_smc = opt && opt->smartmedia_path && !opt->bios_path;
    if (opt && opt->bios_path && gp32_load_bios(g, opt->bios_path) != GP32_OK) { gp32_destroy(g); return NULL; }
    if (opt && opt->smartmedia_path && gp32_load_smartmedia(g, opt->smartmedia_path) != GP32_OK) { gp32_destroy(g); return NULL; }
    gp32_reset(g);
    if (direct_smc && gp32_load_smartmedia_direct(g, opt->smartmedia_path) != GP32_OK) { gp32_destroy(g); return NULL; }
#endif
    return g;
}

void gp32_destroy(gp32_t *g) {
    if (!g) return;
    direct_clear_fpk_assets(g);
    direct_clear_reset_image(g);
    arm920t_destroy(g->cpu);
    s3c2400_destroy(g->soc);
    free(g);
}

gp32_status_t gp32_load_bios(gp32_t *g, const char *path) {
    if (!g || !path) return GP32_ERR_INVALID_ARGUMENT;
    char e[256] = {0};
    if (!s3c2400_load_bios(g->soc, path, e, sizeof(e))) { seterr(g, "%s", e[0] ? e : "BIOS load failed"); return GP32_ERR_IO; }
    return GP32_OK;
}

gp32_status_t gp32_load_smartmedia(gp32_t *g, const char *path) {
    if (!g || !path) return GP32_ERR_INVALID_ARGUMENT;
    char e[256] = {0};
    if (!s3c2400_load_smartmedia(g->soc, path, e, sizeof(e))) { seterr(g, "%s", e[0] ? e : "SmartMedia load failed"); return GP32_ERR_BAD_IMAGE; }
    return GP32_OK;
}

gp32_status_t gp32_load_bios_data(gp32_t *g, const void *data, size_t size) {
    if (!g || !data || !size) return GP32_ERR_INVALID_ARGUMENT;
    char e[256] = {0};
    if (!s3c2400_load_bios_buffer(g->soc, (const uint8_t *)data, size, e, sizeof(e))) { seterr(g, "%s", e[0] ? e : "BIOS buffer load failed"); return GP32_ERR_IO; }
    return GP32_OK;
}

gp32_status_t gp32_load_smartmedia_data(gp32_t *g, const void *data, size_t size) {
    if (!g || !data || !size) return GP32_ERR_INVALID_ARGUMENT;
    char e[256] = {0};
    if (!s3c2400_load_smartmedia_buffer(g->soc, (const uint8_t *)data, size, e, sizeof(e))) { seterr(g, "%s", e[0] ? e : "SmartMedia buffer load failed"); return GP32_ERR_BAD_IMAGE; }
    return GP32_OK;
}

gp32_status_t gp32_load_smartmedia_over_base(gp32_t *g, const char *path) {
    if (!g || !path) return GP32_ERR_INVALID_ARGUMENT;
    char e[256] = {0};
    if (!s3c2400_load_smartmedia_over_base(g->soc, path, e, sizeof(e))) { seterr(g, "%s", e[0] ? e : "SmartMedia saved-image load failed"); return GP32_ERR_BAD_IMAGE; }
    return GP32_OK;
}

gp32_status_t gp32_load_smartmedia_over_base_data(gp32_t *g, const void *data, size_t size) {
    if (!g || !data || !size) return GP32_ERR_INVALID_ARGUMENT;
    char e[256] = {0};
    if (!s3c2400_load_smartmedia_buffer_over_base(g->soc, (const uint8_t *)data, size, e, sizeof(e))) { seterr(g, "%s", e[0] ? e : "SmartMedia saved-image load failed"); return GP32_ERR_BAD_IMAGE; }
    return GP32_OK;
}

gp32_status_t gp32_set_smartmedia_state_base(gp32_t *g, const void *data, size_t size) {
    if (!g || !data || !size) return GP32_ERR_INVALID_ARGUMENT;
    char e[256] = {0};
    if (!s3c2400_set_smartmedia_state_base(g->soc, (const uint8_t *)data, size, e, sizeof(e))) { seterr(g, "%s", e[0] ? e : "SmartMedia state base rejected"); return GP32_ERR_BAD_IMAGE; }
    return GP32_OK;
}

gp32_status_t gp32_set_smartmedia_state_base_file(gp32_t *g, const char *path) {
    if (!g || !path) return GP32_ERR_INVALID_ARGUMENT;
    char e[256] = {0};
    if (!s3c2400_set_smartmedia_state_base_file(g->soc, path, e, sizeof(e))) { seterr(g, "%s", e[0] ? e : "SmartMedia state base rejected"); return GP32_ERR_BAD_IMAGE; }
    return GP32_OK;
}

static gp32_status_t gp32_load_fxe_image(gp32_t *g, fxe_image_t *img);

static void direct_smc_set_launch_paths(gp32_t *g, const char *exe_path) {
    if (!g) return;
    g->direct_smc_executable_path[0] = '\0';
    g->direct_smc_game_dir[0] = '\0';
    if (!exe_path || !exe_path[0]) return;

    char tmp[260];
    size_t j = 0u;
    const char *prefix = "gp:\\";
    const char *last_sep = strrchr(exe_path, '/');
    const char *last_sep2 = strrchr(exe_path, '\\');
    if (last_sep2 && (!last_sep || last_sep2 > last_sep)) last_sep = last_sep2;

    for (size_t i = 0u; prefix[i] && j + 1u < sizeof(tmp); ++i) tmp[j++] = prefix[i];
    for (const char *p = exe_path; *p && j + 1u < sizeof(tmp); ++p) {
        char c = *p;
        if (c == '/') c = '\\';
        /* The BIOS launcher's app-argument string is not fully normalised to
         * lower-case.  It uses a lower-case path prefix (gp:\game\...) but
         * preserves the 8.3 executable basename as stored on the SMC.  Hany's
         * SDK startup explicitly strcmp()s this value against
         * "gp:\game\STAR"; returning "gp:\game\star" makes it take the
         * failure-exit path and eventually return through a zero PC in HLE.
         * Keep directories case-insensitive/lowercase for older direct-FPK
         * users, but preserve the final filename stem/extension. */
        if (!last_sep || p <= last_sep) c = (char)tolower((unsigned char)c);
        tmp[j++] = c;
    }
    tmp[j] = '\0';
    snprintf(g->direct_smc_executable_path, sizeof(g->direct_smc_executable_path), "%s", tmp);
    snprintf(g->direct_smc_game_dir, sizeof(g->direct_smc_game_dir), "%s", tmp);
    char *dot = strrchr(g->direct_smc_game_dir, '.');
    char *slash = strrchr(g->direct_smc_game_dir, '\\');
    if (dot && (!slash || dot > slash)) *dot = '\0';
    else if (slash && slash[1]) slash[1] = '\0';
}

gp32_status_t gp32_load_smartmedia_direct(gp32_t *g, const char *path) {
    if (!g || !path) return GP32_ERR_INVALID_ARGUMENT;
    char e[256] = {0};
    smc_direct_package_t pkg;
    if (!smc_direct_load_file(path, &pkg, e, sizeof(e))) {
        seterr(g, "%s", e[0] ? e : "SmartMedia direct-load failed");
        return GP32_ERR_BAD_IMAGE;
    }
    direct_smc_set_launch_paths(g, pkg.executable_path);
    direct_clear_fpk_assets(g);
    direct_clear_reset_image(g);
    g->direct_fpk_assets = pkg.assets;
    g->direct_fpk_asset_count = pkg.asset_count;
    pkg.assets = NULL;
    pkg.asset_count = 0;
    gp32_status_t st = gp32_load_fxe_image_internal(g, &pkg.image, 1, 1, 1, 0);
    if (st == GP32_OK) {
        direct_init_smc_gpio(g);
        direct_scan_file_hle(g);
    }
    smc_direct_package_free(&pkg);
    return st;
}

gp32_status_t gp32_load_smartmedia_direct_data(gp32_t *g, const void *data, size_t size, const char *label) {
    if (!g || !data || !size) return GP32_ERR_INVALID_ARGUMENT;
    char e[256] = {0};
    smc_direct_package_t pkg;
    if (!smc_direct_load_buffer((const uint8_t *)data, size, label ? label : "smc", &pkg, e, sizeof(e))) {
        seterr(g, "%s", e[0] ? e : "SmartMedia direct buffer load failed");
        return GP32_ERR_BAD_IMAGE;
    }
    direct_smc_set_launch_paths(g, pkg.executable_path);
    direct_clear_fpk_assets(g);
    direct_clear_reset_image(g);
    g->direct_fpk_assets = pkg.assets;
    g->direct_fpk_asset_count = pkg.asset_count;
    pkg.assets = NULL;
    pkg.asset_count = 0;
    gp32_status_t st = gp32_load_fxe_image_internal(g, &pkg.image, 1, 1, 1, 0);
    if (st == GP32_OK) {
        direct_init_smc_gpio(g);
        direct_scan_file_hle(g);
    }
    smc_direct_package_free(&pkg);
    return st;
}

static gp32_status_t gp32_load_fxe_image_internal(gp32_t *g, fxe_image_t *img, int update_reset_image, int scan_file_hle, int init_smc_gpio, int preserve_hle_options) {
    char e[256] = {0};
    memset(&g->elapsed, 0, sizeof(g->elapsed));
    memset(&g->frame_time, 0, sizeof(g->frame_time));
    direct_reset_hle_runtime(g, preserve_hle_options);
    s3c2400_reset(g->soc);
    s3c2400_install_hle_bios(g->soc);
    if (!s3c2400_load_ram_image(g->soc, img->load_addr, img->payload, img->payload_size, e, sizeof(e))) {
        seterr(g, "%s", e[0] ? e : "FXE RAM load failed");
        return GP32_ERR_BAD_IMAGE;
    }
    direct_set_fxe_mode(g, 1u);
    direct_init_soc_handoff(g);
    g->direct_fxe_entry = img->entry_addr;
    g->direct_fxe_stack = GP32_RAM_BASE + (uint32_t)s3c2400_ram_size(g->soc);
    g->direct_fxe_fb_addr = 0u;
    g->direct_fxe_image_end = img->load_addr + (uint32_t)img->payload_size;
    g->direct_fxe_bpp = 0u;
    g->direct_fxe_palette_addr = 0u;
    g->direct_fxe_palette_initialized = 0u;
    g->direct_fxe_lcd_explicit = 0u;
    g->direct_fxe_lcd_enabled = 1u;
    g->direct_vblank_wait_requested = 0;
    direct_apply_gxb_scatterload(g, img);
    for (unsigned i = 0; i < 4u; ++i) {
        g->direct_fxe_lcd_surface[i] = direct_default_surface_addr(i);
        g->direct_fxe_surface_checksum[i] = 0u;
    }
    direct_install_stubs(g);
    direct_sync_fwinfo(g);
    direct_fill_default_palette(g);
    direct_init_lcd_handoff(g);
    snprintf(g->direct_fxe_title, sizeof(g->direct_fxe_title), "%s", img->title[0] ? img->title : "FXE");
    arm920t_reset(g->cpu, img->entry_addr);
    arm920t_set_cpsr(g->cpu, ARM_MODE_SVC | ARM_I_FLAG | ARM_F_FLAG);
    arm920t_set_reg(g->cpu, 13, g->direct_fxe_stack - 16u);
    arm920t_set_reg(g->cpu, 0, img->load_addr);
    arm920t_set_reg(g->cpu, 1, g->direct_fxe_stack - 16u);
    /*
     * The retail firmware installs every exception-mode stack from its ROM
     * table (ROM 0x1058 through the installer at 0x2390) before it launches an
     * image: undefined 0x0C7AC800, abort 0x0C7ACC00, FIQ 0x0C7AD400, IRQ
     * 0x0C7AE800, supervisor 0x0C7AFF00.  GPSDK code takes interrupts during
     * its own startup (the EEPROM/IIC driver raises INT_IIC before the
     * application would install a stack), so on hardware those banks are never
     * empty.  Direct mode has no firmware, and a zero IRQ bank makes the IRQ
     * dispatcher push the handler address to unmapped space and pop garbage
     * back into PC.  Publish the firmware's banks with the firmware's values.
     * The supervisor stack keeps the host's top-of-RAM choice because the
     * image entry already runs on it.
     */
    {
        arm920t_register_context_t ctx;
        arm920t_get_register_context(g->cpu, &ctx);
        ctx.bank_und[0] = GP32_RAM_BASE + 0x007ac800u;
        ctx.bank_abt[0] = GP32_RAM_BASE + 0x007acc00u;
        ctx.bank_fiq[0] = GP32_RAM_BASE + 0x007ad400u;
        ctx.bank_irq[0] = GP32_RAM_BASE + 0x007ae800u;
        arm920t_set_register_context(g->cpu, &ctx);
    }
    if (update_reset_image && !direct_store_reset_image(g, img, scan_file_hle, init_smc_gpio)) {
        seterr(g, "out of memory storing direct-HLE reset image");
        return GP32_ERR_IO;
    }
    return GP32_OK;
}

static gp32_status_t gp32_load_fxe_image(gp32_t *g, fxe_image_t *img) {
    return gp32_load_fxe_image_internal(g, img, 1, 0, 0, 0);
}

gp32_status_t gp32_load_fxe(gp32_t *g, const char *path) {
    if (!g || !path) return GP32_ERR_INVALID_ARGUMENT;
    fxe_image_t img;
    char e[256] = {0};
    if (!fxe_load_file(path, &img, e, sizeof(e))) { seterr(g, "%s", e[0] ? e : "FXE load failed"); return GP32_ERR_BAD_IMAGE; }
    direct_clear_fpk_assets(g);
    direct_clear_reset_image(g);
    gp32_status_t st = gp32_load_fxe_image(g, &img);
    fxe_image_free(&img);
    return st;
}

gp32_status_t gp32_load_fxe_data(gp32_t *g, const void *data, size_t size, const char *label) {
    if (!g || !data || !size) return GP32_ERR_INVALID_ARGUMENT;
    fxe_image_t img;
    char e[256] = {0};
    if (!fxe_load_buffer((const uint8_t *)data, size, label ? label : "buffer", &img, e, sizeof(e))) { seterr(g, "%s", e[0] ? e : "FXE buffer load failed"); return GP32_ERR_BAD_IMAGE; }
    direct_clear_fpk_assets(g);
    direct_clear_reset_image(g);
    gp32_status_t st = gp32_load_fxe_image(g, &img);
    fxe_image_free(&img);
    return st;
}

gp32_status_t gp32_load_fpk(gp32_t *g, const char *path) {
    if (!g || !path) return GP32_ERR_INVALID_ARGUMENT;
    char e[256] = {0};
    fpk_package_t pkg;
    if (!fpk_load_package_file(path, &pkg, e, sizeof(e))) {
        seterr(g, "%s", e[0] ? e : "FPK load failed");
        return GP32_ERR_BAD_IMAGE;
    }
    fxe_image_t img;
    if (!fxe_load_buffer(pkg.fxe_data, pkg.fxe_size, pkg.title[0] ? pkg.title : path, &img, e, sizeof(e))) {
        fpk_package_free(&pkg);
        seterr(g, "%s", e[0] ? e : "FPK embedded FXE load failed");
        return GP32_ERR_BAD_IMAGE;
    }
    if (!img.title[0] && pkg.title[0]) {
        size_t n = strlen(pkg.title);
        if (n >= sizeof(img.title)) n = sizeof(img.title) - 1u;
        memcpy(img.title, pkg.title, n);
        img.title[n] = '\0';
    }
    direct_clear_fpk_assets(g);
    direct_clear_reset_image(g);
    g->direct_fpk_assets = pkg.assets;
    g->direct_fpk_asset_count = pkg.asset_count;
    pkg.assets = NULL;
    pkg.asset_count = 0;
    gp32_status_t st = gp32_load_fxe_image_internal(g, &img, 1, 1, 0, 0);
    if (st == GP32_OK) direct_scan_file_hle(g);
    fxe_image_free(&img);
    fpk_package_free(&pkg);
    return st;
}

gp32_status_t gp32_load_fpk_data(gp32_t *g, const void *data, size_t size, const char *label) {
    if (!g || !data || !size) return GP32_ERR_INVALID_ARGUMENT;
    char e[256] = {0};
    fpk_package_t pkg;
    if (!fpk_load_package_buffer((const uint8_t *)data, size, label ? label : "FPK", &pkg, e, sizeof(e))) {
        seterr(g, "%s", e[0] ? e : "FPK buffer load failed");
        return GP32_ERR_BAD_IMAGE;
    }
    fxe_image_t img;
    if (!fxe_load_buffer(pkg.fxe_data, pkg.fxe_size, pkg.title[0] ? pkg.title : (label ? label : "FPK"), &img, e, sizeof(e))) {
        fpk_package_free(&pkg);
        seterr(g, "%s", e[0] ? e : "FPK embedded FXE load failed");
        return GP32_ERR_BAD_IMAGE;
    }
    if (!img.title[0] && pkg.title[0]) {
        size_t n = strlen(pkg.title);
        if (n >= sizeof(img.title)) n = sizeof(img.title) - 1u;
        memcpy(img.title, pkg.title, n);
        img.title[n] = '\0';
    }
    direct_clear_fpk_assets(g);
    g->direct_fpk_assets = pkg.assets;
    g->direct_fpk_asset_count = pkg.asset_count;
    pkg.assets = NULL;
    pkg.asset_count = 0;
    gp32_status_t st = gp32_load_fxe_image_internal(g, &img, 1, 1, 0, 0);
    if (st == GP32_OK) direct_scan_file_hle(g);
    fxe_image_free(&img);
    fpk_package_free(&pkg);
    return st;
}

gp32_status_t gp32_save_smartmedia(gp32_t *g, const char *path) {
    if (!g || !path) return GP32_ERR_INVALID_ARGUMENT;
    char e[256] = {0};
    if (!s3c2400_save_smartmedia(g->soc, path, e, sizeof(e))) { seterr(g, "%s", e[0] ? e : "SmartMedia save failed"); return GP32_ERR_IO; }
    return GP32_OK;
}

gp32_status_t gp32_save_card_progress(gp32_t *g, const char *path) {
    if (!g || !path) return GP32_ERR_INVALID_ARGUMENT;
    char e[256] = {0};
    if (!s3c2400_save_card_progress(g->soc, path, e, sizeof(e))) { seterr(g, "%s", e[0] ? e : "Card progress save failed"); return GP32_ERR_IO; }
    return GP32_OK;
}

gp32_status_t gp32_poll_card_progress(gp32_t *g, const char *path) {
    if (!g || !path) return GP32_ERR_INVALID_ARGUMENT;
    char e[256] = {0};
    if (!s3c2400_poll_card_progress(g->soc, path, e, sizeof(e))) {
        seterr(g, "%s", e[0] ? e : "Automatic card save failed");
        return GP32_ERR_IO;
    }
    return GP32_OK;
}

gp32_status_t gp32_load_card_progress(gp32_t *g, const char *path) {
    if (!g || !path) return GP32_ERR_INVALID_ARGUMENT;
    char e[256] = {0};
    if (!s3c2400_load_card_progress(g->soc, path, e, sizeof(e))) { seterr(g, "%s", e[0] ? e : "Card progress load failed"); return GP32_ERR_BAD_IMAGE; }
    return GP32_OK;
}

gp32_status_t gp32_boot_mounted_smartmedia(gp32_t *g, const char *label) {
    if (!g) return GP32_ERR_INVALID_ARGUMENT;
    size_t size = 0;
    uint8_t *image = s3c2400_copy_smartmedia(g->soc, &size);
    if (!image) { seterr(g, "Cannot copy mounted card for direct boot"); return GP32_ERR_IO; }
    gp32_status_t st = gp32_load_smartmedia_direct_data(g, image, size, label);
    free(image);
    return st;
}

/* The retail firmware answers a reboot request by restarting the machine: it
 * clears RAM, reloads the firmware and jumps to the entry the launcher
 * published. Direct mode has no firmware to re-enter, so the equivalent is to
 * restart the image that is already loaded - the same restart the host Reset
 * action performs for a direct-loaded guest. */
static gp32_status_t direct_restart_loaded_image(gp32_t *g) {
    gp32_status_t st = gp32_load_fxe_image_internal(g, &g->direct_reset_image, 0, g->direct_reset_scan_file_hle, g->direct_reset_init_smc_gpio, 1);
    if (st == GP32_OK) {
        if (g->direct_reset_init_smc_gpio) direct_init_smc_gpio(g);
        if (g->direct_reset_scan_file_hle) direct_scan_file_hle(g);
    }
    return st;
}

/* SWI 4 asks the firmware to boot the system, and the ROM's own answer to the
 * reset, undefined-instruction and abort vectors is the reboot path at ROM
 * 0x168 (the parks published in the HLE image are their terminal equivalents).
 * Both end at a frame boundary inside this function: a restart is only
 * meaningful with the CPU stopped, and doing it here keeps the guest from
 * resuming a half-torn-down context. */
static gp32_status_t direct_service_reboot_request(gp32_t *g, gp32_status_t st) {
    if (!g || st != GP32_OK) return st;
    if (!g->direct_fxe_mode || !g->direct_reset_image_valid) return st;
    uint32_t pc = arm920t_get_pc(g->cpu);
    int on_fault_vector = (pc == 0x00000000u || pc == 0x00000004u || pc == 0x0000000cu || pc == 0x00000010u);
    if (!g->direct_reboot_pending && !on_fault_vector) return st;
    g->direct_reboot_pending = 0u;
    gp32_status_t rst = direct_restart_loaded_image(g);
    return rst == GP32_OK ? st : rst;
}

gp32_status_t gp32_reset(gp32_t *g) {
    if (!g) return GP32_ERR_INVALID_ARGUMENT;
    memset(&g->elapsed, 0, sizeof(g->elapsed));
    memset(&g->frame_time, 0, sizeof(g->frame_time));
    if (g->direct_reset_image_valid && g->direct_reset_image.payload && g->direct_reset_image.payload_size) {
        return direct_restart_loaded_image(g);
    }
    direct_set_fxe_mode(g, 0u);
    g->direct_fxe_fb_addr = 0;
    g->direct_fxe_image_end = 0;
    g->direct_fxe_bpp = 0;
    g->direct_fxe_palette_addr = 0;
    g->direct_fxe_palette_initialized = 0;
    g->direct_fxe_lcd_explicit = 0;
    g->direct_fxe_lcd_enabled = 0;
    g->direct_vblank_wait_requested = 0;
    for (unsigned i = 0; i < 4u; ++i) {
        g->direct_fxe_lcd_surface[i] = 0;
        g->direct_fxe_surface_checksum[i] = 0;
    }
    direct_reset_hle_runtime(g, 1);
    s3c2400_reset(g->soc);
    arm920t_reset(g->cpu, 0x00000000u);
    return GP32_OK;
}


static uint32_t direct_task_saved_pc(gp32_t *g, uint32_t task_addr) {
    if (!g || !direct_ram_range(g, task_addr, 0x34u)) return 0u;
    uint32_t saved_sp = s3c2400_debug_read32(g->soc, task_addr + 0u);
    if (!direct_ram_range(g, saved_sp, 64u)) return 0u;
    return s3c2400_debug_read32(g->soc, saved_sp + 60u);
}

static int direct_task_record_plausible(gp32_t *g, uint32_t task_addr) {
    if (!g || !direct_ram_range(g, task_addr, 0x34u)) return 0;
    uint32_t state = s3c2400_debug_read32(g->soc, task_addr + 0x14u);
    if (state != 1u && state != 2u && state != 4u && state != 8u) return 0;
    uint32_t entry = s3c2400_debug_read32(g->soc, task_addr + 0x30u);
    return direct_ram_range(g, entry & ~1u, 4u);
}

/*
 * A saved GPSDK task context carries a CPSR the SDK itself built: only the
 * condition flags, I/F/T and the mode field can be set.  RAM that merely looks
 * like a task record can supply any 32-bit word, and restoring one like
 * 0x0C215110 (IT/GE/reserved bits set) jumps the guest straight into data.
 * Accept only mode words that a real ARM context could hold.
 */
static int direct_saved_cpsr_plausible(uint32_t cpsr) {
    static const uint32_t legal = 0xf0000000u /* NZCV */ | 0x000000e0u /* I, F, T */ | 0x0000001fu /* mode */;
    if (cpsr & ~legal) return 0;
    uint32_t m = cpsr & 0x1fu;
    return m == ARM_MODE_SVC || m == 0x10u;
}

static int direct_task_state_can_run(gp32_t *g, uint32_t task_addr) {
    if (!direct_task_record_plausible(g, task_addr)) return 0;
    if (direct_gpos_task_is_internal_timer(g, task_addr)) return 0;
    uint32_t state = s3c2400_debug_read32(g->soc, task_addr + 0x14u);
    if (state == 1u || state == 2u) return 1;
    if (state == 4u) {
        uint32_t elapsed = s3c2400_debug_read32(g->soc, task_addr + 0x20u);
        uint32_t limit = s3c2400_debug_read32(g->soc, task_addr + 0x24u);
        return limit == 0u || elapsed >= limit;
    }
    return 0;
}

static void direct_set_task_scheduler_current(gp32_t *g, uint32_t task_addr, uint32_t idle_pc) {
    if (!g || !direct_task_record_plausible(g, task_addr)) return;
    uint32_t ram_end = GP32_RAM_BASE + (uint32_t)s3c2400_ram_size(g->soc);
    for (uint32_t a = GP32_RAM_BASE; a + 8u < ram_end; a += 4u) {
        uint32_t cur = s3c2400_debug_read32(g->soc, a + 0u);
        uint32_t sel = s3c2400_debug_read32(g->soc, a + 4u);
        if (cur != sel || cur == task_addr) continue;
        if (!direct_task_record_plausible(g, cur)) continue;
        uint32_t saved_pc = direct_task_saved_pc(g, cur);
        if (saved_pc && (saved_pc == idle_pc || saved_pc + 4u == idle_pc || saved_pc == idle_pc - 4u || saved_pc + 8u == idle_pc)) {
            direct_write32_if_ram(g, a + 0u, task_addr);
            direct_write32_if_ram(g, a + 4u, task_addr);
            return;
        }
    }
}

static void direct_task_scan_bounds(gp32_t *g, uint32_t first_task, uint32_t last_task, uint32_t *start, uint32_t *end, uint32_t *step) {
    uint32_t ram_end = GP32_RAM_BASE + (uint32_t)s3c2400_ram_size(g->soc);
    *start = GP32_RAM_BASE;
    *end = ram_end;
    *step = 4u;
    if (!g) return;
    if (direct_ram_range(g, first_task, 0x34u) && direct_ram_range(g, last_task, 0x34u) &&
        last_task >= first_task && last_task - first_task <= 32u * 0x34u) {
        *start = first_task;
        *end = last_task + 0x34u;
        *step = 0x34u;
        return;
    }
    uint32_t image_end = g->direct_fxe_image_end;
    if (image_end > GP32_RAM_BASE && image_end < ram_end) *end = image_end;
}

static void direct_tick_sdk_task_sleepers(gp32_t *g, uint32_t first_task, uint32_t last_task, uint32_t ticks) {
    if (!g || !g->direct_fxe_mode || !ticks) return;
    uint32_t start = GP32_RAM_BASE, end = GP32_RAM_BASE, step = 4u;
    direct_task_scan_bounds(g, first_task, last_task, &start, &end, &step);
    for (uint32_t t = start; t + 0x34u <= end; t += step) {
        if (!direct_task_record_plausible(g, t)) continue;
        if (direct_gpos_task_is_internal_timer(g, t)) continue;
        if (s3c2400_debug_read32(g->soc, t + 0x14u) != 4u) continue;
        uint32_t saved_sp = s3c2400_debug_read32(g->soc, t + 0u);
        if (!direct_ram_range(g, saved_sp, 64u)) continue;
        uint32_t saved_pc = s3c2400_debug_read32(g->soc, saved_sp + 60u);
        if (!direct_ram_range(g, saved_pc & ~1u, 4u)) continue;
        uint32_t elapsed = s3c2400_debug_read32(g->soc, t + 0x20u);
        uint32_t limit = s3c2400_debug_read32(g->soc, t + 0x24u);
        /* No guest task runs between these ticks. Advance once per task,
         * retaining every expiry without repeated table scans or overflow. */
        uint64_t total = (uint64_t)elapsed + ticks;
        if (total >= limit) {
            if (direct_trace_enabled()) fprintf(stderr, "[direct-hle] task wake t=%08x saved_pc=%08x limit=%u\n", t, saved_pc, limit);
            direct_write32_if_ram(g, t + 0x20u, 0u);
            direct_write32_if_ram(g, t + 0x14u, 2u);
        } else {
            direct_write32_if_ram(g, t + 0x20u, (uint32_t)total);
        }
    }
}

static int direct_resume_ready_sdk_task(gp32_t *g, uint32_t pc, uint32_t first_task, uint32_t last_task) {
    if (!g || !g->direct_fxe_mode || !g->cpu) return 0;
    uint32_t start = GP32_RAM_BASE, end = GP32_RAM_BASE, step = 4u;
    direct_task_scan_bounds(g, first_task, last_task, &start, &end, &step);
    if (g->direct_hle_callback_running && !g->direct_callback.sdk_task) {
        /* Bind the callback to the SDK record that actually saved its stack.
         * A scheduler may leave SVC SP at the frame base or at its top before
         * returning to IRQ. Mode/PC alone cannot identify an interrupted task.
         * Without a saved source frame, this service must not discard the
         * callback by restoring an unrelated ready task. */
        arm920t_register_context_t context;
        arm920t_get_register_context(g->cpu, &context);
        uint32_t sp = (context.cpsr & 31u) == ARM_MODE_SVC ? context.r[13] : context.bank_svc[0];
        for (uint32_t t = start; t + 0x34u <= end; t += step) {
            if (!direct_task_record_plausible(g, t) || direct_gpos_task_is_internal_timer(g, t)) continue;
            uint32_t saved_sp = s3c2400_debug_read32(g->soc, t);
            if ((saved_sp & 3u) || !direct_ram_range(g, saved_sp, 64u) ||
                (saved_sp != sp && saved_sp + 64u != sp)) continue;
            uint32_t cpsr = s3c2400_debug_read32(g->soc, saved_sp);
            if (!direct_saved_cpsr_plausible(cpsr) || (cpsr & 31u) != ARM_MODE_SVC ||
                !direct_ram_range(g, direct_task_saved_pc(g, t) & ~1u, 4u)) continue;
            g->direct_callback.sdk_task = t;
            break;
        }
        if (!g->direct_callback.sdk_task) return 0;
    }
    for (uint32_t t = start; t + 0x34u <= end; t += step) {
        if (!direct_task_record_plausible(g, t)) continue;
        if (direct_gpos_task_is_internal_timer(g, t)) continue;
        uint32_t state = s3c2400_debug_read32(g->soc, t + 0x14u);
        if (state != 2u) continue;
        uint32_t saved_sp = s3c2400_debug_read32(g->soc, t + 0u);
        if (!direct_ram_range(g, saved_sp, 64u)) continue;
        uint32_t saved_cpsr = s3c2400_debug_read32(g->soc, saved_sp + 0u);
        uint32_t saved_pc = s3c2400_debug_read32(g->soc, saved_sp + 60u);
        if (!direct_saved_cpsr_plausible(saved_cpsr)) continue;
        if (!direct_ram_range(g, saved_pc & ~1u, 4u)) continue;
        uint32_t saved_insn = s3c2400_debug_read32(g->soc, saved_pc & ~1u);
        if (saved_insn == 0xeafffffeu || saved_insn == 0xeaffffffu) continue;
        if (direct_trace_enabled()) fprintf(stderr, "[direct-hle] resume ready task t=%08x saved_pc=%08x from=%08x\n", t, saved_pc, pc);
        direct_set_task_scheduler_current(g, t, pc);
        direct_write32_if_ram(g, t + 0x14u, 1u);
        if (direct_restore_saved_context(g, t)) return 1;
    }
    return 0;
}

static int direct_restore_saved_context(gp32_t *g, uint32_t task_addr) {
    if (!g || !direct_ram_range(g, task_addr, 0x34u)) return 0;
    uint32_t saved_sp = s3c2400_debug_read32(g->soc, task_addr + 0u);
    if (!direct_ram_range(g, saved_sp, 16u * 4u)) return 0;
    uint32_t saved_cpsr = s3c2400_debug_read32(g->soc, saved_sp + 0u);
    uint32_t new_pc = s3c2400_debug_read32(g->soc, saved_sp + 60u);
    if (!direct_ram_range(g, new_pc & ~1u, 4u)) return 0;
    arm920t_set_cpsr(g->cpu, saved_cpsr);
    for (unsigned i = 0; i < 13u; ++i) {
        arm920t_set_reg(g->cpu, i, s3c2400_debug_read32(g->soc, saved_sp + 4u + i * 4u));
    }
    arm920t_set_reg(g->cpu, 14, s3c2400_debug_read32(g->soc, saved_sp + 56u));
    arm920t_set_reg(g->cpu, 13, saved_sp + 64u);
    arm920t_set_reg(g->cpu, 15, new_pc);
    direct_write32_if_ram(g, task_addr + 0u, saved_sp + 64u);
    direct_write32_if_ram(g, task_addr + 0x14u, 1u);
    if (g->direct_hle_callback_running) {
        g->direct_callback.suspended = task_addr != g->direct_callback.sdk_task;
        /* Settle the outgoing task's time before the incoming task executes,
         * including a switch back to the callback within a large run budget. */
        arm920t_stop_run(g->cpu);
    }
    return 1;
}

/*
 * Exact coarse prefilter for the fallback task-table sweep: true only when all
 * eight consecutive little-endian words at p are outside [1, 8].  The state
 * filter below reads exactly those words and accepts only 1, 2, 4 and 8, so a
 * window reported empty cannot contain a record this pass would resume or
 * tick.  Every surviving position still runs the same full validation; the
 * host implementations below must agree word for word.
 */
static int direct_task_state_window_empty(const uint8_t *p) {
#if GP32_HOST_SSE2
    const __m128i lo = _mm_loadu_si128((const __m128i *)(const void *)p);
    const __m128i hi = _mm_loadu_si128((const __m128i *)(const void *)(p + 16u));
    const __m128i one = _mm_set1_epi32(1);
    /* Unsigned (word - 1) > 7 rejects 0 and everything above 8 as well. */
    const __m128i limit = _mm_set1_epi32((int)(0x80000000u | 7u));
    const __m128i bias = _mm_set1_epi32((int)0x80000000u);
    const __m128i lo_high = _mm_cmpgt_epi32(_mm_xor_si128(_mm_sub_epi32(lo, one), bias), limit);
    const __m128i hi_high = _mm_cmpgt_epi32(_mm_xor_si128(_mm_sub_epi32(hi, one), bias), limit);
    return _mm_movemask_ps(_mm_castsi128_ps(_mm_and_si128(lo_high, hi_high))) == 0x0f;
#elif GP32_HOST_NEON
    const uint32x4_t zero = vdupq_n_u32(0u);
    const uint32x4_t eight = vdupq_n_u32(8u);
    const uint32x4_t lo = vld1q_u32((const uint32_t *)(const void *)p);
    const uint32x4_t hi = vld1q_u32((const uint32_t *)(const void *)(p + 16u));
    const uint32x4_t in = vorrq_u32(vandq_u32(vcleq_u32(lo, eight), vmvnq_u32(vceqq_u32(lo, zero))),
                                    vandq_u32(vcleq_u32(hi, eight), vmvnq_u32(vceqq_u32(hi, zero))));
    const uint32x2_t pairs = vpmax_u32(vget_low_u32(in), vget_high_u32(in));
    const uint32x2_t any = vpmax_u32(pairs, pairs);
    return vget_lane_u32(any, 0u) == 0u;
#else
    for (unsigned i = 0; i < 8u; ++i) {
        uint32_t word = gp32_ld32le(p + i * 4u);
        if (word >= 1u && word <= 8u) return 0;
    }
    return 1;
#endif
}

static int direct_try_resume_sdk_task(gp32_t *g) {
    if (!g || !g->direct_fxe_mode || !g->cpu) return 0;
    uint32_t pc = arm920t_get_pc(g->cpu);
    if (pc >= direct_stub_addr(g) && pc < direct_stub_addr(g) + 0xacu) return 0;
    if (!direct_ram_range(g, pc, 4u) || s3c2400_debug_read32(g->soc, pc) != 0xeafffffeu) return 0;

    /* Several devkitPro/official-GPSDK CRTs fall back into a resident SDK idle
       task after their firmware scheduler setup SWI is stubbed.  The idle loop
       is followed by the SDK's context-builder helper and preceded by a small
       literal pool containing the task globals.  Use that literal pool to
       perform the same saved-context restore the SDK scheduler would have done,
       rather than hard-coding an AKA NOID address. */
    uint32_t current_slot = s3c2400_debug_read32(g->soc, pc - 0x70u);
    uint32_t ready_slot = s3c2400_debug_read32(g->soc, pc - 0x6cu);
    uint32_t task_base = s3c2400_debug_read32(g->soc, pc - 0x60u);
    if (direct_ram_range(g, current_slot, 4u) && direct_ram_range(g, ready_slot, 4u) &&
        direct_ram_range(g, task_base, 8u * 0x34u)) {
        uint32_t current = s3c2400_debug_read32(g->soc, current_slot);
        for (unsigned i = 0; i < 8u; ++i) {
            uint32_t t = task_base + i * 0x34u;
            if (t == current || direct_gpos_task_is_internal_timer(g, t)) continue;
            uint32_t saved_sp = s3c2400_debug_read32(g->soc, t + 0u);
            if (!direct_task_state_can_run(g, t) || !direct_ram_range(g, saved_sp, 64u)) continue;
            uint32_t new_pc = s3c2400_debug_read32(g->soc, saved_sp + 60u);
            if (new_pc == pc || !direct_ram_range(g, new_pc & ~1u, 4u)) continue;
            direct_write32_if_ram(g, current_slot, t);
            direct_write32_if_ram(g, ready_slot, t);
            direct_write32_if_ram(g, t + 0x14u, 1u);
            if (direct_restore_saved_context(g, t)) return 1;
        }
    }
    /*
     * Fallback for SDK task loops whose literal-pool layout differs from the
     * pattern above.  Some GPSDK programs park in a permanent branch while
     * another task context is already marked runnable in the firmware task
     * table.  Scan for the same task-record shape used by the normal restore
     * path: saved SP at +0, runnable state at +0x14, and a saved ARM context
     * whose PC points back into RAM.
     */
    uint32_t ram_end = GP32_RAM_BASE + (uint32_t)s3c2400_ram_size(g->soc);
    const uint8_t *ram = s3c2400_ram_data(g->soc);
    if (!ram) return 0;
    for (uint32_t t = GP32_RAM_BASE; t + 0x34u < ram_end; t += 4u) {
        const uint8_t *rec = ram + (t - GP32_RAM_BASE);
        /*
         * The 32-byte probe stays inside the RAM buffer: the loop bound leaves
         * at least 0x34 bytes of record headroom past t, and the probe covers
         * the state words at +0x14 .. +0x30.  A window with no state word can
         * only contain records that this pass rejects below, so advancing to
         * its end removes reads without changing which records are examined,
         * in which order, or with which side effects.
         */
        if (direct_task_state_window_empty(rec + 0x14u)) {
            t += 28u; /* with the loop step this advances one full window */
            continue;
        }
        uint32_t state = gp32_ld32le(rec + 0x14u);
        /* Every other state word fails direct_task_record_plausible() inside
         * direct_task_state_can_run() below, so the candidate can be neither
         * resumed nor ticked. Rejecting it here leaves the pass, its side
         * effects and its cadence identical while skipping the expensive
         * checks for the bulk of the 8 MiB sweep. */
        if (state != 1u && state != 2u && state != 4u && state != 8u) continue;
        if (direct_gpos_task_is_internal_timer(g, t)) continue;
        if (state == 4u && direct_task_record_plausible(g, t)) {
            /*
             * Firmware-resident GPSDK schedulers sleep tasks by marking their
             * task control block state as 4 and counting scheduler ticks at
             * +0x20 until the delay at +0x24 expires.  In direct-HLE mode
             * these timer IRQ callbacks do not run once the resident idle task
             * has fallen into its permanent branch, so titles such as
             * W.B.W. can park forever with a runnable game task still asleep.
             * When the CPU is demonstrably executing that idle self-branch,
             * advance one scheduler tick while scanning the task table and let
             * the normal context-restore path below resume the awakened task.
             */
            uint32_t elapsed = s3c2400_debug_read32(g->soc, t + 0x20u);
            uint32_t limit = s3c2400_debug_read32(g->soc, t + 0x24u);
            if (limit == 0u || elapsed + 1u >= limit) {
                if (direct_trace_enabled()) fprintf(stderr, "[direct-hle] idle task wake t=%08x limit=%u\n", t, limit);
                direct_write32_if_ram(g, t + 0x20u, 0u);
                direct_write32_if_ram(g, t + 0x14u, 2u);
                state = 2u;
            } else {
                direct_write32_if_ram(g, t + 0x20u, elapsed + 1u);
            }
        }
        if (!direct_task_state_can_run(g, t)) continue;
        uint32_t saved_sp = s3c2400_debug_read32(g->soc, t + 0u);
        if (state == 8u) {
            uint32_t link_or_stack = s3c2400_debug_read32(g->soc, t + 4u);
            uint32_t stack_size = s3c2400_debug_read32(g->soc, t + 8u);
            if (!direct_ram_range(g, link_or_stack, 4u) || stack_size == 0u || stack_size > 0x20000u) continue;
        }
        if (!direct_ram_range(g, saved_sp, 64u)) continue;
        uint32_t saved_cpsr = s3c2400_debug_read32(g->soc, saved_sp + 0u);
        uint32_t saved_pc = s3c2400_debug_read32(g->soc, saved_sp + 60u);
        if (!direct_saved_cpsr_plausible(saved_cpsr)) continue;
        if (!direct_ram_range(g, saved_pc & ~1u, 4u)) continue;
        if ((saved_pc & ~1u) == pc || ((saved_pc + 4u) & ~1u) == pc) continue;
        uint32_t saved_insn = s3c2400_debug_read32(g->soc, saved_pc & ~1u);
        if (saved_insn == 0xeafffffeu || saved_insn == 0xeaffffffu) continue;
        direct_set_task_scheduler_current(g, t, pc);
        direct_write32_if_ram(g, t + 0x14u, 1u);
        if (direct_restore_saved_context(g, t)) return 1;
    }
    return 0;
}

/* Recalculate after every CPU yield: a guest clock-register write ends the
 * current slice, and resumable HLE callbacks also advance elapsed time. */
static uint32_t direct_frame_budget(const gp32_t *g, uint64_t deadline) {
    if (!direct_time_pending(deadline, g->elapsed.nanoseconds)) return 0u;
    uint32_t clock = direct_run_clock_hz(g);
    uint64_t ns = deadline - g->elapsed.nanoseconds;
    if (ns > (uint64_t)32768u * 1000000000u / clock + 1u) return 32768u;
    uint64_t fraction = g->elapsed.remainder;
    if (g->elapsed.clock_hz && g->elapsed.clock_hz != clock)
        fraction = fraction * clock / g->elapsed.clock_hz;
    uint64_t cycles = (ns * clock - fraction + 999999999u) / 1000000000u;
    return cycles > 32768u ? 32768u : (uint32_t)cycles;
}

static int direct_fast_load_audio_running(gp32_t *g) {
    return s3c2400_iis_running(g->soc) || g->direct_hle_pcm_active || g->direct_hle_audio_asset;
}

static gp32_status_t gp32_run(gp32_t *g, uint32_t cycles, int timed, uint32_t restore_speed) {
    if (!g->direct_callback.owner) direct_fix_gp32_additive_blend_shadow_endpoint(g);
    direct_adpcm_fix_update(g, 1);
    direct_pinball_fix_update(g, 1);
    uint32_t remaining = cycles;
    gp32_status_t status = GP32_OK;
    for (;;) {
        if (g->direct_callback.owner == DIRECT_CB_FAULT) {
            direct_callback_error(g);
            status = GP32_ERR_CPU_FAULT;
            break;
        }
        if (!g->direct_callback.owner && g->direct_tick.clock) {
            direct_hle_tick_pump(g);
            if (!g->direct_callback.owner) {
                direct_schedule_vblank_wait(g);
                direct_try_file_hle(g);
                direct_try_resume_sdk_task(g);
            }
        }
        /* Host delivery point. PCM produced by the previous slice (guest IIS
         * DMA, HLE mixing, hardware idle fill) is already in the SoC queue,
         * so a frontend can release it before this frame retires instead of
         * waiting out a guest frame that overruns real time. */
        if (g->host_pump) g->host_pump(g, g->host_pump_user);
        /* Loading can finish inside a frame. Stop boosting before the next
         * CPU slice once sound or a callback resumes; playing code must not
         * inherit the load's faster instruction budget. */
        if (restore_speed && (direct_fast_load_audio_running(g) || g->direct_callback.owner)) {
            (void)s3c2400_set_cpu_speed_percent(g->soc, restore_speed);
            restore_speed = 0u;
            g->fast_load_active = g->fast_load_streak = 0u;
        }
        if (timed) remaining = direct_frame_budget(g, g->frame_time.deadline_ns);
        if (!remaining) break;
        if (g->direct_callback.owner) {
            uint32_t slice = remaining > 4096u ? 4096u : remaining;
            direct_update_fw_tick(g);
            uint32_t clock = direct_run_clock_hz(g);
            uint32_t suspended = g->direct_callback.suspended;
            uint64_t before_ns = g->elapsed.nanoseconds;
            uint32_t done = direct_run_cpu(g, slice, clock);
            remaining = done >= remaining ? 0u : remaining - done;
            direct_update_fw_tick(g);
            if (suspended) g->direct_callback.deadline_ns += g->elapsed.nanoseconds - before_ns;
            /* A valid trap wins over timeout in the same completed slice. */
            if (g->direct_hle_callback_returned) {
                direct_callback_tail_t *cb = &g->direct_callback;
                arm920t_set_register_context(g->cpu, &cb->foreground);
                if (cb->owner == DIRECT_CB_REFILL)
                    direct_write32_if_ram(g, cb->ack_addr, cb->ack_value);
                memset(cb, 0, sizeof(*cb));
                g->direct_hle_callback_returned = 0u;
                g->direct_hle_callback_running = 0u;
            } else if (g->direct_callback.owner != DIRECT_CB_FAULT) {
                if (!done && !g->direct_callback.suspended) direct_callback_fault(g, DIRECT_CB_STALLED);
                else if (!direct_time_pending(g->direct_callback.deadline_ns, g->elapsed.nanoseconds))
                    direct_callback_fault(g, DIRECT_CB_TIMEOUT);
            }
            /* SDK idle recovery must be able to resume the saved callback
             * task. Its consumer tail still owns HLE mixing/timer dispatch;
             * executing another callback here would reuse its private stack. */
            if (g->direct_callback.suspended && g->direct_callback.owner != DIRECT_CB_FAULT) {
                int progressed = direct_try_file_hle(g);
                progressed |= direct_try_resume_sdk_task(g);
                if (!done && !progressed) break;
            }
            continue;
        }
        uint32_t slice = remaining > 32768u ? 32768u : remaining;
        direct_update_fw_tick(g);
        uint32_t clock = direct_run_clock_hz(g);
        uint32_t done = direct_run_cpu(g, slice, clock);
        remaining = done >= remaining ? 0u : remaining - done;
        direct_hle_tick(g, done, clock);
        direct_update_fw_tick(g);
        direct_process_asset_autoload(g);
        if (g->direct_callback.owner) continue;
        direct_schedule_vblank_wait(g);
        if (done == 0) {
            if (direct_try_file_hle(g)) continue;
            if (direct_try_resume_sdk_task(g)) continue;
            break;
        }
        if (direct_try_file_hle(g)) {
            continue;
        }
        if (direct_try_resume_sdk_task(g)) {
            continue;
        }
    }
    direct_update_fw_tick(g);
    direct_process_asset_autoload(g);
    if (!g->direct_callback.owner) direct_fix_gp32_additive_blend_shadow_endpoint(g);
    direct_select_visible_surface(g);
    return status;
}

gp32_status_t gp32_run_cycles(gp32_t *g, uint32_t cycles) {
    if (!g) return GP32_ERR_INVALID_ARGUMENT;
    /* Explicit stepping establishes a fresh origin for the next host frame. */
    memset(&g->frame_time, 0, sizeof(g->frame_time));
    gp32_status_t st = gp32_run(g, cycles, 0, 0u);
    direct_fast_load_reset(g);
    return st;
}

/*
 * Fast loading. Some games freeze the screen, stop audio and pull hundreds of
 * KiB from SmartMedia through the GPIO bit-bang driver; Blue Angelo spends
 * about 1.1 s of guest time this way whenever an NPC dialogue opens. While all
 * of that holds, frames run the guest CPU at four times its programmed speed.
 * Audio, timers and the LCD keep real time, so only the silent, frozen load
 * ends sooner. Entry also requires audio to have stopped within the last 32
 * frames, which keeps games that stream from the card during play (Little
 * Girl Mill) at normal speed.
 */
#define GP32_FAST_LOAD_MIN_BYTES 1024u
#define GP32_FAST_LOAD_RECENT    32u
#define GP32_FAST_LOAD_ENTRY     3u
#define GP32_FAST_LOAD_FACTOR    4u

static void direct_fast_load_update(gp32_t *g) {
    uint64_t total = s3c2400_smc_bytes_read(g->soc);
    uint64_t bytes = total >= g->fast_load_mark ? total - g->fast_load_mark : 0u;
    g->fast_load_mark = total;
    int audio = direct_fast_load_audio_running(g);
    if (audio) g->fast_load_recent_audio = GP32_FAST_LOAD_RECENT;
    int busy = !g->fast_load_disabled && !audio && bytes >= GP32_FAST_LOAD_MIN_BYTES && !g->direct_callback.owner &&
               (g->fast_load_active || g->fast_load_recent_audio);
    if (!audio && g->fast_load_recent_audio) --g->fast_load_recent_audio;
    if (busy) {
        uint64_t h = s3c2400_lcd_surface_hash(g->soc);
        busy = h && g->fast_load_hash_valid && h == g->fast_load_hash;
        g->fast_load_hash = h;
        g->fast_load_hash_valid = h != 0u;
    } else {
        g->fast_load_hash_valid = 0u;
    }
    if (!busy) g->fast_load_streak = 0u;
    else if (g->fast_load_streak < 255u) g->fast_load_streak++;
    g->fast_load_active = g->fast_load_streak >= GP32_FAST_LOAD_ENTRY;
}

gp32_status_t gp32_run_frame(gp32_t *g) {
    if (!g) return GP32_ERR_INVALID_ARGUMENT;
    if (!g->frame_time.valid) {
        g->frame_time.deadline_ns = g->elapsed.nanoseconds;
        g->frame_time.remainder = 0u;
        g->frame_time.valid = 1u;
    }
    /* Retain instruction/callback overshoot across frames instead of adding
     * another full interval to the time already consumed. */
    uint32_t ns = 1000000000u + g->frame_time.remainder;
    g->frame_time.deadline_ns += ns / 60u;
    g->frame_time.remainder = ns % 60u;
    uint32_t user_speed = 0u;
    if (g->fast_load_active) {
        user_speed = s3c2400_cpu_speed_percent(g->soc);
        uint32_t boosted = user_speed * GP32_FAST_LOAD_FACTOR;
        if (!s3c2400_set_cpu_speed_percent(g->soc, boosted > 400u ? 400u : boosted)) user_speed = 0u;
    }
    gp32_status_t st = gp32_run(g, 0u, 1, user_speed);
    if (user_speed) (void)s3c2400_set_cpu_speed_percent(g->soc, user_speed);
    direct_fast_load_update(g);
    if (direct_time_pending(g->frame_time.deadline_ns, g->elapsed.nanoseconds))
        memset(&g->frame_time, 0, sizeof(g->frame_time)); /* CPU stopped */
    return direct_service_reboot_request(g, st);
}

gp32_status_t gp32_set_jit(gp32_t *g, int enabled) {
    if (!g || !g->cpu) return GP32_ERR_INVALID_ARGUMENT;
    arm920t_set_jit(g->cpu, enabled);
    return GP32_OK;
}

/* Opt-in sweep diagnostics: undefined-instruction and abort vector entries.
 * Not part of the machine state; a NULL sink leaves the run unchanged. */
void gp32_set_diag_log(gp32_t *g, gp32_diag_fn fn, void *user) {
    if (g && g->cpu) arm920t_set_diag(g->cpu, fn, user);
}

/* Transient host delivery hook; unlike the machine state it is not
 * serialized, so a frontend must reinstall it after a state load if it
 * replaced the instance. */
void gp32_set_host_pump(gp32_t *g, gp32_host_pump_fn fn, void *user) {
    if (!g) return;
    g->host_pump = fn;
    g->host_pump_user = user;
}

gp32_status_t gp32_set_cpu_speed_percent(gp32_t *g, uint32_t percent) {
    if (!g || !g->soc || g->direct_cpu_running) return GP32_ERR_INVALID_ARGUMENT;
    if (!s3c2400_set_cpu_speed_percent(g->soc, percent)) {
        seterr(g, "CPU speed must be 50..400 percent: %u", (unsigned)percent);
        return GP32_ERR_INVALID_ARGUMENT;
    }
    return GP32_OK;
}

uint32_t gp32_get_cpu_speed_percent(const gp32_t *g) {
    return g ? s3c2400_cpu_speed_percent(g->soc) : 100u;
}

/* Savestates always describe the unmodified machine: phases are converted to
 * the guest-programmed clock around a save or load, so a state written at any
 * CPU speed loads at any other. */
static uint32_t gp32_cpu_speed_to_nominal(gp32_t *g) {
    uint32_t percent = s3c2400_cpu_speed_percent(g->soc);
    if (percent != 100u) (void)s3c2400_set_cpu_speed_percent(g->soc, 100u);
    return percent;
}

static void gp32_cpu_speed_restore(gp32_t *g, uint32_t percent) {
    if (percent != 100u) (void)s3c2400_set_cpu_speed_percent(g->soc, percent);
}

gp32_status_t gp32_set_fast_loading(gp32_t *g, int enabled) {
    if (!g) return GP32_ERR_INVALID_ARGUMENT;
    uint8_t disabled = enabled ? 0u : 1u;
    if (disabled != g->fast_load_disabled) direct_fast_load_reset(g);
    g->fast_load_disabled = disabled;
    return GP32_OK;
}

gp32_status_t gp32_set_game_fixes(gp32_t *g, int enabled) {
    if (!g || g->direct_cpu_running) return GP32_ERR_INVALID_ARGUMENT;
    g->adpcm_fix_disabled = enabled ? 0u : 1u;
    direct_adpcm_fix_update(g, 1);
    direct_pinball_fix_update(g, 1);
    return GP32_OK;
}

gp32_status_t gp32_set_hle_sef_rate(gp32_t *g, uint32_t sample_rate_hz) {
    if (!g) return GP32_ERR_INVALID_ARGUMENT;
    if (sample_rate_hz && (sample_rate_hz < 4000u || sample_rate_hz > 192000u)) {
        seterr(g, "HLE SEF sample rate must be 0 or 4000..192000 Hz");
        return GP32_ERR_INVALID_ARGUMENT;
    }
    g->direct_hle_audio_rate_override = sample_rate_hz;
    if (sample_rate_hz && g->direct_hle_audio_asset) {
        g->direct_hle_audio_rate = sample_rate_hz;
        g->direct_hle_audio_accum = 0;
    }
    return GP32_OK;
}

gp32_status_t gp32_set_buttons(gp32_t *g, uint32_t mask) {
    if (!g) return GP32_ERR_INVALID_ARGUMENT;
    s3c2400_set_buttons(g->soc, mask);
    return GP32_OK;
}

gp32_status_t gp32_get_framebuffer(gp32_t *g, gp32_framebuffer_desc_t *out) {
    if (!g || !out) return GP32_ERR_INVALID_ARGUMENT;
    direct_fix_gp32_additive_blend_shadow_endpoint(g);
    direct_fix_gp32_additive_blend_shadow_pixels(g);
    uint32_t w,h,stride; uint64_t frames;
    const uint32_t *pix = s3c2400_framebuffer(g->soc, &w, &h, &stride, &frames);
    out->pixels_rgba8888 = pix;
    out->width = w;
    out->height = h;
    out->stride_pixels = stride;
    out->frame_counter = frames;
    return pix ? GP32_OK : GP32_ERR_INVALID_ARGUMENT;
}

gp32_status_t gp32_get_audio(gp32_t *g, gp32_audio_desc_t *out) {
    if (!g || !out) return GP32_ERR_INVALID_ARGUMENT;
    uint64_t frames = 0;
    uint32_t rate = 0;
    const int16_t *samples = s3c2400_audio_samples(g->soc, &frames, &rate);
    out->samples_s16_interleaved = samples;
    out->frame_count = frames;
    out->sample_rate_hz = rate;
    return GP32_OK;
}

gp32_status_t gp32_consume_audio(gp32_t *g, uint64_t frames) {
    return g && s3c2400_audio_consume(g->soc, frames) ? GP32_OK : GP32_ERR_INVALID_ARGUMENT;
}

gp32_status_t gp32_clear_audio(gp32_t *g) {
    if (!g) return GP32_ERR_INVALID_ARGUMENT;
    s3c2400_audio_clear(g->soc);
    return GP32_OK;
}

uint32_t gp32_get_pc(const gp32_t *g) { return g ? arm920t_get_pc(g->cpu) : 0; }
uint32_t gp32_get_cpu_reg(const gp32_t *g, unsigned reg) { return g ? arm920t_get_reg(g->cpu, reg) : 0; }
uint32_t gp32_get_cpsr(const gp32_t *g) { return g ? arm920t_get_cpsr(g->cpu) : 0; }
uint32_t gp32_get_cp15(const gp32_t *g, unsigned reg) { return g ? arm920t_get_cp15(g->cpu, reg) : 0; }
uint32_t gp32_debug_read32(gp32_t *g, uint32_t addr) { return g ? s3c2400_debug_read32(g->soc, addr) : 0xffffffffu; }
uint64_t gp32_get_cycles(const gp32_t *g) { return g ? arm920t_get_cycles(g->cpu) : 0; }
uint32_t gp32_get_fclk_hz(const gp32_t *g) { return g ? s3c2400_fclk_hz(g->soc) : 0u; }
uint32_t gp32_get_run_clock_hz(const gp32_t *g) { return g ? s3c2400_run_clock_hz(g->soc) : 0u; }
int gp32_get_lcd_frame_period(const gp32_t *g, uint32_t *period_ns, uint32_t *period_frac) {
    return g ? s3c2400_lcd_frame_period(g->soc, period_ns, period_frac) : 0;
}
uint64_t gp32_get_jit_hits(const gp32_t *g) { return g ? arm920t_get_jit_hits(g->cpu) : 0; }
uint64_t gp32_get_jit_misses(const gp32_t *g) { return g ? arm920t_get_jit_misses(g->cpu) : 0; }
uint64_t gp32_get_jit_fallbacks(const gp32_t *g) { return g ? arm920t_get_jit_fallbacks(g->cpu) : 0; }
gp32_status_t gp32_get_cpu_profile(const gp32_t *g, gp32_cpu_profile_t *out) {
    if (!out) return GP32_ERR_INVALID_ARGUMENT;
    arm920t_get_cpu_profile(g ? g->cpu : NULL, out);
    return GP32_OK;
}
gp32_status_t gp32_reset_cpu_profile(gp32_t *g) {
    if (!g) return GP32_ERR_INVALID_ARGUMENT;
    arm920t_reset_cpu_profile(g->cpu);
    return GP32_OK;
}
const char *gp32_get_error(const gp32_t *g) { return g ? g->error : "invalid gp32 handle"; }

typedef struct gp32_fpk_handle_state_image {
    int32_t asset_index;
    size_t pos;
    int used;
} gp32_fpk_handle_state_image_t;

typedef struct gp32_state_image {
    int direct_fxe_mode;
    uint32_t direct_fxe_entry;
    uint32_t direct_fxe_stack;
    uint32_t direct_fxe_fb_addr;
    uint32_t direct_fxe_image_end;
    uint32_t direct_fxe_lcd_surface[4];
    uint32_t direct_fxe_surface_checksum[4];
    uint32_t direct_fxe_bpp;
    uint32_t direct_fxe_palette_addr;
    uint32_t direct_fxe_palette_initialized;
    uint32_t direct_fxe_lcd_explicit;
    uint32_t direct_fxe_lcd_enabled;
    char direct_fxe_title[33];
    char direct_smc_executable_path[260];
    char direct_smc_game_dir[260];
    gp32_fpk_handle_state_image_t direct_fpk_handles[32];
    uint32_t direct_hle_file_open_addr;
    uint32_t direct_hle_file_read_addr[2];
    uint32_t direct_hle_file_close_addr;
    uint32_t direct_hle_file_size_addr;
    uint32_t direct_hle_file_seek_addr;
    uint32_t direct_hle_pathbuf_addr;
    uint32_t direct_hle_sound_dispatch_addr;
    uint32_t direct_hle_sound_table_addr;
    uint32_t direct_hle_sound_play_addr;
    uint32_t direct_hle_sound_state_addr;
    uint32_t direct_hle_pcm_env_addr;
    uint32_t direct_hle_pcm_init_addr;
    uint32_t direct_hle_pcm_play_addr;
    uint32_t direct_hle_pcm_stop_addr;
    uint32_t direct_hle_pcm_remove_addr;
    uint32_t direct_hle_pcm_lock_addr;
    uint32_t direct_hle_pcm_only_kill_addr;
    uint32_t direct_hle_pcm_initialized;
    uint32_t direct_hle_pcm_sr;
    uint32_t direct_hle_pcm_bit_count;
    uint32_t direct_hle_pcm_rate;
    uint32_t direct_hle_pcm_stereo;
    uint32_t direct_hle_pcm_bits;
    uint32_t direct_hle_pcm_active;
    uint32_t direct_hle_pcm_src_addr;
    uint32_t direct_hle_pcm_size_bytes;
    uint32_t direct_hle_pcm_pos_bytes;
    uint32_t direct_hle_pcm_repeat;
    uint64_t direct_hle_pcm_accum;
    uint32_t direct_hle_sdk_sndmixedbuf_addr;
    uint32_t direct_hle_sdk_sndsrcexist_addr;
    uint32_t direct_hle_sdk_pcm_workidx_addr;
    uint32_t direct_hle_sdk_sndmixer_addr;
    uint32_t direct_hle_sdk_mixbuf0_addr;
    uint32_t direct_hle_sdk_mixbuf1_addr;
    uint32_t direct_hle_sdk_mixbuf_bytes;
    uint32_t direct_hle_sdk_rate;
    uint64_t direct_hle_sdk_accum;
    uint64_t direct_hle_sdk_last_submit_cycle;
    uint64_t direct_hle_sdk_timer_accum;
    uint32_t direct_hle_sdk_submitted_frames;
    uint32_t direct_hle_sdk_timer_table_addr; /* Legacy state field; no runtime lookup. */
    struct {
        uint32_t configured;
        uint32_t enabled;
        uint32_t callback;
        uint32_t tps;
        uint32_t max_exec_tick;
        uint64_t accum;
    } direct_hle_gpos_timer[GP32_DIRECT_GPOS_TIMER_COUNT];
    uint32_t direct_hle_gpos_timers_enabled;
    uint32_t direct_hle_gpos_task_first;
    uint32_t direct_hle_gpos_task_last;
    uint32_t direct_hle_gpos_scheduler_callback;
    uint32_t direct_hle_callback_returned;
    uint32_t direct_hle_callback_running;
    uint32_t direct_hle_audio_rate_override;
    uint32_t direct_hle_audio_last_auto_rate;
    int32_t direct_hle_audio_asset_index;
    uint32_t direct_hle_audio_pos;
    uint32_t direct_hle_audio_size;
    uint32_t direct_hle_audio_rate;
    uint64_t direct_hle_audio_accum;
    struct {
        const fpk_asset_t *asset;
        uint32_t copied;
        uint32_t tries;
    } direct_hle_asset_autoload[16];
    uint64_t direct_vblank_next_cycle;
    uint64_t direct_vblank_wait_cycles;
    int direct_vblank_wait_requested;
} gp32_state_image_t;

static int32_t direct_asset_index(const gp32_t *g, const fpk_asset_t *asset) {
    if (!g || !asset || !g->direct_fpk_assets || g->direct_fpk_asset_count == 0) return -1;
    for (size_t i = 0; i < g->direct_fpk_asset_count; ++i) {
        if (&g->direct_fpk_assets[i] == asset) return (int32_t)i;
    }
    return -1;
}

static const fpk_asset_t *direct_asset_from_index(const gp32_t *g, int32_t index) {
    if (!g || index < 0 || !g->direct_fpk_assets) return NULL;
    if ((size_t)index >= g->direct_fpk_asset_count) return NULL;
    return &g->direct_fpk_assets[index];
}

static void gp32_direct_state_capture(const gp32_t *g, gp32_state_image_t *st) {
    memset(st, 0, sizeof(*st));
    st->direct_fxe_mode = g->direct_fxe_mode;
    st->direct_fxe_entry = g->direct_fxe_entry;
    st->direct_fxe_stack = g->direct_fxe_stack;
    st->direct_fxe_fb_addr = g->direct_fxe_fb_addr;
    st->direct_fxe_image_end = g->direct_fxe_image_end;
    memcpy(st->direct_fxe_lcd_surface, g->direct_fxe_lcd_surface, sizeof(st->direct_fxe_lcd_surface));
    memcpy(st->direct_fxe_surface_checksum, g->direct_fxe_surface_checksum, sizeof(st->direct_fxe_surface_checksum));
    st->direct_fxe_bpp = g->direct_fxe_bpp;
    st->direct_fxe_palette_addr = g->direct_fxe_palette_addr;
    st->direct_fxe_palette_initialized = g->direct_fxe_palette_initialized;
    st->direct_fxe_lcd_explicit = g->direct_fxe_lcd_explicit;
    st->direct_fxe_lcd_enabled = g->direct_fxe_lcd_enabled;
    memcpy(st->direct_fxe_title, g->direct_fxe_title, sizeof(st->direct_fxe_title));
    memcpy(st->direct_smc_executable_path, g->direct_smc_executable_path, sizeof(st->direct_smc_executable_path));
    memcpy(st->direct_smc_game_dir, g->direct_smc_game_dir, sizeof(st->direct_smc_game_dir));
    for (size_t i = 0; i < GP32_ARRAY_COUNT(st->direct_fpk_handles); ++i) {
        st->direct_fpk_handles[i].asset_index = direct_asset_index(g, g->direct_fpk_handles[i].asset);
        st->direct_fpk_handles[i].pos = g->direct_fpk_handles[i].pos;
        st->direct_fpk_handles[i].used = g->direct_fpk_handles[i].used;
    }
    st->direct_hle_file_open_addr = g->direct_hle_file_open_addr;
    memcpy(st->direct_hle_file_read_addr, g->direct_hle_file_read_addr, sizeof(st->direct_hle_file_read_addr));
    st->direct_hle_file_close_addr = g->direct_hle_file_close_addr;
    st->direct_hle_file_size_addr = g->direct_hle_file_size_addr;
    st->direct_hle_file_seek_addr = g->direct_hle_file_seek_addr;
    st->direct_hle_pathbuf_addr = g->direct_hle_pathbuf_addr;
    st->direct_hle_sound_dispatch_addr = g->direct_hle_sound_dispatch_addr;
    st->direct_hle_sound_table_addr = g->direct_hle_sound_table_addr;
    st->direct_hle_sound_play_addr = g->direct_hle_sound_play_addr;
    st->direct_hle_sound_state_addr = g->direct_hle_sound_state_addr;
    st->direct_hle_pcm_env_addr = g->direct_hle_pcm_env_addr;
    st->direct_hle_pcm_init_addr = g->direct_hle_pcm_init_addr;
    st->direct_hle_pcm_play_addr = g->direct_hle_pcm_play_addr;
    st->direct_hle_pcm_stop_addr = g->direct_hle_pcm_stop_addr;
    st->direct_hle_pcm_remove_addr = g->direct_hle_pcm_remove_addr;
    st->direct_hle_pcm_lock_addr = g->direct_hle_pcm_lock_addr;
    st->direct_hle_pcm_only_kill_addr = g->direct_hle_pcm_only_kill_addr;
    st->direct_hle_pcm_initialized = g->direct_hle_pcm_initialized;
    st->direct_hle_pcm_sr = g->direct_hle_pcm_sr;
    st->direct_hle_pcm_bit_count = g->direct_hle_pcm_bit_count;
    st->direct_hle_pcm_rate = g->direct_hle_pcm_rate;
    st->direct_hle_pcm_stereo = g->direct_hle_pcm_stereo;
    st->direct_hle_pcm_bits = g->direct_hle_pcm_bits;
    st->direct_hle_pcm_active = g->direct_hle_pcm_active;
    st->direct_hle_pcm_src_addr = g->direct_hle_pcm_src_addr;
    st->direct_hle_pcm_size_bytes = g->direct_hle_pcm_size_bytes;
    st->direct_hle_pcm_pos_bytes = g->direct_hle_pcm_pos_bytes;
    st->direct_hle_pcm_repeat = g->direct_hle_pcm_repeat;
    st->direct_hle_pcm_accum = g->direct_hle_pcm_accum;
    st->direct_hle_sdk_sndmixedbuf_addr = g->direct_hle_sdk_sndmixedbuf_addr;
    st->direct_hle_sdk_sndsrcexist_addr = g->direct_hle_sdk_sndsrcexist_addr;
    st->direct_hle_sdk_pcm_workidx_addr = g->direct_hle_sdk_pcm_workidx_addr;
    st->direct_hle_sdk_sndmixer_addr = g->direct_hle_sdk_sndmixer_addr;
    st->direct_hle_sdk_mixbuf0_addr = g->direct_hle_sdk_mixbuf0_addr;
    st->direct_hle_sdk_mixbuf1_addr = g->direct_hle_sdk_mixbuf1_addr;
    st->direct_hle_sdk_mixbuf_bytes = g->direct_hle_sdk_mixbuf_bytes;
    st->direct_hle_sdk_rate = g->direct_hle_sdk_rate;
    st->direct_hle_sdk_accum = g->direct_hle_sdk_accum;
    st->direct_hle_sdk_last_submit_cycle = g->direct_hle_sdk_last_submit_cycle;
    st->direct_hle_sdk_timer_accum = g->direct_hle_sdk_timer_accum;
    st->direct_hle_sdk_submitted_frames = g->direct_hle_sdk_submitted_frames;
    st->direct_hle_sdk_timer_table_addr = g->direct_hle_sdk_timer_table_addr;
    memcpy(st->direct_hle_gpos_timer, g->direct_hle_gpos_timer, sizeof(st->direct_hle_gpos_timer));
    st->direct_hle_gpos_timers_enabled = g->direct_hle_gpos_timers_enabled;
    st->direct_hle_gpos_task_first = g->direct_hle_gpos_task_first;
    st->direct_hle_gpos_task_last = g->direct_hle_gpos_task_last;
    st->direct_hle_gpos_scheduler_callback = g->direct_hle_gpos_scheduler_callback;
    st->direct_hle_callback_returned = g->direct_hle_callback_returned;
    st->direct_hle_callback_running = g->direct_hle_callback_running;
    st->direct_hle_audio_rate_override = g->direct_hle_audio_rate_override;
    st->direct_hle_audio_last_auto_rate = g->direct_hle_audio_last_auto_rate;
    st->direct_hle_audio_asset_index = direct_asset_index(g, g->direct_hle_audio_asset);
    st->direct_hle_audio_pos = g->direct_hle_audio_pos;
    st->direct_hle_audio_size = g->direct_hle_audio_size;
    st->direct_hle_audio_rate = g->direct_hle_audio_rate;
    st->direct_hle_audio_accum = g->direct_hle_audio_accum;
    /* Legacy fields remain on the wire; new waits live in CPU/RAM. */
    st->direct_vblank_next_cycle = 0u;
    st->direct_vblank_wait_cycles = 0u;
    st->direct_vblank_wait_requested = 0;
}

static void gp32_direct_state_apply(gp32_t *g, const gp32_state_image_t *st) {
    g->direct_hle_pending_volume = 0;
    g->direct_cpu_running = 0;
    direct_set_fxe_mode(g, st->direct_fxe_mode);
    g->direct_fxe_entry = st->direct_fxe_entry;
    g->direct_fxe_stack = st->direct_fxe_stack;
    g->direct_fxe_fb_addr = st->direct_fxe_fb_addr;
    g->direct_fxe_image_end = st->direct_fxe_image_end;
    memcpy(g->direct_fxe_lcd_surface, st->direct_fxe_lcd_surface, sizeof(g->direct_fxe_lcd_surface));
    memcpy(g->direct_fxe_surface_checksum, st->direct_fxe_surface_checksum, sizeof(g->direct_fxe_surface_checksum));
    g->direct_fxe_bpp = st->direct_fxe_bpp;
    g->direct_fxe_palette_addr = st->direct_fxe_palette_addr;
    g->direct_fxe_palette_initialized = st->direct_fxe_palette_initialized;
    g->direct_fxe_lcd_explicit = st->direct_fxe_lcd_explicit;
    g->direct_fxe_lcd_enabled = st->direct_fxe_lcd_enabled;
    memcpy(g->direct_fxe_title, st->direct_fxe_title, sizeof(g->direct_fxe_title));
    g->direct_fxe_title[sizeof(g->direct_fxe_title) - 1u] = '\0';
    memcpy(g->direct_smc_executable_path, st->direct_smc_executable_path, sizeof(g->direct_smc_executable_path));
    g->direct_smc_executable_path[sizeof(g->direct_smc_executable_path) - 1u] = '\0';
    memcpy(g->direct_smc_game_dir, st->direct_smc_game_dir, sizeof(g->direct_smc_game_dir));
    g->direct_smc_game_dir[sizeof(g->direct_smc_game_dir) - 1u] = '\0';
    for (size_t i = 0; i < GP32_ARRAY_COUNT(st->direct_fpk_handles); ++i) {
        g->direct_fpk_handles[i].asset = direct_asset_from_index(g, st->direct_fpk_handles[i].asset_index);
        g->direct_fpk_handles[i].pos = st->direct_fpk_handles[i].pos;
        g->direct_fpk_handles[i].used = st->direct_fpk_handles[i].used && g->direct_fpk_handles[i].asset;
    }
    g->direct_hle_file_open_addr = st->direct_hle_file_open_addr;
    memcpy(g->direct_hle_file_read_addr, st->direct_hle_file_read_addr, sizeof(g->direct_hle_file_read_addr));
    g->direct_hle_file_close_addr = st->direct_hle_file_close_addr;
    g->direct_hle_file_size_addr = st->direct_hle_file_size_addr;
    g->direct_hle_file_seek_addr = st->direct_hle_file_seek_addr;
    g->direct_hle_pathbuf_addr = st->direct_hle_pathbuf_addr;
    g->direct_hle_sound_dispatch_addr = st->direct_hle_sound_dispatch_addr;
    g->direct_hle_sound_table_addr = st->direct_hle_sound_table_addr;
    g->direct_hle_sound_play_addr = st->direct_hle_sound_play_addr;
    g->direct_hle_sound_state_addr = st->direct_hle_sound_state_addr;
    g->direct_hle_pcm_env_addr = st->direct_hle_pcm_env_addr;
    g->direct_hle_pcm_init_addr = st->direct_hle_pcm_init_addr;
    g->direct_hle_pcm_play_addr = st->direct_hle_pcm_play_addr;
    g->direct_hle_pcm_stop_addr = st->direct_hle_pcm_stop_addr;
    g->direct_hle_pcm_remove_addr = st->direct_hle_pcm_remove_addr;
    g->direct_hle_pcm_lock_addr = st->direct_hle_pcm_lock_addr;
    g->direct_hle_pcm_only_kill_addr = st->direct_hle_pcm_only_kill_addr;
    g->direct_hle_pcm_initialized = st->direct_hle_pcm_initialized;
    g->direct_hle_pcm_sr = st->direct_hle_pcm_sr;
    g->direct_hle_pcm_bit_count = st->direct_hle_pcm_bit_count;
    g->direct_hle_pcm_rate = st->direct_hle_pcm_rate;
    g->direct_hle_pcm_stereo = st->direct_hle_pcm_stereo;
    g->direct_hle_pcm_bits = st->direct_hle_pcm_bits;
    g->direct_hle_pcm_active = st->direct_hle_pcm_active;
    g->direct_hle_pcm_src_addr = st->direct_hle_pcm_src_addr;
    g->direct_hle_pcm_size_bytes = st->direct_hle_pcm_size_bytes;
    g->direct_hle_pcm_pos_bytes = st->direct_hle_pcm_pos_bytes;
    g->direct_hle_pcm_repeat = st->direct_hle_pcm_repeat;
    g->direct_hle_pcm_accum = st->direct_hle_pcm_accum;
    memset(g->direct_hle_pcm_ch, 0, sizeof(g->direct_hle_pcm_ch));
    if (g->direct_hle_pcm_active) {
        g->direct_hle_pcm_ch[0].active = g->direct_hle_pcm_active;
        g->direct_hle_pcm_ch[0].src_addr = g->direct_hle_pcm_src_addr;
        g->direct_hle_pcm_ch[0].size_bytes = g->direct_hle_pcm_size_bytes;
        g->direct_hle_pcm_ch[0].pos_bytes = g->direct_hle_pcm_pos_bytes;
        g->direct_hle_pcm_ch[0].repeat = g->direct_hle_pcm_repeat;
        g->direct_hle_pcm_ch[0].rate = g->direct_hle_pcm_rate;
        g->direct_hle_pcm_ch[0].stereo = g->direct_hle_pcm_stereo;
        g->direct_hle_pcm_ch[0].bits = g->direct_hle_pcm_bits;
    }
    g->direct_hle_sdk_sndmixedbuf_addr = st->direct_hle_sdk_sndmixedbuf_addr;
    g->direct_hle_sdk_sndsrcexist_addr = st->direct_hle_sdk_sndsrcexist_addr;
    g->direct_hle_sdk_pcm_workidx_addr = st->direct_hle_sdk_pcm_workidx_addr;
    g->direct_hle_sdk_sndmixer_addr = st->direct_hle_sdk_sndmixer_addr;
    g->direct_hle_sdk_mixbuf0_addr = st->direct_hle_sdk_mixbuf0_addr;
    g->direct_hle_sdk_mixbuf1_addr = st->direct_hle_sdk_mixbuf1_addr;
    g->direct_hle_sdk_mixbuf_bytes = st->direct_hle_sdk_mixbuf_bytes;
    g->direct_hle_sdk_rate = st->direct_hle_sdk_rate;
    g->direct_hle_sdk_accum = st->direct_hle_sdk_accum;
    g->direct_hle_sdk_last_submit_cycle = st->direct_hle_sdk_last_submit_cycle;
    g->direct_hle_sdk_timer_accum = st->direct_hle_sdk_timer_accum;
    g->direct_hle_sdk_submitted_frames = st->direct_hle_sdk_submitted_frames;
    g->direct_hle_sdk_timer_table_addr = st->direct_hle_sdk_timer_table_addr;
    memcpy(g->direct_hle_gpos_timer, st->direct_hle_gpos_timer, sizeof(g->direct_hle_gpos_timer));
    g->direct_hle_gpos_timers_enabled = st->direct_hle_gpos_timers_enabled;
    g->direct_hle_gpos_task_first = st->direct_hle_gpos_task_first;
    g->direct_hle_gpos_task_last = st->direct_hle_gpos_task_last;
    g->direct_hle_gpos_scheduler_callback = st->direct_hle_gpos_scheduler_callback;
    g->direct_hle_callback_returned = st->direct_hle_callback_returned;
    g->direct_hle_callback_running = st->direct_hle_callback_running;
    g->direct_hle_audio_rate_override = st->direct_hle_audio_rate_override;
    g->direct_hle_audio_last_auto_rate = st->direct_hle_audio_last_auto_rate;
    g->direct_hle_audio_asset = direct_asset_from_index(g, st->direct_hle_audio_asset_index);
    g->direct_hle_audio_pos = st->direct_hle_audio_pos;
    g->direct_hle_audio_size = st->direct_hle_audio_size;
    g->direct_hle_audio_rate = st->direct_hle_audio_rate;
    g->direct_hle_audio_accum = st->direct_hle_audio_accum;
    g->direct_vblank_wait_requested = 0;
}

static const uint8_t gp32_state_magic_v2[16] = { 'G','P','3','2','S','T','A','T','E','v','0','0','0','2',0,0 };
static const uint8_t gp32_state_magic_v3[16] = { 'G','P','3','2','S','T','A','T','E','v','0','0','0','3',0,0 };
static const uint8_t gp32_state_magic_v4[16] = { 'G','P','3','2','S','T','A','T','E','v','0','0','0','4',0,0 };
static const uint8_t gp32_state_magic_v5[16] = { 'G','P','3','2','S','T','A','T','E','v','0','0','0','5',0,0 };
static const uint8_t gp32_state_magic_v6[16] = { 'G','P','3','2','S','T','A','T','E','v','0','0','0','6',0,0 };
static const uint8_t gp32_state_magic_v7[16] = { 'G','P','3','2','S','T','A','T','E','v','0','0','0','7',0,0 };
static const uint8_t gp32_state_magic_v8[16] = { 'G','P','3','2','S','T','A','T','E','v','0','0','0','8',0,0 };
static const uint8_t gp32_state_magic_v9[16] = { 'G','P','3','2','S','T','A','T','E','v','0','0','0','9',0,0 };
static const uint8_t gp32_state_magic_v10[16] = { 'G','P','3','2','S','T','A','T','E','v','0','0','1','0',0,0 };
static const uint8_t gp32_state_magic_v11[16] = { 'G','P','3','2','S','T','A','T','E','v','0','0','1','1',0,0 };
static const uint8_t gp32_state_magic_v12[16] = { 'G','P','3','2','S','T','A','T','E','v','0','0','1','2',0,0 };
/* v13 lets the SmartMedia section carry only the pages that differ from the
 * image the frontend passed instead of the whole 17-34 MB card. The section is
 * self-describing, so every older magic keeps loading through the full-image
 * layout and an older build refuses this magic outright. */
static const uint8_t gp32_state_magic[16] = { 'G','P','3','2','S','T','A','T','E','v','0','0','1','4',0,0 };
static_assert(sizeof(gp32_frame_time_t) == 16u, "fixed frame-time wire extension");
static_assert(sizeof(gp32_elapsed_time_t) == 16u, "fixed elapsed-time wire extension");

/* v10 has 81 LE words; v11 appends four display-cadence words; v12 adds
 * the SDK callback task identity and its suspension flag.
 * No host pointers, padding or raw register-context layout.
 * RAM size/current clock bind these validations to the staged SoC body.
 * The suspended tick may still have a different fractional denominator. */
#define GP32_CONTINUATION_V10_WORDS 81u
#define GP32_CONTINUATION_V11_WORDS 85u
#define GP32_CONTINUATION_WORDS 87u
#define GP32_CONTINUATION_BYTES (GP32_CONTINUATION_WORDS * 4u)
typedef struct gp32_resume_image {
    uint32_t ram_size, run_clock;
    direct_callback_tail_t callback;
    direct_hle_tick_tail_t tick;
    uint32_t epoch[GP32_DIRECT_GPOS_TIMER_COUNT];
    gp32_frame_time_t vblank;
    uint64_t legacy_wait_cycles; /* load migration only, not wire */
    uint32_t legacy_wait_requested;
} gp32_resume_image_t;

static int gp32_resume_write(const gp32_t *g, state_io_t *io) {
    const direct_callback_tail_t *cb = &g->direct_callback;
    const direct_hle_tick_tail_t *tick = &g->direct_tick;
    const arm920t_register_context_t *fg = &cb->foreground;
    uint32_t w[GP32_CONTINUATION_WORDS];
    uint8_t bytes[GP32_CONTINUATION_BYTES];
    size_t n = 0;
#define PUT(v) w[n++] = (v)
    PUT((uint32_t)s3c2400_ram_size(g->soc)); PUT(direct_run_clock_hz(g));
    PUT(cb->owner); PUT(cb->fault_owner); PUT(cb->fault_reason); PUT(cb->fn);
    PUT(cb->ack_addr); PUT(cb->ack_value);
    PUT((uint32_t)cb->deadline_ns); PUT((uint32_t)(cb->deadline_ns >> 32));
    for (unsigned i = 0; i < 16u; ++i) PUT(fg->r[i]);
    PUT(fg->cpsr);
    for (unsigned i = 0; i < 7u; ++i) PUT(fg->bank_usr[i]);
    for (unsigned i = 0; i < 7u; ++i) PUT(fg->bank_fiq[i]);
    for (unsigned i = 0; i < 2u; ++i) PUT(fg->bank_svc[i]);
    for (unsigned i = 0; i < 2u; ++i) PUT(fg->bank_abt[i]);
    for (unsigned i = 0; i < 2u; ++i) PUT(fg->bank_irq[i]);
    for (unsigned i = 0; i < 2u; ++i) PUT(fg->bank_und[i]);
    PUT(fg->spsr_fiq); PUT(fg->spsr_svc); PUT(fg->spsr_abt); PUT(fg->spsr_irq); PUT(fg->spsr_und);
    PUT(tick->phase); PUT(tick->clock); PUT(tick->volume);
    PUT(tick->sdk_frames_left); PUT(tick->sdk_rate); PUT(tick->timer_slot); PUT(tick->calls_left);
    for (unsigned i = 0; i < GP32_DIRECT_GPOS_TIMER_COUNT; ++i) {
        PUT(tick->due[i].callback); PUT(tick->due[i].tps); PUT(tick->due[i].fires); PUT(tick->due[i].epoch);
    }
    for (unsigned i = 0; i < GP32_DIRECT_GPOS_TIMER_COUNT; ++i) PUT(g->direct_hle_gpos_timer_epoch[i]);
    PUT((uint32_t)g->direct_vblank_time.deadline_ns); PUT((uint32_t)(g->direct_vblank_time.deadline_ns >> 32));
    PUT(g->direct_vblank_time.remainder); PUT(g->direct_vblank_time.valid);
    PUT(cb->sdk_task); PUT(cb->suspended);
#undef PUT
    assert(n == GP32_CONTINUATION_WORDS);
    for (size_t i = 0; i < n; ++i) gp32_st32le(bytes + i * 4u, w[i]);
    return state_io_write(io, bytes, sizeof(bytes));
}

static int gp32_resume_read(state_io_t *io, gp32_resume_image_t *resume, int has_wait, int has_tasks) {
    direct_callback_tail_t *cb = &resume->callback;
    direct_hle_tick_tail_t *tick = &resume->tick;
    arm920t_register_context_t *fg = &cb->foreground;
    uint8_t bytes[GP32_CONTINUATION_BYTES];
    size_t words = has_tasks ? GP32_CONTINUATION_WORDS :
        has_wait ? GP32_CONTINUATION_V11_WORDS : GP32_CONTINUATION_V10_WORDS;
    if (!state_io_read(io, bytes, words * 4u)) return 0;
    size_t n = 0;
#define GET() gp32_ld32le(bytes + 4u * n++)
    resume->ram_size = GET(); resume->run_clock = GET();
    cb->owner = GET(); cb->fault_owner = GET(); cb->fault_reason = GET(); cb->fn = GET();
    cb->ack_addr = GET(); cb->ack_value = GET();
    uint32_t lo = GET(), hi = GET();
    cb->deadline_ns = ((uint64_t)hi << 32) | lo;
    for (unsigned i = 0; i < 16u; ++i) fg->r[i] = GET();
    fg->cpsr = GET();
    for (unsigned i = 0; i < 7u; ++i) fg->bank_usr[i] = GET();
    for (unsigned i = 0; i < 7u; ++i) fg->bank_fiq[i] = GET();
    for (unsigned i = 0; i < 2u; ++i) fg->bank_svc[i] = GET();
    for (unsigned i = 0; i < 2u; ++i) fg->bank_abt[i] = GET();
    for (unsigned i = 0; i < 2u; ++i) fg->bank_irq[i] = GET();
    for (unsigned i = 0; i < 2u; ++i) fg->bank_und[i] = GET();
    fg->spsr_fiq = GET(); fg->spsr_svc = GET(); fg->spsr_abt = GET(); fg->spsr_irq = GET(); fg->spsr_und = GET();
    tick->phase = GET(); tick->clock = GET(); tick->volume = GET();
    tick->sdk_frames_left = GET(); tick->sdk_rate = GET(); tick->timer_slot = GET(); tick->calls_left = GET();
    for (unsigned i = 0; i < GP32_DIRECT_GPOS_TIMER_COUNT; ++i) {
        tick->due[i].callback = GET(); tick->due[i].tps = GET(); tick->due[i].fires = GET(); tick->due[i].epoch = GET();
    }
    for (unsigned i = 0; i < GP32_DIRECT_GPOS_TIMER_COUNT; ++i) resume->epoch[i] = GET();
    if (has_wait) {
        lo = GET(); hi = GET();
        resume->vblank.deadline_ns = ((uint64_t)hi << 32) | lo;
        resume->vblank.remainder = GET(); resume->vblank.valid = GET();
    }
    if (has_tasks) { cb->sdk_task = GET(); cb->suspended = GET(); }
#undef GET
    assert(n == words);
    return 1;
}

static int gp32_resume_ram_range(uint32_t ram_size, uint32_t addr, uint32_t bytes) {
    return addr >= GP32_RAM_BASE && addr - GP32_RAM_BASE <= ram_size && bytes <= ram_size - (addr - GP32_RAM_BASE);
}
static int gp32_resume_mode(uint32_t cpsr) {
    switch (cpsr & 0x1fu) {
    case 0x10u: case 0x11u: case 0x12u: case 0x13u: case 0x17u: case 0x1bu: case 0x1fu: return 1;
    default: return 0;
    }
}
static int gp32_resume_validate(const gp32_resume_image_t *r, const gp32_state_image_t *direct,
                                const arm920t_state_image_t *cpu, const gp32_elapsed_time_t *elapsed) {
    const direct_callback_tail_t *cb = &r->callback;
    const direct_hle_tick_tail_t *tick = &r->tick;
    if (!r->ram_size || r->ram_size > 64u * 1024u * 1024u || !r->run_clock ||
        cb->owner > DIRECT_CB_FAULT || tick->phase > DIRECT_TICK_TIMER_CALLS ||
        cb->suspended > 1u || (cb->suspended && !cb->sdk_task) ||
        (cb->sdk_task && ((cb->sdk_task & 3u) || !gp32_resume_ram_range(r->ram_size, cb->sdk_task, 0x34u)))) return 0;
    if (r->vblank.valid > 1u || r->vblank.remainder >= 60u ||
        (!r->vblank.valid && (r->vblank.deadline_ns || r->vblank.remainder))) return 0;
    uint32_t clock = tick->clock ? tick->clock : r->run_clock;
    if (direct->direct_hle_pcm_accum >= clock || direct->direct_hle_sdk_accum >= clock) return 0;
    for (unsigned i = 0; i < GP32_DIRECT_GPOS_TIMER_COUNT; ++i)
        if (direct->direct_hle_gpos_timer[i].accum >= clock) return 0;
    if (cb->owner == DIRECT_CB_NONE) {
        static const direct_callback_tail_t no_callback = {0};
        static const direct_hle_tick_tail_t no_tick = {0};
        return !memcmp(cb, &no_callback, sizeof(*cb)) && !memcmp(tick, &no_tick, sizeof(*tick)) &&
            !direct->direct_hle_callback_running && !direct->direct_hle_callback_returned;
    }
    uint32_t owner = cb->owner == DIRECT_CB_FAULT ? cb->fault_owner : cb->owner;
    uint32_t mode = cpu->cpsr & 0x1fu;
    /* A normal mode switch can select an unused exception stack bank. */
    int empty_exception_stack = !cpu->r[13] &&
        (mode == 0x11u || mode == 0x12u || mode == 0x17u || mode == 0x1bu);
    if (!direct->direct_fxe_mode || !tick->clock || tick->timer_slot >= GP32_DIRECT_GPOS_TIMER_COUNT ||
        tick->calls_left > 8u || (tick->volume && ((tick->volume & ~0x13fu) || !(tick->volume & 0x100u))) ||
        !gp32_resume_ram_range(r->ram_size, cb->fn & ~1u, 4u) ||
        (!(cb->fn & 1u) && (cb->fn & 3u)) ||
        !gp32_resume_mode(cb->foreground.cpsr) || !gp32_resume_mode(cpu->cpsr) ||
        (cb->foreground.r[15] & ((cb->foreground.cpsr & ARM_T_FLAG) ? 1u : 3u)) ||
        (cpu->r[15] & ((cpu->cpsr & ARM_T_FLAG) ? 1u : 3u)) ||
        (!gp32_resume_ram_range(r->ram_size, cpu->r[13], 0u) && !empty_exception_stack) ||
        (!gp32_resume_ram_range(r->ram_size, cb->foreground.r[15], 4u) && cb->foreground.r[15] >= 0x80000u) ||
        direct->direct_hle_sdk_timer_accum >= 64u || direct->direct_hle_callback_returned) return 0;
    /* Bounds of the existing S3C2400 MPLL/divider calculation. A forged tiny
     * denominator must not admit billions of pending samples in the host tail. */
    if (tick->clock < 92307u || tick->clock > 1578000000u) return 0;
    uint32_t work = r->ram_size >= 0x800000u ? GP32_RAM_BASE + 0x7d0000u :
        r->ram_size >= 0x400000u ? GP32_RAM_BASE + r->ram_size - 0x30000u : GP32_RAM_BASE + 0x100u;
    if (!gp32_resume_ram_range(r->ram_size, work + 0x1c00u, 0x300u)) return 0;
    if (owner == DIRECT_CB_REFILL) {
        if (tick->phase != DIRECT_TICK_AFTER_REFILL || cb->ack_value > 1u ||
            !gp32_resume_ram_range(r->ram_size, cb->ack_addr, 4u)) return 0;
    } else if (owner == DIRECT_CB_TIMER) {
        const direct_timer_due_t *due = &tick->due[tick->timer_slot];
        if (tick->phase != DIRECT_TICK_TIMER_CALLS || cb->ack_addr || cb->ack_value ||
            tick->sdk_frames_left || due->callback != cb->fn || !due->fires || !due->tps || due->tps > 1000u) return 0;
    } else return 0;
    if (tick->sdk_rate && (tick->sdk_rate < 4000u || tick->sdk_rate > 192000u)) return 0;
    if (tick->sdk_frames_left && (!tick->sdk_rate ||
        tick->sdk_frames_left > (uint64_t)32768u * tick->sdk_rate / tick->clock + 1u)) return 0;
    if (cb->owner == DIRECT_CB_FAULT) {
        if (cb->fault_reason < DIRECT_CB_TIMEOUT || cb->fault_reason > DIRECT_CB_HOST_DISPLAY_WAIT ||
            direct->direct_hle_callback_running) return 0;
    } else if (cb->fault_owner || cb->fault_reason || !direct->direct_hle_callback_running ||
               !direct_time_pending(cb->deadline_ns, elapsed->nanoseconds) ||
               cb->deadline_ns - elapsed->nanoseconds > 1000000000u || (cpu->halted && !cb->suspended)) return 0;
    return 1;
}

static int gp32_state_write(const gp32_t *g, state_io_t *io) {
    gp32_state_image_t direct;
    gp32_direct_state_capture(g, &direct);
    return state_io_write(io, gp32_state_magic, sizeof(gp32_state_magic)) &&
           state_io_write(io, &direct, sizeof(direct)) &&
           state_io_write(io, &g->elapsed, sizeof(g->elapsed)) &&
           state_io_write(io, &g->frame_time, sizeof(g->frame_time)) &&
           gp32_resume_write(g, io) &&
           arm920t_state_save_io(g->cpu, io) &&
           s3c2400_state_save_io(g->soc, io);
}

static int gp32_state_read(gp32_t *g, state_io_t *io, gp32_state_image_t *direct, gp32_resume_image_t *resume) {
    uint8_t got[sizeof(gp32_state_magic)];
    if (!state_io_read(io, got, sizeof(got))) return 0;
    int legacy = memcmp(got, gp32_state_magic_v2, sizeof(got)) == 0;
    int has_tasks = memcmp(got, gp32_state_magic, sizeof(got)) == 0 ||
                    memcmp(got, gp32_state_magic_v12, sizeof(got)) == 0;
    int has_wait = has_tasks || memcmp(got, gp32_state_magic_v11, sizeof(got)) == 0;
    int has_resume = has_wait || memcmp(got, gp32_state_magic_v10, sizeof(got)) == 0;
    int has_codec = has_resume || memcmp(got, gp32_state_magic_v9, sizeof(got)) == 0;
    int has_idle_phase = has_codec || memcmp(got, gp32_state_magic_v8, sizeof(got)) == 0;
    int has_lcd_phase = has_idle_phase || memcmp(got, gp32_state_magic_v7, sizeof(got)) == 0;
    int has_iis_phase = has_lcd_phase || memcmp(got, gp32_state_magic_v6, sizeof(got)) == 0;
    int has_frame_time = has_iis_phase || memcmp(got, gp32_state_magic_v5, sizeof(got)) == 0;
    int has_spans = has_frame_time || memcmp(got, gp32_state_magic_v4, sizeof(got)) == 0;
    if ((!legacy && !has_spans && memcmp(got, gp32_state_magic_v3, sizeof(got)) != 0) ||
        !state_io_read(io, direct, sizeof(*direct))) return 0;
    memset(resume, 0, sizeof(*resume));
    if (!has_resume && direct->direct_hle_callback_running) return 0;
    gp32_elapsed_time_t elapsed = {0};
    if (!legacy) {
        if (!state_io_read(io, &elapsed, sizeof(elapsed))) return 0;
        if (elapsed.clock_hz ? elapsed.remainder >= elapsed.clock_hz :
            (elapsed.remainder != 0u || elapsed.nanoseconds != 0u)) return 0;
    }
    gp32_frame_time_t frame_time = {0};
    if (has_frame_time) {
        if (!state_io_read(io, &frame_time, sizeof(frame_time))) return 0;
        if (frame_time.valid > 1u || frame_time.remainder >= 60u ||
            (!frame_time.valid && (frame_time.deadline_ns || frame_time.remainder)) ||
            (direct_time_pending(frame_time.deadline_ns, elapsed.nanoseconds) &&
             frame_time.deadline_ns - elapsed.nanoseconds > 16666667u)) return 0;
    }
    /* Keep CPU state pending until the SoC has read every section. Its large
     * state image already uses most of a Windows thread's default stack, so
     * the CPU image must not remain on that stack during the SoC call. */
    arm920t_state_image_t *cpu = malloc(sizeof(*cpu));
    if (!cpu) return 0;
    int ok = (!has_resume || gp32_resume_read(io, resume, has_wait, has_tasks)) && state_io_read(io, cpu, sizeof(*cpu)) &&
        (!has_resume || gp32_resume_validate(resume, direct, cpu, &elapsed));
    uint32_t required_ram_size = 0u;
    if (ok && has_wait && (direct->direct_vblank_next_cycle || direct->direct_vblank_wait_cycles ||
                           direct->direct_vblank_wait_requested)) ok = 0;
    if (ok && !has_wait && direct->direct_fxe_mode) {
        resume->legacy_wait_cycles = direct->direct_vblank_wait_cycles;
        if (direct->direct_vblank_wait_requested && !resume->legacy_wait_cycles) {
            if (direct->direct_vblank_next_cycle > cpu->cycles_total)
                resume->legacy_wait_cycles = direct->direct_vblank_next_cycle - cpu->cycles_total;
            else resume->legacy_wait_requested = 1u; /* one restored-clock frame */
        }
        if (resume->legacy_wait_cycles || resume->legacy_wait_requested) {
            /* Incoming RAM must contain the full migration stack before
             * SoC commit. Do not validate it against this instance. */
            uint32_t sp = cpu->r[13];
            ok = !resume->callback.owner && gp32_resume_mode(cpu->cpsr) && !(sp & 3u) &&
                sp >= GP32_RAM_BASE + 32u && sp <= GP32_RAM_BASE + 64u * 1024u * 1024u &&
                resume->legacy_wait_cycles <= UINT64_MAX / 1000000000u;
            required_ram_size = sp - GP32_RAM_BASE;
            if (required_ram_size < 0x2000u) required_ram_size = 0x2000u;
        }
    }
    if (ok) ok = s3c2400_state_load_io_checked(g->soc, io, has_spans, has_iis_phase, has_lcd_phase, has_idle_phase, has_codec,
                                               resume->ram_size, resume->run_clock, required_ram_size,
                                               memcmp(got, gp32_state_magic, sizeof(got)) == 0 ? SMC_STATE_FORMAT_V14
                                                                                                : SMC_STATE_FORMAT_PRE_V14);
    if (ok) {
        if (legacy) {
            /* v2 has no clock history. Continue from its former observable
             * time, then accumulate subsequent slices at their own rates. */
            uint32_t clock = direct_run_clock_hz(g);
            uint64_t scaled = (cpu->cycles_total % clock) * 1000000000u;
            elapsed.nanoseconds = (cpu->cycles_total / clock) * 1000000000u + scaled / clock;
            elapsed.remainder = (uint32_t)(scaled % clock);
            elapsed.clock_hz = clock;
        }
        arm920t_state_apply(g->cpu, cpu);
        g->elapsed = elapsed;
        g->frame_time = frame_time;
    }
    free(cpu);
    return ok;
}

static void gp32_state_loaded(gp32_t *g, const gp32_state_image_t *direct, const gp32_resume_image_t *resume) {
    gp32_direct_state_apply(g, direct);
    direct_fast_load_reset(g);
    g->direct_callback = resume->callback;
    g->direct_tick = resume->tick;
    g->direct_vblank_time = resume->vblank;
    memcpy(g->direct_hle_gpos_timer_epoch, resume->epoch, sizeof(resume->epoch));
    g->direct_hle_callback_running = g->direct_callback.owner == DIRECT_CB_REFILL || g->direct_callback.owner == DIRECT_CB_TIMER;
    g->direct_hle_callback_returned = 0u;
    g->direct_cpu_running = 0;
    g->direct_hle_pending_volume = 0u;
    g->error[0] = '\0';
    if (g->direct_callback.owner == DIRECT_CB_FAULT) direct_callback_error(g);
    s3c2400_set_irq_sink(g->soc, g->cpu);
    if (g->direct_fxe_mode) {
        /* Also upgrade legacy stub bytes retained in saved RAM. */
        direct_install_stubs(g);
        uint64_t cycles = resume->legacy_wait_cycles;
        if (resume->legacy_wait_requested) {
            cycles = direct_run_clock_hz(g) / 60u;
            if (!cycles) cycles = 1u;
        }
        if (cycles) direct_migrate_vblank_wait(g, cycles);
        direct_update_fw_tick(g);
    }
    gp32_clear_audio(g);
    if (!g->direct_callback.owner) direct_fix_gp32_additive_blend_shadow_endpoint(g);
    direct_adpcm_fix_update(g, 0);  /* the state load emptied the JIT cache */
    direct_pinball_fix_update(g, 0);
}

size_t gp32_state_size(const gp32_t *g) {
    if (!g) return 0;
    state_io_t io = state_io_counter();
    return gp32_state_write(g, &io) ? io.pos : 0;
}

gp32_status_t gp32_save_state_data(gp32_t *g, void *data, size_t size) {
    if (!g || !data) return GP32_ERR_INVALID_ARGUMENT;
    size_t needed = gp32_state_size(g);
    if (!needed) { seterr(g, "count savestate failed"); return GP32_ERR_IO; }
    if (size < needed) {
        seterr(g, "savestate buffer too small: need %zu bytes, got %zu", needed, size);
        return GP32_ERR_INVALID_ARGUMENT;
    }
    state_io_t io = state_io_writer(data, size);
    uint32_t percent = gp32_cpu_speed_to_nominal(g);
    int ok = gp32_state_write(g, &io);
    gp32_cpu_speed_restore(g, percent);
    if (!ok) { seterr(g, "write savestate buffer failed"); return GP32_ERR_IO; }
    if (io.pos < size) memset((uint8_t *)data + io.pos, 0, size - io.pos);
    return GP32_OK;
}

static gp32_status_t gp32_state_load_data(gp32_t *g, const void *data, size_t size, size_t *consumed) {
    if (!g || !data || !size) return GP32_ERR_INVALID_ARGUMENT;
    state_io_t io = state_io_reader(data, size);
    gp32_state_image_t direct;
    gp32_resume_image_t resume;
    uint32_t percent = gp32_cpu_speed_to_nominal(g);
    if (!gp32_state_read(g, &io, &direct, &resume)) {
        gp32_cpu_speed_restore(g, percent);
        seterr(g, "load savestate buffer failed or unsupported version");
        return GP32_ERR_IO;
    }
    if (consumed) *consumed = io.pos;
    gp32_state_loaded(g, &direct, &resume);
    gp32_cpu_speed_restore(g, percent);
    return GP32_OK;
}

gp32_status_t gp32_load_state_data(gp32_t *g, const void *data, size_t size) {
    return gp32_state_load_data(g, data, size, NULL);
}

gp32_status_t gp32_load_state_data_ex(gp32_t *g, const void *data, size_t size, size_t *consumed) {
    return gp32_state_load_data(g, data, size, consumed);
}

gp32_status_t gp32_save_state(gp32_t *g, const char *path) {
    if (!g || !path) return GP32_ERR_INVALID_ARGUMENT;
    char err[192];
    save_atomic_t stage;
    if (!save_atomic_begin(&stage, path, err, sizeof(err))) {
        seterr(g, "%s", err);
        return GP32_ERR_IO;
    }
    state_io_t io = state_io_file(stage.file);
    uint32_t percent = gp32_cpu_speed_to_nominal(g);
    int ok = gp32_state_write(g, &io);
    gp32_cpu_speed_restore(g, percent);
    if (!ok) {
        save_atomic_abort(&stage);
        seterr(g, "write savestate %s failed", path);
        return GP32_ERR_IO;
    }
    if (!save_atomic_commit(&stage, path, err, sizeof(err))) {
        seterr(g, "%s", err);
        return GP32_ERR_IO;
    }
    return GP32_OK;
}

gp32_status_t gp32_load_state(gp32_t *g, const char *path) {
    if (!g || !path) return GP32_ERR_INVALID_ARGUMENT;
    FILE *f = fopen(path, "rb");
    if (!f) { seterr(g, "open savestate %s: %s", path, strerror(errno)); return GP32_ERR_IO; }
    state_io_t io = state_io_file(f);
    gp32_state_image_t direct;
    gp32_resume_image_t resume;
    uint32_t percent = gp32_cpu_speed_to_nominal(g);
    int ok = gp32_state_read(g, &io, &direct, &resume);
    /* Read-only stream: the complete payload is already committed on success.
     * A close error must not report rejection of an applied state. */
    (void)fclose(f);
    if (!ok) {
        gp32_cpu_speed_restore(g, percent);
        seterr(g, "load savestate %s failed or unsupported version", path);
        return GP32_ERR_IO;
    }
    gp32_state_loaded(g, &direct, &resume);
    gp32_cpu_speed_restore(g, percent);
    return GP32_OK;
}
