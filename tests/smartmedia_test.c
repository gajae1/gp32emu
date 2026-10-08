/* READID and block-erase isolation must agree for raw and format-2 cards.
 * Samsung geometry: MAME smartmed.cpp detect_geometry (E3/E6: 16 pages,
 * 73: 32 pages per erase block). No copyrighted card image is needed. */
#include "smartmedia.h"

static int failures;
#define CHECK(c, msg) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", msg); ++failures; } } while (0)

static uint8_t read_page(smc_t *s, unsigned page) {
    smc_command_w(s, 0x00);
    smc_address_w(s, 0);
    smc_address_w(s, (uint8_t)page);
    smc_address_w(s, (uint8_t)(page >> 8));
    return smc_data_r(s);
}

static void check_card(unsigned pages, uint8_t id, unsigned block_pages, bool header) {
    size_t size = (size_t)pages * 528u + (header ? 1024u : 0u);
    uint8_t *image = calloc(1, size);
    smc_t *s = smc_create();
    char err[128] = {0};
    CHECK(image && s, "allocate card");
    if (!image || !s) goto done;
    if (header) { image[0] = 0xec; image[1] = id; }
    int loaded = smc_load_buffer(s, image, size, err, sizeof(err));
    CHECK(loaded, "load card");
    if (!loaded) goto done;
    smc_command_w(s, 0x90);
    smc_address_w(s, 0);
    CHECK(smc_data_r(s) == 0xec, "Samsung manufacturer ID");
    CHECK(smc_data_r(s) == id, "capacity ID");

    /* Erase the second block, addressing its last page. Both adjacent
     * blocks must retain their zero-filled contents. */
    unsigned page = 2u * block_pages - 1u;
    smc_command_w(s, 0x60);
    smc_address_w(s, (uint8_t)page);
    smc_address_w(s, (uint8_t)(page >> 8));
    smc_command_w(s, 0xd0);
    CHECK(read_page(s, block_pages - 1u) == 0, "previous block preserved");
    CHECK(read_page(s, block_pages) == 0xff, "first erased page");
    CHECK(read_page(s, page) == 0xff, "last erased page");
    CHECK(read_page(s, 2u * block_pages) == 0, "next block preserved");
done:
    smc_destroy(s);
    free(image);
}

static void check_progress(bool header) {
    const char *path = "smartmedia-progress-test.tmp";
    size_t prefix = header ? 1024u : 0u, size = prefix + 8192u * 528u;
    uint8_t *original = calloc(1, size), *expected = NULL, *actual = NULL;
    smc_t *s = smc_create();
    char err[256] = {0};
    CHECK(original && s, "allocate persistent card");
    if (!original || !s) goto done;
    if (header) { original[0] = 0xec; original[1] = 0xe3; }
    CHECK(smc_load_buffer(s, original, size, err, sizeof(err)), "mount original");
    CHECK(smc_save_changes(s, path, err, sizeof(err)), "save unchanged card");
    CHECK(smc_load_changes(s, path, err, sizeof(err)), "reload unchanged card");
    FILE *snapshot = tmpfile();
    CHECK(snapshot && smc_state_save(s, snapshot), "capture clean card state");

    /* Erasing must persist spare bytes too, and a later program must survive
     * a fresh mount without changing any other block. */
    smc_command_w(s, 0x60);
    smc_address_w(s, 16);
    smc_address_w(s, 0);
    smc_command_w(s, 0xd0);
    smc_command_w(s, 0x00);
    smc_command_w(s, 0x80);
    smc_address_w(s, 7);
    smc_address_w(s, 18);
    smc_address_w(s, 0);
    smc_data_w(s, 0x42);
    smc_command_w(s, 0x10);
    size_t expected_size = 0, actual_size = 0;
    expected = smc_copy_image(s, &expected_size);
    CHECK(expected && expected_size == size, "copy complete written card");
    if (!expected) goto done;
    CHECK(expected[prefix + 18u * 528u + 7] == 0x42, "program fixture");
    CHECK(expected[prefix + 16u * 528u + 520] == 0xff, "erased spare fixture");
    CHECK(smc_save_changes(s, path, err, sizeof(err)), "save erased and programmed pages");
    CHECK(smc_load_buffer(s, original, size, err, sizeof(err)), "fresh original mount");
    CHECK(smc_load_changes(s, path, err, sizeof(err)), "restore page changes");
    actual = smc_copy_image(s, &actual_size);
    CHECK(actual && actual_size == size && !memcmp(actual, expected, size), "exact card reconstruction including header and spare");
    free(actual); actual = NULL;

    FILE *f = fopen(path, "r+b");
    CHECK(f != NULL, "open checksum corruption fixture");
    if (!f) goto done;
    CHECK(fseek(f, -1, SEEK_END) == 0 && fputc(0, f) != EOF, "corrupt saved body");
    fclose(f);
    CHECK(!smc_load_changes(s, path, err, sizeof(err)), "reject corrupt checksum");
    actual = smc_copy_image(s, &actual_size);
    CHECK(actual && actual_size == size && !memcmp(actual, expected, size), "rejection preserves live card");
    free(actual); actual = NULL;
    CHECK(smc_save_file(s, path, err, sizeof(err)), "export legacy full card");
    CHECK(smc_load_buffer(s, original, size, err, sizeof(err)), "reset for legacy import");
    CHECK(smc_load_changes(s, path, err, sizeof(err)), "import full card save");
    actual = smc_copy_image(s, &actual_size);
    CHECK(actual && actual_size == size && !memcmp(actual, expected, size), "legacy import exact bytes");
    if (snapshot) {
        rewind(snapshot);
        CHECK(smc_state_load(s, snapshot), "restore previously clean state");
        CHECK(smc_is_dirty(s), "state rollback must reach persistent storage");
        fclose(snapshot);
    }
done:
    remove(path);
    free(actual); free(expected); free(original);
    smc_destroy(s);
}

