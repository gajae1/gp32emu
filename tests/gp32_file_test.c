/* SDK existing-file open must reach the asset-backed file service after
 * relocation. The fixture is a short SDK prefix, not a commercial ROM. */
#include "../src/gp32.c"
#include "zip.h"

static int check_open(uint32_t code) {
    gp32_t *g = gp32_create(NULL);
    if (!g) return 0;
    static const uint32_t prefix[] = {
        0xe92d4ff0u, 0xe24dd04cu, 0xe1a06001u, 0xe1a05002u,
        0xe1a07000u, 0xeb000010u, 0xe1a04000u, 0xe3500001u,
        0x23a0000bu,
    };
    const uint32_t path = GP32_RAM_BASE + 0x10000u;
    const uint32_t handle_out = path + 0x100u, buffer = path + 0x200u;
    const char name[] = "gp:\\game\\fixture\\image.bin";
    const uint8_t payload[] = {0x10, 0x20, 0x30, 0x40, 0x50};
    fpk_asset_t asset = {0};
    strcpy(asset.path, "GAME/FIXTURE/IMAGE.BIN");
    asset.data = (uint8_t *)payload;
    asset.size = sizeof(payload);
    g->direct_fpk_assets = &asset;
    g->direct_fpk_asset_count = 1u;
    direct_set_fxe_mode(g, 1u);
    g->direct_fxe_image_end = code + 0x300u;
    for (unsigned i = 0; i < GP32_ARRAY_COUNT(prefix); ++i) {
        s3c2400_write32(g->soc, code + i * 4u, prefix[i]);
        s3c2400_write32(g->soc, code + 0x100u + i * 4u, prefix[i]);
    }
    /* A similar prologue without the SDK invalid-device return is not enough. */
    s3c2400_write32(g->soc, code + 0x120u, 0x23a00000u);
    for (unsigned i = 0; i < sizeof(name); ++i) s3c2400_write8(g->soc, path + i, (uint8_t)name[i]);
    direct_scan_file_hle(g);
    int ok = s3c2400_debug_read32(g->soc, code) == 0xef070001u &&
        s3c2400_debug_read32(g->soc, code + 0x100u) == prefix[0];
    arm920t_set_reg(g->cpu, 0, path);
    arm920t_set_reg(g->cpu, 1, 1u);
    arm920t_set_reg(g->cpu, 2, handle_out);
    arm920t_set_reg(g->cpu, 14, code + 0x200u);
    ok = ok && direct_fxe_swi(g, g->cpu, 0x070001u, code, 0);
    uint32_t handle = s3c2400_debug_read32(g->soc, handle_out);
    ok = ok && arm920t_get_reg(g->cpu, 0) == 0u && handle != 0u &&
        arm920t_get_reg(g->cpu, 15) == code + 0x200u;
    arm920t_set_reg(g->cpu, 0, handle);
    arm920t_set_reg(g->cpu, 1, buffer);
    arm920t_set_reg(g->cpu, 2, 4u);
    arm920t_set_reg(g->cpu, 3, buffer + 0x20u);
    ok = ok && direct_fxe_swi(g, g->cpu, 0x070002u, code, 0);
    ok = ok && s3c2400_debug_read32(g->soc, buffer) == 0x40302010u &&
        s3c2400_debug_read32(g->soc, buffer + 0x20u) == 4u;
    /* The next read must retain the handle cursor and stop at EOF. */
    arm920t_set_reg(g->cpu, 0, handle);
    ok = ok && direct_fxe_swi(g, g->cpu, 0x070002u, code, 0);
    ok = ok && (s3c2400_debug_read32(g->soc, buffer) & 0xffu) == 0x50u &&
        s3c2400_debug_read32(g->soc, buffer + 0x20u) == 1u;
    g->direct_fpk_assets = NULL;
    g->direct_fpk_asset_count = 0u;
    gp32_destroy(g);
    return ok;
}

static int check_card_query(void) {
    gp32_t *g = gp32_create(NULL);
    if (!g) return 0;
    const uint32_t surface = GP32_RAM_BASE + 0x1000u;
    direct_set_fxe_mode(g, 1u);
    direct_install_stubs(g);
    direct_fill_lcd_surface(g, surface, 1u);
    int ok = 1;
    for (unsigned present = 0; present < 2; ++present) {
        g->direct_fpk_asset_count = present;
        arm920t_set_reg(g->cpu, 0, surface);
        arm920t_set_reg(g->cpu, 1, 0x1234u);
        ok &= direct_fxe_swi(g, g->cpu, 0x11u, GP32_RAM_BASE + 0x200u, 0);
        ok &= arm920t_get_reg(g->cpu, 0) == present && arm920t_get_reg(g->cpu, 1) == 0x1234u &&
            g->direct_fxe_fb_addr == 0u && !g->direct_vblank_wait_requested;
    }
    g->direct_fpk_asset_count = 0u;
    arm920t_set_reg(g->cpu, 0, surface);
    ok &= direct_fxe_swi(g, g->cpu, 0x11u, direct_wait_swi_addr(g), 0);
    ok &= g->direct_fxe_fb_addr == direct_default_surface_addr(1) && g->direct_vblank_wait_requested;
    gp32_destroy(g);
    if (!ok) fputs("FAIL: card query aliases display callback\n", stderr);
    return ok;
}

