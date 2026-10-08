/* Exercises the real libretro content-load/teardown paths against the real
 * SmartMedia device model. Scripted pieces are limited to the frontend
 * environment callback, a stub BIOS file, and blank SmartMedia images; the
 * loaders, NAND program/read commands, image save/load, and the core's save
 * file selection all run real code.
 *
 * Link this translation unit with gp32emu, not a second copy of libretro.c.
 */
#include <errno.h>
#ifndef LIBRETRO_SOURCE
#define LIBRETRO_SOURCE "../src/libretro/libretro.c"
#endif
#include LIBRETRO_SOURCE
#include "smartmedia.h"

#if defined(_WIN32)
#include <direct.h>
#define make_dir(p) _mkdir(p)
#else
#include <sys/stat.h>
#define make_dir(p) mkdir((p), 0755)
#endif

/* Small raw 528-byte-page image; large enough for several erase blocks. */
#define ROM_PAGES 128u
#define ROM_SIZE (528u * ROM_PAGES)
#define MARK_BYTE 0x42u
#define MARK_PAGE 3u
#define MARK_COL 7u

static char test_root[4096];
static char test_system_dir[4096];
static char test_save_dir[4096];
static char test_content_dir[4096];
static int failures;
static unsigned error_logs, user_messages;

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s (line %d)\n", message, __LINE__); \
        ++failures; \
    } \
} while (0)

static void quiet_log(int level, const char *fmt, ...) {
    (void)fmt;
    if (level == RETRO_LOG_ERROR) ++error_logs;
}

static bool test_environ(unsigned cmd, void *data) {
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        *(const char **)data = test_system_dir;
        return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *(const char **)data = test_save_dir;
        return true;
    case RETRO_ENVIRONMENT_GET_CONTENT_DIRECTORY:
        *(const char **)data = test_content_dir;
        return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: {
        struct retro_log_callback *cb = (struct retro_log_callback *)data;
        cb->log = quiet_log;
        return true;
    }
    case RETRO_ENVIRONMENT_SET_MESSAGE:
        ++user_messages;
        return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_VARIABLES:
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
        return true;
    default:
        return false;
    }
}

static int write_file(const char *path, const void *data, size_t size) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    int ok = fwrite(data, 1, size, f) == size;
    if (fclose(f) != 0) ok = 0;
    return ok;
}

static int read_file(const char *path, uint8_t *buf, size_t cap, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    if (n < 0 || (size_t)n > cap) { fclose(f); return 0; }
    rewind(f);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); return 0; }
    fclose(f);
    if (out_size) *out_size = (size_t)n;
    return 1;
}

/* Program MARK_BYTE at (MARK_PAGE, MARK_COL) through the real NAND command
 * interface, then persist the image back to path. */
static int nand_write_and_save(const char *path) {
    char err[256] = {0};
    smc_t *s = smc_create();
    int ok = 0;
    if (!s) return 0;
    if (smc_load_file(s, path, err, sizeof(err))) {
        smc_command_w(s, 0xffu);           /* reset */
        smc_command_w(s, 0x80u);           /* serial data input / program */
        smc_address_w(s, MARK_COL);        /* column cycle */
        smc_address_w(s, (uint8_t)(MARK_PAGE & 0xffu));
        smc_address_w(s, (uint8_t)(MARK_PAGE >> 8));
        smc_data_w(s, MARK_BYTE);
        smc_command_w(s, 0x10u);           /* program confirm */
        CHECK(smc_is_dirty(s), "NAND program must mark the image dirty");
        ok = smc_save_file(s, path, err, sizeof(err));
    }
    smc_destroy(s);
    return ok;
}

/* Read one NAND byte back through the real read command interface. */
static int nand_read_byte(const char *path, uint32_t page, uint32_t col, uint8_t *out) {
    char err[256] = {0};
    smc_t *s = smc_create();
    int ok = 0;
    if (!s) return 0;
    if (smc_load_file(s, path, err, sizeof(err))) {
        smc_command_w(s, 0xffu);
        smc_command_w(s, 0x00u);           /* read mode, pointer A */
        smc_address_w(s, (uint8_t)col);
        smc_address_w(s, (uint8_t)(page & 0xffu));
        smc_address_w(s, (uint8_t)(page >> 8));
        *out = smc_data_r(s);
        ok = 1;
    }
    smc_destroy(s);
    return ok;
}

/* Dump the mounted image out of the live emulator through the public API. */
static int dump_mounted_image(const char *probe) {
    return emu && gp32_save_smartmedia(emu, probe) == GP32_OK;
}

static void start_core(void) {
    retro_deinit();
    memset(system_dir, 0, sizeof(system_dir));
    memset(save_dir, 0, sizeof(save_dir));
    memset(content_dir, 0, sizeof(content_dir));
    memset(content_path, 0, sizeof(content_path));
    memset(smartmedia_save_path, 0, sizeof(smartmedia_save_path));
    retro_set_environment(test_environ);
    retro_init();
}


