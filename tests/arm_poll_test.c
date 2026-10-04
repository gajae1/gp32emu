/* Black-box equivalence tests for the stable-poll fast-forward path.
 *
 * Every case runs the same ARMv4 program, data, and run budgets through two
 * public-API arm920t instances. The "fast" CPU's bus exposes
 * is_stable_read32; the reference CPU's bus leaves it NULL. r[0..15], PC,
 * CPSR, and the total cycle count must match at every budget boundary; only
 * the number of observed bus reads may differ when the fast-forward engages.
 */

#include "arm920t.h"
#include "s3c2400.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BIOS_SIZE 0x10000u            /* mapped at physical 0x00000000 */
#define RAM_BASE  0x0c000000u         /* GP32 SDRAM window */
#define RAM_SIZE  0x40000u
#define CODE_ADDR 0x00000400u         /* program base inside the BIOS region */
#define LIT_ADDR  0x00000800u         /* literal word, outside fastmem coverage */
#define DATA_ADDR (RAM_BASE + 0x1000u)
#define PTR_ADDR  (RAM_BASE + 0x0100u)
#define STORE_ADDR (RAM_BASE + 0x2000u)
#define STACK_TOP (RAM_BASE + 0x8000u)
#define MMIO_BASE 0x14000000u         /* S3C2400-style register window, no memory */
#define IRQ_VECTOR 0x18u
#define CPSR_MODE_MASK 0x1fu
#define CPSR_I_FLAG 0x80u
#define MODE_IRQ_VALUE 0x12u
#define MAX_STABLE 8u

typedef struct {
    arm_bus_t bus;
    uint8_t bios[BIOS_SIZE];
    uint8_t ram[RAM_SIZE];
    int fastmem_bios;
    int fastmem_ram;
    int reject_fastmem_writes;             /* reads still expose backing RAM */
    uint32_t no_fast_lo, no_fast_hi;  /* hole: fastmem declines this range */
    int stable_all;                        /* callback answers 1 for every word */
    uint32_t stable_addrs[MAX_STABLE];     /* words reported stable */
    unsigned nstable;
    uint32_t mmio_word;                    /* single latched MMIO cell */
    uint64_t r8, r16, r32, w8, w16, w32, stable_queries;
} test_bus_t;

static test_bus_t bus_fast, bus_ref;
static arm920t_t *cpu_fast, *cpu_ref;
static const char *current_case = "init";
static int failures;

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL[%s]: %s (line %d)\n", current_case, message, __LINE__); \
        ++failures; \
    } \
} while (0)

static uint32_t ld32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t ld16le(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
static void st32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void st16le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}

static uint8_t *bus_ptr(test_bus_t *b, uint32_t a, size_t bytes) {
    if (a < BIOS_SIZE && bytes <= BIOS_SIZE - a) return b->bios + a;
    if (a >= RAM_BASE && a - RAM_BASE <= RAM_SIZE && bytes <= RAM_SIZE - (a - RAM_BASE))
        return b->ram + (a - RAM_BASE);
    return NULL;
}

static uint8_t tb_read8(void *u, uint32_t a) {
    test_bus_t *b = (test_bus_t *)u; b->r8++;
    uint8_t *p = bus_ptr(b, a, 1u);
    return p ? p[0] : 0xffu;
}
static uint16_t tb_read16(void *u, uint32_t a) {
    test_bus_t *b = (test_bus_t *)u; b->r16++;
    uint8_t *p = bus_ptr(b, a, 2u);
    return p ? ld16le(p) : 0xffffu;
}
static uint32_t tb_read32(void *u, uint32_t a) {
    test_bus_t *b = (test_bus_t *)u; b->r32++;
    uint8_t *p = bus_ptr(b, a, 4u);
    if (p) return ld32le(p);
    if (a == MMIO_BASE) return b->mmio_word;
    return 0xffffffffu;
}
static void tb_write8(void *u, uint32_t a, uint8_t v) {
    test_bus_t *b = (test_bus_t *)u; b->w8++;
    uint8_t *p = bus_ptr(b, a, 1u);
    if (p) p[0] = v;
}
static void tb_write16(void *u, uint32_t a, uint16_t v) {
    test_bus_t *b = (test_bus_t *)u; b->w16++;
    uint8_t *p = bus_ptr(b, a, 2u);
    if (p) st16le(p, v);
}
static void tb_write32(void *u, uint32_t a, uint32_t v) {
    test_bus_t *b = (test_bus_t *)u; b->w32++;
    uint8_t *p = bus_ptr(b, a, 4u);
    if (p) { st32le(p, v); return; }
    if (a == MMIO_BASE) b->mmio_word = v;
}
static uint8_t *tb_fastmem(void *u, uint32_t a, size_t bytes, int write) {
    test_bus_t *b = (test_bus_t *)u;
    if (write && b->reject_fastmem_writes) return NULL;
    if (a >= b->no_fast_lo && a < b->no_fast_hi) return NULL;
    if (!b->fastmem_bios && a < BIOS_SIZE) return NULL;
    if (!b->fastmem_ram && a >= RAM_BASE) return NULL;
    return bus_ptr(b, a, bytes);
}
static int tb_stable(void *u, uint32_t a) {
    test_bus_t *b = (test_bus_t *)u;
    b->stable_queries++;
    if (b->stable_all) return 1;
    for (unsigned i = 0; i < b->nstable; ++i)
        if (b->stable_addrs[i] == a) return 1;
    return 0;
}

static void add_stable(test_bus_t *b, uint32_t addr) {
    if (b->nstable < MAX_STABLE) b->stable_addrs[b->nstable++] = addr;
}

static void load_words(test_bus_t *b, uint32_t addr, const uint32_t *words, size_t n) {
    for (size_t i = 0; i < n; ++i) st32le(bus_ptr(b, addr + (uint32_t)i * 4u, 4u), words[i]);
}

static void setup_pair(int fastmem_ram) {
    for (test_bus_t *b = &bus_fast; b; b = (b == &bus_fast) ? &bus_ref : NULL) {
        memset(b, 0, sizeof(*b));
        for (uint32_t a = 0; a < BIOS_SIZE; a += 4u)
            st32le(b->bios + a, 0xEAFFFFFEu); /* B . keeps stray control flow bounded */
        b->bus.read8 = tb_read8;
        b->bus.read16 = tb_read16;
        b->bus.read32 = tb_read32;
        b->bus.write8 = tb_write8;
        b->bus.write16 = tb_write16;
        b->bus.write32 = tb_write32;
        b->bus.fastmem = tb_fastmem;
        b->bus.user = b;
        b->fastmem_bios = 1;   /* code fetches bypass read32; data reads stay countable */
        b->fastmem_ram = fastmem_ram;
    }
    bus_fast.bus.is_stable_read32 = tb_stable;
    bus_ref.bus.is_stable_read32 = NULL;
    cpu_fast = arm920t_create(&bus_fast.bus);
    cpu_ref = arm920t_create(&bus_ref.bus);
    if (!cpu_fast || !cpu_ref) { fprintf(stderr, "arm920t_create failed\n"); exit(2); }
    arm920t_reset(cpu_fast, CODE_ADDR);
    arm920t_reset(cpu_ref, CODE_ADDR);
}

static void teardown_pair(void) {
    arm920t_destroy(cpu_fast);
    arm920t_destroy(cpu_ref);
    cpu_fast = cpu_ref = NULL;
}

static void set_reg_both(unsigned reg, uint32_t value) {
    arm920t_set_reg(cpu_fast, reg, value);
    arm920t_set_reg(cpu_ref, reg, value);
}
static void set_cpsr_both(uint32_t value) {
    arm920t_set_cpsr(cpu_fast, value);
    arm920t_set_cpsr(cpu_ref, value);
}
static void store_both(uint32_t addr, uint32_t value) {
    st32le(bus_ptr(&bus_fast, addr, 4u), value);
    st32le(bus_ptr(&bus_ref, addr, 4u), value);
}

static void compare_state(int with_ram) {
    for (unsigned i = 0; i < 16u; ++i)
        CHECK(arm920t_get_reg(cpu_fast, i) == arm920t_get_reg(cpu_ref, i), "register mismatch");
    CHECK(arm920t_get_pc(cpu_fast) == arm920t_get_pc(cpu_ref), "PC mismatch");
    CHECK(arm920t_get_cpsr(cpu_fast) == arm920t_get_cpsr(cpu_ref), "CPSR mismatch");
    CHECK(arm920t_get_cycles(cpu_fast) == arm920t_get_cycles(cpu_ref), "cycle count mismatch");
    if (with_ram)
        CHECK(!memcmp(bus_fast.ram, bus_ref.ram, RAM_SIZE), "RAM image mismatch");
}

/* Runs identical budget chunks on both CPUs and compares the full visible
 * state at every boundary. */
static void run_chunks(const uint32_t *chunks, size_t n, int with_ram,
                       uint64_t *fast_r32, uint64_t *ref_r32) {
    for (size_t i = 0; i < n; ++i) {
        uint32_t df = arm920t_run(cpu_fast, chunks[i]);
        uint32_t dr = arm920t_run(cpu_ref, chunks[i]);
        CHECK(df == dr, "arm920t_run consumed different budgets");
        compare_state(with_ram);
    }
    if (fast_r32) *fast_r32 = bus_fast.r32;
    if (ref_r32) *ref_r32 = bus_ref.r32;
}

/* Mixed small/large budgets totalling >2500 cycles so the poll loop spins
 * for well over 1000 cycles across many boundary alignments. */
