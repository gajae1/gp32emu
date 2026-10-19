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
    unsigned read_direct, write_direct, reads, writes;
} io_fixture_t;
static io_fixture_t fixture;
static unsigned failures, cases;
static unsigned current_mode, current_operation, current_policy;

static void check(uint32_t got, uint32_t want, const char *what) {
    if (got == want) return;
    if (failures < 32u)
        fprintf(stderr, "mode=%u operation=%u policy=%u %s: got=%08x want=%08x\n",
                current_mode, current_operation, current_policy, what, got, want);
    ++failures;
}
static uint8_t *normal_ptr(io_fixture_t *f, uint32_t a, size_t bytes) {
    if (a < sizeof(f->bios) && bytes <= sizeof(f->bios) - a) return f->bios + a;
    uint32_t offset = a - ARM_JIT_RAM_BASE_ADDR;
    if (offset < sizeof(f->ram) && bytes <= sizeof(f->ram) - offset) return f->ram + offset;
    return NULL;
}
static uint8_t *test_fastmem(void *user, uint32_t a, size_t bytes, int write) {
    io_fixture_t *f = user;
    uint32_t offset = a - TEST_DATA;
    if (offset < TEST_SIZE && bytes <= TEST_SIZE - offset)
        return (write ? f->write_direct : f->read_direct) ? f->direct + offset : NULL;
    return normal_ptr(f, a, bytes);
}
static uint32_t read_value(void *user, uint32_t a, unsigned width) {
    io_fixture_t *f = user;
    uint32_t value = 0, offset = a - TEST_DATA;
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
    uint32_t offset = a - TEST_DATA;
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

typedef struct { uint32_t opcode; unsigned width, read, write, sign, rotation; } operation_t;
static const operation_t operations[] = {
    {0xe5910000u,4u,1u,0u,0u,0u}, /* LDR r0,[r1] */
    {0xe5910001u,4u,1u,0u,0u,8u}, /* LDR r0,[r1,#1] */
    {0xe5d10000u,1u,1u,0u,0u,0u}, /* LDRB r0,[r1] */
    {0xe1d100b0u,2u,1u,0u,0u,0u}, /* LDRH r0,[r1] */
    {0xe1d100d0u,1u,1u,0u,1u,0u}, /* LDRSB r0,[r1] */
    {0xe1d100f0u,2u,1u,0u,1u,0u}, /* LDRSH r0,[r1] */
    {0xe5810000u,4u,0u,1u,0u,0u}, /* STR r0,[r1] */
    {0xe5c10000u,1u,0u,1u,0u,0u}, /* STRB r0,[r1] */
    {0xe1c100b0u,2u,0u,1u,0u,0u}, /* STRH r0,[r1] */
    {0xe1012090u,4u,1u,1u,0u,0u}, /* SWP r2,r0,[r1] */
    {0xe1412090u,1u,1u,1u,0u,0u}, /* SWPB r2,r0,[r1] */
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
static void test_case(unsigned execution, unsigned operation, unsigned policy) {
    current_mode = execution; current_operation = operation; current_policy = policy;
    memset(&fixture,0,sizeof(fixture));
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
        .read32_io=test_read32,.write32_io=test_write32,
        .fastmem=test_fastmem,.user=&fixture};
    arm920t_t *cpu = arm920t_create(&bus);
    if (!cpu) exit(2);
    arm920t_set_jit(cpu,execution == 2u);
    if (!execution) arm920t_set_trace(cpu,1,NULL,NULL);
    uint32_t expected[16];
    for (unsigned i=0;i<16u;++i) expected[i] = 0x10203040u + i;
    expected[0] = 0xa5b6c7d8u; expected[1] = TEST_DATA;
    expected[12] = TEST_CODE + 0x100u; expected[15] = TEST_CODE;
    for (unsigned i=0;i<16u;++i) arm920t_set_reg(cpu,i,expected[i]);
    expected[15] = expected[12];
    uint8_t expect_direct[TEST_SIZE],expect_fallback[TEST_SIZE];
    memcpy(expect_direct,fixture.direct,sizeof(expect_direct));
    memcpy(expect_fallback,fixture.fallback,sizeof(expect_fallback));
    if (op->read) {
        const uint8_t *p = fixture.read_direct ? expect_direct : expect_fallback;
        uint32_t value = 0;
        for (unsigned i=0;i<op->width;++i) value += (uint32_t)p[i] << (i * 8u);
        if (op->sign && p[op->width-1u] >= 128u)
            value |= op->width == 1u ? 0xffffff00u : 0xffff0000u;
        for (unsigned i=0;i<op->rotation;++i) value = value / 2u + (value % 2u ? 0x80000000u : 0u);
        expected[op->write ? 2u : 0u] = value;
    }
    if (op->write) {
        uint8_t *p = fixture.write_direct ? expect_direct : expect_fallback;
        for (unsigned i=0;i<op->width;++i) p[i] = (uint8_t)(0xa5b6c7d8u >> (i * 8u));
    }
    if (execution == 2u) prepare_native(cpu);
    check(arm920t_run(cpu,2u),2u,"retirement");
    for (unsigned i=0;i<16u;++i) check(arm920t_get_reg(cpu,i),expected[i],"register");
    check(arm920t_get_cpsr(cpu),0xd3u,"CPSR");
    check(fixture.reads,op->read && !fixture.read_direct,"read callbacks");
    check(fixture.writes,op->write && !fixture.write_direct,"write callbacks");
    check((uint32_t)memcmp(expect_direct,fixture.direct,TEST_SIZE),0u,"direct bytes");
    check((uint32_t)memcmp(expect_fallback,fixture.fallback,TEST_SIZE),0u,"callback bytes");
#if ARM920T_NATIVE_BACKEND
    if (execution == 2u) check(cpu->prof.native_arm_insns != 0u,1u,"native execution");
#endif
    arm920t_destroy(cpu);
    ++cases;
}
/* Isolated native word subset of the initial independent fastmem oracle.
 * The complete expanded regression is tests/arm_fastmem_io_test.c. */
int main(void) {
    for (unsigned operation=0;operation<GP32_ARRAY_COUNT(operations);++operation)
        if (operations[operation].width == 4u)
            for (unsigned policy=0;policy<4u;++policy)
                test_case(2u,operation,policy);
    printf("native_fastmem_word_test: %u cases, %u failures\n",cases,failures);
    return failures != 0;
}
