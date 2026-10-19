/* A direct BIOS prefix is not a certificate for the complete BIOS window.
 * The callbacks deliberately expose different data outside that prefix, so
 * bypassing one changes architectural results as well as access counts. */
#define main arm_jit_differential_main
#include "arm_jit_test.c"
#undef main

#define READ_ADDR 0x00020000u
typedef struct {
    size_t direct_bytes;
    unsigned count;
    unsigned width[16];
    uint32_t addr[16];
} read_window_t;
static read_window_t reads[2];

static read_window_t *read_state(void *u) { return &reads[u == &bus_ref]; }
static uint8_t *read_window_fastmem(void *u, uint32_t a, size_t bytes, int write) {
    if (a < BIOS_SIZE) {
        size_t limit = read_state(u)->direct_bytes;
        if (write || a > limit || bytes > limit - a) return NULL;
    }
    return bus_ptr(u, a, bytes);
}
static void record_read(void *u, uint32_t a, unsigned width) {
    read_window_t *r = read_state(u);
    unsigned n = r->count++;
    CHECK(n < GP32_ARRAY_COUNT(r->width), "read observation capacity");
    if (n < GP32_ARRAY_COUNT(r->width)) { r->width[n] = width; r->addr[n] = a; }
}
static uint8_t callback_byte(test_bus_t *b, uint32_t a) {
    return (uint8_t)(b->bios[a] ^ 0x5au);
}
static uint8_t read_window_read8(void *u, uint32_t a) {
    if (a < BIOS_SIZE && a >= read_state(u)->direct_bytes) {
        record_read(u, a, 1u);
        return callback_byte(u, a);
    }
    return tb_read8(u, a);
}
static uint16_t read_window_read16(void *u, uint32_t a) {
    if (a < BIOS_SIZE - 1u && a + 2u > read_state(u)->direct_bytes) {
        record_read(u, a, 2u);
        return (uint16_t)callback_byte(u, a) | (uint16_t)((uint16_t)callback_byte(u, a + 1u) << 8);
    }
    return tb_read16(u, a);
}
static uint32_t read_window_read32(void *u, uint32_t a) {
    if (a < BIOS_SIZE - 3u && a + 4u > read_state(u)->direct_bytes) {
        record_read(u, a, 4u);
        uint32_t v = 0;
        for (unsigned i = 0; i < 4u; ++i) v |= (uint32_t)callback_byte(u, a + i) << (8u * i);
        return v;
    }
    return tb_read32(u, a);
}

static const uint32_t loads[] = {
    0xe5980000u, /* LDR r0,[r8] */
    0xe5981001u, /* LDR r1,[r8,#1]: ARMv4 rotated word */
    0xe5d82003u, /* LDRB r2,[r8,#3] */
    0xe1d830b2u, /* LDRH r3,[r8,#2] */
    0xe1d840d0u, /* LDRSB r4,[r8] */
    0xe1d850f0u, /* LDRSH r5,[r8] */
    0xe89800c0u, /* LDMIA r8,{r6,r7} */
    0xe12fff1cu, /* BX r12 */
};
static void reset_load_registers(void) {
    for (unsigned r = 0; r < 8u; ++r) set_reg_both(r, 0u);
    set_reg_both(8u, READ_ADDR);
    set_reg_both(12u, CODE_ADDR + 0x100u);
    set_reg_both(15u, CODE_ADDR);
}
static void run_loads(unsigned jit, int partial) {
    reset_load_registers();
    for (unsigned i = 0; i < 2u; ++i) {
        reads[i].count = 0;
        memset(reads[i].width, 0, sizeof(reads[i].width));
        memset(reads[i].addr, 0, sizeof(reads[i].addr));
    }
    /* Full direct mappings retain real native coverage. A partial mapping
     * may use the existing portable fallback instead. */
    if (jit && !partial) prepare_native_block(cpu_jit, CODE_ADDR);
    CHECK(arm920t_run(cpu_jit, GP32_ARRAY_COUNT(loads)) == GP32_ARRAY_COUNT(loads), "cached retirement");
    CHECK(arm920t_run(cpu_ref, GP32_ARRAY_COUNT(loads)) == GP32_ARRAY_COUNT(loads), "raw retirement");
    compare_state();
    CHECK(reads[0].count == reads[1].count, "read callback count");
    CHECK(!memcmp(reads[0].width, reads[1].width, sizeof(reads[0].width)), "read callback widths");
    CHECK(!memcmp(reads[0].addr, reads[1].addr, sizeof(reads[0].addr)), "read callback addresses");
    CHECK(reads[1].count == (partial ? 8u : 0u), "oracle callback coverage");
    CHECK(ref_reg(0) == (partial ? 0x8877ff80u : 0xd22da5dau), "word result");
    CHECK(ref_reg(1) == (partial ? 0x808877ffu : 0xdad22da5u), "rotated word result");
    CHECK(ref_reg(2) == (partial ? 0x88u : 0xd2u), "byte result");
    CHECK(ref_reg(3) == (partial ? 0x8877u : 0xd22du), "halfword result");
    CHECK(ref_reg(4) == (partial ? 0xffffff80u : 0xffffffdau), "signed byte result");
    CHECK(ref_reg(5) == (partial ? 0xffffff80u : 0xffffa5dau), "signed halfword result");
    CHECK(ref_reg(6) == ref_reg(0) && ref_reg(7) == (partial ? 0x78563412u : 0x220c6e48u), "multiple load result");
#if ARM920T_NATIVE_BACKEND
    if (jit && !partial) CHECK(cpu_jit->prof.native_arm_insns != 0u, "full window native coverage");
#endif
}

