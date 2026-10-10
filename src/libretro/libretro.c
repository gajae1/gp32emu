#include "libretro.h"
#include "gp32emu/gp32.h"
#include "gp32emu/video_effects.h"
#include "audio/gp32_audio_resampler.h"
#include "smc_direct.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif

#define GP32_W 320u
#define GP32_H 240u
#define GP32_RAW_W 240u
#define GP32_RAW_H 320u
/* Host callback cadence, matching gp32_run_frame's 1/60-second interval. */
#define GP32_FPS 60.0
#define GP32_AUDIO_RATE 44100u
#define GP32_AUDIO_PUMP_FRAMES (GP32_AUDIO_RATE / 500u)
#define GP32_AUDIO_FRAMES_PER_VIDEO 735u
#define GP32_AUDIO_QUEUE_LIMIT (GP32_AUDIO_RATE / 4u)
#define GP32_AUDIO_GAP_FRAMES (GP32_AUDIO_RATE / 1000u)

/* The bundled libretro.h only declares the environment commands this core
 * uses. GET_CAN_DUPE is standard ABI value 3: the frontend sets the boolean to
 * true when it accepts a NULL video frame as "repeat the previous frame". */
#ifndef RETRO_ENVIRONMENT_GET_CAN_DUPE
#define RETRO_ENVIRONMENT_GET_CAN_DUPE 3
#endif

static retro_environment_t environ_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_t audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static retro_log_printf_t log_cb;

static gp32_t *emu;
static uint32_t frame_rgb[GP32_W * GP32_H];
static uint32_t effect_rgb[GP32_W * GP32_H];
static gp32_video_effects_t effects;
static gp32_audio_resampler_t audio_resampler;
static int16_t *audio_resample_buf;
static size_t audio_resample_cap;
static size_t audio_pending_frames;
static int16_t audio_last_sent[2], audio_gap_from[2];
static unsigned audio_gap_left;
/* Set once this frame's own PCM has been released to the frontend, whether by
 * the mid-frame pump or by the frame-end drain. The idle fill at the frame
 * boundary then stays out of a frame that produced sound. */
static int audio_guest_pcm_this_frame;

static uint64_t last_video_frame = UINT64_MAX;
static const uint32_t *last_video_ptr;
static int have_last_video;
static int can_dupe;
static int input_bitmasks;
static unsigned input_device = RETRO_DEVICE_JOYPAD;
static int effects_ready;
static char system_dir[4096];
static char save_dir[4096];
static char content_dir[4096];
static char content_path[4096];
static char smartmedia_save_path[4096];
static char smartmedia_legacy_path[4096];
static int smartmedia_legacy_loaded;
static int smartmedia_save_error;
static size_t state_capacity;
static int use_jit;
static int use_lcd_persistence;
static int use_frame_interpolation;
static unsigned cpu_speed_percent = 100u;
static int use_game_fixes = 1;
static int use_fast_loading = 1;
static int boot_mode; /* 0=auto BIOS if available, 1=require BIOS, 2=direct/HLE */

static const char *path_basename(const char *p) {
    if (!p) return "gp32";
    const char *a = strrchr(p, '/');
#ifdef _WIN32
    const char *b = strrchr(p, '\\');
    if (!a || (b && b > a)) a = b;
#endif
    return a ? a + 1 : p;
}

static void strip_ext(char *s) {
    char *slash = strrchr(s, '/');
#ifdef _WIN32
    char *bslash = strrchr(s, '\\');
    if (!slash || (bslash && bslash > slash)) slash = bslash;
#endif
    char *dot = strrchr(slash ? slash + 1 : s, '.');
    if (dot) *dot = 0;
}

static int file_exists(const char *path) {
    if (!path || !path[0]) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static void path_dirname(char *out, size_t outsz, const char *path) {
    if (!out || !outsz) return;
    out[0] = 0;
    if (!path || !path[0]) return;
    snprintf(out, outsz, "%s", path);
    char *slash = strrchr(out, '/');
#ifdef _WIN32
    char *bslash = strrchr(out, '\\');
    if (!slash || (bslash && bslash > slash)) slash = bslash;
#endif
    if (slash) *slash = 0;
    else snprintf(out, outsz, ".");
}

static void join_path(char *out, size_t outsz, const char *dir, const char *name) {
    if (!out || !outsz) return;
    if (!dir || !dir[0]) dir = ".";
    size_t n = strlen(dir);
    snprintf(out, outsz, "%s%s%s", dir, (n && dir[n - 1] == '/') ? "" : "/", name ? name : "");
}

static int has_ext(const char *path, const char *ext) {
    const char *dot = strrchr(path ? path : "", '.');
    if (!dot) return 0;
    ++dot;
    while (*dot && *ext) {
        char a = *dot++, b = *ext++;
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return 0;
    }
    return !*dot && !*ext;
}

static void lr_vlog(int level, const char *fmt, va_list ap) {
    if (log_cb) {
        char buf[2048];
        vsnprintf(buf, sizeof(buf), fmt, ap);
        log_cb(level, "%s", buf);
    } else {
        FILE *f = (level >= RETRO_LOG_WARN) ? stderr : stdout;
        vfprintf(f, fmt, ap);
    }
}

static void lr_log(int level, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    lr_vlog(level, fmt, ap);
    va_end(ap);
}

static void lr_message(const char *msg) {
    if (!environ_cb || !msg) return;
    struct retro_message m;
    m.msg = msg;
    m.frames = 240;
    environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &m);
}

static void gp32_log(void *user, const char *msg) {
    (void)user;
    (void)msg;
}

static void refresh_variables(void) {
    if (!environ_cb) return;
    struct retro_variable var;
    memset(&var, 0, sizeof(var));
    var.key = "gp32emu_jit";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) use_jit = strcmp(var.value, "disabled") != 0;
    memset(&var, 0, sizeof(var));
    var.key = "gp32emu_lcd_persistence";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) use_lcd_persistence = strcmp(var.value, "enabled") == 0;
    memset(&var, 0, sizeof(var));
    var.key = "gp32emu_frame_interpolation";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) use_frame_interpolation = strcmp(var.value, "enabled") == 0;
    memset(&var, 0, sizeof(var));
    var.key = "gp32emu_cpu_speed";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        unsigned long value = strtoul(var.value, NULL, 10);
        cpu_speed_percent = value >= 50u && value <= 400u ? (unsigned)value : 100u;
    }
    memset(&var, 0, sizeof(var));
    var.key = "gp32emu_game_fixes";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) use_game_fixes = strcmp(var.value, "disabled") != 0;
    memset(&var, 0, sizeof(var));
    var.key = "gp32emu_fast_loading";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) use_fast_loading = strcmp(var.value, "disabled") != 0;
    memset(&var, 0, sizeof(var));
    var.key = "gp32emu_boot_mode";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        if (!strcmp(var.value, "require_bios")) boot_mode = 1;
        else if (!strcmp(var.value, "direct_hle")) boot_mode = 2;
        else boot_mode = 0;
    } else {
        /* Compatibility with v100's option key. */
        memset(&var, 0, sizeof(var));
        var.key = "gp32emu_direct_boot";
        if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value && !strcmp(var.value, "enabled")) boot_mode = 2;
    }
    if (emu) gp32_set_jit(emu, use_jit);
    if (emu) gp32_set_cpu_speed_percent(emu, cpu_speed_percent);
    if (emu) gp32_set_game_fixes(emu, use_game_fixes);
    if (emu) gp32_set_fast_loading(emu, use_fast_loading);
    if (effects_ready) gp32_video_effects_set(&effects, use_lcd_persistence, use_frame_interpolation);
}


static void set_default_dirs(void) {
    const char *p = NULL;
    system_dir[0] = save_dir[0] = content_dir[0] = 0;
    if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &p) && p) snprintf(system_dir, sizeof(system_dir), "%s", p);
    p = NULL;
    if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &p) && p) snprintf(save_dir, sizeof(save_dir), "%s", p);
    p = NULL;
    if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_CONTENT_DIRECTORY, &p) && p) snprintf(content_dir, sizeof(content_dir), "%s", p);
    if (!system_dir[0]) snprintf(system_dir, sizeof(system_dir), ".");
    if (!save_dir[0]) snprintf(save_dir, sizeof(save_dir), "%s", system_dir);
}


