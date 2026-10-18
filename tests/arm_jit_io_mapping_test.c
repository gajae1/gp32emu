/* Negative mapping proofs must cover a range, not one rejected byte. The
 * bus may deny independent permissions or widths inside contiguous RAM. */
#ifndef GP32EMU_CPU_PROFILE
#define GP32EMU_CPU_PROFILE 1
#endif
#ifndef CORE_UNDER_TEST
#define CORE_UNDER_TEST "../src/arm920t.c"
#endif
#include CORE_UNDER_TEST

#define MAP_CODE 0x400u
#define MAP_IO 0x14001000u
#define MAP_EXTRA 0x12001000u
#define MAP_ALIAS 0x24001000u
#define MAP_RAM_END (ARM_JIT_RAM_BASE_ADDR + ARM_JIT_RAM_SIZE_BYTES)
#define MAP_EXT_SIZE (MAP_IO + 16u - MAP_RAM_END)

enum { MAP_READ = 1u, MAP_WRITE = 2u, MAP_READ_HOLE = 4u, MAP_WRITE_HOLE = 8u,
       MAP_READ_WORDS = 16u, MAP_WRITE_WORDS = 32u };
static const struct {
    unsigned flags, open_bus, excludes, phases;
} variants[] = {
    {0u, 0u, 0u, 1u}, {0u, 1u, 0u, 1u}, {0u, 1u, 1u, 1u},
    {MAP_READ, 1u, 1u, 1u}, {MAP_WRITE, 1u, 1u, 1u},
    {MAP_READ | MAP_WRITE, 1u, 1u, 1u},
    {MAP_READ | MAP_WRITE, 0u, 0u, 1u},
    {MAP_READ | MAP_WRITE | MAP_READ_HOLE, 1u, 1u, 1u},
    {MAP_READ | MAP_WRITE | MAP_WRITE_HOLE, 1u, 1u, 1u},
    {MAP_READ | MAP_WRITE | MAP_READ_HOLE | MAP_WRITE_HOLE, 1u, 1u, 1u},
    {MAP_READ | MAP_WRITE | MAP_READ_WORDS, 1u, 1u, 1u},
    {MAP_READ | MAP_WRITE | MAP_WRITE_WORDS, 1u, 1u, 1u},
    {MAP_READ | MAP_WRITE | MAP_READ_WORDS | MAP_WRITE_WORDS, 1u, 1u, 1u},
    {MAP_READ | MAP_WRITE, 1u, 0u, 1u},
    {0u, 1u, 1u, 5u} /* Same CPU/bus: absent, read, write, both, absent. */
};
typedef struct {
    uint8_t bios[ARM_JIT_BIOS_SIZE_BYTES], ram[ARM_JIT_RAM_SIZE_BYTES];
    uint8_t *extension;
    uint32_t data_pa;
    unsigned flags, probes[2], callbacks[2], certificates[2];
    uint32_t callback_write;
} mapping_bus_t;
static mapping_bus_t mapping;
static unsigned cases, failures, current_variant, current_mmu, current_phase;
static uint64_t native_insns;

static void mapping_check(int ok, const char *what) {
    if (ok) return;
    fprintf(stderr, "variant=%u mmu=%u phase=%u pa=%08x: %s\n", current_variant,
            current_mmu, current_phase, mapping.data_pa, what);
    ++failures;
}

static uint8_t *mapping_fastmem(void *user, uint32_t a, size_t bytes, int write) {
    mapping_bus_t *m = user;
    if (a >= m->data_pa && a < m->data_pa + 16u) ++m->probes[!!write];
    if (!write && a < sizeof(m->bios) && bytes <= sizeof(m->bios) - a) return m->bios + a;
    uint32_t off = a - ARM_JIT_RAM_BASE_ADDR;
    if (off < sizeof(m->ram) && bytes <= sizeof(m->ram) - off) return m->ram + off;
    off = a - MAP_RAM_END;
    if (!m->extension || !(m->flags & (write ? MAP_WRITE : MAP_READ)) ||
        off >= MAP_EXT_SIZE || bytes > MAP_EXT_SIZE - off) return NULL;
    if ((m->flags & (write ? MAP_WRITE_HOLE : MAP_READ_HOLE)) && off < 4u) return NULL;
    if ((m->flags & (write ? MAP_WRITE_WORDS : MAP_READ_WORDS)) && bytes < 4u) return NULL;
    return m->extension + off;
}

