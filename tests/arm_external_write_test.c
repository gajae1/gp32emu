/* External notifications must cover every translated instruction word, but
 * data sharing a RAM page with code need not renew the translation epoch.
 * The independent oracle intersects byte spans with two known instructions;
 * it does not construct or inspect the production watch bitmap. */
#ifndef ARM_SOURCE
#define ARM_SOURCE "../src/arm920t.c"
#endif
#include ARM_SOURCE

static uint8_t ram[ARM_JIT_RAM_SIZE_BYTES];
static uint8_t bios[ARM_JIT_BIOS_SIZE_BYTES];
static unsigned cases, failures;

#define CHECK(x, what) do { if (!(x)) { \
    if (failures < 12u) fprintf(stderr, "FAIL: %s (line %d)\n", what, __LINE__); \
    ++failures; } } while (0)

static uint8_t *mapped(void *user, uint32_t addr, size_t bytes, int write) {
    GP32_UNUSED(user);
    if (addr >= ARM_JIT_RAM_BASE_ADDR &&
        addr - ARM_JIT_RAM_BASE_ADDR < sizeof(ram) &&
        bytes <= sizeof(ram) - (addr - ARM_JIT_RAM_BASE_ADDR))
        return ram + (addr - ARM_JIT_RAM_BASE_ADDR);
    if (!write && addr < sizeof(bios) && bytes <= sizeof(bios) - addr)
        return bios + addr;
    return NULL;
}
static uint8_t read8(void *user, uint32_t addr) {
    uint8_t *p = mapped(user, addr, 1u, 0); return p ? *p : 0xffu;
}
static uint16_t read16(void *user, uint32_t addr) {
    uint8_t *p = mapped(user, addr, 2u, 0); return p ? gp32_ld16le(p) : 0xffffu;
}
static uint32_t read32(void *user, uint32_t addr) {
    uint8_t *p = mapped(user, addr, 4u, 0); return p ? gp32_ld32le(p) : UINT32_MAX;
}
static void write8(void *user, uint32_t addr, uint8_t value) {
    uint8_t *p = mapped(user, addr, 1u, 1); if (p) *p = value;
}
static void write16(void *user, uint32_t addr, uint16_t value) {
    uint8_t *p = mapped(user, addr, 2u, 1); if (p) gp32_st16le(p, value);
}
static void write32(void *user, uint32_t addr, uint32_t value) {
    uint8_t *p = mapped(user, addr, 4u, 1); if (p) gp32_st32le(p, value);
}

static arm920t_t *fixture(unsigned native, unsigned instruction_cache,
                          uint32_t offset, int mmu) {
    arm_bus_t bus = {0};
    bus.read8 = read8; bus.read16 = read16; bus.read32 = read32;
    bus.write8 = write8; bus.write16 = write16; bus.write32 = write32;
    bus.fastmem = mapped;
    arm920t_t *cpu = arm920t_create(&bus);
    CHECK(cpu != NULL, "create external-write fixture");
    if (!cpu) return NULL;
    arm920t_set_jit(cpu, native);
    gp32_st32le(bios + 0x100u, 0xee010f10u); /* MCR p15,0,r0,c1,c0,0 */
    gp32_st32le(bios + 0x104u, 0xee020f10u); /* MCR p15,0,r0,c2,c0,0 */
    gp32_st32le(bios + 0x108u, 0xee030f10u); /* MCR p15,0,r0,c3,c0,0 */
    if (mmu) {
        memset(ram + 0x10000u, 0, 0x4000u);
        gp32_st32le(ram + 0x10000u, 2u); /* BIOS identity section */
        gp32_st32le(ram + 0x10000u + 0xc0u * 4u, ARM_JIT_RAM_BASE_ADDR | 2u);
        gp32_st32le(ram + 0x10000u + 0xc1u * 4u, ARM_JIT_RAM_BASE_ADDR | 2u);
        arm920t_set_reg(cpu, 0u, ARM_JIT_RAM_BASE_ADDR + 0x10000u);
        arm920t_set_reg(cpu, 15u, 0x104u);
        CHECK(arm920t_run(cpu, 1u) == 1u, "set translation table through guest MCR");
        arm920t_set_reg(cpu, 0u, UINT32_MAX);
        arm920t_set_reg(cpu, 15u, 0x108u);
        CHECK(arm920t_run(cpu, 1u) == 1u, "set manager domains through guest MCR");
    }
    arm920t_set_reg(cpu, 0u, 0x70u | (instruction_cache << 12) | (mmu ? 1u : 0u));
    arm920t_set_reg(cpu, 15u, 0x100u);
    CHECK(arm920t_run(cpu, 1u) == 1u, "set guest I/MMU control");
    gp32_st32le(ram + offset, 0xe3a00011u); /* MOV r0,#0x11 */
    gp32_st32le(ram + offset + 4u, 0xe12fff1cu); /* BX r12 */
    arm920t_set_reg(cpu, 12u, 0x100u);
    for (unsigned warm = 0; warm < 2u; ++warm) {
        arm920t_set_reg(cpu, 15u, ARM_JIT_RAM_BASE_ADDR + offset + (mmu ? 0x100000u : 0u));
        CHECK(arm920t_run(cpu, 2u) == 2u, "prime translated instruction pair");
        CHECK(arm920t_get_reg(cpu, 0u) == 0x11u && arm920t_get_pc(cpu) == 0x100u,
              "literal warm-up result");
    }
#if ARM920T_PROFILING
    if (native && arm920t_get_jit_enabled(cpu))
        CHECK(cpu->prof.native_arm_insns != 0u, "positive native execution");
#endif
    return cpu;
}