static void make_runtime_paths(const char *game_path) {
    char stem[1024];
    snprintf(stem, sizeof(stem), "%s", path_basename(game_path));
    strip_ext(stem);
    char smc_name[1200];
    /* RetroArch-convention name: <save dir>/<rom basename>.gp32.sav.
     * Known limitation: ROMs sharing a basename in different folders share
     * one save file. */
    snprintf(smc_name, sizeof(smc_name), "%s.gp32.sav", stem);
    join_path(smartmedia_save_path, sizeof(smartmedia_save_path), save_dir, smc_name);
    snprintf(smc_name, sizeof(smc_name), "%s.gp32.smc", stem);
    join_path(smartmedia_legacy_path, sizeof(smartmedia_legacy_path), save_dir, smc_name);
    smartmedia_legacy_loaded = 0;
    smartmedia_save_error = 0;
}

/* Flush the persisted SmartMedia image before releasing the emulator. Used by
 * every teardown path so a game replaced mid-session keeps its NAND writes. */
static void destroy_emu_with_save(void) {
    state_capacity = 0;
    if (!emu) return;
    if (smartmedia_save_path[0] && gp32_save_card_progress(emu, smartmedia_save_path) != GP32_OK) {
        lr_log(RETRO_LOG_ERROR, "[gp32emu] Cannot save SmartMedia to %s: %s\n",
               smartmedia_save_path, gp32_get_error(emu));
        lr_message("GP32 save failed. Check free space and write access; latest progress was not saved.");
    } else if (smartmedia_save_path[0] && smartmedia_legacy_loaded && remove(smartmedia_legacy_path) != 0) {
        lr_log(RETRO_LOG_WARN, "[gp32emu] Saved progress; could not remove unused old card %s\n", smartmedia_legacy_path);
    }
    gp32_destroy(emu);
    emu = NULL;
    smartmedia_legacy_loaded = 0;
}

/* Which loader starts SmartMedia content.
 *
 * BIOS keeps the retail firmware in charge. DIRECT extracts the card's
 * executable and mounts no card device. DIRECT_MEDIA extracts the same
 * executable but mounts the card first, because a freeware card reads its own
 * assets through the card and only renders when it is in the slot - the
 * configuration gp32_create's BIOSless direct boot has always used. */
typedef enum content_boot {
    CONTENT_BOOT_BIOS = 0,
    CONTENT_BOOT_DIRECT = 1,
    CONTENT_BOOT_DIRECT_MEDIA = 2
} content_boot_t;

/* Load changes only after mounting the exact immutable original. A present but
 * invalid or inaccessible save aborts loading instead of losing progress. */
static int mount_saved_smartmedia(gp32_t *g) {
    const char *path = smartmedia_save_path;
    if (!path[0]) return 0;
    FILE *f = fopen(path, "rb");
    if (!f && errno == ENOENT) {
        path = smartmedia_legacy_path;
        f = fopen(path, "rb");
        if (!f && errno == ENOENT) return 0;
    }
    if (!f) {
        smartmedia_save_error = 1;
        lr_message("GP32emu: cannot read the card save; check access permissions");
        return -1;
    }
    fclose(f);
    if (gp32_load_card_progress(g, path) != GP32_OK) {
        smartmedia_save_error = 1;
        lr_log(RETRO_LOG_ERROR, "[gp32emu] Cannot restore %s: %s\n", path, gp32_get_error(g));
        lr_message("GP32emu: damaged save or original card mismatch; load aborted to protect progress");
        return -1;
    }
    smartmedia_legacy_loaded = path == smartmedia_legacy_path;
    return 1;
}

static int load_smartmedia_content(gp32_t *g, const void *data, size_t size, const char *path, const char *label, content_boot_t boot) {
    if (boot == CONTENT_BOOT_DIRECT) {
        /* This boot path extracts a program without mounting a writable card. */
        smartmedia_save_path[0] = 0;
        if (data && size) return gp32_load_smartmedia_direct_data(g, data, size, label) == GP32_OK;
        return path && path[0] && gp32_load_smartmedia_direct(g, path) == GP32_OK;
    }
    gp32_status_t st = data && size ? gp32_load_smartmedia_data(g, data, size)
                                  : gp32_load_smartmedia(g, path);
    if (st != GP32_OK) return 0;
    if (mount_saved_smartmedia(g) < 0) return -1;
    if (boot == CONTENT_BOOT_DIRECT_MEDIA)
        return gp32_boot_mounted_smartmedia(g, label) == GP32_OK;
    return 1;
}

static int try_bios_path(char *out, size_t outsz, const char *dir, const char *name) {
    char p[4096];
    if (!dir || !dir[0] || !name || !name[0]) return 0;
    join_path(p, sizeof(p), dir, name);
    if (!file_exists(p)) return 0;
    snprintf(out, outsz, "%s", p);
    return 1;
}

static int find_bios_path(char *out, size_t outsz, const char *game_path) {
    static const char *names[] = {
        "gp32166m.bin",
        "gp32166.bin",
        "gp32.bin",
        "GP32.BIN",
        "bios.bin",
        "[BIOS] GamePark GP32 (Europe) (v1.6.6).bin",
        "[BIOS] GamePark GP32 (Korea) (v1.5.6).bin",
        "[BIOS] GamePark GP32 (Europe) (v1.5.7).bin",
        NULL
    };
    char game_dir[4096];
    path_dirname(game_dir, sizeof(game_dir), game_path);
    const char *dirs[5];
    dirs[0] = system_dir;
    dirs[1] = content_dir;
    dirs[2] = game_dir;
    dirs[3] = ".";
    dirs[4] = NULL;
    if (out && outsz) out[0] = 0;
    for (unsigned d = 0; dirs[d]; ++d) {
        for (unsigned n = 0; names[n]; ++n) {
            if (try_bios_path(out, outsz, dirs[d], names[n])) return 1;
        }
    }
    return 0;
}

void retro_set_environment(retro_environment_t cb) {
    environ_cb = cb;
    log_cb = NULL;
    if (environ_cb) {
        struct retro_log_callback logging;
        memset(&logging, 0, sizeof(logging));
        if (environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging) && logging.log) log_cb = logging.log;
    }
    /* Default to refusing NULL frames unless the frontend explicitly allows
     * them, so an older frontend never gets an unnegotiated duplicate. */
    bool dupe = false;
    can_dupe = (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_CAN_DUPE, &dupe) && dupe) ? 1 : 0;
    bool no_game = false;
    if (environ_cb) environ_cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);
    static const struct retro_variable vars[] = {
        { "gp32emu_jit", "Dynamic recompiler; enabled|disabled" },
        { "gp32emu_boot_mode", "Boot mode; auto|require_bios|direct_hle" },
        { "gp32emu_lcd_persistence", "LCD persistence / GP32 FLU ghosting; disabled|enabled" },
        { "gp32emu_frame_interpolation", "Frame interpolation; disabled|enabled" },
        { "gp32emu_cpu_speed", "CPU speed (guest overclock, may affect compatibility); 100%|125%|150%|175%|200%|250%|300%" },
        { "gp32emu_game_fixes", "Fix game code bugs (ASR audio clicks / Pinball startup mute); enabled|disabled" },
        { "gp32emu_fast_loading", "Fast loading (shortens silent card-loading pauses); enabled|disabled" },
        { NULL, NULL }
    };
    if (environ_cb) environ_cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void*)vars);
    unsigned fmt = RETRO_PIXEL_FORMAT_XRGB8888;
    if (environ_cb) environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt);
    static const struct retro_input_descriptor desc[] = {
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "GP32 A"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "GP32 B"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L, "GP32 L"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, "GP32 R"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "GP32 Start"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "GP32 Select"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "GP32 Up"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "GP32 Down"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "GP32 Left"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "GP32 Right"},
        {0, 0, 0, 0, NULL}
    };
    if (environ_cb) environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, (void*)desc);
}


void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { audio_cb = cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }

