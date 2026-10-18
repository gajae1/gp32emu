/* A NULL instruction fastmem result retains the real read callback. Compare
 * uncached ARM execution with cached/native dispatch without pre-fetching a
 * future opcode, suppressing a later callback, or repeating the first fetch.
 * The internal include allows resident/cold I-side setup without guest setup
 * instructions changing the callback-visible state. */
#define GP32EMU_CPU_PROFILE 1
#include "../src/arm920t.c"

#define FETCH_CODE_PA 0x400u
#define FETCH_RAM_PA  0x0c000000u
#define FETCH_VA      0x10000400u
#define FETCH_TABLE  (FETCH_RAM_PA + 0x8000u)
#define FETCH_IO_TABLE 0x14000000u
#define FETCH_EVENTS 64u

enum fetch_policy { FETCH_CALLBACK, FETCH_DIRECT_HEAD, FETCH_DIRECT };
enum fetch_effect {
    FETCH_NO_EFFECT, FETCH_FLUSH, FETCH_LIMIT_ZERO, FETCH_LIMIT_TWO,
    FETCH_STOP, FETCH_PC, FETCH_FLAGS, FETCH_FIQ_MODE, FETCH_THUMB,
    FETCH_IRQ, FETCH_FIQ, FETCH_TRACE
};
typedef struct {
    uint32_t addr, pc, r0, cpsr, width;
} fetch_event_t;
typedef struct {
    arm920t_t *cpu;
    uint8_t *bios, *ram;
    enum fetch_policy policy;
    enum fetch_effect effect;
    unsigned reads, effects, dynamic;
    fetch_event_t events[FETCH_EVENTS];
} fetch_fixture_t;

