/* Minimal Android libretro host. All calls are confined to the emulation thread. */
#include <android/bitmap.h>
#include <android/log.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <jni.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "libretro.h"

static void *library;
static char system_dir[4096], save_dir[4096], last_message[512];
static bool initialized, loaded;
static uint32_t pixels[320 * 240];
static int16_t audio[8192];
static size_t audio_samples;
static unsigned buttons;
static void (*core_init)(void), (*core_deinit)(void), (*core_run)(void), (*core_unload)(void),
            (*core_reset)(void);
static bool (*core_load)(const struct retro_game_info *), (*core_flush)(void);
static size_t (*core_state_size)(void);
static bool (*core_serialize)(void *, size_t), (*core_unserialize)(const void *, size_t);

static void log_message(int level, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    __android_log_vprint(level >= RETRO_LOG_ERROR ? ANDROID_LOG_ERROR : ANDROID_LOG_INFO,
                        "GP32emu", fmt, args);
    va_end(args);
}

static bool environment(unsigned cmd, void *data) {
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_CONTENT_DIRECTORY:
        *(const char **)data = system_dir; return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *(const char **)data = save_dir; return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        return *(unsigned *)data == RETRO_PIXEL_FORMAT_XRGB8888;
    case 3: /* GET_CAN_DUPE (older bundled header omits this definition). */
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
        *(bool *)data = true; return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *(bool *)data = false; return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        ((struct retro_log_callback *)data)->log = log_message; return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        struct retro_variable *v = data;
        if (!strcmp(v->key, "gp32emu_jit")) v->value = "enabled";
        else if (!strcmp(v->key, "gp32emu_cpu_speed")) v->value = "100";
        else if (!strcmp(v->key, "gp32emu_boot_mode")) v->value = "auto";
        else if (!strcmp(v->key, "gp32emu_lcd_persistence") ||
                 !strcmp(v->key, "gp32emu_frame_interpolation")) v->value = "disabled";
        else { v->value = NULL; return false; }
        return true;
    }
    case RETRO_ENVIRONMENT_SET_MESSAGE:
        snprintf(last_message, sizeof(last_message), "%s", ((struct retro_message *)data)->msg);
        return true;
    case RETRO_ENVIRONMENT_SET_VARIABLES:
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
        return true;
    default: return false;
    }
}

static void video(const void *data, unsigned w, unsigned h, size_t pitch) {
    if (!data || w != 320 || h != 240 || pitch < 320 * 4) return;
    for (unsigned y = 0; y < h; ++y) {
        const uint32_t *src = (const uint32_t *)((const uint8_t *)data + y * pitch);
        for (unsigned x = 0; x < w; ++x) {
            uint32_t p = src[x];
            /* libretro XRGB to Android RGBA byte order on both supported ABIs. */
            pixels[y * 320 + x] = 0xff000000u | ((p & 255u) << 16) |
                                  (p & 0xff00u) | ((p >> 16) & 255u);
        }
    }
}
static size_t audio_batch(const int16_t *data, size_t frames) {
    size_t available = (sizeof(audio) / sizeof(audio[0]) - audio_samples) / 2;
    if (frames > available) frames = available;
    memcpy(audio + audio_samples, data, frames * 2 * sizeof(*data));
    audio_samples += frames * 2;
    return frames; /* The core retains any unconsumed tail. */
}
static void audio_sample(int16_t l, int16_t r) { int16_t pair[] = {l, r}; audio_batch(pair, 1); }
static void poll_input(void) {}
static int16_t read_input(unsigned port, unsigned device, unsigned index, unsigned id) {
    (void)index;
    if (port || device != RETRO_DEVICE_JOYPAD) return 0;
    return id == RETRO_DEVICE_ID_JOYPAD_MASK ? (int16_t)buttons :
           (id < 16 ? (buttons >> id) & 1u : 0);
}

