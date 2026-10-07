/* Atomic replacement regressions for gp32_save_state and smc_save_file.
 *
 * Both writers used to open the destination with "wb" first, so a failed
 * write or close of the new image left the previous file truncated. They now stage
 * into an exclusively created sibling file, publish it with one rename, and
 * never delete a destination or an unrelated "<path>.tmp" they did not create.
 * Synthetic fixtures, no ROM or card image. */
#include "gp32emu/gp32.h"
#include "smartmedia.h"
#include "save_atomic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#if defined(_WIN32)
#include <direct.h>
#define TEST_STAT(p, st) stat((p), (st))
#define TEST_MKDIR(p) _mkdir(p)
#define TEST_RMDIR(p) _rmdir(p)
#else
#include <unistd.h>
#define TEST_STAT(p, st) stat((p), (st))
#define TEST_MKDIR(p) mkdir((p), 0777)
#define TEST_RMDIR(p) rmdir(p)
#endif

static int failures;
#define CHECK(c, msg) do { if (!(c)) { fprintf(stderr, "FAIL: %s (%d)\n", (msg), __LINE__); ++failures; } } while (0)

static int path_exists(const char *path) {
    struct stat st;
    return path && path[0] && TEST_STAT(path, &st) == 0;
}

static uint8_t *read_all(const char *path, size_t *size) {
    *size = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n <= 0) { fclose(f); return NULL; }
    rewind(f);
    uint8_t *buf = (uint8_t *)malloc((size_t)n);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *size = (size_t)n;
    return buf;
}

static int write_all(const char *path, const void *data, size_t size) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    int ok = (fwrite(data, 1, size, f) == size);
    if (fclose(f) != 0) ok = 0;
    return ok;
}

static int bytes_still_are(const char *path, const void *data, size_t size) {
    size_t got = 0;
    uint8_t *buf = read_all(path, &got);
    int ok = buf && got == size && memcmp(buf, data, size) == 0;
    free(buf);
    return ok;
}

/* Two writers staging the same destination must never share a file, and an
 * abort must delete only the stage it created. */
static void check_stage_isolation(const char *base) {
    char path[SAVE_ATOMIC_MAX_PATH];
    snprintf(path, sizeof(path), "%s.isolation", base);
    remove(path);
    CHECK(write_all(path, "old", 3), "create previous save");

    save_atomic_t a = {0}, b = {0};
    CHECK(save_atomic_begin(&a, path, NULL, 0), "begin stage A");
    CHECK(save_atomic_begin(&b, path, NULL, 0), "begin stage B");
    if (!a.active || !b.active) { save_atomic_abort(&a); save_atomic_abort(&b); return; }

    char a_path[SAVE_ATOMIC_MAX_PATH], b_path[SAVE_ATOMIC_MAX_PATH];
    snprintf(a_path, sizeof(a_path), "%s", a.tmp_path);
    snprintf(b_path, sizeof(b_path), "%s", b.tmp_path);
    CHECK(strcmp(a_path, b_path) != 0, "stages use distinct names");
    CHECK(path_exists(a_path) && path_exists(b_path), "both stages exist");
    CHECK(fwrite("A", 1, 1, a.file) == 1 && fwrite("B", 1, 1, b.file) == 1, "write both stages");

    save_atomic_abort(&a);
    CHECK(!path_exists(a_path), "abort removes its own stage");
    CHECK(path_exists(b_path), "abort leaves the other stage alone");
    save_atomic_abort(&b);
    CHECK(!path_exists(b_path), "abort removes the second stage");
    CHECK(bytes_still_are(path, "old", 3), "aborted partial writes preserve previous save bytes");
    remove(path);
}

/* A commit that cannot replace the destination must delete only its own stage
 * and leave the destination untouched. */
static void check_commit_failure(const char *base) {
    char path[SAVE_ATOMIC_MAX_PATH];
    snprintf(path, sizeof(path), "%s.dir", base);
    remove(path); TEST_RMDIR(path);

    save_atomic_t st = {0};
    CHECK(save_atomic_begin(&st, path, NULL, 0), "begin stage over destination");
    if (!st.active) return;
    char stage_path[SAVE_ATOMIC_MAX_PATH];
    snprintf(stage_path, sizeof(stage_path), "%s", st.tmp_path);
    CHECK(fwrite("new", 1, 3, st.file) == 3, "write stage");

    CHECK(TEST_MKDIR(path) == 0, "make replace-blocking destination directory");
    CHECK(!save_atomic_commit(&st, path, NULL, 0), "commit onto a directory fails");
    CHECK(!path_exists(stage_path), "failed commit removes its stage");
    CHECK(TEST_RMDIR(path) == 0, "destination directory untouched");
}

