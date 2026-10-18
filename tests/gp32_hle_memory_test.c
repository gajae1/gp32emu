/* Direct-mode RAM writes must preserve bounds and revoke decoded code,
 * including scalar service outputs. No firmware or game image is needed. */
#include "../src/gp32.c"

static unsigned failures, cases;
#define CHECK(x, m) do { if (!(x)) { if (failures < 12u) fprintf(stderr, "FAIL: %s\n", m); ++failures; } } while (0)

static void check_zero_bounds(void) {
    gp32_t *g = gp32_create(NULL);
    CHECK(g != NULL, "create bounds fixture");
    if (!g) return;
    arm_bus_t bus = s3c2400_get_bus(g->soc);
    uint32_t size = (uint32_t)s3c2400_ram_size(g->soc);
    uint8_t *ram = bus.fastmem(bus.user, GP32_RAM_BASE, size, 1);
    uint8_t *expected = malloc(size);
    CHECK(ram && expected, "allocate bounds fixture");
    if (!ram || !expected) { free(expected); gp32_destroy(g); return; }
    const uint32_t lengths[] = {0u, 1u, 2u, 3u, 4u, 7u, 16u, 31u, 64u, 127u, 4096u, 524288u};
    for (unsigned i = 0; i < GP32_ARRAY_COUNT(lengths); ++i) {
        for (unsigned placement = 0; placement < 5u; ++placement) {
            uint32_t len = lengths[i], off = placement < 4u ? placement : size - len;
            memset(ram, 0xa5, size);
            memset(expected, 0xa5, size);
            memset(expected + off, 0, len);
            direct_zero_if_ram(g, GP32_RAM_BASE + off, len);
            CHECK(!memcmp(ram, expected, size), "zero preserves all bytes outside the requested span");
            ++cases;
        }
    }
    const uint32_t invalid[][2] = {
        {GP32_RAM_BASE - 1u, 2u}, {GP32_RAM_BASE + size - 1u, 2u},
        {GP32_RAM_BASE + size, 1u}, {UINT32_MAX, 2u}, {GP32_RAM_BASE, UINT32_MAX}
    };
    memset(ram, 0xa5, size);
    memset(expected, 0xa5, size);
    for (unsigned i = 0; i < GP32_ARRAY_COUNT(invalid); ++i) {
        direct_zero_if_ram(g, invalid[i][0], invalid[i][1]);
        CHECK(!memcmp(ram, expected, size), "invalid zero range is rejected atomically");
        ++cases;
    }
    free(expected);
    gp32_destroy(g);
}

static void check_zero_code(unsigned engine, unsigned instruction_cache) {
    gp32_t *g = gp32_create(NULL);
    CHECK(g != NULL, "create code fixture");
    if (!g) return;
    const uint32_t code = GP32_RAM_BASE + 0x2000u, control = code + 0x100u;
    arm920t_set_jit(g->cpu, engine == 2u);
    arm920t_set_trace(g->cpu, engine == 0u, NULL, NULL);
    s3c2400_write32(g->soc, control, 0xee010f10u); /* MCR p15,0,r0,c1,c0,0 */
    arm920t_set_reg(g->cpu, 0u, 0x70u | (instruction_cache << 12));
    arm920t_set_reg(g->cpu, 15u, control);
    CHECK(arm920t_run(g->cpu, 1u) == 1u, "set real guest cache control");
    s3c2400_write32(g->soc, code, 0xe3a00011u); /* MOV r0,#0x11 */
    s3c2400_write32(g->soc, code + 4u, 0xe12fff1cu); /* BX r12 */
    arm920t_set_reg(g->cpu, 12u, control);
    for (unsigned warm = 0; warm < 2u; ++warm) {
        arm920t_set_reg(g->cpu, 0u, 0x66u);
        arm920t_set_reg(g->cpu, 15u, code);
        CHECK(arm920t_run(g->cpu, 2u) == 2u, "prime decoded/native code");
        CHECK(arm920t_get_reg(g->cpu, 0u) == 0x11u, "original instruction executes");
    }
#if defined(GP32EMU_CPU_PROFILE) && GP32EMU_CPU_PROFILE
    if (engine == 2u && arm920t_get_jit_enabled(g->cpu)) {
        gp32_cpu_profile_t profile;
        arm920t_get_cpu_profile(g->cpu, &profile);
        CHECK(profile.native_arm_insns != 0u, "native code was primed before HLE zero");
    }
#endif
    direct_zero_if_ram(g, code, 4u);
    CHECK(s3c2400_read32(g->soc, code) == 0u, "HLE zero changed the instruction bytes");
    /* 0x00000000 is ANDEQ r0,r0,r0. With Z clear it skips and leaves r0=0x66. */
    arm920t_set_cpsr(g->cpu, 0xd3u);
    arm920t_set_reg(g->cpu, 0u, 0x66u);
    arm920t_set_reg(g->cpu, 15u, code);
    uint64_t before = arm920t_get_cycles(g->cpu);
    CHECK(arm920t_run(g->cpu, 2u) == 2u, "zeroed code retirement");
    CHECK(arm920t_get_cycles(g->cpu) == before + 2u, "zeroed code cycle count");
    CHECK(arm920t_get_reg(g->cpu, 0u) == 0x66u, "external zero revokes the old MOV");
    CHECK(arm920t_get_pc(g->cpu) == control && arm920t_get_cpsr(g->cpu) == 0xd3u, "return and flags after external zero");
    ++cases;
    gp32_destroy(g);
}

