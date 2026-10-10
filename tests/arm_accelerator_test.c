/* Differential regression for the arm920t accelerator registry.
 *
 * arm920t_set_accelerators binds frontend callbacks to exact guest PCs. A
 * callback retires at most the budget it is handed, leaves the machine
 * untouched when it returns zero so the original code can run, and is only
 * dispatched while the JIT is enabled and tracing is off. Trace stitching
 * must not step over a registered entry, including the BL target and the loop
 * back-edge shapes. The same short math loop runs on two CPUs built from the
 * public header: jit=0 without dispatch (the original interpreter is the
 * oracle) and jit=1 with the entries registered, over ragged run budgets.
 * After every budget the whole architectural state - 16 registers, PC, CPSR,
 * cumulative cycles and the RAM image - must match, and the returned
 * instruction counts must be equal.
 *
 * The fixture maps the real GP32 windows (512 KiB BIOS plus 8 MiB SDRAM at
 * 0x0c000000) so the fastmem-backed JIT actually engages, and the loop result
 * is asserted on the oracle so a mis-encoded program fails loudly. Covered:
 * budget accounting with zero-retire fallback and partial-loop resumes,
 * dispatch coverage for the BL/leaf entry and for the entry re-reached
 * through the stitched loop back-edge, registration validation, in-run
 * rejection, disable/flush, registration preserved across reset and state
 * load, and arm920t_peek_identity_ram's RAM window and MMU rules.
 */

#include "arm920t.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BIOS_SIZE  0x80000u
#define RAM_BASE   0x0c000000u
#define RAM_SIZE   0x800000u
#define CODE_ADDR  0x00000400u
#define DATA_ADDR  (RAM_BASE + 0x1000u)
#define IO_ADDR    0x14000000u
#define TABLE_ADDR (RAM_BASE + 0x8000u)          /* 16 KiB-aligned section table */
#define FAR_VA     0x0c200000u                   /* section mapped non-identity */
#define FAR_PA     0x0c300000u
#define PEEK_DATA  0x13572468u
#define PARK_WORD  0xeafffffeu                   /* B . */

/* Program layout in the BIOS region. 0x40 and 0x44 are both registered, so
 * the BL target (a one-instruction leaf prologue) and the loop head are
 * accelerator entries; the BNE at 0x4c is the back-edge the JIT would stitch
 * straight into 0x44. */
#define P_ENTRY   0x00u
#define P_BL      0x04u
#define P_CAPTURE 0x08u
#define P_PARK    0x0cu
#define P_SUB     0x40u
#define P_LOOP    0x44u
#define P_SUBS    0x48u
#define P_BNE     0x4cu
#define P_RET     0x50u

#define LOOP_COUNT 32u
#define PER_ITER   3u                            /* ADD, SUBS, BNE per iteration */
#define LOOP_TOTAL ((LOOP_COUNT * (LOOP_COUNT + 1u)) / 2u)
#define MAX_REPORT 12

typedef struct {
    arm_bus_t bus;
    uint8_t bios[BIOS_SIZE];
    uint8_t ram[RAM_SIZE];
    unsigned trace_lines;
} test_bus_t;

/* Per-CPU accelerator observations. Every callback bound to the accelerated
 * CPU records here; the structures bound to the interpreter CPU must stay
 * untouched because dispatch is gated by the JIT. */
typedef struct {
    unsigned sub_calls, loop_calls;
    unsigned zero_retires;                       /* returned 0 without touching state */
    unsigned retire_violations;                  /* reported more than the budget */
    unsigned pc_violations;                      /* dispatched at a different PC */
    unsigned lr_violations;                      /* leaf entry reached without the link */
    unsigned refuse_left;                        /* staged: decline the first N loop calls */
    unsigned guard_tries, guard_ret;             /* in-run re-registration attempt */
    unsigned peeked, peek_in_run_ok;
    uint32_t min_r1_seen, max_r1_seen;
    uint64_t retired_total;
} accel_state_t;

typedef struct {
    uint64_t cycles;
    uint32_t pc, cpsr, cp15_c1;
} machine_snapshot_t;

static test_bus_t bus_jit, bus_ref;
static arm920t_t *cpu_jit, *cpu_ref;
static accel_state_t state_jit, state_ref;
static accel_state_t early_report;           /* the ragged stage, kept for the summary */
static unsigned shadow_calls;
static const char *current_case = "init";
static int failures;

static void fail(const char *what) {
    if (failures < MAX_REPORT) fprintf(stderr, "FAIL[%s]: %s\n", current_case, what);
    ++failures;
}
static void report(const char *what, uint64_t jit, uint64_t ref) {
    if (failures < MAX_REPORT)
        fprintf(stderr, "FAIL[%s]: %s jit=0x%016" PRIx64 " ref=0x%016" PRIx64 "\n",
                current_case, what, jit, ref);
    ++failures;
}
#define CHECK(cond, msg) do { if (!(cond)) fail(msg); } while (0)

