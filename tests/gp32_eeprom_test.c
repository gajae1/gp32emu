/* Execute the SDK's raw EEPROM SWI, then observe the same bytes through IIC.
 * Synthetic ARM code only; no firmware or commercial image is required. */
#ifndef GP32_SOURCE
#define GP32_SOURCE "../src/gp32.c"
#endif
#include GP32_SOURCE

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
static const uint32_t code = GP32_RAM_BASE + 0x10000u;
static const uint32_t devid_code = GP32_RAM_BASE + 0x10100u;
static const uint32_t buffer = GP32_RAM_BASE + 0x20000u;

static uint32_t call(gp32_t *g, uint32_t offset, uint32_t count, uint32_t ptr, uint32_t write) {
    arm920t_set_cpsr(g->cpu, 0xd3u);
    arm920t_set_reg(g->cpu, 0, offset);
    arm920t_set_reg(g->cpu, 1, count);
    arm920t_set_reg(g->cpu, 2, ptr);
    arm920t_set_reg(g->cpu, 3, write);
    arm920t_set_reg(g->cpu, 4, 0x12345678u);
    arm920t_set_reg(g->cpu, 14, code + 8u);
    arm920t_set_reg(g->cpu, 15, code);
    CHECK(arm920t_run(g->cpu, 1u) == 1u);
    CHECK(arm920t_get_pc(g->cpu) == code + 4u);
    CHECK(arm920t_get_reg(g->cpu, 1) == count);
    CHECK(arm920t_get_reg(g->cpu, 4) == 0x12345678u);
    CHECK(arm920t_get_reg(g->cpu, 14) == code + 8u);
    return arm920t_get_reg(g->cpu, 0);
}

static void iic_byte(s3c2400_t *s, uint8_t value) {
    s3c2400_write32(s, 0x1540000cu, value);
    s3c2400_write32(s, 0x15400000u, 0x80u); /* acknowledge and transfer */
}

/* SDK _gp_dev_id_get: one argument, r0 = 16-byte output buffer. */
static uint32_t call_devid(gp32_t *g, uint32_t ptr) {
    arm920t_set_cpsr(g->cpu, 0xa00000d3u);
    arm920t_set_reg(g->cpu, 0, ptr);
    arm920t_set_reg(g->cpu, 1, 0x0badf00du);
    arm920t_set_reg(g->cpu, 14, devid_code + 8u);
    arm920t_set_reg(g->cpu, 15, devid_code);
    CHECK(arm920t_run(g->cpu, 1u) == 1u);
    CHECK(arm920t_get_pc(g->cpu) == devid_code + 4u);
    CHECK(arm920t_get_reg(g->cpu, 1) == 0x0badf00du);
    CHECK(arm920t_get_reg(g->cpu, 14) == devid_code + 8u);
    CHECK(arm920t_get_cpsr(g->cpu) == 0xa00000d3u);
    return arm920t_get_reg(g->cpu, 0);
}

static void iic_address(s3c2400_t *s, uint32_t addr) {
    s3c2400_write32(s, 0x15400004u, 0xd0u); /* stop */
    s3c2400_write32(s, 0x15400000u, 0x80u);
    s3c2400_write32(s, 0x1540000cu, 0xa0u);
    s3c2400_write32(s, 0x15400004u, 0xf0u); /* master transmit, start */
    iic_byte(s, (uint8_t)(addr >> 8));
    iic_byte(s, (uint8_t)addr);
}

static uint8_t iic_read(s3c2400_t *s, uint32_t addr) {
    iic_address(s, addr);
    s3c2400_write32(s, 0x15400004u, 0xd0u);
    s3c2400_write32(s, 0x1540000cu, 0xa1u);
    s3c2400_write32(s, 0x15400004u, 0xb0u); /* master receive */
    s3c2400_write32(s, 0x15400000u, 0x80u);
    uint8_t value = (uint8_t)s3c2400_read32(s, 0x1540000cu);
    s3c2400_write32(s, 0x15400004u, 0x90u);
    return value;
}

/* Retail firmware's random-read sequence uses a repeated START, acknowledges
 * the device address, discards that dummy read, then receives the data byte. */
