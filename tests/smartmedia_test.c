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

int main(void) {
    for (unsigned header = 0; header < 2; ++header) {
        check_card(8192, 0xe3, 16, header != 0);
        check_card(16384, 0xe6, 16, header != 0);
        check_card(32768, 0x73, 32, header != 0);
    }
    printf("SmartMedia geometry: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