static uint8_t *bus_ptr(test_bus_t *b, uint32_t a, size_t bytes) {
    if (a < BIOS_SIZE && bytes <= BIOS_SIZE - a) return b->bios + a;
    if (a >= RAM_BASE && (uint64_t)(a - RAM_BASE) + bytes <= RAM_SIZE) return b->ram + (a - RAM_BASE);
    return NULL;
}
static uint8_t tb_read8(void *u, uint32_t a) {
    uint8_t *p = bus_ptr((test_bus_t *)u, a, 1u);
    return p ? p[0] : 0xffu;
}
static uint16_t tb_read16(void *u, uint32_t a) {
    uint8_t *p = bus_ptr((test_bus_t *)u, a, 2u);
    return p ? gp32_ld16le(p) : 0xffffu;
}
static uint32_t tb_read32(void *u, uint32_t a) {
    uint8_t *p = bus_ptr((test_bus_t *)u, a, 4u);
    return p ? gp32_ld32le(p) : UINT32_MAX;
}
static void tb_write8(void *u, uint32_t a, uint8_t v) {
    uint8_t *p = bus_ptr((test_bus_t *)u, a, 1u);
    if (p) p[0] = v;
}
static void tb_write16(void *u, uint32_t a, uint16_t v) {
    uint8_t *p = bus_ptr((test_bus_t *)u, a, 2u);
    if (p) gp32_st16le(p, v);
}
static void tb_write32(void *u, uint32_t a, uint32_t v) {
    uint8_t *p = bus_ptr((test_bus_t *)u, a, 4u);
    if (p) gp32_st32le(p, v);
}
/* The fastmem contract must cover the full BIOS window and the whole SDRAM
 * window at 0x0c000000, exactly like the real frontend bus, or the native
 * backends silently fall back to checked helpers instead of direct RAM. */
static uint8_t *tb_fastmem(void *u, uint32_t a, size_t bytes, int write) {
    (void)write;
    return bus_ptr((test_bus_t *)u, a, bytes);
}
static void tb_trace(void *user, const char *line) {
    test_bus_t *b = (test_bus_t *)user;
    if (line && (line[0] == 'A' || line[0] == 'T')) ++b->trace_lines;
}

static machine_snapshot_t snapshot_machine(const arm920t_t *cpu) {
    machine_snapshot_t s;
    s.cycles = arm920t_get_cycles(cpu);
    s.pc = arm920t_get_pc(cpu);
    s.cpsr = arm920t_get_cpsr(cpu);
    s.cp15_c1 = arm920t_get_cp15(cpu, 1u);
    return s;
}
static int machine_unchanged(const arm920t_t *cpu, const machine_snapshot_t *s) {
    return arm920t_get_cycles(cpu) == s->cycles && arm920t_get_pc(cpu) == s->pc &&
           arm920t_get_cpsr(cpu) == s->cpsr && arm920t_get_cp15(cpu, 1u) == s->cp15_c1;
}

/* Two shadow callbacks bound to the same PCs: an in-run registration attempt
 * must be rejected, so these counters must stay zero. */
static uint32_t shadow_sub(void *user, arm920t_t *cpu, uint32_t budget) {
    GP32_UNUSED(user); GP32_UNUSED(cpu); GP32_UNUSED(budget);
    ++shadow_calls;
    return 0u;
}
static uint32_t shadow_loop(void *user, arm920t_t *cpu, uint32_t budget) {
    GP32_UNUSED(user); GP32_UNUSED(cpu); GP32_UNUSED(budget);
    ++shadow_calls;
    return 0u;
}
static const arm_accel_entry_t SHADOW_ENTRIES[] = {
    {CODE_ADDR + P_SUB, shadow_sub},
    {CODE_ADDR + P_LOOP, shadow_loop},
};

/* Leaf entry: the MOV r1,#LOOP_COUNT prologue of the BL callee. */
static uint32_t accel_sub(void *user, arm920t_t *cpu, uint32_t budget) {
    accel_state_t *s = (accel_state_t *)user;
    ++s->sub_calls;
    if (arm920t_get_pc(cpu) != CODE_ADDR + P_SUB) ++s->pc_violations;
    if (arm920t_get_reg(cpu, 14u) != CODE_ADDR + P_CAPTURE) ++s->lr_violations;
    if (budget < 1u) return 0u;                  /* untouched: let the original run */
    arm920t_set_reg(cpu, 1u, LOOP_COUNT);        /* MOV r1,#LOOP_COUNT */
    arm920t_set_reg(cpu, 15u, CODE_ADDR + P_LOOP);
    return 1u;
}

