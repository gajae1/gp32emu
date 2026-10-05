/* Cross-session SmartMedia savestate regression (v0014).
 *
 * A state stores only the NAND pages that differ from the image the frontend
 * passed, so it stays small. That image is the stable one: the core persists
 * guest writes to <save dir>/<rom>.gp32.smc, and every later session mounts
 * that persisted card *over* the frontend content. The loaded state must then
 * reproduce the save-time card exactly, including undoing pages the guest wrote
 * after the state was saved.
 *
 * Synthetic cards only; no ROM needed. GP32_SOURCE can select a saved
 * pre-change source for the same fixture. */
#ifndef GP32_SOURCE
#define GP32_SOURCE "../src/gp32.c"
#endif
#include GP32_SOURCE

#define CARD_PAGES 8192u
#define CARD_PAGE_BYTES 528u
#define CARD_BYTES ((size_t)CARD_PAGES * CARD_PAGE_BYTES)

static int failures;
#define CHECK(c, msg) do { if (!(c)) { fprintf(stderr, "FAIL: %s (%d)\n", (msg), __LINE__); ++failures; } } while (0)

/* Deterministic card image whose pages are never all 0xff, so an erase and a
 * program both change what a page holds. */
static void fill_card(uint8_t *image) {
    for (size_t i = 0; i < CARD_BYTES; ++i) image[i] = (uint8_t)((i * 31u) ^ (i >> 9) ^ 0x5au);
    for (size_t page = 0; page < CARD_PAGES; ++page) image[page * CARD_PAGE_BYTES] = 0x5au;
}

static void write_file_bytes(const char *path, const uint8_t *data, size_t size) {
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(data, 1, size, f) != size || fclose(f) != 0) {
        fprintf(stderr, "FAIL: write %s\n", path);
        ++failures;
    }
}

/* NAND command interface over the GPB smart-media pins. Every operation below
 * re-drives the lines because a state load restores the guest's GPIO values. */
static void nand_select(gp32_t *g) {
    s3c2400_write32(g->soc, 0x15600008u, 0x1u);
    s3c2400_write32(g->soc, 0x15600030u, 0x8u);
    s3c2400_write32(g->soc, 0x15600024u, 0x0u);
}

static void nand_send(gp32_t *g, unsigned latch, uint8_t value) {
    s3c2400_write32(g->soc, 0x15600008u, 0x1u);
    s3c2400_write32(g->soc, 0x15600030u, latch | 0x8u);
    s3c2400_write32(g->soc, 0x1560000cu, value);
    s3c2400_write32(g->soc, 0x15600030u, latch);
    s3c2400_write32(g->soc, 0x15600030u, latch | 0x8u);
}

static void nand_erase(gp32_t *g, uint32_t page) {
    nand_select(g);
    nand_send(g, 0x20u, 0x60u);
    nand_send(g, 0x10u, (uint8_t)page);
    nand_send(g, 0x10u, (uint8_t)(page >> 8));
    nand_send(g, 0x20u, 0xd0u);
}

static void nand_program(gp32_t *g, uint32_t page, const uint8_t *bytes, size_t count) {
    nand_select(g);
    nand_send(g, 0x20u, 0x80u);
    nand_send(g, 0x10u, 0u);
    nand_send(g, 0x10u, (uint8_t)page);
    nand_send(g, 0x10u, (uint8_t)(page >> 8));
    for (size_t i = 0; i < count; ++i) nand_send(g, 0x00u, bytes[i]);
    nand_send(g, 0x20u, 0x10u);
}

static uint8_t nand_read(gp32_t *g, uint32_t page, uint32_t column) {
    arm_bus_t bus = s3c2400_get_bus(g->soc);
    nand_select(g);
    nand_send(g, 0x20u, 0x00u);
    nand_send(g, 0x10u, (uint8_t)column);
    nand_send(g, 0x10u, (uint8_t)page);
    nand_send(g, 0x10u, (uint8_t)(page >> 8));
    s3c2400_write32(g->soc, 0x15600030u, 0x8u);
    s3c2400_write32(g->soc, 0x15600024u, 0x140u);
    s3c2400_write32(g->soc, 0x15600008u, 0x0u);
    s3c2400_write32(g->soc, 0x15600024u, 0x40u);
    uint8_t value = bus.read8(bus.user, 0x1560000cu);
    s3c2400_write32(g->soc, 0x15600024u, 0x140u);
    return value;
}

/* Whole-machine state image, or NULL. */
static uint8_t *capture(gp32_t *g, size_t *size) {
    *size = gp32_state_size(g);
    uint8_t *buf = (uint8_t *)malloc(*size ? *size : 1u);
    if (!buf || !*size || gp32_save_state_data(g, buf, *size) != GP32_OK) { free(buf); *size = 0; return NULL; }
    return buf;
}

static gp32_t *machine_with_card(const uint8_t *image, size_t size) {
    gp32_t *g = gp32_create(NULL);
    if (!g) return NULL;
    if (gp32_load_smartmedia_data(g, image, size) != GP32_OK) { gp32_destroy(g); return NULL; }
    return g;
}

