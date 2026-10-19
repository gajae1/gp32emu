/* A readable fastmem window does not authorize writes. Exercise the public
 * CPU bus through raw, cached and native execution, including mappings whose
 * write view differs from their read view. */
#define main arm_jit_differential_main
#include "arm_jit_test.c"
#undef main

enum write_policy { WRITE_DENIED, WRITE_ALLOWED, WRITE_PARTIAL, WRITE_SPLIT };
typedef struct {
    enum write_policy policy;
    uint8_t *shadow;
    unsigned calls, width[16];
    uint32_t addr[16], value[16];
    unsigned revoke_on_read, revoke_on_write;
} write_bus_t;
static write_bus_t writes[2];

static write_bus_t *write_state(void *u) {
    return &writes[u == &bus_ref];
}
static uint8_t *permission_fastmem(void *u, uint32_t addr, size_t bytes, int write) {
    if (!write) return bus_ptr(u, addr, bytes);
    write_bus_t *w = write_state(u);
    if (w->policy == WRITE_DENIED) return NULL;
    if (w->policy == WRITE_PARTIAL &&
        (addr < DATA_ADDR || addr - DATA_ADDR >= 4u || bytes > 4u - (addr - DATA_ADDR)))
        return NULL;
    if (w->policy == WRITE_SPLIT) {
        if (addr < RAM_BASE || (uint64_t)(addr - RAM_BASE) + bytes > RAM_SIZE) return NULL;
        return w->shadow + (addr - RAM_BASE);
    }
    return bus_ptr(u, addr, bytes);
}
static void record_denied_write(void *u, uint32_t addr, uint32_t value, unsigned width) {
    write_bus_t *w = write_state(u);
    unsigned i = w->calls++;
    CHECK(i < GP32_ARRAY_COUNT(w->addr), "write observation capacity");
    if (i < GP32_ARRAY_COUNT(w->addr)) {
        w->width[i] = width;
        w->addr[i] = addr;
        w->value[i] = value;
    }
    /* This bus deliberately rejects the store. A cached read pointer must
     * not make backing RAM writable or suppress this observable callback. */
    if (w->revoke_on_write) {
        w->revoke_on_write = 0;
        w->policy = WRITE_DENIED;
        arm920t_flush_jit(((test_bus_t *)u)->observe_cpu);
    }
}
static void permission_write8(void *u, uint32_t addr, uint8_t value) {
    record_denied_write(u, addr, value, 1u);
}
static void permission_write16(void *u, uint32_t addr, uint16_t value) {
    record_denied_write(u, addr, value, 2u);
}
static void permission_write32(void *u, uint32_t addr, uint32_t value) {
    record_denied_write(u, addr, value, 4u);
}
static uint32_t permission_read32(void *u, uint32_t addr) {
    write_bus_t *w = write_state(u);
    if (addr == IO_ADDR && w->revoke_on_read) {
        w->revoke_on_read = 0;
        w->policy = WRITE_DENIED;
        arm920t_flush_jit(((test_bus_t *)u)->observe_cpu);
        return 0;
    }
    return tb_read32(u, addr);
}
static void setup_permission_pair(unsigned jit, enum write_policy policy) {
    memset(writes, 0, sizeof(writes));
    setup_pair();
    arm920t_set_trace(cpu_ref, 1, NULL, NULL); /* genuine uncached oracle */
    for (unsigned i = 0; i < 2u; ++i) {
        arm920t_t *c = i ? cpu_ref : cpu_jit;
        test_bus_t *b = i ? &bus_ref : &bus_jit;
        writes[i].policy = policy;
        if (policy == WRITE_SPLIT) {
            writes[i].shadow = calloc(1, RAM_SIZE);
            CHECK(writes[i].shadow != NULL, "split write backing allocation");
            if (!writes[i].shadow) exit(2);
        }
        c->bus.fastmem = permission_fastmem;
        c->bus.read32 = permission_read32;
        c->bus.write8 = permission_write8;
        c->bus.write16 = permission_write16;
        c->bus.write32 = permission_write32;
        b->observe_cpu = c;
        arm920t_set_jit(c, i ? 0 : (int)jit);
    }
    set_reg_both(0u, 0x12345678u);
    set_reg_both(1u, DATA_ADDR);
    set_reg_both(2u, 0x89abcdefu);
    set_reg_both(3u, 0x76543210u);
    set_reg_both(7u, DATA_ADDR + 8u);
    set_reg_both(9u, IO_ADDR);
    set_reg_both(12u, CODE_ADDR + 0x100u);
    set_mem_both(DATA_ADDR, 0xdecafbadu);
}
static void compare_writes(void) {
    CHECK(writes[0].calls == writes[1].calls, "write callback count");
    CHECK(!memcmp(writes[0].width, writes[1].width, sizeof(writes[0].width)), "write widths");
    CHECK(!memcmp(writes[0].addr, writes[1].addr, sizeof(writes[0].addr)), "write addresses");
    CHECK(!memcmp(writes[0].value, writes[1].value, sizeof(writes[0].value)), "write values");
    if (writes[0].shadow)
        CHECK(!memcmp(writes[0].shadow, writes[1].shadow, RAM_SIZE), "separate write backing");
}
static void finish_permission_pair(void) {
    free(writes[0].shadow);
    free(writes[1].shadow);
    teardown_pair();
}
static void run_permission_pair(unsigned jit, uint32_t count) {
    if (jit) prepare_native_block(cpu_jit, arm920t_get_pc(cpu_jit));
    CHECK(arm920t_run(cpu_jit, count) == arm920t_run(cpu_ref, count), "run budget");
    compare_state();
    compare_writes();
#if ARM920T_NATIVE_BACKEND
    if (jit) CHECK(cpu_jit->prof.native_arm_insns != 0u, "fixture executes native instructions");
#else
    GP32_UNUSED(jit);
#endif
}