/* Loop entry: retires whole ADD/SUBS/BNE iterations and hands the remainder
 * back to the interpreter by returning zero without touching any state. */
static uint32_t accel_loop(void *user, arm920t_t *cpu, uint32_t budget) {
    accel_state_t *s = (accel_state_t *)user;
    ++s->loop_calls;
    if (s->guard_tries == 0u) {
        /* Re-registering from inside a run is forbidden and must not change
         * the active entries. */
        s->guard_tries = 1u;
        s->guard_ret = (unsigned)arm920t_set_accelerators(cpu, SHADOW_ENTRIES,
                                                          GP32_ARRAY_COUNT(SHADOW_ENTRIES), NULL);
    }
    if (arm920t_get_pc(cpu) != CODE_ADDR + P_LOOP) { ++s->pc_violations; return 0u; }
    if (!s->peeked) {
        s->peeked = 1u;
        uint8_t *p = arm920t_peek_identity_ram(cpu, DATA_ADDR, 4u, 0);
        s->peek_in_run_ok = (unsigned)(p != NULL && gp32_ld32le(p) == PEEK_DATA);
    }
    uint32_t left = arm920t_get_reg(cpu, 1u);
    if (s->max_r1_seen == 0u || left > s->max_r1_seen) s->max_r1_seen = left;
    if (s->min_r1_seen == 0u || left < s->min_r1_seen) s->min_r1_seen = left;
    if (s->refuse_left) { --s->refuse_left; ++s->zero_retires; return 0u; }
    uint32_t iters = budget / PER_ITER;
    if (iters > left) iters = left;
    if (iters == 0u) { ++s->zero_retires; return 0u; }
    uint32_t acc = arm920t_get_reg(cpu, 0u), rest = left;
    for (uint32_t i = 0u; i < iters; ++i) { acc += rest; --rest; }
    arm920t_set_reg(cpu, 0u, acc);
    arm920t_set_reg(cpu, 1u, rest);
    /* SUBS r1,r1,#1 with 1 <= r1 <= LOOP_COUNT: C=1, N=V=0, Z only at zero. */
    uint32_t cpsr = arm920t_get_cpsr(cpu);
    arm920t_set_cpsr(cpu, (cpsr & ~0xf0000000u) | 0x20000000u | (rest == 0u ? 0x40000000u : 0u));
    arm920t_set_reg(cpu, 15u, CODE_ADDR + (rest ? P_LOOP : P_RET));
    uint32_t retired = iters * PER_ITER;
    if (retired > budget) ++s->retire_violations;
    s->retired_total += retired;
    return retired;
}

static const arm_accel_entry_t ENTRIES[] = {
    {CODE_ADDR + P_SUB, accel_sub},
    {CODE_ADDR + P_LOOP, accel_loop},
};

static void setup_pair(void) {
    for (test_bus_t *b = &bus_jit; b; b = (b == &bus_jit) ? &bus_ref : NULL) {
        memset(b, 0, sizeof(*b));
        for (uint32_t a = 0; a < BIOS_SIZE; a += 4u)
            gp32_st32le(b->bios + a, PARK_WORD); /* B . bounds stray control flow */
        b->bus.read8 = tb_read8;
        b->bus.read16 = tb_read16;
        b->bus.read32 = tb_read32;
        b->bus.write8 = tb_write8;
        b->bus.write16 = tb_write16;
        b->bus.write32 = tb_write32;
        b->bus.fastmem = tb_fastmem;
        b->bus.user = b;
    }
    cpu_ref = arm920t_create(&bus_ref.bus);
    cpu_jit = arm920t_create(&bus_jit.bus);
    if (!cpu_jit || !cpu_ref) { fprintf(stderr, "arm920t_create failed\n"); exit(2); }
    arm920t_reset(cpu_ref, CODE_ADDR);
    arm920t_reset(cpu_jit, CODE_ADDR);
    arm920t_set_jit(cpu_ref, 0);                 /* the original interpreter is the oracle */
    arm920t_set_jit(cpu_jit, 1);
}
static void teardown_pair(void) {
    arm920t_destroy(cpu_jit);
    arm920t_destroy(cpu_ref);
    cpu_jit = cpu_ref = NULL;
}
static void compare_state(void) {
    for (unsigned i = 0; i < 16u; ++i) {
        uint32_t j = arm920t_get_reg(cpu_jit, i), r = arm920t_get_reg(cpu_ref, i);
        if (j != r) { char nm[8]; snprintf(nm, sizeof(nm), "r%u", i); report(nm, j, r); }
    }
    if (arm920t_get_pc(cpu_jit) != arm920t_get_pc(cpu_ref))
        report("PC", arm920t_get_pc(cpu_jit), arm920t_get_pc(cpu_ref));
    if (arm920t_get_cpsr(cpu_jit) != arm920t_get_cpsr(cpu_ref))
        report("CPSR", arm920t_get_cpsr(cpu_jit), arm920t_get_cpsr(cpu_ref));
    if (arm920t_get_cycles(cpu_jit) != arm920t_get_cycles(cpu_ref))
        report("cycles", arm920t_get_cycles(cpu_jit), arm920t_get_cycles(cpu_ref));
    CHECK(!memcmp(bus_jit.ram, bus_ref.ram, RAM_SIZE), "RAM image mismatch");
}
static void set_mem_both(uint32_t addr, uint32_t v) {
    gp32_st32le(bus_ptr(&bus_jit, addr, 4u), v);
    gp32_st32le(bus_ptr(&bus_ref, addr, 4u), v);
}
static void load_words_pair(uint32_t addr, const uint32_t *words, size_t count) {
    for (size_t i = 0; i < count; ++i) set_mem_both(addr + (uint32_t)i * 4u, words[i]);
}
/* Ragged budgets: small values land mid-block and inside the loop, larger ones
 * let callbacks retire whole iteration groups. */