static const uint32_t CHUNKS[] = {1u, 2u, 3u, 4u, 5u, 7u, 11u, 13u, 17u,
                                  64u, 128u, 257u, 500u, 513u, 999u};
#define NCHUNKS (sizeof(CHUNKS) / sizeof(CHUNKS[0]))

/* LDR r0,[r1]; CMP r0,#0; BEQ back; B . terminal on exit. */
static const uint32_t PROG_POLL[] = {
    0xE5910000u, 0xE3500000u, 0x0AFFFFFCu, 0xEAFFFFFEu
};
/* LDR r0,[pc,#0x3f8]; CMP r0,#0; BEQ back; B .; literal word at LIT_ADDR. */
static const uint32_t PROG_LIT[] = {
    0xE59F03F8u, 0xE3500000u, 0x0AFFFFFCu, 0xEAFFFFFEu
};
/* BL helper; CMP r0,#0; BEQ back; B .  helper: PUSH {lr}; LDR r0,[r1]; POP {pc}. */
static const uint32_t PROG_BL[] = {
    0xEB000002u, 0xE3500000u, 0x0AFFFFFCu, 0xEAFFFFFEu,
    0xE92D4000u, 0xE5910000u, 0xE8BD8000u
};
/* LDR r2,[r1]; LDR r0,[r2]; CMP r0,#0; BEQ back: a two-hop pointer chain. */
static const uint32_t PROG_CHAIN[] = {
    0xE5912000u, 0xE5920000u, 0xE3500000u, 0x0AFFFFFBu, 0xEAFFFFFEu
};
/* LDR r0,[r1],#4; CMP r0,#0; BEQ back: base advances on every iteration. */
static const uint32_t PROG_WALK[] = {
    0xE4910004u, 0xE3500000u, 0x0AFFFFFCu, 0xEAFFFFFEu
};
/* LDR r0,[r1]; ADD r1,r1,#4; LDR r2,[r1]; SUB r1,r1,#4; CMP r0,#0; BEQ back:
 * the base register is temporarily modified and restored inside the loop. */
static const uint32_t PROG_BASETMP[] = {
    0xE5910000u, 0xE2811004u, 0xE5912000u, 0xE2411004u,
    0xE3500000u, 0x0AFFFFF9u, 0xEAFFFFFEu
};
/* LDR r0,[r1]; ADD r2,r2,#1; CMP r2,#40; BNE back: exits on a register, not the load. */
static const uint32_t PROG_ADD[] = {
    0xE5910000u, 0xE2822001u, 0xE3520028u, 0x1AFFFFFBu, 0xEAFFFFFEu
};
/* STR r0,[r1],#4; ADD r0,r0,#1; CMP r0,#32; BNE back. */
static const uint32_t PROG_STORE[] = {
    0xE4810004u, 0xE2800001u, 0xE3500020u, 0x1AFFFFFBu, 0xEAFFFFFEu
};
/* Little Wizard-shaped poll: BL timer; STR r0,[fp,#4]; SUB r0,r0,r4;
 * BL abs; CMP r0,r5; BCC back; B .
 * timer: PUSH {lr}; LDR r0,[r1]; POP {pc}.
 * abs: MOV r2,r0,ASR #31; EOR r0,r0,r2; SUB r0,r0,r2; MOV pc,lr. */
static const uint32_t PROG_TIMER_STORE_ABS[] = {
    0xEB000005u, 0xE58B0004u, 0xE0400004u, 0xEB000005u,
    0xE1500005u, 0x3AFFFFF9u, 0xEAFFFFFEu,
    0xE92D4000u, 0xE5910000u, 0xE8BD8000u,
    0xE1A02FC0u, 0xE0200002u, 0xE0400002u, 0xE1A0F00Eu
};
/* LDR r0,[r1]; STR r0,[fp,#4]; CMP r0,#0; BEQ back; B . */
static const uint32_t PROG_POLL_STORE[] = {
    0xE5910000u, 0xE58B0004u, 0xE3500000u, 0x0AFFFFFBu, 0xEAFFFFFEu
};
/* Same CPU and RAM state at each backedge, but only the first STR is
 * idempotent: LDR r0,[r1]; STR r2,[fp,#4]; STR r3,[fp,#4];
 * STR r2,[fp,#4]; CMP r0,#0; BEQ back; B . */
static const uint32_t PROG_POLL_STORE_RESTORE[] = {
    0xE5910000u, 0xE58B2004u, 0xE58B3004u, 0xE58B2004u,
    0xE3500000u, 0x0AFFFFF9u, 0xEAFFFFFEu
};

static void load_prog(const uint32_t *prog, size_t n) {
    load_words(&bus_fast, CODE_ADDR, prog, n);
    load_words(&bus_ref, CODE_ADDR, prog, n);
}

/* Stable zero word polled forever: the fast CPU must stay bit-identical while
 * issuing far fewer data reads than the reference. */
static void case_poll_stable(int fastmem_ram) {
    current_case = fastmem_ram ? "poll-stable-fastmem" : "poll-stable";
    setup_pair(fastmem_ram);
    arm920t_set_jit(cpu_fast, 1);
    arm920t_set_jit(cpu_ref, 0);
    load_prog(PROG_POLL, sizeof(PROG_POLL) / sizeof(PROG_POLL[0]));
    add_stable(&bus_fast, DATA_ADDR);
    set_reg_both(1u, DATA_ADDR);

    /* An aligned budget isolates the complete backedge block. Ragged budgets
     * below can legitimately compile a suffix whose backedge targets a
     * different entry, even in the original implementation. */
    arm920t_reset_cpu_profile(cpu_fast);
    CHECK(arm920t_run(cpu_fast, 384u) == arm920t_run(cpu_ref, 384u), "aligned stable-poll budget");
    compare_state(0);
    gp32_cpu_profile_t p;
    arm920t_get_cpu_profile(cpu_fast, &p);
    if (p.supported) {
        CHECK(p.native_arm_insns == 0u, "complete stable poll must retain portable execution");
        CHECK(p.poll_skipped_insns > 0u, "true stable poll must still fast-forward");
    }
    printf("%s aligned_native=%" PRIu64 " aligned_skipped=%" PRIu64 "\n",
           current_case, p.native_arm_insns, p.poll_skipped_insns);
    uint64_t fr = 0, rr = 0;
    run_chunks(CHUNKS, NCHUNKS, 0, &fr, &rr);
    CHECK(bus_fast.stable_queries > 0u, "stability callback was never consulted");
    arm920t_get_cpu_profile(cpu_fast, &p);
    if (p.supported)
        CHECK(p.poll_skipped_insns > 0u, "true stable poll must still fast-forward");
    printf("%s native=%" PRIu64 " skipped=%" PRIu64 "\n",
           current_case, p.native_arm_insns, p.poll_skipped_insns);
    if (!fastmem_ram) {
        /* fastmem would bypass the read32 counter, so only the callback-bus
         * variant can observe the polling rate and the reduction. */
        CHECK(rr >= 250u, "reference did not poll the word enough times");
        CHECK(fr * 8u <= rr, "fast path did not significantly reduce polling reads");
    }
    teardown_pair();
}

/* Apparent increments which do NOT prove progress. In particular, a load or
 * reset before the step invalidates the proof just as a later overwrite does.
 * Run these with JIT enabled and a traced, instruction-by-instruction oracle;
 * their stable repetitions must remain portable and actually fast-forward. */
static void case_false_progress(void) {
    static const struct {
        const char *name;
        unsigned count;
        uint32_t program[7];
    } cases[] = {
        {"zero-step", 3u, {0xe5910000u, 0xe2822000u, 0xeafffffcu}},
        {"rotated-zero-step", 3u, {0xe5910000u, 0xe2822100u, 0xeafffffcu}},
        {"conditional-step", 4u, {0xe5910000u, 0xe3500000u, 0x12822001u, 0xeafffffbu}},
        {"different-source", 3u, {0xe5910000u, 0xe2802001u, 0xeafffffcu}},
        {"reload-before-step", 3u, {0xe5912000u, 0xe2822001u, 0xeafffffcu}},
        {"reload-after-step", 3u, {0xe2822001u, 0xe5912000u, 0xeafffffcu}},
        {"reset-before-step", 4u, {0xe5910000u, 0xe3a02000u, 0xe2822001u, 0xeafffffbu}},
        {"reset-after-step", 5u, {0xe5910000u, 0xe3500000u, 0xe2822001u, 0x03a02000u, 0xeafffffau}},
        {"cancelled-step", 4u, {0xe5910000u, 0xe2822001u, 0xe2422001u, 0xeafffffbu}},
        /* BEQ skips the increment but rejoins at the unconditional backedge. */
        {"bypassed-step", 5u, {0xe5910000u, 0xe3500000u, 0x0a000000u, 0xe2822001u, 0xeafffffau}},
        /* Two backedges: the earlier one can bypass the step. */
        {"two-backedges", 5u, {0xe5910000u, 0xe3500000u, 0x0afffffcu, 0xe2822001u, 0xeafffffau}}
    };
    for (unsigned k = 0; k < sizeof(cases) / sizeof(cases[0]); ++k) {
        current_case = cases[k].name;
        setup_pair(0);
        arm920t_set_jit(cpu_fast, 1);
        arm920t_set_jit(cpu_ref, 0);
        arm920t_set_trace(cpu_ref, 1, NULL, NULL);
        load_prog(cases[k].program, cases[k].count);
        add_stable(&bus_fast, DATA_ADDR);
        set_reg_both(1u, DATA_ADDR);
        run_chunks(CHUNKS, NCHUNKS, 1, NULL, NULL);
        gp32_cpu_profile_t p;
        arm920t_get_cpu_profile(cpu_fast, &p);
        if (p.supported) {
            CHECK(p.native_arm_insns == 0u, "false progress must not admit native polling");
            CHECK(p.poll_skipped_insns > 0u, "stable false-progress loop must still fast-forward");
        }
        CHECK(bus_fast.r32 * 8u <= bus_ref.r32, "false-progress poll must retain reduced reads");
        printf("%s native=%" PRIu64 " skipped=%" PRIu64 "\n",
               current_case, p.native_arm_insns, p.poll_skipped_insns);
        teardown_pair();
    }
}

