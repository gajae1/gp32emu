/* Black-box equivalence tests for the stable-poll fast-forward path.
 *
 * Every case runs the same ARMv4 program, data, and run budgets through two
 * public-API arm920t instances. The "fast" CPU's bus exposes
 * is_stable_read32; the reference CPU's bus leaves it NULL. r[0..15], PC,
 * CPSR, and the total cycle count must match at every budget boundary; only
 * the number of observed bus reads may differ when the fast-forward engages.
 */

#include "arm920t.h"

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
    if (fastmem_ram) arm920t_set_jit(cpu_fast, 1);
    load_prog(PROG_POLL, sizeof(PROG_POLL) / sizeof(PROG_POLL[0]));
    add_stable(&bus_fast, DATA_ADDR);
    set_reg_both(1u, DATA_ADDR);

    uint64_t fr = 0, rr = 0;
    run_chunks(CHUNKS, NCHUNKS, 0, &fr, &rr);
    CHECK(bus_fast.stable_queries > 0u, "stability callback was never consulted");
    if (!fastmem_ram) {
        /* fastmem would bypass the read32 counter, so only the callback-bus
         * variant can observe the polling rate and the reduction. */
        CHECK(rr >= 250u, "reference did not poll the word enough times");
        CHECK(fr * 8u <= rr, "fast path did not significantly reduce polling reads");
    }
    teardown_pair();
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

int main(void) {
    case_poll_stable(0);
    case_poll_stable(1);   /* identical contract when RAM loads ride fastmem */
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
    if (failures) {
        fprintf(stderr, "arm poll: %d failures\n", failures);
        return 1;
    }
    puts("PASS: stable-poll equivalence, reduced reads, literal/BL/chain/timer-store-leaf, off/walk/basetmp/mmio/add/store/IRQ");
    return 0;
}