static int files_equal(const char *a, const char *b) {
    FILE *fa = fopen(a, "rb");
    FILE *fb = fopen(b, "rb");
    if (!fa || !fb) {
        if (fa) fclose(fa);
        if (fb) fclose(fb);
        return 0;
    }
    int equal = 1;
    for (;;) {
        uint8_t ba[4096], bb[4096];
        size_t na = fread(ba, 1, sizeof(ba), fa);
        size_t nb = fread(bb, 1, sizeof(bb), fb);
        if (na != nb || memcmp(ba, bb, na) != 0) { equal = 0; break; }
        if (na < sizeof(ba)) break;
    }
    fclose(fa);
    fclose(fb);
    return equal;
}

int main(int argc, char **argv) {
    const char *content_path = argc > 2 ? argv[1] : "savestate-session-content.smc";
    const char *saved_path = argc > 2 ? argv[2] : "savestate-session-saved.smc";
    const char *dumped_path = argc > 3 ? argv[3] : "savestate-session-dumped.smc";
    const uint8_t save_byte = 0x5au;    /* written before the state is saved */
    const uint8_t after_byte = 0x3cu;   /* written after the state is saved */

    uint8_t *content = (uint8_t *)malloc(CARD_BYTES);
    if (!content) return 2;
    fill_card(content);
    write_file_bytes(content_path, content, CARD_BYTES);
    /* Fixed part of every state plus the whole-image SmartMedia section: what
     * this game would serialize to without the delta form. */
    gp32_t *empty = gp32_create(NULL);
    size_t empty_size = empty ? gp32_state_size(empty) : 0;
    if (empty) gp32_destroy(empty);
    CHECK(empty_size != 0, "a card-less machine still reports a state size");
    const size_t full_expected = empty_size + CARD_BYTES + CARD_PAGE_BYTES;

    /* ---- Session 1: mount the frontend content, save a state, then write. ---- */
    gp32_t *s1 = machine_with_card(content, CARD_BYTES);
    if (!s1) return 2;
    nand_erase(s1, 33u);
    nand_program(s1, 33u, &save_byte, 1u);
    CHECK(nand_read(s1, 33u, 0u) == save_byte, "session 1 save byte reads back");
    size_t state_size = 0;
    uint8_t *state = capture(s1, &state_size);
    CHECK(state != NULL, "capture session 1 state");
    if (!state) return 2;
    /* The delta section must stand in for most of the 4.1 MiB image. */
    CHECK(state_size + CARD_BYTES / 2u < full_expected, "state carries only the differing pages");
    CHECK(!memcmp(state, "GP32STATEv0014", 14u), "v14 writer");
    /* A state saved before any guest write is the same size: one mounted card
     * always serializes to one size. */
    gp32_t *probe = machine_with_card(content, CARD_BYTES);
    CHECK(probe != NULL, "probe machine with pristine card");
    if (probe) {
        CHECK(gp32_state_size(probe) == state_size, "payload size does not depend on the written pages");
        gp32_destroy(probe);
    }

    /* The guest keeps playing and writes its save data to the card. */
    nand_erase(s1, 49u);
    nand_program(s1, 49u, &after_byte, 1u);
    CHECK(gp32_save_smartmedia(s1, saved_path) == GP32_OK, "core persists the written card");
    CHECK(gp32_state_size(s1) == state_size, "payload stays constant while the card is written");
    gp32_destroy(s1);

    /* ---- Session 2: content plus the persisted card, then load the state. ---- */
    gp32_t *s2 = gp32_create(NULL);
    CHECK(s2 != NULL, "create session 2");
    if (s2) {
        CHECK(gp32_set_smartmedia_state_base_file(s2, content_path) == GP32_OK, "content becomes the state base");
        CHECK(gp32_load_smartmedia_over_base(s2, saved_path) == GP32_OK, "persisted card mounts over the base");
        CHECK(nand_read(s2, 33u, 0u) == save_byte, "mounted card holds the earlier write");
        CHECK(nand_read(s2, 49u, 0u) == after_byte, "mounted card holds the post-save write");
        CHECK(gp32_load_state_data(s2, state, state_size) == GP32_OK,
              "session 1 state loads in a later session after the card was written");
        size_t again_size = 0;
        uint8_t *again = capture(s2, &again_size);
        CHECK(again && again_size == state_size && !memcmp(again, state, state_size),
              "loaded machine serializes byte identically");
        CHECK(nand_read(s2, 33u, 0u) == save_byte, "loaded card keeps the save-time write");
        CHECK(nand_read(s2, 49u, 0u) == content[49u * CARD_PAGE_BYTES], "loaded card undoes the post-save write");
        free(again);
        gp32_destroy(s2);
    }

    /* ---- Session 3: the state alone must reconstruct on the content image. ---- */
    gp32_t *s3 = machine_with_card(content, CARD_BYTES);
    CHECK(s3 != NULL, "create session 3");
    if (s3) {
        CHECK(gp32_load_state_data(s3, state, state_size) == GP32_OK, "state loads on the pristine content alone");
        size_t again_size = 0;
        uint8_t *again = capture(s3, &again_size);
        CHECK(again && again_size == state_size && !memcmp(again, state, state_size),
              "content-only session reproduces the same machine");
        free(again);
        gp32_destroy(s3);
    }

    /* ---- A different content image cannot supply the pages the state omits. ---- */
    /* Warped copy of the content, also reused below as a foreign card. */
    uint8_t *other = (uint8_t *)malloc(CARD_BYTES);
    CHECK(other != NULL, "allocate a different content image");
    if (other) {
        memcpy(other, content, CARD_BYTES);
        other[100u * CARD_PAGE_BYTES + 200u] ^= 0x5au;
        gp32_t *s4 = machine_with_card(other, CARD_BYTES);
        CHECK(s4 != NULL, "create session 4");
        if (s4) {
            size_t live_size = 0;
            uint8_t *live = capture(s4, &live_size);
            CHECK(gp32_load_state_data(s4, state, state_size) != GP32_OK, "a different content image refuses the delta");
            size_t after_size = 0;
            uint8_t *after = capture(s4, &after_size);
            CHECK(live && after && after_size == live_size && !memcmp(live, after, live_size),
                  "refused load leaves the machine byte identical");
            free(after);
            free(live);
            gp32_destroy(s4);
        }
    }

    /* ---- A card the guest rewrites past the budget keeps the full section. ---- */
    gp32_t *s5 = machine_with_card(content, CARD_BYTES);
    CHECK(s5 != NULL, "create session 5");
    if (s5) {
        /* 16 pages per erase block; the session budget is 2048 entries. */
        for (uint32_t block = 0; block < 130u; ++block) nand_erase(s5, (block + 1u) * 16u);
        CHECK(gp32_state_size(s5) == full_expected,
              "a rewrite past the budget falls back to the full image payload");
        size_t big_size = 0;
        uint8_t *big = capture(s5, &big_size);
        CHECK(big != NULL, "capture the full-image state");
        if (big) {
            CHECK(gp32_save_smartmedia(s5, saved_path) == GP32_OK, "persist the rewritten card");
            gp32_t *s6 = gp32_create(NULL);
            CHECK(s6 != NULL, "create session 6");
            if (s6) {
                /* A full-image state needs no base: even a different card can
                 * be mounted and the state still reconstructs exactly. */
                CHECK(other && gp32_load_smartmedia_data(s6, other, CARD_BYTES) == GP32_OK, "mount a different card");
                CHECK(gp32_load_state_data(s6, big, big_size) == GP32_OK, "full-image state loads over a different card");
                CHECK(gp32_get_cycles(s6) == gp32_get_cycles(s5), "restored machine keeps the CPU timeline");
                size_t again_size = 0;
                uint8_t *again = capture(s6, &again_size);
                /* This session holds a different base, so its own plan may pick
                 * the delta form again; the payload must never exceed what the
                 * same machine cost before v0014, and the card must be exact. */
                CHECK(again && again_size <= full_expected, "restored session never exceeds the pre-delta payload");
                CHECK(gp32_save_smartmedia(s6, dumped_path) == GP32_OK, "dump the restored card");
                CHECK(files_equal(saved_path, dumped_path), "restored card matches the saved card byte for byte");
                free(again);
                gp32_destroy(s6);
            }
            free(big);
        }
        gp32_destroy(s5);
    }
    free(other);

    /* ---- v0012 full-image streams keep loading and replace the card. ---- */
    {
        const uint32_t small_pages = 512u;
        const size_t small_bytes = (size_t)small_pages * CARD_PAGE_BYTES;
        uint8_t *small = (uint8_t *)malloc(small_bytes);
        CHECK(small != NULL, "allocate a small card");
        if (small) {
            for (size_t i = 0; i < small_bytes; ++i) small[i] = (uint8_t)(0x11u ^ (i * 7u));
            gp32_t *legacy_src = machine_with_card(small, small_bytes);
            CHECK(legacy_src != NULL, "create the legacy writer");
            if (legacy_src) {
                size_t legacy_size = 0;
                uint8_t *legacy = capture(legacy_src, &legacy_size);
                CHECK(legacy != NULL, "capture the small-card state");
                if (legacy) {
                    /* The full form of v0014 is byte-identical to the older
                     * layout, so only the magic distinguishes them. */
                    memcpy(legacy, "GP32STATEv0012", 14u);
                    gp32_t *target = machine_with_card(content, CARD_BYTES);
                    CHECK(target != NULL, "create the legacy target");
                    if (target) {
                        CHECK(gp32_load_state_data(target, legacy, legacy_size) == GP32_OK, "v0012 full-image state still loads");
                        CHECK(nand_read(target, 3u, 0u) == small[3u * CARD_PAGE_BYTES], "legacy state card reads back");
                        gp32_destroy(target);
                    }
                    free(legacy);
                }
                gp32_destroy(legacy_src);
            }
            free(small);
        }
    }

    remove(content_path);
    remove(saved_path);
    remove(dumped_path);
    free(state);
    free(content);
    if (failures) { printf("savestate_session: FAIL\n"); return 1; }
    puts("PASS: cross-session SmartMedia savestates");
    return 0;
}