unsigned retro_api_version(void) { return RETRO_API_VERSION; }
void retro_get_system_info(struct retro_system_info *info) {
    memset(info, 0, sizeof(*info));
    info->library_name = "gp32emu";
    info->library_version = "1.0.0";
    info->valid_extensions = "smc|fxe|fpk";
    info->need_fullpath = true;
    info->block_extract = false;
}
void retro_get_system_av_info(struct retro_system_av_info *info) {
    memset(info, 0, sizeof(*info));
    info->geometry.base_width = GP32_W;
    info->geometry.base_height = GP32_H;
    info->geometry.max_width = GP32_W;
    info->geometry.max_height = GP32_H;
    info->geometry.aspect_ratio = 4.0f / 3.0f;
    /* This is retro_run's time step, not the independent guest LCD clock.
     * gp32_run_frame advances 1/60 second; advertising the panel rate would
     * change emulation speed and make audio production drift against the
     * frontend's 44100 Hz consumer. Scanout still follows the guest divider. */
    info->timing.fps = GP32_FPS;
    info->timing.sample_rate = GP32_AUDIO_RATE;
}

/* Fractional idle duration, in 1/(60 * 2^24) source-frame units. The
 * extra precision preserves elapsed time when the guest changes sample rate;
 * the bounded remainder times any uint32_t rate still fits in uint64_t. */
static uint64_t audio_idle_fraction;
static uint32_t audio_idle_rate;

/* Delivery state that the guest savestate cannot describe. Run-ahead and
 * rewind replay the same guest frames from a state, so everything this file
 * carries *between* runs has to be restored as well, or the replayed output
 * differs from the uninterrupted run: the duplicate/LCD-identity decision
 * (last_video_frame/have_last_video), the output queue and its resampler,
 * gap ramps anchored on already delivered samples, the idle-silence
 * time budget, and the effect histories when a filter is enabled.
 *
 * It travels in a versioned section appended after the guest payload. A state
 * without it still loads (previous builds, path/guest-only savestates) and
 * keeps the old reset behavior; the guest loader ignores trailing bytes. */
#define LRST_MAGIC "LRST"
#define LRST_VERSION 2u
#define LRST_FLAG_PREV_RAW 1u
#define LRST_FLAG_PREV_LCD 2u

enum { LRST_VIDEO_NONE = 0, LRST_VIDEO_FRAME = 1, LRST_VIDEO_EFFECT = 2 };

/* Fixed-width mirror of gp32_audio_resampler_t so the section layout does not
 * depend on the in-memory struct.  The anti-imaging coefficient table is
 * deliberately absent: it is rebuilt from the two rates when the key does not
 * match, which keeps every rewind or run-ahead state ~33 KB smaller than the
 * engine struct. */
typedef struct lrst_resampler {
    uint32_t src_rate, dst_rate;
    uint64_t phase_q32;
    int16_t prev_l, prev_r;
    int32_t have_prev;
    int16_t last_out_l, last_out_r;
    int32_t have_last_out;
    uint32_t fade_left, fade_total;
    uint32_t poly_src_rate, poly_dst_rate;
    int16_t poly_hist_l[GP32_AUDIO_POLY_HISTORY];
    int16_t poly_hist_r[GP32_AUDIO_POLY_HISTORY];
} lrst_resampler_t;

typedef struct lrst_body {
    uint64_t last_video_frame;
    int32_t last_video_source;
    int32_t have_last_video;
    int16_t audio_last_sent[2];
    /* Legacy slope-filter fields retain the v2 wire layout. Written as zero
     * and ignored on load: steep musical waveforms are not output gaps. */
    int16_t audio_raw_tail[2];
    int32_t audio_have_raw_tail;
    int16_t audio_gap_from[2];
    uint32_t audio_gap_left;
    int32_t audio_declick_anchor[2];
    uint32_t audio_declick_left[2];
    uint64_t audio_idle_fraction;
    uint32_t audio_idle_rate;
    uint32_t reserved;
    lrst_resampler_t resampler;
} lrst_body_t;

typedef struct lrst_header {
    char magic[4];
    uint32_t version;
    uint32_t total_size;      /* header + body + trailing sample/pixel arrays */
    uint32_t body_size;       /* sizeof(lrst_body_t) */
    uint32_t pending_frames;  /* stereo frames after the body */
    uint32_t flags;           /* LRST_FLAG_* pixel histories after those */
    uint32_t reserved;
} lrst_header_t;

static size_t lrst_size(void) {
    size_t size = sizeof(lrst_header_t) + sizeof(lrst_body_t);
    if (audio_pending_frames > GP32_AUDIO_QUEUE_LIMIT) return 0;
    size += audio_pending_frames * 2u * sizeof(int16_t);
    if (effects.have_prev_raw) size += (size_t)GP32_VIDEO_EFFECTS_PIXELS * sizeof(uint32_t);
    if (effects.have_prev_lcd) size += (size_t)GP32_VIDEO_EFFECTS_PIXELS * sizeof(uint32_t);
    return size;
}

/* Largest section lrst_write can produce. Frontends size run-ahead and rewind
 * buffers from one retro_serialize_size call, so report the bound instead of
 * the current queue depth. */
static size_t lrst_capacity(void) {
    return sizeof(lrst_header_t) + sizeof(lrst_body_t) +
           (size_t)GP32_AUDIO_QUEUE_LIMIT * 2u * sizeof(int16_t) +
           2u * (size_t)GP32_VIDEO_EFFECTS_PIXELS * sizeof(uint32_t);
}

/* Appends the delivery section at dst. Returns the section length, or 0 when
 * it does not fit the remaining room. */
static size_t lrst_write(uint8_t *dst, size_t room) {
    size_t size = lrst_size();
    if (!dst || !size || room < size) return 0;
    lrst_header_t head;
    lrst_body_t body;
    memset(&head, 0, sizeof(head));
    memset(&body, 0, sizeof(body));
    memcpy(head.magic, LRST_MAGIC, sizeof(head.magic));
    head.version = LRST_VERSION;
    head.total_size = (uint32_t)size;
    head.body_size = (uint32_t)sizeof(body);
    head.pending_frames = (uint32_t)audio_pending_frames;
    body.last_video_frame = last_video_frame;
    body.have_last_video = have_last_video;
    body.last_video_source = have_last_video
        ? (last_video_ptr == effect_rgb ? LRST_VIDEO_EFFECT
           : last_video_ptr == frame_rgb ? LRST_VIDEO_FRAME : LRST_VIDEO_NONE)
        : LRST_VIDEO_NONE;
    memcpy(body.audio_last_sent, audio_last_sent, sizeof(body.audio_last_sent));
    memcpy(body.audio_gap_from, audio_gap_from, sizeof(body.audio_gap_from));
    body.audio_gap_left = audio_gap_left;
    body.audio_idle_fraction = audio_idle_fraction;
    body.audio_idle_rate = audio_idle_rate;
    body.resampler.src_rate = audio_resampler.src_rate;
    body.resampler.dst_rate = audio_resampler.dst_rate;
    body.resampler.phase_q32 = audio_resampler.phase_q32;
    body.resampler.prev_l = audio_resampler.prev_l;
    body.resampler.prev_r = audio_resampler.prev_r;
    body.resampler.have_prev = audio_resampler.have_prev;
    body.resampler.last_out_l = audio_resampler.last_out_l;
    body.resampler.last_out_r = audio_resampler.last_out_r;
    body.resampler.have_last_out = audio_resampler.have_last_out;
    body.resampler.fade_left = audio_resampler.fade_left;
    body.resampler.fade_total = audio_resampler.fade_total;
    body.resampler.poly_src_rate = audio_resampler.poly_src_rate;
    body.resampler.poly_dst_rate = audio_resampler.poly_dst_rate;
    memcpy(body.resampler.poly_hist_l, audio_resampler.poly_hist_l,
           sizeof(body.resampler.poly_hist_l));
    memcpy(body.resampler.poly_hist_r, audio_resampler.poly_hist_r,
           sizeof(body.resampler.poly_hist_r));
    uint8_t *p = dst;
    memcpy(p, &head, sizeof(head));
    p += sizeof(head);
    memcpy(p, &body, sizeof(body));
    p += sizeof(body);
    if (audio_pending_frames) {
        memcpy(p, audio_resample_buf, audio_pending_frames * 2u * sizeof(int16_t));
        p += audio_pending_frames * 2u * sizeof(int16_t);
    }
    if (effects.have_prev_raw) {
        head.flags |= LRST_FLAG_PREV_RAW;
        memcpy(p, effects.prev_raw, (size_t)GP32_VIDEO_EFFECTS_PIXELS * sizeof(uint32_t));
        p += (size_t)GP32_VIDEO_EFFECTS_PIXELS * sizeof(uint32_t);
    }
    if (effects.have_prev_lcd) {
        head.flags |= LRST_FLAG_PREV_LCD;
        memcpy(p, effects.prev_lcd, (size_t)GP32_VIDEO_EFFECTS_PIXELS * sizeof(uint32_t));
        p += (size_t)GP32_VIDEO_EFFECTS_PIXELS * sizeof(uint32_t);
    }
    /* flags are part of the header; rewrite it now that they are known. */
    memcpy(dst, &head, sizeof(head));
    return size;
}

