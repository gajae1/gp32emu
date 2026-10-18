/* Reloading firmware through a public API must replace cached instructions
 * without requiring an undocumented reset. Synthetic instructions only. */
#include "../src/gp32.c"

static unsigned failures, cases;
#define CHECK(x, msg) do { if (!(x)) { fprintf(stderr, "FAIL: %s\n", msg); ++failures; } } while (0)

static void check_reload(unsigned engine, unsigned icache, unsigned file, const char *path) {
    gp32_t *g = gp32_create(NULL);
    uint8_t *image = malloc(0x80000u);
    CHECK(g && image, "allocate BIOS fixture");
    if (!g || !image) { free(image); gp32_destroy(g); return; }
    memset(image, 0xff, 0x80000u);
    const uint32_t code = 0x2000u, control = GP32_RAM_BASE + 0x10000u;
    gp32_st32le(image + code, 0xe3a00011u);
    gp32_st32le(image + code + 4u, 0xe12fff1cu);
    CHECK(gp32_load_bios_data(g, image, 0x80000u) == GP32_OK, "initial public BIOS load");
    arm920t_set_jit(g->cpu, engine == 2u);
    arm920t_set_trace(g->cpu, engine == 0u, NULL, NULL);
    s3c2400_write32(g->soc, control, 0xee010f10u);
    arm920t_set_reg(g->cpu, 0u, 0x70u | (icache << 12));
    arm920t_set_reg(g->cpu, 15u, control);
    CHECK(arm920t_run(g->cpu, 1u) == 1u, "set guest I-cache control");
    arm920t_set_reg(g->cpu, 12u, control);
    for (unsigned warm = 0; warm < 8u; ++warm) {
        arm920t_set_reg(g->cpu, 15u, code);
        CHECK(arm920t_run(g->cpu, 2u) == 2u && arm920t_get_reg(g->cpu, 0u) == 0x11u,
              "prime original BIOS instruction pair");
    }
#if defined(GP32EMU_CPU_PROFILE) && GP32EMU_CPU_PROFILE
    if (engine == 2u && arm920t_get_jit_enabled(g->cpu)) {
        gp32_cpu_profile_t p;
        arm920t_get_cpu_profile(g->cpu, &p);
        CHECK(p.native_arm_insns != 0u, "BIOS pair executed through native code");
    }
#endif
    gp32_st32le(image + code, 0xe3a00022u);
    uint64_t cycles = arm920t_get_cycles(g->cpu);
    if (file) {
        FILE *f = fopen(path, "wb");
        CHECK(f != NULL, "open replacement firmware");
        if (!f) { free(image); gp32_destroy(g); return; }
        CHECK(fwrite(image, 1u, code + 8u, f) == code + 8u, "write synthetic firmware");
        CHECK(fclose(f) == 0, "close synthetic firmware");
        CHECK(gp32_load_bios(g, path) == GP32_OK, "public file BIOS reload");
    } else CHECK(gp32_load_bios_data(g, image, code + 8u) == GP32_OK, "public buffer BIOS reload");
    CHECK(arm920t_get_cycles(g->cpu) == cycles && arm920t_get_pc(g->cpu) == control &&
          arm920t_get_cpsr(g->cpu) == 0xd3u, "reload preserves architectural state");
    CHECK(s3c2400_read32(g->soc, code) == 0xe3a00022u &&
          s3c2400_read32(g->soc, 0x7fffcu) == UINT32_MAX, "new firmware and erased tail");
    arm920t_set_reg(g->cpu, 0u, 0x66u);
    arm920t_set_reg(g->cpu, 15u, code);
    CHECK(arm920t_run(g->cpu, 2u) == 2u, "reload instruction retirement");
    CHECK(arm920t_get_reg(g->cpu, 0u) == 0x22u, "public reload revokes old BIOS instruction");
    CHECK(arm920t_get_pc(g->cpu) == control && arm920t_get_cycles(g->cpu) == cycles + 2u,
          "reload keeps return and cycle count");
    CHECK(gp32_load_bios_data(g, image, 0x80001u) != GP32_OK &&
          s3c2400_read32(g->soc, code) == 0xe3a00022u, "oversized buffer leaves current BIOS intact");
    free(image);
    gp32_destroy(g);
    ++cases;
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    for (unsigned engine = 0; engine < 3u; ++engine)
        for (unsigned icache = 0; icache < 2u; ++icache)
            for (unsigned file = 0; file < 2u; ++file)
                check_reload(engine, icache, file, argv[1]);
    (void)remove(argv[1]);
    printf("gp32_bios_reload_test: %u cases, %u failures\n", cases, failures);
    return failures != 0u;
}