JNIEXPORT jstring JNICALL Java_org_gp32emu_app_NativeCore_open
  (JNIEnv *env, jclass cls, jstring system, jstring saves, jstring path) {
    (void)cls;
    if (loaded) { core_unload(); loaded = false; }
    last_message[0] = 0;
    if (!library) {
        library = dlopen("libgp32core.so", RTLD_NOW | RTLD_LOCAL);
        if (!library) return (*env)->NewStringUTF(env, "Cannot load the GP32 core.");
    }
    const char *s = (*env)->GetStringUTFChars(env, system, NULL);
    if (!s) return NULL;
    snprintf(system_dir, sizeof(system_dir), "%s", s);
    (*env)->ReleaseStringUTFChars(env, system, s);
    s = (*env)->GetStringUTFChars(env, saves, NULL);
    if (!s) return NULL;
    snprintf(save_dir, sizeof(save_dir), "%s", s);
    (*env)->ReleaseStringUTFChars(env, saves, s);
    if (!initialized) {
        void (*set_environment)(retro_environment_t);
        void (*set_video)(retro_video_refresh_t);
        void (*set_batch)(retro_audio_sample_batch_t);
        void (*set_sample)(retro_audio_sample_t);
        void (*set_poll)(retro_input_poll_t);
        void (*set_input)(retro_input_state_t);
#define RESOLVE(dst, name) do { *(void **)(&(dst)) = dlsym(library, name); \
    if (!(dst)) return (*env)->NewStringUTF(env, "Incompatible GP32 core."); } while (0)
        RESOLVE(core_init, "retro_init"); RESOLVE(core_deinit, "retro_deinit");
        RESOLVE(core_load, "retro_load_game"); RESOLVE(core_unload, "retro_unload_game");
        RESOLVE(core_run, "retro_run"); RESOLVE(core_flush, "gp32emu_android_flush_save");
        RESOLVE(core_reset, "retro_reset");
        RESOLVE(core_state_size, "retro_serialize_size");
        RESOLVE(core_serialize, "retro_serialize"); RESOLVE(core_unserialize, "retro_unserialize");
        RESOLVE(set_environment, "retro_set_environment"); RESOLVE(set_video, "retro_set_video_refresh");
        RESOLVE(set_batch, "retro_set_audio_sample_batch"); RESOLVE(set_sample, "retro_set_audio_sample");
        RESOLVE(set_poll, "retro_set_input_poll"); RESOLVE(set_input, "retro_set_input_state");
#undef RESOLVE
        set_environment(environment); set_video(video); set_batch(audio_batch);
        set_sample(audio_sample); set_poll(poll_input); set_input(read_input);
        core_init(); initialized = true;
    }
    s = (*env)->GetStringUTFChars(env, path, NULL);
    if (!s) return NULL;
    struct retro_game_info game = {.path = s};
    loaded = core_load(&game);
    (*env)->ReleaseStringUTFChars(env, path, s);
    memset(pixels, 0, sizeof(pixels));
    audio_samples = 0;
    return loaded ? NULL : (*env)->NewStringUTF(env, last_message[0] ? last_message : "Unable to open this game.");
}

JNIEXPORT jint JNICALL Java_org_gp32emu_app_NativeCore_frame
  (JNIEnv *env, jclass cls, jint input, jobject bitmap, jshortArray pcm) {
    (void)cls;
    if (!loaded || (*env)->GetArrayLength(env, pcm) < 8192) return -1;
    buttons = (unsigned)input;
    audio_samples = 0;
    core_run();
    AndroidBitmapInfo info;
    void *dst;
    if (AndroidBitmap_getInfo(env, bitmap, &info) != ANDROID_BITMAP_RESULT_SUCCESS ||
        info.width != 320 || info.height != 240 || info.format != ANDROID_BITMAP_FORMAT_RGBA_8888 ||
        AndroidBitmap_lockPixels(env, bitmap, &dst) != ANDROID_BITMAP_RESULT_SUCCESS) return -1;
    for (unsigned y = 0; y < 240; ++y)
        memcpy((uint8_t *)dst + y * info.stride, pixels + y * 320, 320 * sizeof(uint32_t));
    AndroidBitmap_unlockPixels(env, bitmap);
    (*env)->SetShortArrayRegion(env, pcm, 0, (jsize)audio_samples, audio);
    return (jint)audio_samples;
}
JNIEXPORT jboolean JNICALL Java_org_gp32emu_app_NativeCore_flush
  (JNIEnv *env, jclass cls) { (void)env; (void)cls; return !loaded || core_flush(); }
JNIEXPORT void JNICALL Java_org_gp32emu_app_NativeCore_reset
  (JNIEnv *env, jclass cls) {
    (void)env; (void)cls;
    if (!loaded) return;
    core_flush(); /* Keep card progress made before the restart. */
    core_reset();
    audio_samples = 0;
}

/* State file: 8-byte magic, payload size and CRC-32 (little endian), then the
 * core's serialized bytes. The checks reject truncated or damaged files before
 * the core sees them, because a core may apply part of a state it then rejects. */
#define STATE_HEADER 16u
#define STATE_MAX (256u << 20)
static const uint8_t state_magic[8] = {'G', 'P', '3', '2', 'S', 'T', '1', 0};

static uint32_t crc32_of(const uint8_t *p, size_t n) {
    static uint32_t table[256];
    if (!table[1])
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? 0xedb88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    uint32_t crc = ~0u;
    while (n--) crc = table[(crc ^ *p++) & 255u] ^ (crc >> 8);
    return ~crc;
}
static void put_le32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i)); }
static uint32_t get_le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static bool write_all(int fd, const uint8_t *p, size_t n) {
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return false;
        p += w; n -= (size_t)w;
    }
    return true;
}
static bool read_all(int fd, uint8_t *p, size_t n) {
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return false;
        p += r; n -= (size_t)r;
    }
    return true;
}