/* Restores a section written by lrst_write. Returns 1 when the section was
 * present and applied, 0 when the state predates it (or carries an unknown
 * version), and -1 when a section of this version is present but truncated or
 * malformed. */
static int lrst_read(const uint8_t *src, size_t room) {
    lrst_header_t head;
    lrst_body_t body;
    if (!src || room < sizeof(head)) return 0;
    memcpy(&head, src, sizeof(head));
    if (memcmp(head.magic, LRST_MAGIC, sizeof(head.magic)) != 0) return 0;
    if (head.version != LRST_VERSION) {
        lr_log(RETRO_LOG_WARN, "[gp32emu] savestate carries delivery section v%u; ignoring it.\n", head.version);
        return 0;
    }
    if (head.body_size != sizeof(body) || head.pending_frames > GP32_AUDIO_QUEUE_LIMIT) return -1;
    uint64_t want = (uint64_t)sizeof(head) + sizeof(body) +
                    (uint64_t)head.pending_frames * 2u * sizeof(int16_t);
    if (head.flags & ~(uint32_t)(LRST_FLAG_PREV_RAW | LRST_FLAG_PREV_LCD)) return -1;
    if (head.flags & LRST_FLAG_PREV_RAW) want += (uint64_t)GP32_VIDEO_EFFECTS_PIXELS * sizeof(uint32_t);
    if (head.flags & LRST_FLAG_PREV_LCD) want += (uint64_t)GP32_VIDEO_EFFECTS_PIXELS * sizeof(uint32_t);
    if (want != head.total_size || (uint64_t)room < want) return -1;
    const uint8_t *p = src + sizeof(head);
    memcpy(&body, p, sizeof(body));
    p += sizeof(body);
    if (body.last_video_source != LRST_VIDEO_NONE && body.last_video_source != LRST_VIDEO_FRAME &&
        body.last_video_source != LRST_VIDEO_EFFECT) return -1;
    if (body.audio_have_raw_tail != 0 && body.audio_have_raw_tail != 1) return -1;
    if (head.pending_frames) {
        if (audio_resample_cap < head.pending_frames) {
            int16_t *grown = (int16_t *)realloc(audio_resample_buf,
                                                (size_t)head.pending_frames * 2u * sizeof(int16_t));
            if (!grown) {
                lr_log(RETRO_LOG_ERROR, "[gp32emu] savestate audio queue restore failed.\n");
                return -1;
            }
            audio_resample_buf = grown;
            audio_resample_cap = head.pending_frames;
        }
        memcpy(audio_resample_buf, p, (size_t)head.pending_frames * 2u * sizeof(int16_t));
        p += (size_t)head.pending_frames * 2u * sizeof(int16_t);
    }
    audio_pending_frames = head.pending_frames;
    last_video_frame = body.last_video_frame;
    have_last_video = body.have_last_video ? 1 : 0;
    last_video_ptr = have_last_video
        ? (body.last_video_source == LRST_VIDEO_EFFECT ? effect_rgb : frame_rgb)
        : NULL;
    memcpy(audio_last_sent, body.audio_last_sent, sizeof(audio_last_sent));
    memcpy(audio_gap_from, body.audio_gap_from, sizeof(audio_gap_from));
    audio_gap_left = body.audio_gap_left;
    audio_idle_fraction = body.audio_idle_fraction;
    audio_idle_rate = body.audio_idle_rate;
    /* Keep the coefficient table when it already names the restored rates;
     * invalidate the key otherwise so the next resample rebuilds it. */
    uint32_t table_src = audio_resampler.poly_src_rate;
    uint32_t table_dst = audio_resampler.poly_dst_rate;
    int table_kept = table_src == body.resampler.poly_src_rate &&
                     table_dst == body.resampler.poly_dst_rate;
    audio_resampler.src_rate = body.resampler.src_rate;
    audio_resampler.dst_rate = body.resampler.dst_rate;
    audio_resampler.phase_q32 = body.resampler.phase_q32;
    audio_resampler.prev_l = body.resampler.prev_l;
    audio_resampler.prev_r = body.resampler.prev_r;
    audio_resampler.have_prev = body.resampler.have_prev;
    audio_resampler.last_out_l = body.resampler.last_out_l;
    audio_resampler.last_out_r = body.resampler.last_out_r;
    audio_resampler.have_last_out = body.resampler.have_last_out;
    audio_resampler.fade_left = body.resampler.fade_left;
    audio_resampler.fade_total = body.resampler.fade_total;
    audio_resampler.poly_src_rate = table_kept ? table_src : 0u;
    audio_resampler.poly_dst_rate = table_kept ? table_dst : 0u;
    memcpy(audio_resampler.poly_hist_l, body.resampler.poly_hist_l,
           sizeof(audio_resampler.poly_hist_l));
    memcpy(audio_resampler.poly_hist_r, body.resampler.poly_hist_r,
           sizeof(audio_resampler.poly_hist_r));
    effects.have_prev_raw = 0;
    effects.have_prev_lcd = 0;
    if (head.flags & LRST_FLAG_PREV_RAW) {
        if (effects.prev_raw) {
            memcpy(effects.prev_raw, p, (size_t)GP32_VIDEO_EFFECTS_PIXELS * sizeof(uint32_t));
            effects.have_prev_raw = 1;
        }
        p += (size_t)GP32_VIDEO_EFFECTS_PIXELS * sizeof(uint32_t);
    }
    if (head.flags & LRST_FLAG_PREV_LCD) {
        if (effects.prev_lcd) {
            memcpy(effects.prev_lcd, p, (size_t)GP32_VIDEO_EFFECTS_PIXELS * sizeof(uint32_t));
            effects.have_prev_lcd = 1;
        }
    }
    return 1;
}

static void reset_audio(void) {
    audio_pending_frames = 0;
    audio_idle_fraction = 0;
    audio_idle_rate = 0;
    memset(audio_last_sent, 0, sizeof(audio_last_sent));
    memset(audio_gap_from, 0, sizeof(audio_gap_from));
    audio_gap_left = 0;
    gp32_audio_resampler_init(&audio_resampler);
}

/* Frame-identity state only describes the game session that produced it. Any
 * load, reset or state restore can replay a frame counter whose pixels differ
 * from what this run already presented, so the duplicate decision is voided. */
static void invalidate_last_video(void) {
    last_video_frame = UINT64_MAX;
    last_video_ptr = NULL;
    have_last_video = 0;
}

/* Historical post-load delivery reset, kept for savestates that carry no
 * delivery section (or an unreadable one) so old files behave as before. */
static void reset_loaded_delivery_state(void) {
    gp32_video_effects_reset(&effects);
    reset_audio();
    invalidate_last_video();
}

static void refresh_input_bitmask_support(void);

void retro_init(void) {
    input_device = RETRO_DEVICE_JOYPAD;
    set_default_dirs();
    reset_audio();
    if (!effects_ready) effects_ready = gp32_video_effects_init(&effects);
    refresh_variables();
    refresh_input_bitmask_support();
}
void retro_deinit(void) {
    destroy_emu_with_save();
    invalidate_last_video();
    if (effects_ready) { gp32_video_effects_shutdown(&effects); effects_ready = 0; }
    free(audio_resample_buf);
    audio_resample_buf = NULL;
    audio_resample_cap = 0;
    reset_audio();
}
void retro_reset(void) { if (emu) { gp32_reset(emu); gp32_video_effects_reset(&effects); reset_audio(); invalidate_last_video(); } }
void retro_set_controller_port_device(unsigned port, unsigned device) {
    if (port == 0) input_device = device & RETRO_DEVICE_MASK;
}