static int check_raw_image_labels(void) {
    uint8_t image[32] = {0};
    gp32_st32le(image, 0xea000000u);
    gp32_st32le(image + 4u, GP32_RAM_BASE);
    for (unsigned off = 8u; off <= 16u; off += 4u)
        gp32_st32le(image + off, GP32_RAM_BASE + sizeof(image));
    char long_label[201];
    memset(long_label, 'x', sizeof(long_label) - 1u);
    long_label[sizeof(long_label) - 1u] = '\0';
    const char *labels[] = {long_label, "game.gxb", "", NULL};
    const char *titles[] = {"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "game.gxb", "raw GXB", "raw GXB"};
    int ok = 1;
    for (unsigned i = 0; i < GP32_ARRAY_COUNT(labels); ++i) {
        struct { fxe_image_t image; uint8_t guard[256]; } out;
        memset(&out, 0xa5, sizeof(out));
        char error[128];
        int loaded = fxe_load_buffer(image, sizeof(image), labels[i], &out.image, error, sizeof(error));
        ok &= loaded && strcmp(out.image.title, titles[i]) == 0 &&
              !out.image.author[0] && !out.image.was_fxe && !out.image.was_b2fxec &&
              !out.image.was_host_decrunched;
        for (unsigned j = 0; j < sizeof(out.guard); ++j) ok &= out.guard[j] == 0xa5u;
        fxe_image_free(&out.image);
    }
    if (!ok) fputs("FAIL: raw image label overwrites loader metadata\n", stderr);
    return ok;
}

/* Firmware SWI #5 hands over an image a packer decrunched into scratch RAM.
 * The image carries its own GXB header, which states the rom_start the image
 * belongs at, so the hand-off has to place it there before entering it.
 * Entering the scratch copy instead leaves every absolute address inside the
 * image pointing at packed bytes, and the guest runs off the end of RAM.
 * The two windows overlap, so both copy directions have to survive. */
static int check_swi5_place_image(uint32_t scratch, uint32_t rom_start) {
    gp32_t *g = gp32_create(NULL);
    if (!g) return 0;
    direct_set_fxe_mode(g, 1u);
    const uint32_t size_word = scratch;
    const uint32_t hdr = scratch + 4u;
    const uint32_t image_end = rom_start + 0x100u;
    const uint32_t payload = 0xe1a00000u;
    s3c2400_write32(g->soc, size_word, 0x100u);
    s3c2400_write32(g->soc, hdr + 0x00u, 0xea000006u); /* b hdr+0x20 */
    s3c2400_write32(g->soc, hdr + 0x04u, rom_start);
    s3c2400_write32(g->soc, hdr + 0x08u, image_end);
    s3c2400_write32(g->soc, hdr + 0x0cu, image_end);
    s3c2400_write32(g->soc, hdr + 0x10u, image_end);
    for (uint32_t i = 0; i < 8u; ++i) s3c2400_write32(g->soc, hdr + 0x20u + i * 4u, payload + i);
    arm920t_set_reg(g->cpu, 0, size_word);      /* image behind a four-byte length word */
    arm920t_set_reg(g->cpu, 1, rom_start | 1u); /* packer scratch value, not a stack */
    int ok = direct_fxe_swi(g, g->cpu, 0x05u, rom_start + 0x400u, 0) &&
             arm920t_get_reg(g->cpu, 15) == rom_start &&
             s3c2400_debug_read32(g->soc, rom_start) == 0xea000006u &&
             s3c2400_debug_read32(g->soc, rom_start + 0x24u) == payload + 1u &&
             arm920t_get_reg(g->cpu, 13) != (rom_start | 1u);
    gp32_destroy(g);
    if (!ok) fputs("FAIL: SWI #5 hand-off leaves the image at its scratch address\n", stderr);
    return ok;
}

/* A packed hand-off arrives with the image still at its scratch address, so the
 * initialised RW data and the ZI window only exist behind the ROM window inside
 * the blob. Firmware ROM 0x64c8 moves the data and clears the window; the
 * hand-off has to do the same or the title runs with a zeroed .data/bss. */
static int check_swi5_scatter_rw_and_zi(void) {
    gp32_t *g = gp32_create(NULL);
    if (!g) return 0;
    direct_set_fxe_mode(g, 1u);
    const uint32_t scratch = GP32_RAM_BASE + 0x8000u;
    const uint32_t rom_start = GP32_RAM_BASE + 0x100u;
    const uint32_t size_word = scratch;
    const uint32_t hdr = scratch + 4u;
    const uint32_t rom_size = 0x100u;
    const uint32_t image_end = rom_start + rom_size;
    const uint32_t rw_end = image_end + 0x40u;
    const uint32_t zi_end = rw_end + 0x80u;
    s3c2400_write32(g->soc, size_word, rom_size + 0x40u);
    s3c2400_write32(g->soc, hdr + 0x00u, 0xea000006u);
    s3c2400_write32(g->soc, hdr + 0x04u, rom_start);
    s3c2400_write32(g->soc, hdr + 0x08u, image_end);
    s3c2400_write32(g->soc, hdr + 0x0cu, image_end);
    s3c2400_write32(g->soc, hdr + 0x10u, zi_end);
    s3c2400_write32(g->soc, hdr + 0x14u, rw_end);
    s3c2400_write32(g->soc, hdr + 0x18u, zi_end);
    for (uint32_t i = 0; i < 8u; ++i) s3c2400_write32(g->soc, hdr + 0x20u + i * 4u, 0xe1a00000u + i);
    /* The decruncher leaves the initialised RW data right behind the ROM window. */
    for (uint32_t i = 0; i < 0x40u; i += 4u) s3c2400_write32(g->soc, hdr + rom_size + i, 0xdead0000u + i);
    /* Poison both destination windows so the move and the clear are visible. */
    for (uint32_t i = 0; i < zi_end - image_end; i += 4u) s3c2400_write32(g->soc, image_end + i, 0xffffffffu);
    arm920t_set_reg(g->cpu, 0, size_word);
    arm920t_set_reg(g->cpu, 1, 0u);
    int ok = direct_fxe_swi(g, g->cpu, 0x05u, rom_start + 0x400u, 0) &&
             arm920t_get_reg(g->cpu, 15) == rom_start &&
             s3c2400_debug_read32(g->soc, rom_start) == 0xea000006u &&
             s3c2400_debug_read32(g->soc, rom_start + 0x24u) == 0xe1a00001u &&
             s3c2400_debug_read32(g->soc, image_end) == 0xdead0000u &&
             s3c2400_debug_read32(g->soc, rw_end - 4u) == 0xdead003cu &&
             s3c2400_debug_read32(g->soc, rw_end) == 0u &&
             s3c2400_debug_read32(g->soc, zi_end - 4u) == 0u;
    gp32_destroy(g);
    if (!ok) fputs("FAIL: SWI #5 hand-off leaves the RW data and ZI window behind\n", stderr);
    return ok;
}

static int check_zip_entry_name(const char *path) {
    const uint8_t payload[] = {0x12, 0x34, 0x56, 0x78};
    const char *name = "a.../game.gxb";
    uint8_t *data = NULL;
    size_t size = 0;
    char selected[64], error[128];
    const char *extensions[] = {".gxb"};
    int ok = gp32_zip_read_first_matching(path, extensions, 1u, &data, &size,
                                             selected, sizeof(selected), error, sizeof(error));
    ok = ok && size == sizeof(payload) && memcmp(data, payload, size) == 0 && strcmp(selected, name) == 0;
    free(data);
    if (!ok) fputs("FAIL: ZIP skips a supported game in a dotted directory\n", stderr);
    return ok;
}

int main(int argc, char **argv) {
    if (!check_raw_image_labels()) return 1;
    /* Overlapping copy in both directions: destination below and above source. */
    if (!check_swi5_place_image(GP32_RAM_BASE + 0x100u, GP32_RAM_BASE + 0x80u)) return 1;
    if (!check_swi5_place_image(GP32_RAM_BASE + 0x100u, GP32_RAM_BASE + 0x140u)) return 1;
    if (!check_swi5_scatter_rw_and_zi()) return 1;
    if (argc > 1 && !check_zip_entry_name(argv[1])) return 1;
    if (!check_card_query()) return 1;
    if (!check_open(GP32_RAM_BASE + 0x200u) || !check_open(GP32_RAM_BASE + 0x2400u)) {
        fprintf(stderr, "FAIL: relocated SDK file open/read\n");
        return 1;
    }
    puts("PASS: relocated SDK file open/read and EOF");
    return 0;
}