/* Temp file + fsync + rename, so a crash leaves the previous state intact. */
static const char *write_state_file(const char *path, const uint8_t *data, size_t size) {
    char temp[4096 + 8];
    if (snprintf(temp, sizeof(temp), "%s.tmp", path) >= (int)sizeof(temp)) return "State path is too long.";
    uint8_t head[STATE_HEADER];
    memcpy(head, state_magic, 8);
    put_le32(head + 8, (uint32_t)size);
    put_le32(head + 12, crc32_of(data, size));
    int fd = open(temp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return "Cannot create the state file.";
    bool ok = write_all(fd, head, sizeof(head)) && write_all(fd, data, size) && fsync(fd) == 0;
    if (close(fd) != 0) ok = false;
    if (!ok || rename(temp, path) != 0) {
        unlink(temp);
        return "Cannot write the state file. Check free storage.";
    }
    char folder[4096];
    snprintf(folder, sizeof(folder), "%s", path);
    char *slash = strrchr(folder, '/');
    if (slash) {
        *slash = 0; /* Make the rename durable; failure here is not fatal. */
        int dir = open(folder, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dir >= 0) { fsync(dir); close(dir); }
    }
    return NULL;
}

JNIEXPORT jstring JNICALL Java_org_gp32emu_app_NativeCore_saveState
  (JNIEnv *env, jclass cls, jstring path) {
    (void)cls;
    const char *error = NULL;
    if (!loaded) return (*env)->NewStringUTF(env, "No game is running.");
    size_t size = core_state_size();
    if (!size || size > STATE_MAX) return (*env)->NewStringUTF(env, "This game cannot be saved to a state.");
    uint8_t *buffer = malloc(size);
    if (!buffer) return (*env)->NewStringUTF(env, "Not enough memory.");
    if (!core_serialize(buffer, size)) error = "The core could not capture the state.";
    else {
        const char *s = (*env)->GetStringUTFChars(env, path, NULL);
        if (s) { error = write_state_file(s, buffer, size); (*env)->ReleaseStringUTFChars(env, path, s); }
        else { (*env)->ExceptionClear(env); error = "Invalid state path."; }
    }
    free(buffer);
    return error ? (*env)->NewStringUTF(env, error) : NULL;
}

JNIEXPORT jstring JNICALL Java_org_gp32emu_app_NativeCore_loadState
  (JNIEnv *env, jclass cls, jstring path) {
    (void)cls;
    if (!loaded) return (*env)->NewStringUTF(env, "No game is running.");
    const char *s = (*env)->GetStringUTFChars(env, path, NULL);
    if (!s) { (*env)->ExceptionClear(env); return (*env)->NewStringUTF(env, "Invalid state path."); }
    const char *error = NULL;
    uint8_t *buffer = NULL;
    int fd = open(s, O_RDONLY | O_CLOEXEC);
    (*env)->ReleaseStringUTFChars(env, path, s);
    struct stat st;
    uint8_t head[STATE_HEADER];
    if (fd < 0) error = "There is no saved state in this slot.";
    else if (fstat(fd, &st) != 0 || st.st_size < (off_t)STATE_HEADER || st.st_size > (off_t)(STATE_MAX + STATE_HEADER) ||
             !read_all(fd, head, sizeof(head)) || memcmp(head, state_magic, 8) != 0 ||
             get_le32(head + 8) != (uint32_t)(st.st_size - STATE_HEADER))
        error = "The saved state is damaged or has the wrong size.";
    else {
        size_t size = (size_t)(st.st_size - STATE_HEADER);
        buffer = malloc(size);
        if (!buffer) error = "Not enough memory.";
        else if (!read_all(fd, buffer, size) || crc32_of(buffer, size) != get_le32(head + 12))
            error = "The saved state is damaged or has the wrong size.";
        else if (!core_unserialize(buffer, size))
            error = "The core rejected this state. It may belong to another game or version.";
        else audio_samples = 0; /* Sound produced before the load is stale. */
    }
    free(buffer);
    if (fd >= 0) close(fd);
    return error ? (*env)->NewStringUTF(env, error) : NULL;
}

JNIEXPORT jstring JNICALL Java_org_gp32emu_app_NativeCore_message
  (JNIEnv *env, jclass cls) {
    (void)cls;
    if (!last_message[0]) return NULL;
    jstring result = (*env)->NewStringUTF(env, last_message);
    last_message[0] = 0;
    return result;
}
JNIEXPORT void JNICALL Java_org_gp32emu_app_NativeCore_close
  (JNIEnv *env, jclass cls) {
    (void)env; (void)cls;
    if (initialized) core_deinit();
    initialized = loaded = false;
    /* Keep dlopen alive: Android may recreate an Activity in this process. */
}