static int mapping_excludes(void *user, uint32_t a, size_t bytes, int write) {
    mapping_bus_t *m = user;
    uint64_t end = (uint64_t)a + bytes;
    ++m->certificates[!!write];
    if (!bytes || end > UINT64_C(0x100000000)) return 0;
    if (!write && a < sizeof(m->bios)) return 0;
    if (a < MAP_RAM_END && end > ARM_JIT_RAM_BASE_ADDR) return 0;
    /* Conservatively decline to certify holes or narrow spans inside RAM. */
    return !(m->flags & (write ? MAP_WRITE : MAP_READ)) ||
           a >= MAP_RAM_END + MAP_EXT_SIZE || end <= MAP_RAM_END;
}

static uint32_t mapping_read32(void *user, uint32_t a) {
    mapping_bus_t *m = user;
    if (a >= MAP_IO && a < MAP_IO + 16u) {
        ++m->callbacks[0];
        return 0x89abcdefu;
    }
    if (a < sizeof(m->bios) - 3u) return gp32_ld32le(m->bios + a);
    /* Without a direct read mapping the open-bus promise applies here;
     * return the constant without an observer or other state change. */
    return UINT32_MAX;
}
static uint16_t mapping_read16(void *user, uint32_t a) { return (uint16_t)mapping_read32(user, a); }
static uint8_t mapping_read8(void *user, uint32_t a) { return (uint8_t)mapping_read32(user, a); }
static void mapping_write32(void *user, uint32_t a, uint32_t value) {
    mapping_bus_t *m = user;
    if (a >= m->data_pa && a < m->data_pa + 16u) {
        ++m->callbacks[1];
        m->callback_write = value;
    }
}
static void mapping_write16(void *user, uint32_t a, uint16_t value) { mapping_write32(user, a, value); }
static void mapping_write8(void *user, uint32_t a, uint8_t value) { mapping_write32(user, a, value); }