int main(int argc, char **argv) {
    snprintf(test_root, sizeof(test_root), "%s", argc > 1 ? argv[1] : "libretro_persistence_tmp");
    join_path(test_system_dir, sizeof(test_system_dir), test_root, "system");
    join_path(test_save_dir, sizeof(test_save_dir), test_root, "saves");
    join_path(test_content_dir, sizeof(test_content_dir), test_root, "content");
    make_dir(test_root); make_dir(test_system_dir); make_dir(test_save_dir); make_dir(test_content_dir);
    char bios[4096], card[4096], other[4096], saved[4096], legacy[4096], modified[4096], probe[4096];
    join_path(bios, sizeof(bios), test_system_dir, "gp32.bin");
    join_path(card, sizeof(card), test_content_dir, "delta.smc");
    join_path(other, sizeof(other), test_content_dir, "other.smc");
    join_path(saved, sizeof(saved), test_save_dir, "delta.gp32.sav");
    join_path(legacy, sizeof(legacy), test_save_dir, "delta.gp32.smc");
    join_path(modified, sizeof(modified), test_root, "modified.smc");
    join_path(probe, sizeof(probe), test_root, "probe.smc");
    remove(saved); remove(legacy);
    size_t bytes = 528u * 8192u;
    uint8_t *image = malloc(bytes);
    if (!image) return 2;
    memset(image, 0xff, bytes);
    CHECK(write_file(bios, image, 64), "write stub BIOS");
    CHECK(write_file(card, image, bytes) && write_file(other, image, bytes) && write_file(modified, image, bytes), "write cards");
    CHECK(nand_write_and_save(modified), "program save cell through real NAND commands");
    struct retro_game_info game = {card, NULL, 0, NULL};
    struct retro_game_info game_mem = {card, image, bytes, NULL};
    struct retro_game_info game_other = {other, NULL, 0, NULL};
    start_core();
    CHECK(retro_load_game(&game), "initial file load");
    size_t state_size = retro_serialize_size();
    void *state = malloc(state_size);
    CHECK(state && retro_serialize(state, state_size), "snapshot before save write");
    CHECK(gp32_load_smartmedia_over_base(emu, modified) == GP32_OK, "mount NAND-written live image over original");
    retro_unload_game();
    uint8_t compact[2048], cell = 0;
    size_t compact_size = 0;
    CHECK(read_file(saved, compact, sizeof(compact), &compact_size) && compact_size < 1024, "one changed page has a small persistent save");
    CHECK(nand_read_byte(card, MARK_PAGE, MARK_COL, &cell) && cell == 0xff, "source card remains immutable");
    CHECK(retro_load_game(&game), "reopen saved game by original filename");
    CHECK(dump_mounted_image(probe) && nand_read_byte(probe, MARK_PAGE, MARK_COL, &cell) && cell == MARK_BYTE, "persistent delta restores NAND bytes");
    CHECK(retro_serialize_size() == state_size && retro_unserialize(state, state_size), "earlier slot restores across sessions");
    CHECK(dump_mounted_image(probe) && nand_read_byte(probe, MARK_PAGE, MARK_COL, &cell) && cell == 0xff, "slot rolls back card changes");
    retro_unload_game();
    CHECK(write_file(saved, compact, compact_size), "restore the captured delta for buffer loading");
    CHECK(retro_load_game(&game_mem), "buffer frontend mounts same delta");
    CHECK(dump_mounted_image(probe) && nand_read_byte(probe, MARK_PAGE, MARK_COL, &cell) && cell == MARK_BYTE, "buffer frontend sees progress");
    retro_unload_game();

    uint8_t bad[97]; memset(bad, 0x11, sizeof(bad));
    CHECK(write_file(saved, bad, sizeof(bad)), "install corrupt save fixture");
    CHECK(!retro_load_game(&game), "corrupt save must not fall back to direct boot");
    retro_unload_game();
    uint8_t readback[2048]; size_t readback_size;
    CHECK(read_file(saved, readback, sizeof(readback), &readback_size) && readback_size == sizeof(bad) && !memcmp(readback, bad, sizeof(bad)), "failed load leaves corrupt save untouched");
    CHECK(write_file(saved, compact, compact_size), "restore delta fixture");
    image[100] = 0x33;
    CHECK(write_file(card, image, bytes), "change base image fixture");
    CHECK(!retro_load_game(&game), "wrong original is rejected");
    retro_unload_game();
    image[100] = 0xff;
    CHECK(write_file(card, image, bytes), "restore correct original");

    remove(saved);
    CHECK(write_file(legacy, image, bytes) && nand_write_and_save(legacy), "create old full-image save");
    CHECK(retro_load_game(&game), "legacy save imports");
    CHECK(file_exists(legacy), "legacy retained until new save commits");
    CHECK(dump_mounted_image(probe) && nand_read_byte(probe, MARK_PAGE, MARK_COL, &cell) && cell == MARK_BYTE, "legacy progress loaded");
    CHECK(retro_load_game(&game_other), "game replacement flushes previous card");
    CHECK(file_exists(saved) && !file_exists(legacy), "successful migration removes only consumed legacy");
    CHECK(read_file(saved, readback, sizeof(readback), &readback_size) && readback_size < 1024, "legacy conversion is compact");
    retro_unload_game();
    CHECK(retro_load_game(&game), "reload migrated save");
    join_path(smartmedia_save_path, sizeof(smartmedia_save_path), saved, "blocked.sav");
    unsigned logs = error_logs, messages = user_messages;
    retro_unload_game();
    CHECK(error_logs == logs + 1 && user_messages == messages + 1, "write failure is reported");
    CHECK(read_file(saved, readback, sizeof(readback), &readback_size) && readback_size == compact_size && !memcmp(readback, compact, compact_size), "write failure preserves previous save");
    retro_deinit();
    free(state); free(image);
    printf("Persistent page saves: %s (%d failures), %zu-byte card -> %zu-byte save\n", failures ? "FAIL" : "PASS", failures, bytes, compact_size);
    return failures ? 1 : 0;
}
