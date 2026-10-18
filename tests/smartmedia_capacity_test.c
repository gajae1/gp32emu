/* A supported 128 MiB data card has another 4 MiB of NAND spare bytes.
 * Its own saved state and compact persistent save must both load again.
 * The image is synthetic; the test uses real public mount/command/save APIs. */
#include "../src/smartmedia.c"

static int failures;
static const char *current_case;
#define CHECK(c, msg) do { if (!(c)) { fprintf(stderr, "FAIL[%s]: %s\n", current_case, msg); ++failures; } } while (0)

enum { CARD_PAGES = 262144u, PAGE_BYTES = 528u, BLOCK_PAGES = 32u };

static void row_address(smc_t *s, uint32_t page) {
    smc_address_w(s, (uint8_t)page);
    smc_address_w(s, (uint8_t)(page >> 8));
    smc_address_w(s, (uint8_t)(page >> 16));
}

static void erase_page_block(smc_t *s, uint32_t page) {
    smc_command_w(s, 0x60u);
    row_address(s, page);
    smc_command_w(s, 0xd0u);
}

static uint8_t read_byte(smc_t *s, uint32_t page, unsigned column) {
    smc_command_w(s, column >= 512u ? 0x50u : 0x00u);
    smc_address_w(s, (uint8_t)column);
    row_address(s, page);
    return smc_data_r(s);
}

static void program_byte(smc_t *s, uint32_t page, unsigned column, uint8_t value) {
    smc_command_w(s, column >= 512u ? 0x50u : 0x00u);
    smc_command_w(s, 0x80u);
    smc_address_w(s, (uint8_t)column);
    row_address(s, page);
    smc_data_w(s, value);
    smc_command_w(s, 0x10u);
}

static void prepare_tail(smc_t *s) {
    erase_page_block(s, CARD_PAGES - 1u);
    program_byte(s, CARD_PAGES - 1u, 0u, 0x5au);
    program_byte(s, CARD_PAGES - 1u, 527u, 0xa5u);
}

static void change_tail(smc_t *s) {
    program_byte(s, CARD_PAGES - 1u, 0u, 0u);
    program_byte(s, CARD_PAGES - 1u, 527u, 0u);
    CHECK(read_byte(s, CARD_PAGES - 1u, 0u) == 0u &&
          read_byte(s, CARD_PAGES - 1u, 527u) == 0u, "guest changed the tail before loading");
}

static void check_tail(smc_t *s) {
    CHECK(read_byte(s, CARD_PAGES - 1u, 0u) == 0x5au,
          "restored last page data byte at the highest row address");
    CHECK(read_byte(s, CARD_PAGES - 1u, 527u) == 0xa5u,
          "restored final spare byte of the image");
    CHECK(read_byte(s, CARD_PAGES - BLOCK_PAGES - 1u, 0u) == 0u,
          "unrelated preceding block remains unchanged");
}

static void check_capacity(bool header) {
    current_case = header ? "128 MiB format-2 card" : "128 MiB raw card";
    const char *path = header ? "smartmedia-capacity-header.tmp" : "smartmedia-capacity-raw.tmp";
    size_t payload_size = (size_t)CARD_PAGES * PAGE_BYTES;
    size_t image_size = payload_size + (header ? 1024u : 0u);
    uint8_t *image = calloc(1, payload_size + 1024u), *saved = NULL;
    smc_t *s = smc_create();
    FILE *full = NULL;
    char err[256] = {0};
    CHECK(image && s, "allocate largest supported synthetic card");
    if (!image || !s) goto done;
    if (!header) {
        CHECK(!smc_load_buffer(s, image, payload_size + PAGE_BYTES, err, sizeof(err)),
              "reject a raw card one page above the supported data capacity");
        smc_destroy(s);
        s = smc_create();
        CHECK(s != NULL, "reset fixture after oversized mount probe");
        if (!s) goto done;
    }
    if (header) { image[0] = 0xecu; image[1] = 0x79u; }
    int loaded = smc_load_buffer(s, image, image_size, err, sizeof(err));
    free(image); image = NULL;
    CHECK(loaded && smc_image_size(s) == payload_size, "mount supported card including its spare area");
    if (!loaded) goto done;
    smc_command_w(s, 0x90u); smc_address_w(s, 0u);
    CHECK(smc_data_r(s) == 0xecu && smc_data_r(s) == 0x79u, "mounted device identifies as the 128 MiB geometry");
    prepare_tail(s);
    check_tail(s);

    state_io_t count = state_io_counter();
    CHECK(smc_state_save_io(s, &count), "count compact state");
    CHECK(count.pos < 2u * 1024u * 1024u, "sparse writes use the compact state form");
    saved = malloc(count.pos);
    CHECK(saved != NULL, "allocate compact state");
    if (!saved) goto done;
    state_io_t io = state_io_writer(saved, count.pos);
    CHECK(smc_state_save_io(s, &io), "save compact state");
    change_tail(s);
    io = state_io_reader(saved, count.pos - 1u);
    CHECK(!smc_state_load_io(s, &io, SMC_STATE_FORMAT_V14), "truncated compact state is rejected");
    CHECK(read_byte(s, CARD_PAGES - 1u, 0u) == 0u,
          "truncated state preserves the live card");
    io = state_io_reader(saved, count.pos);
    CHECK(smc_state_load_io(s, &io, SMC_STATE_FORMAT_V14), "reload this card's complete compact state");
    check_tail(s);

    /* Keep the persistent-save fixture independent even on a failing core. */
    prepare_tail(s);
    CHECK(smc_save_changes(s, path, err, sizeof(err)), "save compact card changes");
    change_tail(s);
    CHECK(smc_load_changes(s, path, err, sizeof(err)), "reload this card's compact persistent save");
    check_tail(s);

    /* Once a real guest exceeds the session budget, the serializer must use
     * its existing full form, and the same supported geometry must still load. */
    if (!header) {
        prepare_tail(s);
        for (uint32_t page = 0u; page <= 2048u; page += BLOCK_PAGES) erase_page_block(s, page);
        count = state_io_counter();
        CHECK(smc_state_save_io(s, &count) && count.pos > payload_size, "guest writes trigger the existing full-state fallback");
        full = tmpfile();
        CHECK(full && smc_state_save(s, full), "save full card state");
        if (full) {
            change_tail(s);
            rewind(full);
            CHECK(smc_state_load(s, full), "reload this card's full state");
            check_tail(s);
            CHECK(read_byte(s, 2048u, 0u) == 0xffu, "full state restores changes outside the old delta budget");
        }
    }

    /* A scalar head one NAND page beyond the supported geometry is enough
     * to prove rejection before allocating or reading a declared large body. */
    smc_state_image_t too_large = {0};
    too_large.data_size = payload_size + PAGE_BYTES;
    too_large.page_data_size = 512u;
    too_large.page_total_size = PAGE_BYTES;
    too_large.num_pages = CARD_PAGES + 1u;
    io = state_io_reader(&too_large, sizeof(too_large));
    CHECK(!smc_state_load_io(s, &io, SMC_STATE_FORMAT_V14), "reject a state above the supported NAND capacity");
    check_tail(s);
done:
    if (full) fclose(full);
    remove(path);
    free(saved); free(image); smc_destroy(s);
}

int main(void) {
    check_capacity(false);
    check_capacity(true);
    printf("SmartMedia capacity: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures != 0;
}
