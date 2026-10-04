#include "libretro.h"
#include "gp32emu/gp32.h"
#include "gp32emu/video_effects.h"
#include "audio/gp32_audio_resampler.h"
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
#define GP32_FPS 60.0
#define GP32_AUDIO_RATE 44100u
#define GP32_AUDIO_FRAMES_PER_VIDEO 735u
#define GP32_AUDIO_QUEUE_LIMIT (GP32_AUDIO_RATE / 4u)

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
static size_t state_capacity;
static int use_jit;
static int use_lcd_persistence;
static int use_frame_interpolation;
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
    /* RetroArch-convention name: <save dir>/<rom basename>.gp32.smc.
     * Known limitation: ROMs sharing a basename in different folders share
     * one save file. */
    snprintf(smc_name, sizeof(smc_name), "%s.gp32.smc", stem);
    join_path(smartmedia_save_path, sizeof(smartmedia_save_path), save_dir, smc_name);
}

/* Flush the persisted SmartMedia image before releasing the emulator. Used by
 * every teardown path so a game replaced mid-session keeps its NAND writes. */
static void destroy_emu_with_save(void) {
    state_capacity = 0;
    if (!emu) return;
    if (smartmedia_save_path[0]) gp32_save_smartmedia(emu, smartmedia_save_path);
    gp32_destroy(emu);
    emu = NULL;
}

/* A persisted SmartMedia image, when present, supersedes the original media.
 * Returns 1 if the save was mounted, 0 if absent, -1 if present but unreadable.
 * The -1 case fails the content load instead of silently mounting the pristine
 * ROM, which would overwrite the save on exit. */
static int mount_saved_smartmedia(gp32_t *g, int use_direct) {
    if (!smartmedia_save_path[0] || !file_exists(smartmedia_save_path)) return 0;
    gp32_status_t st = use_direct ? gp32_load_smartmedia_direct(g, smartmedia_save_path)
                                  : gp32_load_smartmedia(g, smartmedia_save_path);
    if (st != GP32_OK) {
        const char *err = gp32_get_error(g);
        lr_log(RETRO_LOG_ERROR, "[gp32emu] saved SmartMedia image %s is unreadable: %s\n",
               smartmedia_save_path, (err && err[0]) ? err : "invalid image");
        lr_message("GP32emu: saved SmartMedia image is unreadable; load aborted to protect it");
        return -1;
    }
    lr_log(RETRO_LOG_INFO, "[gp32emu] restored saved SmartMedia image: %s\n", smartmedia_save_path);
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
    info->library_version = "v102-libretro-av-fix";
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
    info->timing.fps = GP32_FPS;
    info->timing.sample_rate = GP32_AUDIO_RATE;
}

static void reset_audio(void) {
    audio_pending_frames = 0;
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
    if (!input_state_cb || input_device != RETRO_DEVICE_JOYPAD) return 0;
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

static int submit_audio_resampled(const gp32_audio_desc_t *aud) {
    if (!aud || !aud->samples_s16_interleaved || !aud->frame_count) return 0;
    if (!audio_batch_cb && !audio_cb) return 1;
    uint32_t src_rate = aud->sample_rate_hz ? aud->sample_rate_hz : GP32_AUDIO_RATE;
    uint32_t dst_rate = GP32_AUDIO_RATE;
    const size_t max_frames = SIZE_MAX / (2u * sizeof(int16_t));
    if (aud->frame_count > max_frames) return 0;

    size_t in_frames = (size_t)aud->frame_count;
    size_t need = in_frames;
    int resample = src_rate != dst_rate || audio_resampler.have_prev;
    if (resample) {
        need = gp32_audio_resampler_max_output_frames(&audio_resampler, in_frames, src_rate, dst_rate, 0);
    }
    if (!need) return 0;
    /* Bound memory and latency if the frontend stops consuming audio. Short
     * backpressure remains lossless; a sustained stall retains recent sound. */
    if (need > GP32_AUDIO_QUEUE_LIMIT) {
        flush_audio();
        audio_pending_frames = 0;
        gp32_audio_resampler_reset(&audio_resampler);
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
        gp32_audio_resampler_reset(&audio_resampler);
        memcpy(out, aud->samples_s16_interleaved, in_frames * 2u * sizeof(int16_t));
    }
    audio_pending_frames += out_frames;
    return 1;
}

static void flush_audio(void) {
    size_t sent = 0;
    if (audio_batch_cb) {
        while (sent < audio_pending_frames) {
            size_t remaining = audio_pending_frames - sent;
            size_t accepted = audio_batch_cb(audio_resample_buf + sent * 2u, remaining);
            /* A frontend may stop accepting audio until the next retro_run. */
            if (!accepted) break;
            if (accepted > remaining) accepted = remaining;
            sent += accepted;
        }
    } else {
        if (audio_cb) {
            for (size_t i = 0; i < audio_pending_frames; ++i) {
                audio_cb(audio_resample_buf[i * 2u], audio_resample_buf[i * 2u + 1u]);
            }
        }
        sent = audio_pending_frames;
    }
    audio_pending_frames -= sent;
    if (sent && audio_pending_frames) {
        memmove(audio_resample_buf, audio_resample_buf + sent * 2u,
                audio_pending_frames * 2u * sizeof(int16_t));
    }
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
            /* Each span has its own source rate. Release it only after the
             * delivery queue retains its output, even under backpressure. */
            do {
                if (!submit_audio_resampled(&aud)) break;
                if (gp32_consume_audio(emu, aud.frame_count) != GP32_OK) break;
            } while (gp32_get_audio(emu, &aud) == GP32_OK && aud.frame_count);
        } else {
            /* Keep audio-driven frontend pacing alive while emulated audio is
             * idle. Never pad short active blocks or alter their sample rate. */
            static const int16_t silence[GP32_AUDIO_FRAMES_PER_VIDEO * 2u] = {0};
            gp32_audio_desc_t silent = {silence, GP32_AUDIO_FRAMES_PER_VIDEO, GP32_AUDIO_RATE};
            gp32_audio_resampler_reset(&audio_resampler);
            submit_audio_resampled(&silent);
        }
    }
    flush_audio();
}

