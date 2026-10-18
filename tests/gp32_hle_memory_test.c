/* Direct-mode RAM initialization must preserve bounds and revoke decoded code
 * just like the existing external asset-copy helper. No firmware is needed. */
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

int main(void) {
    check_zero_bounds();
    for (unsigned engine = 0; engine < 3u; ++engine)
        for (unsigned instruction_cache = 0; instruction_cache < 2u; ++instruction_cache)
            check_zero_code(engine, instruction_cache);
    if (failures) { fprintf(stderr, "HLE memory: %u cases, %u failures\n", cases, failures); return 1; }
    printf("PASS: %u HLE zero bounds and instruction-coherency cases\n", cases);
    return 0;
}