static uint8_t iic_restart_read(s3c2400_t *s, uint32_t addr) {
    iic_address(s, addr);
    s3c2400_write32(s, 0x1540000cu, 0xa1u);
    s3c2400_write32(s, 0x15400004u, 0xb0u);
    s3c2400_write32(s, 0x15400000u, 0x80u);
    (void)s3c2400_read32(s, 0x1540000cu); /* address phase */
    s3c2400_write32(s, 0x15400000u, 0x80u);
    uint8_t value = (uint8_t)s3c2400_read32(s, 0x1540000cu);
    s3c2400_write32(s, 0x15400004u, 0x90u);
    return value;
}

static void check_mode(int jit) {
    gp32_t *g = gp32_create(NULL);
    CHECK(g != NULL);
    if (!g) return;
    gp32_set_jit(g, jit);
    s3c2400_write32(g->soc, code, 0xef000105u);
    s3c2400_write32(g->soc, code + 4u, 0xeafffffeu);
    /* The normal BIOS path must still take the exception vector. */
    arm920t_set_cpsr(g->cpu, 0xd3u);
    arm920t_set_reg(g->cpu, 15, code);
    CHECK(arm920t_run(g->cpu, 1u) == 1u);
    CHECK(arm920t_get_pc(g->cpu) == 8u);
    direct_set_fxe_mode(g, 1u);

    /* SWI 0x104 returns EEPROM[0x10..0x1f] XOR the firmware key "SANGHYUK".
     * Expected bytes are fixed literals; an erased chip gives all-0xff. */
    s3c2400_write32(g->soc, devid_code, 0xef000104u);
    s3c2400_write32(g->soc, devid_code + 4u, 0xeafffffeu);
    s3c2400_write8(g->soc, buffer - 1u, 0x6bu);
    s3c2400_write8(g->soc, buffer + 16u, 0xb6u);
    CHECK(call_devid(g, buffer) == 0u);
    {
        static const uint8_t fresh[16] = {0xac, 0xbe, 0xb1, 0xb8, 0xb7, 0xa6, 0xaa, 0xb4,
                                          0xac, 0xbe, 0xb1, 0xb8, 0xb7, 0xa6, 0xaa, 0xb4};
        for (unsigned i = 0; i < 16u; ++i) CHECK(s3c2400_read8(g->soc, buffer + i) == fresh[i]);
    }
    CHECK(s3c2400_read8(g->soc, buffer - 1u) == 0x6bu);
    CHECK(s3c2400_read8(g->soc, buffer + 16u) == 0xb6u);

    /* Seed 16 bytes through SWI 0x105 and confirm the ID follows the data. */
    for (unsigned i = 0; i < 16u; ++i) s3c2400_write8(g->soc, buffer + 0x30u + i, (uint8_t)(i ^ 0x5au));
    CHECK(call(g, 0x10u, 16u, buffer + 0x30u, 1u) == 0u);
    CHECK(call_devid(g, buffer) == 0u);
    {
        static const uint8_t seeded[16] = {0x09, 0x1a, 0x16, 0x1e, 0x16, 0x06, 0x09, 0x16,
                                           0x01, 0x12, 0x1e, 0x16, 0x1e, 0x0e, 0x01, 0x1e};
        for (unsigned i = 0; i < 16u; ++i) CHECK(s3c2400_read8(g->soc, buffer + i) == seeded[i]);
    }
    CHECK(iic_read(g->soc, 0x10u) == 0x5au);
    CHECK(s3c2400_read8(g->soc, buffer - 1u) == 0x6bu);
    CHECK(s3c2400_read8(g->soc, buffer + 16u) == 0xb6u);

    s3c2400_write32(g->soc, buffer, 0x12345678u);
    CHECK(call(g, 0u, 4u, buffer, 0u) == 0u);
    CHECK(s3c2400_read32(g->soc, buffer) == 0xffffffffu);

    /* A nonzero selector writes, with an unaligned buffer and a transfer
     * crossing the low EEPROM address byte. Observe it via hardware IIC. */
    const uint8_t bytes[] = {0x31, 0x80, 0x00, 0xfe, 0x52};
    for (unsigned i = 0; i < sizeof(bytes); ++i)
        s3c2400_write8(g->soc, buffer + 1u + i, bytes[i]);
    CHECK(call(g, 0xfeu, sizeof(bytes), buffer + 1u, 7u) == 0u);
    for (unsigned i = 0; i < sizeof(bytes); ++i)
        CHECK(iic_read(g->soc, 0xfeu + i) == bytes[i]);
    for (unsigned i = 0; i < sizeof(bytes); ++i)
        CHECK(iic_restart_read(g->soc, 0xfeu + i) == bytes[i]);

    /* Conversely, a hardware write is immediately visible to the SDK. */
    iic_address(g->soc, 0x100u);
    iic_byte(g->soc, 0xa7u);
    s3c2400_write32(g->soc, 0x15400004u, 0xd0u);
    CHECK(call(g, 0x100u, 1u, buffer, 0u) == 0u);
    CHECK(s3c2400_read8(g->soc, buffer) == 0xa7u);

    /* Exactly reaching 4 KiB is valid. Negative offsets clamp to zero. */
    CHECK(call(g, 0xffbu, sizeof(bytes), buffer + 1u, 1u) == 0u);
    CHECK(iic_read(g->soc, 0xfffu) == bytes[4]);
    CHECK(call(g, 0xfffffff0u, 1u, buffer, 1u) == 0u);
    CHECK(iic_read(g->soc, 0u) == 0xa7u);
    const uint32_t bad[][2] = {{0, 0}, {0, 0xffffffffu}, {0xfff, 2},
                              {0x1000, 1}, {0x7fffff00, 0x200}};
    for (unsigned i = 0; i < GP32_ARRAY_COUNT(bad); ++i) {
        s3c2400_write8(g->soc, buffer, 0x6bu);
        CHECK(call(g, bad[i][0], bad[i][1], buffer, 0u) == 0x21u);
        CHECK(s3c2400_read8(g->soc, buffer) == 0x6bu);
        CHECK(call(g, bad[i][0], bad[i][1], buffer, 1u) == 0x21u);
        CHECK(iic_read(g->soc, 0u) == 0xa7u);
        CHECK(iic_read(g->soc, 0xfffu) == bytes[4]);
    }

    /* Save/load and reset must not introduce a second EEPROM backing store. */
    size_t size = gp32_state_size(g);
    uint8_t *state = (uint8_t *)malloc(size);
    CHECK(state != NULL);
    if (state) {
        CHECK(gp32_save_state_data(g, state, size) == GP32_OK);
        CHECK(call(g, 0x100u, 1u, buffer, 1u) == 0u);
        CHECK(iic_read(g->soc, 0x100u) == 0x6bu);
        CHECK(gp32_load_state_data(g, state, size) == GP32_OK);
        CHECK(iic_read(g->soc, 0x100u) == 0xa7u);
        CHECK(call(g, 0x100u, 1u, buffer, 0u) == 0u);
        CHECK(s3c2400_read8(g->soc, buffer) == 0xa7u);
        free(state);
    }
    s3c2400_reset(g->soc);
    CHECK(iic_read(g->soc, 0x100u) == 0xa7u);
    gp32_destroy(g);
}