/* Nonzero stable word: the loop exits on the first check and parks on B self. */
static void case_poll_exits(void) {
    current_case = "poll-exits";
    setup_pair(0);
    load_prog(PROG_POLL, sizeof(PROG_POLL) / sizeof(PROG_POLL[0]));
    store_both(DATA_ADDR, 0x12345678u);
    add_stable(&bus_fast, DATA_ADDR);
    set_reg_both(1u, DATA_ADDR);

    uint64_t fr = 0, rr = 0;
    run_chunks(CHUNKS, NCHUNKS, 0, &fr, &rr);
    CHECK(arm920t_get_pc(cpu_fast) == CODE_ADDR + 12u, "loop exit did not reach the terminal branch");
    teardown_pair();
}

/* PC-relative literal load marked stable: supported form, identical state and
 * far fewer bus reads. BIOS fastmem is disabled entirely: the core caches a
 * whole-region base pointer from a single probe, so only disabling the region
 * keeps fetches and the literal observable on read32. */
static void case_literal(void) {
    current_case = "literal";
    setup_pair(0);
    load_prog(PROG_LIT, sizeof(PROG_LIT) / sizeof(PROG_LIT[0]));
    for (test_bus_t *b = &bus_fast; b; b = (b == &bus_fast) ? &bus_ref : NULL) {
        b->fastmem_bios = 0;
    }
    store_both(LIT_ADDR, 0u);
    add_stable(&bus_fast, LIT_ADDR);

    uint64_t fr = 0, rr = 0;
    run_chunks(CHUNKS, NCHUNKS, 0, &fr, &rr);
    CHECK(rr >= 250u, "reference did not poll the literal enough times");
    CHECK(fr * 4u <= rr, "literal-load poll did not reduce reads");
    teardown_pair();
}

/* Already-inlined BL helper {PUSH lr; LDR r0,[r1]; POP pc} inside the poll
 * loop. The stack write is the same return address every complete loop, so
 * the RAM image including the stack slot must match at every boundary. RAM
 * fastmem stays enabled so the stack word is writable through fastmem, but
 * the probe at RAM_BASE is refused so the core cannot cache a whole-region
 * base pointer and the polled word's loads stay countable. */
static void case_bl_helper(void) {
    current_case = "bl-helper";
    setup_pair(1);
    arm920t_set_jit(cpu_fast, 1);
    arm920t_set_jit(cpu_ref, 0);
    load_prog(PROG_BL, sizeof(PROG_BL) / sizeof(PROG_BL[0]));
    for (test_bus_t *b = &bus_fast; b; b = (b == &bus_fast) ? &bus_ref : NULL) {
        b->no_fast_lo = RAM_BASE;
        b->no_fast_hi = DATA_ADDR + 4u;
    }
    add_stable(&bus_fast, DATA_ADDR);
    set_reg_both(1u, DATA_ADDR);
    set_reg_both(13u, STACK_TOP);

    uint64_t fr = 0, rr = 0;
    run_chunks(CHUNKS, NCHUNKS, 1, &fr, &rr);
    CHECK(rr >= 250u, "reference did not poll through the helper enough times");
    CHECK(fr * 4u <= rr, "BL-wrapped poll did not reduce reads");
    CHECK(ld32le(bus_ptr(&bus_fast, STACK_TOP - 4u, 4u)) == CODE_ADDR + 4u,
          "helper did not push the BL return address");
    teardown_pair();
}

/* Same BL helper but the stack lives at an MMIO address without fastmem
 * backing. The wrapper is ineligible, so every push/pop must reach the bus
 * identically on both CPUs and the visible state must match. */
static void case_mmio_stack(void) {
    current_case = "mmio-stack";
    setup_pair(1);
    load_prog(PROG_BL, sizeof(PROG_BL) / sizeof(PROG_BL[0]));
    add_stable(&bus_fast, DATA_ADDR);
    set_reg_both(1u, DATA_ADDR);
    set_reg_both(13u, MMIO_BASE + 4u);

    run_chunks(CHUNKS, NCHUNKS, 1, NULL, NULL);
    CHECK(bus_fast.w32 >= 200u, "helper pushes were not exercised");
    CHECK(bus_fast.w32 == bus_ref.w32, "MMIO stack writes were suppressed");
    CHECK(bus_fast.mmio_word == CODE_ADDR + 4u, "helper pushed a wrong return address");
    teardown_pair();
}

/* Keep timer loads on the bus while the stack and ordinary STR destination
 * retain fastmem backing. Only DATA_ADDR changes between run calls: the first
 * timestamp store after an update must execute before polling can settle.
 * Both signs of the register-only abs leaf and its real MOV pc,lr return are
 * exercised, including budget boundaries inside both helpers. */
static void case_idempotent_store_timer_leaf(void) {
    current_case = "idempotent-store-timer-leaf";
    setup_pair(1);
    load_prog(PROG_TIMER_STORE_ABS, sizeof(PROG_TIMER_STORE_ABS) / sizeof(PROG_TIMER_STORE_ABS[0]));
    for (test_bus_t *b = &bus_fast; b; b = (b == &bus_fast) ? &bus_ref : NULL) {
        b->no_fast_lo = RAM_BASE;
        b->no_fast_hi = DATA_ADDR + 4u;
    }
    add_stable(&bus_fast, DATA_ADDR);
    set_reg_both(1u, DATA_ADDR);
    set_reg_both(4u, 100u); /* origin */
    set_reg_both(5u, 40u);  /* deadline */
    set_reg_both(11u, STORE_ADDR - 4u);
    set_reg_both(13u, STACK_TOP);
    store_both(STORE_ADDR, 80u);

    const uint32_t clocks[] = {80u, 85u, 120u};
    const uint32_t distances[] = {20u, 15u, 20u};
    /* Thirteen executed instructions per repetition; finish at the backedge
     * before updating the clock, while CHUNKS still probes interior states. */
    const uint32_t finish_iteration[] = {11u}; /* sum(CHUNKS) + 11 == 195 * 13 */
    for (size_t i = 0; i < sizeof(clocks) / sizeof(clocks[0]); ++i) {
        uint64_t before_fr = bus_fast.r32, before_rr = bus_ref.r32;
        store_both(DATA_ADDR, clocks[i]);
        run_chunks(CHUNKS, NCHUNKS, 1, NULL, NULL);
        run_chunks(finish_iteration, 1u, 1, NULL, NULL);
        uint64_t fr = bus_fast.r32 - before_fr, rr = bus_ref.r32 - before_rr;
        CHECK(rr >= 150u, "reference did not exercise the timer/store/abs loop enough times");
        CHECK(fr * 4u <= rr, "idempotent STR and MOV pc,lr leaf did not reduce timer reads");
        CHECK(arm920t_get_pc(cpu_fast) == CODE_ADDR, "timer loop did not finish at its backedge");
        CHECK(ld32le(bus_ptr(&bus_fast, STORE_ADDR, 4u)) == clocks[i],
              "clock update did not reach the timestamp store");
        CHECK(arm920t_get_reg(cpu_fast, 0u) == distances[i], "abs leaf returned the wrong distance");
        CHECK(arm920t_get_reg(cpu_fast, 13u) == STACK_TOP, "timer helper did not restore SP");
        CHECK(ld32le(bus_ptr(&bus_fast, STACK_TOP - 4u, 4u)) == CODE_ADDR + 4u,
              "timer helper did not preserve its return address");
    }
    CHECK(bus_fast.stable_queries > 0u, "timer stability callback was never consulted");

    store_both(DATA_ADDR, 160u); /* cross the deadline between run calls */
    run_chunks(CHUNKS, NCHUNKS, 1, NULL, NULL);
    CHECK(arm920t_get_pc(cpu_fast) == CODE_ADDR + 24u, "updated timer did not exit at the deadline");
    CHECK(arm920t_get_reg(cpu_fast, 0u) == 60u, "deadline exit used a stale timer value");
    CHECK(ld32le(bus_ptr(&bus_fast, STORE_ADDR, 4u)) == 160u,
          "deadline exit lost the final timestamp store");
    teardown_pair();
}

/* An unchanged first store and equal backedge state do not justify skipping
 * the following stores. Compare RAM at small boundaries inside the mutation
 * as well as the complete-loop boundary, and require every timer read. */
