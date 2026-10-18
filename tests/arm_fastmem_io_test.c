/* fastmem is an independent permission for each complete physical span.
 * A bus may expose direct memory inside the identity-I/O address window when
 * open_bus_valid is clear. Both direct and callback views below are explicit
 * fixture data; expected values do not come from another CPU executor. */
#ifndef GP32EMU_CPU_PROFILE
#define GP32EMU_CPU_PROFILE 1
#endif
#ifndef CORE_UNDER_TEST
#define CORE_UNDER_TEST "../src/arm920t.c"
#endif
#include CORE_UNDER_TEST

#define TEST_CODE 0x400u
#define TEST_DATA 0x14001000u
#define TEST_SIZE 64u
typedef struct {
    uint8_t bios[ARM_JIT_BIOS_SIZE_BYTES];
    uint8_t ram[ARM_JIT_RAM_SIZE_BYTES];
    uint8_t direct[TEST_SIZE], fallback[TEST_SIZE];
    uint32_t data_base;
    unsigned direct_limit, read_direct, write_direct, reads, writes;
} io_fixture_t;
static io_fixture_t fixture;
static unsigned failures, cases;
static unsigned failures_by_mode[3], cases_by_mode[3];
static uint64_t native_insns;
static unsigned current_mode, current_operation, current_policy, current_mmu, current_offset;

static void check(uint32_t got, uint32_t want, const char *what) {
    if (got == want) return;
    if (failures < 32u)
        fprintf(stderr, "mode=%u operation=%u policy=%u mmu=%u base=%08x offset=%u span=%u %s: got=%08x want=%08x\n",
                current_mode, current_operation, current_policy, current_mmu,
                fixture.data_base, current_offset, fixture.direct_limit, what, got, want);
    ++failures;
    ++failures_by_mode[current_mode];
}
static uint8_t *normal_ptr(io_fixture_t *f, uint32_t a, size_t bytes) {
    if (a < sizeof(f->bios) && bytes <= sizeof(f->bios) - a) return f->bios + a;
    uint32_t offset = a - ARM_JIT_RAM_BASE_ADDR;
    if (offset < sizeof(f->ram) && bytes <= sizeof(f->ram) - offset) return f->ram + offset;
    return NULL;
}
static uint8_t *test_fastmem(void *user, uint32_t a, size_t bytes, int write) {
    io_fixture_t *f = user;
    uint32_t offset = a - f->data_base;
    if (offset < TEST_SIZE && bytes <= TEST_SIZE - offset)
        return (write ? f->write_direct : f->read_direct) && offset < f->direct_limit &&
               bytes <= f->direct_limit - offset ? f->direct + offset : NULL;
    return normal_ptr(f, a, bytes);
}
static uint32_t read_value(void *user, uint32_t a, unsigned width) {
    io_fixture_t *f = user;
    uint32_t value = 0, offset = a - f->data_base;
    uint8_t *p;
    if (offset < TEST_SIZE && width <= TEST_SIZE - offset) {
        ++f->reads;
        p = f->fallback + offset;
    } else p = normal_ptr(f, a, width);
    if (p) for (unsigned i = 0; i < width; ++i) value += (uint32_t)p[i] << (i * 8u);
    return value;
}
static uint8_t test_read8(void *user, uint32_t a) { return (uint8_t)read_value(user,a,1u); }
static uint16_t test_read16(void *user, uint32_t a) { return (uint16_t)read_value(user,a,2u); }
static uint32_t test_read32(void *user, uint32_t a) { return read_value(user,a,4u); }
static void write_value(void *user, uint32_t a, uint32_t value, unsigned width) {
    io_fixture_t *f = user;
    uint32_t offset = a - f->data_base;
    uint8_t *p;
    if (offset < TEST_SIZE && width <= TEST_SIZE - offset) {
        ++f->writes;
        p = f->fallback + offset;
    } else p = normal_ptr(f, a, width);
    if (p) for (unsigned i = 0; i < width; ++i) p[i] = (uint8_t)(value >> (i * 8u));
}
static void test_write8(void *user, uint32_t a, uint8_t v) { write_value(user,a,v,1u); }
static void test_write16(void *user, uint32_t a, uint16_t v) { write_value(user,a,v,2u); }
static void test_write32(void *user, uint32_t a, uint32_t v) { write_value(user,a,v,4u); }

