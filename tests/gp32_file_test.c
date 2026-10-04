/* SDK existing-file open must reach the asset-backed file service after
 * relocation. The fixture is a short SDK prefix, not a commercial ROM. */
#include "../src/gp32.c"

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

int main(void) {
    if (!check_open(GP32_RAM_BASE + 0x200u) || !check_open(GP32_RAM_BASE + 0x2400u)) {
        fprintf(stderr, "FAIL: relocated SDK file open/read\n");
        return 1;
    }
    puts("PASS: relocated SDK file open/read and EOF");
    return 0;
}