static void case_store_modified_restored(void) {
    current_case = "store-modified-restored";
    setup_pair(1);
    load_prog(PROG_POLL_STORE_RESTORE,
              sizeof(PROG_POLL_STORE_RESTORE) / sizeof(PROG_POLL_STORE_RESTORE[0]));
    for (test_bus_t *b = &bus_fast; b; b = (b == &bus_fast) ? &bus_ref : NULL) {
        b->no_fast_lo = RAM_BASE;
        b->no_fast_hi = DATA_ADDR + 4u;
    }
    add_stable(&bus_fast, DATA_ADDR);
    set_reg_both(1u, DATA_ADDR);
    set_reg_both(2u, 0x11111111u);
    set_reg_both(3u, 0x22222222u);
    set_reg_both(11u, STORE_ADDR - 4u);
    store_both(STORE_ADDR, 0x11111111u);

    run_chunks(CHUNKS, 2u, 1, NULL, NULL); /* LDR; unchanged STR; changing STR */
    CHECK(ld32le(bus_ptr(&bus_fast, STORE_ADDR, 4u)) == 0x22222222u,
          "intermediate non-idempotent store was not exercised");
    run_chunks(CHUNKS + 2u, NCHUNKS - 2u, 1, NULL, NULL);
    CHECK(bus_ref.r32 >= 400u, "reference did not exercise the changing stores enough times");
    CHECK(bus_fast.r32 == bus_ref.r32, "restored RAM state incorrectly allowed polling skips");
    CHECK(ld32le(bus_ptr(&bus_fast, STORE_ADDR, 4u)) == 0x11111111u,
          "final store did not restore the original RAM value");
    teardown_pair();
}

/* Equal-valued MMIO writes still have observable side effects. Check their
 * count and latched value at every run boundary, even though the stable RAM
 * load and full CPU state return to a fixed point. */
static void case_mmio_poll_store(void) {
    current_case = "mmio-poll-store";
    setup_pair(1);
    load_prog(PROG_POLL_STORE, sizeof(PROG_POLL_STORE) / sizeof(PROG_POLL_STORE[0]));
    for (test_bus_t *b = &bus_fast; b; b = (b == &bus_fast) ? &bus_ref : NULL) {
        b->no_fast_lo = RAM_BASE;
        b->no_fast_hi = DATA_ADDR + 4u;
    }
    add_stable(&bus_fast, DATA_ADDR);
    set_reg_both(1u, DATA_ADDR);
    set_reg_both(11u, MMIO_BASE - 4u);

    for (size_t i = 0; i < NCHUNKS; ++i) {
        run_chunks(CHUNKS + i, 1u, 1, NULL, NULL);
        CHECK(bus_fast.w32 == bus_ref.w32, "ordinary STR suppressed MMIO writes");
        CHECK(bus_fast.mmio_word == bus_ref.mmio_word, "MMIO store value diverged");
    }
    CHECK(bus_ref.w32 >= 500u, "reference did not exercise MMIO STR enough times");
    CHECK(bus_fast.r32 == bus_ref.r32, "MMIO STR incorrectly allowed polling skips");
    teardown_pair();
}

/* Readable backing memory is not enough: fastmem must explicitly accept
 * write access for the STR. Refuse only writes so a read probe would succeed,
 * and require every equal-valued store to fall back to the public bus. */
static void case_poll_store_no_write_fastmem(void) {
    current_case = "poll-store-no-write-fastmem";
    setup_pair(1);
    load_prog(PROG_POLL_STORE, sizeof(PROG_POLL_STORE) / sizeof(PROG_POLL_STORE[0]));
    for (test_bus_t *b = &bus_fast; b; b = (b == &bus_fast) ? &bus_ref : NULL) {
        b->no_fast_lo = RAM_BASE;
        b->no_fast_hi = DATA_ADDR + 4u;
        b->reject_fastmem_writes = 1;
    }
    add_stable(&bus_fast, DATA_ADDR);
    set_reg_both(1u, DATA_ADDR);
    set_reg_both(11u, STORE_ADDR - 4u);

    for (size_t i = 0; i < NCHUNKS; ++i) {
        run_chunks(CHUNKS + i, 1u, 1, NULL, NULL);
        CHECK(bus_fast.w32 == bus_ref.w32, "rejected fastmem STR suppressed bus writes");
    }
    CHECK(bus_ref.w32 >= 500u, "reference did not exercise rejected fastmem STR enough times");
    CHECK(bus_fast.r32 == bus_ref.r32, "read-only fastmem incorrectly allowed polling skips");
    teardown_pair();
}

/* Pointer chain: r1 holds a pointer, the polled value sits one hop behind it.
 * Both load addresses are marked stable. */
static void case_pointer_chain(void) {
    current_case = "pointer-chain";
    setup_pair(0);
    load_prog(PROG_CHAIN, sizeof(PROG_CHAIN) / sizeof(PROG_CHAIN[0]));
    store_both(PTR_ADDR, DATA_ADDR);
    add_stable(&bus_fast, PTR_ADDR);
    add_stable(&bus_fast, DATA_ADDR);
    set_reg_both(1u, PTR_ADDR);

    uint64_t fr = 0, rr = 0;
    run_chunks(CHUNKS, NCHUNKS, 0, &fr, &rr);
    CHECK(rr >= 250u, "reference did not walk the pointer chain enough times");
    CHECK(fr * 4u <= rr, "chained poll did not reduce reads");
    teardown_pair();
}

/* Same program with the callback absent on the fast bus, then present but
 * always answering false: no fast-forward may engage in either variant. */
static void case_callback_off(void) {
    const char *names[2] = {"callback-absent", "callback-false"};
    for (int variant = 0; variant < 2; ++variant) {
        current_case = names[variant];
        setup_pair(0);
        if (!variant) {
            /* CPU creation copies the bus; editing the original bus afterward
             * does not remove the installed callback. Recreate before running. */
            arm920t_destroy(cpu_fast);
            bus_fast.bus.is_stable_read32 = NULL;
            cpu_fast = arm920t_create(&bus_fast.bus);
            if (!cpu_fast) { fprintf(stderr, "arm920t_create failed\n"); exit(2); }
            arm920t_reset(cpu_fast, CODE_ADDR);
        }
        load_prog(PROG_POLL, sizeof(PROG_POLL) / sizeof(PROG_POLL[0]));
        set_reg_both(1u, DATA_ADDR);
        /* else: no stable addresses are registered, so every query answers 0 */

        uint64_t fr = 0, rr = 0;
        run_chunks(CHUNKS, NCHUNKS, 0, &fr, &rr);
        CHECK(fr == rr, "disabled callback must not change the read pattern");
        CHECK(variant ? bus_fast.stable_queries > 0u : bus_fast.stable_queries == 0u,
              "callback presence did not match the requested variant");
        teardown_pair();
    }
}

/* Post-indexed LDR walks the base register through RAM. Even with the
 * callback answering true for every address, the writeback form is
 * unsupported and must execute identically, including r1 advancement. */
static void case_changing_load_addr(void) {
    current_case = "walk";
    setup_pair(0);
    load_prog(PROG_WALK, sizeof(PROG_WALK) / sizeof(PROG_WALK[0]));
    const unsigned zeros = 17u;
    for (test_bus_t *b = &bus_fast; b; b = (b == &bus_fast) ? &bus_ref : NULL) {
        memset(b->ram, 0, sizeof(b->ram));
        st32le(bus_ptr(b, RAM_BASE + zeros * 4u, 4u), 0xA5A5A5A5u);
    }
    bus_fast.stable_all = 1;
    set_reg_both(1u, RAM_BASE);

    run_chunks(CHUNKS, NCHUNKS, 0, NULL, NULL);
    CHECK(arm920t_get_reg(cpu_fast, 1u) == RAM_BASE + (zeros + 1u) * 4u,
          "base register did not walk through every element");
    CHECK(arm920t_get_pc(cpu_fast) == CODE_ADDR + 12u, "walk loop did not exit at the sentinel");
    teardown_pair();
}

/* The base register is temporarily modified then restored inside the loop.
 * Positive variant marks both actual load addresses stable; negative variant
 * leaves the intermediate address volatile. Exactness only, in both. */
static void case_base_modified_restored(void) {
    const char *names[2] = {"basetmp-all-stable", "basetmp-volatile-mid"};
    for (int variant = 0; variant < 2; ++variant) {
        current_case = names[variant];
        setup_pair(0);
        load_prog(PROG_BASETMP, sizeof(PROG_BASETMP) / sizeof(PROG_BASETMP[0]));
        store_both(DATA_ADDR + 4u, 0x5a5a5a5au);
        add_stable(&bus_fast, DATA_ADDR);
        if (!variant) add_stable(&bus_fast, DATA_ADDR + 4u);
        set_reg_both(1u, DATA_ADDR);

        run_chunks(CHUNKS, NCHUNKS, 1, NULL, NULL);
        CHECK(arm920t_get_reg(cpu_fast, 1u) == DATA_ADDR, "base register was not restored");
        CHECK(arm920t_get_reg(cpu_fast, 2u) == 0x5a5a5a5au, "intermediate load lost its value");
        teardown_pair();
    }
}

/* Loop exit depends on a register accumulator while the polled word is
 * stable: the loop body is not a pure poll and must run unmodified. */
static void case_add_loop(void) {
    current_case = "add-loop";
    setup_pair(0);
    load_prog(PROG_ADD, sizeof(PROG_ADD) / sizeof(PROG_ADD[0]));
    add_stable(&bus_fast, DATA_ADDR);
    set_reg_both(1u, DATA_ADDR);

    run_chunks(CHUNKS, NCHUNKS, 0, NULL, NULL);
    CHECK(arm920t_get_reg(cpu_fast, 2u) == 40u, "accumulator loop ran a wrong iteration count");
    CHECK(arm920t_get_pc(cpu_fast) == CODE_ADDR + 16u, "add loop did not exit at its limit");
    teardown_pair();
}

/* Stores have side effects; a store loop must execute every iteration in
 * both CPUs and leave byte-identical RAM images behind. */