/* Optional single-query joypad polling. A frontend that answers
 * RETRO_ENVIRONMENT_GET_INPUT_BITMASKS can return every RetroPad button from
 * one input_state call; frontends that do not know the command, or that
 * explicitly report no support, keep the original per-button queries.
 * The boolean is pre-set so a frontend that relies on its return value alone
 * still enables the fast path, and it stays false unless the call succeeds. */
static void refresh_input_bitmask_support(void) {
    bool supported = true;
    if (!environ_cb || !environ_cb(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, &supported)) supported = false;
    input_bitmasks = supported ? 1 : 0;
}

static uint32_t read_buttons(void) {
    /* An analog RetroPad still exposes its digital controls as JOYPAD.
     * GP32 uses those buttons only; NONE and unrelated devices stay inactive. */
    if (!input_state_cb ||
        (input_device != RETRO_DEVICE_JOYPAD && input_device != RETRO_DEVICE_ANALOG)) return 0;
    uint32_t m = 0;
    if (input_bitmasks) {
        /* Bit N of the mask is RETRO_DEVICE_ID_JOYPAD_<N>; the 16-button
         * bitmask arrives sign-extended through the int16_t return value. */
        uint32_t bits = (uint16_t)input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK);
        if (bits & (1u << RETRO_DEVICE_ID_JOYPAD_A)) m |= GP32_BUTTON_A;
        if (bits & (1u << RETRO_DEVICE_ID_JOYPAD_B)) m |= GP32_BUTTON_B;
        if (bits & (1u << RETRO_DEVICE_ID_JOYPAD_L)) m |= GP32_BUTTON_L;
        if (bits & (1u << RETRO_DEVICE_ID_JOYPAD_R)) m |= GP32_BUTTON_R;
        if (bits & (1u << RETRO_DEVICE_ID_JOYPAD_START)) m |= GP32_BUTTON_START;
        if (bits & (1u << RETRO_DEVICE_ID_JOYPAD_SELECT)) m |= GP32_BUTTON_SELECT;
        if (bits & (1u << RETRO_DEVICE_ID_JOYPAD_UP)) m |= GP32_BUTTON_UP;
        if (bits & (1u << RETRO_DEVICE_ID_JOYPAD_DOWN)) m |= GP32_BUTTON_DOWN;
        if (bits & (1u << RETRO_DEVICE_ID_JOYPAD_LEFT)) m |= GP32_BUTTON_LEFT;
        if (bits & (1u << RETRO_DEVICE_ID_JOYPAD_RIGHT)) m |= GP32_BUTTON_RIGHT;
        return m;
    }
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A)) m |= GP32_BUTTON_A;
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B)) m |= GP32_BUTTON_B;
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L)) m |= GP32_BUTTON_L;
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R)) m |= GP32_BUTTON_R;
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START)) m |= GP32_BUTTON_START;
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT)) m |= GP32_BUTTON_SELECT;
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP)) m |= GP32_BUTTON_UP;
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN)) m |= GP32_BUTTON_DOWN;
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT)) m |= GP32_BUTTON_LEFT;
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT)) m |= GP32_BUTTON_RIGHT;
    return m;
}


static uint32_t force_xrgb(uint32_t p) {
    return 0xff000000u | (p & 0x00ffffffu);
}

static int stage_frame_320x240(const gp32_framebuffer_desc_t *fb, uint32_t *dst) {
    if (!fb || !dst || !fb->pixels_rgba8888 || !fb->width || !fb->height || fb->stride_pixels < fb->width) return 0;
    const uint32_t *src = fb->pixels_rgba8888;
    uint32_t stride = fb->stride_pixels;

    if (fb->width == GP32_W && fb->height == GP32_H) {
        for (uint32_t y = 0; y < GP32_H; ++y) {
            const uint32_t *row = src + (size_t)y * stride;
            uint32_t *out = dst + (size_t)y * GP32_W;
            for (uint32_t x = 0; x < GP32_W; ++x) out[x] = force_xrgb(row[x]);
        }
        return 1;
    }

    if (fb->width == GP32_RAW_W && fb->height == GP32_RAW_H) {
        /* S3C2400 native LCD memory is 240x320 portrait. GP32 is held
         * landscape, so rotate 90 degrees counter-clockwise to expose the
         * standard libretro 320x240 display. This matches the SDL/Win64/media
         * presenter path and fixes the earlier cropped/scrambled 240x320 copy. */
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
        const uint32x4_t alpha = vdupq_n_u32(0xff000000u);
        for (uint32_t by = 0; by < GP32_H; by += 4u) {
            for (uint32_t bx = 0; bx < GP32_W; bx += 4u) {
                const uint32_t *p = src + (size_t)bx * stride + GP32_RAW_W - 4u - by;
                uint32x4_t a = vorrq_u32(vld1q_u32(p), alpha);
                uint32x4_t b = vorrq_u32(vld1q_u32(p + stride), alpha);
                uint32x4_t c = vorrq_u32(vld1q_u32(p + (size_t)stride * 2u), alpha);
                uint32x4_t d = vorrq_u32(vld1q_u32(p + (size_t)stride * 3u), alpha);
                uint32x4x2_t ab = vtrnq_u32(a, b);
                uint32x4x2_t cd = vtrnq_u32(c, d);
                uint32_t *out = dst + (size_t)by * GP32_W + bx;
                /* Transpose four rows, then reverse the column order for CCW. */
                vst1q_u32(out, vcombine_u32(vget_high_u32(ab.val[1]), vget_high_u32(cd.val[1])));
                vst1q_u32(out + GP32_W, vcombine_u32(vget_high_u32(ab.val[0]), vget_high_u32(cd.val[0])));
                vst1q_u32(out + GP32_W * 2u, vcombine_u32(vget_low_u32(ab.val[1]), vget_low_u32(cd.val[1])));
                vst1q_u32(out + GP32_W * 3u, vcombine_u32(vget_low_u32(ab.val[0]), vget_low_u32(cd.val[0])));
            }
        }
#else
        /* Keep both sides of the transpose in cache. A full output row reads
         * 320 different source rows; small tiles reuse those source cache
         * lines before advancing to the next part of the image. */
        for (uint32_t by = 0; by < GP32_H; by += 8u) {
            for (uint32_t bx = 0; bx < GP32_W; bx += 8u) {
                for (uint32_t y = by; y < by + 8u; ++y) {
                    uint32_t sx = GP32_RAW_W - 1u - y;
                    uint32_t *out = dst + (size_t)y * GP32_W;
                    for (uint32_t x = bx; x < bx + 8u; ++x)
                        out[x] = force_xrgb(src[(size_t)x * stride + sx]);
                }
            }
        }
#endif
        return 1;
    }

    /* Conservative fallback for unusual LCD register settings: scale/crop into
     * the fixed GP32 landscape size rather than presenting invalid dimensions. */
    for (uint32_t y = 0; y < GP32_H; ++y) {
        uint32_t sy = (uint32_t)(((uint64_t)y * fb->height) / GP32_H);
        if (sy >= fb->height) sy = fb->height - 1u;
        const uint32_t *row = src + (size_t)sy * stride;
        uint32_t *out = dst + (size_t)y * GP32_W;
        for (uint32_t x = 0; x < GP32_W; ++x) {
            uint32_t sx = (uint32_t)(((uint64_t)x * fb->width) / GP32_W);
            if (sx >= fb->width) sx = fb->width - 1u;
            out[x] = force_xrgb(row[sx]);
        }
    }
    return 1;
}

static void flush_audio(void);

static void mark_audio_output_gap(void) {
    /* Queue eviction breaks continuity at the retained head, not at the
     * resampler's generated tail. Anchor recovery to actual acceptance. */
    memcpy(audio_gap_from, audio_last_sent, sizeof(audio_gap_from));
    audio_gap_left = GP32_AUDIO_GAP_FRAMES;
}

