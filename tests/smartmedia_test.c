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
done:
    remove(path);
    free(actual); free(expected); free(original);
    smc_destroy(s);
}

int main(void) {
    for (unsigned header = 0; header < 2; ++header) {
        check_progress(header != 0);
        check_card(8192, 0xe3, 16, header != 0);
        check_card(16384, 0xe6, 16, header != 0);
        check_card(32768, 0x73, 32, header != 0);
    }
    printf("SmartMedia geometry: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