static void case_store_loop(void) {
    current_case = "store-loop";
    setup_pair(0);
    load_prog(PROG_STORE, sizeof(PROG_STORE) / sizeof(PROG_STORE[0]));
    bus_fast.stable_all = 1;
    set_reg_both(1u, STORE_ADDR);

    run_chunks(CHUNKS, NCHUNKS, 0, NULL, NULL);
    CHECK(arm920t_get_reg(cpu_fast, 0u) == 32u, "store loop ran a wrong iteration count");
    CHECK(bus_fast.w32 == bus_ref.w32, "store loop issued different write counts");
    CHECK(!memcmp(bus_fast.ram, bus_ref.ram, RAM_SIZE), "store loop left divergent RAM");
    teardown_pair();
}

/* An IRQ raised between run calls must be taken identically even while the
 * fast CPU is inside a poll loop it could otherwise fast-forward through. */
static void case_pending_irq(void) {
    current_case = "pending-irq";
    setup_pair(0);
    load_prog(PROG_POLL, sizeof(PROG_POLL) / sizeof(PROG_POLL[0]));
    add_stable(&bus_fast, DATA_ADDR);
    set_reg_both(1u, DATA_ADDR);
    set_cpsr_both(arm920t_get_cpsr(cpu_fast) & ~CPSR_I_FLAG); /* unmask IRQ */

    for (size_t i = 0; i < NCHUNKS; ++i) {
        uint32_t df = arm920t_run(cpu_fast, CHUNKS[i]);
        uint32_t dr = arm920t_run(cpu_ref, CHUNKS[i]);
        CHECK(df == dr, "arm920t_run consumed different budgets");
        compare_state(0);
        if (i == 6u) {
            arm920t_set_irq(cpu_fast, 1);
            arm920t_set_irq(cpu_ref, 1);
        }
    }
    CHECK((arm920t_get_cpsr(cpu_fast) & CPSR_MODE_MASK) == MODE_IRQ_VALUE, "pending IRQ was never taken");
    CHECK(arm920t_get_pc(cpu_fast) == IRQ_VECTOR, "IRQ did not vector to 0x18");
    teardown_pair();
}

/* A peripheral read may shorten a live run. Native chains and stable-poll
 * skipping must use the new deadline; later runs start with a fresh budget. */
static unsigned deadline_reads, deadline_trigger;
static uint32_t deadline_limit;
static int deadline_action;
static int deadline_swi(void *u, arm920t_t *cpu, uint32_t imm, uint32_t pc, int is_thumb) {
    (void)u; (void)imm; (void)pc; (void)is_thumb;
    arm920t_limit_run(cpu, deadline_limit);
    return 1;
}
static uint32_t deadline_read(void *u, uint32_t a) {
    if (a == MMIO_BASE && deadline_action == 2 && deadline_reads == 0) arm920t_limit_run(cpu_fast, 100);
    if (a == MMIO_BASE && ++deadline_reads == deadline_trigger) {
        arm920t_limit_run(cpu_fast, deadline_limit);
        arm920t_limit_run(cpu_fast, UINT32_MAX); /* never extend */
        if (deadline_action == 1) arm920t_stop_run(cpu_fast);
    }
    return tb_read32(u, a);
}
static void deadline_setup(unsigned mode, const uint32_t *prog, size_t n) {
    setup_pair(1);
    arm920t_destroy(cpu_fast);
    bus_fast.bus.read32 = deadline_read;
    bus_fast.bus.read32_io = deadline_read;
    cpu_fast = arm920t_create(&bus_fast.bus);
    load_words(&bus_fast, CODE_ADDR, prog, n);
    arm920t_reset(cpu_fast, CODE_ADDR);
    arm920t_set_reg(cpu_fast, 1, MMIO_BASE);
    arm920t_set_jit(cpu_fast, mode != 0);
    if (mode == 2) arm920t_set_trace(cpu_fast, 1, NULL, NULL);
    deadline_reads = 0; deadline_trigger = 1; deadline_limit = 17; deadline_action = 0;
}
static void deadline_expect(uint32_t budget, uint32_t want) {
    uint64_t before = arm920t_get_cycles(cpu_fast);
    uint32_t got = arm920t_run(cpu_fast, budget);
    if (got != want) { fprintf(stderr, "%s got=%u want=%u\n", current_case, got, want); ++failures; }
    CHECK(arm920t_get_cycles(cpu_fast) == before + got, "cycle accounting");
}
static void case_run_deadlines(void) {
    static const uint32_t progress[] = {0xe2822001,0xe5910000,0xe35200ff,0x1afffffb,0xeafffffe};
    for (unsigned mode=0; mode<3; ++mode) {
        current_case = "deadline-stable-poll";
        deadline_setup(mode, PROG_POLL, 4);
        add_stable(&bus_fast, MMIO_BASE);
        deadline_expect(32768,17);
        gp32_cpu_profile_t poll_profile;
        arm920t_get_cpu_profile(cpu_fast,&poll_profile);
        if (mode != 2 && poll_profile.supported) CHECK(poll_profile.poll_skipped_insns>0,"stable skip exercised");
        arm920t_limit_run(cpu_fast,0); /* outside run is ignored */
        deadline_trigger=0;
        deadline_expect(23,23); /* no stale per-run deadline */
        teardown_pair();

        current_case = "deadline-native-late-callback";
        deadline_setup(mode, progress, 5);
        deadline_trigger=8; deadline_limit=31; /* eighth LDR retires at cycle 30 */
        deadline_expect(32768,31);
        CHECK(arm920t_get_reg(cpu_fast,2)==8, "no stale native chain");
        gp32_cpu_profile_t p;
        arm920t_get_cpu_profile(cpu_fast,&p);
        if(mode==1 && p.supported && p.native_backend) CHECK(p.native_block_calls>0,"native path exercised");
        teardown_pair();

        current_case = "deadline-already-past";
        deadline_setup(mode, progress, 5);
        deadline_trigger=8; deadline_limit=0;
        deadline_expect(32768,30); /* finish current instruction, no rollback */
        teardown_pair();

        current_case = "deadline-stop-run";
        deadline_setup(mode, progress, 5);
        deadline_limit=100; deadline_action=1;
        deadline_expect(32768,2);
        deadline_trigger=0; deadline_action=0;
        deadline_expect(9,9);
        teardown_pair();

        current_case = "deadline-repeated-shrink";
        deadline_setup(mode, progress, 5);
        deadline_action=2; deadline_trigger=3; deadline_limit=17;
        deadline_expect(32768,17);
        teardown_pair();

        current_case = "deadline-swi";
        static const uint32_t swi_prog[] = {0xe2822001,0xef000000,0xe2822001,0xeafffffe};
        deadline_setup(mode, swi_prog, 4);
        arm920t_set_swi_handler(cpu_fast,deadline_swi,NULL);
        deadline_limit=2;
        deadline_expect(32768,2);
        CHECK(arm920t_get_reg(cpu_fast,2)==1,"SWI exits before following ADD");
        teardown_pair();

        current_case = "deadline-thumb";
        deadline_setup(mode, progress, 5);
        st16le(bus_fast.bios+CODE_ADDR,0x6808); /* LDR r0,[r1] */
        st16le(bus_fast.bios+CODE_ADDR+2,0x2800); /* CMP r0,#0 */
        st16le(bus_fast.bios+CODE_ADDR+4,0xd0fc); /* BEQ back */
        arm920t_set_cpsr(cpu_fast,arm920t_get_cpsr(cpu_fast)|0x20);
        deadline_expect(32768,17);
        teardown_pair();

        current_case = "deadline-thumb-to-arm-absolute-origin";
        deadline_setup(mode, progress, 5);
        st16le(bus_fast.bios+CODE_ADDR,0x4718); /* BX r3 */
        load_words(&bus_fast,CODE_ADDR+0x40,progress,5);
        arm920t_set_reg(cpu_fast,3,CODE_ADDR+0x40);
        arm920t_set_cpsr(cpu_fast,arm920t_get_cpsr(cpu_fast)|0x20);
        deadline_limit=9;
        deadline_expect(32768,9);
        teardown_pair();

        current_case = "deadline-does-not-extend";
        deadline_setup(mode, progress, 5);
        deadline_limit=100;
        deadline_expect(7,7);
        teardown_pair();
    }
}

/* Generic signed positive countdown, two fixed loads and AND accumulators.
 * The reference has no stability contract and therefore executes every load.
 * This catches a skip which freezes the counter, loses CMP flags, or crosses
 * the exit/partial-iteration boundary. No GP32 MMIO or title addresses here. */
static const uint32_t PROG_COUNTED[] = {
    0xe3520000u, 0xda000005u, 0xe5905000u, 0xe0033005u,
    0xe5915000u, 0xe0044005u, 0xe2422001u, 0xeafffff7u,
    0xeafffffeu
};

static void counted_setup(unsigned jit, uint32_t counter) {
    setup_pair(0);
    load_prog(PROG_COUNTED, sizeof(PROG_COUNTED) / sizeof(PROG_COUNTED[0]));
    add_stable(&bus_fast, DATA_ADDR);
    add_stable(&bus_fast, DATA_ADDR + 4u);
    store_both(DATA_ADDR, 0xf0f0aa55u);
    store_both(DATA_ADDR + 4u, 0x55aa0ff0u);
    set_reg_both(0, DATA_ADDR);
    set_reg_both(1, DATA_ADDR + 4u);
    set_reg_both(2, counter);
    set_reg_both(3, 0xffffffffu);
    set_reg_both(4, 0xffffffffu);
    set_cpsr_both(0xf00000d3u);
    arm920t_set_jit(cpu_fast, jit);
    arm920t_set_jit(cpu_ref, jit);
}