typedef struct { uint32_t opcode; unsigned width, read, write, sign, rotation, count, writeback; } operation_t;
static const operation_t operations[] = {
    {0xe5910000u,4u,1u,0u,0u,0u,1u,0u}, /* LDR r0,[r1] */
    {0xe5910001u,4u,1u,0u,0u,8u,1u,0u}, /* LDR r0,[r1,#1] */
    {0xe5d10000u,1u,1u,0u,0u,0u,1u,0u}, /* LDRB r0,[r1] */
    {0xe1d100b0u,2u,1u,0u,0u,0u,1u,0u}, /* LDRH r0,[r1] */
    {0xe1d100d0u,1u,1u,0u,1u,0u,1u,0u}, /* LDRSB r0,[r1] */
    {0xe1d100f0u,2u,1u,0u,1u,0u,1u,0u}, /* LDRSH r0,[r1] */
    {0xe5810000u,4u,0u,1u,0u,0u,1u,0u}, /* STR r0,[r1] */
    {0xe5c10000u,1u,0u,1u,0u,0u,1u,0u}, /* STRB r0,[r1] */
    {0xe1c100b0u,2u,0u,1u,0u,0u,1u,0u}, /* STRH r0,[r1] */
    {0xe1012090u,4u,1u,1u,0u,0u,1u,0u}, /* SWP r2,r0,[r1] */
    {0xe1412090u,1u,1u,1u,0u,0u,1u,0u}, /* SWPB r2,r0,[r1] */
    {0xe8b1000du,4u,1u,0u,0u,0u,3u,12u}, /* LDMIA r1!,{r0,r2,r3} */
    {0xe8a1000du,4u,0u,1u,0u,0u,3u,12u}, /* STMIA r1!,{r0,r2,r3} */
    {0xe4910004u,4u,1u,0u,0u,0u,1u,4u}, /* LDR r0,[r1],#4 */
    {0xe4810004u,4u,0u,1u,0u,0u,1u,4u}, /* STR r0,[r1],#4 */
    {0xe0d100b2u,2u,1u,0u,0u,0u,1u,2u}, /* LDRH r0,[r1],#2 */
    {0xe0c100b2u,2u,0u,1u,0u,0u,1u,2u}, /* STRH r0,[r1],#2 */
};