static void erase_block(smc_t *s, unsigned page) {
    smc_command_w(s, 0x60); smc_address_w(s, (uint8_t)page);
    smc_address_w(s, (uint8_t)(page >> 8)); smc_command_w(s, 0xd0);
}

static void check_autosave(void) {
    const char *path = "smartmedia-auto-test.tmp";
    size_t size = 8192u * 528u;
    uint8_t *original = calloc(1, size);
    smc_t *s = smc_create(), *restored = smc_create();
    char err[256] = {0};
    CHECK(original && s && restored, "allocate autosave fixture");
    if (!original || !s || !restored) goto done;
    remove(path);
    CHECK(smc_load_buffer(s, original, size, err, sizeof(err)), "autosave original");
    CHECK(smc_autosave_poll(s, path, 100, err, sizeof(err)), "initial idle poll");
    erase_block(s, 16);
    CHECK(smc_autosave_poll(s, path, 200, err, sizeof(err)), "observe card write");
    CHECK(smc_autosave_poll(s, path, 1000, err, sizeof(err)), "coalesce pending write");
    FILE *f = fopen(path, "rb"); CHECK(!f, "no early storage write"); if (f) fclose(f);
    CHECK(smc_autosave_poll(s, path, 10100, err, sizeof(err)), "start periodic save");
    /* New writes while a snapshot is pending must remain dirty after its join. */
    erase_block(s, 32);
    CHECK(smc_autosave_wait(s, err, sizeof(err)), "join background save");
    CHECK(smc_is_dirty(s), "later write survives worker completion");
    CHECK(smc_load_buffer(restored, original, size, err, sizeof(err)) &&
          smc_load_changes(restored, path, err, sizeof(err)), "reopen before emulator exit");
    CHECK(read_page(restored, 16) == 0xff && read_page(restored, 32) == 0,
          "worker captured only the original snapshot");
    CHECK(smc_save_changes(s, path, err, sizeof(err)), "exit flush includes newer writes");
    CHECK(smc_load_changes(restored, path, err, sizeof(err)) && read_page(restored, 32) == 0xff,
          "final save is never overwritten by an older worker");
    CHECK(!smc_is_dirty(s), "committed current image is clean");
    remove(path);
    CHECK(smc_autosave_poll(s, path, 50000, err, sizeof(err)) && smc_autosave_wait(s, err, sizeof(err)), "idle after commit");
    f = fopen(path, "rb"); CHECK(!f, "unchanged card causes no write"); if (f) fclose(f);
    CHECK(smc_save_changes(s, path, err, sizeof(err)), "seed previous valid save");
    erase_block(s, 48);
    CHECK(smc_autosave_poll(s, path, 50100, err, sizeof(err)), "observe retry fixture");
    CHECK(smc_autosave_poll(s, "smartmedia-auto-test.tmp/blocked", 60100, err, sizeof(err)), "start failed write");
    CHECK(!smc_autosave_wait(s, err, sizeof(err)) && smc_is_dirty(s), "failed save retains pending changes");
    CHECK(smc_load_changes(restored, path, err, sizeof(err)) && read_page(restored, 48) == 0,
          "failed save preserves previous file");
    CHECK(smc_autosave_poll(s, path, 60101, err, sizeof(err)) && smc_autosave_wait(s, err, sizeof(err)), "retry is throttled");
    CHECK(smc_is_dirty(s), "throttled retry has not cleared changes");
    CHECK(smc_autosave_poll(s, path, 70100, err, sizeof(err)) && smc_autosave_wait(s, err, sizeof(err)), "retry recovers");
    CHECK(smc_load_changes(restored, path, err, sizeof(err)) && read_page(restored, 48) == 0xff,
          "retry persists previously failed changes");
done:
    smc_destroy(s); smc_destroy(restored); free(original); remove(path);
}

int main(void) {
    check_autosave();
    for (unsigned header = 0; header < 2; ++header) {
        check_progress(header != 0);
        check_card(8192, 0xe3, 16, header != 0);
        check_card(16384, 0xe6, 16, header != 0);
        check_card(32768, 0x73, 32, header != 0);
    }
    printf("SmartMedia geometry: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