static const uint32_t EARLY[] = {1u, 2u, 3u, 5u, 8u, 13u, 21u, 34u}; /* stops mid-loop */
static const uint32_t LATE[]  = {55u, 89u};                          /* finishes and parks */
static const uint32_t FULL[]  = {1u, 2u, 3u, 5u, 8u, 13u, 21u, 34u, 55u, 89u};

static void run_chunks(const uint32_t *chunks, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        uint32_t dj = arm920t_run(cpu_jit, chunks[i]);
        uint32_t dr = arm920t_run(cpu_ref, chunks[i]);
        if (dj != dr) report("arm920t_run budget", dj, dr);
        compare_state();
    }
}
static int mid_loop(const arm920t_t *cpu) {
    uint32_t pc = arm920t_get_pc(cpu);
    return arm920t_get_reg(cpu, 1u) > 0u && arm920t_get_reg(cpu, 1u) < LOOP_COUNT &&
           arm920t_get_reg(cpu, 0u) != LOOP_TOTAL &&
           (pc == CODE_ADDR + P_LOOP || pc == CODE_ADDR + P_SUBS || pc == CODE_ADDR + P_BNE);
}

static void load_program(void) {
    static const struct { uint32_t off, word; } program[] = {
        {P_ENTRY,   0xe3a00000u}, /* MOV r0,#0 */
        {P_BL,      0xeb00000du}, /* BL 0x40 (leaf prologue), link 0x08 */
        {P_CAPTURE, 0xe1a03000u}, /* MOV r3,r0 */
        {P_PARK,    PARK_WORD},   /* B . */
        {P_SUB,     0xe3a01020u}, /* MOV r1,#LOOP_COUNT */
        {P_LOOP,    0xe0800001u}, /* ADD r0,r0,r1 */
        {P_SUBS,    0xe2511001u}, /* SUBS r1,r1,#1 */
        {P_BNE,     0x1afffffcu}, /* BNE 0x44 */
        {P_RET,     0xe12fff1eu}, /* BX LR */
    };
    for (size_t i = 0; i < GP32_ARRAY_COUNT(program); ++i)
        set_mem_both(CODE_ADDR + program[i].off, program[i].word);
    set_mem_both(DATA_ADDR, PEEK_DATA);
}