static void case_counted_poll(void) {
    for (unsigned jit = 0; jit < 2; ++jit) {
        current_case = "counted-already-stable-entry";
        counted_setup(jit, 100u);
        arm920t_set_trace(cpu_ref, 1, NULL, NULL);
        /* A prior run can leave the input accumulators and scratch register
         * stable already. Still observe both real loads in this new run. */
        set_reg_both(3, 0xf0f0aa55u); set_reg_both(4, 0x55aa0ff0u);
        set_reg_both(5, 0x55aa0ff0u); set_cpsr_both(0x200000d3u);
        CHECK(arm920t_run(cpu_fast, 16u) == arm920t_run(cpu_ref, 16u), "warm entry budget");
        compare_state(1);
        CHECK(bus_fast.r32 == 2u && bus_ref.r32 == 4u,
              "one real stable observation suffices for the second repetition");
        /* Changed host input must be consumed even when the previous run
         * established a fixed point; no entry proof persists between runs. */
        store_both(DATA_ADDR, 0x0000ff00u);
        store_both(DATA_ADDR + 4u, 0xff000000u);
        CHECK(arm920t_run(cpu_fast, 17u) == arm920t_run(cpu_ref, 17u), "changed input partial budget");
        compare_state(1);
        CHECK(arm920t_get_reg(cpu_fast, 3) == 0x0000aa00u &&
              arm920t_get_reg(cpu_fast, 4) == 0x55000000u, "fresh input after previous proof");
        teardown_pair();

        current_case = jit ? "counted-native-enabled" : "counted-portable";
        counted_setup(jit, 10000u);
        run_chunks(CHUNKS, NCHUNKS, 1, NULL, NULL);
        CHECK(bus_fast.r32 * 8u < bus_ref.r32, "counted poll did not reduce loads");
        gp32_cpu_profile_t p;
        arm920t_get_cpu_profile(cpu_fast, &p);
        if (p.supported) CHECK(p.poll_skipped_insns > 0, "counted skip not exercised");
        printf("%s reads=%lu/%lu skipped=%lu\n", current_case,
               (unsigned long)bus_fast.r32, (unsigned long)bus_ref.r32,
               (unsigned long)p.poll_skipped_insns);
        /* New run must read the changed input before applying a new proof. */
        store_both(DATA_ADDR, 0x0000ff00u);
        store_both(DATA_ADDR + 4u, 0xff000000u);
        run_chunks(CHUNKS, NCHUNKS, 1, NULL, NULL);
        teardown_pair();

        static const uint32_t starts[] = {0u, 1u, 2u, 3u, 7u, 0x7fffffffu,
                                         0x80000000u, 0xffffffffu};
        for (unsigned s = 0; s < sizeof(starts) / sizeof(starts[0]); ++s) {
            current_case = "counted-signed-boundaries";
            counted_setup(jit, starts[s]);
            const uint32_t chunks[] = {16u, 1u, 6u, 1u, 7u, 1u, 31u, 4097u};
            run_chunks(chunks, sizeof(chunks) / sizeof(chunks[0]), 1, NULL, NULL);
            teardown_pair();
        }
    }
}

/* Trace forces the independent instruction interpreter. Sweep every cycle
 * boundary around the exit, including nonunit/rotated steps and signed
 * underflow without signed overflow. Also rename every working register. */
static void case_counted_boundaries(void) {
    static const uint32_t steps[] = {0xe2422001u, 0xe2422007u, 0xe2422c01u,
                                     0xe2422101u}; /* 1,7,256,0x40000000 */
    static const uint32_t starts[] = {40u, 22u, 1000u, 0x7fffffffu};
    for (unsigned s = 0; s < 4; ++s) {
        current_case = "counted-every-exit-boundary";
        counted_setup(s & 1u, starts[s]);
        store_both(CODE_ADDR + 24u, steps[s]);
        arm920t_set_trace(cpu_ref, 1, NULL, NULL);
        for (uint32_t budget = 0; budget <= 337u; ++budget) {
            arm920t_reset(cpu_fast, CODE_ADDR);
            arm920t_reset(cpu_ref, CODE_ADDR);
            set_reg_both(0, DATA_ADDR); set_reg_both(1, DATA_ADDR + 4u);
            set_reg_both(2, starts[s]); set_reg_both(3, UINT32_MAX); set_reg_both(4, UINT32_MAX);
            set_cpsr_both(0xf00000d3u);
            CHECK(arm920t_run(cpu_fast, budget) == arm920t_run(cpu_ref, budget), "exit budget mismatch");
            compare_state(0);
        }
        teardown_pair();
    }
    current_case = "counted-renamed-registers";
    counted_setup(1, 0);
    static const uint32_t renamed[] = {
        0xe3580000u,0xda000005u,0xe599b00cu,0xe006600bu,
        0xe51ab004u,0xe007700bu,0xe2488003u,0xeafffff7u,0xeafffffeu
    }; /* counter r8, bases r9/r10, temporary r11, accumulators r6/r7 */
    load_prog(renamed, sizeof(renamed) / sizeof(renamed[0]));
    set_reg_both(8, 10000u); set_reg_both(9, DATA_ADDR - 12u); set_reg_both(10, DATA_ADDR + 8u);
    set_reg_both(6, UINT32_MAX); set_reg_both(7, UINT32_MAX);
    arm920t_set_trace(cpu_ref, 1, NULL, NULL);
    run_chunks(CHUNKS, NCHUNKS, 1, NULL, NULL);
    CHECK(bus_fast.r32 * 8u < bus_ref.r32, "renamed countdown did not accelerate");
    teardown_pair();
}

static void case_counted_refusals(void) {
    static const struct { unsigned index; uint32_t insn; } cases[] = {
        {3,0xe0033002u}, /* counter-dependent AND */
        {3,0xe0033085u}, /* shifted AND */
        {3,0xe0133005u}, /* flag-setting AND */
        {6,0xe2522001u}, /* flag-setting SUB */
        {6,0x12422001u}, /* conditional step */
        {6,0xe2822001u}, /* ADD: possible signed wrap */
        {2,0xe4905004u}, /* post-indexed, moving load */
        {2,0xe5d05000u}, /* byte load */
        {4,0xe5815000u}  /* peripheral/RAM store */
    };
    for (unsigned s = 0; s < sizeof(cases) / sizeof(cases[0]); ++s) {
        current_case = "counted-unsupported-shape";
        counted_setup(0, 10000u);
        store_both(CODE_ADDR + cases[s].index * 4u, cases[s].insn);
        bus_fast.stable_all = 1;
        arm920t_set_trace(cpu_ref, 1, NULL, NULL);
        run_chunks(CHUNKS, NCHUNKS, 1, NULL, NULL);
        CHECK(bus_fast.r32 == bus_ref.r32, "unsupported counted shape elided a load");
        CHECK(bus_fast.w32 == bus_ref.w32, "unsupported counted shape elided a store");
        teardown_pair();
    }
    for (unsigned missing = 0; missing < 3; ++missing) {
        current_case = "counted-missing-stability-or-alignment";
        counted_setup(1, 10000u);
        if (missing == 0) bus_fast.nstable = 1; /* second load is volatile */
        if (missing == 1) bus_fast.nstable = 0;
        if (missing == 2) set_reg_both(0, DATA_ADDR + 1u);
        run_chunks(CHUNKS, NCHUNKS, 1, NULL, NULL);
        CHECK(bus_fast.r32 == bus_ref.r32, "unproven load was elided");
        teardown_pair();
    }
}

/* Rejected MMIO reads change on every access. A stable-load callback is
 * installed, but rejection must preserve the old native progress route and
 * every observable read. Reuse the same compiled entry after contract/base
 * changes so a cached rejection cannot hide the supported path. */
static int counted_volatile;
static uint32_t counted_fallback_read(void *u, uint32_t a) {
    test_bus_t *b = (test_bus_t *)u;
    uint32_t value = tb_read32(u, a);
    if (a == MMIO_BASE || a == MMIO_BASE + 4u) {
        if (counted_volatile) ++b->mmio_word;
        value = b->mmio_word;
    }
    return value;
}