enum refresh { INITIAL_PARTIAL, FLUSH, RESET, STATE_APPLY, JIT_RECONFIGURE };
static void case_read_window(unsigned jit, enum refresh refresh) {
    current_case = jit ? "BIOS-read-window-native-request" : "BIOS-read-window-cached";
    setup_pair();
    memset(reads, 0, sizeof(reads));
    load_both(loads, GP32_ARRAY_COUNT(loads));
    set_mem_both(READ_ADDR, 0xd22da5dau);
    set_mem_both(READ_ADDR + 4u, 0x220c6e48u);
    arm920t_set_trace(cpu_ref, 1, NULL, NULL);
    for (unsigned i = 0; i < 2u; ++i) {
        arm920t_t *c = i ? cpu_ref : cpu_jit;
        reads[i].direct_bytes = refresh == INITIAL_PARTIAL ? 0x10000u : BIOS_SIZE;
        c->bus.fastmem = read_window_fastmem;
        c->bus.read8 = read_window_read8;
        c->bus.read16 = read_window_read16;
        c->bus.read32 = read_window_read32;
        arm920t_set_jit(c, i ? 0 : (int)jit);
    }
    if (refresh != INITIAL_PARTIAL) {
        run_loads(jit, 0);
        for (unsigned i = 0; i < 2u; ++i) {
            arm920t_t *c = i ? cpu_ref : cpu_jit;
            reads[i].direct_bytes = 0x10000u;
            if (refresh == FLUSH) arm920t_flush_jit(c);
            else if (refresh == RESET) arm920t_reset(c, CODE_ADDR);
            else if (refresh == STATE_APPLY) {
                arm920t_state_image_t st;
                uint8_t bytes[sizeof(st) + sizeof(arm920t_translation_state_image_t)];
                state_io_t io = state_io_writer(bytes, sizeof(bytes));
                CHECK(arm920t_state_save_io(c, &io), "capture CPU state");
                memcpy(&st, bytes, sizeof(st));
                arm920t_state_apply(c, &st);
            } else arm920t_set_jit(c, i ? 0 : (int)jit);
        }
    }
    run_loads(jit, 1);
    run_loads(jit, 1); /* already decoded block must keep per-access callbacks */
    teardown_pair();
}
int main(void) {
    for (unsigned jit = 0; jit < 2u; ++jit)
        for (unsigned refresh = INITIAL_PARTIAL; refresh <= JIT_RECONFIGURE; ++refresh)
            case_read_window(jit, (enum refresh)refresh);
    if (failures) { fprintf(stderr, "BIOS read window: %d failures\n", failures); return 1; }
    puts("PASS: 10 BIOS window scenarios preserve load callbacks, mapping refresh and full-window native execution");
    return 0;
}