static int submit_audio_resampled(const gp32_audio_desc_t *aud) {
    if (!aud || !aud->samples_s16_interleaved || !aud->frame_count) return 0;
    if (!audio_batch_cb && !audio_cb) return 1;
    uint32_t src_rate = aud->sample_rate_hz ? aud->sample_rate_hz : GP32_AUDIO_RATE;
    uint32_t dst_rate = GP32_AUDIO_RATE;
    const size_t max_frames = SIZE_MAX / (2u * sizeof(int16_t));
    if (aud->frame_count > max_frames) return 0;

    size_t in_frames = (size_t)aud->frame_count;
    size_t need = in_frames;
    int copy_aligned = audio_resampler.src_rate == src_rate &&
                       audio_resampler.phase_q32 == (UINT64_C(1) << 32);
    int resample = src_rate != dst_rate || (audio_resampler.have_prev && !copy_aligned);
    if (resample) {
        need = gp32_audio_resampler_max_output_frames(&audio_resampler, in_frames, src_rate, dst_rate, 0);
        if (need > GP32_AUDIO_QUEUE_LIMIT - audio_pending_frames) {
            /* Allocation slack is not generated PCM. Only pay for an exact
             * count near the limit, before evicting any retained samples. */
            need = gp32_audio_resampler_output_frames(&audio_resampler, in_frames, src_rate, dst_rate, 0);
            if (!need) {
                int16_t unused[2];
                (void)gp32_audio_resampler_process(&audio_resampler, aud->samples_s16_interleaved,
                                                    in_frames, src_rate, dst_rate, 0, unused, 1u);
                return 1;
            }
        }
    }
    if (!need) return 0;
    /* Bound memory and latency if the frontend stops consuming audio. Short
     * backpressure remains lossless; a sustained stall retains recent sound. */
    if (need > GP32_AUDIO_QUEUE_LIMIT) {
        flush_audio();
        audio_pending_frames = 0;
        gp32_audio_resampler_reset(&audio_resampler);
        mark_audio_output_gap();
        return 1;
    }
    /* A recovered frontend may consume the backlog before any PCM needs to
     * be discarded. Keep the normal, non-overflow delivery path unchanged. */
    if (need > GP32_AUDIO_QUEUE_LIMIT - audio_pending_frames) flush_audio();
    if (need > GP32_AUDIO_QUEUE_LIMIT - audio_pending_frames) {
        size_t drop = audio_pending_frames + need - GP32_AUDIO_QUEUE_LIMIT;
        audio_pending_frames -= drop;
        memmove(audio_resample_buf, audio_resample_buf + drop * 2u,
                audio_pending_frames * 2u * sizeof(int16_t));
        mark_audio_output_gap();
    }
    size_t total = audio_pending_frames + need;
    if (total > audio_resample_cap) {
        size_t cap = total;
        if (audio_resample_cap <= max_frames / 2u && cap < audio_resample_cap * 2u) cap = audio_resample_cap * 2u;
        if (cap > GP32_AUDIO_QUEUE_LIMIT) cap = GP32_AUDIO_QUEUE_LIMIT;
        int16_t *p = (int16_t *)realloc(audio_resample_buf, cap * 2u * sizeof(int16_t));
        if (!p) {
            lr_log(RETRO_LOG_ERROR, "[gp32emu] libretro audio allocation failed (%zu frames).\n", cap);
            return 0;
        }
        audio_resample_buf = p;
        audio_resample_cap = cap;
    }
    int16_t *out = audio_resample_buf + audio_pending_frames * 2u;
    size_t out_frames = in_frames;

    if (resample) {
        out_frames = gp32_audio_resampler_process(&audio_resampler,
                                                  aud->samples_s16_interleaved,
                                                  in_frames,
                                                  src_rate,
                                                  dst_rate,
                                                  0,
                                                  out,
                                                  need);
    } else {
        /* Direct copies have already emitted their final sample. Retain that
         * endpoint with the next output one tick ahead for a later rate
         * change, and keep the kernel history so a resumed resample joins the
         * stream instead of starting from an empty window. */
        gp32_audio_resampler_copy(&audio_resampler, aud->samples_s16_interleaved,
                                  in_frames, src_rate, out);
    }
    /* Preserve the waveform. Only a known queue discard marks a gap for
     * flush_audio(); sample amplitude alone cannot identify a click. */
    audio_pending_frames += out_frames;
    return 1;
}

static void submit_idle_silence(uint32_t rate) {
    static const int16_t silence[GP32_AUDIO_FRAMES_PER_VIDEO * 2u] = {0};
    const uint64_t denominator = UINT64_C(60) << 24;
    if (!rate) rate = GP32_AUDIO_RATE;
    if (audio_idle_rate && rate != audio_idle_rate)
        audio_idle_fraction = audio_idle_fraction * rate / audio_idle_rate;
    audio_idle_rate = rate;
    uint64_t budget = audio_idle_fraction + ((uint64_t)(rate % 60u) << 24);
    size_t frames = rate / 60u + (size_t)(budget / denominator);
    audio_idle_fraction = budget % denominator;
    while (frames) {
        size_t chunk = frames < GP32_AUDIO_FRAMES_PER_VIDEO ? frames : GP32_AUDIO_FRAMES_PER_VIDEO;
        gp32_audio_desc_t silent = {silence, chunk, rate};
        if (!submit_audio_resampled(&silent)) break;
        frames -= chunk;
    }
}

static void flush_audio(void) {
    if (!audio_batch_cb && !audio_cb) { audio_pending_frames = 0; return; }
    size_t sent = 0;
    int16_t recovery[GP32_AUDIO_GAP_FRAMES * 2u];
    while (sent < audio_pending_frames) {
        size_t frames = audio_pending_frames - sent;
        const int16_t *data = audio_resample_buf + sent * 2u;
        if (audio_gap_left) {
            if (frames > audio_gap_left) frames = audio_gap_left;
            unsigned progress = GP32_AUDIO_GAP_FRAMES - audio_gap_left;
            /* Scratch output leaves queued PCM unchanged when a callback
             * refuses or partially accepts this prefix. Advance only by the
             * frames it actually takes, including across later retro_run calls. */
            for (size_t i = 0; i < frames; ++i)
                for (unsigned ch = 0; ch < 2u; ++ch) {
                    int32_t from = audio_gap_from[ch];
                    int32_t delta = (int32_t)data[i * 2u + ch] - from;
                    recovery[i * 2u + ch] = (int16_t)(from +
                        delta * (int32_t)(progress + i + 1u) / (int32_t)GP32_AUDIO_GAP_FRAMES);
                }
            data = recovery;
        }
        size_t accepted;
        if (audio_batch_cb) accepted = audio_batch_cb(data, frames);
        else {
            for (size_t i = 0; i < frames; ++i) audio_cb(data[i * 2u], data[i * 2u + 1u]);
            accepted = frames;
        }
        if (!accepted) break;
        if (accepted > frames) accepted = frames;
        audio_last_sent[0] = data[(accepted - 1u) * 2u];
        audio_last_sent[1] = data[(accepted - 1u) * 2u + 1u];
        if (audio_gap_left) audio_gap_left -= (unsigned)accepted;
        sent += accepted;
    }
    audio_pending_frames -= sent;
    if (sent && audio_pending_frames) {
        memmove(audio_resample_buf, audio_resample_buf + sent * 2u,
                audio_pending_frames * 2u * sizeof(int16_t));
    }
}

/* Release every queued guest PCM span into the delivery queue. Each borrowed
 * span is consumed only once its output is retained, so a frontend that
 * refuses this call keeps the samples for the next one. */
static void drain_guest_audio(gp32_t *g, const gp32_audio_desc_t *first) {
    gp32_audio_desc_t aud = *first;
    while (aud.frame_count) {
        audio_guest_pcm_this_frame = 1;
        if (!submit_audio_resampled(&aud)) break;
        if (gp32_consume_audio(g, aud.frame_count) != GP32_OK) break;
        if (gp32_get_audio(g, &aud) != GP32_OK) break;
    }
}

/* Host pump installed on the core (gp32_set_host_pump). It runs between guest
 * slices, so PCM the guest has already produced reaches the frontend while the
 * frame is still executing. A frame that overruns 1/60 s no longer leaves the
 * frontend FIFO dry for its whole duration: the same PCM is delivered in
 * partial blocks on the way instead of only at the frame boundary. */