static void case_accelerators(void) {
    current_case = "accelerators";
    setup_pair();
    load_program();

    const arm_accel_entry_t unaligned[] = {{CODE_ADDR + P_SUB + 2u, accel_sub}};
    const arm_accel_entry_t duplicate[] = {{CODE_ADDR + P_SUB, accel_sub},
                                           {CODE_ADDR + P_SUB, accel_loop}};
    const arm_accel_entry_t null_callback[] = {{CODE_ADDR + P_SUB, NULL}};

    /* Validation on an unregistered CPU. */
    CHECK(arm920t_set_accelerators(cpu_jit, unaligned, GP32_ARRAY_COUNT(unaligned), NULL) == 0,
          "an unaligned PC must be rejected");
    CHECK(arm920t_set_accelerators(cpu_jit, duplicate, GP32_ARRAY_COUNT(duplicate), NULL) == 0,
          "a duplicated PC must be rejected");
    CHECK(arm920t_set_accelerators(cpu_jit, null_callback, GP32_ARRAY_COUNT(null_callback), NULL) == 0,
          "a NULL callback must be rejected");
    CHECK(arm920t_set_accelerators(cpu_jit, NULL, 1u, NULL) == 0,
          "NULL entries with a nonzero count must be rejected");
    CHECK(arm920t_set_accelerators(cpu_jit, NULL, 0u, NULL) == 1,
          "NULL+0 must clear an empty registration");
    CHECK(arm920t_set_accelerators(cpu_jit, ENTRIES, GP32_ARRAY_COUNT(ENTRIES), &state_jit) == 1,
          "aligned unique entries with callbacks must register");
    CHECK(arm920t_set_accelerators(cpu_ref, ENTRIES, GP32_ARRAY_COUNT(ENTRIES), &state_ref) == 1,
          "the interpreter CPU accepts the same registration");

    /* A run budget of exactly one callback group must dispatch the entry and
     * retire exactly that group, resuming at the entry for the next one. */
    arm920t_reset(cpu_jit, CODE_ADDR);
    arm920t_reset(cpu_ref, CODE_ADDR);
    static const uint32_t GROUP[] = {1u, 2u, PER_ITER}; /* MOV, BL+leaf, one group */
    run_chunks(GROUP, GP32_ARRAY_COUNT(GROUP));
    CHECK(state_jit.sub_calls == 1u, "the BL leaf entry must be dispatched on each pass");
    CHECK(state_jit.lr_violations == 0u, "the leaf entry must be reached through the BL link");
    CHECK(state_jit.loop_calls == 1u && state_jit.retired_total == PER_ITER,
          "a budget equal to the group length must retire exactly one group");
    CHECK(state_jit.zero_retires == 0u, "a covered group must not decline");
    CHECK(arm920t_get_reg(cpu_jit, 1u) == LOOP_COUNT - 1u, "one group must retire one iteration");
    CHECK(arm920t_get_pc(cpu_jit) == CODE_ADDR + P_LOOP, "an unfinished loop must resume at the entry");

    /* A budget shorter than the group must decline without touching any
     * state, leaving the original instruction to the interpreter. */
    memset(&state_jit, 0, sizeof(state_jit));
    arm920t_reset(cpu_jit, CODE_ADDR);
    arm920t_reset(cpu_ref, CODE_ADDR);
    static const uint32_t SHORT[] = {1u, 2u, 1u};   /* the last chunk lands on the entry */
    run_chunks(SHORT, GP32_ARRAY_COUNT(SHORT));
    CHECK(state_jit.loop_calls == 1u && state_jit.retired_total == 0u && state_jit.zero_retires == 1u,
          "a budget shorter than the group must decline untouched");
    CHECK(arm920t_get_reg(cpu_jit, 1u) == LOOP_COUNT && arm920t_get_pc(cpu_jit) == CODE_ADDR + P_SUBS,
          "the interpreter must retire the declined instruction itself");

    /* Ragged budgets with a staged refusal: covered calls may still return
     * zero, tiny budgets decline, and partial groups resume at the entry. */
    memset(&state_jit, 0, sizeof(state_jit));
    memset(&state_ref, 0, sizeof(state_ref));
    arm920t_reset(cpu_jit, CODE_ADDR);
    arm920t_reset(cpu_ref, CODE_ADDR);
    state_jit.refuse_left = 2u;                  /* first covered loop calls decline */
    run_chunks(EARLY, GP32_ARRAY_COUNT(EARLY));
    early_report = state_jit;

    CHECK(mid_loop(cpu_ref), "staging: the early chunks must stop inside the loop");
    CHECK(state_jit.sub_calls == 1u, "the BL leaf entry must be dispatched on each pass");
    CHECK(state_jit.lr_violations == 0u, "the leaf entry must be reached through the BL link");
    CHECK(state_jit.loop_calls >= 3u, "the loop entry must be dispatched");
    CHECK(state_jit.pc_violations == 0u, "accelerators must run at their exact PC only");
    CHECK(state_jit.retire_violations == 0u, "a callback must never retire more than its budget");
    CHECK(state_jit.zero_retires >= 2u, "a zero-retire callback must leave the original code to run");
    CHECK(state_jit.retired_total > 0u, "part of the loop must be retired by the callback");
    CHECK(state_jit.retired_total <= LOOP_COUNT * PER_ITER, "retired work stays inside the loop");
    CHECK(state_jit.min_r1_seen < LOOP_COUNT && state_jit.max_r1_seen == LOOP_COUNT,
          "the entry must be re-reached through the stitched back-edge, not inlined away");
    CHECK(state_jit.guard_tries == 1u && state_jit.guard_ret == 0u,
          "re-registering from inside a run must be rejected");
    CHECK(shadow_calls == 0u, "a rejected registration must never take effect");
    CHECK(state_jit.peek_in_run_ok != 0u, "peek must serve a registered callback during its run");
    CHECK(state_ref.sub_calls == 0u && state_ref.loop_calls == 0u,
          "a JIT-disabled CPU must never dispatch registered accelerators");

    /* Failed attempts after live registration must leave the entries alone:
     * the stages below keep using the registration made above. */
    CHECK(arm920t_set_accelerators(cpu_jit, unaligned, GP32_ARRAY_COUNT(unaligned), NULL) == 0,
          "a rejected registration must not disturb the active entries");
    CHECK(arm920t_set_accelerators(cpu_jit, duplicate, GP32_ARRAY_COUNT(duplicate), NULL) == 0,
          "a rejected duplicate must not disturb the active entries");
    CHECK(arm920t_set_accelerators(cpu_jit, null_callback, GP32_ARRAY_COUNT(null_callback), NULL) == 0,
          "a rejected NULL callback must not disturb the active entries");

    /* Snapshot the mid-loop image for the state-load stage. */
    FILE *jit_snap = tmpfile(), *ref_snap = tmpfile();
    CHECK(jit_snap && ref_snap, "snapshot files must open");
    if (jit_snap) CHECK(arm920t_state_save(cpu_jit, jit_snap) != 0, "mid-loop state save");
    if (ref_snap) CHECK(arm920t_state_save(cpu_ref, ref_snap) != 0, "mid-loop state save");

    /* A reset must preserve the registration and its dispatch. */
    memset(&state_jit, 0, sizeof(state_jit));
    memset(&state_ref, 0, sizeof(state_ref));
    arm920t_reset(cpu_jit, CODE_ADDR);
    arm920t_reset(cpu_ref, CODE_ADDR);
    run_chunks(FULL, GP32_ARRAY_COUNT(FULL));
    CHECK(state_jit.sub_calls > 0u && state_jit.loop_calls > 0u,
          "reset must preserve the registration");
    CHECK(state_jit.pc_violations == 0u && state_jit.retire_violations == 0u &&
          state_jit.lr_violations == 0u, "post-reset dispatch discipline");
    CHECK(arm920t_get_reg(cpu_ref, 0u) == LOOP_TOTAL && arm920t_get_reg(cpu_ref, 3u) == LOOP_TOTAL &&
          arm920t_get_reg(cpu_ref, 1u) == 0u, "the loop must compute 32+...+1 and capture it");

    /* A state load must preserve the registration too: reload the mid-loop
     * image and let the loop finish again. */
    if (jit_snap) rewind(jit_snap);
    if (ref_snap) rewind(ref_snap);
    if (jit_snap) CHECK(arm920t_state_load(cpu_jit, jit_snap) != 0, "mid-loop state load");
    if (ref_snap) CHECK(arm920t_state_load(cpu_ref, ref_snap) != 0, "mid-loop state load");
    memset(&state_jit, 0, sizeof(state_jit));
    compare_state();
    CHECK(mid_loop(cpu_jit), "the reloaded state must still be mid-loop");
    run_chunks(LATE, GP32_ARRAY_COUNT(LATE));
    CHECK(state_jit.loop_calls > 0u, "state load must preserve the registration");
    CHECK(arm920t_get_reg(cpu_jit, 0u) == LOOP_TOTAL && arm920t_get_reg(cpu_jit, 3u) == LOOP_TOTAL,
          "the reloaded loop must finish to the oracle value");
    if (jit_snap) fclose(jit_snap);
    if (ref_snap) fclose(ref_snap);

    /* Dropping the registration mid-loop must flush translated code and stop
     * the dispatch while the loop head block is still live. */
    memset(&state_jit, 0, sizeof(state_jit));
    arm920t_reset(cpu_jit, CODE_ADDR);
    arm920t_reset(cpu_ref, CODE_ADDR);
    run_chunks(EARLY, GP32_ARRAY_COUNT(EARLY));
    CHECK(mid_loop(cpu_ref), "staging: the loop head must be live when the registration is dropped");
    unsigned calls_before = state_jit.sub_calls + state_jit.loop_calls;
    CHECK(arm920t_set_accelerators(cpu_jit, NULL, 0u, NULL) == 1,
          "NULL+0 must disable an active registration");
    run_chunks(LATE, GP32_ARRAY_COUNT(LATE));
    CHECK(state_jit.sub_calls + state_jit.loop_calls == calls_before,
          "a disabled registration must never be dispatched again");
    CHECK(arm920t_get_reg(cpu_jit, 0u) == LOOP_TOTAL && arm920t_get_reg(cpu_jit, 3u) == LOOP_TOTAL,
          "the loop must still finish once the accelerators are gone");

    /* Re-registering after a disable must work and dispatch again. */
    memset(&state_jit, 0, sizeof(state_jit));
    CHECK(arm920t_set_accelerators(cpu_jit, ENTRIES, GP32_ARRAY_COUNT(ENTRIES), &state_jit) == 1,
          "a fresh registration after a disable must be accepted");
    arm920t_reset(cpu_jit, CODE_ADDR);
    arm920t_reset(cpu_ref, CODE_ADDR);
    run_chunks(FULL, GP32_ARRAY_COUNT(FULL));
    CHECK(state_jit.sub_calls > 0u && state_jit.loop_calls > 0u,
          "the re-registered accelerators must dispatch");
    CHECK(state_jit.pc_violations == 0u && state_jit.retire_violations == 0u &&
          state_jit.lr_violations == 0u, "re-registered dispatch discipline");

    /* Tracing gates the dispatch off entirely, and the traced run must still
     * match the interpreter. */
    memset(&state_jit, 0, sizeof(state_jit));
    bus_jit.trace_lines = 0;
    arm920t_set_trace(cpu_jit, 1, tb_trace, &bus_jit);
    arm920t_set_trace(cpu_ref, 1, tb_trace, &bus_ref);
    arm920t_reset(cpu_jit, CODE_ADDR);
    arm920t_reset(cpu_ref, CODE_ADDR);
    run_chunks(FULL, GP32_ARRAY_COUNT(FULL));
    CHECK(state_jit.sub_calls == 0u && state_jit.loop_calls == 0u,
          "a traced CPU must not dispatch registered accelerators");
    CHECK(bus_jit.trace_lines > 0u, "the trace stage must actually trace");
    arm920t_set_trace(cpu_jit, 0, NULL, NULL);
    arm920t_set_trace(cpu_ref, 0, NULL, NULL);
    CHECK(arm920t_get_jit_hits(cpu_jit) + arm920t_get_jit_misses(cpu_jit) > 0u,
          "the JIT translation path never engaged");
    teardown_pair();
}