static int load_content(gp32_t *g, const struct retro_game_info *game, int use_direct) {
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
            int saved = mount_saved_smartmedia(g, use_direct);
            if (saved) return saved > 0;
            if (use_direct) return gp32_load_smartmedia_direct_data(g, data, size, label) == GP32_OK;
            return gp32_load_smartmedia_data(g, data, size) == GP32_OK;
        }
    }
    if (!path || !path[0]) return 0;
    if (ext_fxe) return gp32_load_fxe(g, path) == GP32_OK;
    if (ext_fpk) return gp32_load_fpk(g, path) == GP32_OK;
    if (ext_smc) {
        int saved = mount_saved_smartmedia(g, use_direct);
        if (saved) return saved > 0;
        if (use_direct) return gp32_load_smartmedia_direct(g, path) == GP32_OK;
        return gp32_load_smartmedia(g, path) == GP32_OK;
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
    int use_direct = (boot_mode == 2);
    char bios_path[4096];
    int have_bios = find_bios_path(bios_path, sizeof(bios_path), content_path);

    lr_log(RETRO_LOG_INFO, "[gp32emu] system directory: %s\n", system_dir);
    lr_log(RETRO_LOG_INFO, "[gp32emu] save directory: %s\n", save_dir);
    if (content_path[0]) lr_log(RETRO_LOG_INFO, "[gp32emu] loading content: %s\n", content_path);

    if (boot_mode == 1 && !have_bios) {
        lr_log(RETRO_LOG_ERROR, "[gp32emu] Required BIOS not found. Put gp32166m.bin in RetroArch's system directory.\n");
        lr_message("GP32emu: missing gp32166m.bin in system directory");
        return false;
    }

    if (!use_direct && have_bios) {
        lr_log(RETRO_LOG_INFO, "[gp32emu] using BIOS: %s\n", bios_path);
        emu = create_core_with_optional_bios(bios_path);
        if (emu && load_content(emu, game, 0)) {
            gp32_set_jit(emu, use_jit);
            gp32_video_effects_reset(&effects);
            return true;
        }
        if (emu) {
            const char *err = gp32_get_error(emu);
            lr_log(RETRO_LOG_ERROR, "[gp32emu] BIOS boot content load failed: %s\n", (err && err[0]) ? err : "unknown error");
            gp32_destroy(emu);
            emu = NULL;
        }
        if (boot_mode == 1 || !ext_smc) return false;
        lr_log(RETRO_LOG_WARN, "[gp32emu] falling back to BIOSless direct SmartMedia boot.\n");
        lr_message("GP32emu: BIOS boot failed, using direct SmartMedia boot");
        use_direct = 1;
    } else if (!use_direct && !have_bios) {
        if (ext_smc && boot_mode == 0) {
            lr_log(RETRO_LOG_WARN, "[gp32emu] BIOS not found; trying BIOSless direct SmartMedia boot. Put gp32166m.bin in the system directory for normal BIOS boot.\n");
            lr_message("GP32emu: BIOS not found, using direct SmartMedia boot");
            use_direct = 1;
        } else {
            lr_log(RETRO_LOG_INFO, "[gp32emu] BIOS not found; loading content through direct/HLE path.\n");
            use_direct = 1;
        }
    }

    emu = create_core_with_optional_bios(NULL);
    if (!emu) {
        lr_log(RETRO_LOG_ERROR, "[gp32emu] core allocation failed.\n");
        return false;
    }
    gp32_set_jit(emu, use_jit);
    if (!load_content(emu, game, use_direct)) {
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
    /* Libretro must never report a larger size during one loaded game. Normal
     * retro_run drains captured PCM; latch the first exact payload size and
     * reject later growth that cannot fit the frontend's supplied buffer. */
    if (!state_capacity) state_capacity = gp32_state_size(emu);
    return state_capacity;
}
bool retro_serialize(void *data, size_t size) {
    if (!emu || !data) return false;
    size_t capacity = retro_serialize_size();
    if (!capacity || size < capacity) return false;
    return gp32_save_state_data(emu, data, size) == GP32_OK;
}
bool retro_unserialize(const void *data, size_t size) {
    if (!emu || !data || !size) return false;
    if (gp32_load_state_data(emu, data, size) != GP32_OK) return false;
    gp32_video_effects_reset(&effects);
    reset_audio();
    /* A loaded state can restore a frame counter this run already presented,
     * so equality alone cannot prove the pixels are unchanged. */
    invalidate_last_video();
    return true;
}
void *retro_get_memory_data(unsigned id) { (void)id; return NULL; }
size_t retro_get_memory_size(unsigned id) { (void)id; return 0; }
void retro_cheat_reset(void) {}
void retro_cheat_set(unsigned index, bool enabled, const char *code) { (void)index; (void)enabled; (void)code; }