static void prepare_native(arm920t_t *cpu) {
#if ARM920T_NATIVE_BACKEND
    arm_jit_block_t *b = arm_jit_translate(cpu, TEST_CODE);
    check(b != NULL,1u,"block collected");
    if (!b) return;
    if (b->native_ok == 4u) b->native_ok = 0u;
    check((uint32_t)arm_jit_lazy_compile(cpu,b),0u,"no arena recycle");
    check(b->native != NULL,1u,"native entry prepared");
#else
    (void)cpu;
#endif
}
static void test_case(unsigned execution, unsigned operation, unsigned policy,
                      uint32_t data_base, unsigned offset, unsigned direct_limit,
                      unsigned mmu, unsigned io_callbacks, unsigned certified) {
    current_mode = execution; current_operation = operation; current_policy = policy;
    current_mmu = mmu; current_offset = offset;
    memset(&fixture,0,sizeof(fixture));
    fixture.data_base = data_base;
    fixture.direct_limit = direct_limit;
    fixture.read_direct = policy & 1u;
    fixture.write_direct = policy & 2u;
    for (unsigned i=0;i<TEST_SIZE;++i) {
        fixture.direct[i] = (uint8_t)(0x80u + i * 11u);
        fixture.fallback[i] = (uint8_t)(0x19u + i * 7u);
    }
    const operation_t *op = &operations[operation];
    gp32_st32le(fixture.bios + TEST_CODE,op->opcode);
    gp32_st32le(fixture.bios + TEST_CODE + 4u,0xe12fff1cu); /* BX r12 */
    arm_bus_t bus = {.read8=test_read8,.read16=test_read16,.read32=test_read32,
        .write8=test_write8,.write16=test_write16,.write32=test_write32,
        .read32_io=io_callbacks ? test_read32 : NULL,.write32_io=io_callbacks ? test_write32 : NULL,
        .fastmem=test_fastmem,.user=&fixture};
    bus.open_bus_valid = (int)certified;
    arm920t_t *cpu = arm920t_create(&bus);
    if (!cpu) exit(2);
    uint32_t guest_base = mmu == 2u ? 0x24000000u + (data_base % 0x100000u) : data_base;
    if (mmu) {
        /* Section mappings are configured by actual guest MCRs. Expected
         * data addresses are the literal mapping above, independently of
         * the CPU's translation helpers and resident-lookup representation. */
        gp32_st32le(fixture.bios+0x4000u,0x00000c02u);
        unsigned first = guest_base / 0x100000u;
        unsigned last = (guest_base + TEST_SIZE - 1u) / 0x100000u;
        gp32_st32le(fixture.bios+0x4000u+4u*first,(data_base & 0xfff00000u)|0xc02u);
        gp32_st32le(fixture.bios+0x4000u+4u*last,((data_base+TEST_SIZE-1u) & 0xfff00000u)|0xc02u);
        gp32_st32le(fixture.bios+0x100u,0xee020f10u); /* MCR p15,0,r0,c2,c0,0 */
        gp32_st32le(fixture.bios+0x104u,0xee031f10u); /* MCR p15,0,r1,c3,c0,0 */
        gp32_st32le(fixture.bios+0x108u,0xee012f10u); /* MCR p15,0,r2,c1,c0,0 */
        arm920t_set_trace(cpu,1,NULL,NULL);
        arm920t_set_reg(cpu,0u,0x4000u); arm920t_set_reg(cpu,1u,0xffffffffu);
        arm920t_set_reg(cpu,2u,0x71u); arm920t_set_reg(cpu,15u,0x100u);
        check(arm920t_run(cpu,3u),3u,"MMU setup retirement");
        arm920t_set_trace(cpu,0,NULL,NULL);
    }
    arm920t_set_jit(cpu,execution == 2u);
    if (!execution) arm920t_set_trace(cpu,1,NULL,NULL);
    uint32_t expected[16];
    for (unsigned i=0;i<16u;++i) expected[i] = 0x10203040u + i;
    expected[0] = 0xa5b6c7d8u; expected[1] = guest_base + offset;
    expected[12] = TEST_CODE + 0x100u; expected[15] = TEST_CODE;
    for (unsigned i=0;i<16u;++i) arm920t_set_reg(cpu,i,expected[i]);
    expected[15] = expected[12];
    uint8_t expect_direct[TEST_SIZE],expect_fallback[TEST_SIZE];
    memcpy(expect_direct,fixture.direct,sizeof(expect_direct));
    memcpy(expect_fallback,fixture.fallback,sizeof(expect_fallback));
    unsigned expected_reads=0u,expected_writes=0u;
    static const unsigned block_registers[] = {0u,2u,3u};
    for (unsigned transfer=0;transfer<op->count;++transfer) {
        unsigned address = offset + transfer*4u;
        int full = address < direct_limit && op->width <= direct_limit - address;
        unsigned reg = block_registers[transfer];
        if (op->read) {
            unsigned direct = fixture.read_direct && full;
            const uint8_t *p = (direct ? expect_direct : expect_fallback) + address;
            uint32_t value = 0;
            for (unsigned i=0;i<op->width;++i) value += (uint32_t)p[i] << (i * 8u);
            if (op->sign && p[op->width-1u] >= 128u)
                value |= op->width == 1u ? 0xffffff00u : 0xffff0000u;
            for (unsigned i=0;i<op->rotation;++i) value = value / 2u + (value % 2u ? 0x80000000u : 0u);
            expected[op->write ? 2u : reg] = value;
            expected_reads += !direct;
        }
        if (op->write) {
            unsigned direct = fixture.write_direct && full;
            uint8_t *p = (direct ? expect_direct : expect_fallback) + address;
            uint32_t value = transfer ? expected[reg] : 0xa5b6c7d8u;
            for (unsigned i=0;i<op->width;++i) p[i] = (uint8_t)(value >> (i * 8u));
            expected_writes += !direct;
        }
    }
    expected[1] += op->writeback;
    if (execution == 2u) prepare_native(cpu);
    check(arm920t_run(cpu,2u),2u,"retirement");
    for (unsigned i=0;i<16u;++i) check(arm920t_get_reg(cpu,i),expected[i],"register");
    check(arm920t_get_cpsr(cpu),0xd3u,"CPSR");
    check(fixture.reads,expected_reads,"read callbacks");
    check(fixture.writes,expected_writes,"write callbacks");
    check((uint32_t)memcmp(expect_direct,fixture.direct,TEST_SIZE),0u,"direct bytes");
    check((uint32_t)memcmp(expect_fallback,fixture.fallback,TEST_SIZE),0u,"callback bytes");
#if ARM920T_NATIVE_BACKEND
    if (execution == 2u) check(cpu->prof.native_arm_insns != 0u,1u,"native execution");
#endif
    native_insns += cpu->prof.native_arm_insns;
    arm920t_destroy(cpu);
    ++cases;
    ++cases_by_mode[execution];
}
int main(void) {
    static const unsigned spans[] = {2u,5u,TEST_SIZE};
    for (unsigned execution=0;execution<3u;++execution)
        for (unsigned operation=0;operation<GP32_ARRAY_COUNT(operations);++operation)
            for (unsigned policy=0;policy<4u;++policy)
                for (unsigned mmu=0;mmu<3u;++mmu)
                    for (unsigned span=0;span<GP32_ARRAY_COUNT(spans);++span)
                        test_case(execution,operation,policy,TEST_DATA,0u,spans[span],mmu,1u,0u);
    static const uint32_t boundaries[] = {
        0x14000000u,0x13ffffe0u,0x15ffffc0u,0x15ffffe0u,0x18001000u
    };
    static const unsigned offsets[] = {0u,28u,48u};
    static const unsigned boundary_ops[] = {0u,6u,11u,12u};
    for (unsigned execution=0;execution<3u;++execution)
        for (unsigned operation=0;operation<GP32_ARRAY_COUNT(boundary_ops);++operation)
            for (unsigned location=0;location<GP32_ARRAY_COUNT(boundaries);++location)
                for (unsigned offset=0;offset<GP32_ARRAY_COUNT(offsets);++offset)
                    for (unsigned mmu=0;mmu<3u;++mmu)
                        test_case(execution,boundary_ops[operation],3u,boundaries[location],
                                  offsets[offset],TEST_SIZE,mmu,1u,0u);
    for (unsigned execution=0;execution<3u;++execution)
        for (unsigned operation=0;operation<GP32_ARRAY_COUNT(operations);++operation) {
            test_case(execution,operation,3u,TEST_DATA,0u,TEST_SIZE,0u,0u,0u);
            test_case(execution,operation,0u,TEST_DATA,0u,TEST_SIZE,0u,1u,1u);
        }
    printf("arm_fastmem_io_test: %u cases, %u failures\n",cases,failures);
    for (unsigned execution=0;execution<3u;++execution)
        printf("mode %u: %u cases, %u failures\n",execution,cases_by_mode[execution],failures_by_mode[execution]);
    printf("native instructions: %" PRIu64 "\n",native_insns);
    return failures != 0;
}