static void pump_audio_delivery(gp32_t *g, void *user) {
    (void)user;
    if (!g) return;
    gp32_audio_desc_t aud;
    if (gp32_get_audio(g, &aud) == GP32_OK && aud.frame_count) drain_guest_audio(g, &aud);
    /* CPU yields can produce only a handful of frames. Batch up to two ms
     * of output instead of entering the frontend thousands of times/second.
     * retro_run still flushes the remainder at every frame boundary. Keep
     * draining guest spans above so a short span at a rate change cannot
     * prevent the following span from reaching this queue. */
    if (audio_pending_frames >= GP32_AUDIO_PUMP_FRAMES) flush_audio();
}

/* libretro only accepts a NULL frame when the frontend returned true from
 * RETRO_ENVIRONMENT_GET_CAN_DUPE. Other frontends must be handed real pixels
 * again; after an effect pass those pixels live in effect_rgb, so the last
 * presented pointer is tracked instead of assuming frame_rgb. */
static void present_duplicate_frame(void) {
    if (!video_cb) return;
    if (can_dupe) {
        video_cb(NULL, 0, 0, 0);
        return;
    }
    if (have_last_video) {
        video_cb(last_video_ptr, GP32_W, GP32_H, GP32_W * sizeof(uint32_t));
        return;
    }
    /* Nothing has been presented yet, so a blank frame is the only ABI-valid
     * alternative to NULL for a frontend that cannot dupe. Latch it like any
     * presented frame: without that, every retro_run while no game is loaded
     * re-fills the whole staging buffer instead of re-sending the same
     * pixels through the last_video_ptr path. */
    for (size_t i = 0; i < GP32_W * GP32_H; ++i) frame_rgb[i] = 0xff000000u;
    video_cb(frame_rgb, GP32_W, GP32_H, GP32_W * sizeof(uint32_t));
    last_video_ptr = frame_rgb;
    have_last_video = 1;
}

void retro_run(void) {
    bool updated = false;
    if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated) refresh_variables();
    if (!emu) { present_duplicate_frame(); return; }
    if (input_poll_cb) input_poll_cb();
    gp32_set_buttons(emu, read_buttons());
    audio_guest_pcm_this_frame = 0;
    /* The pump releases produced PCM between guest slices; the frame-boundary
     * drain below picks up whatever it left. Reinstalling it per run is
     * deliberate: the hook is frontend state, not machine state, and every
     * core instance in this file goes through this single run path. */
    gp32_set_host_pump(emu, pump_audio_delivery, NULL);
    gp32_run_frame(emu);
    gp32_framebuffer_desc_t fb;
    int have_fb = gp32_get_framebuffer(emu, &fb) == GP32_OK;
    int effects_active = effects_ready && gp32_video_effects_active(&effects);
    if (have_fb && fb.frame_counter == last_video_frame && !effects_active) {
        /* The emulated LCD produced no new frame this run (for example while
         * the panel is disabled). Present a duplicate instead of restaging and
         * repushing identical pixels; the frontend re-shows the last frame when
         * it can dupe, otherwise the identical pixels are resent.
         * Time-varying filters are exempt because their output still changes. */
        present_duplicate_frame();
    } else if (have_fb && stage_frame_320x240(&fb, frame_rgb)) {
        const uint32_t *src = frame_rgb;
        if (effects_active && gp32_video_effects_process_320x240(&effects, frame_rgb, effect_rgb)) src = effect_rgb;
        if (video_cb) video_cb(src, GP32_W, GP32_H, GP32_W * sizeof(uint32_t));
        last_video_frame = fb.frame_counter;
        last_video_ptr = src;
        have_last_video = 1;
    } else {
        present_duplicate_frame();
    }
    gp32_audio_desc_t aud;
    if (gp32_get_audio(emu, &aud) == GP32_OK) {
        if (aud.frame_count) {
            /* Each span has its own source rate; drain_guest_audio releases it
             * only after the delivery queue retains its output. */
            drain_guest_audio(emu, &aud);
        } else if (!audio_guest_pcm_this_frame) {
            /* Keep audio-driven frontend pacing alive while emulated audio is
             * idle. Never pad short active blocks or alter their sample rate. */
            /* Silence occupies the same source sample grid as active audio,
             * including the boundary interpolation and fractional duration. */
            submit_idle_silence(aud.sample_rate_hz);
        }
    }
    flush_audio();
    if (smartmedia_save_path[0] && gp32_poll_card_progress(emu, smartmedia_save_path) != GP32_OK) {
        lr_log(RETRO_LOG_ERROR, "[gp32emu] Automatic card save failed: %s\n", gp32_get_error(emu));
        lr_message("GP32 automatic save failed. Check space and write access; retrying in the background.");
    }
}

static int load_content(gp32_t *g, const struct retro_game_info *game, content_boot_t boot) {
    const char *path = game ? game->path : NULL;
    const void *data = game ? game->data : NULL;
    size_t size = game ? game->size : 0;
    const char *label = (path && path[0]) ? path_basename(path) : "libretro-content";
    int ext_fxe = path && has_ext(path, "fxe");
    int ext_fpk = path && has_ext(path, "fpk");
    int ext_smc = path && has_ext(path, "smc");
    if (data && size) {
        if (ext_fxe) return gp32_load_fxe_data(g, data, size, label) == GP32_OK;
        if (ext_fpk) return gp32_load_fpk_data(g, data, size, label) == GP32_OK;
        if (ext_smc || (!ext_fxe && !ext_fpk)) {
            return load_smartmedia_content(g, data, size, path, label, boot) > 0;
        }
    }
    if (!path || !path[0]) return 0;
    if (ext_fxe) return gp32_load_fxe(g, path) == GP32_OK;
    if (ext_fpk) return gp32_load_fpk(g, path) == GP32_OK;
    if (ext_smc) {
        return load_smartmedia_content(g, NULL, 0, path, label, boot) > 0;
    }
    return 0;
}

static gp32_t *create_core_with_optional_bios(const char *bios_path) {
    gp32_options_t opt;
    memset(&opt, 0, sizeof(opt));
    opt.bios_path = (bios_path && bios_path[0]) ? bios_path : NULL;
    opt.log = gp32_log;
    gp32_t *g = gp32_create(&opt);
    if (!g && bios_path && bios_path[0]) lr_log(RETRO_LOG_ERROR, "[gp32emu] BIOS load failed: %s\n", bios_path);
    return g;
}