static unsigned failures;
static unsigned current_mmu, current_native, current_policy, current_effect, current_budget, current_walk;
static void fetch_fail(const char *what, uint32_t got, uint32_t want) {
    if (failures++ < 20u)
        fprintf(stderr, "FAIL[fetch mmu=%u native-request=%u policy=%u effect=%u budget=%u walk=%u] %s: got=%#x want=%#x\n",
            current_mmu, current_native, current_policy, current_effect, current_budget, current_walk, what, got, want);
}
static void fetch_equal(const char *what, uint32_t got, uint32_t want) {
    if (got != want) fetch_fail(what, got, want);
}
static uint8_t *fetch_storage(fetch_fixture_t *b, uint32_t addr, size_t bytes) {
    if (addr < ARM_JIT_BIOS_SIZE_BYTES && bytes <= ARM_JIT_BIOS_SIZE_BYTES - addr)
        return b->bios + addr;
    if (addr >= FETCH_RAM_PA && addr - FETCH_RAM_PA < ARM_JIT_RAM_SIZE_BYTES &&
        bytes <= ARM_JIT_RAM_SIZE_BYTES - (addr - FETCH_RAM_PA))
        return b->ram + addr - FETCH_RAM_PA;
    return NULL;
}
static uint8_t *fetch_fastmem(void *user, uint32_t addr, size_t bytes, int write) {
    fetch_fixture_t *b = user;
    uint32_t first = FETCH_CODE_PA + (b->policy == FETCH_DIRECT_HEAD ? 4u : 0u);
    if (!write && b->policy != FETCH_DIRECT && bytes && addr < FETCH_CODE_PA + 12u &&
        (uint64_t)addr + bytes > first) return NULL;
    return fetch_storage(b, addr, bytes);
}
static void fetch_observe(fetch_fixture_t *b, uint32_t addr, unsigned width) {
    unsigned i = b->reads++;
    if (i < FETCH_EVENTS)
        b->events[i] = (fetch_event_t){addr, arm920t_get_pc(b->cpu),
            arm920t_get_reg(b->cpu, 0u), arm920t_get_cpsr(b->cpu), width};
    if (b->effects || b->effect == FETCH_NO_EFFECT) return;
    ++b->effects;
    switch (b->effect) {
    case FETCH_FLUSH: arm920t_flush_jit(b->cpu); break;
    case FETCH_LIMIT_ZERO: arm920t_limit_run(b->cpu, 0u); break;
    case FETCH_LIMIT_TWO: arm920t_limit_run(b->cpu, 2u); break;
    case FETCH_STOP: arm920t_stop_run(b->cpu); break;
    case FETCH_PC: arm920t_set_reg(b->cpu, 15u, arm920t_get_pc(b->cpu) + 0x80u); break;
    case FETCH_FLAGS: arm920t_set_cpsr(b->cpu, arm920t_get_cpsr(b->cpu) | Z_FLAG); break;
    case FETCH_FIQ_MODE:
        arm920t_set_cpsr(b->cpu, (arm920t_get_cpsr(b->cpu) & ~MODE_MASK) | MODE_FIQ);
        break;
    case FETCH_THUMB: arm920t_set_cpsr(b->cpu, arm920t_get_cpsr(b->cpu) | T_FLAG); break;
    case FETCH_IRQ: arm920t_set_irq(b->cpu, 1); break;
    case FETCH_FIQ: arm920t_set_fiq(b->cpu, 1); break;
    case FETCH_TRACE: arm920t_set_trace(b->cpu, 1, NULL, NULL); break;
    default: break;
    }
}
static uint32_t fetch_read32(void *user, uint32_t addr) {
    fetch_fixture_t *b = user;
    fetch_observe(b, addr, 4u);
    if (current_walk && (addr == FETCH_IO_TABLE || addr == FETCH_IO_TABLE + 0x400u))
        return 2u; /* callback-backed section descriptor maps BIOS */
    if (b->dynamic && addr == FETCH_CODE_PA + 4u)
        return 0xe2811000u | (arm920t_get_reg(b->cpu, 0u) & 0xffu); /* ADD r1,r1,#live r0 */
    uint8_t *p = fetch_storage(b, addr, 4u);
    return p ? gp32_ld32le(p) : 0xeafffffeu;
}
static uint16_t fetch_read16(void *user, uint32_t addr) {
    fetch_fixture_t *b = user;
    fetch_observe(b, addr, 2u);
    uint8_t *p = fetch_storage(b, addr, 2u);
    return p ? gp32_ld16le(p) : 0xe7feu;
}
static uint8_t fetch_read8(void *user, uint32_t addr) {
    fetch_fixture_t *b = user;
    fetch_observe(b, addr, 1u);
    uint8_t *p = fetch_storage(b, addr, 1u);
    return p ? *p : 0u;
}
static void fetch_write32(void *user, uint32_t addr, uint32_t value) {
    uint8_t *p = fetch_storage(user, addr, 4u);
    if (p) gp32_st32le(p, value);
}
static void fetch_write16(void *user, uint32_t addr, uint16_t value) {
    uint8_t *p = fetch_storage(user, addr, 2u);
    if (p) gp32_st16le(p, value);
}
static void fetch_write8(void *user, uint32_t addr, uint8_t value) {
    uint8_t *p = fetch_storage(user, addr, 1u);
    if (p) *p = value;
}