/* The caller owns an EEPROM read's destination. It can be a previously
 * executed RAM function, so these host writes need the same external-code
 * notification as an HLE file read. Exercise actual SWIs, including the
 * device-ID transformation, while keeping the raw ARM path as a control. */
static void check_read_code(unsigned engine, unsigned instruction_cache, int devid) {
    gp32_t *g = gp32_create(NULL);
    CHECK(g != NULL);
    if (!g) return;
    direct_set_fxe_mode(g, 1u);
    arm920t_set_jit(g->cpu, engine == 2u);
    arm920t_set_trace(g->cpu, engine == 0u, NULL, NULL);
    const uint32_t control = code + 0x200u;
    s3c2400_write32(g->soc, code, 0xef000105u);
    s3c2400_write32(g->soc, devid_code, 0xef000104u);
    s3c2400_write32(g->soc, control, 0xee010f10u); /* MCR p15,0,r0,c1,c0,0 */
    arm920t_set_reg(g->cpu, 0u, 0x70u | (instruction_cache << 12));
    arm920t_set_reg(g->cpu, 15u, control);
    CHECK(arm920t_run(g->cpu, 1u) == 1u);
    s3c2400_write32(g->soc, buffer, 0xe3a00011u); /* MOV r0,#0x11 */
    s3c2400_write32(g->soc, buffer + 4u, 0xe12fff1cu); /* BX r12 */
    arm920t_set_reg(g->cpu, 12u, control);
    for (unsigned warm = 0; warm < 2u; ++warm) {
        arm920t_set_reg(g->cpu, 0u, 0u);
        arm920t_set_reg(g->cpu, 15u, buffer);
        CHECK(arm920t_run(g->cpu, 2u) == 2u);
        CHECK(arm920t_get_reg(g->cpu, 0u) == 0x11u);
    }
#if defined(GP32EMU_CPU_PROFILE) && GP32EMU_CPU_PROFILE
    if (engine == 2u && arm920t_get_jit_enabled(g->cpu)) {
        gp32_cpu_profile_t profile;
        arm920t_get_cpu_profile(g->cpu, &profile);
        CHECK(profile.native_arm_insns != 0u);
    }
#endif
    static const uint8_t replacement[16] = {
        0x22u, 0x00u, 0xa0u, 0xe3u, /* MOV r0,#0x22 */
        0x1cu, 0xffu, 0x2fu, 0xe1u, /* BX r12 */
        0x00u, 0x00u, 0xa0u, 0xe1u, 0x00u, 0x00u, 0xa0u, 0xe1u
    };
    static const uint8_t key[8] = {'S', 'A', 'N', 'G', 'H', 'Y', 'U', 'K'};
    uint32_t count = devid ? 16u : 4u, offset = devid ? 0x10u : 0x200u;
    for (uint32_t i = 0; i < count; ++i)
        s3c2400_eeprom_write8(g->soc, offset + i,
            (uint8_t)(replacement[i] ^ (devid ? key[i & 7u] : 0u)));
    s3c2400_write8(g->soc, buffer - 1u, 0xa5u);
    if (devid) s3c2400_write8(g->soc, buffer + count, 0x5au);
    CHECK((devid ? call_devid(g, buffer) : call(g, offset, count, buffer, 0u)) == 0u);
    for (uint32_t i = 0; i < count; ++i)
        CHECK(s3c2400_read8(g->soc, buffer + i) == replacement[i]);
    CHECK(s3c2400_read8(g->soc, buffer - 1u) == 0xa5u);
    CHECK(s3c2400_read32(g->soc, buffer + 4u) == 0xe12fff1cu);
    if (devid) CHECK(s3c2400_read8(g->soc, buffer + count) == 0x5au);
    arm920t_set_cpsr(g->cpu, 0xd3u);
    arm920t_set_reg(g->cpu, 0u, 0u);
    arm920t_set_reg(g->cpu, 15u, buffer);
    uint64_t before = arm920t_get_cycles(g->cpu);
    CHECK(arm920t_run(g->cpu, 2u) == 2u);
    CHECK(arm920t_get_cycles(g->cpu) == before + 2u);
    CHECK(arm920t_get_reg(g->cpu, 0u) == 0x22u);
    CHECK(arm920t_get_pc(g->cpu) == control);
    CHECK(arm920t_get_cpsr(g->cpu) == 0xd3u);

    /* A write to EEPROM cannot change guest code, and an ordinary read
     * buffer on an untracked page must not invalidate the code cache. */
    gp32_cpu_profile_t p0, p1;
    arm920t_get_cpu_profile(g->cpu, &p0);
    CHECK(call(g, 0x300u, 4u, buffer, 1u) == 0u);
    CHECK((devid ? call_devid(g, buffer + 0x1000u) :
        call(g, offset, count, buffer + 0x1000u, 0u)) == 0u);
    arm920t_get_cpu_profile(g->cpu, &p1);
    CHECK(p0.jit_invalidations == p1.jit_invalidations);
    gp32_destroy(g);
}

int main(void) {
    check_mode(0);
    check_mode(1);
    for (unsigned engine = 0; engine < 3u; ++engine)
        for (unsigned instruction_cache = 0; instruction_cache < 2u; ++instruction_cache)
            for (int devid = 0; devid < 2; ++devid)
                check_read_code(engine, instruction_cache, devid);
    if (!failures) puts("PASS: SDK EEPROM / IIC / state and 12 read-code coherency cases");
    return failures ? 1 : 0;
}
