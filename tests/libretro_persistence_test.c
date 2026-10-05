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

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s (line %d)\n", message, __LINE__); \
        ++failures; \
    } \
} while (0)

static void quiet_log(int level, const char *fmt, ...) { (void)level; (void)fmt; }

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

/* Cross-session savestates over a persisted card.
 *
 * Session 1 loads the frontend content, saves a state and quits; the guest's
 * save data lands on the card file afterwards. Session 2 mounts that persisted
 * card *over* the content the frontend passes again and must still load the
 * session-1 state, because the state is expressed against the content image and
 * not against the card that happened to be mounted when it was saved. */
#define BIG_PAGES 8192u
#define BIG_SIZE ((size_t)528u * BIG_PAGES)

static void test_state_across_sessions(void) {
    char big_rom[4096], big_save[4096], probe[4096];
    join_path(big_rom, sizeof(big_rom), test_content_dir, "rom_big.smc");
    join_path(big_save, sizeof(big_save), test_save_dir, "rom_big.gp32.smc");
    join_path(probe, sizeof(probe), test_root, "probe_big.smc");
    remove(big_save);
    uint8_t *image = (uint8_t *)malloc(BIG_SIZE);
    if (!image) { ++failures; return; }
    for (size_t i = 0; i < BIG_SIZE; ++i) image[i] = (uint8_t)((i * 31u) ^ (i >> 9) ^ 0x5au);
    for (uint32_t page = 0; page < BIG_PAGES; ++page) image[(size_t)page * 528u] = 0x5au;
    /* Blank save cell in the content: the state below is saved before the guest
     * writes it, so restoring must bring the blank value back. */
    image[(size_t)MARK_PAGE * 528u + MARK_COL] = 0xffu;
    if (!write_file(big_rom, image, BIG_SIZE)) { free(image); ++failures; return; }

    struct retro_game_info game = { big_rom, NULL, 0, NULL };
    start_core();
    CHECK(retro_load_game(&game), "big-card content load must succeed");
    size_t state_size = retro_serialize_size();
    CHECK(state_size > 0, "state size must be reported");
    void *state = malloc(state_size);
    if (!state_size || !state || !retro_serialize(state, state_size)) { free(state); free(image); ++failures; return; }
    retro_unload_game();

    /* The game writes its save data to the card after the state was saved. */
    CHECK(nand_write_and_save(big_save), "guest save write lands on the persisted card");
    CHECK(file_exists(big_save), "unload must persist the card");

    start_core();
    CHECK(retro_load_game(&game), "later session must mount the persisted card over the content");
    size_t again_size = retro_serialize_size();
    CHECK(again_size == state_size, "the same mounted card must report the same state size");
    CHECK(retro_unserialize(state, state_size), "session 1 state must load in a later session");
    void *again = malloc(again_size);
    CHECK(again && retro_serialize(again, again_size) && !memcmp(again, state, state_size < again_size ? state_size : again_size),
          "loaded machine must serialize byte identically");
    free(again);
    /* The state restores the save-time card: the write that came after it is
     * undone, exactly as a full-image state always did. */
    CHECK(dump_mounted_image(probe), "restored card dump must succeed");
    uint8_t cell = 0x5au;
    CHECK(nand_read_byte(probe, MARK_PAGE, MARK_COL, &cell) && cell == 0xffu,
          "restored card must drop the write that followed the state");
    retro_unload_game();
    retro_deinit();

    /* The same game without its content image cannot rebuild the delta base and
     * falls back to carrying the whole card: the size difference is the win. */
    char hidden[4096];
    join_path(hidden, sizeof(hidden), test_content_dir, "rom_big.hidden");
    remove(hidden);
    CHECK(rename(big_rom, hidden) == 0, "hide the content image");
    start_core();
    CHECK(retro_load_game(&game), "load without the content image still mounts the persisted card");
    size_t full_size = retro_serialize_size();
    retro_unload_game();
    retro_deinit();
    CHECK(rename(hidden, big_rom) == 0, "restore the content image");
    CHECK(full_size > state_size + BIG_SIZE / 2u, "the delta payload is far smaller than the full-image payload");

    remove(big_save);
    free(state);
    free(image);
}