static void case_counted_native_fallback(void) {
    current_case = "counted-rejected-native-and-reselection";
    counted_setup(1, 10000u);
    arm920t_destroy(cpu_fast); arm920t_destroy(cpu_ref);
    bus_fast.fastmem_ram = bus_ref.fastmem_ram = 1; /* Enable native backend. */
    bus_fast.bus.read32 = bus_ref.bus.read32 = counted_fallback_read;
    bus_fast.bus.read32_io = bus_ref.bus.read32_io = counted_fallback_read;
    cpu_fast = arm920t_create(&bus_fast.bus); cpu_ref = arm920t_create(&bus_ref.bus);
    if (!cpu_fast || !cpu_ref) exit(2);
    arm920t_reset(cpu_fast, CODE_ADDR); arm920t_reset(cpu_ref, CODE_ADDR);
    arm920t_set_jit(cpu_fast, 1);
    arm920t_set_trace(cpu_ref, 1, NULL, NULL);
    set_reg_both(0, MMIO_BASE); set_reg_both(1, MMIO_BASE + 4u);
    set_reg_both(2, 10000u); set_reg_both(3, UINT32_MAX); set_reg_both(4, UINT32_MAX);
    set_cpsr_both(0xf00000d3u);
    /* A cold supported counted entry must allocate no native code at all. */
    counted_volatile = 0;
    bus_fast.nstable = 0;
    add_stable(&bus_fast, MMIO_BASE); add_stable(&bus_fast, MMIO_BASE + 4u);
    bus_fast.mmio_word = bus_ref.mmio_word = 0xa5a55a5au;
    arm920t_reset_cpu_profile(cpu_fast);
    CHECK(arm920t_run(cpu_fast, 512u) == arm920t_run(cpu_ref, 512u), "cold stable budget");
    compare_state(1);
    gp32_cpu_profile_t cold;
    arm920t_get_cpu_profile(cpu_fast, &cold);
    if (cold.supported) {
        CHECK(cold.poll_skipped_insns > 0u && cold.native_arm_insns == 0u, "cold supported loop must skip");
        CHECK(cold.jit_native_compiled == 0u && cold.jit_native_failed == 0u && cold.jit_code_used == 0u,
              "supported counted loop must not attempt native compilation");
    }
    /* JIT-disabled rejected execution must not emit or run native code. */
    arm920t_set_jit(cpu_fast, 0);
    counted_volatile = 1;
    bus_fast.nstable = 0;
    arm920t_reset_cpu_profile(cpu_fast);
    uint64_t disabled_fr = bus_fast.r32, disabled_rr = bus_ref.r32;
    CHECK(arm920t_run(cpu_fast, 512u) == arm920t_run(cpu_ref, 512u), "disabled fallback budget");
    compare_state(1);
    CHECK(bus_fast.r32 - disabled_fr == bus_ref.r32 - disabled_rr, "disabled fallback read count");
    arm920t_get_cpu_profile(cpu_fast, &cold);
    if (cold.supported) CHECK(cold.native_arm_insns == 0u && cold.jit_native_compiled == 0u &&
                              cold.jit_native_failed == 0u && cold.jit_code_used == 0u &&
                              cold.poll_skipped_insns == 0u, "JIT disabled must not compile or skip rejection");
    arm920t_set_jit(cpu_fast, 1);
    for (unsigned phase = 0; phase < 4u; ++phase) {
        counted_volatile = phase != 1u;
        bus_fast.nstable = 0;
        if (phase == 1u) {
            add_stable(&bus_fast, MMIO_BASE); add_stable(&bus_fast, MMIO_BASE + 4u);
            bus_fast.mmio_word = bus_ref.mmio_word = 0xa5a55a5au;
        }
        if (phase == 2u) { /* Contract still accepts MMIO, but bases moved. */
            add_stable(&bus_fast, MMIO_BASE); add_stable(&bus_fast, MMIO_BASE + 4u);
            set_reg_both(0, MMIO_BASE + 8u); set_reg_both(1, MMIO_BASE + 12u);
        }
        if (phase == 3u) {
            set_reg_both(0, MMIO_BASE); set_reg_both(1, MMIO_BASE + 4u);
        }
        if (phase == 1u) {
            /* A caller in another page must return through dispatcher before
             * entering the counted block already compiled under rejection. */
            const uint32_t caller = CODE_ADDR + 0x1000u;
            uint32_t branch = 0xea000000u | (((CODE_ADDR - caller - 8u) >> 2) & 0x00ffffffu);
            store_both(caller, branch);
            set_reg_both(15, caller);
        }
        arm920t_reset_cpu_profile(cpu_fast);
        uint64_t fr = bus_fast.r32, rr = bus_ref.r32;
        uint32_t budget = phase == 1u ? 513u : 512u;
        CHECK(arm920t_run(cpu_fast, budget) == arm920t_run(cpu_ref, budget), "fallback budget");
        compare_state(1);
        CHECK(bus_fast.mmio_word == bus_ref.mmio_word, "fallback read side effects");
        gp32_cpu_profile_t p;
        arm920t_get_cpu_profile(cpu_fast, &p);
        if (phase == 1u) {
            if (p.supported) CHECK(p.poll_skipped_insns > 0u && p.native_arm_insns <= 1u &&
                                  p.block_interp_arm_insns + p.native_arm_insns + p.poll_skipped_insns == budget,
                                  "caller entry must reselect stable proof for cached native counted block");
        } else {
            CHECK(bus_fast.r32 - fr == bus_ref.r32 - rr, "rejected proof elided a read");
            if (p.supported) {
                CHECK(p.poll_skipped_insns == 0u, "rejected proof skipped instructions");
                if (p.native_backend) CHECK(p.native_arm_insns > 0u && p.block_interp_arm_insns <= 8u &&
                                            p.native_arm_insns + p.block_interp_arm_insns == 512u,
                                            "rejected counted loop needs native execution after one real observation");
            }
        }
    }
    counted_volatile = 0;
    teardown_pair();
}

static unsigned counted_action;
static uint32_t counted_limit;
static uint32_t counted_callback_read(void *u, uint32_t a) {
    test_bus_t *b = (test_bus_t *)u;
    uint32_t value = counted_fallback_read(u, a);
    arm920t_t *cpu = b == &bus_fast ? cpu_fast : cpu_ref;
    if (b->r32 == 3u) { /* Second iteration: no skip proof exists yet. */
        switch (counted_action) {
        case 0: arm920t_limit_run(cpu, counted_limit); break;
        case 1: arm920t_stop_run(cpu); break;
        case 2: arm920t_set_irq(cpu, 1); break;
        case 3: arm920t_set_fiq(cpu, 1); break;
        case 4:
            st32le(b->bios + CODE_ADDR + 24u, 0xe2422007u);
            arm920t_flush_jit(cpu);
            break;
        case 5: {
            FILE *f = tmpfile();
            CHECK(f != NULL, "callback state temporary file");
            if (f) {
                CHECK(arm920t_state_save(cpu, f), "callback state save"); rewind(f);
                CHECK(arm920t_state_load(cpu, f), "callback state load"); fclose(f);
            }
            break;
        }
        case 6: arm920t_set_trace(cpu, 1, NULL, NULL); break;
        case 7: arm920t_set_jit(cpu, 0); break;
        }
    }
    return value;
}

static void case_counted_callbacks(void) {
    for (unsigned rejected = 0; rejected < 2u; ++rejected) {
    for (unsigned action = 0; action < 8; ++action) {
        for (unsigned deadline = 0; deadline < (action ? 1u : 4u); ++deadline) {
            current_case = "counted-callback-invalidation-and-deadline";
            counted_setup(1, 10000u);
            arm920t_destroy(cpu_fast); arm920t_destroy(cpu_ref);
            counted_volatile = rejected;
            if (rejected) {
                bus_fast.fastmem_ram = bus_ref.fastmem_ram = 1;
                bus_fast.nstable = 0;
            }
            bus_fast.bus.read32 = bus_ref.bus.read32 = counted_callback_read;
            bus_fast.bus.read32_io = bus_ref.bus.read32_io = counted_callback_read;
            cpu_fast = arm920t_create(&bus_fast.bus); cpu_ref = arm920t_create(&bus_ref.bus);
            arm920t_reset(cpu_fast, CODE_ADDR); arm920t_reset(cpu_ref, CODE_ADDR);
            uint32_t data = rejected ? MMIO_BASE : DATA_ADDR;
            set_reg_both(0, data); set_reg_both(1, data + 4u);
            set_reg_both(2, 10000u); set_reg_both(3, UINT32_MAX); set_reg_both(4, UINT32_MAX);
            set_cpsr_both(0x13u); /* IRQ and FIQ unmasked */
            arm920t_set_jit(cpu_fast, 1);
            arm920t_set_trace(cpu_ref, 1, NULL, NULL);
            counted_action = action;
            static const uint32_t limits[] = {0u, 13u, 65u, UINT32_MAX};
            counted_limit = limits[deadline];
            /* Permit a full native entry after the first real observation,
             * then end inside a straight handler prefix. Its default B-self
             * would otherwise legitimately skip unrelated instructions. */
            if (rejected && (action == 2u || action == 3u))
                for (uint32_t vector = 0x18u; vector <= 0x2cu; vector += 4u)
                    store_both(vector, 0xe1acc00cu); /* MOV r12,r12 */
            uint32_t budget = rejected && (action == 2u || action == 3u) ? 16u : 513u;
            uint32_t got = arm920t_run(cpu_fast, budget);
            CHECK(got == arm920t_run(cpu_ref, budget), "callback budget mismatch");
            if (action == 0) CHECK(got == (deadline == 0 ? 11u : deadline == 3 ? 513u : counted_limit),
                                   "deadline was not enforced after the read");
            if (action == 1) CHECK(got == 11u, "stop-run did not finish the in-flight LDR");
            if (action == 2) CHECK((arm920t_get_cpsr(cpu_fast) & CPSR_MODE_MASK) == MODE_IRQ_VALUE,
                                   "read callback IRQ was not taken");
            if (action == 3) CHECK((arm920t_get_cpsr(cpu_fast) & CPSR_MODE_MASK) == 0x11u,
                                   "read callback FIQ was not taken");
            compare_state(1);
            if (rejected) {
                gp32_cpu_profile_t p;
                arm920t_get_cpu_profile(cpu_fast, &p);
                CHECK(bus_fast.r32 == bus_ref.r32 && bus_fast.mmio_word == bus_ref.mmio_word,
                      "native callback changed observable reads");
                if (p.supported) {
                    CHECK(p.poll_skipped_insns == 0u, "rejected callback proof skipped instructions");
                    if (p.native_backend) CHECK(p.native_arm_insns > 0u, "callback must exercise native fallback");
                }
            }
            run_chunks(CHUNKS, NCHUNKS, 1, NULL, NULL);
            teardown_pair();
        }
    }
    }
    counted_volatile = 0;
}

/* Serialized images include banked registers, CP15 and authoritative TLB
 * payloads: compare them too, beyond the visible-register oracle. */