static void notify_known_span(arm920t_t *cpu, uint32_t pc, uint32_t code_bytes,
                             uint32_t addr, uint32_t len) {
    /* Mathematical half-open intervals in uint64_t avoid uint32_t overflow.
     * The oracle uses the literal code span, never the production bitmap. */
    int overlap = len && (uint64_t)addr < (uint64_t)pc + code_bytes &&
                  (uint64_t)pc < (uint64_t)addr + len;
    uint32_t epoch = cpu->jit_cache_epoch;
    arm920t_invalidate_icache_range(cpu, addr, len);
    CHECK((cpu->jit_cache_epoch != epoch) == overlap,
          "only notified instruction words renew a tracked epoch");
    ++cases;
}

static void notify_span(arm920t_t *cpu, uint32_t pc, uint32_t addr, uint32_t len) {
    notify_known_span(cpu, pc, 8u, addr, len);
}

/* A successful revalidation records complete runs, unlike the individual
 * four-byte fetches in ordinary collection. Check all notification bytes
 * around 32-instruction bitmap boundaries, page boundaries and RAM's end.
 * A separate preceding instruction must survive later overlapping proofs. */
static void check_revalidation_ranges(unsigned instruction_cache) {
    const uint32_t offsets[] = {256u, 380u, 384u, 4092u, 4096u, sizeof(ram) - 384u};
    const unsigned counts[] = {1u, 2u, 31u, 32u, 33u, 63u, 64u, 65u, 95u, 96u};
    const uint32_t lengths[] = {0u, 1u, 2u, 4u, 17u, 129u};
    uint32_t words[96];
    for (unsigned i = 0u; i < GP32_ARRAY_COUNT(words); ++i)
        words[i] = 0xe2800001u; /* ADD r0,r0,#1 */
    arm920t_t *cpu = fixture(0u, instruction_cache, 0u, 0);
    if (!cpu) return;
    for (unsigned o = 0u; o < GP32_ARRAY_COUNT(offsets); ++o)
        for (unsigned n = 0u; n < GP32_ARRAY_COUNT(counts); ++n) {
            unsigned count = counts[n];
            uint32_t pc = ARM_JIT_RAM_BASE_ADDR + offsets[o], code_bytes = count * 4u;
            arm920t_flush_jit(cpu);
            for (unsigned i = 0u; i < count; ++i)
                write32(NULL, pc + i * 4u, words[i]);
            CHECK(arm_jit_prove_run(cpu, pc, count, words, NULL, 0u),
                  "literal opcode run revalidates");
            for (int delta = -132; delta <= (int)code_bytes + 132; ++delta)
                for (unsigned l = 0u; l < GP32_ARRAY_COUNT(lengths); ++l)
                    notify_known_span(cpu, pc, code_bytes,
                                      (uint32_t)((int64_t)pc + delta), lengths[l]);

            /* Adjacent proofs form a larger known interval. Existing watch
             * bits outside the new proof must not be overwritten by a mask. */
            write32(NULL, pc - 4u, words[0]);
            CHECK(arm_jit_prove_run(cpu, pc - 4u, 1u, words, NULL, 0u),
                  "adjacent opcode revalidates");
            CHECK(arm_jit_prove_run(cpu, pc, count, words, NULL, 0u),
                  "repeated long proof preserves adjacent watch");
            notify_known_span(cpu, pc - 4u, code_bytes + 4u, pc - 4u, 1u);
            notify_known_span(cpu, pc - 4u, code_bytes + 4u, pc - 5u, 1u);
            notify_known_span(cpu, pc - 4u, code_bytes + 4u, pc + code_bytes, 1u);
        }
    arm920t_destroy(cpu);
}