static void check_gp32_state_atomic(const char *base) {
    char path[SAVE_ATOMIC_MAX_PATH];
    char unrelated[SAVE_ATOMIC_MAX_PATH];
    static const char sentinel[] = "unrelated .tmp payload";
    snprintf(path, sizeof(path), "%s.gp32st", base);
    snprintf(unrelated, sizeof(unrelated), "%s.tmp", path);
    remove(path); TEST_RMDIR(path); remove(unrelated); TEST_RMDIR(unrelated);
    CHECK(write_all(unrelated, sentinel, sizeof(sentinel)), "create unrelated .tmp");

    gp32_t *g = gp32_create(NULL);
    CHECK(g != NULL, "create machine");
    if (!g) return;

    /* First save exercises the replace path with no previous destination. */
    CHECK(gp32_save_state(g, path) == GP32_OK, "initial state save");
    size_t original_size = 0;
    uint8_t *original = read_all(path, &original_size);
    CHECK(original != NULL && original_size > 0, "read initial state file");
    CHECK(bytes_still_are(unrelated, sentinel, sizeof(sentinel)), "successful state save leaves unrelated .tmp");

    /* An existing directory cannot be replaced, so the save must fail without
     * touching the destination or the unrelated .tmp. */
    remove(path);
    CHECK(TEST_MKDIR(path) == 0, "state destination directory");
    CHECK(gp32_save_state(g, path) != GP32_OK, "failed state replacement reports failure");
    CHECK(TEST_RMDIR(path) == 0, "state destination directory untouched");
    CHECK(bytes_still_are(unrelated, sentinel, sizeof(sentinel)), "failed state save leaves unrelated .tmp");

    /* The same machine serializes to the same bytes, so replacing an existing
     * file must not alter the on-disk format. */
    CHECK(original && write_all(path, original, original_size), "restore previous state before replacement");
    CHECK(gp32_save_state(g, path) == GP32_OK, "state save replaces existing file");
    size_t again_size = 0;
    uint8_t *again = read_all(path, &again_size);
    CHECK(original && again && again_size == original_size &&
          memcmp(original, again, original_size) == 0,
          "state replacement preserves bytes");
    CHECK(bytes_still_are(unrelated, sentinel, sizeof(sentinel)), "replaced state save leaves unrelated .tmp");

    free(again);
    free(original);
    remove(path);
    remove(unrelated);
    gp32_destroy(g);
}

static void check_smc_atomic(const char *base) {
    char path[SAVE_ATOMIC_MAX_PATH];
    char unrelated[SAVE_ATOMIC_MAX_PATH];
    static const char sentinel[] = "unrelated .tmp payload";
    snprintf(path, sizeof(path), "%s.smc", base);
    snprintf(unrelated, sizeof(unrelated), "%s.tmp", path);
    remove(path); TEST_RMDIR(path); remove(unrelated); TEST_RMDIR(unrelated);
    CHECK(write_all(unrelated, sentinel, sizeof(sentinel)), "create unrelated smc .tmp");

    /* Raw 528-byte-page card: 4 pages of erased NAND. */
    uint8_t image[4u * 528u];
    memset(image, 0xff, sizeof(image));
    smc_t *s = smc_create();
    char err[160] = {0};
    CHECK(s && smc_load_buffer(s, image, sizeof(image), err, sizeof(err)), "load raw card");
    if (!s) return;

    CHECK(smc_save_file(s, path, err, sizeof(err)), "initial smc save");
    CHECK(!smc_is_dirty(s), "card clean after initial save");
    CHECK(bytes_still_are(unrelated, sentinel, sizeof(sentinel)), "successful smc save leaves unrelated .tmp");

    /* Program page 0 byte 0 to 0x00 and keep the change pending. */
    smc_command_w(s, 0x80);
    smc_address_w(s, 0x00);
    smc_address_w(s, 0x00);
    smc_address_w(s, 0x00);
    smc_data_w(s, 0x00);
    smc_command_w(s, 0x10);
    CHECK(smc_is_dirty(s), "program marks card dirty");

    size_t original_size = 0;
    uint8_t *original = read_all(path, &original_size);
    CHECK(original != NULL && original_size == sizeof(image), "read initial smc file");

    remove(path);
    CHECK(TEST_MKDIR(path) == 0, "smc destination directory");
    CHECK(!smc_save_file(s, path, err, sizeof(err)), "failed smc replacement reports failure");
    CHECK(smc_is_dirty(s), "failed smc save keeps card dirty");
    CHECK(TEST_RMDIR(path) == 0, "smc destination directory untouched");
    CHECK(bytes_still_are(unrelated, sentinel, sizeof(sentinel)), "failed smc save leaves unrelated .tmp");
    CHECK(original && write_all(path, original, original_size), "restore previous card before replacement");
    free(original);

    CHECK(smc_save_file(s, path, err, sizeof(err)), "smc save replaces existing file");
    CHECK(!smc_is_dirty(s), "card clean after successful re-save");
    size_t saved_size = 0;
    uint8_t *saved = read_all(path, &saved_size);
    CHECK(saved && saved_size == sizeof(image) && saved[0] == 0x00,
          "re-save persists the programmed card");
    CHECK(bytes_still_are(unrelated, sentinel, sizeof(sentinel)), "re-saved smc leaves unrelated .tmp");
    free(saved);

    remove(path);
    remove(unrelated);
    smc_destroy(s);
}

int main(int argc, char **argv) {
    const char *base = argc > 1 ? argv[1] : "atomic-save";
    check_stage_isolation(base);
    check_commit_failure(base);
    check_gp32_state_atomic(base);
    check_smc_atomic(base);
    printf("Atomic save replacement: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