static void fetch_setup(fetch_fixture_t *b, int raw) {
    memset(b, 0, sizeof(*b));
    b->policy = (enum fetch_policy)current_policy;
    b->effect = (enum fetch_effect)current_effect;
    b->dynamic = b->policy != FETCH_DIRECT;
    b->bios = calloc(1u, ARM_JIT_BIOS_SIZE_BYTES);
    b->ram = calloc(1u, ARM_JIT_RAM_SIZE_BYTES);
    if (!b->bios || !b->ram) exit(2);
    uint32_t first = b->effect == FETCH_THUMB ? 0xe1a0f006u :
        b->effect == FETCH_FLAGS ? 0x02800001u :
        b->effect == FETCH_FIQ_MODE ? 0xe1a04008u : 0xe2800001u;
    gp32_st32le(b->bios + FETCH_CODE_PA, first);
    gp32_st32le(b->bios + FETCH_CODE_PA + 4u, 0xe2811001u); /* ADD r1,r1,#1 */
    gp32_st32le(b->bios + FETCH_CODE_PA + 8u, 0xeafffffcu); /* B first */
    gp32_st16le(b->bios + FETCH_CODE_PA + 0x40u, 0x2455u); /* Thumb MOV r4,#0x55 */
    gp32_st16le(b->bios + FETCH_CODE_PA + 0x42u, 0xe7feu); /* Thumb B . */
    gp32_st32le(b->bios + FETCH_CODE_PA + 0x80u, 0xeafffffeu); /* stray-PC sentinel */
    /* Both code aliases resolve to BIOS. The table walk itself is direct and
     * must not add any instruction-read observation. */
    gp32_st32le(b->ram + (FETCH_TABLE - FETCH_RAM_PA), 2u);
    gp32_st32le(b->ram + (FETCH_TABLE - FETCH_RAM_PA) + 0x400u, 2u);
    arm_bus_t bus = {.user = b, .read8 = fetch_read8, .read16 = fetch_read16,
        .read32 = fetch_read32, .write8 = fetch_write8, .write16 = fetch_write16,
        .write32 = fetch_write32, .fastmem = fetch_fastmem};
    b->cpu = arm920t_create(&bus);
    if (!b->cpu) exit(2);
    uint32_t entry = current_mmu ? FETCH_VA : FETCH_CODE_PA;
    arm920t_reset(b->cpu, entry);
    arm920t_set_reg(b->cpu, 6u, entry + 0x40u);
    arm920t_set_reg(b->cpu, 8u, 0x11223344u);
    b->cpu->bank_fiq[0] = 0x55667788u;
    if (b->effect == FETCH_IRQ || b->effect == FETCH_FIQ)
        arm920t_set_cpsr(b->cpu, arm920t_get_cpsr(b->cpu) &
            ~(b->effect == FETCH_IRQ ? I_FLAG : F_FLAG));
    if (current_mmu) {
        b->cpu->cp15[1] |= 1u;
        b->cpu->cp15[2] = current_walk ? FETCH_IO_TABLE : FETCH_TABLE;
        if (current_mmu == 2u) itlb_store(b->cpu, entry, 0u, 0xfffffu);
    }
    arm920t_set_jit(b->cpu, raw ? 0 : (int)current_native);
    if (raw) arm920t_set_trace(b->cpu, 1, NULL, NULL);
}
static void fetch_compare(const fetch_fixture_t *got, const fetch_fixture_t *want) {
    for (unsigned r = 0; r < 16u; ++r) {
        char name[16];
        snprintf(name, sizeof(name), "r%u", r);
        fetch_equal(name, arm920t_get_reg(got->cpu, r), arm920t_get_reg(want->cpu, r));
    }
    fetch_equal("CPSR", arm920t_get_cpsr(got->cpu), arm920t_get_cpsr(want->cpu));
    fetch_equal("cycles", (uint32_t)arm920t_get_cycles(got->cpu), (uint32_t)arm920t_get_cycles(want->cpu));
    fetch_equal("callback count", got->reads, want->reads);
    fetch_equal("callback effects", got->effects, want->effects);
    unsigned n = want->reads < FETCH_EVENTS ? want->reads : FETCH_EVENTS;
    for (unsigned i = 0; i < n; ++i) {
        const fetch_event_t *g = &got->events[i], *w = &want->events[i];
        fetch_equal("callback address", g->addr, w->addr);
        fetch_equal("callback PC", g->pc, w->pc);
        fetch_equal("callback r0", g->r0, w->r0);
        fetch_equal("callback CPSR", g->cpsr, w->cpsr);
        fetch_equal("callback width", g->width, w->width);
    }
}
static void fetch_destroy(fetch_fixture_t *b) {
    arm920t_destroy(b->cpu);
    free(b->bios);
    free(b->ram);
}
static void fetch_case(void) {
    fetch_fixture_t got, want;
    fetch_setup(&got, 0);
    fetch_setup(&want, 1);
    uint32_t dg = arm920t_run(got.cpu, current_budget);
    uint32_t dw = arm920t_run(want.cpu, current_budget);
    fetch_equal("retired instructions", dg, dw);
    fetch_compare(&got, &want);
    if (current_effect == FETCH_LIMIT_ZERO || current_effect == FETCH_STOP)
        fetch_equal("in-flight instruction completes", dw, 1u);
    if (current_effect == FETCH_NO_EFFECT && current_budget == 6u) {
        fetch_equal("raw ADD iterations", arm920t_get_reg(want.cpu, 0u), 2u);
        fetch_equal("raw dynamic sum", arm920t_get_reg(want.cpu, 1u),
            current_policy == FETCH_DIRECT ? 2u : 3u);
    }
    if (current_effect == FETCH_FIQ_MODE)
        fetch_equal("live bank reaches instruction", arm920t_get_reg(want.cpu, 4u), 0x55667788u);
    if (current_effect == FETCH_THUMB)
        fetch_equal("ARM fetch finishes before Thumb", arm920t_get_reg(want.cpu, 4u), 0x55u);
    if (current_effect == FETCH_IRQ || current_effect == FETCH_FIQ)
        fetch_equal("fetch finishes before interrupt", arm920t_get_reg(want.cpu, 0u), 1u);
    /* A second ragged run proves that callback state does not become a cached
     * opcode and that stop/deadline effects were scoped to the first run. */
    fetch_equal("following run budget", arm920t_run(got.cpu, 5u), arm920t_run(want.cpu, 5u));
    fetch_compare(&got, &want);
#if ARM920T_NATIVE_BACKEND
    if (current_native && current_policy == FETCH_DIRECT && current_effect == FETCH_NO_EFFECT &&
        current_budget == 17u && !current_walk)
        fetch_equal("direct control runs native code", got.cpu->prof.native_arm_insns != 0u, 1u);
#endif
    fetch_destroy(&got);
    fetch_destroy(&want);
}