static void case_store_widths(unsigned jit, enum write_policy policy) {
    static const uint32_t program[] = {
        0xe5810000u, /* STR r0,[r1] */
        0xe5c10004u, /* STRB r0,[r1,#4] */
        0xe1c100b6u, /* STRH r0,[r1,#6] */
        0xe8a7000du, /* STMIA r7!,{r0,r2,r3} */
        0xe1014090u, /* SWP r4,r0,[r1] */
        0xe1415090u, /* SWPB r5,r0,[r1] */
        0xe5916000u, /* LDR r6,[r1] */
        0xe12fff1cu, /* BX r12: keep the native fixture's retired count exact */
    };
    current_case = jit ? "write-permission-native" : "write-permission-cached";
    setup_permission_pair(jit, policy);
    load_both(program, GP32_ARRAY_COUNT(program));
    run_permission_pair(jit, GP32_ARRAY_COUNT(program));
    CHECK(ref_reg(7u) == DATA_ADDR + 20u, "STM writeback completes despite denied writes");
    if (policy == WRITE_DENIED || policy == WRITE_SPLIT)
        CHECK(ref_reg(4u) == 0xdecafbadu && ref_reg(5u) == 0xadu && ref_reg(6u) == 0xdecafbadu,
              "reads retain the independent read view");
    else CHECK(ref_reg(6u) == 0x12345678u, "authorized write reaches RAM");
    CHECK(writes[1].calls == (policy == WRITE_DENIED ? 8u : policy == WRITE_PARTIAL ? 5u : 0u),
          "oracle exercises the expected denied lanes");
    if (policy == WRITE_SPLIT)
        CHECK(gp32_ld32le(writes[1].shadow + DATA_ADDR - RAM_BASE + 16u) == 0x76543210u,
              "stores reach the write view");
    finish_permission_pair();
}