int main(int argc, char **argv) {
    snprintf(test_root, sizeof(test_root), "%s", argc > 1 ? argv[1] : "libretro_persistence_tmp");
    join_path(test_system_dir, sizeof(test_system_dir), test_root, "system");
    join_path(test_save_dir, sizeof(test_save_dir), test_root, "saves");
    join_path(test_content_dir, sizeof(test_content_dir), test_root, "content");
    make_dir(test_root);
    make_dir(test_system_dir);
    make_dir(test_save_dir);
    make_dir(test_content_dir);

    char bios_path[4096], rom_a[4096], rom_b[4096], rom_c[4096];
    char save_a[4096], save_b[4096], save_c[4096], probe[4096];
    join_path(bios_path, sizeof(bios_path), test_system_dir, "gp32.bin");
    join_path(rom_a, sizeof(rom_a), test_content_dir, "rom_a.smc");
    join_path(rom_b, sizeof(rom_b), test_content_dir, "rom_b.smc");
    join_path(rom_c, sizeof(rom_c), test_content_dir, "rom_c.smc");
    join_path(save_a, sizeof(save_a), test_save_dir, "rom_a.gp32.smc");
    join_path(save_b, sizeof(save_b), test_save_dir, "rom_b.gp32.smc");
    join_path(save_c, sizeof(save_c), test_save_dir, "rom_c.gp32.smc");
    join_path(probe, sizeof(probe), test_root, "probe.smc");

    uint8_t stub_bios[64];
    memset(stub_bios, 0x5a, sizeof(stub_bios));
    if (!write_file(bios_path, stub_bios, sizeof(stub_bios))) { fprintf(stderr, "FAIL: bios write\n"); return 2; }

    uint8_t *rom = (uint8_t *)malloc(ROM_SIZE);
    if (!rom) return 2;
    memset(rom, 0xff, ROM_SIZE);
    if (!write_file(rom_a, rom, ROM_SIZE) || !write_file(rom_b, rom, ROM_SIZE) ||
        !write_file(rom_c, rom, ROM_SIZE)) { fprintf(stderr, "FAIL: rom write\n"); return 2; }

    struct retro_game_info game_a = { rom_a, NULL, 0, NULL };
    struct retro_game_info game_b = { rom_b, NULL, 0, NULL };
    struct retro_game_info game_c = { rom_c, NULL, 0, NULL };

    /* ---- Persisted image must be restored on next load. ---- */
    start_core();
    CHECK(retro_load_game(&game_a), "initial smc load must succeed");
    retro_unload_game();
    CHECK(file_exists(save_a), "unload must persist the SmartMedia image");
    CHECK(nand_write_and_save(save_a), "NAND program must write the saved image");

    CHECK(retro_load_game(&game_a), "reload must succeed with a valid save present");
    CHECK(dump_mounted_image(probe), "mounted image dump must succeed");
    uint8_t mounted = 0;
    CHECK(nand_read_byte(probe, MARK_PAGE, MARK_COL, &mounted),
          "mounted image must be readable through NAND commands");
    CHECK(mounted == MARK_BYTE,
          "reloaded session must see the persisted NAND write");
    uint8_t rom_probe = 0;
    CHECK(nand_read_byte(rom_a, MARK_PAGE, MARK_COL, &rom_probe) && rom_probe == 0xffu,
          "original media file must stay unmodified");
    retro_unload_game();

    /* The same rule applies when the frontend passes the image in memory. */
    struct retro_game_info game_a_mem = { rom_a, rom, ROM_SIZE, NULL };
    CHECK(retro_load_game(&game_a_mem), "buffer-path smc load must succeed");
    CHECK(dump_mounted_image(probe), "mounted image dump must succeed (buffer path)");
    mounted = 0;
    CHECK(nand_read_byte(probe, MARK_PAGE, MARK_COL, &mounted),
          "mounted image must be readable (buffer path)");
    CHECK(mounted == MARK_BYTE,
          "buffer-path load must also restore the persisted image");
    retro_unload_game();

    /* ---- A present-but-corrupt save must fail the load, not be replaced. ---- */
    uint8_t garbage[97];
    memset(garbage, 0x11, sizeof(garbage));
    if (!write_file(save_c, garbage, sizeof(garbage))) { fprintf(stderr, "FAIL: garbage write\n"); return 2; }
    CHECK(!retro_load_game(&game_c), "unreadable saved image must fail the load");
    retro_unload_game();
    retro_deinit();
    uint8_t after[97];
    size_t after_size = 0;
    CHECK(read_file(save_c, after, sizeof(after), &after_size) &&
          after_size == sizeof(garbage) && !memcmp(after, garbage, sizeof(garbage)),
          "failed load must leave the unreadable save file untouched");

    /* ---- Replacing a running game must flush the previous game's image. ---- */
    start_core();
    remove(save_a);
    remove(save_b);
    CHECK(retro_load_game(&game_a), "replace test first load must succeed");
    CHECK(retro_load_game(&game_b), "direct game replacement must succeed");
    CHECK(file_exists(save_a), "replaced game must flush its SmartMedia image");
    {
        char err[256] = {0};
        smc_t *s = smc_create();
        CHECK(s && smc_load_file(s, save_a, err, sizeof(err)),
              "flushed image must be a valid SmartMedia file");
        if (s) smc_destroy(s);
    }
    retro_unload_game();
    CHECK(file_exists(save_b), "unload after replacement must persist the second image");

    retro_deinit();
    test_state_across_sessions();
    free(rom);
    if (failures) {
        fprintf(stderr, "libretro persistence: %d failures\n", failures);
        return 1;
    }
    puts("PASS: save restore on reload (path+buffer), corrupt-save protection, replace flush");
    return 0;
}