static void compare_cpu_images(void) {
    FILE *fast = tmpfile(), *ref = tmpfile();
    CHECK(fast && ref, "CPU image temporary files");
    if (fast && ref) {
        CHECK(arm920t_state_save(cpu_fast, fast) && arm920t_state_save(cpu_ref, ref), "CPU image save");
        rewind(fast); rewind(ref);
        int a, b;
        do { a = fgetc(fast); b = fgetc(ref); } while (a == b && a != EOF);
        CHECK(a == b, "banked/CP15/TLB state image mismatch");
    }
    if (fast) fclose(fast);
    if (ref) fclose(ref);
}

static void case_counted_restore(void) {
    current_case = "counted-state-load-after-skip";
    counted_setup(1, 10000u);
    arm920t_set_trace(cpu_ref, 1, NULL, NULL);
    const uint32_t first[] = {513u};
    run_chunks(first, 1, 1, NULL, NULL);
    gp32_cpu_profile_t p;
    arm920t_get_cpu_profile(cpu_fast, &p);
    if (p.supported) CHECK(p.poll_skipped_insns > 0, "state-save fixture did not accelerate");
    compare_cpu_images();
    FILE *fast = tmpfile(), *ref = tmpfile();
    CHECK(fast && ref, "CPU restore temporary files");
    if (fast && ref) {
        CHECK(arm920t_state_save(cpu_fast, fast) && arm920t_state_save(cpu_ref, ref), "CPU restore save");
        store_both(CODE_ADDR + 24u, 0xe2422007u); /* a new affine step */
        store_both(DATA_ADDR, 0x00000ff0u);
        store_both(DATA_ADDR + 4u, 0xff000000u);
        rewind(fast); rewind(ref);
        CHECK(arm920t_state_load(cpu_fast, fast) && arm920t_state_load(cpu_ref, ref), "CPU restore load");
        run_chunks(CHUNKS, NCHUNKS, 1, NULL, NULL);
        compare_cpu_images();
    }
    if (fast) fclose(fast);
    if (ref) fclose(ref);
    teardown_pair();
}

/* Section translations include a deliberately colliding direct TLB slot.
 * GPIO-like stable endpoints do not make page-table reads safe to omit. */
static void case_counted_mmu(void) {
    for (unsigned jit = 0; jit < 2u; ++jit) {
    for (unsigned variant = 0; variant < 4; ++variant) {
        current_case = "counted-mmu-mapping-proof";
        counted_setup(0, 10000u);
        if (jit) {
            bus_fast.fastmem_ram = bus_ref.fastmem_ram = 1;
            /* Leave page-table reads observable, while providing the native
             * entry RAM pointer. No full direct RAM window is advertised. */
            bus_fast.no_fast_lo = bus_ref.no_fast_lo = RAM_BASE + 0x10000u;
            bus_fast.no_fast_hi = bus_ref.no_fast_hi = RAM_BASE + 0x14000u;
        }
        set_reg_both(0, 0x10000000u);
        set_reg_both(1, variant == 1 ? 0x11000000u : 0x10001000u);
        bus_fast.mmio_word = bus_ref.mmio_word = 0xa5a55a5au;
        add_stable(&bus_fast, MMIO_BASE);
        add_stable(&bus_fast, MMIO_BASE + 0x1000u);
        add_stable(&bus_fast, RAM_BASE);
        add_stable(&bus_fast, DATA_ADDR);
        const uint32_t table = RAM_BASE + 0x10000u;
        store_both(table, 2u); /* code VA 0 -> BIOS */
        /* A native CPU has a direct RAM anchor, so use a non-RAM/non-I/O PA
         * to retain the secondary-translation refusal in that variant. */
        uint32_t mapped = variant == 3 ? (jit ? 0x13000000u : RAM_BASE) : MMIO_BASE;
        if (jit && variant == 3) add_stable(&bus_fast, mapped);
        store_both(table + 0x400u, variant == 2 ? 0u : mapped | 2u);
        store_both(table + 0x440u, MMIO_BASE | 2u);
        /* Public state_apply installs a fixed image without guest MCRs. */
        arm920t_state_image_t *image = calloc(1, sizeof(*image));
        CHECK(image != NULL, "MMU image allocation");
        if (image) {
            for (unsigned r = 0; r < 16; ++r) image->r[r] = arm920t_get_reg(cpu_fast, r);
            image->cpsr = arm920t_get_cpsr(cpu_fast);
            image->cp15[1] = 0x71u; image->cp15[2] = table;
            arm920t_state_apply(cpu_fast, image); arm920t_state_apply(cpu_ref, image);
            free(image);
        }
        arm920t_set_jit(cpu_fast, jit);
        uint64_t fr, rr;
        run_chunks(CHUNKS, NCHUNKS, 1, &fr, &rr);
        printf("counted-mmu jit=%u variant=%u reads=%lu/%lu\n", jit, variant, (unsigned long)fr, (unsigned long)rr);
        if (variant == 0) CHECK(fr * 4u < rr, "cached nonconflicting MMU poll did not accelerate");
        else CHECK(fr == rr, "missing/conflicting/secondary mappings changed page-table reads");
        CHECK(arm920t_get_cp15(cpu_fast,5) == arm920t_get_cp15(cpu_ref,5) &&
              arm920t_get_cp15(cpu_fast,6) == arm920t_get_cp15(cpu_ref,6), "MMU fault registers mismatch");
        compare_cpu_images();
        if (jit && variant) {
            gp32_cpu_profile_t p;
            arm920t_get_cpu_profile(cpu_fast, &p);
            if (p.supported) {
                CHECK(p.poll_skipped_insns == 0u, "rejected MMU mapping skipped instructions");
                if (p.native_backend) CHECK(p.native_arm_insns > 0u, "rejected MMU mapping must retain native execution");
            }
        }
        teardown_pair();
    }
    }
}

static void case_counted_gpio(void) {
    current_case = "counted-real-gpio";
    s3c2400_t *soc_fast = s3c2400_create(0), *soc_ref = s3c2400_create(0);
    if (!soc_fast || !soc_ref) exit(2);
    arm_bus_t fast_bus = s3c2400_get_bus(soc_fast), ref_bus = s3c2400_get_bus(soc_ref);
    ref_bus.is_stable_read32 = NULL;
    cpu_fast = arm920t_create(&fast_bus); cpu_ref = arm920t_create(&ref_bus);
    if (!cpu_fast || !cpu_ref) exit(2);
    s3c2400_set_irq_sink(soc_fast, cpu_fast); s3c2400_set_irq_sink(soc_ref, cpu_ref);
    for (unsigned i = 0; i < sizeof(PROG_COUNTED)/sizeof(PROG_COUNTED[0]); ++i) {
        fast_bus.write32(fast_bus.user, RAM_BASE + 0x400u + 4u*i, PROG_COUNTED[i]);
        ref_bus.write32(ref_bus.user, RAM_BASE + 0x400u + 4u*i, PROG_COUNTED[i]);
    }
    arm920t_reset(cpu_fast, RAM_BASE + 0x400u); arm920t_reset(cpu_ref, RAM_BASE + 0x400u);
    set_reg_both(0, 0x1560000cu); set_reg_both(1, 0x15600030u);
    set_reg_both(2, 10000u); set_reg_both(3, UINT32_MAX); set_reg_both(4, UINT32_MAX);
    arm920t_set_jit(cpu_fast, 1);
    arm920t_set_trace(cpu_ref, 1, NULL, NULL);
    static const uint32_t masks[] = {0u, GP32_BUTTON_A | GP32_BUTTON_START,
                                    GP32_BUTTON_UP | GP32_BUTTON_SELECT};
    for (unsigned m = 0; m < 3; ++m) {
        s3c2400_set_buttons(soc_fast, masks[m]); s3c2400_set_buttons(soc_ref, masks[m]);
        fast_bus.write32(fast_bus.user, 0x15600030u, m << 8);
        ref_bus.write32(ref_bus.user, 0x15600030u, m << 8);
        for (size_t i = 0; i < NCHUNKS; ++i) {
            CHECK(s3c2400_run_cpu(soc_fast, CHUNKS[i]) == s3c2400_run_cpu(soc_ref, CHUNKS[i]),
                  "GPIO run/tick budget mismatch");
            compare_state(0);
        }
    }
    gp32_cpu_profile_t p;
    arm920t_get_cpu_profile(cpu_fast, &p);
    if (p.supported) CHECK(p.poll_skipped_insns > 0, "real GPIO countdown did not accelerate");
    teardown_pair();
    s3c2400_destroy(soc_fast); s3c2400_destroy(soc_ref);
}

int main(void) {
    case_poll_stable(0);
    case_poll_stable(1);   /* identical contract when RAM loads ride fastmem */
    case_false_progress();
    case_poll_exits();
    case_literal();
    case_bl_helper();
    case_mmio_stack();
    case_idempotent_store_timer_leaf();
    case_store_modified_restored();
    case_mmio_poll_store();
    case_poll_store_no_write_fastmem();
    case_pointer_chain();
    case_callback_off();
    case_changing_load_addr();
    case_base_modified_restored();
    case_add_loop();
    case_store_loop();
    case_pending_irq();
    case_run_deadlines();
    case_counted_poll();
    case_counted_boundaries();
    case_counted_refusals();
    case_counted_native_fallback();
    case_counted_callbacks();
    case_counted_restore();
    case_counted_mmu();
    case_counted_gpio();
    if (failures) {
        fprintf(stderr, "arm poll: %d failures\n", failures);
        return 1;
    }
    puts("PASS: stable/countdown poll equivalence, reads, exit boundaries, refusal, callbacks, state, MMU, GPIO");
    return 0;
}