int main(int argc, char **argv) {
    int walk_only = argc == 2 && !strcmp(argv[1], "--walk");
    static const unsigned budgets[] = {1u, 2u, 6u, 17u};
    for (current_mmu = 0; !walk_only && current_mmu < 3u; ++current_mmu)
        for (current_native = 0; current_native < 2u; ++current_native) {
            current_effect = FETCH_NO_EFFECT;
            for (current_policy = 0; current_policy <= FETCH_DIRECT; ++current_policy)
                for (unsigned i = 0; i < GP32_ARRAY_COUNT(budgets); ++i) {
                    current_budget = budgets[i];
                    fetch_case();
                }
            current_policy = FETCH_CALLBACK;
            current_budget = 4u;
            for (current_effect = FETCH_FLUSH; current_effect <= FETCH_TRACE; ++current_effect)
                fetch_case();
        }
    /* Even when the opcode itself is direct, a page-table read callback is
     * part of the in-flight fetch. Its deadline/PC/CPSR changes cannot cause
     * that already-fetched ARM instruction to disappear. */
    current_walk = 1u;
    current_mmu = 1u;
    current_policy = FETCH_DIRECT;
    current_budget = 4u;
    for (current_native = 0; current_native < 2u; ++current_native)
        for (current_effect = FETCH_NO_EFFECT; current_effect <= FETCH_TRACE; ++current_effect)
            fetch_case();
    if (failures) {
        fprintf(stderr, "arm_fetch_callback: %u failures\n", failures);
        return 1;
    }
    puts("arm_fetch_callback: dynamic opcodes, fetch order, flush, deadline, interrupts, PC/CPSR transitions passed");
    return 0;
}