bool retro_load_game(const struct retro_game_info *game) {
    set_default_dirs();
    refresh_variables();
    refresh_input_bitmask_support();
    destroy_emu_with_save();
    reset_audio();
    invalidate_last_video();
    content_path[0] = smartmedia_save_path[0] = 0;
    if (game && game->path) snprintf(content_path, sizeof(content_path), "%s", game->path);
    make_runtime_paths(content_path[0] ? content_path : "gp32");
    unsigned fmt = RETRO_PIXEL_FORMAT_XRGB8888;
    if (environ_cb) environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt);

    int ext_smc = content_path[0] && has_ext(content_path, "smc");
    if (!ext_smc && !(game && game->data && game->size && !has_ext(content_path, "fxe") && !has_ext(content_path, "fpk"))) smartmedia_save_path[0] = 0;
    char bios_path[4096];
    int have_bios = find_bios_path(bios_path, sizeof(bios_path), content_path);

    lr_log(RETRO_LOG_INFO, "[gp32emu] system directory: %s\n", system_dir);
    lr_log(RETRO_LOG_INFO, "[gp32emu] save directory: %s\n", save_dir);
    if (content_path[0]) lr_log(RETRO_LOG_INFO, "[gp32emu] loading content: %s\n", content_path);

    /* The retail BIOS launcher starts a card from its top-level GAME\\ directory.
     * A card whose only executable sits in another folder (the freeware GPMM\
     * layout) ends in the firmware's own card-scan error path and stays on the
     * DATA LOADING screen forever, exactly as on hardware without the Free
     * Launcher. Those cards boot through the host-side direct loader instead,
     * which also mounts the card because the game reads its own assets through
     * it. require_bios stays an explicit BIOS choice. */
    smc_card_launch_layout_t layout = SMC_CARD_LAYOUT_NONE;
    char layout_exe[260] = {0};
    if (ext_smc && boot_mode != 1) {
        char layout_err[256] = {0};
        layout = (game && game->data && game->size)
            ? smc_direct_classify_buffer(game->data, game->size, path_basename(content_path),
                                         layout_exe, sizeof(layout_exe), layout_err, sizeof(layout_err))
            : smc_direct_classify_file(content_path, layout_exe, sizeof(layout_exe), layout_err, sizeof(layout_err));
        if (layout == SMC_CARD_LAYOUT_DIRECT_ONLY) {
            lr_log(RETRO_LOG_WARN, "[gp32emu] %s starts from %s: the retail BIOS launcher only boots GAME\\ cards, using the direct SmartMedia boot with the card mounted.\n",
                   path_basename(content_path), layout_exe[0] ? layout_exe : "a freeware layout");
            lr_message("GP32emu: freeware card layout - using direct boot");
        } else if (layout == SMC_CARD_LAYOUT_NONE) {
            lr_log(RETRO_LOG_INFO, "[gp32emu] no GAME\\ or freeware executable in %s (%s); keeping the configured boot path.\n",
                   path_basename(content_path), layout_err[0] ? layout_err : "unknown layout");
        }
    }
    content_boot_t boot = (boot_mode == 2) ? CONTENT_BOOT_DIRECT : CONTENT_BOOT_BIOS;
    if (layout == SMC_CARD_LAYOUT_DIRECT_ONLY) boot = CONTENT_BOOT_DIRECT_MEDIA;

    if (boot_mode == 1 && !have_bios) {
        lr_log(RETRO_LOG_ERROR, "[gp32emu] Required BIOS not found. Put gp32166m.bin in RetroArch's system directory.\n");
        lr_message("GP32emu: missing gp32166m.bin in system directory");
        return false;
    }

    if (boot == CONTENT_BOOT_BIOS && have_bios) {
        lr_log(RETRO_LOG_INFO, "[gp32emu] using BIOS: %s\n", bios_path);
        emu = create_core_with_optional_bios(bios_path);
        if (emu && load_content(emu, game, CONTENT_BOOT_BIOS)) {
            gp32_set_jit(emu, use_jit);
            gp32_set_cpu_speed_percent(emu, cpu_speed_percent);
            gp32_set_game_fixes(emu, use_game_fixes);
            gp32_set_fast_loading(emu, use_fast_loading);
            gp32_video_effects_reset(&effects);
            return true;
        }
        if (emu) {
            const char *err = gp32_get_error(emu);
            lr_log(RETRO_LOG_ERROR, "[gp32emu] BIOS boot content load failed: %s\n", (err && err[0]) ? err : "unknown error");
            gp32_destroy(emu);
            emu = NULL;
        }
        if (smartmedia_save_error || boot_mode == 1 || !ext_smc) return false;
        lr_log(RETRO_LOG_WARN, "[gp32emu] falling back to BIOSless direct SmartMedia boot.\n");
        lr_message("GP32emu: BIOS boot failed, using direct SmartMedia boot");
        boot = (layout == SMC_CARD_LAYOUT_DIRECT_ONLY) ? CONTENT_BOOT_DIRECT_MEDIA : CONTENT_BOOT_DIRECT;
    } else if (boot == CONTENT_BOOT_BIOS && !have_bios) {
        if (ext_smc && boot_mode == 0) {
            lr_log(RETRO_LOG_WARN, "[gp32emu] BIOS not found; trying BIOSless direct SmartMedia boot. Put gp32166m.bin in the system directory for normal BIOS boot.\n");
            lr_message("GP32emu: BIOS not found, using direct SmartMedia boot");
            boot = (layout == SMC_CARD_LAYOUT_DIRECT_ONLY) ? CONTENT_BOOT_DIRECT_MEDIA : CONTENT_BOOT_DIRECT;
        } else {
            lr_log(RETRO_LOG_INFO, "[gp32emu] BIOS not found; loading content through direct/HLE path.\n");
            boot = CONTENT_BOOT_DIRECT;
        }
    }

    emu = create_core_with_optional_bios(NULL);
    if (!emu) {
        lr_log(RETRO_LOG_ERROR, "[gp32emu] core allocation failed.\n");
        return false;
    }
    gp32_set_jit(emu, use_jit);
    gp32_set_cpu_speed_percent(emu, cpu_speed_percent);
    gp32_set_game_fixes(emu, use_game_fixes);
    gp32_set_fast_loading(emu, use_fast_loading);
    if (!load_content(emu, game, boot)) {
        const char *err = gp32_get_error(emu);
        lr_log(RETRO_LOG_ERROR, "[gp32emu] content load failed: %s\n", (err && err[0]) ? err : "unknown or unsupported content");
        lr_message("GP32emu: content load failed");
        gp32_destroy(emu);
        emu = NULL;
        return false;
    }
    gp32_video_effects_reset(&effects);
    return true;
}

#ifdef __ANDROID__
/* The standalone Android host flushes on lifecycle pause, on the same thread
 * as retro_run. Android may kill a stopped app without an unload callback. */
__attribute__((visibility("default"))) bool gp32emu_android_flush_save(void) {
    return !emu || !smartmedia_save_path[0] ||
           gp32_save_card_progress(emu, smartmedia_save_path) == GP32_OK;
}
#endif

void retro_unload_game(void) {
    destroy_emu_with_save();
    content_path[0] = 0;
    reset_audio();
    invalidate_last_video();
}
unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }
bool retro_load_game_special(unsigned game_type, const struct retro_game_info *info, size_t num_info) { (void)game_type; (void)info; (void)num_info; return false; }
size_t retro_serialize_size(void) {
    if (!emu) return 0;
    /* Libretro must never report a smaller size than the core then writes.
     * Normal retro_run drains captured PCM and the SmartMedia delta pads to a
     * per-session budget, so one game serializes to one size; the single case
     * that grows is a guest that rewrites more card pages than the budget
     * holds, which puts the whole image back into the section. Take the largest
     * value seen so a frontend buffer always fits what we write. */
    size_t guest = gp32_state_size(emu);
    if (!guest) return 0;
    size_t size = guest + lrst_capacity();
    if (size > state_capacity) state_capacity = size;
    return state_capacity;
}
bool retro_serialize(void *data, size_t size) {
    if (!emu || !data) return false;
    size_t capacity = retro_serialize_size();
    if (!capacity || size < capacity) return false;
    size_t guest = gp32_state_size(emu);
    if (!guest || guest > size) return false;
    if (gp32_save_state_data(emu, data, guest) != GP32_OK) return false;
    /* The frontend delivery section follows the guest payload; old loaders
     * stop after the guest sections and ignore it. */
    size_t written = lrst_write((uint8_t *)data + guest, size - guest);
    if (!written) return false;
    if (guest + written < size)
        memset((uint8_t *)data + guest + written, 0, size - guest - written);
    return true;
}
bool retro_unserialize(const void *data, size_t size) {
    if (!emu || !data || !size) return false;
    size_t guest = 0;
    if (gp32_load_state_data_ex(emu, data, size, &guest) != GP32_OK) return false;
    if (guest && guest <= size) {
        int loaded = lrst_read((const uint8_t *)data + guest, size - guest);
        if (loaded == 1) return true;
        if (loaded < 0) {
            /* A truncated delivery section would silently replay with reset
             * audio/duplicate state, which is exactly the divergence this
             * section exists to prevent. */
            reset_loaded_delivery_state();
            return false;
        }
    }
    /* State from before the delivery section, or one this build cannot read:
     * keep the historical reset. A loaded state can restore a frame counter
     * this run already presented, so equality alone cannot prove the pixels
     * are unchanged. */
    reset_loaded_delivery_state();
    return true;
}
void *retro_get_memory_data(unsigned id) { (void)id; return NULL; }
size_t retro_get_memory_size(unsigned id) { (void)id; return 0; }
void retro_cheat_reset(void) {}
void retro_cheat_set(unsigned index, bool enabled, const char *code) { (void)index; (void)enabled; (void)code; }