enum refresh_kind { REFRESH_FLUSH, REFRESH_RESET, REFRESH_STATE, REFRESH_ENABLE, REFRESH_CP15 };
static void case_permission_refresh(unsigned jit, enum refresh_kind kind) {
    static const uint32_t program[] = {0xe5810000u, 0xe5912000u, 0xe12fff1cu};
    current_case = jit ? "write-permission-refresh-native" : "write-permission-refresh-cached";
    setup_permission_pair(jit, WRITE_ALLOWED);
    load_both(program, GP32_ARRAY_COUNT(program));
    run_permission_pair(jit, 3u); /* Compile and execute a writable native block. */
    CHECK(ref_reg(2u) == 0x12345678u, "initial mapping is writable");
    writes[0].policy = writes[1].policy = WRITE_DENIED;
    for (unsigned i = 0; i < 2u; ++i) {
        arm920t_t *c = i ? cpu_ref : cpu_jit;
        if (kind == REFRESH_FLUSH) arm920t_flush_jit(c);
        else if (kind == REFRESH_RESET) arm920t_reset(c, CODE_ADDR);
        else if (kind == REFRESH_STATE) {
            arm920t_state_image_t st;
            uint8_t image[sizeof(st) + sizeof(arm920t_translation_state_image_t)];
            state_io_t io = state_io_writer(image, sizeof(image));
            CHECK(arm920t_state_save_io(c, &io), "capture state for mapping refresh");
            memcpy(&st, image, sizeof(st));
            arm920t_state_apply(c, &st);
        } else if (kind == REFRESH_ENABLE) arm920t_set_jit(c, i ? 0 : (int)jit);
    }
    if (kind == REFRESH_CP15) {
        set_mem_both(CODE_ADDR + 0x80u, 0xee070f15u); /* MCR p15,0,r0,c7,c5,0 */
        set_reg_both(15u, CODE_ADDR + 0x80u);
        run_permission_pair(jit, 1u);
    }
    set_reg_both(0u, 0xaabbccddu);
    set_reg_both(1u, DATA_ADDR);
    set_reg_both(12u, CODE_ADDR + 0x100u);
    set_reg_both(15u, CODE_ADDR);
    run_permission_pair(jit, 3u);
    CHECK(ref_reg(2u) == 0x12345678u && writes[1].calls == 1u,
          "revoked mapping uses the rejecting write callback");
    finish_permission_pair();
}

static void case_callback_revoke(unsigned jit) {
    static const uint32_t program[] = {
        0xe5810000u, /* accepted STR */
        0xe5998000u, /* LDR r8,[r9]: callback revokes writes and flushes */
        0xe5812000u, /* rejected STR */
        0xe5916000u,
        0xe12fff1cu,
    };
    current_case = jit ? "write-permission-callback-native" : "write-permission-callback-cached";
    setup_permission_pair(jit, WRITE_ALLOWED);
    load_both(program, GP32_ARRAY_COUNT(program));
    writes[0].revoke_on_read = writes[1].revoke_on_read = 1;
    run_permission_pair(jit, GP32_ARRAY_COUNT(program));
    CHECK(ref_reg(6u) == 0x12345678u && writes[1].calls == 1u,
          "callback revocation takes effect before the next store");
    finish_permission_pair();
}

static void case_stm_callback_revoke(unsigned jit) {
    static const uint32_t program[] = {0xe8a1000du, 0xe12fff1cu}; /* STMIA r1!,{r0,r2,r3}; BX r12 */
    current_case = jit ? "write-permission-STM-callback-native" : "write-permission-STM-callback-cached";
    setup_permission_pair(jit, WRITE_ALLOWED);
    load_both(program, GP32_ARRAY_COUNT(program));
    /* The first lane is outside RAM and uses a bus callback. It revokes the
     * cached writable RAM view before lanes two and three enter that window. */
    set_reg_both(1u, RAM_BASE - 4u);
    writes[0].revoke_on_write = writes[1].revoke_on_write = 1;
    run_permission_pair(jit, GP32_ARRAY_COUNT(program));
    CHECK(writes[1].calls == 3u && ref_reg(1u) == RAM_BASE + 8u,
          "all denied STM lanes and final writeback complete after revocation");
    CHECK(gp32_ld32le(bus_ref.ram) == 0u && gp32_ld32le(bus_ref.ram + 4u) == 0u,
          "revoked STM RAM lanes preserve backing bytes");
    finish_permission_pair();
}

int main(void) {
    for (unsigned jit = 0; jit < 2u; ++jit) {
        for (unsigned policy = WRITE_DENIED; policy <= WRITE_SPLIT; ++policy)
            case_store_widths(jit, (enum write_policy)policy);
        for (unsigned kind = REFRESH_FLUSH; kind <= REFRESH_CP15; ++kind)
            case_permission_refresh(jit, (enum refresh_kind)kind);
        case_callback_revoke(jit);
        case_stm_callback_revoke(jit);
    }
    if (failures) {
        fprintf(stderr, "arm write permission: %d failures\n", failures);
        return 1;
    }
    puts("PASS: ARM cached/native writes honor permission, views and mapping refresh");
    return 0;
}