#if ARM920T_NATIVE_BACKEND
static void mapping_case(unsigned variant, unsigned mmu, uint32_t data_pa) {
    current_variant = variant; current_mmu = mmu;
    memset(&mapping, 0, sizeof(mapping));
    mapping.data_pa = data_pa;
    /* calloc reserves the complete physical extension without touching its
     * unused pages. Every accepted span lies inside allocated RAM. */
    mapping.extension = calloc(1u, MAP_EXT_SIZE);
    if (!mapping.extension) exit(2);
    gp32_st32le(mapping.extension + data_pa - MAP_RAM_END, 0x12345678u);
    gp32_st32le(mapping.bios + MAP_CODE, 0xe5910001u); /* LDR r0,[r1,#1] */
    gp32_st32le(mapping.bios + MAP_CODE + 4u, 0xe5812004u); /* STR r2,[r1,#4] */
    gp32_st32le(mapping.bios + MAP_CODE + 8u, 0xe12fff1cu); /* BX r12 */
    arm_bus_t bus = {.read8=mapping_read8, .read16=mapping_read16, .read32=mapping_read32,
        .read32_io=mapping_read32, .write8=mapping_write8, .write16=mapping_write16,
        .write32=mapping_write32, .write32_io=mapping_write32, .fastmem=mapping_fastmem,
        .user=&mapping, .open_bus_valid=(int)variants[variant].open_bus,
        .open_bus_word32=UINT32_MAX,
        .fastmem_excludes=variants[variant].excludes ? mapping_excludes : NULL};
    arm920t_t *c = arm920t_create(&bus);
    if (!c) exit(2);
    arm920t_set_jit(c, 1);
    c->cp15[1] = mmu;
    for (unsigned phase = 0; phase < variants[variant].phases; ++phase) {
        current_phase = phase;
        mapping.flags = variants[variant].phases == 1u ? variants[variant].flags : phase & 3u;
        arm920t_flush_jit(c);
        if (mmu) {
            itlb_store(c, MAP_CODE, 0u, 0xfffu);
            tlb_store_checked(c, MAP_ALIAS, data_pa, 0xfffu, NULL);
        }
        uint32_t expected[16];
        for (unsigned r = 0; r < 16u; ++r) expected[r] = 0x10203040u + r;
        expected[1] = mmu ? MAP_ALIAS : data_pa;
        expected[2] = 0xa5b6c7d8u;
        expected[12] = MAP_CODE + 0x100u;
        expected[15] = MAP_CODE;
        for (unsigned r = 0; r < 16u; ++r) arm920t_set_reg(c, r, expected[r]);
        arm920t_set_cpsr(c, 0x600000d3u);
        memset(mapping.certificates, 0, sizeof(mapping.certificates));
        arm_jit_block_t *b = arm_jit_translate(c, MAP_CODE);
        mapping_check(b && b->count == 3u, "complete three-instruction block");
        if (b) {
            if (b->native_ok == 4u) b->native_ok = 0u;
            mapping_check(!arm_jit_lazy_compile(c, b) && b->native, "native entry prepared");
        }
        if (variants[variant].excludes)
            mapping_check(mapping.certificates[0] && mapping.certificates[1],
                          "mapping certificate queried again after flush");
        memset(mapping.probes, 0, sizeof(mapping.probes));
        memset(mapping.callbacks, 0, sizeof(mapping.callbacks));
        gp32_st32le(mapping.extension + data_pa + 4u - MAP_RAM_END, 0u);
        arm920t_reset_cpu_profile(c);
        mapping_check(arm920t_run(c, 3u) == 3u, "exact retirement");
        int direct_read = !!(mapping.flags & MAP_READ), direct_write = !!(mapping.flags & MAP_WRITE);
        expected[0] = direct_read ? 0x78123456u : data_pa == MAP_IO ? 0xef89abcdu : UINT32_MAX;
        expected[15] = expected[12];
        for (unsigned r = 0; r < 16u; ++r)
            mapping_check(arm920t_get_reg(c, r) == expected[r], "architectural register result");
        mapping_check(arm920t_get_cpsr(c) == 0x600000d3u, "ARM state and flags preserved");
        mapping_check(mapping.callbacks[0] == (unsigned)(data_pa == MAP_IO && !direct_read) &&
                      mapping.callbacks[1] == (unsigned)!direct_write, "exact callback selection");
        unsigned read_probes = 1u, write_probes = 1u;
        if (variants[variant].excludes && data_pa == MAP_IO) {
            read_probes = (unsigned)direct_read; write_probes = (unsigned)direct_write;
        }
#if defined(__aarch64__)
        if (variants[variant].excludes && variants[variant].open_bus && data_pa == MAP_EXTRA)
            read_probes = (unsigned)direct_read;
#endif
        mapping_check(mapping.probes[0] == read_probes && mapping.probes[1] == write_probes,
                      "only the matching complete-range certificate removes a probe");
        if (direct_write)
            mapping_check(gp32_ld32le(mapping.extension + data_pa + 4u - MAP_RAM_END) == expected[2],
                          "direct word store reaches extended RAM");
        else mapping_check(mapping.callback_write == expected[2], "callback receives original source");
        mapping_check(c->prof.native_block_calls == 1u && c->prof.native_arm_insns == 3u,
                      "both memory operations run in one native entry");
        native_insns += c->prof.native_arm_insns;
        ++cases;
    }
    arm920t_destroy(c);
    free(mapping.extension);
}
#endif

int main(void) {
#if ARM920T_NATIVE_BACKEND
    for (unsigned variant = 0; variant < GP32_ARRAY_COUNT(variants); ++variant)
        for (unsigned mmu = 0; mmu < 2u; ++mmu) {
            mapping_case(variant, mmu, MAP_IO);
            mapping_case(variant, mmu, MAP_EXTRA);
        }
#endif
    printf("arm_jit_io_mapping_test: %u cases, %u failures, native-insns=%" PRIu64 "\n",
           cases, failures, native_insns);
    return failures != 0;
}