enum output_writer {
    OUTPUT_WORD, OUTPUT_BYTE, OUTPUT_HALF, OUTPUT_CSTR,
    OUTPUT_APP_ZERO, OUTPUT_APP_LENGTH, OUTPUT_APP_STRING,
    OUTPUT_PCM_BYTE, OUTPUT_PCM_WORD, OUTPUT_FILE_COUNT, OUTPUT_FILE_SIZE,
    OUTPUT_PALETTE, OUTPUT_ISR_INSTALL, OUTPUT_ISR_REMOVE, OUTPUT_WRITERS
};

/* A service output is allowed to overlap a previously executed function.
 * The oracle is the literal instruction written by the service, not a
 * comparison between two emulator engines. Actual guest SWIs cover every
 * gateway as well as direct helper calls. I=0 and I=1 both obey the core's
 * existing external-writer contract; this does not model a physical I-cache. */
static void check_scalar_code(unsigned engine, unsigned instruction_cache,
                              enum output_writer writer) {
    gp32_t *g = gp32_create(NULL);
    CHECK(g != NULL, "create scalar output fixture");
    if (!g) return;
    direct_set_fxe_mode(g, 1u);
    arm920t_set_jit(g->cpu, engine == 2u);
    arm920t_set_trace(g->cpu, engine == 0u, NULL, NULL);
    const uint32_t control = GP32_RAM_BASE + 0x10000u;
    const uint32_t trap = GP32_RAM_BASE + 0x14000u;
    const uint32_t data = GP32_RAM_BASE + 0x18000u;
    uint32_t code = GP32_RAM_BASE + 0x22000u;
    if (writer == OUTPUT_APP_STRING) code = direct_app_arg_addr(g);
    if (writer == OUTPUT_ISR_INSTALL || writer == OUTPUT_ISR_REMOVE)
        code = S3C2400_HLE_ISR_TABLE_ADDR + 7u * 4u;
    uint32_t written = 0xe3a00022u, result = 0x22u, end_pc = control;
    s3c2400_write32(g->soc, control, 0xee010f10u); /* MCR p15,0,r0,c1,c0,0 */
    arm920t_set_reg(g->cpu, 0u, 0x70u | (instruction_cache << 12));
    arm920t_set_reg(g->cpu, 15u, control);
    CHECK(arm920t_run(g->cpu, 1u) == 1u, "set scalar fixture cache control");
    s3c2400_write32(g->soc, code, 0xe3a00011u); /* MOV r0,#0x11 */
    s3c2400_write32(g->soc, code + 4u, 0xe12fff1cu); /* BX r12 */
    arm920t_set_reg(g->cpu, 12u, control);
    for (unsigned warm = 0; warm < 2u; ++warm) {
        arm920t_set_reg(g->cpu, 0u, 0x66u);
        arm920t_set_reg(g->cpu, 15u, code);
        CHECK(arm920t_run(g->cpu, 2u) == 2u, "prime scalar output code");
        CHECK(arm920t_get_reg(g->cpu, 0u) == 0x11u, "original scalar fixture MOV");
    }
#if defined(GP32EMU_CPU_PROFILE) && GP32EMU_CPU_PROFILE
    if (engine == 2u && arm920t_get_jit_enabled(g->cpu)) {
        gp32_cpu_profile_t profile;
        arm920t_get_cpu_profile(g->cpu, &profile);
        CHECK(profile.native_arm_insns != 0u, "native execution before scalar output");
    }
#endif
    uint32_t swi = 0u;
    switch (writer) {
    case OUTPUT_WORD: direct_write32_if_ram(g, code, written); break;
    case OUTPUT_BYTE: direct_write8_if_ram(g, code, 0x22u); break;
    case OUTPUT_HALF: direct_write16_if_ram(g, code, 0x22u); break;
    case OUTPUT_CSTR: direct_write_cstr_if_ram(g, code, "\x22"); break;
    case OUTPUT_APP_ZERO:
        swi = 0xef00000fu;
        arm920t_set_reg(g->cpu, 0u, code);
        arm920t_set_reg(g->cpu, 4u, 99u);
        written = 0u; result = 0x66u;
        break;
    case OUTPUT_APP_LENGTH:
        swi = 0xef00000fu;
        strcpy(g->direct_smc_game_dir, "ab");
        arm920t_set_reg(g->cpu, 0u, code);
        arm920t_set_reg(g->cpu, 4u, 1u);
        written = 2u; result = 0x66u;
        break;
    case OUTPUT_APP_STRING:
        swi = 0xef00000fu;
        strcpy(g->direct_smc_game_dir, "\x22");
        arm920t_set_reg(g->cpu, 0u, data);
        arm920t_set_reg(g->cpu, 4u, 0u);
        break;
    case OUTPUT_PCM_BYTE:
    case OUTPUT_PCM_WORD:
    case OUTPUT_FILE_COUNT:
    case OUTPUT_FILE_SIZE: {
        g->direct_fpk_assets = calloc(1u, sizeof(*g->direct_fpk_assets));
        CHECK(g->direct_fpk_assets != NULL, "allocate empty synthetic asset");
        if (!g->direct_fpk_assets) { gp32_destroy(g); return; }
        g->direct_fpk_asset_count = 1u;
        strcpy(g->direct_fpk_assets[0].path, "scalar.bin");
        if (writer == OUTPUT_PCM_BYTE || writer == OUTPUT_PCM_WORD) {
            g->direct_hle_pcm_initialized = 1u;
            g->direct_hle_pcm_sr = 2u;
            g->direct_hle_pcm_bit_count = 1u;
            g->direct_hle_pcm_rate = 22050u;
            swi = 0xef070009u;
            arm920t_set_reg(g->cpu, 0u, writer == OUTPUT_PCM_BYTE ? code : data);
            arm920t_set_reg(g->cpu, 1u, data + 4u);
            arm920t_set_reg(g->cpu, 2u, writer == OUTPUT_PCM_WORD ? code : data + 8u);
            written = writer == OUTPUT_PCM_BYTE ? 0xe3a00002u : 22050u;
            result = writer == OUTPUT_PCM_BYTE ? 2u : 0x66u;
        } else {
            uint32_t handle = direct_alloc_fpk_handle(g, &g->direct_fpk_assets[0]);
            CHECK(handle != 0u, "allocate synthetic file handle");
            swi = writer == OUTPUT_FILE_COUNT ? 0xef070002u : 0xef070004u;
            arm920t_set_reg(g->cpu, 0u, handle);
            arm920t_set_reg(g->cpu, 1u, writer == OUTPUT_FILE_COUNT ? data : code);
            arm920t_set_reg(g->cpu, 2u, 0u); /* zero-length read still writes its output count */
            arm920t_set_reg(g->cpu, 3u, code);
            written = 0u; result = 0x66u;
        }
        break;
    }
    case OUTPUT_PALETTE:
        for (unsigned i = 0; i < 256u; ++i)
            s3c2400_write32(g->soc, 0x14a00400u + i * 4u, i ? 0u : 0x22u);
        direct_copy_lcd_palette_to_ram(g, code);
        written = 0x22u; result = 0x66u; end_pc = code + 8u;
        break;
    case OUTPUT_ISR_INSTALL:
    case OUTPUT_ISR_REMOVE:
        swi = writer == OUTPUT_ISR_INSTALL ? 0xef000009u : 0xef00000au;
        arm920t_set_reg(g->cpu, 0u, 7u);
        arm920t_set_reg(g->cpu, 1u, written);
        if (writer == OUTPUT_ISR_REMOVE) { written = 0u; result = 0x66u; }
        break;
    default: CHECK(0, "known output writer"); break;
    }
    if (swi) {
        s3c2400_write32(g->soc, trap, swi);
        arm920t_set_reg(g->cpu, 14u, control);
        arm920t_set_reg(g->cpu, 15u, trap);
        CHECK(arm920t_run(g->cpu, 1u) == 1u, "execute real output SWI");
    }
    CHECK(s3c2400_read32(g->soc, code) == written, "scalar output changed instruction bytes");
    /* Values below 0x10000000 have EQ condition; Z=0 makes them skip. */
    arm920t_set_cpsr(g->cpu, 0xd3u);
    arm920t_set_reg(g->cpu, 0u, 0x66u);
    arm920t_set_reg(g->cpu, 12u, control);
    arm920t_set_reg(g->cpu, 15u, code);
    unsigned before = failures;
    CHECK(arm920t_run(g->cpu, 2u) == 2u, "scalar replacement retirement");
    CHECK(arm920t_get_reg(g->cpu, 0u) == result, "scalar output revokes previous MOV");
    CHECK(arm920t_get_pc(g->cpu) == end_pc && arm920t_get_cpsr(g->cpu) == 0xd3u,
          "scalar replacement PC and flags");
    if (failures != before)
        fprintf(stderr, "scalar writer=%u engine=%u I=%u\n", writer, engine, instruction_cache);
    ++cases;
    gp32_destroy(g);
}

int main(void) {
    check_zero_bounds();
    for (unsigned engine = 0; engine < 3u; ++engine)
        for (unsigned instruction_cache = 0; instruction_cache < 2u; ++instruction_cache)
            check_zero_code(engine, instruction_cache);
    for (unsigned engine = 0; engine < 3u; ++engine)
        for (unsigned instruction_cache = 0; instruction_cache < 2u; ++instruction_cache)
            for (unsigned writer = 0; writer < OUTPUT_WRITERS; ++writer)
                check_scalar_code(engine, instruction_cache, (enum output_writer)writer);
    if (failures) { fprintf(stderr, "HLE memory: %u cases, %u failures\n", cases, failures); return 1; }
    printf("PASS: %u HLE bounds and instruction-coherency cases\n", cases);
    return 0;
}