static void check_tracked(unsigned native, unsigned instruction_cache, uint32_t offset) {
    arm920t_t *cpu = fixture(native, instruction_cache, offset, 0);
    if (!cpu) return;
    uint32_t pc = ARM_JIT_RAM_BASE_ADDR + offset;
    for (int delta = -12; delta <= 16; ++delta)
        for (uint32_t len = 0u; len <= 17u; ++len)
            notify_span(cpu, pc, (uint32_t)((int64_t)pc + delta), len);
    const uint32_t spans[][2] = {
        {ARM_JIT_RAM_BASE_ADDR - 4u, 4u}, {ARM_JIT_RAM_BASE_ADDR - 4u, 5u},
        {ARM_JIT_RAM_BASE_ADDR, sizeof(ram)}, {ARM_JIT_RAM_BASE_ADDR, UINT32_MAX},
        {ARM_JIT_RAM_BASE_ADDR + sizeof(ram) - 1u, 16u},
        {ARM_JIT_RAM_BASE_ADDR + sizeof(ram), 1u}, {UINT32_MAX, 1u},
        {pc & ~4095u, 4096u}, {pc - 129u, 129u}, {pc - 129u, 130u},
        {pc + 8u, 128u}, {pc - 128u, 256u}, {pc - 4096u, 8192u}
    };
    for (unsigned i = 0u; i < GP32_ARRAY_COUNT(spans); ++i)
        notify_span(cpu, pc, spans[i][0], spans[i][1]);
    /* A byte, halfword and word external replacement must execute new code
     * after the epoch-only checks, in both guest I-cache settings. */
    for (unsigned bytes = 1u; bytes <= 4u; bytes *= 2u) {
        uint32_t immediate = 0x20u + bytes;
        if (bytes == 1u) write8(NULL, pc, (uint8_t)immediate);
        else if (bytes == 2u) write16(NULL, pc, (uint16_t)immediate);
        else write32(NULL, pc, 0xe3a00000u | immediate);
        arm920t_invalidate_icache_range(cpu, pc, bytes);
        arm920t_set_reg(cpu, 15u, pc);
        CHECK(arm920t_run(cpu, 2u) == 2u && arm920t_get_reg(cpu, 0u) == immediate,
              "notified replacement executes its new immediate");
        ++cases;
    }
    arm920t_destroy(cpu);
}

static void check_mmu(unsigned native, unsigned instruction_cache) {
    const uint32_t offset = 0x2000u, pc = ARM_JIT_RAM_BASE_ADDR + offset;
    arm920t_t *cpu = fixture(native, instruction_cache, offset, 1);
    if (!cpu) return;
    uint32_t epoch = cpu->jit_cache_epoch;
    arm920t_invalidate_icache_range(cpu, pc + 128u, 4u);
    CHECK(cpu->jit_cache_epoch != epoch, "MMU fallback remains conservative for data");
    write32(NULL, pc, 0xe3a00033u);
    arm920t_invalidate_icache_range(cpu, pc, 4u);
    arm920t_set_reg(cpu, 15u, pc + 0x100000u);
    CHECK(arm920t_run(cpu, 2u) == 2u && arm920t_get_reg(cpu, 0u) == 0x33u,
          "physical notification updates virtual alias");
    cases += 2u;
    arm920t_destroy(cpu);
}

int main(void) {
    const uint32_t offsets[] = {0u, 124u, 128u, 4092u, 4096u, sizeof(ram) - 8u};
    for (unsigned native = 0u; native < 2u; ++native)
        for (unsigned instruction_cache = 0u; instruction_cache < 2u; ++instruction_cache) {
            for (unsigned i = 0u; i < GP32_ARRAY_COUNT(offsets); ++i)
                check_tracked(native, instruction_cache, offsets[i]);
            check_mmu(native, instruction_cache);
        }
    for (unsigned instruction_cache = 0u; instruction_cache < 2u; ++instruction_cache)
        check_revalidation_ranges(instruction_cache);
    if (failures) {
        fprintf(stderr, "external writes: %u cases, %u failures\n", cases, failures);
        return 1;
    }
    printf("PASS: %u external-write span, replacement, revalidation and MMU fallback cases\n", cases);
    return 0;
}