static void case_peek_identity_ram(void) {
    current_case = "peek-identity-ram";
    setup_pair();

    /* MMU off: the SDRAM window is directly addressable. */
    uint8_t *p_read = arm920t_peek_identity_ram(cpu_jit, DATA_ADDR, 4u, 0);
    uint8_t *p_write = arm920t_peek_identity_ram(cpu_jit, DATA_ADDR, 4u, 1);
    CHECK(p_read != NULL && p_read == bus_ptr(&bus_jit, DATA_ADDR, 4u),
          "peek must return the direct RAM pointer of the bus window");
    CHECK(p_write == p_read, "the write flag must not change the identity RAM pointer");
    if (p_write) gp32_st32le(p_write, 0x0badf00du);   /* poke through the peeked pointer */
    gp32_st32le(bus_ptr(&bus_ref, DATA_ADDR, 4u), 0x0badf00du); /* the oracle bus sees it too */
    static const uint32_t load_program_words[] = {0xe5910000u /* LDR r0,[r1] */, PARK_WORD};
    load_words_pair(CODE_ADDR, load_program_words, GP32_ARRAY_COUNT(load_program_words));
    arm920t_set_reg(cpu_jit, 1u, DATA_ADDR);
    arm920t_set_reg(cpu_ref, 1u, DATA_ADDR);
    uint32_t dj = arm920t_run(cpu_jit, 1u), dr = arm920t_run(cpu_ref, 1u);
    if (dj != dr) report("peek alias budget", dj, dr);
    compare_state();
    CHECK(arm920t_get_reg(cpu_jit, 0u) == 0x0badf00du,
          "the value poked through the peeked pointer must be guest visible");

    /* Refusals must not move machine state: no table walks, faults or MMIO. */
    machine_snapshot_t before = snapshot_machine(cpu_jit);
    CHECK(arm920t_peek_identity_ram(cpu_jit, IO_ADDR, 4u, 0) == NULL, "the I/O window is not RAM");
    CHECK(arm920t_peek_identity_ram(cpu_jit, IO_ADDR, 4u, 1) == NULL, "the I/O window is not RAM for writes");
    CHECK(arm920t_peek_identity_ram(cpu_jit, CODE_ADDR, 4u, 0) == NULL, "the BIOS window is not RAM");
    CHECK(arm920t_peek_identity_ram(cpu_jit, RAM_BASE + RAM_SIZE - 2u, 4u, 0) == NULL,
          "a window crossing the RAM end must be refused");
    CHECK(arm920t_peek_identity_ram(cpu_jit, RAM_BASE + RAM_SIZE, 4u, 0) == NULL,
          "a window past the RAM end must be refused");
    CHECK(machine_unchanged(cpu_jit, &before), "a refused peek must not move machine state");

    /* MMU on with identity sections only: cached identity mappings still
     * serve the pointer, and the staging really enabled the MMU. */
    arm920t_reset(cpu_jit, CODE_ADDR);
    arm920t_reset(cpu_ref, CODE_ADDR);
    set_mem_both(TABLE_ADDR, 0x00000002u);                       /* VA 0x00000000-0x000fffff */
    set_mem_both(TABLE_ADDR + 0xc0u * 4u, 0x0c000002u);          /* VA 0x0c000000 identity */
    set_mem_both(DATA_ADDR, PEEK_DATA);
    static const uint32_t mmu_identity_program[] = {
        0xee02af10u, /* MCR p15,0,r10,c2,c0,0 : table base */
        0xee01bf10u, /* MCR p15,0,r11,c1,c0,0 : control, M=1 */
        0xe5943000u, /* LDR r3,[r4] : identity VA */
        PARK_WORD
    };
    load_words_pair(CODE_ADDR, mmu_identity_program, GP32_ARRAY_COUNT(mmu_identity_program));
    arm920t_set_reg(cpu_jit, 10u, TABLE_ADDR);
    arm920t_set_reg(cpu_ref, 10u, TABLE_ADDR);
    arm920t_set_reg(cpu_jit, 11u, 1u);
    arm920t_set_reg(cpu_ref, 11u, 1u);
    arm920t_set_reg(cpu_jit, 4u, DATA_ADDR);
    arm920t_set_reg(cpu_ref, 4u, DATA_ADDR);
    dj = arm920t_run(cpu_jit, 8u);
    dr = arm920t_run(cpu_ref, 8u);
    if (dj != dr) report("mmu identity budget", dj, dr);
    compare_state();
    CHECK((arm920t_get_cp15(cpu_jit, 1u) & 1u) != 0u,
          "staging: the MMU must be enabled before the identity peek");
    CHECK(arm920t_get_reg(cpu_jit, 3u) == PEEK_DATA, "the identity-mapped load value");
    CHECK(arm920t_peek_identity_ram(cpu_jit, DATA_ADDR, 4u, 0) == bus_ptr(&bus_jit, DATA_ADDR, 4u),
          "cached identity mappings must still serve the RAM pointer");

    /* A cached non-identity section must stop the pointer for its own span,
     * and an uncached span must not be walked into one. */
    set_mem_both(TABLE_ADDR + (FAR_VA >> 20) * 4u, (FAR_PA & 0xfff00000u) | 2u);
    set_mem_both(FAR_PA, 0x24681357u);
    static const uint32_t far_program[] = {0xe5943000u /* LDR r3,[r4] */, PARK_WORD};
    load_words_pair(CODE_ADDR, far_program, GP32_ARRAY_COUNT(far_program));
    arm920t_flush_jit(cpu_jit);                  /* the host rewrote guest code */
    arm920t_flush_jit(cpu_ref);
    arm920t_set_reg(cpu_jit, 4u, FAR_VA);
    arm920t_set_reg(cpu_ref, 4u, FAR_VA);
    arm920t_set_reg(cpu_jit, 15u, CODE_ADDR);
    arm920t_set_reg(cpu_ref, 15u, CODE_ADDR);
    dj = arm920t_run(cpu_jit, 2u);
    dr = arm920t_run(cpu_ref, 2u);
    if (dj != dr) report("non-identity budget", dj, dr);
    compare_state();
    CHECK(arm920t_get_reg(cpu_jit, 3u) == 0x24681357u,
          "the non-identity section must translate the VA to the far PA");
    before = snapshot_machine(cpu_jit);
    CHECK(arm920t_peek_identity_ram(cpu_jit, FAR_VA, 4u, 0) == NULL,
          "a cached non-identity mapping must stop the peek");
    CHECK(arm920t_peek_identity_ram(cpu_jit, 0x0c500000u, 4u, 0) == NULL,
          "an uncached span must not be walked into a pointer");
    CHECK(machine_unchanged(cpu_jit, &before), "refused peeks must not move machine state");
    uint32_t tail[1] = {4u};
    run_chunks(tail, 1u);                        /* execution still matches the oracle */
    teardown_pair();
}

int main(void) {
    setup_pair();
    gp32_cpu_profile_t profile;
    arm920t_get_cpu_profile(cpu_jit, &profile);
    teardown_pair();
    if (!profile.native_backend) {
        puts("SKIP: arm920t native backend unavailable");
        return 0;
    }
    case_accelerators();
    case_peek_identity_ram();
    current_case = "summary";
    if (failures) {
        fprintf(stderr, "arm accelerators: %d mismatches\n", failures);
        return 1;
    }
    printf("PASS: arm accelerator registry differential (oracle=interpreter), "
           "ragged loop calls=%u retired=%" PRIu64 " zero-retires=%u\n",
           early_report.loop_calls, early_report.retired_total, early_report.zero_retires);
    return 0;
}
