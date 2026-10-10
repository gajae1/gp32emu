/* Differential regression for the arm920t native JIT.
 *
 * Each case runs one hand-encoded ARMv4 program through two arm920t instances
 * built from the public header: jit=0 (portable interpreter, the oracle) and
 * jit=1 (translated / native execution). Both buses are identical little-endian
 * RAM+BIOS images with fastmem, and neither installs is_stable_read32, so no
 * poll fast-forward can hide native execution. After every ragged run budget
 * the whole architectural state must match: 16 registers, PC, CPSR, the
 * cumulative cycle count and the RAM image. The same budget goes to both CPUs,
 * so a divergence in the returned count is itself a failure. A few results are
 * asserted on the oracle so a mis-encoded program fails loudly instead of
 * comparing two identically wrong runs.
 */

#include "arm920t.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BIOS_SIZE 0x80000u
#define RAM_BASE  0x0c000000u           /* GP32 SDRAM window */
/* A64 enables direct RAM loads/stores only after validating the full GP32
 * window. A smaller mock would silently exercise helpers instead. */
#define RAM_SIZE  0x800000u
#define CODE_ADDR 0x00000400u           /* program base inside the BIOS region */
#define DATA_ADDR (RAM_BASE + 0x1000u)
#define IO_ADDR   0x14000000u
#define MAX_REPORT 12

typedef struct {
    arm_bus_t bus;
    uint8_t bios[BIOS_SIZE];
    uint8_t ram[RAM_SIZE];
    arm920t_t *observe_cpu;
    uint32_t io_pc[8];
    unsigned io_count;
    /* Loop callback controls: raise IRQ once at the Nth IO_ADDR access and
     * count writes to IO_ADDR+4 that acknowledge it.  Zero when unused, so
     * the pre-existing cases observe no behavior change. */
    uint32_t io_raise_at;
    unsigned io_acks;
    unsigned io_flush_at, io_flushes;
    unsigned io_trace_at, trace_lines;
    unsigned io_return_at;
    unsigned block_io, block_effect, block_at, block_count;
    uint32_t block_addr[3], block_pc[3], block_base[3], block_value[3];
    unsigned mem_probe, mem_effect, mem_calls;
    uint32_t mem_addr, mem_pc, mem_base, mem_value;
    uint32_t live_pa, live_value;
    unsigned live_reads, live_writes, live_rebind_rejected;
} test_bus_t;

static test_bus_t bus_jit, bus_ref;
static arm920t_t *cpu_jit, *cpu_ref;
static const char *current_case = "init";
static int failures;
static uint64_t jit_events, jit_fallbacks;

static void fail(const char *what) {
    if (failures < MAX_REPORT) fprintf(stderr, "FAIL[%s]: %s\n", current_case, what);
    ++failures;
}
static void report(const char *what, uint64_t jit, uint64_t ref) {
    if (failures < MAX_REPORT)
        fprintf(stderr, "FAIL[%s]: %s jit=0x%016" PRIx64 " ref=0x%016" PRIx64 "\n", current_case,
                what, jit, ref);
    ++failures;
}
#define CHECK(cond, msg) do { if (!(cond)) fail(msg); } while (0)

static uint8_t *bus_ptr(test_bus_t *b, uint32_t a, size_t bytes) {
    if (a < BIOS_SIZE && bytes <= BIOS_SIZE - a) return b->bios + a;
    if (a >= RAM_BASE && (uint64_t)(a - RAM_BASE) + bytes <= RAM_SIZE) return b->ram + (a - RAM_BASE);
    return NULL;
}
static void tb_trace(void *user, const char *line) {
    test_bus_t *b=user;
    if(line && (line[0]=='A' || line[0]=='T')) ++b->trace_lines;
}

static uint32_t tb_io_value(test_bus_t *b, uint32_t a) {
    if (b->live_pa && a == b->live_pa) {
        ++b->live_reads;
        return b->live_value;
    }
    if (b->observe_cpu && a == IO_ADDR) {
        uint32_t pc = arm920t_get_pc(b->observe_cpu);
        if (b->io_count < GP32_ARRAY_COUNT(b->io_pc)) b->io_pc[b->io_count] = pc;
        ++b->io_count;
        /* Assert inside MMIO; the native helper must exit before executing
         * the following instruction with an unmasked pending interrupt. */
        if (b->io_raise_at && b->io_count == b->io_raise_at)
            arm920t_set_irq(b->observe_cpu, 1);
        if (b->io_trace_at && b->io_count == b->io_trace_at)
            arm920t_set_trace(b->observe_cpu, 1, tb_trace, b);
        if (b->io_return_at && b->io_count == b->io_return_at) {
            uint32_t sp=arm920t_get_reg(b->observe_cpu,13u);
            uint8_t *slot=bus_ptr(b,sp,4u);
            if(slot) gp32_st32le(slot,CODE_ADDR+0x0cu);
        }
        if (b->io_flush_at && b->io_count == b->io_flush_at) {
            gp32_st32le(b->bios + CODE_ADDR + 0x0cu, 0xe3a06077u); /* MOV r6,#0x77 */
            arm920t_flush_jit(b->observe_cpu);
            ++b->io_flushes;
        }
        return pc;
    }
    return UINT32_MAX;
}
/* Observe all lanes, including writeback timing, before raising an exit. */
static uint32_t tb_block_io(test_bus_t *b, uint32_t a, uint32_t value) {
    unsigned lane = b->block_count++;
    if (lane < 3u) {
        b->block_addr[lane] = a;
        b->block_pc[lane] = arm920t_get_pc(b->observe_cpu);
        b->block_base[lane] = arm920t_get_reg(b->observe_cpu, 4u);
        b->block_value[lane] = value;
    }
    if (b->block_count == b->block_at) {
        if (b->block_effect == 1u) arm920t_set_irq(b->observe_cpu, 1);
        else if (b->block_effect == 4u) arm920t_stop_run(b->observe_cpu);
        else {
            gp32_st32le(b->bios + CODE_ADDR + 4u, 0xe3a06077u);
            if (b->block_effect == 2u) arm920t_flush_jit(b->observe_cpu);
            else arm920t_set_jit(b->observe_cpu, 0);
        }
    }
    return value;
}
/* One access observes committed PC but the original base, then changes CPU
 * control state. Stores must capture their source before these changes. */
static uint32_t tb_mem_io(test_bus_t *b, uint32_t a, uint32_t value) {
    arm920t_t *c = b->observe_cpu;
    ++b->mem_calls;
    b->mem_addr = a;
    b->mem_pc = arm920t_get_pc(c);
    b->mem_base = arm920t_get_reg(c, 4u);
    b->mem_value = value;
    arm920t_set_reg(c, 2u, 0xabcdef01u);
    arm920t_set_reg(c, 4u, 0x12345678u);
    switch (b->mem_effect) {
    case 1: arm920t_stop_run(c); break;
    case 2: arm920t_set_reg(c, 15u, CODE_ADDR + 8u); break;
    case 3: arm920t_set_cpsr(c, arm920t_get_cpsr(c) ^ 0x40000000u); break;
    case 4: arm920t_set_cpsr(c, 0x33u); break; /* Thumb SVC */
    case 5:
        gp32_st32le(b->bios + CODE_ADDR + 4u, 0xe3a06077u);
        arm920t_flush_jit(c);
        break;
    case 6:
        gp32_st32le(b->bios + CODE_ADDR + 4u, 0xe3a06077u);
        arm920t_set_jit(c, 0);
        break;
    case 7: arm920t_set_trace(c, 1, tb_trace, b); break;
    case 8: arm920t_set_cpsr(c, 0x1fu); break; /* SYS mode */
    case 9: arm920t_set_fiq(c, 1); break;
    }
    return 0x8877ff80u;
}
static uint8_t tb_read8(void *u, uint32_t a) {
    test_bus_t *b = (test_bus_t *)u;
    uint8_t *p = bus_ptr(b, a, 1u);
    if (!p && b->mem_probe) return (uint8_t)tb_mem_io(b, a, 0u);
    return p ? p[0] : (uint8_t)tb_io_value(b, a);
}
static uint16_t tb_read16(void *u, uint32_t a) {
    test_bus_t *b = (test_bus_t *)u;
    uint8_t *p = bus_ptr(b, a, 2u);
    if (!p && b->mem_probe) return (uint16_t)tb_mem_io(b, a, 0u);
    return p ? gp32_ld16le(p) : (uint16_t)tb_io_value(b, a);
}
static uint32_t tb_read32(void *u, uint32_t a) {
    test_bus_t *b = (test_bus_t *)u;
    uint8_t *p = bus_ptr(b, a, 4u);
    if (!p && b->bus.open_bus_valid && a >= BIOS_SIZE &&
        a - RAM_BASE >= RAM_SIZE && a - IO_ADDR >= 0x02000000u)
        return b->bus.open_bus_word32;
    if (!p && b->mem_probe) return tb_mem_io(b, a, 0u);
    if (b->block_io && a >= IO_ADDR && a < IO_ADDR + 12u)
        return tb_block_io(b, a, 0x11110000u + (a - IO_ADDR) / 4u);
    return p ? gp32_ld32le(p) : tb_io_value(b, a);
}
static void tb_write8(void *u, uint32_t a, uint8_t v) {
    test_bus_t *b = (test_bus_t *)u;
    uint8_t *p = bus_ptr(b, a, 1u);
    if (p) p[0] = v;
    else if (b->mem_probe) (void)tb_mem_io(b, a, v);
    else (void)tb_io_value(b, a);
}
static void tb_write16(void *u, uint32_t a, uint16_t v) {
    test_bus_t *b = (test_bus_t *)u;
    uint8_t *p = bus_ptr(b, a, 2u);
    if (p) gp32_st16le(p, v);
    else if (b->mem_probe) (void)tb_mem_io(b, a, v);
    else (void)tb_io_value(b, a);
}
static void tb_write32(void *u, uint32_t a, uint32_t v) {
    test_bus_t *b = (test_bus_t *)u;
    uint8_t *p = bus_ptr(b, a, 4u);
    if (p) gp32_st32le(p, v);
    else if (b->live_pa && a == b->live_pa) {
        b->live_value = v;
        ++b->live_writes;
        b->live_rebind_rejected += !arm920t_set_live_read32(b->observe_cpu, NULL, 0);
    }
    else if (b->mem_probe) (void)tb_mem_io(b, a, v);
    else if (b->block_io && a >= IO_ADDR && a < IO_ADDR + 12u)
        (void)tb_block_io(b, a, v);
    else if (b->observe_cpu && a == IO_ADDR + (b->block_io ? 0x40u : 4u)) {
        /* Guest IRQ-acknowledge write from inside the vector handler.  Kept
         * out of io_count so the loop's read count stays deterministic. */
        arm920t_set_irq(b->observe_cpu, 0);
        ++b->io_acks;
    }
    else (void)tb_io_value(b, a);
}
static uint8_t *tb_fastmem(void *u, uint32_t a, size_t bytes, int write) {
    (void)write;
    return bus_ptr((test_bus_t *)u, a, bytes);   /* must satisfy the RAM+BIOS probes */
}
static int portable_callbacks;

static void setup_pair(void) {
    for (test_bus_t *b = &bus_jit; b; b = (b == &bus_jit) ? &bus_ref : NULL) {
        memset(b, 0, sizeof(*b));
        for (uint32_t a = 0; a < BIOS_SIZE; a += 4u)
            gp32_st32le(b->bios + a, 0xEAFFFFFEu);   /* B . bounds stray control flow */
        b->bus.read8 = tb_read8;
        b->bus.read16 = tb_read16;
        b->bus.read32 = tb_read32;
        b->bus.write8 = tb_write8;
        b->bus.write16 = tb_write16;
        b->bus.write32 = tb_write32;
        b->bus.fastmem = tb_fastmem;
        b->bus.user = b;
        b->bus.is_stable_read32 = NULL;   /* isolate native execution: no fast-forward */
    }
    cpu_ref = arm920t_create(&bus_ref.bus);
    cpu_jit = arm920t_create(&bus_jit.bus);
    if (!cpu_jit || !cpu_ref) { fprintf(stderr, "arm920t_create failed\n"); exit(2); }
    arm920t_reset(cpu_ref, CODE_ADDR);
    arm920t_reset(cpu_jit, CODE_ADDR);
    arm920t_set_jit(cpu_ref, 0);
    arm920t_set_jit(cpu_jit, !portable_callbacks);
}
static void teardown_pair(void) {
    jit_events += arm920t_get_jit_hits(cpu_jit) + arm920t_get_jit_misses(cpu_jit);
    jit_fallbacks += arm920t_get_jit_fallbacks(cpu_jit);
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
/* Ragged budgets: small values land mid-block and below native block lengths,
 * large ones let translated/native blocks run and the tail park on B self. */
static const uint32_t CHUNKS[] = {1u, 1u, 2u, 3u, 5u, 8u, 13u, 21u, 34u, 55u,
                                  89u, 144u, 1u, 233u, 377u, 610u, 987u, 7u};
static void run_chunks(void) {
    for (size_t i = 0; i < GP32_ARRAY_COUNT(CHUNKS); ++i) {
        uint32_t dj = arm920t_run(cpu_jit, CHUNKS[i]);
        uint32_t dr = arm920t_run(cpu_ref, CHUNKS[i]);
        if (dj != dr) report("arm920t_run budget", dj, dr);
        compare_state();
    }
}
static void load_both(const uint32_t *w, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        gp32_st32le(bus_ptr(&bus_jit, CODE_ADDR + (uint32_t)i * 4u, 4u), w[i]);
        gp32_st32le(bus_ptr(&bus_ref, CODE_ADDR + (uint32_t)i * 4u, 4u), w[i]);
    }
}
static void set_reg_both(unsigned reg, uint32_t v) {
    arm920t_set_reg(cpu_jit, reg, v);
    arm920t_set_reg(cpu_ref, reg, v);
}
static void set_mem_both(uint32_t addr, uint32_t v) {
    gp32_st32le(bus_ptr(&bus_jit, addr, 4u), v);
    gp32_st32le(bus_ptr(&bus_ref, addr, 4u), v);
}
static uint32_t ref_reg(unsigned i) { return arm920t_get_reg(cpu_ref, i); }

/* Read-only cold paths must retain ARM rotations, writeback and the I/O
 * fallback. A nonuniform open-bus value exposes missing word rotation. */
static void case_native_read_windows(void) {
    const uint32_t addresses[] = {0x200u, BIOS_SIZE - 4u, 0xfffffeecu,
                                  RAM_BASE + RAM_SIZE, IO_ADDR};
    const uint32_t program[] = {
        0xee02af10u, /* MCR p15,0,r10,c2,c0,0: translation table */
        0xee01bf10u, /* MCR p15,0,r11,c1,c0,0: MMU control */
        0xe5902000u, /* LDR r2,[r0] */
        0xe5903001u, /* LDR r3,[r0,#1] */
        0xe5904003u, /* LDR r4,[r0,#3] */
        0xe4905004u, /* LDR r5,[r0],#4 */
        0xe2400004u, /* SUB r0,r0,#4 */
        0xeafffff9u, /* B first LDR */
    };
    for (unsigned mmu = 0; mmu < 2u; ++mmu) {
        for (unsigned k = 0; k < GP32_ARRAY_COUNT(addresses); ++k) {
            current_case = "native-read-windows";
            setup_pair();
            arm920t_destroy(cpu_jit);
            arm920t_destroy(cpu_ref);
            bus_jit.bus.open_bus_valid = bus_ref.bus.open_bus_valid = 1;
            bus_jit.bus.open_bus_word32 = bus_ref.bus.open_bus_word32 = 0x12345678u;
            cpu_jit = arm920t_create(&bus_jit.bus);
            cpu_ref = arm920t_create(&bus_ref.bus);
            if (!cpu_jit || !cpu_ref) exit(2);
            arm920t_reset(cpu_jit, CODE_ADDR);
            arm920t_reset(cpu_ref, CODE_ADDR);
            arm920t_set_jit(cpu_jit, 1);
            arm920t_set_jit(cpu_ref, 0);
            load_both(program, GP32_ARRAY_COUNT(program));
            set_reg_both(0u, addresses[k]);
            set_reg_both(10u, RAM_BASE + 0x8000u);
            set_reg_both(11u, mmu);
            set_mem_both(RAM_BASE + 0x8000u, 2u); /* identity-map code */
            set_mem_both(RAM_BASE + 0x8000u + (addresses[k] >> 20) * 4u,
                         (addresses[k] & 0xfff00000u) | 2u);
            if (addresses[k] < BIOS_SIZE) set_mem_both(addresses[k], 0x12345678u);
            run_chunks();
            CHECK(ref_reg(0u) == addresses[k], "read-window writeback restored");
            CHECK(ref_reg(2u) == (addresses[k] == IO_ADDR ? UINT32_MAX : 0x12345678u),
                  "read-window source value");
            CHECK(ref_reg(3u) == (addresses[k] == IO_ADDR ? UINT32_MAX : 0x78123456u),
                  "read-window unaligned rotation");
            teardown_pair();
        }
    }
}

static int tb_poll_stable(void *u, uint32_t a) {
    (void)u;
    return a == DATA_ADDR || a == DATA_ADDR + 4u;
}

/* Stable loads do not make a changing counter a poll. Use different registers
 * from workload code, and inspect native coverage before reaching any parking
 * branch. The traced reference also avoids decoded-block/poll optimizations. */
static void case_poll_progress(void) {
    static const struct {
        const char *name;
        uint32_t step, initial, expected;
    } cases[] = {
        {"poll-countdown", 0xe2477001u, 96u, 32u},
        {"poll-add-wrap", 0xe2877001u, UINT32_MAX, 63u},
        {"poll-sub-wrap", 0xe2477001u, 0u, UINT32_MAX - 63u},
        {"poll-adds", 0xe2977001u, UINT32_MAX, 63u},
        {"poll-subs", 0xe2577001u, 0u, UINT32_MAX - 63u},
        {"poll-rotated-step", 0xe2877102u, 9u, 9u},
        {"poll-load-alias", 0xe2477001u, 96u, 32u},
        {"poll-conditional-backedge", 0xe2577001u, 96u, 32u}
    };
    for (unsigned k = 0; k < GP32_ARRAY_COUNT(cases); ++k) {
        current_case = cases[k].name;
        setup_pair();
        /* The bus is copied at create time. Recreate only the native CPU. */
        arm920t_destroy(cpu_jit);
        bus_jit.bus.is_stable_read32 = tb_poll_stable;
        cpu_jit = arm920t_create(&bus_jit.bus);
        if (!cpu_jit) exit(2);
        arm920t_reset(cpu_jit, CODE_ADDR);
        arm920t_set_jit(cpu_jit, 1);
        arm920t_set_trace(cpu_ref, 1, NULL, NULL);
        uint32_t program[] = {
            k == 0u ? 0xe3570000u : 0xe3560000u, /* CMP counter/exit latch,#0 */
            k == 0u ? 0xda000005u : 0x1a000005u, /* BLE/BNE outside loop */
            k == 6u ? 0xe5900000u : 0xe5905000u, /* LDR, including Rd==Rn */
            0xe0033005u, 0xe5915000u, 0xe0044005u,
            cases[k].step, k == 7u ? 0x1afffff7u : 0xeafffff7u, 0xeafffffeu
        };
        load_both(program, GP32_ARRAY_COUNT(program));
        set_reg_both(0u, DATA_ADDR);
        set_reg_both(1u, DATA_ADDR + 4u);
        set_reg_both(3u, UINT32_MAX);
        set_reg_both(4u, UINT32_MAX);
        set_reg_both(7u, cases[k].initial);
        set_mem_both(DATA_ADDR, k == 6u ? DATA_ADDR : 0x55aa55aau);
        set_mem_both(DATA_ADDR + 4u, 0xaa55aa55u);
        CHECK(arm920t_run(cpu_jit, 512u) == arm920t_run(cpu_ref, 512u), "counter coverage budget");
        compare_state();
        CHECK(ref_reg(7u) == cases[k].expected, "counter must change modulo 2^32");
        CHECK(arm920t_get_pc(cpu_jit) == CODE_ADDR, "coverage ends on loop entry");
        gp32_cpu_profile_t p;
        arm920t_get_cpu_profile(cpu_jit, &p);
        if (p.supported) {
            if (k == 0u) {
                CHECK(p.poll_skipped_insns > 0u && p.native_arm_insns == 0u,
                      "proven stable countdown must use portable proof and bounded skips");
                CHECK(p.block_interp_arm_insns + p.poll_skipped_insns == 512u,
                      "countdown must account every executed or skipped instruction");
            } else CHECK(p.poll_skipped_insns == 0u, "unsupported progress must never fast-forward");
            if (p.native_backend && k != 0u) {
                /* A conditional backedge includes its exit in the translated
                 * block. Its final short budget can legitimately use portable
                 * execution; bound that tail by the complete trace length. */
                if (k == 7u)
                    CHECK(p.native_arm_insns > 0u &&
                          p.block_interp_arm_insns < GP32_ARRAY_COUNT(program) &&
                          p.native_arm_insns + p.block_interp_arm_insns == 512u,
                          "conditional counter loop needs native coverage with only a short tail");
                else CHECK(p.native_arm_insns == 512u, "counter loop must execute entirely native");
            }
        }
        printf("%s native=%" PRIu64 " portable=%" PRIu64 " skipped=%" PRIu64 "\n",
               current_case, p.native_arm_insns, p.block_interp_arm_insns, p.poll_skipped_insns);
        run_chunks(); /* flags, partial blocks, and countdown exit */
        /* Take the early exit with a large native-eligible budget as well. */
        set_reg_both(15u, CODE_ADDR);
        if (k == 0u) set_reg_both(7u, 0x80000000u);
        else set_reg_both(6u, 1u);
        CHECK(arm920t_run(cpu_jit, 16u) == arm920t_run(cpu_ref, 16u), "early-exit budget");
        compare_state();
        CHECK(arm920t_get_pc(cpu_jit) == CODE_ADDR + 32u, "early exit reaches parking branch");
        teardown_pair();
    }
}

/* ------------------------------------------------------------------ cases */

/* ADDS/ADC/SBCS/RSBS/TST plus conditional MVNNE / ADDEQ. */
static const uint32_t P_FLAGS[] = {
    0xE3E00000u, /* MVN   r0, #0             ; 0xFFFFFFFF                  */
    0xE3A02102u, /* MOV   r2, #0x80000000                                  */
    0xE3A03001u, /* MOV   r3, #1                                           */
    0xE0901003u, /* ADDS  r1, r0, r3         ; 0xFFFFFFFF+1 -> 0, C=1,Z=1   */
    0xE0A04003u, /* ADC   r4, r0, r3         ; +carry -> 1, C=1             */
    0xE0D05003u, /* SBCS  r5, r0, r3         ; carry set -> 0xFFFFFFFE     */
    0xE2736000u, /* RSBS  r6, r3, #0         ; 0-1 -> 0xFFFFFFFF, C=0      */
    0xE1160006u, /* TST   r6, r6             ; N=1,Z=0,C=0 (shifter carry) */
    0x11E07006u, /* MVNNE r7, r6             ; Z=0 so runs: r7 = 0         */
    0x02868001u, /* ADDEQ r8, r6, #1         ; Z=0 so skipped              */
    0xEAFFFFFEu, /* B .                                                    */
};
static void case_flags(void) {
    current_case = "flags";
    setup_pair();
    load_both(P_FLAGS, GP32_ARRAY_COUNT(P_FLAGS));
    run_chunks();
    CHECK(ref_reg(1) == 0x00000000u, "ADDS result");
    CHECK(ref_reg(4) == 0x00000001u, "ADC carry-in result");
    CHECK(ref_reg(5) == 0xFFFFFFFEu, "SBCS result");
    CHECK(ref_reg(6) == 0xFFFFFFFFu, "RSBS result");
    CHECK(ref_reg(7) == 0x00000000u, "MVNNE was not executed");
    CHECK(ref_reg(8) == 0x00000000u, "ADDEQ was not skipped");
    CHECK((arm920t_get_cpsr(cpu_ref) >> 31) == 1u, "TST did not set N");
    teardown_pair();
}

/* Immediate and register-specified LSL/LSR/ASR/ROR/RRX, incl. shifts by 32. */
static const uint32_t P_SHIFT[] = {
    0xE3A00081u, /* MOV   r0, #0x81                                        */
    0xE1B01100u, /* MOVS  r1, r0, LSL #2     ; 0x204                       */
    0xE1B020A0u, /* MOVS  r2, r0, LSR #1     ; 0x40, C=1 (bit0 of 0x81)    */
    0xE1B03142u, /* MOVS  r3, r2, ASR #2     ; 0x10                       */
    0xE1B04263u, /* MOVS  r4, r3, ROR #4     ; 1                          */
    0xE1B05064u, /* MOVS  r5, r4, RRX        ; 0, C=1                     */
    0xE3A06020u, /* MOV   r6, #0x20                                        */
    0xE1A07610u, /* MOV   r7, r0, LSL r6     ; shift by 32 -> 0            */
    0xE1A08630u, /* MOV   r8, r0, LSR r6     ; shift by 32 -> 0            */
    0xE1A09650u, /* MOV   r9, r0, ASR r6     ; shift by 32 -> 0            */
    0xE1A0A670u, /* MOV   r10, r0, ROR r6    ; rot by 32 mod 32 -> 0x81    */
    0xEAFFFFFEu, /* B .                                                    */
};
static void case_shift(void) {
    current_case = "shift";
    setup_pair();
    load_both(P_SHIFT, GP32_ARRAY_COUNT(P_SHIFT));
    run_chunks();
    CHECK(ref_reg(1) == 0x00000204u, "LSL #2 result");
    CHECK(ref_reg(2) == 0x00000040u, "LSR #1 result");
    CHECK(ref_reg(3) == 0x00000010u, "ASR #2 result");
    CHECK(ref_reg(4) == 0x00000001u, "ROR #4 result");
    CHECK(ref_reg(5) == 0x00000000u, "RRX result");
    CHECK(ref_reg(7) == 0x00000000u, "LSL by 32 result");
    CHECK(ref_reg(10) == 0x00000081u, "ROR by 32 result");
    teardown_pair();
}

/* Forward B, BL into a leaf returning via BX lr, a SUBS/BNE loop, a taken BEQ. */
static const uint32_t P_BRANCH[] = {
    0xE3A00000u, /* MOV r0, #0                                             */
    0xEA000001u, /* B   +1 (word 4)                                        */
    0xE3A000EEu, /* MOV r0, #0xEE     ; skipped                            */
    0xE3A03011u, /* MOV r3, #0x11     ; skipped                            */
    0xEB000007u, /* BL  +7 (helper at word 13)                             */
    0xE3A01003u, /* MOV r1, #3                                             */
    0xE2511001u, /* SUBS r1, r1, #1   ; loop                               */
    0x1AFFFFFDu, /* BNE  -3 (word 6)                                       */
    0xE350002Au, /* CMP r0, #0x2A                                          */
    0x0A000001u, /* BEQ  +1 (word 12) ; taken when r0 == 0x2A              */
    0xE3A02055u, /* MOV r2, #0x55     ; skipped                            */
    0xE3A04077u, /* MOV r4, #0x77     ; skipped                            */
    0xEAFFFFFEu, /* B .                                                    */
    0xE280002Au, /* ADD r0, r0, #0x2A ; helper body                        */
    0xE12FFF1Eu, /* BX  lr                                                 */
};
static void case_branch(void) {
    current_case = "branch";
    setup_pair();
    load_both(P_BRANCH, GP32_ARRAY_COUNT(P_BRANCH));
    run_chunks();
    CHECK(ref_reg(0) == 0x0000002Au, "BL helper did not run");
    CHECK(ref_reg(1) == 0x00000000u, "SUBS loop did not finish");
    CHECK(ref_reg(2) == 0x00000000u, "taken BEQ did not skip the block");
    CHECK(ref_reg(3) == 0x00000000u, "forward B did not skip the block");
    teardown_pair();
}

/* A long stitched branch chain crosses native trace capacity. Each branch
 * skips an ADD trap, so a wrong fallthrough at a trace boundary is observable
 * even if execution eventually reaches the same final loop. */
static void case_branch_chain(void) {
    uint32_t program[258];
    for (unsigned i = 0; i < 128u; ++i) {
        program[2u * i] = 0xea000000u;      /* B pc+8 */
        program[2u * i + 1u] = 0xe2811001u; /* ADD r1,r1,#1: must be skipped */
    }
    program[256] = 0xe3a00077u;
    program[257] = 0xeafffffeu;
    current_case = "stitched-branch-chain";
    setup_pair(); load_both(program, GP32_ARRAY_COUNT(program));
    CHECK(arm920t_run(cpu_jit, 256u) == arm920t_run(cpu_ref, 256u),
          "branch chain budget");
    compare_state();
    CHECK(ref_reg(0) == 0x77u && ref_reg(1) == 0u, "branch chain oracle");
    teardown_pair();
}

/* Forward stitching requires a BL before the entry backedge. Start at the
 * actual loop entry so native-sized budgets exercise that trace, and compare
 * against single-step execution. Cold MOVEQ instructions observe guest flags. */
static void case_forward_loop(void) {
    const uint32_t cond_program[] = {
        0xe3520000u, /* entry: CMP r2,#0 */
        0x1a000001u, /* BNE body */
        0x03a06077u, /* cold: MOVEQ r6,#0x77 */
        0xea000004u, /* B park */
        0xeb00003au, /* body: BL leaf at +0x100 */
        0xe0800001u, /* ADD r0,r0,r1 */
        0xe2511001u, /* SUBS r1,r1,#1 */
        0x1afffff7u, /* BNE entry */
        0x03a06099u, /* MOVEQ r6,#0x99 */
        0xeafffffeu, /* park */
    };
    const uint32_t ldr_program[] = {
        0xe4943004u, /* entry: LDR r3,[r4],#4 */
        0xe3530000u, /* CMP r3,#0 */
        0x1a000001u, /* BNE body */
        0x03a06077u, /* cold: MOVEQ r6,#0x77 */
        0xea000003u, /* B park */
        0xeb000039u, /* body: BL leaf at +0x100 */
        0xe0800003u, /* ADD r0,r0,r3 */
        0xe2511001u, /* SUBS r1,r1,#1 */
        0xeafffff6u, /* B entry */
        0xeafffffeu, /* park */
    };
    for (unsigned scenario = 0; scenario < 3u; ++scenario) {
        current_case = scenario == 0u ? "forward-loop-taken" :
                       scenario == 1u ? "forward-loop-untaken" : "forward-loop-ldr";
        setup_pair();
        arm920t_set_trace(cpu_ref, 1, NULL, NULL);
        load_both(scenario == 2u ? ldr_program : cond_program,
                  GP32_ARRAY_COUNT(cond_program));
        set_mem_both(CODE_ADDR + 0x100u, 0xe92d4000u); /* PUSH lr */
        set_mem_both(CODE_ADDR + 0x104u, 0xe2877001u); /* ADD r7,r7,#1 */
        set_mem_both(CODE_ADDR + 0x108u, 0xe8bd8000u); /* POP pc */
        set_reg_both(0u, 0u);
        set_reg_both(1u, scenario == 2u ? 3u : 6u);
        set_reg_both(2u, scenario == 1u ? 0u : 1u);
        set_reg_both(6u, 0u);
        set_reg_both(7u, 0u);
        set_reg_both(13u, DATA_ADDR + 0x100u);
        if (scenario == 2u) {
            set_reg_both(4u, DATA_ADDR + 0x40u);
            set_mem_both(DATA_ADDR + 0x40u, 5u);
            set_mem_both(DATA_ADDR + 0x44u, 7u);
            set_mem_both(DATA_ADDR + 0x48u, 0u);
            set_mem_both(DATA_ADDR + 0x4cu, 9u);
        }
        /* At least one complete native trace fits. A small remainder splits
         * the next repetition immediately after its forward taken branch. */
        uint32_t budget = scenario == 2u ? 13u : 11u;
        CHECK(arm920t_run(cpu_jit, budget) == arm920t_run(cpu_ref, budget),
              "forward-loop native and partial budget");
        compare_state();
        CHECK(ref_reg(15u) == CODE_ADDR +
              (scenario == 0u ? 0x10u : scenario == 1u ? 0x24u : 0x14u),
              "forward-loop branch/budget successor");
        run_chunks();
        CHECK(ref_reg(0u) == (scenario == 0u ? 21u : scenario == 1u ? 0u : 12u),
              "forward-loop accumulator");
        CHECK(ref_reg(1u) == (scenario == 0u ? 0u : scenario == 1u ? 6u : 1u),
              "forward-loop iteration count");
        CHECK(ref_reg(7u) == (scenario == 0u ? 6u : scenario == 1u ? 0u : 2u),
              "forward-loop real leaf calls");
        CHECK(ref_reg(6u) == (scenario == 0u ? 0x99u : 0x77u) &&
              (arm920t_get_cpsr(cpu_ref) & 0xf0000000u) == 0x60000000u,
              "forward-loop guest flags survive host guards");
        CHECK(ref_reg(13u) == DATA_ADDR + 0x100u &&
              ref_reg(15u) == CODE_ADDR + 0x24u, "forward-loop stack and exit PC");
        if (scenario == 2u)
            CHECK(ref_reg(4u) == DATA_ADDR + 0x4cu, "sentinel writeback exactly once");
        teardown_pair();
    }
}

/* BX is a common ARM function return.  Check the native even-target path,
 * condition skip, Thumb interworking fallback, and the BX PC pipeline case. */
static void case_bx(void) {
    static const uint32_t conditional[] = {
        0xE3510000u, /* CMP r1, #0: Z=1 */
        0x112FFF10u, /* BXNE r0: skipped */
        0x012FFF10u, /* BXEQ r0: taken */
        0xE3A02055u, /* MOV r2, #0x55: skipped */
        0xEAFFFFFEu, /* B . */
    };
    current_case = "bx-arm-conditional";
    setup_pair();
    load_both(conditional, GP32_ARRAY_COUNT(conditional));
    set_reg_both(0, CODE_ADDR + 16u);
    set_reg_both(1, 0u);
    run_chunks();
    CHECK(ref_reg(2) == 0u, "conditional BX did not skip MOV");
    teardown_pair();

    static const uint32_t interwork[] = {
        0xE12FFF10u, /* BX r0: enter Thumb */
        0xEAFFFFFEu, /* B . if BX fails */
        0xE7FEE7FEu, /* Thumb B . in both halfwords */
    };
    current_case = "bx-thumb";
    setup_pair();
    load_both(interwork, GP32_ARRAY_COUNT(interwork));
    set_reg_both(0, (CODE_ADDR + 8u) | 1u);
    run_chunks();
    CHECK((arm920t_get_cpsr(cpu_ref) & 0x20u) != 0u, "BX did not enter Thumb state");
    teardown_pair();

    static const uint32_t pipeline[] = {
        0xE12FFF1Fu, /* BX PC reads the interpreter's pipeline PC */
        0xEAFFFFFEu, /* B . */
    };
    current_case = "bx-pc";
    setup_pair();
    load_both(pipeline, GP32_ARRAY_COUNT(pipeline));
    run_chunks();
    teardown_pair();
}

/* Word/byte/halfword/signed loads and stores, plus STMIA/LDMIA writeback. */
static const uint32_t P_MEM[] = {
    0xE3A0140Cu, /* MOV  r1, #0x0C000000                                   */
    0xE2811A01u, /* ADD  r1, r1, #0x1000  ; r1 = DATA                      */
    0xE3A00011u, /* MOV  r0, #0x11                                         */
    0xE5810000u, /* STR  r0, [r1]                                          */
    0xE5912000u, /* LDR  r2, [r1]                                          */
    0xE5C10004u, /* STRB r0, [r1, #4]                                      */
    0xE5D13004u, /* LDRB r3, [r1, #4]                                      */
    0xE3A04034u, /* MOV  r4, #0x34                                         */
    0xE1C140B8u, /* STRH r4, [r1, #8]                                      */
    0xE1D150B8u, /* LDRH r5, [r1, #8]                                      */
    0xE1D160D9u, /* LDRSB r6, [r1, #9]    ; high byte of 0x0034 -> 0      */
    0xE1D170F8u, /* LDRSH r7, [r1, #8]                                     */
    0xE3A080AAu, /* MOV  r8, #0xAA                                         */
    0xE8A10101u, /* STMIA r1!, {r0, r8}                                    */
    0xE2411008u, /* SUB  r1, r1, #8                                        */
    0xE8B10600u, /* LDMIA r1!, {r9, r10}                                   */
    0xEAFFFFFEu, /* B .                                                    */
};
static void case_mem(void) {
    current_case = "mem";
    setup_pair();
    load_both(P_MEM, GP32_ARRAY_COUNT(P_MEM));
    run_chunks();
    CHECK(ref_reg(2) == 0x00000011u, "STR/LDR round trip");
    CHECK(ref_reg(3) == 0x00000011u, "STRB/LDRB round trip");
    CHECK(ref_reg(5) == 0x00000034u, "STRH/LDRH round trip");
    CHECK(ref_reg(6) == 0x00000000u, "LDRSB high byte");
    CHECK(ref_reg(7) == 0x00000034u, "LDRSH value");
    CHECK(ref_reg(10) == 0x000000AAu, "LDMIA reload");
    CHECK(ref_reg(1) == DATA_ADDR + 8u, "STMIA/LDMIA writeback");
    CHECK(gp32_ld32le(bus_ptr(&bus_ref, DATA_ADDR + 4u, 4u)) == 0x000000AAu, "STMIA store to RAM");
    teardown_pair();
}

/* Halfword addressing modes that the A64 RAM path may inline or fall back. */
static uint32_t half_insn(unsigned p, unsigned u, unsigned w, unsigned l,
                          unsigned imm, unsigned rn, unsigned rd,
                          unsigned kind, unsigned off) {
    return 0xE0000090u | (p << 24) | (u << 23) | (imm << 22) |
           (w << 21) | (l << 20) | (rn << 16) | (rd << 12) |
           ((imm ? (off & 0xf0u) : 0u) << 4) | (kind << 5) | (off & 0xfu);
}
static void case_half_modes(void) {
    current_case = "half-modes";
    setup_pair();
    const uint32_t program[] = {
        half_insn(1, 1, 0, 0, 0, 0, 3, 1, 2), /* STRH  r3, [r0, r2]  */
        half_insn(1, 1, 0, 1, 0, 0, 5, 1, 2), /* LDRH  r5, [r0, r2]  */
        half_insn(1, 1, 0, 1, 1, 0, 6, 2, 2), /* LDRSB r6, [r0, #2]  */
        half_insn(1, 1, 0, 1, 1, 0, 7, 3, 2), /* LDRSH r7, [r0, #2]  */
        half_insn(0, 1, 0, 1, 1, 0, 8, 1, 2), /* LDRH  r8, [r0], #2  */
        half_insn(1, 0, 1, 0, 1, 0, 3, 1, 2), /* STRH  r3, [r0, #-2]! */
        half_insn(1, 1, 0, 1, 1, 0, 9, 1, 3), /* LDRH  r9, [r0, #3]  */
        half_insn(1, 1, 0, 1, 1, 4, 10, 1, 0),/* LDRH  r10, [r4] IO */
        half_insn(1, 1, 0, 1, 0, 0, 11, 3, 2),/* LDRSH r11, [r0,r2] */
        half_insn(1, 1, 0, 1, 0, 14, 12, 1, 15),/* LDRH r12,[r14,r15]: raw PC+4 */
        half_insn(1, 1, 1, 1, 1, 0, 0, 1, 2), /* LDRH  r0, [r0,#2]! alias */
        0xEAFFFFFEu,
    };
    load_both(program, GP32_ARRAY_COUNT(program));
    set_reg_both(0u, DATA_ADDR);
    set_reg_both(2u, 2u);
    set_reg_both(3u, 0xFFFF8080u);
    set_reg_both(4u, 0x15000000u);
    set_reg_both(14u, DATA_ADDR - (CODE_ADDR + 9u * 4u + 4u));
    run_chunks();
    CHECK(ref_reg(0) == DATA_ADDR + 2u, "halfword alias/writeback result");
    CHECK(ref_reg(5) == 0x8080u && ref_reg(6) == 0xFFFFFF80u &&
          ref_reg(7) == 0xFFFF8080u, "halfword signed and unsigned loads");
    CHECK(ref_reg(9) == 0x80u && ref_reg(10) == 0xFFFFu &&
          ref_reg(11) == 0xFFFF8080u && ref_reg(12) == 0x8080u,
          "unaligned/IO/register/PC-offset halfword loads");
    teardown_pair();
}

static uint32_t block_insn(unsigned p, unsigned u, unsigned s, unsigned w,
                           unsigned l, unsigned rn, unsigned regs) {
    return 0xE8000000u | (p << 24) | (u << 23) | (s << 22) |
           (w << 21) | (l << 20) | (rn << 16) | regs;
}

/* LDM with PC in the list.  Even (bit0 clear) ARM targets stay on the native
 * in-RAM path; odd PC targets and every span-guard failure
 * bail to the classified helper before any guest register changes, so the
 * oracle and JIT must stay in lockstep either way.  SP-based pops cover the
 * common function-return shape; the BL-inlined leaf wrapper must keep its
 * helper semantics and land exactly on the BL fallthrough. */
static void case_native_ldm_pc(void) {
    /* A one-instruction budget proves the return itself is native, rather
     * than counting native execution in a later target loop. */
    for (unsigned mmu = 0; mmu < 2u; ++mmu) {
        for (unsigned variant = 0; variant < 5u; ++variant) {
            current_case = "native-ldm-pc";
            setup_pair();
            const uint32_t ttb = RAM_BASE + 0x4000u;
            const uint32_t base = mmu ? 0x10001000u : DATA_ADDR;
            const uint32_t target = CODE_ADDR + 0x80u;
            const uint32_t low_bits = variant == 1u ? 2u : variant == 2u ? 1u : 0u;
            const uint32_t program[] = {
                0xee02af10u, /* MCR p15,0,r10,c2,c0,0 */
                0xee01bf10u, /* MCR p15,0,r11,c1,c0,0 */
                0xe591c000u, /* LDR r12,[r1]: prime the data mapping */
                block_insn(0, 1, 0, 1, 1, 1, variant == 4u ? 0x8022u : 0x8030u) &
                    (variant == 3u ? 0x0fffffffu : UINT32_MAX), /* EQ fails with Z=0 */
                0xe3a090eeu, /* must not execute in this budget */
                0xe1a0f00eu
            };
            load_both(program, GP32_ARRAY_COUNT(program));
            arm920t_set_cpsr(cpu_jit, 0x200000d3u);
            arm920t_set_cpsr(cpu_ref, 0x200000d3u);
            set_reg_both(1u, base);
            set_reg_both(10u, ttb);
            set_reg_both(11u, mmu);
            set_mem_both(ttb, 2u); /* identity BIOS section */
            set_mem_both(ttb + 0x400u, RAM_BASE | 2u);
            set_mem_both(DATA_ADDR, 0x11223344u);
            set_mem_both(DATA_ADDR + 4u, 0x55667788u);
            set_mem_both(DATA_ADDR + 8u, target | low_bits);
            CHECK(arm920t_run(cpu_jit, 3u) == arm920t_run(cpu_ref, 3u), "LDM setup budget");
            compare_state();
            arm920t_reset_cpu_profile(cpu_jit);
            CHECK(arm920t_run(cpu_jit, 1u) == arm920t_run(cpu_ref, 1u), "LDM return budget");
            compare_state();
            CHECK(ref_reg(9u) == 0u, "return cannot execute fallthrough");
            CHECK(arm920t_get_cpsr(cpu_ref) == 0x200000d3u, "LDM preserves ARM state and flags");
            if (variant == 3u) {
                CHECK(ref_reg(1u) == base && ref_reg(4u) == 0u && ref_reg(5u) == 0u &&
                      arm920t_get_pc(cpu_ref) == CODE_ADDR + 16u, "failed condition has no transfer");
            } else if (variant == 4u) {
                CHECK(ref_reg(1u) == 0x11223344u && ref_reg(4u) == 0u &&
                      ref_reg(5u) == 0x55667788u && arm920t_get_pc(cpu_ref) == target,
                      "loaded base register suppresses final writeback");
            } else {
                CHECK(ref_reg(1u) == base + 12u && ref_reg(4u) == 0x11223344u &&
                      ref_reg(5u) == 0x55667788u && arm920t_get_pc(cpu_ref) == target,
                      "LDM loads, final writeback and aligned PC");
            }
            gp32_cpu_profile_t profile;
            arm920t_get_cpu_profile(cpu_jit, &profile);
            if (profile.supported && profile.native_backend) {
                CHECK(profile.native_block_calls == 1u && profile.native_arm_insns == 1u,
                      "return instruction must execute natively");
                CHECK(profile.helper_op_kinds[7u] ==
                          ((profile.native_backend == 2u && variant == 2u) ||
                           (profile.native_backend == 1u && !mmu && variant == 4u) ? 1u : 0u),
                      "only backend-specific odd-target or base-in-list guards need helpers");
            }
            teardown_pair();
        }
    }
}

static void case_ldm_pc(void) {
    /* ldmia r1!, {r4-r6, pc} with an even target: loads, writeback, branch. */
    const uint32_t even[] = {
        0xE3A0140Cu,                            /* MOV  r1, #0x0C000000        */
        0xE2811A01u,                            /* ADD  r1, r1, #0x1000        */
        block_insn(0, 1, 0, 1, 1, 1, 0x8070u),  /* LDMIA r1!, {r4-r6, pc}      */
        0xE3A020EEu,                            /* MOV  r2, #0xEE ; dead       */
        0xEAFFFFFEu,                            /* B . ; dead                  */
        /* +0x14: branch target */
        0xE3A09077u,                            /* MOV  r9, #0x77              */
        0xEAFFFFFEu,                            /* B .                         */
    };
    current_case = "ldm-pc-even";
    setup_pair();
    load_both(even, GP32_ARRAY_COUNT(even));
    set_mem_both(DATA_ADDR + 0u, 0x11111111u);
    set_mem_both(DATA_ADDR + 4u, 0x22222222u);
    set_mem_both(DATA_ADDR + 8u, 0x33333333u);
    set_mem_both(DATA_ADDR + 12u, CODE_ADDR + 0x14u);
    run_chunks();
    CHECK(ref_reg(4) == 0x11111111u && ref_reg(5) == 0x22222222u &&
          ref_reg(6) == 0x33333333u, "LDM {..,pc} register loads");
    CHECK(ref_reg(1) == DATA_ADDR + 16u, "LDMIA pc writeback");
    CHECK(ref_reg(9) == 0x77u && ref_reg(2) == 0u, "LDM pc did not reach target");
    CHECK(arm920t_get_pc(cpu_ref) == CODE_ADDR + 0x18u, "LDM pc did not park on target loop");
    teardown_pair();

    /* pc word with bit1 set (even but halfword-aligned): ARMv4T masks both low bits. */
    const uint32_t misaligned[] = {
        0xE3A0140Cu, 0xE2811A01u,
        block_insn(0, 1, 0, 1, 1, 1, 0x8010u),  /* LDMIA r1!, {r4, pc}         */
        0xE3A020EEu,                            /* MOV  r2, #0xEE ; dead       */
        0xEAFFFFFEu,                            /* B . ; dead                  */
        /* +0x14: target */
        0xE3A08099u,                            /* MOV  r8, #0x99              */
        0xEAFFFFFEu,                            /* B .                         */
    };
    current_case = "ldm-pc-even-masked";
    setup_pair();
    load_both(misaligned, GP32_ARRAY_COUNT(misaligned));
    set_mem_both(DATA_ADDR + 0u, 0x44444444u);
    set_mem_both(DATA_ADDR + 4u, (CODE_ADDR + 0x14u) | 2u);
    run_chunks();
    CHECK(ref_reg(4) == 0x44444444u && ref_reg(8) == 0x99u && ref_reg(2) == 0u,
          "LDM pc did not mask bit1");
    teardown_pair();

    /* ldmia sp!, {pc} alone (the hot leaf shape, standalone): even target. */
    const uint32_t pop_pc[] = {
        block_insn(0, 1, 0, 1, 1, 13, 0x8000u), /* LDMIA r13!, {pc}            */
        0xEAFFFFFEu,                            /* B . ; dead                  */
        /* +0x08: target */
        0xE3A07066u,                            /* MOV  r7, #0x66              */
        0xEAFFFFFEu,                            /* B .                         */
    };
    current_case = "ldm-pc-pop";
    setup_pair();
    load_both(pop_pc, GP32_ARRAY_COUNT(pop_pc));
    set_reg_both(13u, DATA_ADDR + 0x20u);
    set_mem_both(DATA_ADDR + 0x20u, CODE_ADDR + 0x08u);
    run_chunks();
    CHECK(ref_reg(7) == 0x66u && ref_reg(13) == DATA_ADDR + 0x24u,
          "pop {pc} did not branch/writeback");
    teardown_pair();

    /* ldmdb r13!, {r4,r5,pc}: decrement-before ordering, PC last slot. */
    const uint32_t db_pc[] = {
        block_insn(1, 0, 0, 1, 1, 13, 0x8030u), /* LDMDB r13!, {r4,r5,pc}      */
        0xEAFFFFFEu,                            /* B . ; dead                  */
        /* +0x08: target */
        0xE3A090ABu,                            /* MOV  r9, #0xAB              */
        0xEAFFFFFEu,                            /* B .                         */
    };
    current_case = "ldm-pc-db";
    setup_pair();
    load_both(db_pc, GP32_ARRAY_COUNT(db_pc));
    set_reg_both(13u, DATA_ADDR + 0x40u);
    set_mem_both(DATA_ADDR + 0x34u, 0x55555555u); /* r4 slot: r13-12 */
    set_mem_both(DATA_ADDR + 0x38u, 0x66666666u); /* r5 slot: r13-8  */
    set_mem_both(DATA_ADDR + 0x3Cu, CODE_ADDR + 0x08u); /* pc slot: r13-4 */
    run_chunks();
    CHECK(ref_reg(4) == 0x55555555u && ref_reg(5) == 0x66666666u &&
          ref_reg(9) == 0xABu && ref_reg(13) == DATA_ADDR + 0x34u,
          "LDMDB pc ordering/writeback");
    teardown_pair();

    /* ARMv4T LDM PC ignores both low bits and retains ARM state, unlike
     * ARMv5 interworking. Keep the loaded data and writeback checks too. */
    const uint32_t odd_pc[] = {
        0xE3A0140Cu, 0xE2811A01u,
        block_insn(0, 1, 0, 1, 1, 1, 0x8010u), /* LDMIA r1!, {r4, pc} */
        0xEAFFFFFEu,
        0xE3A07077u, /* +0x10: ARM MOV r7,#0x77 */
        0xEAFFFFFEu,
    };
    current_case = "ldm-pc-odd-armv4";
    setup_pair();
    load_both(odd_pc, GP32_ARRAY_COUNT(odd_pc));
    set_mem_both(DATA_ADDR + 0u, 0x44444444u);
    set_mem_both(DATA_ADDR + 4u, (CODE_ADDR + 0x10u) | 1u);
    run_chunks();
    CHECK(ref_reg(4) == 0x44444444u, "odd PC load lost the r4 load");
    CHECK(ref_reg(7) == 0x77u, "ARM target code did not run");
    CHECK((arm920t_get_cpsr(cpu_ref) & 0x20u) == 0u, "LDM pc must retain ARM state");
    CHECK(arm920t_get_pc(cpu_ref) == CODE_ADDR + 0x14u, "LDM pc low-bit mask");
    teardown_pair();

    /* Conditional forms: LDMNE must skip loads+writeback+branch entirely,
     * LDMEQ then takes the native path and jumps. */
    const uint32_t cond[] = {
        0xE3500000u,                            /* CMP  r0, #0 -> Z=1          */
        0xE3A0140Cu, 0xE2811A01u,               /* r1 = DATA_ADDR              */
        0x18B18030u,                            /* LDMNE r1!, {r4,r5,pc} skip  */
        0xE3A02055u,                            /* MOV  r2, #0x55              */
        0x08B18040u,                            /* LDMEQ r1!, {r6,pc}  taken   */
        0xE3A030EEu,                            /* MOV  r3, #0xEE ; dead       */
        /* +0x1C: target */
        0xE3A08088u,                            /* MOV  r8, #0x88              */
        0xEAFFFFFEu,                            /* B .                         */
    };
    current_case = "ldm-pc-cond";
    setup_pair();
    load_both(cond, GP32_ARRAY_COUNT(cond));
    set_mem_both(DATA_ADDR + 0u, 0x77777777u);
    set_mem_both(DATA_ADDR + 4u, CODE_ADDR + 0x1Cu);
    run_chunks();
    CHECK(ref_reg(4) == 0u && ref_reg(5) == 0u, "skipped LDMNE still loaded");
    CHECK(ref_reg(2) == 0x55u && ref_reg(3) == 0u, "conditional LDM pc flow");
    CHECK(ref_reg(6) == 0x77777777u && ref_reg(8) == 0x88u, "LDMEQ pc not taken");
    CHECK(ref_reg(1) == DATA_ADDR + 8u, "skipped writeback committed anyway");
    teardown_pair();

    /* Span crossing a 4 KiB boundary inside RAM: same-page guard fails, the
     * helper re-executes both words on unmodified state. */
    const uint32_t page[] = {
        0xE3A0140Cu,                            /* MOV  r1, #0x0C000000        */
        0xE2811A02u,                            /* ADD  r1, r1, #0x2000        */
        0xE2411004u,                            /* SUB  r1, r1, #4 -> +0x1FFC  */
        block_insn(0, 1, 0, 1, 1, 1, 0x8010u),  /* LDMIA r1!, {r4, pc}         */
        0xEAFFFFFEu,                            /* B . ; dead                  */
        /* +0x14: target */
        0xE3A0A0AAu,                            /* MOV  r10, #0xAA             */
        0xEAFFFFFEu,                            /* B .                         */
    };
    current_case = "ldm-pc-page";
    setup_pair();
    load_both(page, GP32_ARRAY_COUNT(page));
    set_mem_both(RAM_BASE + 0x1FFCu, 0xABCD1234u);
    set_mem_both(RAM_BASE + 0x2000u, CODE_ADDR + 0x14u);
    run_chunks();
    CHECK(ref_reg(4) == 0xABCD1234u && ref_reg(10) == 0xAAu,
          "page-crossing LDM pc fallback diverged");
    CHECK(ref_reg(1) == RAM_BASE + 0x2004u, "page-crossing writeback");
    teardown_pair();

    /* Span crossing the 8 MiB RAM end: the PC word reads outside the direct
     * window, so the helper must run; its bus read returns 0xFFFFFFFF, an odd
     * value, which ARMv4T aligns to 0xFFFFFFFC without changing state. */
    const uint32_t mmio[] = {
        0xE3A0140Cu,                            /* MOV  r1, #0x0C000000        */
        0xE2811502u,                            /* ADD  r1, r1, #0x800000      */
        0xE2411004u,                            /* SUB  r1, r1, #4  -> end-4   */
        block_insn(0, 1, 0, 1, 1, 1, 0x8010u),  /* LDMIA r1!, {r4, pc}         */
        0xEAFFFFFEu,                            /* B . ; dead                  */
    };
    current_case = "ldm-pc-mmio";
    setup_pair();
    load_both(mmio, GP32_ARRAY_COUNT(mmio));
    set_mem_both(RAM_BASE + 0x7FFFFCu, 0xDEADBEEFu);
    CHECK(arm920t_run(cpu_jit, 4u) == 4u && arm920t_run(cpu_ref, 4u) == 4u,
          "RAM-end LDM initial budget");
    compare_state();
    CHECK(ref_reg(4) == 0xDEADBEEFu, "RAM-end LDM pc lost the in-window word");
    CHECK(ref_reg(1) == RAM_BASE + 0x800004u, "RAM-end LDM writeback");
    CHECK((arm920t_get_cpsr(cpu_ref) & 0x20u) == 0u &&
          arm920t_get_pc(cpu_ref) == 0xfffffffcu,
          "out-of-window PC must remain word-aligned ARM via helper");
    run_chunks();
    teardown_pair();

    /* Unaligned span start: helper path; deterministic rotated garbage for
     * the PC word is fine because both sides run the identical op. */
    const uint32_t unaligned[] = {
        0xE3A0140Cu, 0xE2811A01u,
        0xE2811002u,                            /* ADD  r1, r1, #2             */
        block_insn(0, 1, 0, 1, 1, 1, 0x8010u),  /* LDMIA r1!, {r4, pc}         */
        0xEAFFFFFEu,                            /* B . ; dead                  */
    };
    current_case = "ldm-pc-unaligned";
    setup_pair();
    load_both(unaligned, GP32_ARRAY_COUNT(unaligned));
    for (uint32_t a = DATA_ADDR; a < DATA_ADDR + 32u; a += 4u)
        set_mem_both(a, CODE_ADDR + 0x10u);
    run_chunks();
    CHECK(ref_reg(1) == DATA_ADDR + 10u, "unaligned LDM pc writeback");
    teardown_pair();

    /* stmia r1!, {r4, pc} stays on the helper: the stored PC slot must hold
     * the instruction's pipeline value (its own pc + 8), and LDM reads it. */
    const uint32_t stm_pc[] = {
        0xE3A0140Cu, 0xE2811A01u,
        0xE3A04044u,                            /* MOV  r4, #0x44              */
        block_insn(0, 1, 0, 1, 0, 1, 0x8010u),  /* STMIA r1!, {r4, pc}         */
        0xE3A0240Cu, 0xE2822A01u,               /* r2 = DATA_ADDR              */
        0xE5927004u,                            /* LDR  r7, [r2, #4]           */
        0xEAFFFFFEu,                            /* B .                         */
    };
    current_case = "stm-pc-pipeline";
    setup_pair();
    load_both(stm_pc, GP32_ARRAY_COUNT(stm_pc));
    run_chunks();
    CHECK(ref_reg(7) == CODE_ADDR + 0x14u, "STM {..,pc} stored wrong PC value");
    CHECK(ref_reg(1) == DATA_ADDR + 8u, "STM pc writeback");
    teardown_pair();

    /* BL -> push{lr}/mov/pop{pc} leaf must return to BL fallthrough. */
    {
        const uint32_t prog[] = {
            0xE3A0D40Cu,                        /* MOV  r13, #0x0C000000       */
            0xE28DDA02u,                        /* ADD  r13, r13, #0x2000      */
            0xEB000002u,                        /* BL   +2 -> leaf at +0x18    */
            0xE3A02055u,                        /* MOV  r2, #0x55 ; fallthrough */
            0xEAFFFFFEu,                        /* B .                          */
            0xE1A00000u,                        /* NOP  ; unreachable pad       */
            /* +0x18: leaf */
            0xE92D4000u,                        /* STMDB r13!, {lr} ; push lr   */
            0xE3A03077u,                        /* MOV  r3, #0x77               */
            0xE8BD8000u,                        /* LDMIA r13!, {pc} ; pop pc    */
        };
        current_case = "ldm-pc-leaf";
        setup_pair();
        load_both(prog, GP32_ARRAY_COUNT(prog));
        run_chunks();
        CHECK(ref_reg(2) == 0x55u && ref_reg(3) == 0x77u,
              "leaf wrapper did not return to BL fallthrough");
        CHECK(ref_reg(13) == DATA_ADDR + 0x1000u, "leaf push/pop unbalanced SP");
        teardown_pair();
    }

    /* The callee changes its saved return through an alias of SP. The
     * translator still classifies a balanced leaf; native pop must use the
     * actual RAM value and leave the trace instead of assuming BL fallthrough. */
    {
        const uint32_t prog[] = {
            0xE3A0D40Cu, 0xE28DDA02u,  /* SP = RAM_BASE + 0x2000 */
            0xE24D1004u,               /* r1 = saved-LR slot */
            0xE3A00E42u,               /* r0 = CODE_ADDR + 0x20 (0x420) */
            0xEB000004u,               /* BL leaf at +0x28 */
            0xE3A020EEu,               /* normal fallthrough: must not execute */
            0xEAFFFFFEu, 0xE1A00000u,
            0xE3A07077u, 0xEAFFFFFEu,  /* changed return target */
            0xE92D4000u,               /* push LR */
            0xE5810000u,               /* STR r0, [r1]: replace saved return */
            0xE8BD8000u,               /* pop PC */
        };
        current_case = "ldm-pc-leaf-changed-return";
        setup_pair();
        load_both(prog, GP32_ARRAY_COUNT(prog));
        CHECK(arm920t_run(cpu_jit, 128u) == arm920t_run(cpu_ref, 128u), "changed-return budget");
        compare_state();
        CHECK(ref_reg(2) == 0u && ref_reg(7) == 0x77u, "changed return continued stale trace");
        CHECK(ref_reg(13) == RAM_BASE + 0x2000u, "changed return SP");
        teardown_pair();
    }
}

/* Hot memcpy list: contiguous r3/r4 plus sparse r12/r14. Exercise both
 * four-byte (not eight-byte) alignment and a final pair ending at a page edge.
 * Rn is transferred as well, so store-old-base/load-suppressed-writeback and
 * an odd leftover register are checked alongside the pair path. */
/* A word load into PC ends the trace: the x64 emitter proves the RAM span,
 * commits write_r's masked result and retires with the loaded target. A
 * rejected span re-executes the whole instruction through the checked helper
 * and still retires; byte loads and unusual shapes keep their old paths.
 * One instruction of budget proves the load itself ran natively, not the
 * target it reaches. */
static void case_native_ldr_pc(void) {
    for (unsigned mmu = 0; mmu < 2u; ++mmu) {
        for (unsigned variant = 0; variant < 7u; ++variant) {
            current_case = "native-ldr-pc";
            setup_pair();
            const uint32_t ttb = RAM_BASE + 0x4000u;
            const uint32_t base = mmu ? 0x10001000u : DATA_ADDR;
            const uint32_t target = CODE_ADDR + 0x80u;
            const uint32_t low = variant == 0u ? 0u : variant == 1u ? 2u : variant == 2u ? 3u : 0u;
            const uint32_t insn =
                variant == 2u ? 0xe491f004u : /* LDR  pc,[r1],#4  (post-index)  */
                variant == 3u ? 0xe791f103u : /* LDR  pc,[r1,r3,LSL #2]         */
                variant == 4u ? 0xe5d1f000u : /* LDRB pc,[r1]  (byte: helper)   */
                variant == 5u ? 0x0591f000u : /* LDREQ pc,[r1] with Z=0         */
                variant == 6u ? 0xe591f000u : /* LDR  pc,[r1] out of window     */
                                0xe591f000u;  /* LDR  pc,[r1]                   */
            const uint32_t program[] = {
                0xee02af10u, /* MCR p15,0,r10,c2,c0,0 */
                0xee01bf10u, /* MCR p15,0,r11,c1,c0,0 */
                0xe5912000u, /* LDR r2,[r1]: prime the mapping, excluded */
                insn,
                0xe3a090eeu, /* must not execute in this budget */
                0xe1a0f00eu
            };
            load_both(program, GP32_ARRAY_COUNT(program));
            arm920t_set_cpsr(cpu_jit, 0x200000d3u);
            arm920t_set_cpsr(cpu_ref, 0x200000d3u);
            set_reg_both(1u, variant == 6u ? IO_ADDR : base);
            set_reg_both(3u, 1u); /* register-offset variant reads the second word */
            set_reg_both(10u, ttb);
            set_reg_both(11u, mmu);
            set_mem_both(ttb, 2u);                    /* identity BIOS section */
            set_mem_both(ttb + 0x400u, RAM_BASE | 2u);
            set_mem_both(DATA_ADDR, target | low);
            set_mem_both(DATA_ADDR + 4u, target | low);
            CHECK(arm920t_run(cpu_jit, 3u) == arm920t_run(cpu_ref, 3u), "LDR pc setup budget");
            compare_state();
            arm920t_reset_cpu_profile(cpu_jit);
            CHECK(arm920t_run(cpu_jit, 1u) == arm920t_run(cpu_ref, 1u), "LDR pc load budget");
            compare_state();
            CHECK(ref_reg(2u) == (variant == 6u ? 0xffffffffu : (target | low)),
                  "primed load observed the data word");
            CHECK(ref_reg(9u) == 0u, "load budget cannot reach the successor");
            if (variant == 4u) {
                CHECK(arm920t_get_pc(cpu_ref) == ((target & 0xffu) & ~3u),
                      "LDRB pc masks the low byte");
            } else if (variant == 5u) {
                CHECK(arm920t_get_pc(cpu_ref) == CODE_ADDR + 16u, "failed condition keeps PC sequential");
            } else if (variant == 6u) {
                CHECK(arm920t_get_pc(cpu_ref) == 0xfffffffcu, "out-of-window load masks the bus value");
            } else {
                CHECK(arm920t_get_pc(cpu_ref) == target, "LDR pc target word alignment");
            }
            CHECK((arm920t_get_cpsr(cpu_ref) & 0x20u) == 0u, "LDR pc must retain ARM state");
            gp32_cpu_profile_t profile;
            arm920t_get_cpu_profile(cpu_jit, &profile);
            if (profile.supported && profile.native_backend) {
                CHECK(profile.native_block_calls == 1u && profile.native_arm_insns == 1u,
                      "PC load must retire as one native instruction");
            }
            if (profile.supported && profile.native_backend == 1u) {
                int helper = variant == 4u || variant == 6u;
                CHECK(profile.helper_op_kinds[6] == (uint64_t)helper,
                      "only byte/out-of-window PC loads need the checked helper");
            }
            teardown_pair();
        }
    }
}

/* Seeded chained PC loads: the RAM table holds a seeded permutation of the
 * load slots, so one program exercises many native commits and dispatch
 * hand-offs over unpredictable targets. Every load stays inside the proved
 * RAM window, so no SINGLE_DT helper may run on the x64 direct path. */
static void case_native_ldr_pc_chain(void) {
    enum { CHAIN = 20u };
    uint32_t rng = 0x2545f491u;
    for (unsigned seed = 0; seed < 8u; ++seed) {
        for (unsigned mmu = 0; mmu < 2u; ++mmu) {
            current_case = "native-ldr-pc-chain";
            setup_pair();
            const uint32_t ttb = RAM_BASE + 0x8000u;
            const uint32_t table = DATA_ADDR + 0x100u;
            const uint32_t low = seed & 3u;
            uint8_t perm[CHAIN];
            for (unsigned i = 0; i < CHAIN; ++i) perm[i] = (uint8_t)i;
            for (unsigned i = CHAIN; i > 1u; --i) {
                rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
                unsigned j = (rng >> 7) % i;
                uint8_t t = perm[i - 1u]; perm[i - 1u] = perm[j]; perm[j] = t;
            }
            uint32_t program[8u + CHAIN];
            unsigned n = 0;
            program[n++] = 0xee02af10u; /* MCR p15,0,r10,c2,c0,0 */
            program[n++] = 0xee01bf10u; /* MCR p15,0,r11,c1,c0,0 */
            program[n++] = mmu ? 0xe3a04201u : 0xe3a0440cu; /* MOV r4, table VA */
            program[n++] = 0xe2844a01u; /* ADD r4,r4,#0x1000 */
            program[n++] = 0xe2844c01u; /* ADD r4,r4,#0x100  */
            program[n++] = 0xe5942000u; /* LDR r2,[r4]: prime the mapping */
            const unsigned first = n;
            for (unsigned i = 0; i < CHAIN; ++i) program[n++] = 0xe494f004u; /* LDR pc,[r4],#4 */
            program[n++] = 0xeafffffeu; /* B . */
            load_both(program, n);
            arm920t_set_cpsr(cpu_jit, 0x200000d3u);
            arm920t_set_cpsr(cpu_ref, 0x200000d3u);
            set_reg_both(10u, ttb);
            set_reg_both(11u, mmu);
            set_mem_both(ttb, 2u);
            set_mem_both(ttb + 0x400u, RAM_BASE | 2u);
            for (unsigned i = 0; i < CHAIN; ++i)
                set_mem_both(table + 4u * i, (CODE_ADDR + 4u * (first + perm[i])) | low);
            /* Run the setup and the mapping prime before the profile reset so
             * only the chained loads are attributed. */
            CHECK(arm920t_run(cpu_jit, 6u) == arm920t_run(cpu_ref, 6u), "chain setup budget");
            compare_state();
            arm920t_reset_cpu_profile(cpu_jit);
            run_chunks();
            gp32_cpu_profile_t profile;
            arm920t_get_cpu_profile(cpu_jit, &profile);
            if (profile.supported && profile.native_backend == 1u) {
                CHECK(profile.helper_op_kinds[6] == 0u,
                      "chained PC loads must not use SINGLE_DT helpers");
                CHECK(profile.native_arm_insns > 0u, "chained PC loads must run natively");
            }
            teardown_pair();
        }
    }
}

static void case_block_pairs(void) {
    const uint32_t lists[] = {0x5018u, 0x5019u};
    for (unsigned shape = 0; shape < GP32_ARRAY_COUNT(lists); ++shape) {
        unsigned count = shape ? 5u : 4u;
        const uint32_t starts[] = {DATA_ADDR + 4u, RAM_BASE + 0x2000u - 4u * count};
        for (unsigned edge = 0; edge < GP32_ARRAY_COUNT(starts); ++edge) {
            for (unsigned load = 0; load < 2u; ++load) {
                current_case = load ? "block-pairs-load" : "block-pairs-store";
                setup_pair();
                const uint32_t program[] = {
                    block_insn(0, 1, 0, 1, (int)load, 0, lists[shape]),
                    0xe1a0f001u, /* MOV pc,r1: fixed two-op block, then idle */
                };
                load_both(program, GP32_ARRAY_COUNT(program));
                for (unsigned r = 0; r < 15u; ++r)
                    set_reg_both(r, 0x11220000u + r);
                set_reg_both(0u, starts[edge]);
                set_reg_both(1u, CODE_ADDR + 4u);
                unsigned lane = 0;
                for (unsigned r = 0; r < 15u; ++r) {
                    if (!(lists[shape] & (1u << r))) continue;
                    set_mem_both(starts[edge] + 4u * lane++, 0xaabb0000u + r);
                }
                /* Warm a complete native block, then revisit it with a
                 * one-instruction remainder and with a full-block budget. */
                const uint32_t budgets[] = {2u, 1u, 2u};
                for (unsigned step = 0; step < GP32_ARRAY_COUNT(budgets); ++step) {
                    set_reg_both(0u, starts[edge]);
                    set_reg_both(15u, CODE_ADDR);
                    CHECK(arm920t_run(cpu_jit, budgets[step]) ==
                          arm920t_run(cpu_ref, budgets[step]), "paired transfer budget");
                    compare_state();
                    CHECK(ref_reg(0u) == (load && shape ? 0xaabb0000u :
                          starts[edge] + 4u * count), "paired base-in-list/writeback");
                    lane = 0;
                    for (unsigned r = 0; r < 15u; ++r) {
                        if (!(lists[shape] & (1u << r))) continue;
                        uint32_t word = gp32_ld32le(bus_ptr(&bus_ref,
                            starts[edge] + 4u * lane++, 4u));
                        CHECK(load ? ref_reg(r) == 0xaabb0000u + r :
                              word == (r ? 0x11220000u + r : starts[edge]),
                              "paired transfer lane order");
                    }
                }
                gp32_cpu_profile_t profile;
                arm920t_get_cpu_profile(cpu_jit, &profile);
                if (profile.supported && profile.native_backend)
                    CHECK(profile.native_block_calls != 0u, "paired case requires native code");
                teardown_pair();
            }
        }
    }
}

static void case_block_modes(void) {
    current_case = "block-modes";
    setup_pair();
    const uint32_t program[] = {
        block_insn(1, 0, 0, 1, 0, 0, 0x000eu), /* STMDB r0!, {r1-r3} */
        0xE3A01000u, 0xE3A02000u, 0xE3A03000u, /* clear r1-r3 */
        block_insn(0, 1, 0, 1, 1, 0, 0x000eu), /* LDMIA r0!, {r1-r3} */
        block_insn(0, 1, 0, 1, 0, 4, 0x000eu), /* STMIA r4!, {r1-r3} */
        block_insn(1, 0, 0, 0, 1, 4, 0x00e0u), /* LDMDB r4, {r5-r7} */
        block_insn(0, 1, 0, 1, 0, 8, 0x0006u), /* STMIA r8!, {r1,r2}: crosses 4K */
        block_insn(1, 0, 0, 0, 1, 8, 0x0600u), /* LDMDB r8, {r9,r10}: crosses 4K */
        0xEAFFFFFEu,
    };
    load_both(program, GP32_ARRAY_COUNT(program));
    set_reg_both(0u, DATA_ADDR + 0x20u);
    set_reg_both(1u, 0x11u);
    set_reg_both(2u, 0x22u);
    set_reg_both(3u, 0x33u);
    set_reg_both(4u, DATA_ADDR + 0x40u);
    set_reg_both(8u, RAM_BASE + 0x1ffcu);
    run_chunks();
    CHECK(ref_reg(0) == DATA_ADDR + 0x20u && ref_reg(4) == DATA_ADDR + 0x4cu,
          "block transfer writeback");
    CHECK(ref_reg(1) == 0x11u && ref_reg(2) == 0x22u && ref_reg(3) == 0x33u &&
          ref_reg(5) == 0x11u && ref_reg(6) == 0x22u && ref_reg(7) == 0x33u,
          "block transfer register order");
    CHECK(ref_reg(8) == RAM_BASE + 0x2004u && ref_reg(9) == 0x11u &&
          ref_reg(10) == 0x22u, "cross-page block transfer fallback");
    teardown_pair();
}

/* MUL/MLA and 32x32->64 UMULL/SMULL (wide carry / signed negate). */
static const uint32_t P_MUL[] = {
    0xE3A00007u, /* MOV   r0, #7                                           */
    0xE3A01006u, /* MOV   r1, #6                                           */
    0xE0020190u, /* MUL   r2, r0, r1         ; 42                          */
    0xE0232190u, /* MLA   r3, r0, r1, r2     ; 7*6 + 42 = 84               */
    0xE0854190u, /* UMULL r4, r5, r0, r1     ; 42 -> r4, 0 -> r5           */
    0xE3A060FFu, /* MOV   r6, #0xFF                                        */
    0xE3E07000u, /* MVN   r7, #0             ; 0xFFFFFFFF                  */
    0xE0898697u, /* UMULL r8, r9, r6, r7     ; 0xFE_FFFFFF01               */
    0xE0CBA697u, /* SMULL r10, r11, r6, r7   ; 0xFFFFFFFF_FFFFFF01         */
    0xE01C0097u, /* MULS  r12, r7, r0        ; negative, preserves C/V      */
    0xE01D0095u, /* MULS  r13, r5, r0        ; zero, preserves C/V          */
    0xE02CC190u, /* MLA   r12, r0, r1, r12   ; Rd aliases Rn, result 35    */
    0xEAFFFFFEu, /* B .                                                    */
};
static void case_mul(void) {
    current_case = "mul";
    setup_pair();
    load_both(P_MUL, GP32_ARRAY_COUNT(P_MUL));
    arm920t_set_cpsr(cpu_jit, arm920t_get_cpsr(cpu_jit) | 0x30000000u);
    arm920t_set_cpsr(cpu_ref, arm920t_get_cpsr(cpu_ref) | 0x30000000u);
    run_chunks();
    CHECK(ref_reg(2) == 42u, "MUL result");
    CHECK(ref_reg(3) == 84u, "MLA result");
    CHECK(ref_reg(4) == 42u && ref_reg(5) == 0u, "UMULL low product");
    CHECK(ref_reg(8) == 0xFFFFFF01u && ref_reg(9) == 0x000000FEu, "UMULL wide product");
    CHECK(ref_reg(10) == 0xFFFFFF01u && ref_reg(11) == 0xFFFFFFFFu, "SMULL signed product");
    CHECK(ref_reg(12) == 35u && ref_reg(13) == 0u, "MUL flags/MLA alias result");
    CHECK((arm920t_get_cpsr(cpu_ref) & 0xF0000000u) == 0x70000000u,
          "MULS zero did not preserve C/V and set Z");
    teardown_pair();
}

/* Non-zero initial registers with carry-in: ADC/SBC/RSC/ORRS/BIC and the
 * CPSR dependency of the block tag. */
static const uint32_t P_SEED[] = {
    0xE0B04001u, /* ADCS r4, r0, r1                                        */
    0xE0D45002u, /* SBCS r5, r4, r2                                        */
    0xE0F06002u, /* RSCS r6, r0, r2                                        */
    0xE1977005u, /* ORRS r7, r4, r5                                        */
    0xE1C78003u, /* BIC  r8, r7, r3                                        */
    0xEAFFFFFEu, /* B .                                                    */
};
static void case_seeded(void) {
    current_case = "seeded";
    setup_pair();
    load_both(P_SEED, GP32_ARRAY_COUNT(P_SEED));
    set_reg_both(0u, 0x80000000u);
    set_reg_both(1u, 0x80000000u);
    set_reg_both(2u, 0xFFFFFFFFu);
    set_reg_both(3u, 0x0F0F0F0Fu);
    arm920t_set_cpsr(cpu_jit, arm920t_get_cpsr(cpu_jit) | 0x20000000u);   /* C=1 carry-in */
    arm920t_set_cpsr(cpu_ref, arm920t_get_cpsr(cpu_ref) | 0x20000000u);
    run_chunks();
    CHECK(ref_reg(4) == 0x00000001u, "ADCS overflow result");
    CHECK(ref_reg(5) == 0x00000002u, "SBCS carry-in result");
    CHECK(ref_reg(6) == 0x7FFFFFFEu, "RSCS borrow result");
    CHECK(ref_reg(8) == 0x00000000u, "BIC mask result");
    teardown_pair();
}

/* Counting loop used to probe budget boundaries at every alignment. */
static const uint32_t P_LOOP[] = {
    0xE3A00000u, /* MOV r0, #0                                            */
    0xE2800001u, /* ADD r0, r0, #1                                        */
    0xE3500064u, /* CMP r0, #100                                          */
    0x1AFFFFFCu, /* BNE -4 (word 1)                                       */
    0xEAFFFFFEu, /* B .                                                   */
};
static void case_budget(void) {
    current_case = "budget";
    setup_pair();
    load_both(P_LOOP, GP32_ARRAY_COUNT(P_LOOP));
    run_chunks();
    CHECK(ref_reg(0) == 100u, "loop did not complete");
    CHECK(arm920t_get_pc(cpu_ref) == CODE_ADDR + 16u, "loop did not park on B self");
    teardown_pair();
}

/* A plain RAM loop is dispatched by the direct native entry once its PC is
 * bound to the current epoch and generation: the block claims one whole call
 * instead of the general per-block bookkeeping. Six budgets above the loop
 * length retire thousands of such transitions, and short budgets then mix
 * partial blocks, portable tails and the compiled path. Registers, PC, CPSR,
 * the retired cycle count and the RAM image must stay identical to the
 * interpreter after every run, and the workload must really have run natively
 * rather than falling back to the portable blocks. */
static void case_direct_dispatch(void) {
    static const uint32_t program[] = {
        0xE3A00000u, /* MOV r0, #0         */
        0xE2800001u, /* loop: ADD r0,r0,#1 */
        0xE3500DFAu, /*       CMP r0,#16000 */
        0x1AFFFFFCu, /*       BNE loop      */
        0xEAFFFFFEu, /* B .                */
    };
    current_case = "direct-dispatch";
    setup_pair();
    load_both(program, GP32_ARRAY_COUNT(program));
    for (unsigned round = 0; round < 8u; ++round) {
        uint32_t budget = round < 6u ? 8192u : 250u;
        uint32_t dj = arm920t_run(cpu_jit, budget), dr = arm920t_run(cpu_ref, budget);
        if (dj != dr) report("direct dispatch budget", dj, dr);
        compare_state();
    }
    CHECK(ref_reg(0) == 16000u, "direct-dispatch loop did not run every iteration");
    CHECK(arm920t_get_pc(cpu_ref) == CODE_ADDR + 16u,
          "direct-dispatch loop did not park on B self");
    gp32_cpu_profile_t profile;
    arm920t_get_cpu_profile(cpu_jit, &profile);
    if (profile.supported && profile.native_backend)
        CHECK(profile.native_arm_insns != 0u && profile.native_block_calls != 0u,
              "direct-dispatch loop never entered native code");
    teardown_pair();
}

/* Start with enough budget to execute the actual native block. The ragged
 * harness alone can interpret a short program before ever entering its JIT. */
static void run_native_case(void) {
    uint32_t j = arm920t_run(cpu_jit, 64u), r = arm920t_run(cpu_ref, 64u);
    CHECK(j == 64u && r == 64u, "native case instruction budget");
    compare_state();
}

/* Bus callbacks observe the current instruction's PC+4, even after an inlined
 * return. Cover each MMU-off native bus helper in one caller sequence. */
static void case_callback_pc(void) {
    const uint32_t returns[] = {0xe1a0f00eu, 0xe12fff1eu};
    const uint32_t expected[] = {0x408u, 0x40cu, 0x410u, 0x414u,
                                 0x418u, 0x41cu, 0x420u, 0x424u};
    const uint32_t program[] = {
        0xeb00001eu, /* BL CODE_ADDR+0x80 */
        0xe5901000u, /* LDR r1,[r0] */
        0xe5d02000u, /* LDRB r2,[r0] */
        0xe1d030b0u, /* LDRH r3,[r0] */
        0xe1d040d0u, /* LDRSB r4,[r0] */
        0xe1d050f0u, /* LDRSH r5,[r0] */
        0xe5806000u, /* STR r6,[r0] */
        0xe5c06000u, /* STRB r6,[r0] */
        0xe1c060b0u, /* STRH r6,[r0] */
        0xeafffffeu,
    };
    for (unsigned ret = 0; ret < GP32_ARRAY_COUNT(returns); ++ret) {
        current_case = ret ? "callback-PC-bx" : "callback-PC-mov";
        setup_pair();
        arm920t_set_trace(cpu_ref, 1, NULL, NULL); /* exec_arm_at oracle */
        bus_ref.observe_cpu = cpu_ref;
        bus_jit.observe_cpu = cpu_jit;
        load_both(program, GP32_ARRAY_COUNT(program));
        set_mem_both(CODE_ADDR + 0x80u, 0xe3a07001u); /* MOV r7,#1 */
        set_mem_both(CODE_ADDR + 0x84u, returns[ret]);
        set_reg_both(0, IO_ADDR);
        set_reg_both(6, 0x12345678u);
        run_native_case();
        CHECK(bus_ref.io_count == GP32_ARRAY_COUNT(expected) &&
              bus_jit.io_count == GP32_ARRAY_COUNT(expected), "callback access count");
        CHECK(!memcmp(bus_ref.io_pc, expected, sizeof(expected)), "interpreter callback PC+4");
        for (unsigned i = 0; i < GP32_ARRAY_COUNT(expected); ++i)
            if (bus_jit.io_pc[i] != bus_ref.io_pc[i])
                report("callback PC", bus_jit.io_pc[i], bus_ref.io_pc[i]);
        CHECK(ref_reg(1) == 0x408u, "MMIO read did not return interpreter PC+4");
        gp32_cpu_profile_t profile;
        arm920t_get_cpu_profile(cpu_jit, &profile);
        if (profile.supported && profile.native_backend)
            CHECK(profile.native_block_calls != 0u, "callback case never entered native code");
        teardown_pair();
    }
}

/* An unframed BL leaf must retire the real return, including short budgets.
 * The complete four-insn call needs one dispatch; changing LR or the return
 * condition must retain ordinary control flow rather than a stale caller PC. */
static void case_unframed_leaf(void) {
    const uint32_t returns[] = {0xe1a0f00eu, 0xe12fff1eu}; /* MOV pc,lr; BX lr */
    const uint32_t inputs[] = {0xfffffff9u, 0u, 7u, 0x80000000u};
    const uint32_t outputs[] = {7u, 0u, 7u, 0x80000000u};
    const uint32_t budgets[] = {0u, 1u, 2u, 3u, 4u, 7u};
    for (unsigned ret = 0; ret < GP32_ARRAY_COUNT(returns); ++ret) {
        for (unsigned input = 0; input < GP32_ARRAY_COUNT(inputs); ++input) {
            for (unsigned budget = 0; budget < GP32_ARRAY_COUNT(budgets); ++budget) {
                uint32_t program[] = {
                    0xeb000006u, 0xe3a02055u, 0xeafffffeu,
                    0xe1a00000u, 0xe3a07077u, 0xeafffffeu,
                    0xe1a00000u, 0xe1a00000u,
                    0xe3500000u, 0xb2600000u, returns[ret],
                };
                current_case = ret ? "unframed-leaf-bx" : "unframed-leaf-mov";
                setup_pair(); load_both(program, GP32_ARRAY_COUNT(program));
                set_reg_both(0, inputs[input]);
                CHECK(arm920t_run(cpu_jit, budgets[budget]) ==
                      arm920t_run(cpu_ref, budgets[budget]), "unframed leaf partial budget");
                compare_state();
                if (budgets[budget] == 4u) {
                    CHECK(arm920t_get_jit_hits(cpu_jit) + arm920t_get_jit_misses(cpu_jit) == 1u,
                          "complete unframed call was not one dispatch");
                    CHECK(ref_reg(0) == outputs[input] && ref_reg(14) == CODE_ADDR + 4u &&
                          ref_reg(15) == CODE_ADDR + 4u, "unframed leaf result/return");
                }
                if (budgets[budget] == 7u) {
                    gp32_cpu_profile_t profile;
                    arm920t_get_cpu_profile(cpu_jit, &profile);
                    if (profile.supported && profile.native_backend)
                        CHECK(profile.native_block_calls == 1u && profile.native_arm_insns == 7u,
                              "native leaf did not continue through caller fallthrough");
                }
                run_native_case(); run_chunks();
                CHECK(ref_reg(0) == outputs[input] && ref_reg(2) == 0x55u,
                      "unframed leaf abs/fallthrough");
                teardown_pair();
            }
        }
    }

    /* PC/LR operands must use the callee's pipeline PC and the BL link. */
    {
        const uint32_t program[] = {
            0xeb000002u, 0xe3a02055u, 0xeafffffeu, 0xe1a00000u,
            0xe28f3000u, 0xe1a0500eu, 0xe1a0f00eu,
        };
        current_case = "unframed-leaf-pc-lr";
        setup_pair(); load_both(program, GP32_ARRAY_COUNT(program));
        run_native_case(); run_chunks();
        CHECK(ref_reg(3) == CODE_ADDR + 24u && ref_reg(5) == CODE_ADDR + 4u,
              "unframed callee PC/LR operands");
        teardown_pair();
    }

    for (unsigned ret = 0; ret < GP32_ARRAY_COUNT(returns); ++ret) {
        uint32_t program[] = {
            0xeb000006u, 0xe3a02055u, 0xeafffffeu, 0xe1a00000u,
            ret ? 0xe7fe2777u : 0xe3a07077u, 0xeafffffeu,
            0xe1a00000u, 0xe1a00000u,
            0xe1a0e004u, returns[ret], /* MOV lr,r4; return to changed target */
        };
        current_case = "unframed-leaf-changed-lr";
        setup_pair(); load_both(program, GP32_ARRAY_COUNT(program));
        set_reg_both(4, CODE_ADDR + 16u + ret);
        run_native_case(); run_chunks();
        CHECK(ref_reg(2) == 0u && ref_reg(7) == 0x77u &&
              !!(arm920t_get_cpsr(cpu_ref) & 0x20u) == !!ret,
              "changed LR continued caller or lost BX interworking");
        teardown_pair();
    }
    {
        const uint32_t program[] = {
            0xeb000002u, 0xe3a02055u, 0xeafffffeu, 0xe1a00000u,
            0xe3500000u, 0x11a0f00eu, 0xe3a07077u, 0xeafffffeu,
        };
        current_case = "unframed-leaf-conditional-return";
        setup_pair(); load_both(program, GP32_ARRAY_COUNT(program));
        run_native_case(); run_chunks();
        CHECK(ref_reg(2) == 0u && ref_reg(7) == 0x77u,
              "failed conditional return continued caller");
        teardown_pair();
    }
}

/* A helper must observe every ALU result before exception entry banks SP/LR.
 * Comparing only the state after arm920t_run would miss stale callback state. */
static int terminal_swi_yield(void *user, arm920t_t *cpu, uint32_t imm,
                              uint32_t pc, int is_thumb) {
    ++*(unsigned *)user;
    CHECK(imm == 0x43u && pc == CODE_ADDR && !is_thumb &&
          arm920t_get_pc(cpu) == CODE_ADDR + 4u, "terminal SWI callback PC");
    arm920t_set_reg(cpu, 15u, CODE_ADDR + 0x80u);
    arm920t_flush_jit(cpu);
    arm920t_limit_run(cpu, 1u);
    arm920t_set_irq(cpu, 1);
    return 1;
}

static int terminal_swi_decline(void *user, arm920t_t *cpu, uint32_t imm,
                                uint32_t pc, int is_thumb) {
    ++*(unsigned *)user;
    CHECK(imm == 0x123456u && pc == CODE_ADDR && !is_thumb &&
          arm920t_get_pc(cpu) == CODE_ADDR + 4u, "declined SWI callback arguments");
    arm920t_set_cpsr(cpu, 0x600000d1u); /* mutation precedes architectural entry */
    arm920t_set_reg(cpu, 13u, 0x12340000u);
    arm920t_set_reg(cpu, 15u, CODE_ADDR + 0x80u);
    arm920t_flush_jit(cpu);
    return 0;
}

static void case_terminal_swi_decline(void) {
    const uint32_t instructions[] = {0xef123456u, 0x0f123456u, 0x0f123456u, 0xff123456u};
    for (unsigned i = 0; i < GP32_ARRAY_COUNT(instructions); ++i) {
        current_case = "terminal-swi-declined-or-predicated";
        setup_pair();
        arm920t_set_trace(cpu_ref, 1, NULL, NULL);
        uint32_t cpsr = i == 1u ? 0x400000d3u : 0xd3u;
        arm920t_set_cpsr(cpu_jit, cpsr); arm920t_set_cpsr(cpu_ref, cpsr);
        unsigned observed_jit = 0, observed_ref = 0;
        arm920t_set_swi_handler(cpu_jit, terminal_swi_decline, &observed_jit);
        arm920t_set_swi_handler(cpu_ref, terminal_swi_decline, &observed_ref);
        load_both(&instructions[i], 1u);
        CHECK(arm920t_run(cpu_jit, 1u) == 1u && arm920t_run(cpu_ref, 1u) == 1u,
              "SWI or failed predicate consumes one instruction");
        compare_state();
        arm920t_register_context_t actual = {0}, expected = {0};
        arm920t_get_register_context(cpu_jit, &actual);
        arm920t_get_register_context(cpu_ref, &expected);
        CHECK(!memcmp(&actual, &expected, sizeof(actual)), "SWI preserves all exception banks");
        CHECK(observed_jit == (i < 2u) && observed_ref == (i < 2u),
              "only passing SWI predicates invoke the handler");
        if (i < 2u) {
            CHECK(arm920t_get_pc(cpu_ref) == 8u && ref_reg(14u) == CODE_ADDR + 4u,
                  "declined hook enters SVC with original SWI return address");
            CHECK(expected.spsr_svc == 0x600000d1u && expected.bank_fiq[5] == 0x12340000u,
                  "exception entry saves the handler's updated live state");
        } else CHECK(arm920t_get_pc(cpu_ref) == CODE_ADDR + 4u &&
                     arm920t_get_cpsr(cpu_ref) == cpsr, "failed predicate has no exception effects");
        teardown_pair();
    }
}

static void case_terminal_swi_yield(void) {
    case_terminal_swi_decline();
    current_case = "terminal-swi-yield";
    setup_pair();
    const uint32_t program[] = {0xef000043u, 0xe3a05055u, 0xeafffffeu};
    load_both(program, GP32_ARRAY_COUNT(program));
    set_mem_both(CODE_ADDR + 0x80u, 0xe3a05055u);
    set_mem_both(0x18u, 0xe3a06066u);
    arm920t_set_cpsr(cpu_jit, 0x53u);
    arm920t_set_cpsr(cpu_ref, 0x53u);
    unsigned observed_jit = 0, observed_ref = 0;
    arm920t_set_swi_handler(cpu_jit, terminal_swi_yield, &observed_jit);
    arm920t_set_swi_handler(cpu_ref, terminal_swi_yield, &observed_ref);
    CHECK(arm920t_run(cpu_jit, 32u) == 1u && arm920t_run(cpu_ref, 32u) == 1u,
          "terminal callback yields after its instruction");
    compare_state();
    CHECK(observed_jit == 1u && observed_ref == 1u && ref_reg(5u) == 0u &&
          arm920t_get_pc(cpu_ref) == CODE_ADDR + 0x80u, "callback redirect survives return");
    gp32_cpu_profile_t profile;
    arm920t_get_cpu_profile(cpu_jit, &profile);
    if (profile.supported && profile.native_backend)
        CHECK(profile.native_block_calls == 1u && profile.native_arm_insns == 1u,
              "yield must leave an active native block");
    CHECK(arm920t_run(cpu_jit, 1u) == arm920t_run(cpu_ref, 1u), "pending IRQ budget");
    compare_state();
    CHECK(ref_reg(5u) == 0u && ref_reg(6u) == 0x66u &&
          (arm920t_get_cpsr(cpu_ref) & 31u) == 0x12u,
          "dispatcher takes pending IRQ before the redirected successor");
    teardown_pair();
}

/* A predicate-failed coprocessor load ends its native block. The classified
 * executor commits PC+4 before it tests the predicate, so the block's failed
 * path must commit the same PC; otherwise the dispatcher re-enters this PC
 * forever and no further guest instruction ever retires. ARM920T has no p8
 * coprocessor and this model keeps the unallocated access a no-op, so a failed
 * predicate has exactly one architectural effect: the PC advances and no
 * writeback happens. */
static void case_terminal_coproc_predicate(void) {
    static const struct { const char *name; uint32_t insn, cpsr; int failed; } cases[] = {
        {"ldc-cs-predicate-failed", 0x2cb358a5u, 0x80000053u, 1}, /* C=0 -> CS fails */
        {"ldc-ne-predicate-failed", 0x1cb358a5u, 0x40000053u, 1}, /* Z=1 -> NE fails */
        {"ldc-passing-predicate", 0xecb358a5u, 0x60000053u, 0}     /* control: executes */
    };
    for (unsigned k = 0; k < GP32_ARRAY_COUNT(cases); ++k) {
        current_case = cases[k].name;
        setup_pair();
        arm920t_set_trace(cpu_ref, 1, NULL, NULL);
        const uint32_t program[] = {
            0xe3a03000u, /* MOV r3,#0: LDC base and writeback target */
            cases[k].insn,
            0xe3a02011u, /* MOV r2,#0x11: reached only if the PC advanced */
            0xeafffffeu, /* B . */
        };
        load_both(program, GP32_ARRAY_COUNT(program));
        arm920t_set_cpsr(cpu_jit, cases[k].cpsr);
        arm920t_set_cpsr(cpu_ref, cases[k].cpsr);
        run_chunks();
        CHECK(ref_reg(2u) == 0x11u, "terminal coprocessor load must advance the PC");
        if (cases[k].failed)
            CHECK(ref_reg(3u) == 0u, "failed predicate must suppress the writeback");
        CHECK(arm920t_get_pc(cpu_ref) == CODE_ADDR + 12u && arm920t_get_pc(cpu_jit) == CODE_ADDR + 12u,
              "both backends park after the terminal load");
        gp32_cpu_profile_t profile;
        arm920t_get_cpu_profile(cpu_jit, &profile);
        if (profile.supported && profile.native_backend)
            CHECK(profile.native_block_calls != 0u, "terminal coprocessor load must run natively");
        teardown_pair();
    }
}

static int alu_region_swi(void *user, arm920t_t *cpu, uint32_t imm,
                          uint32_t pc, int is_thumb) {
    const uint32_t expected[] = {
        2u, 4u, 3u, 5u, 5u, 1u, 4u, 0u, 0xffffffffu, 0u,
        0x80000fffu, 0x11223344u, 0x80001000u, 0x80001000u,
        0x80000fffu, CODE_ADDR + 72u
    };
    ++*(unsigned *)user;
    CHECK(imm == 0x42u && pc == CODE_ADDR + 68u && !is_thumb,
          "ALU observer SWI arguments");
    for (unsigned r = 0; r < GP32_ARRAY_COUNT(expected); ++r)
        CHECK(arm920t_get_reg(cpu, r) == expected[r], "ALU result not visible to SWI");
    CHECK(arm920t_get_cpsr(cpu) == 0xb00000dfu, "flagless ALU changed CPSR");
    return 0; /* Take the real SVC exception after observing the unbanked state. */
}

static void case_native_alu_region(void) {
    const uint32_t program[] = {
        0xe3a00001u, /* MOV r0,#1 */
        0xe2800001u, /* ADD r0,r0,#1 */
        0xe0801000u, /* ADD r1,r0,r0 */
        0xe2412001u, /* SUB r2,r1,#1 */
        0xe2623008u, /* RSB r3,r2,#8 */
        0xe1834001u, /* ORR r4,r3,r1 */
        0xe2045003u, /* AND r5,r4,#3 */
        0xe0256003u, /* EOR r6,r5,r3 */
        0xe3c67004u, /* BIC r7,r6,#4 */
        0xe1e08007u, /* MVN r8,r7 */
        0xe1a09008u, /* MOV r9,r8 */
        0xe2899001u, /* ADD r9,r9,#1 */
        0xe28aaa01u, /* ADD r10,r10,#0x1000 */
        0xe1a0c00au, /* MOV r12,r10 */
        0xe28cc001u, /* ADD r12,r12,#1 */
        0xe1a0d00cu, /* MOV r13,r12 */
        0xe24de001u, /* SUB r14,r13,#1 */
        0xef000042u, /* SWI #0x42 */
    };
    const uint32_t partial_budgets[] = {0u, 1u, 17u};
    for (unsigned i = 0; i < GP32_ARRAY_COUNT(partial_budgets); ++i) {
        unsigned observed_jit = 0, observed_ref = 0;
        current_case = "native-alu-region-observer";
        setup_pair(); load_both(program, GP32_ARRAY_COUNT(program));
        set_reg_both(10, 0x7fffffffu);
        set_reg_both(11, 0x11223344u);
        arm920t_set_cpsr(cpu_jit, 0xb00000dfu);
        arm920t_set_cpsr(cpu_ref, 0xb00000dfu);
        arm920t_set_swi_handler(cpu_jit, alu_region_swi, &observed_jit);
        arm920t_set_swi_handler(cpu_ref, alu_region_swi, &observed_ref);
        if (partial_budgets[i]) {
            CHECK(arm920t_run(cpu_jit, partial_budgets[i]) ==
                  arm920t_run(cpu_ref, partial_budgets[i]), "ALU partial budget");
            compare_state();
        }
        run_native_case();
        CHECK(observed_jit == 1u && observed_ref == 1u, "ALU observer invocation");
        CHECK(arm920t_get_pc(cpu_ref) == 8u &&
              arm920t_get_cpsr(cpu_ref) == 0xb00000d3u, "ALU SVC exception state");
        /* Restore SYS to check that exception entry saved the committed bank. */
        arm920t_set_cpsr(cpu_jit, 0xb00000dfu);
        arm920t_set_cpsr(cpu_ref, 0xb00000dfu);
        compare_state();
        CHECK(ref_reg(13) == 0x80001000u && ref_reg(14) == 0x80000fffu,
              "ALU results lost during SP/LR banking");
        teardown_pair();
    }
}

/* Register dependencies must survive conditionals and scratch/helper clobbers. */
static void case_native_forwarding(void) {
    const uint32_t program[] = {
        /* MOV/ADD chain, both sources alias, then shifted operand. */
        0xe1a01000u, 0xe2811001u, 0xe0812001u, 0xe0223081u, 0xe48b3004u,
        /* MVN r4,#0; ADDS r4,r4,#1; ADC r5,r4,#7; record carry use. */
        0xe3e04000u, 0xe2944001u, 0xe2a45007u, 0xe48b5004u,
        /* CMP r0,r0; MOV r1,#22; skipped MOVNE, then taken MOVEQ. */
        0xe1500000u, 0xe3a01022u, 0x13a01066u, 0xe2812001u, 0xe48b2004u,
        0x03a01066u, 0xe2812001u, 0xe48b2004u,
        /* TST overwrites host result scratch but not guest r1. */
        0xe3110000u, 0xe2812001u, 0xe48b2004u,
        /* LDR overwrites r1; register-specified shift uses scratch W2. */
        0xe3a01009u, 0xe59a1000u, 0xe2812001u, 0xe48b2004u,
        0xe3a02003u, 0xe1a03211u, 0xe2824001u, 0xe48b4004u,
        /* MRS overwrites r1 and SWP crosses a classified helper. */
        0xe1500000u, 0xe3a01011u, 0xe10f1000u, 0xe2812001u, 0xe48b2004u,
        0xe3a02042u, 0xe3a01009u, 0xe10a1092u, 0xe2812001u, 0xe48b2004u,
        /* MUL overwrites r1. ARMv4 NV must never become a producer. */
        0xe3a02003u, 0xe3a01055u, 0xe0010292u, 0xe2812001u, 0xe48b2004u,
        0xe3a01055u, 0xf3a01066u, 0xe2812001u, 0xe48b2004u, 0xeafffffeu
    };
    const uint32_t expected[] = {
        0u, 8u, 0x23u, 0x67u, 0x67u, 0x12345679u, 4u,
        0x600000d4u, 0x12345679u, 10u, 0x56u
    };
    const uint32_t seeds[] = {0u, 0xffffffffu, 0x80000000u};
    for (unsigned seed = 0; seed < GP32_ARRAY_COUNT(seeds); ++seed)
        for (unsigned partial = 0; partial < 2; ++partial) {
            current_case = "native-forwarding";
            setup_pair(); load_both(program, GP32_ARRAY_COUNT(program));
            set_reg_both(0, seeds[seed]);
            set_reg_both(10, DATA_ADDR + 0x200u); set_reg_both(11, DATA_ADDR);
            set_mem_both(DATA_ADDR + 0x200u, 0x12345678u);
            if (partial) {
                CHECK(arm920t_run(cpu_jit, 2u) == arm920t_run(cpu_ref, 2u), "partial entry budget");
                compare_state();
            }
            run_native_case();
            for (unsigned i = 0; i < GP32_ARRAY_COUNT(expected); ++i)
                CHECK(gp32_ld32le(bus_ref.ram + (DATA_ADDR - RAM_BASE) + i * 4u) == expected[i],
                      "forwarding fixture result");
            CHECK(ref_reg(11) == DATA_ADDR + sizeof(expected), "recorded every boundary result");
            CHECK(gp32_ld32le(bus_ref.ram + (DATA_ADDR - RAM_BASE) + 0x200u) == 0x42u, "SWP performed its store");
            teardown_pair();
        }
}

/* Self moves may disappear, but flags, shifted forms and PC writes may not.
 * Record intermediate values before later instructions can hide a mistake. */
static void case_native_self_move_noop(void) {
    const uint32_t program[] = {
        0xe1a05000u, /* MOV  r5,r0          ; r5 = seed                       */
        0xe1a08000u, /* MOV  r8,r0          ; r8 = seed                       */
        0xe2855001u, /* ADD  r5,r5,#1       ; r5 = seed+1, publishes EAX/w2   */
        0xe1a05005u, /* MOV  r5,r5          ; no-op between producer/consumer */
        0xe2854002u, /* ADD  r4,r5,#2       ; r4 = seed+3 through forwarded r5 */
        0xe8aa0010u, /* STMIA r10!,{r4}                                    */
        0xe1500000u, /* CMP  r0,r0          ; EQ always passes                */
        0x01a04004u, /* MOVEQ r4,r4         ; taken conditional no-op         */
        0x02844001u, /* ADDEQ r4,r4,#1                                     */
        0xe8aa0010u, /* STMIA r10!,{r4}                                    */
        0xe3500000u, /* CMP  r0,#0          ; NE is seed != 0                 */
        0x11a04004u, /* MOVNE r4,r4         ; skipped when seed == 0          */
        0x12844005u, /* ADDNE r4,r4,#5                                     */
        0xe8aa0010u, /* STMIA r10!,{r4}                                    */
        0xe128f00bu, /* MSR  CPSR_f,r11     ; NZCV = 0010 before MOVS         */
        0xe1a05000u, /* MOV  r5,r0                                         */
        0xe1b05005u, /* MOVS r5,r5          ; S=1 still writes N/Z, keeps C   */
        0xe10f1000u, /* MRS  r1,CPSR                                       */
        0xe8aa0002u, /* STMIA r10!,{r1}                                    */
        0xe1a02080u, /* MOV  r2,r0,LSL#1                                   */
        0xe1a02082u, /* MOV  r2,r2,LSL#1    ; shifted self-move still shifts   */
        0xe8aa0004u, /* STMIA r10!,{r2}                                    */
        0xe1a08028u, /* MOV  r8,r8,LSR#0    ; LSR #0 is LSR #32, not a no-op   */
        0xe8aa0100u, /* STMIA r10!,{r8}                                    */
        0xe1a0f00fu, /* MOV  pc,pc          ; still jumps to pc+8             */
        0xe3a04000u, /* MOV  r4,#0          ; skipped by that jump            */
        0xe2844001u, /* ADD  r4,r4,#1                                      */
        0xe8aa0010u, /* STMIA r10!,{r4}                                    */
        0xeafffffeu  /* B .                                                */
    };
    const uint32_t seeds[] = {0u, 1u, 0x7fffffffu, 0x80000000u, 0x12345678u, 0xffffffffu};
    for (unsigned i = 0; i < GP32_ARRAY_COUNT(seeds); ++i) {
        const uint32_t seed = seeds[i];
        uint32_t expect;
        for (unsigned partial = 0; partial < 2u; ++partial) {
            current_case = "native-self-move-noop";
            setup_pair();
            load_both(program, GP32_ARRAY_COUNT(program));
            set_reg_both(0u, seed);
            set_reg_both(10u, DATA_ADDR);
            set_reg_both(11u, 0x20000000u); /* deterministic NZCV before MOVS */
            if (partial) {
                CHECK(arm920t_run(cpu_jit, 2u) == arm920t_run(cpu_ref, 2u), "partial entry budget");
                compare_state();
            }
            run_native_case();
            /* Replay the entry after warming, so the observer also checks
             * its native body rather than only the final park branch. */
            set_reg_both(0u, seed);
            set_reg_both(10u, DATA_ADDR);
            set_reg_both(11u, 0x20000000u);
            set_reg_both(15u, CODE_ADDR);
            run_native_case();
            const uint8_t *rec = bus_ref.ram + (DATA_ADDR - RAM_BASE);
            CHECK(ref_reg(10u) == DATA_ADDR + 28u, "every self-move result recorded");
            expect = seed + 3u;
            CHECK(gp32_ld32le(rec) == expect, "plain self-move keeps its value");
            expect += 1u;
            CHECK(gp32_ld32le(rec + 4u) == expect, "taken MOVEQ self-move");
            if (seed) expect += 5u;
            CHECK(gp32_ld32le(rec + 8u) == expect, "MOVNE self-move follows its condition");
            uint32_t nz = (seed & 0x80000000u) ? 0x80000000u :
                          seed == 0u ? 0x40000000u : 0u;
            CHECK((gp32_ld32le(rec + 12u) & 0xf0000000u) == (nz | 0x20000000u),
                  "MOVS writes N/Z and preserves C");
            CHECK(gp32_ld32le(rec + 16u) == (seed << 2), "shifted self-move still shifts");
            CHECK(gp32_ld32le(rec + 20u) == 0u, "LSR #0 self-move still shifts by 32");
            CHECK(gp32_ld32le(rec + 24u) == expect + 1u, "MOV pc,pc still jumps one instruction");
            CHECK(ref_reg(4u) == expect + 1u && ref_reg(8u) == 0u,
                  "register file matches the recorded values");
            gp32_cpu_profile_t p;
            arm920t_get_cpu_profile(cpu_jit, &p);
            if (p.supported && p.native_backend)
                CHECK(p.native_arm_insns > 0u, "self-move no-op case must run native");
            teardown_pair();
        }
    }
}

/* Record every immediate result and NZCV before the next instruction can
 * overwrite it. A long initial budget exercises native blocks from entry. */
static void case_native_immediates(void) {
    const uint32_t seeds[] = {0u, 1u, 0x7fffffffu, 0xffffffffu};
    const uint32_t immediates[] = {0u, 1u, 0xffu, 0xc01u, 0xa01u, 0xaffu, 0x480u, 0x4ffu};
    const uint32_t operations[] = {
        0xe2901000u, 0xe2501000u, 0xe3500000u, 0xe3700000u, /* ADDS/SUBS/CMP/CMN */
        0xe2801000u, 0xe2401000u, 0xe3a01000u, 0xe3e01000u, /* ADD/SUB/MOV/MVN */
        0xe3b01000u, 0xe3f01000u                          /* MOVS/MVNS */
    };
    uint32_t program[241];
    unsigned n = 0;
    for (unsigned i = 0; i < GP32_ARRAY_COUNT(immediates); ++i)
        for (unsigned op = 0; op < GP32_ARRAY_COUNT(operations); ++op) {
            program[n++] = operations[op] | immediates[i];
            program[n++] = 0xe10f2000u; /* MRS r2,CPSR */
            program[n++] = 0xe8aa0006u; /* STMIA r10!,{r1,r2} */
        }
    program[n++] = 0xeafffffeu;
    for (unsigned seed = 0; seed < GP32_ARRAY_COUNT(seeds); ++seed) {
        current_case = "native-immediates";
        setup_pair();
        load_both(program, n);
        set_reg_both(0, seeds[seed]);
        set_reg_both(10, DATA_ADDR);
        uint32_t j = arm920t_run(cpu_jit, 512), r = arm920t_run(cpu_ref, 512);
        CHECK(j == 512 && r == 512, "immediate program budget");
        compare_state();
        CHECK(ref_reg(10) == DATA_ADDR + 640u, "all immediate results must be recorded");
        const uint8_t *add_one = bus_ref.ram + (DATA_ADDR - RAM_BASE) + 80u;
        if (seeds[seed] == 0xffffffffu) {
            CHECK(gp32_ld32le(add_one) == 0 &&
                  (gp32_ld32le(add_one + 4) & 0xf0000000u) == 0x60000000u,
                  "immediate addition must retain unsigned carry and zero flags");
        } else if (seeds[seed] == 0x7fffffffu) {
            CHECK(gp32_ld32le(add_one) == 0x80000000u &&
                  (gp32_ld32le(add_one + 4) & 0xf0000000u) == 0x90000000u,
                  "immediate addition must retain signed overflow and negative flags");
        }
        teardown_pair();
    }
}

/* Compare shifted ALU results and flags after each instruction, including
 * the ARM encodings for LSR/ASR #32 and RRX (amount field zero). */
static void case_native_immshift(void) {
    const unsigned amounts[] = {0u, 1u, 31u};
    for (unsigned carry = 0; carry < 2u; ++carry) {
        for (unsigned type = 0; type < 4u; ++type) {
            for (unsigned a = 0; a < GP32_ARRAY_COUNT(amounts); ++a) {
                uint32_t program[161];
                unsigned n = 0;
                current_case = "native-immshift";
                setup_pair();
                set_reg_both(0, 0x7fffffffu);
                set_reg_both(1, 0x80000001u);
                set_reg_both(10, DATA_ADDR);
                set_reg_both(11, carry ? 0xb0000000u : 0x90000000u);
                for (unsigned code = 0; code < 16u; ++code) {
                    for (unsigned flags = 0; flags < 2u; ++flags) {
                        if (code >= 8u && code <= 11u && !flags) continue;
                        program[n++] = 0xe128f00bu; /* MSR CPSR_f,r11 */
                        program[n++] = 0xe0002001u | (code << 21) | (flags << 20) |
                                       (amounts[a] << 7) | (type << 5);
                        program[n++] = 0xe48a2004u; /* STR r2,[r10],#4 */
                        program[n++] = 0xe10f3000u; /* MRS r3,CPSR */
                        program[n++] = 0xe48a3004u; /* STR r3,[r10],#4 */
                    }
                }
                program[n++] = 0xeafffffeu;
                load_both(program, n);
                CHECK(arm920t_run(cpu_jit, 256u) == arm920t_run(cpu_ref, 256u),
                      "shifted ALU instruction budget");
                compare_state();
                teardown_pair();
            }
        }
    }
}

/* Every logical S opcode, every immediate shift encoding, and rotated
 * immediates must agree with the traced interpreter. Record each result,
 * flags and carry consumer before the next operation overwrites them.
 * The DATA helper assertion prevents a passing test through the S-gate. */
static void case_native_sflag_logic(void) {
    const unsigned codes[] = {0u, 1u, 8u, 9u, 12u, 13u, 14u, 15u};
    const uint32_t edges[] = {0u, 1u, 0x80000000u, 0x80000001u,
                              0x7fffffffu, 0xffffffffu, 0xaaaaaaaau, 0x55555555u};
    const unsigned bytes[] = {0u, 1u, 0x80u, 0xffu};
    uint32_t random = 0x1415f1a9u;
    for (unsigned seed = 0; seed < 16u; ++seed) {
        random ^= random << 13; random ^= random >> 17; random ^= random << 5;
        uint32_t operand = seed < GP32_ARRAY_COUNT(edges) ? edges[seed] : random;
        for (unsigned cv = 0; cv < 4u; ++cv) {
            uint32_t program[10000];
            unsigned n = 0, results = 0;
            current_case = "native-sflag-logic";
            setup_pair();
            arm920t_set_trace(cpu_ref, 1, NULL, NULL);
            set_reg_both(0, operand ^ random);
            set_reg_both(1, operand);
            set_reg_both(10, DATA_ADDR);
            set_reg_both(11, 0xc0000000u | (cv << 28)); /* old N/Z, C and V */
            set_reg_both(12, 0u);
            for (unsigned shape = 0; shape < 192u; ++shape) {
                uint32_t op2 = shape < 128u ?
                    1u | ((shape / 32u) << 5) | ((shape % 32u) << 7) :
                    (1u << 25) | (((shape - 128u) / 4u) << 8) | bytes[(shape - 128u) % 4u];
                for (unsigned k = 0; k < GP32_ARRAY_COUNT(codes); ++k) {
                    program[n++] = 0xe128f00bu; /* MSR CPSR_f,r11 */
                    program[n++] = 0xe3a02055u; /* sentinel for TST/TEQ no-write */
                    program[n++] = 0xe0102000u | (codes[k] << 21) | op2;
                    program[n++] = 0xe2ac4000u; /* ADC r4,r12,#0: consume shifter C */
                    program[n++] = 0xe10f3000u; /* MRS r3,CPSR */
                    program[n++] = 0xe8aa001cu; /* STMIA r10!,{r2,r3,r4} */
                    ++results;
                }
            }
            program[n++] = 0xeafffffeu;
            load_both(program, n);
            CHECK(arm920t_run(cpu_jit, 20000u) == arm920t_run(cpu_ref, 20000u),
                  "logical S budget");
            compare_state();
            CHECK(ref_reg(10) == DATA_ADDR + results * 12u, "all logical S results recorded");
            gp32_cpu_profile_t profile;
            arm920t_get_cpu_profile(cpu_jit, &profile);
            if (profile.supported && profile.native_backend) {
                CHECK(profile.native_arm_insns > 0u, "logical S native blocks engaged");
                CHECK(profile.helper_op_kinds[1] == 0u, "logical S must not use DATA helpers");
            }
            teardown_pair();
        }
    }
}

/* Independent C model of the ARM carry/reverse data-processing forms. */
static void carry_arith_model(unsigned code, uint32_t a, uint32_t b, unsigned cin,
                              uint32_t *result, uint32_t *nzcv) {
    uint32_t r;
    if (code == 5u) { /* ADC: a + b + C */
        uint64_t wide = (uint64_t)a + (uint64_t)b + (uint64_t)cin;
        r = (uint32_t)wide;
        *result = r;
        *nzcv = (r & 0x80000000u) | (r == 0u ? 0x40000000u : 0u) |
                ((wide >> 32) ? 0x20000000u : 0u) |
                ((~(a ^ b) & (a ^ r) & 0x80000000u) ? 0x10000000u : 0u);
        return;
    }
    uint32_t minuend, subtrahend;
    unsigned borrow;
    if (code == 6u) { minuend = a; subtrahend = b; borrow = cin ? 0u : 1u; } /* SBC */
    else if (code == 3u) { minuend = b; subtrahend = a; borrow = 0u; }       /* RSB */
    else { minuend = b; subtrahend = a; borrow = cin ? 0u : 1u; }            /* RSC */
    uint64_t diff = (uint64_t)minuend - (uint64_t)subtrahend - (uint64_t)borrow;
    r = (uint32_t)diff;
    *result = r;
    *nzcv = (r & 0x80000000u) | (r == 0u ? 0x40000000u : 0u) |
            ((diff >> 63) ? 0u : 0x20000000u) |
            (((minuend ^ subtrahend) & (minuend ^ r) & 0x80000000u) ? 0x10000000u : 0u);
}

static uint32_t carry_arith_ror32(uint32_t v, unsigned s) {
    s &= 31u;
    return s ? ((v >> s) | (v << (32u - s))) : v;
}

/* Immediate-shift operand2. An immediate LSR/ASR #0 means the 32-bit shift;
 * RRX is deliberately absent because arithmetic RRX still uses the helper. */
static uint32_t carry_arith_shift(unsigned type, unsigned amount, uint32_t v) {
    switch (type) {
    case 0u: return amount ? (amount < 32u ? v << amount : 0u) : v;
    case 1u: return amount ? (amount < 32u ? v >> amount : 0u) : 0u;
    case 2u:
        if (amount == 0u || amount >= 32u) return (v & 0x80000000u) ? 0xffffffffu : 0u;
        return (v & 0x80000000u) ? ((v >> amount) | (0xffffffffu << (32u - amount))) : (v >> amount);
    default: return amount ? carry_arith_ror32(v, amount) : v;
    }
}

/* Register-specified operand2: a zero amount performs no shift for every type,
 * unlike the immediate LSR/ASR #32 encodings above. */
static uint32_t carry_arith_regshift(unsigned type, unsigned amount, uint32_t v) {
    amount &= 0xffu;
    if (amount == 0u) return v;
    switch (type) {
    case 0u: return amount < 32u ? v << amount : 0u;
    case 1u: return amount < 32u ? v >> amount : 0u;
    case 2u:
        if (amount >= 32u) return (v & 0x80000000u) ? 0xffffffffu : 0u;
        return (v & 0x80000000u) ? ((v >> amount) | (0xffffffffu << (32u - amount))) : (v >> amount);
    default: return carry_arith_ror32(v, amount);
    }
}

typedef struct {
    uint32_t *program, *expect_result, *expect_flags;
    unsigned n, results, code, s, cin, rn;
    uint32_t a;
} carry_arith_build_t;

/* MSR CPSR_f seed + tested op + MRS + STMIA.  r5 = 0x20000000 and r11 = 0
 * select the incoming carry, r10 walks the record area, r4 is the destination
 * and r12 the MRS scratch register. */
static void carry_arith_emit(carry_arith_build_t *b, uint32_t op2, uint32_t operand2) {
    uint32_t nzcv;
    carry_arith_model(b->code, b->a, operand2, b->cin, &b->expect_result[b->results], &nzcv);
    b->program[b->n++] = b->cin ? 0xe128f005u : 0xe128f00bu; /* MSR CPSR_f,r5/r11 */
    b->program[b->n++] = 0xe0000000u | (b->code << 21) | (b->s << 20) |
                         (b->rn << 16) | (4u << 12) | op2;
    b->program[b->n++] = 0xe10fc000u;  /* MRS r12, CPSR */
    b->program[b->n++] = 0xe8aa1010u;  /* STMIA r10!, {r4, r12} */
    b->expect_flags[b->results] = b->s ? nzcv : (b->cin ? 0x20000000u : 0u);
    ++b->results;
}

/* Seeded ADC/SBC/RSB/RSC.  Every opcode crosses both operands' boundary values
 * (0, 0x7fffffff, 0x80000000, 0xffffffff), both carry-in states and the
 * register, immediate, immediate-shift and register-shift operand forms; the S=0
 * pass covers the register and immediate encodings as well.  Each case records
 * its result and committed NZCV, checked against the C model above.  The x64
 * emitter routed every S=1 form to the classified helper, so the DATA helper
 * counter must stay zero on both native backends. */
static void case_native_carry_arith(void) {
    const uint32_t edges[4] = {0u, 0x7fffffffu, 0x80000000u, 0xffffffffu};
    const unsigned codes[4] = {3u, 5u, 6u, 7u};                     /* RSB ADC SBC RSC */
    const uint32_t imm_ops[4] = {0u, 1u, 0xffu, (4u << 8) | 0xffu}; /* #0 #1 #0xff #0xff000000 */
    const uint32_t imm_vals[4] = {0u, 1u, 0xffu, 0xff000000u};
    const struct { unsigned type, amount; } shifts[6] = {
        {0u, 0u}, {0u, 31u}, {1u, 0u}, {1u, 5u}, {2u, 4u}, {3u, 7u}
    };
    const uint32_t amounts[4] = {0u, 1u, 31u, 33u}; /* seeded into r6..r9 */
    /* The BIOS code window holds 16128 instructions: 3328 cases at 4 each. */
    static uint32_t program[13400];
    static uint32_t expect_result[3400], expect_flags[3400];
    carry_arith_build_t build = {program, expect_result, expect_flags, 0u, 0u, 0u, 0u, 0u, 0u, 0u};
    current_case = "native-carry-arith";
    setup_pair();
    arm920t_set_trace(cpu_ref, 1, NULL, NULL);
    set_reg_both(0, edges[0]);
    set_reg_both(1, edges[1]);
    set_reg_both(2, edges[2]);
    set_reg_both(3, edges[3]);
    set_reg_both(5, 0x20000000u); /* C=1 MSR CPSR_f seed */
    set_reg_both(11, 0u);         /* C=0 MSR CPSR_f seed */
    set_reg_both(6, amounts[0]);
    set_reg_both(7, amounts[1]);
    set_reg_both(8, amounts[2]);
    set_reg_both(9, amounts[3]);
    set_reg_both(10, DATA_ADDR);
    for (unsigned cv = 0; cv < 2u; ++cv)
        for (unsigned k = 0; k < GP32_ARRAY_COUNT(codes); ++k) {
            build.cin = cv;
            build.code = codes[k];
            for (unsigned rn = 0; rn < 4u; ++rn) {
                build.rn = rn;
                build.a = edges[rn];
                for (unsigned sv = 1u; ; --sv) {   /* S=1 first, then S=0 */
                    build.s = sv;
                    for (unsigned rmi = 0; rmi < 4u; ++rmi)
                        carry_arith_emit(&build, rmi, edges[rmi]);
                    for (unsigned ii = 0; ii < 4u; ++ii)
                        carry_arith_emit(&build, (1u << 25) | imm_ops[ii], imm_vals[ii]);
                    if (!sv) break;
                }
                for (unsigned si = 0; si < GP32_ARRAY_COUNT(shifts); ++si)
                    for (unsigned rmi = 0; rmi < 4u; ++rmi)
                        carry_arith_emit(&build, (shifts[si].amount << 7) |
                                         (shifts[si].type << 5) | rmi,
                                         carry_arith_shift(shifts[si].type, shifts[si].amount, edges[rmi]));
                for (unsigned type = 0; type < 4u; ++type)
                    for (unsigned ai = 0; ai < GP32_ARRAY_COUNT(amounts); ++ai)
                        for (unsigned rmi = 0; rmi < 4u; ++rmi)
                            carry_arith_emit(&build, (1u << 4) | ((6u + ai) << 8) |
                                             (type << 5) | rmi,
                                             carry_arith_regshift(type, amounts[ai], edges[rmi]));
            }
        }
    program[build.n++] = 0xeafffffeu;
    load_both(program, build.n);
    CHECK(arm920t_run(cpu_jit, 100000u) == arm920t_run(cpu_ref, 100000u),
          "carry arithmetic budget");
    compare_state();
    CHECK(ref_reg(10) == DATA_ADDR + build.results * 8u, "every carry arithmetic result recorded");
    for (unsigned i = 0; i < build.results; ++i) {
        const uint8_t *slot = bus_ref.ram + (DATA_ADDR - RAM_BASE) + i * 8u;
        uint32_t got = gp32_ld32le(slot);
        if (got != expect_result[i]) { report("carry arithmetic result", got, expect_result[i]); break; }
        got = gp32_ld32le(slot + 4u) & 0xf0000000u;
        if (got != expect_flags[i]) { report("carry arithmetic NZCV", got, expect_flags[i]); break; }
    }
    gp32_cpu_profile_t profile;
    arm920t_get_cpu_profile(cpu_jit, &profile);
    if (profile.supported && profile.native_backend) {
        CHECK(profile.native_arm_insns > 0u, "carry arithmetic native blocks engaged");
        CHECK(profile.helper_op_kinds[1] == 0u, "ADC/SBC/RSB/RSC must not use the DATA helper");
    }
    teardown_pair();
}

/* Conditions immediately after arithmetic can reuse native NZCV. A logical
 * flag update, RAM range guard, helper or skipped predicated producer must
 * not accidentally reuse a different set of host flags. Store each decision
 * so later instructions cannot hide a wrong conditional execution. */
static void case_native_condition_flags(void) {
    const uint32_t operands[][2] = {
        {0u, 0u}, {0u, 1u}, {1u, 0u},
        {0x80000000u, 1u}, {0x7fffffffu, 0xffffffffu}, {0xffffffffu, 0u}
    };
    const uint32_t between[] = {
        0u, 0xe1100000u, /* TST r0,r0: preserves guest C/V */
        0xe59a4000u,     /* LDR r4,[r10]: host range-check flags */
        0x00904001u,     /* ADDSEQ r4,r0,r1: taken/skipped join */
        0xe10a4091u      /* SWP r4,r1,[r10]: classified helper */
    };
    for (unsigned v = 0; v < GP32_ARRAY_COUNT(operands); ++v) {
        for (unsigned path = 0; path < GP32_ARRAY_COUNT(between); ++path) {
            uint32_t program[127];
            unsigned n = 0;
            current_case = "native-condition-flags";
            setup_pair();
            set_reg_both(0, operands[v][0]);
            set_reg_both(1, operands[v][1]);
            set_reg_both(10, DATA_ADDR);
            set_reg_both(11, DATA_ADDR + 0x100u);
            for (unsigned cond = 0; cond < 14u; ++cond) {
                program[n++] = 0xe3a02000u; /* MOV r2,#0 */
                program[n++] = 0xe3a03000u; /* MOV r3,#0 */
                program[n++] = 0xe1500001u; /* CMP r0,r1 */
                if (between[path]) program[n++] = between[path];
                program[n++] = (cond << 28) | 0x03a02001u;
                program[n++] = ((cond ^ 1u) << 28) | 0x03a03001u;
                program[n++] = 0xe48b2004u; /* STR r2,[r11],#4 */
                program[n++] = 0xe48b3004u; /* STR r3,[r11],#4 */
            }
            program[n++] = 0xeafffffeu;
            load_both(program, n);
            CHECK(arm920t_run(cpu_jit, 256u) == arm920t_run(cpu_ref, 256u),
                  "condition flag instruction budget");
            compare_state();
            CHECK(ref_reg(2) + ref_reg(3) == 1u, "inverse conditions must partition");
            teardown_pair();
        }
    }
}

static void case_native_regshift(void) {
    const unsigned amounts[] = {0, 1, 31, 32, 33, 255, 256};
    for (unsigned carry = 0; carry != 2; ++carry) {
        for (unsigned a = 0; a < GP32_ARRAY_COUNT(amounts); ++a) {
            uint32_t program[16];
            unsigned n = 0;
            current_case = "native-regshift";
            setup_pair();
            set_reg_both(0, 0x7fffffffu);
            set_reg_both(1, 0x80000001u);
            set_reg_both(2, amounts[a]);
            set_reg_both(11, carry ? 0x20000000u : 0u);
            for (unsigned type = 0; type < 4; ++type) {
                program[n++] = 0xe128f00bu; /* MSR CPSR_f, r11 */
                program[n++] = 0xe1b00211u | ((3u + 2u * type) << 12) | (type << 5);
                program[n++] = 0xe10f0000u | ((4u + 2u * type) << 12); /* MRS */
            }
            program[n++] = 0xe0b0c231u; /* ADCS r12,r0,r1,LSR r2 */
            program[n++] = 0xe10fd000u; /* MRS r13,CPSR */
            program[n++] = 0xe1100271u; /* TST r0,r1,ROR r2 */
            program[n++] = 0xeafffffeu;
            load_both(program, n);
            run_native_case();
            if (amounts[a] == 32u) {
                CHECK(ref_reg(3) == 0 && ref_reg(5) == 0 &&
                      ref_reg(7) == 0xffffffffu && ref_reg(9) == 0x80000001u,
                      "32-bit shift boundary values");
            }
            teardown_pair();
        }
    }
}

static void set_cpsr_both(uint32_t v); /* defined with the SPSR bank cases */

/* S=0 data-processing writes of PC are computed jumps.  They must commit
 * write_r(15) alignment, leave the ALU flags alone and end the native
 * block, on both native backends.  A64 already inlined the shape; this
 * pins the x64 route that mirrors it (BIOS dispatch tables, then the
 * shifted-register, register-specified-shift and immediate forms). */
static void case_native_data_pc(void) {
    /* LS = C==0 or Z==1: 0x000000d3 takes the jump, 0x200000d3 (C=1,Z=0)
     * falls through and proves the skipped condition still retires PC+4. */
    const uint32_t table[] = {
        0x908ff100u, /* ADDLS pc,pc,r0,LSL #2: r0=2 -> pc+8+8 = +0x10 */
        0xe3a03011u, /* MOV r3,#0x11: fall-through path only */
        0xe3a03022u, /* MOV r3,#0x22 */
        0xe3a03033u, /* MOV r3,#0x33 */
        0xe3a05077u, /* MOV r5,#0x77: taken landing (+0x10) */
        0xe3a06066u, /* MOV r6,#0x66 */
        0xeafffffeu
    };
    for (unsigned taken = 0; taken < 2u; ++taken) {
        current_case = taken ? "data-pc-jump-taken" : "data-pc-jump-skipped";
        setup_pair();
        set_cpsr_both(taken ? 0x000000d3u : 0x200000d3u);
        set_reg_both(0u, 2u);
        load_both(table, GP32_ARRAY_COUNT(table));
        run_native_case();
        CHECK(ref_reg(3u) == (taken ? 0u : 0x33u), "computed jump writes PC only");
        CHECK(ref_reg(5u) == 0x77u && ref_reg(6u) == 0x66u, "computed jump landing path");
        CHECK(arm920t_get_pc(cpu_ref) == CODE_ADDR + 0x18u, "computed jump trace tail");
        CHECK((arm920t_get_cpsr(cpu_ref) & 0xf0000000u) ==
              (taken ? 0x00000000u : 0x20000000u), "computed jump preserves ALU flags");
        gp32_cpu_profile_t profile;
        arm920t_get_cpu_profile(cpu_jit, &profile);
        if (profile.supported && profile.native_backend) {
            CHECK(profile.helper_op_kinds[1] == 0u,
                  "computed jump must not use the classified DATA helper");
            CHECK(profile.native_arm_insns > 0u, "computed jump must run natively");
        }
        teardown_pair();
    }

    const uint32_t shifted[] = {
        0xe1a0f121u, /* MOV pc,r1,LSR #2: r1=0x1040 -> pc+0x10 */
        0xe3a03011u,
        0xe3a03022u,
        0xe3a03033u,
        0xe3a05077u, /* landing */
        0xeafffffeu
    };
    current_case = "data-pc-shifted";
    setup_pair();
    set_cpsr_both(0xa00000d3u); /* N=1,C=1 must survive the PC write */
    set_reg_both(1u, 0x1040u);
    load_both(shifted, GP32_ARRAY_COUNT(shifted));
    run_native_case();
    CHECK(ref_reg(3u) == 0u && ref_reg(5u) == 0x77u, "shifted computed jump");
    CHECK((arm920t_get_cpsr(cpu_ref) & 0xf0000000u) == 0xa0000000u,
          "S=0 PC write must not change ALU flags");
    teardown_pair();

    const uint32_t regshift[] = {
        0xe08ff130u, /* ADD pc,pc,r0,LSR r1: r0=0x40, r1=3 -> pc+8+8 */
        0xe3a03011u,
        0xe3a03022u,
        0xe3a03033u,
        0xe3a05077u, /* landing */
        0xeafffffeu
    };
    current_case = "data-pc-regshift";
    setup_pair();
    set_cpsr_both(0x000000d3u);
    set_reg_both(0u, 0x40u);
    set_reg_both(1u, 3u);
    load_both(regshift, GP32_ARRAY_COUNT(regshift));
    run_native_case();
    CHECK(ref_reg(3u) == 0u && ref_reg(5u) == 0x77u, "register-shift computed jump");
    teardown_pair();

    const uint32_t immediate[] = {
        0xe3a0fe41u, /* MOV pc,#0x410: immediate operand, ror #28 */
        0xe3a03011u,
        0xe3a03022u,
        0xe3a03033u,
        0xe3a05077u, /* landing (+0x10) */
        0xeafffffeu
    };
    current_case = "data-pc-immediate";
    setup_pair();
    set_cpsr_both(0x000000d3u);
    load_both(immediate, GP32_ARRAY_COUNT(immediate));
    run_native_case();
    CHECK(ref_reg(3u) == 0u && ref_reg(5u) == 0x77u, "immediate computed jump");
    teardown_pair();
}

static void case_native_longmul_psr(void) {
    const uint32_t program[] = {
        0xe168f001u, /* MSR SPSR_f,r1 */
        0xe14f3000u, /* MRS r3,SPSR: must not read CPSR */
        0xe128f002u, /* MSR CPSR_f,r2: clear all upper eight bits */
        0xe10f4000u,
        0xee106f10u, /* MRC p15,0,r6,c0,c0,0 */
        0xee117f10u, /* MRC p15,0,r7,c1,c0,0 */
        0xeafffffeu
    };
    current_case = "native-psr";
    setup_pair();
    arm920t_set_cpsr(cpu_jit, 0xaf0000d3u);
    arm920t_set_cpsr(cpu_ref, 0xaf0000d3u);
    set_reg_both(1, 0x20000000u);
    set_reg_both(2, 0u);
    load_both(program, GP32_ARRAY_COUNT(program));
    run_native_case();
    CHECK((ref_reg(3) & 0xff000000u) == 0x20000000u, "SPSR flags");
    CHECK(ref_reg(4) == 0xd3u, "CPSR_f replacement preserves low fields");
    CHECK(ref_reg(6) == 0x41129200u, "MRC main ID");
    teardown_pair();

    const uint32_t mul[] = {
        0xe0943291u, /* UMULLS r3,r4,r1,r2 */
        0xe0b43291u, /* UMLALS r3,r4,r1,r2 */
        0xe0d65291u, /* SMULLS r5,r6,r1,r2 */
        0xe0f65291u, /* SMLALS r5,r6,r1,r2 */
        0xe0b81291u, /* UMLALS r1,r8,r1,r2: source/destination alias */
        0xe0da9291u, /* SMULLS r9,r10,r1,r2: N from the high word */
        0xeafffffeu
    };
    current_case = "native-longmul";
    setup_pair();
    arm920t_set_cpsr(cpu_jit, arm920t_get_cpsr(cpu_jit) | 0x30000000u);
    arm920t_set_cpsr(cpu_ref, arm920t_get_cpsr(cpu_ref) | 0x30000000u);
    set_reg_both(1, 0xffffffffu);
    set_reg_both(2, 2u);
    set_reg_both(8, 7u);
    load_both(mul, GP32_ARRAY_COUNT(mul));
    run_native_case();
    CHECK(ref_reg(3) == 0xfffffffcu && ref_reg(4) == 3u, "unsigned accumulate");
    CHECK(ref_reg(5) == 0xfffffffcu && ref_reg(6) == 0xffffffffu, "signed accumulate");
    CHECK(ref_reg(1) == 0xfffffffdu && ref_reg(8) == 9u, "multiply input alias");
    CHECK(ref_reg(9) == 0xfffffffau && ref_reg(10) == 0xffffffffu, "signed high-word sign");
    CHECK((arm920t_get_cpsr(cpu_ref) & 0xf0000000u) == 0xb0000000u,
          "long multiply flags set N/Z and preserve C/V");
    gp32_cpu_profile_t profile;
    arm920t_get_cpu_profile(cpu_jit, &profile);
    if (profile.supported && profile.native_backend) {
        CHECK(profile.helper_op_kinds[3u] == 0u,
              "long multiply forms must stay out of the classified MUL helper");
        CHECK(profile.native_arm_insns > 0u, "long multiply forms must execute natively");
    }
    teardown_pair();
}

/* Banked SPSR coverage.  No byte pattern repeats between the seeds, so a
 * read or write that lands on the wrong bank cannot match the expectation.
 * arm920t_set_register_context seeds every bank at once without switching
 * modes, keeping the full-CPSR set that follows independent of the seed. */
#define SPSR_FIQ_SEED 0xf1a2b3c4u
#define SPSR_IRQ_SEED 0x91d2e3f4u
#define SPSR_SVC_SEED 0x13243546u
#define SPSR_ABT_SEED 0xabcdef01u
#define SPSR_UND_SEED 0x2468ace0u

#define SPSR_MODE_USR 0x10u
#define SPSR_MODE_FIQ 0x11u
#define SPSR_MODE_IRQ 0x12u
#define SPSR_MODE_SVC 0x13u
#define SPSR_MODE_ABT 0x17u
#define SPSR_MODE_UND 0x1bu
#define SPSR_MODE_SYS 0x1fu
#define SPSR_MODE_UNUSED 0x1cu /* unused encoding, owns no SPSR bank */

static uint32_t spsr_seed(unsigned m) {
    switch (m) {
    case SPSR_MODE_FIQ: return SPSR_FIQ_SEED;
    case SPSR_MODE_IRQ: return SPSR_IRQ_SEED;
    case SPSR_MODE_SVC: return SPSR_SVC_SEED;
    case SPSR_MODE_ABT: return SPSR_ABT_SEED;
    case SPSR_MODE_UND: return SPSR_UND_SEED;
    default: return 0u;
    }
}
static uint32_t spsr_bank(const arm920t_t *cpu, unsigned m) {
    arm920t_register_context_t c;
    arm920t_get_register_context(cpu, &c);
    switch (m) {
    case SPSR_MODE_FIQ: return c.spsr_fiq;
    case SPSR_MODE_IRQ: return c.spsr_irq;
    case SPSR_MODE_SVC: return c.spsr_svc;
    case SPSR_MODE_ABT: return c.spsr_abt;
    case SPSR_MODE_UND: return c.spsr_und;
    default: return 0u;
    }
}
static void seed_spsr_banks(void) {
    arm920t_t *pair[2] = {cpu_jit, cpu_ref};
    for (unsigned i = 0; i < 2u; ++i) {
        arm920t_register_context_t c;
        arm920t_get_register_context(pair[i], &c);
        c.spsr_fiq = SPSR_FIQ_SEED; c.spsr_irq = SPSR_IRQ_SEED; c.spsr_svc = SPSR_SVC_SEED;
        c.spsr_abt = SPSR_ABT_SEED; c.spsr_und = SPSR_UND_SEED;
        arm920t_set_register_context(pair[i], &c);
    }
}
static void set_cpsr_both(uint32_t v) { arm920t_set_cpsr(cpu_jit, v); arm920t_set_cpsr(cpu_ref, v); }
static void set_pc_both(uint32_t v) { arm920t_set_reg(cpu_jit, 15u, v); arm920t_set_reg(cpu_ref, 15u, v); }
/* Every bank except keep_mode must still hold its seed.  Pass a value that is
 * not a banked mode to require all five. */
static void check_bank_seeds(unsigned keep_mode) {
    static const unsigned modes[5] = {SPSR_MODE_FIQ, SPSR_MODE_IRQ, SPSR_MODE_SVC, SPSR_MODE_ABT, SPSR_MODE_UND};
    for (unsigned i = 0; i < 5u; ++i)
        if (modes[i] != keep_mode)
            CHECK(spsr_bank(cpu_ref, modes[i]) == spsr_seed(modes[i]), "untouched SPSR bank changed");
}
static void compare_spsr_banks(void) {
    static const unsigned modes[5] = {SPSR_MODE_FIQ, SPSR_MODE_IRQ, SPSR_MODE_SVC, SPSR_MODE_ABT, SPSR_MODE_UND};
    for (unsigned i = 0; i < 5u; ++i) {
        uint32_t j = spsr_bank(cpu_jit, modes[i]), r = spsr_bank(cpu_ref, modes[i]);
        if (j != r) { char nm[16]; snprintf(nm, sizeof(nm), "spsr_%02x", modes[i]); report(nm, j, r); }
    }
}

/* MSR SPSR_f writes only the live mode's bank, MRS SPSR reads it back and
 * CPSR keeps running normally.  Iterating the five banked modes proves the
 * bank follows the current cpsr rather than a translation-time mode. */
static void case_native_spsr_mode_banks(void) {
    static const unsigned modes[5] = {SPSR_MODE_FIQ, SPSR_MODE_SVC, SPSR_MODE_ABT, SPSR_MODE_IRQ, SPSR_MODE_UND};
    const uint32_t program[] = {
        0xe168f001u, /* MSR SPSR_f,r1 */
        0xe14f2000u, /* MRS r2,SPSR */
        0xe10f3000u, /* MRS r3,CPSR */
        0xeafffffeu
    };
    for (unsigned i = 0; i < 5u; ++i) {
        uint32_t cpsr = 0x600000c0u | modes[i];
        uint32_t expect = (spsr_seed(modes[i]) & 0x00ffffffu) | 0x5a000000u;
        current_case = "native-spsr-bank";
        setup_pair();
        seed_spsr_banks();
        set_cpsr_both(cpsr);
        set_reg_both(1, 0x5a000000u);
        load_both(program, GP32_ARRAY_COUNT(program));
        run_native_case();
        CHECK(ref_reg(2) == expect, "MRS SPSR read the current mode bank");
        CHECK(ref_reg(3) == cpsr, "SPSR write left CPSR alone");
        CHECK(spsr_bank(cpu_ref, modes[i]) == expect, "MSR SPSR_f wrote the current mode bank");
        CHECK(ref_reg(1) == 0x5a000000u, "MSR source register unchanged");
        check_bank_seeds(modes[i]);
        compare_spsr_banks();
        teardown_pair();
    }
}

/* USR/SYS and an unused encoding own no SPSR bank: MRS SPSR falls back to
 * CPSR and MSR SPSR is ignored, with every bank left at its seed. */
static void case_native_spsr_no_bank(void) {
    static const unsigned modes[3] = {SPSR_MODE_USR, SPSR_MODE_SYS, SPSR_MODE_UNUSED};
    const uint32_t program[] = {
        0xe168f001u, /* MSR SPSR_f,r1: ignored without a bank */
        0xe14f2000u, /* MRS r2,SPSR: CPSR fallback */
        0xe10f3000u, /* MRS r3,CPSR */
        0xeafffffeu
    };
    for (unsigned i = 0; i < 3u; ++i) {
        uint32_t cpsr = 0x600000c0u | modes[i];
        current_case = "native-spsr-nobank";
        setup_pair();
        seed_spsr_banks();
        set_cpsr_both(cpsr);
        set_reg_both(1, 0x5a000000u);
        load_both(program, GP32_ARRAY_COUNT(program));
        run_native_case();
        CHECK(ref_reg(2) == ref_reg(3), "MRS SPSR without a bank reads CPSR");
        CHECK(ref_reg(3) == cpsr, "CPSR fallback value");
        CHECK(arm920t_get_cpsr(cpu_ref) == cpsr, "ignored SPSR write changed no CPSR bit");
        check_bank_seeds(0u);
        compare_spsr_banks();
        teardown_pair();
    }
}

/* MSR SPSR field masks: each nibble bit selects one byte, in register and
 * immediate form, with the existing zero-field NZCV-nibble (0xf0000000)
 * fallback.  The last row reads the pipeline PC (address + 8) as its source. */
static void case_native_spsr_fields(void) {
    static const struct {
        const char *name;
        uint32_t msr;
        unsigned rm;      /* 16 = no source register (immediate / PC) */
        uint32_t rm_value;
        uint32_t expect;  /* SPSR_SVC after the write */
    } rows[] = {
        {"native-spsr-c",      0xe161f001u, 1u,  0x000000a5u, 0x132435a5u},
        {"native-spsr-x",      0xe162f002u, 2u,  0x0000b700u, 0x1324b746u},
        {"native-spsr-s",      0xe164f003u, 3u,  0x00c90000u, 0x13c93546u},
        {"native-spsr-f",      0xe168f004u, 4u,  0xdb000000u, 0xdb243546u},
        {"native-spsr-cx",     0xe163f005u, 5u,  0x0000e11fu, 0x1324e11fu},
        {"native-spsr-zero",   0xe160f006u, 6u,  0xf2abcdefu, 0xf3243546u},
        {"native-spsr-imm-f",  0xe368f4f0u, 16u, 0u,          0xf0243546u},
        {"native-spsr-imm-c",  0xe361f0ffu, 16u, 0u,          0x132435ffu},
        {"native-spsr-imm-zero", 0xe360f0aau, 16u, 0u,        0x03243546u},
        {"native-spsr-pc",     0xe16ff00fu, 16u, 0u,          CODE_ADDR + 8u},
    };
    for (unsigned i = 0; i < GP32_ARRAY_COUNT(rows); ++i) {
        const uint32_t program[] = {
            rows[i].msr,
            0xe14f9000u, /* MRS r9,SPSR: read the bank back */
            0xeafffffeu
        };
        current_case = rows[i].name;
        setup_pair();
        seed_spsr_banks();
        set_cpsr_both(0x600000c0u | SPSR_MODE_SVC);
        if (rows[i].rm < 16u) set_reg_both(rows[i].rm, rows[i].rm_value);
        load_both(program, GP32_ARRAY_COUNT(program));
        run_native_case();
        CHECK(spsr_bank(cpu_ref, SPSR_MODE_SVC) == rows[i].expect, "MSR SPSR field mask");
        CHECK(ref_reg(9) == rows[i].expect, "MRS SPSR round trip");
        if (rows[i].rm < 16u) CHECK(ref_reg(rows[i].rm) == rows[i].rm_value, "MSR source register unchanged");
        check_bank_seeds(SPSR_MODE_SVC);
        compare_spsr_banks();
        teardown_pair();
    }
}

/* Conditional MRS/MSR SPSR: a skipped execution leaves the bank untouched,
 * a taken one applies the read or write. */
static void case_native_spsr_conditions(void) {
    static const struct {
        const char *name;
        uint32_t flags;        /* source of MSR CPSR_f,r7 */
        uint32_t expect_bank;
        uint32_t expect_r5;
    } rows[] = {
        {"native-spsr-cond-skip", 0x00000000u, 0x13243546u, 0x13243546u},
        {"native-spsr-cond-take", 0x40000000u, 0x77243546u, 0xfeedf00du},
    };
    const uint32_t program[] = {
        0xe128f007u, /* MSR CPSR_f,r7 */
        0x0168f006u, /* MSREQ SPSR_f,r6 */
        0xc14f5000u, /* MRSGT r5,SPSR */
        0xe10f8000u, /* MRS r8,CPSR */
        0xeafffffeu
    };
    for (unsigned i = 0; i < GP32_ARRAY_COUNT(rows); ++i) {
        current_case = rows[i].name;
        setup_pair();
        seed_spsr_banks();
        set_cpsr_both(0x600000c0u | SPSR_MODE_SVC);
        set_reg_both(5, 0xfeedf00du);
        set_reg_both(6, 0x77000000u);
        set_reg_both(7, rows[i].flags);
        load_both(program, GP32_ARRAY_COUNT(program));
        run_native_case();
        CHECK(spsr_bank(cpu_ref, SPSR_MODE_SVC) == rows[i].expect_bank, "conditional MSR SPSR");
        CHECK(ref_reg(5) == rows[i].expect_r5, "conditional MRS SPSR");
        CHECK(ref_reg(8) == arm920t_get_cpsr(cpu_ref), "conditional case MRS CPSR");
        check_bank_seeds(SPSR_MODE_SVC);
        compare_spsr_banks();
        teardown_pair();
    }
}

/* A translated block must resolve the SPSR bank from the live cpsr on every
 * execution: run it in SVC, switch to FIQ through arm920t_set_cpsr (which
 * does not flush the cache), rewind PC and run the same block again. */
static void case_native_spsr_bank_reuse(void) {
    const uint32_t program[] = {
        0xe168f001u, /* MSR SPSR_f,r1 */
        0xe14f2000u, /* MRS r2,SPSR */
        0xe1a03002u, /* MOV r3,r2 */
        0xeafffffeu
    };
    current_case = "native-spsr-reuse";
    setup_pair();
    seed_spsr_banks();
    set_cpsr_both(0x600000c0u | SPSR_MODE_SVC);
    set_reg_both(1, 0x5a000000u);
    load_both(program, GP32_ARRAY_COUNT(program));
    uint64_t misses_before = arm920t_get_jit_misses(cpu_jit);
    uint64_t hits_before = arm920t_get_jit_hits(cpu_jit);
    run_native_case();
    uint32_t svc_after = spsr_bank(cpu_ref, SPSR_MODE_SVC);
    CHECK(svc_after == ((SPSR_SVC_SEED & 0x00ffffffu) | 0x5a000000u), "SVC bank write");
    CHECK(ref_reg(2) == svc_after && ref_reg(3) == svc_after, "SVC bank read");
    uint64_t misses_mid = arm920t_get_jit_misses(cpu_jit);
    CHECK(misses_mid > misses_before && arm920t_get_jit_hits(cpu_jit) > hits_before,
          "first run translated and executed the block");

    set_cpsr_both(0x600000c0u | SPSR_MODE_FIQ);
    set_pc_both(CODE_ADDR);
    set_reg_both(1, 0x3c000000u);
    hits_before = arm920t_get_jit_hits(cpu_jit);
    CHECK(arm920t_run(cpu_jit, 64u) == arm920t_run(cpu_ref, 64u), "reuse run budget");
    compare_state();
    CHECK(arm920t_get_jit_misses(cpu_jit) == misses_mid, "mode change kept the translated block");
    CHECK(arm920t_get_jit_hits(cpu_jit) > hits_before, "second run reused the translated block");
    CHECK(spsr_bank(cpu_ref, SPSR_MODE_FIQ) == ((SPSR_FIQ_SEED & 0x00ffffffu) | 0x3c000000u),
          "reused block wrote the FIQ bank");
    CHECK(spsr_bank(cpu_ref, SPSR_MODE_SVC) == svc_after, "SVC bank kept its first-run write");
    CHECK(ref_reg(2) == spsr_bank(cpu_ref, SPSR_MODE_FIQ), "reused block read the FIQ bank");
    CHECK(arm920t_get_pc(cpu_ref) == CODE_ADDR + 12u, "reuse run executed the whole block");
    compare_spsr_banks();
    teardown_pair();
}

/* The native SPSR write must be visible to the MOVS PC,LR exception return,
 * which restores CPSR and switches mode from the current bank. */
static void case_native_spsr_exception_return(void) {
    uint32_t program[20];
    const uint32_t target = CODE_ADDR + 0x40u;
    for (unsigned i = 0; i < GP32_ARRAY_COUNT(program); ++i) program[i] = 0xeafffffeu;
    program[0] = 0xe16ff001u;  /* MSR SPSR_cxsf,r1: full CPSR image */
    program[1] = 0xe1b0f00eu;  /* MOVS PC,LR */
    program[16] = 0xe10f4000u; /* MRS r4,CPSR after the return */
    current_case = "native-spsr-return";
    setup_pair();
    seed_spsr_banks();
    set_cpsr_both(0x600000c0u | SPSR_MODE_SVC);
    set_reg_both(1, 0x00000010u); /* return to USR mode, IRQ/FIQ enabled */
    set_reg_both(14, target);
    load_both(program, GP32_ARRAY_COUNT(program));
    run_native_case();
    CHECK(arm920t_get_cpsr(cpu_ref) == 0x00000010u, "SPSR image became the new CPSR");
    CHECK(ref_reg(4) == 0x00000010u, "returned code runs in the restored mode");
    CHECK(arm920t_get_pc(cpu_ref) == target + 4u, "returned to LR and parked on B self");
    compare_spsr_banks();
    teardown_pair();
}

/* Retire just the return, before executing the target. This distinguishes
 * restored Thumb alignment and old-bank LR reads from an ordinary PC write. */
static void case_native_exception_return(void) {
    static const struct {
        const char *name;
        uint32_t insn, cpsr, saved, lr, pc, result_cpsr;
    } rows[] = {
        {"return-svc-arm", 0xe1b0f00eu, 0x600000d3u, 0x90000010u,
         CODE_ADDR + 0x43u, CODE_ADDR + 0x40u, 0x90000010u},
        {"return-irq-thumb", 0xe25ef004u, 0x600000d2u, 0xa000003fu,
         CODE_ADDR + 0x47u, CODE_ADDR + 0x42u, 0xa000003fu},
        {"return-fiq-svc", 0xe25ef004u, 0x600000d1u, 0x30000013u,
         CODE_ADDR + 0x47u, CODE_ADDR + 0x40u, 0x30000013u},
        {"return-usr-flags", 0xe1b0f00eu, 0xf00000d0u, 0u,
         CODE_ADDR + 0x43u, CODE_ADDR + 0x40u, 0x300000d0u},
        {"return-sys-flags", 0xe25ef004u, 0xf00000dfu, 0u,
         CODE_ADDR + 0x47u, CODE_ADDR + 0x40u, 0x200000dfu},
        {"return-condition-skipped", 0x01b0f00eu, 0x200000d3u, 0x90000030u,
         CODE_ADDR + 0x43u, CODE_ADDR + 4u, 0x200000d3u},
        {"return-fiq-regshift", 0xe1b0f218u, 0x600000d1u, 0x90000030u,
         0u, CODE_ADDR + 0x42u, 0x90000030u},
    };
    for (unsigned i = 0; i < GP32_ARRAY_COUNT(rows); ++i) {
        current_case = rows[i].name;
        setup_pair();
        seed_spsr_banks();
        set_cpsr_both(rows[i].cpsr);
        set_reg_both(14u, rows[i].lr);
        set_reg_both(8u, (CODE_ADDR + 0x42u) / 2u);
        set_reg_both(2u, 1u);
        arm920t_t *pair[] = {cpu_jit, cpu_ref};
        for (unsigned p = 0; p < 2u; ++p) {
            arm920t_register_context_t ctx;
            arm920t_get_register_context(pair[p], &ctx);
            switch (rows[i].cpsr & 31u) {
            case SPSR_MODE_SVC: ctx.spsr_svc = rows[i].saved; break;
            case SPSR_MODE_IRQ: ctx.spsr_irq = rows[i].saved; break;
            case SPSR_MODE_FIQ: ctx.spsr_fiq = rows[i].saved; break;
            }
            arm920t_set_register_context(pair[p], &ctx);
        }
        const uint32_t program[] = {rows[i].insn, 0xe1a0f00fu};
        load_both(program, GP32_ARRAY_COUNT(program));
        CHECK(arm920t_run(cpu_jit, 1u) == 1u, "native return exact budget");
        CHECK(arm920t_run(cpu_ref, 1u) == 1u, "reference return exact budget");
        compare_state();
        compare_spsr_banks();
        arm920t_register_context_t actual = {0}, expected = {0};
        arm920t_get_register_context(cpu_jit, &actual);
        arm920t_get_register_context(cpu_ref, &expected);
        CHECK(!memcmp(&actual, &expected, sizeof(actual)), "return preserves every register bank");
        CHECK(arm920t_get_pc(cpu_ref) == rows[i].pc, "return target/alignment");
        CHECK(arm920t_get_cpsr(cpu_ref) == rows[i].result_cpsr, "restored or bankless ALU flags");
        gp32_cpu_profile_t prof;
        arm920t_get_cpu_profile(cpu_jit, &prof);
        if (prof.supported && prof.native_backend) {
            CHECK(prof.native_block_calls == 1u && prof.native_arm_insns == 1u,
                  "return used native block");
            if (rows[i].insn != 0xe1b0f218u || prof.native_backend == 2u)
                CHECK(prof.helper_op_kinds[1] == 0u, "return avoids classified DATA helper");
        }
        if (i == 0u) {
            /* Restoring IRQ enable must expose a pending interrupt before
             * the first target instruction, even across the run boundary. */
            arm920t_set_irq(cpu_jit, 1);
            arm920t_set_irq(cpu_ref, 1);
            CHECK(arm920t_run(cpu_jit, 1u) == 1u, "return IRQ native budget");
            CHECK(arm920t_run(cpu_ref, 1u) == 1u, "return IRQ reference budget");
            compare_state();
            compare_spsr_banks();
            CHECK(arm920t_get_pc(cpu_ref) == 0x18u, "IRQ taken before return target");
        }
        teardown_pair();
    }
}

/* SPSR bank selection uses host comparisons, but the following predicates
 * must still see guest Z=1. Cover one actual native block and cuts on both
 * sides of each PSR instruction. */
static void case_native_psr_continuation(void) {
    const uint32_t program[] = {
        0xe1500000u, /* CMP r0,r0: Z=1, unlike the nonzero SPSR bank offset */
        0xe14f2000u, /* MRS r2,SPSR */
        0x03a0305au, /* MOVEQ r3,#0x5a */
        0xe168f001u, /* MSR SPSR_f,r1 */
        0x02834001u, /* ADDEQ r4,r3,#1 */
        0x13a040eeu, /* MOVNE r4,#0xee: must remain skipped */
        0xe1a0f006u  /* MOV pc,r6: explicit boundary, no B-self trace stitching */
    };
    for (unsigned split = 0; split < 2u; ++split) {
        current_case = split ? "psr-continuation-budget" : "psr-continuation-native";
        setup_pair();
        seed_spsr_banks();
        set_cpsr_both(0xd3u);
        set_reg_both(1u, 0x12000000u);
        set_reg_both(6u, CODE_ADDR + 0x40u);
        load_both(program, GP32_ARRAY_COUNT(program));
        static const uint32_t cuts[] = {1u, 1u, 1u, 1u, 2u, 1u};
        for (unsigned i = 0; i < (split ? GP32_ARRAY_COUNT(cuts) : 1u); ++i) {
            uint32_t budget = split ? cuts[i] : 7u;
            CHECK(arm920t_run(cpu_jit, budget) == budget, "PSR native budget");
            CHECK(arm920t_run(cpu_ref, budget) == budget, "PSR reference budget");
            compare_state();
            compare_spsr_banks();
        }
        CHECK(ref_reg(2) == SPSR_SVC_SEED && ref_reg(3) == 0x5au && ref_reg(4) == 0x5bu,
              "predicates after PSR access retain guest flags");
        CHECK(spsr_bank(cpu_ref, SPSR_MODE_SVC) == 0x12243546u, "saved flags updated");
        CHECK(arm920t_get_cpsr(cpu_ref) == 0x600000d3u, "SPSR access preserves active flags");
        CHECK(arm920t_get_pc(cpu_ref) == CODE_ADDR + 0x40u, "PSR trace ends at explicit PC write");
        gp32_cpu_profile_t profile;
        arm920t_get_cpu_profile(cpu_jit, &profile);
        if (!split && profile.supported && profile.native_backend) {
            if (profile.native_block_calls != 1u || profile.native_arm_insns != 7u)
                fprintf(stderr, "PSR block route: backend=%u calls=%" PRIu64
                        " native=%" PRIu64 " portable=%" PRIu64 " compiled=%" PRIu64 "\n",
                        profile.native_backend, profile.native_block_calls,
                        profile.native_arm_insns, profile.block_interp_arm_insns,
                        profile.jit_native_compiled);
            CHECK(profile.native_block_calls == 1u && profile.native_arm_insns == 7u,
                  "PSR sequence and dependent predicates execute in one native block");
        }
        teardown_pair();
    }
}

/* A status read is read-only, so it must stay in the native block on both
 * backends and resolve the SPSR bank from the live mode.  Bankless modes
 * (USER/SYS and unused encodings) keep the interpreter's CPSR fallback.
 * The program holds only status reads and a self branch, so a classified
 * PSR helper call (kind 2) is a regression. */
static void case_native_mrs_status(void) {
    static const unsigned modes[] = {SPSR_MODE_USR, SPSR_MODE_FIQ, SPSR_MODE_IRQ, SPSR_MODE_SVC,
                                     SPSR_MODE_ABT, SPSR_MODE_UND, SPSR_MODE_SYS, SPSR_MODE_UNUSED};
    const uint32_t program[] = {
        0xe14f2000u, /* MRS r2,SPSR */
        0xe10f3000u, /* MRS r3,CPSR */
        0xe14f4000u, /* MRS r4,SPSR: the same bank as r2 */
        0xe10f5000u, /* MRS r5,CPSR */
        0xeafffffeu
    };
    for (unsigned i = 0; i < GP32_ARRAY_COUNT(modes); ++i) {
        uint32_t cpsr = 0x600000c0u | modes[i];
        uint32_t bank = spsr_seed(modes[i]);
        uint32_t expect = bank ? bank : cpsr;
        current_case = "native-mrs-status";
        setup_pair();
        seed_spsr_banks();
        set_cpsr_both(cpsr);
        load_both(program, GP32_ARRAY_COUNT(program));
        run_native_case();
        CHECK(ref_reg(2) == expect && ref_reg(4) == expect, "MRS SPSR value");
        CHECK(ref_reg(3) == cpsr && ref_reg(5) == cpsr, "MRS CPSR value");
        gp32_cpu_profile_t profile;
        arm920t_get_cpu_profile(cpu_jit, &profile);
        if (profile.supported && (profile.native_backend == 1u || profile.native_backend == 2u)) {
            CHECK(profile.helper_op_kinds[2] == 0u, "MRS must not use the classified PSR helper");
            CHECK(profile.native_arm_insns > 0u, "MRS must run natively");
        }
        teardown_pair();
    }

    /* The translated block must resolve the bank at execution time: run it in
     * SVC, switch to ABT without a flush, rewind PC and run the same block. */
    current_case = "native-mrs-status-reuse";
    setup_pair();
    seed_spsr_banks();
    set_cpsr_both(0x600000c0u | SPSR_MODE_SVC);
    load_both(program, GP32_ARRAY_COUNT(program));
    run_native_case();
    uint64_t misses = arm920t_get_jit_misses(cpu_jit);
    CHECK(ref_reg(2) == SPSR_SVC_SEED && ref_reg(3) == (0x600000c0u | SPSR_MODE_SVC),
          "first mode reads its own bank");
    set_cpsr_both(0x600000c0u | SPSR_MODE_ABT);
    set_pc_both(CODE_ADDR);
    CHECK(arm920t_run(cpu_jit, 64u) == arm920t_run(cpu_ref, 64u), "reuse run budget");
    compare_state();
    CHECK(arm920t_get_jit_misses(cpu_jit) == misses, "mode change kept the translated block");
    CHECK(ref_reg(2) == SPSR_ABT_SEED && ref_reg(4) == SPSR_ABT_SEED,
          "reused block reads the new mode bank");
    CHECK(ref_reg(3) == (0x600000c0u | SPSR_MODE_ABT) && ref_reg(5) == ref_reg(3),
          "reused block reads the new CPSR");
    gp32_cpu_profile_t profile;
    arm920t_get_cpu_profile(cpu_jit, &profile);
    if (profile.supported && (profile.native_backend == 1u || profile.native_backend == 2u))
        CHECK(profile.helper_op_kinds[2] == 0u, "reused MRS block stays native");
    compare_spsr_banks();
    teardown_pair();
}

/* Focused SPSR bundle; --psr runs this plus case_native_longmul_psr. */
static void case_native_spsr(void) {
    case_native_spsr_mode_banks();
    case_native_spsr_no_bank();
    case_native_spsr_fields();
    case_native_spsr_conditions();
    case_native_spsr_bank_reuse();
    case_native_spsr_exception_return();
    case_native_mrs_status();
}

/* CPSR writes retire before dispatch observes a new register bank, execution
 * state or interrupt mask. Source registers must be read before bank swaps. */
static void case_native_cpsr_same_mode(void) {
    static const struct { unsigned field; uint32_t privileged, user; } rows[] = {
        {0u, 0xa23456c0u, 0xa23456c0u},
        {1u, 0x12345640u, 0x123456c0u},
        {2u, 0x12345ac0u, 0x123456c0u},
        {4u, 0x125a56c0u, 0x123456c0u},
        {7u, 0x125a5a40u, 0x123456c0u},
        {9u, 0xa5345640u, 0xa53456c0u},
        {15u, 0xa55a5a40u, 0xa53456c0u},
    };
    static const unsigned modes[] = {0x13u, 0x10u, 0x11u, 0x1fu};
    for (unsigned i = 0; i < GP32_ARRAY_COUNT(rows); ++i) {
        current_case = "native-cpsr-live-privilege";
        setup_pair();
        seed_spsr_banks();
        const uint32_t program[] = {0xe120f001u | (rows[i].field << 16), 0xeafffffeu};
        load_both(program, GP32_ARRAY_COUNT(program));
        uint64_t misses = 0;
        for (unsigned j = 0; j < GP32_ARRAY_COUNT(modes); ++j) {
            unsigned m = modes[j];
            set_cpsr_both(0x123456c0u | m);
            set_reg_both(1u, 0xa55a5a40u | (m == 0x10u ? 0x11u : m));
            set_pc_both(CODE_ADDR);
            CHECK(arm920t_run(cpu_jit, 1u) == 1u, "CPSR exact native budget");
            CHECK(arm920t_run(cpu_ref, 1u) == 1u, "CPSR exact reference budget");
            compare_state();
            CHECK(arm920t_get_cpsr(cpu_ref) ==
                  ((m == 0x10u ? rows[i].user : rows[i].privileged) | m),
                  "live privilege and selected CPSR bytes");
            CHECK(arm920t_get_pc(cpu_ref) == CODE_ADDR + 4u, "MSR retires once");
            arm920t_register_context_t actual = {0}, expected = {0};
            arm920t_get_register_context(cpu_jit, &actual);
            arm920t_get_register_context(cpu_ref, &expected);
            CHECK(!memcmp(&actual, &expected, sizeof(actual)), "CPSR preserves banked state");
            if (j) CHECK(arm920t_get_jit_misses(cpu_jit) == misses,
                         "reuse compiled MSR across privilege modes");
            misses = arm920t_get_jit_misses(cpu_jit);
        }
        gp32_cpu_profile_t profile;
        arm920t_get_cpu_profile(cpu_jit, &profile);
        if (profile.supported && (profile.native_backend == 1u || profile.native_backend == 2u)) {
            CHECK(profile.native_arm_insns == GP32_ARRAY_COUNT(modes), "native MSR coverage");
            /* A control-byte write with a deliberately different USER source
             * mode takes the shared privilege helper. Other writes stay native;
             * architectural expectations above do not depend on that route. */
            CHECK(profile.helper_interp_ops == ((rows[i].field & 1u) ? 1u : 0u),
                  "only USER's different source mode needs the privilege helper");
        }
        teardown_pair();
    }
}

static void case_native_cpsr(void) {
    case_native_cpsr_same_mode();
    const uint32_t bank_program[] = {
        0xe12ff00du, /* MSR CPSR_fsxc,sp: source is the outgoing SVC bank */
        0xe1a06008u, /* MOV r6,r8: observe incoming FIQ bank */
        0xe121f00du, /* MSR CPSR_c,sp: source is now FIQ's SP */
        0xe1a0500du, /* MOV r5,sp: original SVC SP must be restored */
        0xeafffffeu
    };
    current_case = "native-cpsr-bank-source";
    setup_pair();
    set_cpsr_both(0x600000d3u);
    set_reg_both(13u, 0x600000d1u);
    arm920t_t *pair[] = {cpu_jit, cpu_ref};
    for (unsigned i = 0; i < 2u; ++i) {
        arm920t_register_context_t context;
        arm920t_get_register_context(pair[i], &context);
        context.bank_fiq[0] = 0x77788899u;
        context.bank_fiq[5] = 0xd3u;
        arm920t_set_register_context(pair[i], &context);
    }
    load_both(bank_program, GP32_ARRAY_COUNT(bank_program));
    run_native_case();
    CHECK(ref_reg(6) == 0x77788899u, "CPSR switch selected the FIQ register bank");
    CHECK(ref_reg(5) == 0x600000d1u && ref_reg(13) == 0x600000d1u,
          "banked MSR source was captured before switching and SVC SP restored");
    CHECK(arm920t_get_cpsr(cpu_ref) == 0x600000d3u, "CPSR bank round trip");
    teardown_pair();

    const uint32_t thumb_program[] = {
        0xe321f033u, /* MSR CPSR_c,#SVC|T */
        0x21bb205au, /* Thumb MOVS r0,#0x5a; MOVS r1,#0xbb */
        0xe7fee7feu  /* Thumb B self; decoding as ARM instead would fault */
    };
    current_case = "native-cpsr-thumb-exit";
    setup_pair();
    set_cpsr_both(0xd3u);
    load_both(thumb_program, GP32_ARRAY_COUNT(thumb_program));
    run_native_case();
    CHECK(ref_reg(0) == 0x5au && ref_reg(1) == 0xbbu, "MSR must dispatch into Thumb");
    CHECK((arm920t_get_cpsr(cpu_ref) & 0x20u) && arm920t_get_pc(cpu_ref) == CODE_ADDR + 8u,
          "Thumb state and final PC after MSR");
    teardown_pair();

    for (unsigned fiq = 0; fiq < 2u; ++fiq) {
        const uint32_t program[] = {
            fiq ? 0xe321f093u : 0xe321f053u, /* MSR CPSR_c, unmask one line */
            0xe3a0505au, /* MOV r5,#0x5a: must not execute before the exception */
            0xeafffffeu
        };
        current_case = fiq ? "native-cpsr-unmask-fiq" : "native-cpsr-unmask-irq";
        setup_pair();
        set_cpsr_both(0xd3u);
        load_both(program, GP32_ARRAY_COUNT(program));
        set_mem_both(0x18u, 0xeafffffeu);
        set_mem_both(0x1cu, 0xeafffffeu);
        if (fiq) { arm920t_set_fiq(cpu_jit, 1); arm920t_set_fiq(cpu_ref, 1); }
        else { arm920t_set_irq(cpu_jit, 1); arm920t_set_irq(cpu_ref, 1); }
        run_native_case();
        CHECK(ref_reg(5) == 0u && arm920t_get_pc(cpu_ref) == (fiq ? 0x1cu : 0x18u),
              "unmasked pending interrupt taken before the next guest instruction");
        CHECK((arm920t_get_cpsr(cpu_ref) & 31u) == (fiq ? 0x11u : 0x12u), "interrupt mode");
        teardown_pair();
    }

    const uint32_t partial_program[] = {
        0xe122f00fu, /* MSR CPSR_x,pc: source PC+8 supplies byte 0x04 */
        0xe124f00fu, /* MSR CPSR_s,pc: source PC+8 supplies byte 0x00 */
        0xe120f002u, /* existing zero-field NZCV-only behavior */
        0xe10f3000u, /* MRS r3,CPSR */
        0xeafffffeu
    };
    current_case = "native-cpsr-partial-pc";
    setup_pair();
    set_cpsr_both(0x0fffaad3u);
    set_reg_both(2u, 0xb1234567u);
    load_both(partial_program, GP32_ARRAY_COUNT(partial_program));
    run_native_case();
    CHECK(ref_reg(3) == 0xbf0004d3u && arm920t_get_cpsr(cpu_ref) == 0xbf0004d3u,
          "PC-source byte writes and zero-field NZCV preserve unselected bits");
    teardown_pair();
}

static void case_native_mapped_block(void) {
    const uint32_t va = 0x10000ffcu;
    const uint32_t ttb = RAM_BASE + 0x4000u, l2 = RAM_BASE + 0x8000u;
    const uint32_t phys0 = RAM_BASE + 0x10000u, phys1 = RAM_BASE + 0x30000u;
    for (unsigned mode = 0; mode < 5; ++mode) {
        int store = mode >= 3, odd = mode == 1, tiny = mode == 2, nonram = mode == 3;
        uint32_t program[] = {
            0xee02af10u, /* MCR p15,0,r10,c2,c0,0: translation table */
            0xee01bf10u, /* MCR p15,0,r11,c1,c0,0: enable MMU */
            0xe590c000u, /* LDR r12,[r0]: fill first-page TLB */
            0xe592c000u, /* LDR r12,[r2]: fill second-page TLB */
            0,          /* LDM/STMIA r0[!],{r0,r3,pc} */
            0xeafffffeu,
            0xe3a07077u, /* ARMv4T LDM PC stays ARM even for odd targets. */
            0xeafffffeu
        };
        current_case = mode == 0 ? "mapped-block-load" : mode == 1 ? "mapped-block-odd" :
                       mode == 2 ? "mapped-block-tiny" : mode == 3 ? "mapped-block-nonram" :
                       "mapped-block-store";
        setup_pair();
        program[4] = block_insn(0, 1, 0, mode != 2, !store, 0, 0x8009u);
        load_both(program, GP32_ARRAY_COUNT(program));
        set_reg_both(0, va);
        set_reg_both(2, va + 4u);
        set_reg_both(3, 0xaabbccddu);
        set_reg_both(10, ttb);
        set_reg_both(11, 1u);
        set_mem_both(ttb, 2u); /* identity section containing BIOS code */
        set_mem_both(ttb + 0x400u, l2 | 1u); /* L1[0x100] coarse table */
        set_mem_both(l2, phys0 | 2u);
        set_mem_both(l2 + 4u, (nonram ? 0x14000000u : phys1) | (tiny ? 3u : 2u));
        set_mem_both(phys0 + 0xffcu, 0x11223344u);
        set_mem_both(phys1, 0x55667788u);
        set_mem_both(phys1 + 4u, (CODE_ADDR + 24u) | (odd ? 1u : 0u));
        run_native_case();
        CHECK(arm920t_get_cp15(cpu_ref, 1) & 1u, "MMU fixture was not enabled");
        if (store) {
            CHECK(gp32_ld32le(bus_ref.ram + 0x10ffcu) == va, "STM base-in-list value");
            CHECK(ref_reg(0) == va + 12u, "STM writeback");
            if (!nonram)
                CHECK(gp32_ld32le(bus_ref.ram + 0x30004u) == CODE_ADDR + 24u, "STM PC pipeline");
        } else {
            CHECK(ref_reg(0) == 0x11223344u && ref_reg(3) == 0x55667788u,
                  "LDM noncontiguous pages and base-in-list");
            CHECK(ref_reg(7) == 0x77u, "LDM mapped PC target");
            CHECK(!(arm920t_get_cpsr(cpu_ref) & 0x20u) &&
                  arm920t_get_pc(cpu_ref) == CODE_ADDR + 28u, "LDM mapped PC state/alignment");
        }
        teardown_pair();
    }
}

/* CP15 revalidation must agree with instruction fetch, not with a second
 * copy of the block cache: tracing with no logger forces exec_arm on ref. */
static void setup_cache_pair(void) {
    setup_pair();
    arm920t_set_trace(cpu_ref, 1, NULL, NULL);
    set_mem_both(CODE_ADDR, 0xee070f15u); /* MCR p15,0,r0,c7,c5,0 */
    set_reg_both(0, 0x12345678u);
}
static void run_cache_pair(uint32_t pc, uint32_t cycles) {
    set_reg_both(15, pc);
    CHECK(arm920t_run(cpu_jit, cycles) == arm920t_run(cpu_ref, cycles), "cache-maintenance budget");
    compare_state();
    for (unsigned i = 0; i < 16u; ++i) {
        uint32_t j = arm920t_get_cp15(cpu_jit, i), r = arm920t_get_cp15(cpu_ref, i);
        if (j != r) { char nm[12]; snprintf(nm, sizeof(nm), "cp15[%u]", i); report(nm, j, r); }
    }
}
static uint64_t cache_code_used(void) {
    gp32_cpu_profile_t p;
    arm920t_get_cpu_profile(cpu_jit, &p);
    return p.jit_code_used;
}
static void case_native_literal_addresses(void) {
    const uint32_t pc = RAM_BASE + 0x2000u;
    for (unsigned down = 0; down < 2u; ++down) {
        for (unsigned lane = 0; lane < 4u; ++lane) {
            current_case = "native-literal-addresses";
            setup_pair();
            const uint32_t addr = (down ? pc - 0x100u : pc + 0x100u) + lane;
            const uint32_t off0 = down ? pc + 8u - addr : addr - pc - 8u;
            const uint32_t off1 = down ? pc + 12u - addr : addr - pc - 12u;
            set_mem_both(pc, (down ? 0xe51f1000u : 0xe59f1000u) | off0); /* LDR r1 */
            set_mem_both(pc + 4u, (down ? 0xe55f2000u : 0xe5df2000u) | off1); /* LDRB r2 */
            set_mem_both(pc + 8u, 0xe58f3100u | lane); /* STR r3,[pc,#256+lane] */
            set_mem_both(pc + 12u, 0xeafffffeu);
            set_reg_both(3, 0x98765432u);
            for (unsigned pass = 0; pass < 2u; ++pass) {
                uint32_t value = pass ? 0x88776655u : 0x44332211u;
                set_mem_both(addr & ~3u, value);
                run_cache_pair(pc, 20u);
                CHECK(ref_reg(1) == gp32_ror32(value, lane * 8u), "literal LDR preserves ARM rotation");
                CHECK(ref_reg(2) == ((value >> (lane * 8u)) & 0xffu), "literal LDRB selects exact byte");
                CHECK(gp32_ld32le(bus_ptr(&bus_ref, pc + 0x110u, 4u)) == 0x98765432u,
                      "PC-relative STR aligns its destination");
            }
            teardown_pair();
        }
    }
}
static uint32_t cache_branch(uint32_t pc, uint32_t target, int link) {
    return (link ? 0xeb000000u : 0xea000000u) | (((target - pc - 8u) >> 2) & 0x00ffffffu);
}
/* The caller is translated before its callee has a TLB entry. Once real
 * execution fills that entry, it must gain the same call folding as a warm
 * mapping, without changing partial-budget or later cache-flush behavior. */
static void case_cold_leaf_mapping(void) {
    const uint32_t caller = 0x10001000u, callee = 0x10002000u;
    const uint32_t code = RAM_BASE + 0x10000u, leaf = RAM_BASE + 0x20000u;
    const uint32_t ttb = RAM_BASE + 0x4000u, l2 = RAM_BASE + 0x8000u;
    const uint32_t setup[] = {0xee02af10u, 0xee01bf10u};
    current_case = "cold-callee-mapping";
    setup_pair();
    arm920t_set_trace(cpu_ref, 1, NULL, NULL);
    load_both(setup, GP32_ARRAY_COUNT(setup));
    set_mem_both(ttb, 2u);
    set_mem_both(ttb + 0x300u, RAM_BASE | 2u);
    set_mem_both(ttb + 0x400u, l2 | 1u);
    set_mem_both(l2 + 4u, code | 2u);
    set_mem_both(l2 + 8u, leaf | 2u);
    set_mem_both(code, cache_branch(caller, callee, 1));
    set_mem_both(code + 4u, 0xe2844001u); /* ADD r4,r4,#1 */
    set_mem_both(code + 8u, cache_branch(caller + 8u, caller, 0));
    set_mem_both(leaf, 0xe92d4000u);      /* PUSH {lr} */
    set_mem_both(leaf + 4u, 0xe5970000u); /* LDR r0,[r7] */
    set_mem_both(leaf + 8u, 0xe8bd8000u); /* POP {pc} */
    set_mem_both(DATA_ADDR, 0x12345678u);
    set_reg_both(7, DATA_ADDR);
    set_reg_both(10, ttb);
    set_reg_both(11, 1u);
    set_reg_both(13, RAM_BASE + 0x30000u);
    run_cache_pair(CODE_ADDR, 2u);
    run_cache_pair(caller, 1u); /* cold call; do not fetch its target early */
    CHECK(ref_reg(15) == callee, "cold call target");
    run_chunks();
    set_reg_both(13, RAM_BASE + 0x30000u);
    set_reg_both(4, 0u);
    arm920t_reset_cpu_profile(cpu_jit);
    run_cache_pair(caller, 1200u);
    CHECK(ref_reg(0) == 0x12345678u && ref_reg(4) == 200u, "folded call results");
    gp32_cpu_profile_t p;
    arm920t_get_cpu_profile(cpu_jit, &p);
    if (p.supported && p.native_backend)
        CHECK(p.native_block_calls < 300u, "cold mapping kept separate call/return dispatches");
    set_mem_both(leaf + 4u, 0xe3a00077u); /* change the collected callee */
    set_mem_both(CODE_ADDR + 8u, 0xee070f15u); /* MCR p15,0,r0,c7,c5,0 */
    run_cache_pair(CODE_ADDR + 8u, 1u);
    run_cache_pair(caller, 6u);
    CHECK(ref_reg(0) == 0x77u, "cache maintenance revalidates collected callee");
    teardown_pair();
}
static void case_cache_unchanged(void) {
    const uint32_t target = RAM_BASE + 0x3000u;
    current_case = "cache-unchanged-reuse";
    setup_cache_pair();
    set_mem_both(target, 0xe2844001u);      /* ADD r4,r4,#1 */
    set_mem_both(target + 4u, 0xe5975000u); /* LDR r5,[r7]: A64 checked helper for BIOS */
    set_mem_both(target + 8u, 0xeafffffeu);
    set_mem_both(CODE_ADDR + 0x1000u, 0x87654321u);
    set_reg_both(7, CODE_ADDR + 0x1000u);
    run_cache_pair(CODE_ADDR, 1u); /* warm the terminal CP15 block too */
    run_cache_pair(target, 32u);
    uint64_t misses = arm920t_get_jit_misses(cpu_jit), used = cache_code_used();
    for (unsigned i = 0; i < 3u; ++i) {
        set_reg_both(4, 0u);
        run_cache_pair(CODE_ADDR, 1u);
        CHECK(arm920t_get_cp15(cpu_ref, 7) == 0x12345678u, "guest CP15 write did not execute");
        CHECK(cache_code_used() == used, "unchanged invalidation reset the code arena");
        run_cache_pair(target, 32u);
        CHECK(ref_reg(4) == 1u && ref_reg(5) == 0x87654321u, "reused helper trace result");
        CHECK(arm920t_get_jit_misses(cpu_jit) == misses, "unchanged blocks were translated again");
        CHECK(cache_code_used() == used, "unchanged blocks allocated new native code");
    }
    /* An explicit API flush must still discard every block. */
    arm920t_flush_jit(cpu_jit);
    CHECK(cache_code_used() == 0u, "API flush retained the code arena");
    run_cache_pair(target, 32u);
    CHECK(arm920t_get_jit_misses(cpu_jit) > misses, "API flush reused obsolete metadata");
    teardown_pair();
}
static void case_cache_modified(int leaf) {
    const uint32_t target = RAM_BASE + 0x3000u, changed = target + (leaf ? 0x804u : 0x100u);
    const uint32_t retained = RAM_BASE + 0x3c00u;
    current_case = leaf ? "cache-inlined-callee-modified" : "cache-noncontiguous-ram-modified";
    setup_cache_pair();
    set_mem_both(target, cache_branch(target, leaf ? changed - 4u : changed, leaf));
    if (leaf) {
        set_mem_both(target + 4u, 0xe3a08033u); /* MOV r8,#0x33 after return */
        set_mem_both(target + 8u, 0xeafffffeu);
        set_mem_both(changed - 4u, 0xe92d4000u); /* PUSH {lr} */
        set_mem_both(changed + 4u, 0xe8bd8000u); /* POP {pc} */
        set_reg_both(13, DATA_ADDR + 0x1000u);
    } else set_mem_both(changed + 4u, 0xeafffffeu);
    set_mem_both(changed, 0xe3a04011u); /* MOV r4,#0x11 */
    set_mem_both(retained, 0xe3a06055u);
    set_mem_both(retained + 4u, 0xeafffffeu);
    run_cache_pair(target, 32u);
    CHECK(ref_reg(4) == 0x11u, "original traced instruction");
    if (leaf) {
        /* x64 can return at the inline POP and compile the caller suffix;
         * a separately dispatched callee would require a fourth block. */
        CHECK(arm920t_get_jit_misses(cpu_jit) <= 3u, "fixture did not inline the callee");
        CHECK(ref_reg(8) == 0x33u, "leaf did not return to caller");
    }
    run_cache_pair(retained, 32u);
    /* Modify a recorded instruction outside the caller's contiguous range
     * through an actual guest STR immediately before the full I-cache op. */
    set_mem_both(CODE_ADDR, 0xe5821000u); /* STR r1,[r2] */
    set_mem_both(CODE_ADDR + 4u, 0xee070f15u);
    set_reg_both(1, 0xe3a04022u);
    set_reg_both(2, changed);
    run_cache_pair(CODE_ADDR, 2u);
    CHECK(gp32_ld32le(bus_ref.ram + (changed - RAM_BASE)) == 0xe3a04022u, "guest code write");
    uint64_t misses = arm920t_get_jit_misses(cpu_jit);
    run_cache_pair(retained, 32u);
    CHECK(arm920t_get_jit_misses(cpu_jit) == misses, "unrelated unchanged block was invalidated");
    set_reg_both(4, 0u);
    run_cache_pair(target, 32u);
    CHECK(ref_reg(4) == 0x22u, "modified trace instruction was not fetched");
    CHECK(arm920t_get_jit_misses(cpu_jit) > misses, "modified caller was reused");
    if (leaf) CHECK(ref_reg(13) == DATA_ADDR + 0x1000u, "modified inline leaf unbalanced SP");
    teardown_pair();
}
/* Exercise native RAM-page lookup against instruction-by-instruction MMU
 * execution, including two VAs colliding in the 4096-entry TLB index. */
static void case_native_ram_mmio_alias(void) {
    const uint32_t va = 0x10007000u, alias = 0x11007000u;
    const uint32_t ttb = RAM_BASE + 0x4000u, l2 = RAM_BASE + 0x8000u;
    const uint32_t program[] = {
        0xee02af10u, 0xee01bf10u, /* MCR TTB/control */
        0xe5901000u, 0xe5902000u, /* fill RAM mapping, then hit it */
        0xe5983000u,             /* alias must read MMIO, not cached RAM */
        0xe5904000u, 0xe5905000u, /* restore RAM mapping, then hit it again */
        0xeafffffeu,
    };
    current_case = "mapped-RAM-MMIO-tag-collision";
    setup_pair();
    arm920t_set_trace(cpu_ref, 1, NULL, NULL);
    load_both(program, GP32_ARRAY_COUNT(program));
    set_reg_both(0, va); set_reg_both(8, alias);
    set_reg_both(10, ttb); set_reg_both(11, 1u);
    set_mem_both(ttb, 2u); /* BIOS identity section */
    set_mem_both(ttb + 0x400u, l2 | 1u);
    set_mem_both(ttb + 0x440u, (l2 + 0x1000u) | 1u);
    set_mem_both(l2 + 0x1cu, DATA_ADDR | 2u);
    set_mem_both(l2 + 0x101cu, IO_ADDR | 2u);
    set_mem_both(DATA_ADDR, 0x89abcdefu);
    bus_jit.observe_cpu = cpu_jit; bus_ref.observe_cpu = cpu_ref;
    for (unsigned repeat = 0; repeat < 2u; ++repeat) {
        run_cache_pair(CODE_ADDR + (repeat ? 8u : 0u), 64u);
        CHECK(ref_reg(1) == 0x89abcdefu && ref_reg(2) == 0x89abcdefu &&
              ref_reg(4) == 0x89abcdefu && ref_reg(5) == 0x89abcdefu,
              "RAM reads survive a non-RAM colliding tag");
        CHECK(ref_reg(3) == CODE_ADDR + 20u, "MMIO observes its real instruction PC");
        CHECK(bus_jit.io_count == repeat + 1u && bus_ref.io_count == repeat + 1u,
              "each non-RAM alias executes exactly one bus callback");
    }
    gp32_cpu_profile_t profile;
    arm920t_get_cpu_profile(cpu_jit, &profile);
    if (profile.supported && profile.native_backend)
        CHECK(profile.native_arm_insns >= 10u, "tag collision exercises native execution");
    teardown_pair();
}

static void case_native_mapped_pages(void) {
    case_native_ram_mmio_alias();
    const uint32_t masks[] = {0xfffu, 0xffffu, 0xfffffu, 0x3ffu};
    const uint32_t va = 0x100077fdu, alias = va + 0x01000000u;
    const uint32_t ttb = RAM_BASE + 0x4000u, l2 = RAM_BASE + 0x8000u;
    const uint32_t program[] = {
        0xee02af10u, /* MCR p15,0,r10,c2,c0,0 */
        0xee01bf10u, /* MCR p15,0,r11,c1,c0,0 */
        0xe590c000u, /* LDR r12,[r0]: fill the data mapping */
        0xe5901000u, /* LDR r1,[r0]: unaligned ARM rotation */
        0xe5d02001u, /* LDRB r2,[r0,#1] */
        0xe1d030b1u, /* LDRH r3,[r0,#1] */
        0xe5c04002u, /* STRB r4,[r0,#2] */
        0xe1d050b1u, /* LDRH r5,[r0,#1] */
        0xe5986000u, /* LDR r6,[r8]: different VA, same TLB index */
        0xe5907000u, /* LDR r7,[r0]: restore the original mapping */
        0xeafffffeu,
    };
    for (unsigned shape = 0; shape < GP32_ARRAY_COUNT(masks); ++shape) {
        uint32_t mask = masks[shape];
        uint32_t phys = RAM_BASE + (shape == 2u ? 0x100000u : 0x10000u);
        uint32_t phys_alias = RAM_BASE + (shape == 2u ? 0x200000u : 0x20000u);
        uint32_t type = shape == 1u ? 1u : shape == 3u ? 3u : 2u;
        uint32_t descriptor = shape == 2u ? ttb + 0x400u : l2 + ((va >> 10) & 0x3fcu);
        current_case = shape == 0u ? "mapped-page-small" : shape == 1u ? "mapped-page-large" :
                       shape == 2u ? "mapped-page-section" : "mapped-page-tiny";
        setup_pair();
        arm920t_set_trace(cpu_ref, 1, NULL, NULL);
        load_both(program, GP32_ARRAY_COUNT(program));
        set_reg_both(0, va);
        set_reg_both(4, 0xaau);
        set_reg_both(8, alias);
        set_reg_both(10, ttb);
        set_reg_both(11, 1u);
        set_mem_both(ttb, 2u); /* BIOS identity section */
        if (shape == 2u) {
            set_mem_both(ttb + 0x400u, phys | type);
            set_mem_both(ttb + 0x440u, phys_alias | type);
        } else {
            set_mem_both(ttb + 0x400u, l2 | 1u);
            set_mem_both(ttb + 0x440u, (l2 + 0x1000u) | 1u);
            set_mem_both(descriptor, phys | type);
            set_mem_both(descriptor + 0x1000u, phys_alias | type);
        }
        set_mem_both(phys + ((va & mask) & ~3u), 0x44332211u);
        set_mem_both(phys_alias + ((alias & mask) & ~3u), 0x88776655u);
        run_cache_pair(CODE_ADDR, 64u);
        CHECK(ref_reg(1) == 0x11443322u && ref_reg(2) == 0x33u && ref_reg(3) == 0x4433u,
              "mapped word rotation/byte/halfword loads");
        CHECK(ref_reg(5) == 0xaa33u && ref_reg(6) == 0x55887766u && ref_reg(7) == 0x11aa3322u,
              "mapped store and colliding VA tag");
        /* A state roundtrip must rebuild the derived lookup from saved TLB
         * masks/indices, including sections whose VA base omits page bits. */
        arm920t_t *pair[] = {cpu_jit, cpu_ref};
        for (unsigned i = 0; i < GP32_ARRAY_COUNT(pair); ++i) {
            state_io_t count = state_io_counter();
            CHECK(arm920t_state_save_io(pair[i], &count), "count CPU state");
            uint8_t *saved = (uint8_t *)malloc(count.pos);
            if (!saved) { fail("allocate CPU state"); break; }
            state_io_t out = state_io_writer(saved, count.pos);
            CHECK(arm920t_state_save_io(pair[i], &out), "save CPU state");
            state_io_t in = state_io_reader(saved, count.pos);
            CHECK(arm920t_state_load_io(pair[i], &in), "reload CPU state");
            free(saved);
        }
        run_cache_pair(CODE_ADDR + 12u, 64u);
        CHECK(ref_reg(7) == 0x11aa3322u, "loaded TLB mapping changed");
        /* Change the table under a live cached mapping, then invalidate it
         * using the guest instruction rather than an API shortcut. */
        set_mem_both(descriptor, phys_alias | type);
        set_mem_both(CODE_ADDR + 0x100u, 0xee080f17u); /* MCR p15,0,r0,c8,c7,0 */
        run_cache_pair(CODE_ADDR + 0x100u, 1u);
        run_cache_pair(CODE_ADDR + 12u, 64u);
        CHECK(ref_reg(1) == 0x55887766u && ref_reg(7) == 0x55aa7766u,
              "TLB invalidation retained an obsolete RAM-page mapping");
        set_reg_both(15, CODE_ADDR + 12u);
        run_chunks();
        teardown_pair();
    }
}

/* Recompile the same memory block across MMU mode changes. The VA is itself
 * in RAM, so stale MMU-off code reads/writes a different valid physical page.
 * Restore both saved modes too: the mapped-page roundtrip keeps MMU enabled. */
static void case_native_mmu_mode_changes(void) {
    const uint32_t va = RAM_BASE + 0x10000u, mapped = RAM_BASE + 0x20000u;
    const uint32_t ttb = RAM_BASE + 0x4000u, l2 = RAM_BASE + 0x8000u;
    const uint32_t values[] = {0x11223344u, 0x55667788u};
    const uint32_t markers[] = {0xa1a2a3a4u, 0xb1b2b3b4u, 0xc1c2c3c4u,
                                0xd1d2d3d4u, 0xe1e2e3e4u};
    const uint32_t program[] = {
        0xe5901000u, /* LDR r1,[r0] */
        0xe5802004u, /* STR r2,[r0,#4] */
        0xe5d03000u, /* LDRB r3,[r0] */
        half_insn(1, 1, 0, 1, 1, 0, 4, 1, 2), /* LDRH r4,[r0,#2] */
        0xe5c02008u, /* STRB r2,[r0,#8] */
        half_insn(1, 1, 0, 0, 1, 0, 2, 1, 10), /* STRH r2,[r0,#10] */
        0xe8900060u, /* LDMIA r0,{r5,r6} */
        0xe8890060u, /* STMIA r9,{r5,r6} */
        0xeafffffeu,
    };
    uint32_t stores[] = {0x01020304u, 0x05060708u};
    uint8_t *saved[2][2] = {{NULL, NULL}, {NULL, NULL}};
    arm920t_t *pair[2];
    current_case = "native-MMU-mode-changes";
    setup_pair();
    pair[0] = cpu_jit;
    pair[1] = cpu_ref;
    arm920t_set_trace(cpu_ref, 1, NULL, NULL);
    load_both(program, GP32_ARRAY_COUNT(program));
    set_mem_both(CODE_ADDR + 0x100u, 0xee01bf10u); /* MCR p15,0,r11,c1,c0,0 */
    set_mem_both(CODE_ADDR + 0x104u, 0xee02af10u); /* MCR p15,0,r10,c2,c0,0 */
    set_reg_both(0, va);
    set_reg_both(9, va + 16u);
    set_reg_both(10, ttb);
    set_mem_both(ttb, 2u); /* BIOS identity section */
    set_mem_both(ttb + ((va >> 20) * 4u), l2 | 1u);
    set_mem_both(l2 + ((va >> 10) & 0x3fcu), mapped | 2u);
    set_mem_both(va, values[0]);
    set_mem_both(mapped, values[1]);
    set_mem_both(va + 4u, stores[0]);
    set_mem_both(mapped + 4u, stores[1]);
    run_cache_pair(CODE_ADDR + 0x104u, 1u);
    state_io_t count = state_io_counter();
    CHECK(arm920t_state_save_io(cpu_jit, &count), "count MMU-mode CPU state");
    for (unsigned phase = 0; phase < GP32_ARRAY_COUNT(markers); ++phase) {
        unsigned mode = phase & 1u;
        if (phase < 3u) { /* Guest control writes: off -> on -> off. */
            set_reg_both(11, 0x70u | mode);
            run_cache_pair(CODE_ADDR + 0x100u, 1u);
        } else { /* Restore on over off, then off over on, with live blocks. */
            for (unsigned i = 0; i < GP32_ARRAY_COUNT(pair); ++i) {
                state_io_t in = state_io_reader(saved[mode][i], count.pos);
                CHECK(arm920t_state_load_io(pair[i], &in), "restore different MMU mode");
            }
        }
        CHECK((arm920t_get_cp15(cpu_jit, 1) & 1u) == mode &&
              (arm920t_get_cp15(cpu_ref, 1) & 1u) == mode, "requested MMU mode");
        set_reg_both(2, markers[phase]);
        stores[mode] = markers[phase];
        /* The first mapped load fills a cold TLB; repeat from the same PC
         * to exercise the cached block with a direct RAM-page lookup. */
        for (unsigned repeat = 0; repeat < 2u; ++repeat) {
            set_reg_both(1, UINT32_MAX);
            run_cache_pair(CODE_ADDR, 64u);
            CHECK(ref_reg(1) == values[mode], "MMU mode selected the wrong load page");
            CHECK(ref_reg(3) == (values[mode] & 0xffu) &&
                  ref_reg(4) == (values[mode] >> 16), "MMU byte/halfword loads");
            CHECK(ref_reg(5) == values[mode] && ref_reg(6) == stores[mode], "MMU block load");
            uint32_t selected = mode ? mapped : va;
            CHECK(tb_read32(&bus_ref, selected + 16u) == values[mode] &&
                  tb_read32(&bus_ref, selected + 20u) == stores[mode], "MMU block store");
            CHECK(tb_read32(&bus_ref, va) == values[0] &&
                  tb_read32(&bus_ref, mapped) == values[1], "MMU load words changed");
            CHECK(tb_read32(&bus_ref, va + 4u) == stores[0] &&
                  tb_read32(&bus_ref, mapped + 4u) == stores[1], "MMU mode selected the wrong store page");
        }
        if (phase < 2u) {
            for (unsigned i = 0; i < GP32_ARRAY_COUNT(pair); ++i) {
                saved[mode][i] = (uint8_t *)malloc(count.pos);
                if (!saved[mode][i]) { fail("allocate MMU-mode CPU state"); goto done; }
                state_io_t out = state_io_writer(saved[mode][i], count.pos);
                CHECK(arm920t_state_save_io(pair[i], &out), "save MMU-mode CPU state");
            }
        }
    }
done:
    for (unsigned mode = 0; mode < 2u; ++mode)
        for (unsigned i = 0; i < GP32_ARRAY_COUNT(pair); ++i) free(saved[mode][i]);
    teardown_pair();
}

static void case_native_mapped_ram_end(void) {
    const uint32_t va = 0x10007ffcu, ttb = RAM_BASE + 0x4000u, l2 = RAM_BASE + 0x8000u;
    const uint32_t program[] = {
        0xee02af10u, 0xee01bf10u, 0xe590c000u,
        half_insn(1, 1, 0, 1, 1, 0, 1, 1, 2), /* last in-RAM halfword */
        half_insn(1, 1, 0, 1, 1, 0, 2, 1, 3), /* halfword overruns RAM */
        half_insn(1, 1, 0, 1, 1, 0, 3, 3, 3), /* signed overrun */
        half_insn(1, 1, 0, 0, 1, 0, 4, 1, 3), /* rejected store */
        0xe5d05003u, 0xeafffffeu,
    };
    current_case = "mapped-page-RAM-end";
    setup_pair();
    arm920t_set_trace(cpu_ref, 1, NULL, NULL);
    load_both(program, GP32_ARRAY_COUNT(program));
    set_reg_both(0, va);
    set_reg_both(4, 0x1234u);
    set_reg_both(10, ttb);
    set_reg_both(11, 1u);
    set_mem_both(ttb, 2u);
    set_mem_both(ttb + 0x400u, l2 | 1u);
    set_mem_both(l2 + 7u * 4u, (RAM_BASE + RAM_SIZE - 0x1000u) | 2u);
    set_mem_both(RAM_BASE + RAM_SIZE - 4u, 0x88776655u);
    run_cache_pair(CODE_ADDR, 64u);
    CHECK(ref_reg(1) == 0x8877u && ref_reg(2) == 0xffffu && ref_reg(3) == UINT32_MAX,
          "mapped halfword RAM-end callback semantics");
    CHECK(ref_reg(5) == 0x88u && gp32_ld32le(bus_ref.ram + RAM_SIZE - 4u) == 0x88776655u,
          "mapped halfword overrun wrote beyond RAM");
    teardown_pair();
}

/* Loop boundaries must preserve instruction budgets, MMIO, interrupts,
 * invalidation and returns. Oracle assertions supplement full CPU/RAM
 * comparisons. Finite callback/leaf cases expose native-call counts without
 * counting the idle B at the end as evidence of useful chaining. */

/* Loop body reads an MMIO word whose helper raises IRQ on the 50th access;
 * the 0x18 vector handler counts the event in RAM and clears the line with
 * an acknowledge write.  A chained backedge that skips the IRQ fence would
 * run extra iterations or defer the handler past a chunk compare. */
static const uint32_t P_LOOP_IRQ[] = {
    0xE3A010C8u, /* MOV  r1, #200          ; loop iterations                */
    0xE3A04414u, /* MOV  r4, #0x14000000   ; IO_ADDR                       */
    0xE2848004u, /* ADD  r8, r4, #4        ; IRQ acknowledge MMIO          */
    0xE3A0940Cu, /* MOV  r9, #0x0c000000   ; RAM_BASE: handler counter     */
    0xE3A05000u, /* MOV  r5, #0            ; accumulator                   */
    0xE5942000u, /* loop: LDR r2, [r4]     ; MMIO read, raises IRQ @ #50    */
    0xE0855002u, /*       ADD r5, r5, r2                                  */
    0xE2511001u, /*       SUBS r1, r1, #1                                 */
    0x1AFFFFFBu, /*       BNE  loop        ; conditional backward B         */
    0xE1A00005u, /* MOV  r0, r5                                          */
    0xEAFFFFFEu, /* B .                                                   */
};
static const uint32_t P_IRQ_HANDLER[] = {  /* at vector 0x18               */
    0xE599C000u, /* LDR  r12, [r9]         ; irq_count++                   */
    0xE28CC001u, /* ADD  r12, r12, #1                                    */
    0xE589C000u, /* STR  r12, [r9]                                        */
    0xE5883000u, /* STR  r3, [r8]          ; acknowledge: drop IRQ line    */
    0xE25EF004u, /* SUBS pc, lr, #4       ; return + SPSR restore          */
};
static void case_loop_irq_fence(void) {
    current_case = "loop-irq-fence";
    setup_pair();
    load_both(P_LOOP_IRQ, GP32_ARRAY_COUNT(P_LOOP_IRQ));
    for (unsigned i = 0; i < GP32_ARRAY_COUNT(P_IRQ_HANDLER); ++i)
        set_mem_both(0x18u + i * 4u, P_IRQ_HANDLER[i]);
    arm920t_set_cpsr(cpu_jit, 0x1fu);   /* system mode, IRQ/FIQ unmasked */
    arm920t_set_cpsr(cpu_ref, 0x1fu);
    bus_jit.observe_cpu = cpu_jit;
    bus_ref.observe_cpu = cpu_ref;
    bus_jit.io_raise_at = bus_ref.io_raise_at = 50u;
    run_chunks();
    CHECK(ref_reg(1) == 0u, "IRQ loop did not run all 200 iterations");
    CHECK(ref_reg(0) == 200u * (CODE_ADDR + 0x18u),
          "IRQ loop accumulated the wrong MMIO values");
    CHECK(arm920t_get_pc(cpu_ref) == CODE_ADDR + 0x28u,
          "IRQ loop did not park on B self");
    CHECK(gp32_ld32le(bus_ref.ram) == 1u, "IRQ handler did not run exactly once");
    CHECK(bus_ref.io_acks == 1u, "guest did not acknowledge the IRQ once");
    CHECK(bus_ref.io_count == 200u, "oracle saw a wrong MMIO read count");
    CHECK(bus_jit.io_count == bus_ref.io_count && bus_jit.io_acks == bus_ref.io_acks,
          "jit MMIO/ack counts diverged from the interpreter");
    CHECK((arm920t_get_cpsr(cpu_ref) & 0xffu) == 0x1fu,
          "handler return did not restore system mode with IRQ unmasked");
    gp32_cpu_profile_t profile;
    arm920t_get_cpu_profile(cpu_jit, &profile);
    if (profile.supported && profile.native_backend)
        CHECK(profile.native_block_calls != 0u, "IRQ loop never entered native code");
    teardown_pair();
}

/* Guest STR patches the head before a conditional cache invalidation.
 * MCR ends translation, so this case checks the conservative fallback and
 * subsequent code revalidation; callback_flush below covers an active chain. */
static const uint32_t P_LOOP_SMC[] = {
    0xE3A00000u, /* MOV  r0, #0            ; accumulator                   */
    0xE3A01006u, /* MOV  r1, #6            ; iterations                    */
    0xE3A07B01u, /* MOV  r7, #0x400        ; CODE_ADDR                     */
    0xE2877014u, /* ADD  r7, r7, #0x14     ; r7 = &loop head (patch site)  */
    0xE59F601Cu, /* LDR  r6, [pc, #0x1c]   ; patch word: MOV r2, #0x77     */
    0xE2800001u, /* loop: ADD r0, r0, #1   ; patched to MOV r2, #0x77      */
    0xE3510003u, /*       CMP r1, #3                                      */
    0x05876000u, /*       STREQ r6, [r7]   ; one-time code patch           */
    0x0E070F15u, /*       MCREQ p15,0,r0,c7,c5,0 ; I-cache invalidate      */
    0xE2511001u, /*       SUBS r1, r1, #1                                 */
    0x1AFFFFF9u, /*       BNE  loop        ; conditional backward B         */
    0xEAFFFFFEu, /* B .                                                   */
    0xEAFFFFFEu, /* B .  (padding keeps the literal out of the flow)       */
    0xE3A02077u, /* .word MOV r2, #0x77    ; literal patch word            */
};
static void case_loop_smc_epoch(void) {
    current_case = "loop-smc-epoch";
    setup_pair();
    load_both(P_LOOP_SMC, GP32_ARRAY_COUNT(P_LOOP_SMC));
    run_chunks();
    CHECK(ref_reg(0) == 4u, "patched loop head kept executing stale ADD");
    CHECK(ref_reg(1) == 0u, "SMC loop did not finish");
    CHECK(ref_reg(2) == 0x77u, "patched MOV never executed");
    CHECK(ref_reg(7) == CODE_ADDR + 0x14u, "patch address register wrong");
    CHECK(arm920t_get_pc(cpu_ref) == CODE_ADDR + 0x2Cu,
          "SMC loop did not park on B self");
    CHECK(gp32_ld32le(bus_ptr(&bus_ref, CODE_ADDR + 0x14u, 4u)) == 0xE3A02077u,
          "guest code store did not land");
    CHECK(!memcmp(bus_jit.bios, bus_ref.bios, BIOS_SIZE),
          "code image diverged between engines");
    gp32_cpu_profile_t smc_profile;
    arm920t_get_cpu_profile(cpu_jit, &smc_profile);
    if (smc_profile.supported && smc_profile.native_backend)
        CHECK(smc_profile.native_block_calls != 0u, "SMC loop never entered native code");
    teardown_pair();
}

/* An MMIO callback can invalidate native code while a self-loop is active.
 * Unlike guest MCR (which terminates translation), this load's normal path
 * continues to a chainable backedge. Patch the head and use the public flush. */
static void case_loop_callback_flush(void) {
    const uint32_t program[] = {
        0xe3a00000u, 0xe3a010c8u, 0xe3a04414u,
        0xe2800001u, /* loop: ADD r0,r0,#1; patched after read 50 */
        0xe5942000u, /* LDR r2,[r4]: MMIO callback */
        0xe2511001u, 0x1afffffbu, 0xeafffffeu
    };
    current_case = "loop-callback-flush";
    setup_pair();
    load_both(program, GP32_ARRAY_COUNT(program));
    bus_jit.observe_cpu=cpu_jit; bus_ref.observe_cpu=cpu_ref;
    bus_jit.io_flush_at=bus_ref.io_flush_at=50u;
    /* Stop before the idle B, so the profile covers the memory loop itself. */
    CHECK(arm920t_run(cpu_jit, 150u)==arm920t_run(cpu_ref,150u), "pre-flush budget");
    compare_state();
    CHECK(arm920t_run(cpu_jit, 653u)==arm920t_run(cpu_ref,653u), "flush/loop budget");
    compare_state();
    CHECK(ref_reg(0)==50u && ref_reg(6)==0x77u && ref_reg(1)==0u, "callback patch outcome");
    CHECK(arm920t_get_pc(cpu_ref)==CODE_ADDR+0x1cu, "exact loop retirement");
    CHECK(bus_ref.io_count==200u && bus_jit.io_count==200u, "loop MMIO count");
    CHECK(bus_ref.io_flushes==1u && bus_jit.io_flushes==1u, "one callback flush");
    CHECK(!memcmp(bus_jit.bios,bus_ref.bios,BIOS_SIZE), "callback code image");
    gp32_cpu_profile_t profile;
    arm920t_get_cpu_profile(cpu_jit,&profile);
    if(profile.supported) printf("loop-flush native_calls=%" PRIu64 " native_insns=%" PRIu64 "\n",profile.native_block_calls,profile.native_arm_insns);
    teardown_pair();
}

static void case_loop_callback_trace(void) {
    const uint32_t program[]={0xe3a00000u,0xe3a010c8u,0xe3a04414u,
        0xe2800001u,0xe5942000u,0xe2511001u,0x1afffffbu,0xeafffffeu};
    current_case="loop-callback-trace";
    setup_pair(); load_both(program,GP32_ARRAY_COUNT(program));
    bus_jit.observe_cpu=cpu_jit;bus_ref.observe_cpu=cpu_ref;
    bus_jit.io_trace_at=bus_ref.io_trace_at=3u;
    CHECK(arm920t_run(cpu_jit,43u)==arm920t_run(cpu_ref,43u),"trace callback budget");
    compare_state();
    CHECK(bus_ref.trace_lines==30u,"trace starts after the third MMIO read");
    CHECK(bus_jit.trace_lines==bus_ref.trace_lines,"native callback must enable next-instruction tracing");
    teardown_pair();
}

static void case_nested_framed_leaf(void) {
    /* The inner callee can alias the outer saved return. Check every short
     * budget against single-step execution, not another flattened trace. */
    const uint32_t program[] = {0xeb00001eu, 0xe3a08063u, 0xeafffffeu,
                                0xeafffffeu, 0xeafffffeu};
    for (unsigned single = 0; single < 2u; ++single)
    for (unsigned redirect = 0; redirect < 2u; ++redirect)
    for (unsigned budget = 1; budget <= 14u; ++budget) {
        current_case = redirect ? "nested-leaf-aliased-return" : "nested-leaf-budget";
        setup_pair();
        arm920t_set_trace(cpu_ref, 1, NULL, NULL);
        load_both(program, GP32_ARRAY_COUNT(program));
        set_mem_both(CODE_ADDR + 0x80u, single ? 0xe52de004u : 0xe92d4000u);
        set_mem_both(CODE_ADDR + 0x84u, 0xeb00001du); /* BL inner at +0x100 */
        set_mem_both(CODE_ADDR + 0x88u, 0xe2800001u); /* ADD r0,r0,#1 */
        set_mem_both(CODE_ADDR + 0x8cu, single ? 0xe49df004u : 0xe8bd8000u);
        set_mem_both(CODE_ADDR + 0x100u, 0xe92d4000u);
        set_mem_both(CODE_ADDR + 0x104u, redirect ? 0xe5865000u : 0xe1a05005u);
        set_mem_both(CODE_ADDR + 0x108u, 0xe5940000u);
        set_mem_both(CODE_ADDR + 0x10cu, 0xe8bd8000u);
        set_mem_both(DATA_ADDR, 7u);
        set_reg_both(4u, DATA_ADDR);
        set_reg_both(5u, CODE_ADDR + 0x10u);
        set_reg_both(6u, DATA_ADDR + 0xfcu);
        set_reg_both(8u, 0u);
        set_reg_both(13u, DATA_ADDR + 0x100u);
        CHECK(arm920t_run(cpu_jit, budget) == arm920t_run(cpu_ref, budget),
              "nested leaf short budget");
        compare_state();
        CHECK(arm920t_run(cpu_jit, 30u) == arm920t_run(cpu_ref, 30u),
              "nested leaf continuation");
        compare_state();
        CHECK(ref_reg(0u) == 8u && ref_reg(13u) == DATA_ADDR + 0x100u,
              "nested leaf result and balanced stack");
        CHECK(ref_reg(8u) == (redirect ? 0u : 99u) &&
              ref_reg(15u) == CODE_ADDR + (redirect ? 0x10u : 8u),
              "nested leaf honors loaded outer return");
        teardown_pair();
    }
}

static void case_loop_framed_leaf(void) {
    /* BL -> PUSH LR; MMIO read; POP PC. A callback can replace the real
     * stacked return on repetition 50; chaining must honor that exit. */
    const uint32_t program[]={0xeb00001eu,0xe2511001u,0x1afffffcu,0xeafffffeu};
    for(unsigned redirect=0;redirect<2u;++redirect) {
        current_case=redirect?"loop-framed-changed-return":"loop-framed-leaf";
        setup_pair();load_both(program,GP32_ARRAY_COUNT(program));
        set_mem_both(CODE_ADDR+0x80u,0xe92d4000u);
        set_mem_both(CODE_ADDR+0x84u,0xe5940000u);
        set_mem_both(CODE_ADDR+0x88u,0xe8bd8000u);
        set_reg_both(1u,200u);set_reg_both(4u,IO_ADDR);
        set_reg_both(13u,DATA_ADDR+0x100u);
        bus_jit.observe_cpu=cpu_jit;bus_ref.observe_cpu=cpu_ref;
        bus_jit.io_return_at=bus_ref.io_return_at=redirect?50u:0u;
        CHECK(arm920t_run(cpu_jit,31u)==arm920t_run(cpu_ref,31u),"framed partial budget");
        compare_state();
        CHECK(arm920t_run(cpu_jit,1169u)==arm920t_run(cpu_ref,1169u),"framed remaining budget");
        compare_state();
        CHECK(ref_reg(1)==(redirect?151u:0u),"framed loop iterations");
        CHECK(ref_reg(13)==DATA_ADDR+0x100u,"framed stack restored");
        CHECK(arm920t_get_pc(cpu_ref)==CODE_ADDR+0x0cu,"framed exit PC");
        CHECK(bus_ref.io_count==(redirect?50u:200u) && bus_jit.io_count==bus_ref.io_count,"framed MMIO count");
        gp32_cpu_profile_t profile;arm920t_get_cpu_profile(cpu_jit,&profile);
        if(!redirect && profile.supported) printf("loop-framed native_calls=%" PRIu64 " native_insns=%" PRIu64 "\n",profile.native_block_calls,profile.native_arm_insns);
        teardown_pair();
    }
}

/* A callback-raised IRQ must see the instruction immediately after a complete
 * transfer, including post-index writeback, before the following MOV retires.
 * The handler captures r6 in RAM so delayed IRQ delivery remains observable
 * even after both engines return to the idle branch. */

/* Superblock trace extension: an unconditional BL the leaf collectors cannot
 * flatten continues the same native trace inside its callee, and the callee
 * return continues at the decoded call continuation only through the guarded
 * stack return or a BL link that no decoded instruction could have replaced.
 * Each program is compared against the traced portable oracle at ragged
 * budgets, so the appended PCs, an aliased stack return, a cross-page callee,
 * the LR proof and a modified appended instruction are all checked against
 * instruction-by-instruction execution. */
static uint32_t cond_branch(uint32_t cond, uint32_t pc, uint32_t target) {
    return 0x0a000000u | (cond << 28) | (((target - pc - 8u) >> 2) & 0x00ffffffu);
}

/* BL at CODE_ADDR+4 into CODE_ADDR+0x40 with a conditional branch inside the
 * callee, so no leaf collector can flatten it. r0 ends at 16 + ret_imm + 5. */
static void superblock_program(uint32_t ret_imm) {
    const uint32_t callee = CODE_ADDR + 0x40u;
    const uint32_t program[] = {0xe3a00000u, cache_branch(CODE_ADDR + 4u, callee, 1),
                                0xe2800005u, 0xeafffffeu};
    load_both(program, GP32_ARRAY_COUNT(program));
    set_mem_both(callee + 0x00u, 0xe92d4000u);   /* PUSH {lr} */
    set_mem_both(callee + 0x04u, 0xe3500000u);   /* CMP r0,#0 */
    set_mem_both(callee + 0x08u, 0x03a00010u);   /* MOVEQ r0,#16 */
    set_mem_both(callee + 0x0cu, 0x12800020u);   /* ADDNE r0,r0,#32 */
    set_mem_both(callee + 0x10u,
                 cond_branch(0x1u, callee + 0x10u, callee + 0x1cu)); /* BNE */
    set_mem_both(callee + 0x14u, 0xe2800000u | ret_imm);
    set_mem_both(callee + 0x18u, 0xe8bd8000u);   /* POP {pc} */
    set_mem_both(callee + 0x1cu, 0xe2800002u);
    set_mem_both(callee + 0x20u, 0xe8bd8000u);
    set_reg_both(13u, DATA_ADDR + 0x100u);
}

static void case_superblock_trace(void) {
    const uint32_t callee = CODE_ADDR + 0x40u;
    current_case = "superblock-call-return";
    /* Ragged budgets exercise every partial-trace exit; the complete
     * call/return and the caller suffix must still match the oracle. */
    for (unsigned budget = 1; budget <= 24u; ++budget) {
        setup_pair();
        arm920t_set_trace(cpu_ref, 1, NULL, NULL);
        superblock_program(1u);
        CHECK(arm920t_run(cpu_jit, budget) == arm920t_run(cpu_ref, budget),
              "superblock short budget");
        compare_state();
        CHECK(arm920t_run(cpu_jit, 40u) == arm920t_run(cpu_ref, 40u),
              "superblock remaining budget");
        compare_state();
        CHECK(ref_reg(0u) == 22u, "superblock callee result");
        CHECK(ref_reg(13u) == DATA_ADDR + 0x100u, "superblock stack balanced");
        CHECK(ref_reg(15u) == CODE_ADDR + 0x0cu, "superblock exit PC");
        teardown_pair();
    }
    /* One complete run on a fresh profile: the BL, callee body and caller
     * suffix must be a single extended trace, so a translation without the
     * extension would need several native block entries. */
    current_case = "superblock-single-trace";
    setup_pair();
    arm920t_set_trace(cpu_ref, 1, NULL, NULL);
    superblock_program(1u);
    arm920t_reset_cpu_profile(cpu_jit);
    CHECK(arm920t_run(cpu_jit, 12u) == arm920t_run(cpu_ref, 12u), "superblock single trace run");
    compare_state();
    {
        gp32_cpu_profile_t profile;
        arm920t_get_cpu_profile(cpu_jit, &profile);
        if (profile.supported && !portable_callbacks) {
            CHECK(profile.native_block_calls == 1u, "call and return were not one trace");
            CHECK(profile.native_arm_insns >= 10u, "extended trace retired too few instructions");
        }
    }
    teardown_pair();
    /* A callback that replaces the real stacked return must leave the trace at
     * the loaded target instead of following the decoded continuation. */
    for (unsigned redirect = 0; redirect < 2u; ++redirect) {
        const uint32_t inner = CODE_ADDR + 0x40u;
        current_case = redirect ? "superblock-aliased-return" : "superblock-plain-return";
        setup_pair();
        arm920t_set_trace(cpu_ref, 1, NULL, NULL);
        {
            const uint32_t program[] = {0xe3a00000u, cache_branch(CODE_ADDR + 4u, inner, 1),
                                        0xe2800007u, 0xeafffffeu};
            load_both(program, GP32_ARRAY_COUNT(program));
        }
        set_mem_both(inner + 0x00u, 0xe92d4000u);   /* PUSH {lr} */
        set_mem_both(inner + 0x04u, 0xe5942000u);   /* LDR r2,[r4]: MMIO callback */
        set_mem_both(inner + 0x08u, 0xe3500000u);   /* CMP r0,#0 */
        set_mem_both(inner + 0x0cu,
                     cond_branch(0x1u, inner + 0x0cu, inner + 0x18u)); /* BNE */
        set_mem_both(inner + 0x10u, 0xe2800001u);   /* ADD r0,r0,#1 */
        set_mem_both(inner + 0x14u, 0xe8bd8000u);   /* POP {pc} */
        set_mem_both(inner + 0x18u, 0xe2800002u);
        set_mem_both(inner + 0x1cu, 0xe8bd8000u);
        set_reg_both(0u, 0u);
        set_reg_both(4u, IO_ADDR);
        set_reg_both(13u, DATA_ADDR + 0x100u);
        bus_jit.observe_cpu = cpu_jit;
        bus_ref.observe_cpu = cpu_ref;
        bus_jit.io_return_at = bus_ref.io_return_at = redirect ? 1u : 0u;
        CHECK(arm920t_run(cpu_jit, 40u) == arm920t_run(cpu_ref, 40u),
              "superblock guarded return budget");
        compare_state();
        CHECK(ref_reg(0u) == (redirect ? 1u : 8u), "superblock guarded return result");
        CHECK(ref_reg(15u) == CODE_ADDR + 0x0cu, "superblock guarded return PC");
        CHECK(bus_ref.io_count == 1u && bus_jit.io_count == bus_ref.io_count,
              "superblock callee MMIO count");
        teardown_pair();
    }
    /* MOV pc,lr is followed only while the BL link is provably intact; the
     * clobbered variant would run the caller suffix if it were stitched. */
    for (unsigned clobber = 0; clobber < 2u; ++clobber) {
        const uint32_t inner = CODE_ADDR + 0x3cu;
        current_case = clobber ? "superblock-clobbered-link" : "superblock-link-return";
        setup_pair();
        arm920t_set_trace(cpu_ref, 1, NULL, NULL);
        {
            const uint32_t program[] = {0xe3a01004u, cache_branch(CODE_ADDR + 4u, inner, 1),
                                        0xe2811008u, 0xeafffffeu};
            load_both(program, GP32_ARRAY_COUNT(program));
        }
        set_mem_both(inner + 0x00u, 0xe5932000u);   /* LDR r2,[r3]: not a data leaf */
        set_mem_both(inner + 0x04u, clobber ? 0xe3a0e050u : 0xe0811002u);
        set_mem_both(inner + 0x08u, 0xe1a0f00eu);   /* MOV pc,lr */
        set_reg_both(1u, 4u);
        set_reg_both(3u, DATA_ADDR);
        set_reg_both(13u, DATA_ADDR + 0x100u);
        CHECK(arm920t_run(cpu_jit, 40u) == arm920t_run(cpu_ref, 40u), "superblock LR return");
        compare_state();
        CHECK(ref_reg(1u) == (clobber ? 4u : 12u), "superblock LR return result");
        /* The clobbered link is the absolute 0x50, where the BIOS filler parks. */
        CHECK(ref_reg(15u) == (clobber ? 0x50u : CODE_ADDR + 0x0cu), "superblock LR return PC");
        teardown_pair();
    }
    /* A callee on the next 1 KiB code page keeps its own page anchor. */
    current_case = "superblock-cross-page";
    setup_pair();
    arm920t_set_trace(cpu_ref, 1, NULL, NULL);
    {
        const uint32_t inner = CODE_ADDR + 0x400u;
        const uint32_t program[] = {0xe3a00000u, cache_branch(CODE_ADDR + 4u, inner, 1),
                                    0xe2800003u, 0xeafffffeu};
        load_both(program, GP32_ARRAY_COUNT(program));
        set_mem_both(inner + 0x00u, 0xe92d4000u);
        set_mem_both(inner + 0x04u, 0xe3500000u);
        set_mem_both(inner + 0x08u, cond_branch(0x1u, inner + 0x08u, inner + 0x14u));
        set_mem_both(inner + 0x0cu, 0xe2800004u);
        set_mem_both(inner + 0x10u, 0xe8bd8000u);
        set_mem_both(inner + 0x14u, 0xe2800008u);
        set_mem_both(inner + 0x18u, 0xe8bd8000u);
        set_reg_both(13u, DATA_ADDR + 0x100u);
        CHECK(arm920t_run(cpu_jit, 40u) == arm920t_run(cpu_ref, 40u), "superblock cross page");
        compare_state();
        CHECK(ref_reg(0u) == 7u, "superblock cross-page result");
    }
    teardown_pair();
    /* Two nested BL levels inside one trace, each with its own continuation. */
    current_case = "superblock-nested-levels";
    setup_pair();
    arm920t_set_trace(cpu_ref, 1, NULL, NULL);
    {
        const uint32_t mid = CODE_ADDR + 0x40u, deep = CODE_ADDR + 0x80u;
        const uint32_t program[] = {0xe3a00000u, cache_branch(CODE_ADDR + 4u, mid, 1),
                                    0xe2800001u, 0xeafffffeu};
        load_both(program, GP32_ARRAY_COUNT(program));
        set_mem_both(mid + 0x00u, 0xe92d4000u);
        set_mem_both(mid + 0x04u, cache_branch(mid + 0x04u, deep, 1));
        set_mem_both(mid + 0x08u, 0xe2800002u);
        set_mem_both(mid + 0x0cu, 0xe8bd8000u);
        set_mem_both(deep + 0x00u, 0xe92d4000u);
        set_mem_both(deep + 0x04u, 0xe3500000u);
        set_mem_both(deep + 0x08u, cond_branch(0x1u, deep + 0x08u, deep + 0x14u));
        set_mem_both(deep + 0x0cu, 0xe2800004u);
        set_mem_both(deep + 0x10u, 0xe8bd8000u);
        set_mem_both(deep + 0x14u, 0xe2800008u);
        set_mem_both(deep + 0x18u, 0xe8bd8000u);
        set_reg_both(13u, DATA_ADDR + 0x100u);
        CHECK(arm920t_run(cpu_jit, 60u) == arm920t_run(cpu_ref, 60u), "superblock nested levels");
        compare_state();
        CHECK(ref_reg(0u) == 7u, "superblock nested result");
        CHECK(ref_reg(13u) == DATA_ADDR + 0x100u, "superblock nested stack balanced");
    }
    teardown_pair();
    /* The call/return inside a counted loop must stay exact over repetitions. */
    current_case = "superblock-call-loop";
    setup_pair();
    arm920t_set_trace(cpu_ref, 1, NULL, NULL);
    {
        const uint32_t program[] = {0xe3a01032u, 0xe3a00000u,
                                    cache_branch(CODE_ADDR + 8u, callee, 1),
                                    0xe2511001u, cond_branch(0x1u, CODE_ADDR + 0x10u, CODE_ADDR + 8u),
                                    0xeafffffeu};
        load_both(program, GP32_ARRAY_COUNT(program));
        set_mem_both(callee + 0x00u, 0xe92d4000u);
        set_mem_both(callee + 0x04u, 0xe3500000u);
        set_mem_both(callee + 0x08u, cond_branch(0x0u, callee + 0x08u, callee + 0x14u));
        set_mem_both(callee + 0x0cu, 0xe2800003u);
        set_mem_both(callee + 0x10u, 0xe8bd8000u);
        set_mem_both(callee + 0x14u, 0xe2800007u);
        set_mem_both(callee + 0x18u, 0xe8bd8000u);
        set_reg_both(13u, DATA_ADDR + 0x100u);
        CHECK(arm920t_run(cpu_jit, 700u) == arm920t_run(cpu_ref, 700u), "superblock call loop");
        compare_state();
        /* r0 == 0 adds 7 through the taken side exit, the remaining 49
         * repetitions add 3 on the fall-through path: 7 + 49 * 3. */
        CHECK(ref_reg(0u) == 154u, "superblock call loop result");
        CHECK(ref_reg(13u) == DATA_ADDR + 0x100u, "superblock call loop stack balanced");
    }
    teardown_pair();
    /* Rewriting an appended callee instruction must invalidate the trace and
     * re-fetch it: 16 + 9 + 5 = 30. */
    current_case = "superblock-appended-modified";
    setup_pair();
    arm920t_set_trace(cpu_ref, 1, NULL, NULL);
    superblock_program(1u);
    CHECK(arm920t_run(cpu_jit, 40u) == arm920t_run(cpu_ref, 40u), "superblock pre-modification");
    compare_state();
    CHECK(ref_reg(0u) == 22u, "superblock pre-modification result");
    set_mem_both(callee + 0x14u, 0xe2800009u);   /* ADD r0,r0,#9 */
    arm920t_flush_jit(cpu_jit);
    set_reg_both(0u, 0u);                        /* re-enter the caller entry */
    set_reg_both(13u, DATA_ADDR + 0x100u);
    set_reg_both(15u, CODE_ADDR);
    CHECK(arm920t_run(cpu_jit, 40u) == arm920t_run(cpu_ref, 40u), "superblock post-modification");
    compare_state();
    CHECK(ref_reg(0u) == 30u, "superblock modified appended instruction");
    teardown_pair();
}

static void case_callback_irq_commit(void) {
    const uint32_t transfers[] = {
        0xe4942004u, 0xe4d42004u, /* LDR/LDRB r2,[r4],#4 */
        0xe0d420b4u, 0xe0d420d4u, 0xe0d420f4u, /* LDRH/LDRSB/LDRSH */
        0xe4842004u, 0xe4c42004u, 0xe0c420b4u, /* STR/STRB/STRH */
    };
    const uint32_t handler[] = {
        0xe5896000u, /* STR r6,[r9]: capture state before next guest MOV */
        0xe5883000u, /* STR r3,[r8]: acknowledge IRQ */
        0xe25ef004u, /* SUBS pc,lr,#4: resume after the transfer */
    };
    current_case = "callback-irq-commit";
    for (unsigned t = 0; t < GP32_ARRAY_COUNT(transfers); ++t) {
        uint32_t program[] = {transfers[t], 0xe3a06001u, 0xeafffffeu};
        setup_pair();
        arm920t_set_trace(cpu_ref, 1, NULL, NULL); /* instruction-by-instruction oracle */
        load_both(program, GP32_ARRAY_COUNT(program));
        for (unsigned i = 0; i < GP32_ARRAY_COUNT(handler); ++i)
            set_mem_both(0x18u + 4u * i, handler[i]);
        set_reg_both(2u, 0x12345678u);
        set_reg_both(4u, IO_ADDR);
        set_reg_both(8u, IO_ADDR + 4u);
        set_reg_both(9u, DATA_ADDR);
        set_mem_both(DATA_ADDR, 0xdeadbeefu);
        arm920t_set_cpsr(cpu_jit, 0x1fu);
        arm920t_set_cpsr(cpu_ref, 0x1fu);
        bus_jit.observe_cpu = cpu_jit; bus_ref.observe_cpu = cpu_ref;
        bus_jit.io_raise_at = bus_ref.io_raise_at = 1u;
        CHECK(arm920t_run(cpu_jit, 32u) == arm920t_run(cpu_ref, 32u), "IRQ transfer budget");
        compare_state();
        CHECK(gp32_ld32le(bus_ptr(&bus_ref, DATA_ADDR, 4u)) == 0u,
              "oracle IRQ must precede the following MOV");
        CHECK(gp32_ld32le(bus_ptr(&bus_jit, DATA_ADDR, 4u)) == 0u,
              "IRQ must precede the following MOV");
        CHECK(ref_reg(4u) == IO_ADDR + 4u && arm920t_get_reg(cpu_jit, 4u) == IO_ADDR + 4u,
              "post-index writeback must commit once");
        CHECK(bus_ref.io_count == 1u && bus_jit.io_count == 1u &&
              bus_ref.io_acks == 1u && bus_jit.io_acks == 1u, "one transfer and IRQ acknowledge");
        CHECK(bus_ref.io_pc[0] == CODE_ADDR + 4u && bus_jit.io_pc[0] == CODE_ADDR + 4u,
              "callback sees transfer PC+4");
        CHECK(ref_reg(6u) == 1u && arm920t_get_pc(cpu_ref) == CODE_ADDR + 8u,
              "IRQ returns to the instruction after the committed transfer");
        gp32_cpu_profile_t profile;
        arm920t_get_cpu_profile(cpu_jit, &profile);
        if (profile.supported && portable_callbacks) {
            CHECK(profile.native_block_calls == 0u, "portable callback uses no native code");
            CHECK(profile.block_interp_arm_insns != 0u, "portable callback executes decoded block");
        } else if (profile.supported && profile.native_backend)
            CHECK(profile.native_block_calls != 0u, "regression requires native execution");
        printf("callback-irq transfer=%08" PRIx32 " captured=%08" PRIx32
               " pc=%08" PRIx32 " wb=%08" PRIx32 " native_calls=%" PRIu64 "\n",
               transfers[t], gp32_ld32le(bus_ptr(&bus_jit, DATA_ADDR, 4u)),
               bus_jit.io_pc[0], arm920t_get_reg(cpu_jit, 4u), profile.native_block_calls);
        teardown_pair();
    }
}

/* A deferred MMIO bus can yield on any store lane. Complete this ARM
 * instruction's ordered stores and writeback, then return exactly its prefix
 * so the SoC can tick before committing the queued device writes. */
static void case_block_callback_yield(void) {
    for (unsigned at = 1u; at <= 3u; at += 2u) {
        current_case = "block-callback-yield";
        setup_pair();
        const uint32_t program[] = {0xe8a40007u, 0xe3a06001u, 0xeafffffeu};
        load_both(program, GP32_ARRAY_COUNT(program));
        for (unsigned r = 0; r < 3u; ++r) set_reg_both(r, 0x22220000u + r);
        set_reg_both(4u, IO_ADDR);
        bus_jit.observe_cpu = cpu_jit; bus_ref.observe_cpu = cpu_ref;
        bus_jit.block_io = bus_ref.block_io = 1u;
        bus_jit.block_effect = bus_ref.block_effect = 4u;
        bus_jit.block_at = bus_ref.block_at = at;
        CHECK(arm920t_run(cpu_jit, 128u) == 1u && arm920t_run(cpu_ref, 128u) == 1u,
              "MMIO yield returns one completed instruction");
        compare_state();
        CHECK(bus_jit.block_count == 3u && bus_ref.block_count == 3u,
              "MMIO yield completes every store lane");
        for (unsigned i = 0; i < 3u; ++i) {
            CHECK(bus_jit.block_addr[i] == IO_ADDR + 4u * i &&
                  bus_jit.block_value[i] == 0x22220000u + i &&
                  bus_jit.block_base[i] == IO_ADDR, "yield lane order/value/base");
        }
        CHECK(ref_reg(4u) == IO_ADDR + 12u && ref_reg(6u) == 0u &&
              arm920t_get_pc(cpu_jit) == CODE_ADDR + 4u, "yield commits WB before next MOV");
        gp32_cpu_profile_t profile;
        arm920t_get_cpu_profile(cpu_jit, &profile);
        if (profile.supported && profile.native_backend)
            CHECK(profile.native_block_calls == 1u, "yield exercises native helper exit");
        CHECK(arm920t_run(cpu_jit, 1u) == 1u && arm920t_run(cpu_ref, 1u) == 1u,
              "yield resumes next instruction");
        compare_state();
        CHECK(ref_reg(6u) == 1u && bus_jit.block_count == 3u, "yield does not replay stores");
        teardown_pair();
    }
}

/* Whole BLOCK completion precedes IRQ or code invalidation exits. Trace on
 * the oracle forces instruction boundaries; jit=0 alone can batch portable ops. */
static void case_block_callback_exit(void) {
    const uint32_t handler[] = {
        0xe5896000u, 0xe5894004u, 0xe5892008u, /* capture next-MOV, WB, last lane */
        0xe5883000u, 0xe25ef004u,             /* IRQ ack and return */
    };
    unsigned cases = 0;
    for (unsigned effect = 1u; effect <= 3u; ++effect)
    for (unsigned load = 0; load < 2u; ++load)
    for (unsigned mode = 0; mode < 4u; ++mode)
    for (unsigned at = 1u; at <= 3u; at += 2u) {
        unsigned p = mode & 1u, u = mode >> 1;
        uint32_t base = u ? IO_ADDR - (p ? 4u : 0u) : IO_ADDR + (p ? 12u : 8u);
        uint32_t final_base = u ? base + 12u : base - 12u;
        uint32_t transfer = block_insn(p, u, 0, 1, load, 4, 7u);
        const uint32_t program[] = {transfer, 0xe3a06001u, 0xeafffffeu};
        current_case = effect == 1u ? "block-callback-irq" :
                       effect == 2u ? "block-callback-flush" : "block-callback-disable";
        setup_pair();
        arm920t_set_trace(cpu_ref, 1, NULL, NULL);
        load_both(program, GP32_ARRAY_COUNT(program));
        for (unsigned i = 0; i < GP32_ARRAY_COUNT(handler); ++i)
            set_mem_both(0x18u + 4u * i, handler[i]);
        for (unsigned r = 0; r < 3u; ++r) set_reg_both(r, 0x22220000u + r);
        set_reg_both(4u, base); set_reg_both(8u, IO_ADDR + 0x40u);
        set_reg_both(9u, DATA_ADDR);
        arm920t_set_cpsr(cpu_jit, 0x1fu); arm920t_set_cpsr(cpu_ref, 0x1fu);
        bus_jit.observe_cpu = cpu_jit; bus_ref.observe_cpu = cpu_ref;
        bus_jit.block_io = bus_ref.block_io = 1u;
        bus_jit.block_effect = bus_ref.block_effect = effect;
        bus_jit.block_at = bus_ref.block_at = at;
        CHECK(arm920t_run(cpu_jit, 32u) == arm920t_run(cpu_ref, 32u), "block exit budget");
        compare_state();
        CHECK(bus_jit.block_count == 3u && bus_ref.block_count == 3u, "all lanes commit once");
        CHECK(!memcmp(bus_jit.block_addr, bus_ref.block_addr, sizeof(bus_ref.block_addr)), "lane ordering");
        CHECK(!memcmp(bus_jit.block_value, bus_ref.block_value, sizeof(bus_ref.block_value)), "lane values");
        for (unsigned i = 0; i < 3u; ++i) {
            CHECK(bus_ref.block_addr[i] == IO_ADDR + 4u * i, "oracle ascending lane addresses");
            CHECK(bus_ref.block_pc[i] == CODE_ADDR + 4u && bus_jit.block_pc[i] == CODE_ADDR + 4u, "lane PC+4");
            CHECK(bus_ref.block_base[i] == base && bus_jit.block_base[i] == base, "writeback follows all callbacks");
        }
        CHECK(arm920t_get_reg(cpu_jit, 4u) == final_base && ref_reg(4u) == final_base, "one final writeback");
        if (effect == 1u) {
            CHECK(gp32_ld32le(bus_ptr(&bus_jit, DATA_ADDR, 4u)) == 0u, "IRQ precedes next MOV");
            CHECK(gp32_ld32le(bus_ptr(&bus_jit, DATA_ADDR + 4u, 4u)) == final_base, "IRQ sees committed writeback");
            CHECK(gp32_ld32le(bus_ptr(&bus_jit, DATA_ADDR + 8u, 4u)) == (load ? 0x11110002u : 0x22220002u), "IRQ sees final lane");
            CHECK(bus_jit.io_acks == 1u && bus_ref.io_acks == 1u, "IRQ acknowledged once");
            CHECK(ref_reg(6u) == 1u, "IRQ returns after transfer");
        } else CHECK(ref_reg(6u) == 0x77u && arm920t_get_reg(cpu_jit, 6u) == 0x77u, "next instruction revalidated");
        CHECK(arm920t_get_pc(cpu_jit) == CODE_ADDR + 8u, "exit PC");
        gp32_cpu_profile_t profile;
        arm920t_get_cpu_profile(cpu_jit, &profile);
        if (profile.supported && portable_callbacks) {
            CHECK(profile.native_block_calls == 0u, "portable callback uses no native code");
            CHECK(profile.block_interp_arm_insns != 0u, "portable callback executes decoded block");
        } else if (profile.supported && profile.native_backend)
            CHECK(profile.native_block_calls != 0u, "regression requires native execution");
        printf("block-exit effect=%u insn=%08" PRIx32 " lane=%u count=%u r6=%u wb=%08" PRIx32 " native_calls=%" PRIu64 "\n",
               effect, transfer, at, bus_jit.block_count, arm920t_get_reg(cpu_jit, 6u),
               arm920t_get_reg(cpu_jit, 4u), profile.native_block_calls);
        ++cases;
        teardown_pair();
    }
    printf("block callback cases=%u\n", cases);
}

/* Data-cache maintenance updates c7 but must not end a decoded trace or
 * invalidate executable code. I-cache/MMU operations retain their exits. */
static void case_cache_maintenance_native(void) {
    const uint32_t program[] = {
        0xee070f5eu, /* MCR p15,0,r0,c7,c14,2: clean/invalidate by index */
        0xee171f10u, /* MRC p15,0,r1,c7,c0,0: observe modeled c7 value */
        0xe2800001u, /* ADD r0,r0,#1 */
        0xe2522001u, /* SUBS r2,r2,#1 */
        0x1afffffau, /* BNE start */
        0xee070f9au, /* MCR p15,0,r0,c7,c10,4: drain write buffer */
        0x0e073f16u, /* MCREQ p15,0,r3,c7,c6,0 */
        0x1e074f16u, /* MCRNE: failed condition must not write c7 */
        0xee175f10u, /* MRC r5,c7 */
        0xeafffffeu,
    };
    current_case = "cache-maintenance-native";
    setup_pair();
    load_both(program, GP32_ARRAY_COUNT(program));
    set_reg_both(0u, 0x12340000u);
    set_reg_both(2u, 200u);
    set_reg_both(3u, 0x43210000u);
    set_reg_both(4u, 0xbad00000u);
    const uint32_t budgets[] = {1u, 2u, 3u, 17u, 300u, 701u};
    for (unsigned i = 0; i < GP32_ARRAY_COUNT(budgets); ++i) {
        CHECK(arm920t_run(cpu_jit, budgets[i]) == arm920t_run(cpu_ref, budgets[i]), "maintenance cycle budget");
        compare_state();
        CHECK(arm920t_get_cp15(cpu_jit, 7u) == arm920t_get_cp15(cpu_ref, 7u), "maintenance c7 matches interpreter");
    }
    CHECK(ref_reg(0u) == 0x123400c8u && ref_reg(1u) == 0x123400c7u,
          "maintenance loop completes every index");
    CHECK(ref_reg(5u) == 0x43210000u, "conditional maintenance preserves c7");
    gp32_cpu_profile_t profile;
    arm920t_get_cpu_profile(cpu_jit, &profile);
    if (profile.supported && profile.native_backend)
        CHECK(profile.helper_op_kinds[10u] == 0u, "maintenance stays in native code");
    teardown_pair();
}

/* Focused checked-access regression: no polling, one callback per fixture,
 * ragged budgets elsewhere, and an instruction-at-a-time reference here.
 * On A64 the profile must also prove that whole-op dispatch was removed. */
static void case_checked_access(void) {
    /* dir: 0 = halfword helper (no single counters), 1 = single load, 2 = single store;
     * direct: 1 = word transfer the A64 direct identity-I/O path owns. */
    const struct { uint32_t insn; unsigned effect; uint32_t base; unsigned dir; unsigned direct; } cases[] = {
        {0xe4942004u, 1u, IO_ADDR, 1u, 1u},       /* LDR post / SoC yield */
        {0xe4d42004u, 2u, IO_ADDR, 1u, 0u},       /* LDRB post / redirected PC */
        {0xe0d420f4u, 3u, IO_ADDR, 0u, 0u},       /* LDRSH post / NZCV change */
        {0xe5d42000u, 4u, IO_ADDR, 1u, 0u},       /* LDRB / Thumb change */
        {0xe7a42081u, 5u, IO_ADDR, 2u, 1u},       /* STR shifted offset! / flush */
        {0xe5642001u, 6u, IO_ADDR + 1u, 2u, 0u},  /* STRB negative offset! / disable */
        {0xe0c420b4u, 7u, IO_ADDR, 0u, 0u},       /* STRH post / trace */
        {0xe13440b1u, 8u, IO_ADDR + 2u, 0u, 0u},  /* LDRH r4,[r4,-r1]! / Rd==Rn */
        {0xe1d420d0u, 9u, IO_ADDR, 0u, 0u},       /* LDRSB / FIQ */
        {0xe5942000u, 0u, IO_ADDR + 1u, 1u, 1u},  /* unaligned word / rotate */
        {0xe0d420b4u, 1u, IO_ADDR, 0u, 0u},       /* LDRH post / yield */
        {0xe4842004u, 1u, IO_ADDR, 2u, 1u},       /* STR post / yield */
    };
    for (unsigned i = 0; i < GP32_ARRAY_COUNT(cases); ++i) {
        current_case = "checked-access";
        setup_pair();
        arm920t_set_trace(cpu_ref, 1, NULL, NULL);
        const uint32_t program[] = {cases[i].insn, 0xe3a06001u, 0xeafffffeu};
        load_both(program, GP32_ARRAY_COUNT(program));
        if (cases[i].effect == 3u) set_mem_both(CODE_ADDR + 4u, 0x03a06001u); /* MOVEQ */
        if (cases[i].effect == 4u) set_mem_both(CODE_ADDR + 4u, 0xe7fe2601u); /* Thumb MOV; B . */
        set_mem_both(0x1cu, 0xe5806000u); /* FIQ captures r6 before next MOV */
        set_mem_both(0x20u, 0xe3a06077u);
        set_mem_both(0x24u, 0xeafffffeu);
        set_reg_both(0u, DATA_ADDR); set_reg_both(1u, 2u);
        set_reg_both(2u, 0x1234ff80u); set_reg_both(4u, cases[i].base);
        arm920t_set_cpsr(cpu_jit, 0x13u); arm920t_set_cpsr(cpu_ref, 0x13u);
        bus_jit.observe_cpu = cpu_jit; bus_ref.observe_cpu = cpu_ref;
        bus_jit.mem_probe = bus_ref.mem_probe = 1u;
        bus_jit.mem_effect = bus_ref.mem_effect = cases[i].effect;
        uint32_t dj = arm920t_run(cpu_jit, 32u), dr = arm920t_run(cpu_ref, 32u);
        CHECK(dj == dr, "checked-access budget");
        compare_state();
        CHECK(bus_jit.mem_calls == 1u && bus_ref.mem_calls == 1u, "one bus access");
        CHECK(bus_jit.mem_addr == bus_ref.mem_addr && bus_jit.mem_value == bus_ref.mem_value,
              "callback address/store source");
        CHECK(bus_jit.mem_pc == CODE_ADDR + 4u && bus_ref.mem_pc == CODE_ADDR + 4u &&
              bus_jit.mem_base == cases[i].base && bus_ref.mem_base == cases[i].base,
              "callback PC and pre-writeback base");
        if (cases[i].effect == 1u) {
            CHECK(dj == 1u && ref_reg(6u) == 0u && ref_reg(4u) == IO_ADDR + 4u,
                  "yield completes transfer and WB before next instruction");
            CHECK(arm920t_run(cpu_jit, 1u) == arm920t_run(cpu_ref, 1u), "yield resumes");
            compare_state();
            CHECK(bus_jit.mem_calls == 1u && ref_reg(6u) == 1u, "yield never replays access");
        }
        if (i == 2u) CHECK(ref_reg(2u) == 0xffffff80u && ref_reg(6u) == 1u, "signed half/NZCV");
        if (i == 7u) CHECK(ref_reg(4u) == IO_ADDR, "Rd==Rn WB follows loaded value");
        if (i == 8u) CHECK(gp32_ld32le(bus_ref.ram + 0x1000u) == 0u, "FIQ before next MOV");
        if (i == 9u) CHECK(ref_reg(2u) == 0x808877ffu && ref_reg(4u) == 0x12345678u,
                           "unaligned rotation and callback base mutation");
        gp32_cpu_profile_t profile;
        arm920t_get_cpu_profile(cpu_jit, &profile);
        if (profile.supported && profile.native_backend == 2u) {
            const uint64_t access_helpers =
                profile.helper_ld_word + profile.helper_ld_byte + profile.helper_ld_half +
                profile.helper_ld_sbyte + profile.helper_ld_shalf + profile.helper_st_word +
                profile.helper_st_byte + profile.helper_st_half;
            CHECK(profile.native_block_calls != 0u, "checked-access native coverage");
            CHECK(profile.helper_op_kinds[5u] == 0u && profile.helper_op_kinds[6u] == 0u,
                  "checked access avoids whole-op HALF/SINGLE_DT dispatch");
            if (cases[i].direct) {
                /* Word transfers whose RAM guard failed take the direct
                 * identity-I/O path: the bus observation above already proved
                 * the one real access, so no access helper and no slow-reason
                 * probe may be entered for it. */
                CHECK(access_helpers == 0u, "direct I/O word path bypasses the access helpers");
                CHECK(profile.slow_bail_single_tlbmiss == 0u &&
                      profile.slow_bail_single_nonram == 0u &&
                      profile.single_nonram_regions[IO_ADDR >> 24] == 0u,
                      "direct I/O word path is not attributed as a slow bail");
                CHECK(profile.native_arm_insns != 0u, "direct I/O word path runs native code");
            } else if (cases[i].dir) {
                /* Byte transfers keep the checked single-transfer helper and
                 * its pre-access attribution: MMU is off so the probe always
                 * hits, and the IO window lands on the non-RAM counter and its
                 * record. */
                CHECK(access_helpers == 1u, "checked byte access reaches one access helper");
                CHECK(profile.slow_bail_single_tlbmiss == 0u &&
                      profile.slow_bail_single_nonram == 1u &&
                      profile.single_nonram_regions[IO_ADDR >> 24] == 1u,
                      "checked access attributes the non-RAM single transfer");
                const gp32_memory_profile_t *rec = &profile.single_nonram_addresses[0];
                CHECK(rec->physical_address == bus_jit.mem_addr &&
                      rec->first_pc == CODE_ADDR && rec->reads + rec->writes == 1u &&
                      (cases[i].dir == 1u ? rec->reads : rec->writes) == 1u,
                      "non-RAM record tracks the one real bus access");
            } else {
                /* Halfword ops share the checked helper but are never single
                 * transfers; only the generic bail reason may advance. */
                CHECK(access_helpers == 1u, "checked halfword access reaches one access helper");
                CHECK(profile.slow_bail_single_nonram == 0u &&
                      profile.single_nonram_regions[IO_ADDR >> 24] == 0u &&
                      (profile.single_nonram_addresses[0].reads |
                       profile.single_nonram_addresses[0].writes) == 0u &&
                      profile.slow_bail_other != 0u,
                      "checked halfword never enters single counters");
            }
        }
        printf("checked-access %u insn=%08" PRIx32 " effect=%u cycles=%u\n",
               i, cases[i].insn, cases[i].effect, dj);
        teardown_pair();
    }
}

/* A physical fallback must not reinterpret its PA as a second guest VA. */
static void case_physical_access_once(void) {
    const uint32_t ttb = RAM_BASE + 0x4000u, va = 0x10000000u, pa = 0x13000000u;
    const uint32_t accesses[] = {0xe5901000u, 0xe5d01000u, 0xe5801000u, 0xe5c01000u};
    for (unsigned native = 0; native < 2u; ++native) {
        for (unsigned alias = 0; alias < 2u; ++alias) {
            for (unsigned op = 0; op < GP32_ARRAY_COUNT(accesses); ++op) {
                current_case = "physical-access-translated-once";
                setup_pair();
                arm920t_set_jit(cpu_jit, (int)native);
                arm920t_set_trace(cpu_ref, 1, NULL, NULL);
                const uint32_t program[] = {
                    0xee02af10u, 0xee01bf10u, accesses[op], 0xeafffffeu,
                };
                load_both(program, GP32_ARRAY_COUNT(program));
                set_mem_both(ttb, 2u); /* BIOS identity */
                set_mem_both(ttb + ((va >> 18) & 0x3ffcu), pa | 2u);
                /* A second translation would either read/write RAM instead of
                 * the physical bus or spuriously update the fault registers. */
                set_mem_both(ttb + ((pa >> 18) & 0x3ffcu), alias ? RAM_BASE | 2u : 0u);
                set_reg_both(0u, va); set_reg_both(1u, 0x12345678u);
                set_reg_both(10u, ttb); set_reg_both(11u, 1u);
                bus_jit.observe_cpu = cpu_jit; bus_ref.observe_cpu = cpu_ref;
                bus_jit.mem_probe = bus_ref.mem_probe = 1u;
                CHECK(arm920t_run(cpu_jit, 2u) == 2u && arm920t_run(cpu_ref, 2u) == 2u,
                      "physical access MMU setup");
                CHECK(arm920t_run(cpu_jit, 32u) == 32u && arm920t_run(cpu_ref, 32u) == 32u,
                      "physical access execution");
                compare_state();
                CHECK(bus_jit.mem_calls == 1u && bus_ref.mem_calls == 1u &&
                      bus_jit.mem_addr == pa && bus_ref.mem_addr == pa,
                      "exactly one data access at translated physical address");
                CHECK(bus_jit.mem_pc == CODE_ADDR + 12u && bus_ref.mem_pc == CODE_ADDR + 12u,
                      "physical callback observes architectural PC");
                CHECK(arm920t_get_cp15(cpu_jit, 5u) == arm920t_get_cp15(cpu_ref, 5u) &&
                      arm920t_get_cp15(cpu_jit, 6u) == arm920t_get_cp15(cpu_ref, 6u),
                      "physical fallback does not cause a second translation fault");
                if (op < 2u) CHECK(ref_reg(1u) == (op ? 0x80u : 0x8877ff80u), "physical load value");
                else CHECK(bus_jit.mem_value == bus_ref.mem_value &&
                           bus_ref.mem_value == (op == 3u ? 0x78u : 0x12345678u), "physical store value");
                gp32_cpu_profile_t profile;
                arm920t_get_cpu_profile(cpu_jit, &profile);
                if (native && profile.supported && profile.native_backend == 2u) {
                    /* 0x13000000 is outside the identity-I/O window, so even the
                     * word transfers keep the checked helper and the
                     * attribution the emitted gates always produced.  The
                     * translation itself runs inside that helper (after this
                     * read-only classifier), so a cold TLB entry stays the
                     * first failing guard: the TLB-miss counter advances, no
                     * non-RAM record may appear, and no direct bus call may
                     * take the access over. */
                    CHECK(profile.slow_bail_single_tlbmiss == 1u &&
                          profile.slow_bail_single_nonram == 0u &&
                          profile.single_nonram_regions[pa >> 24] == 0u,
                          "non-I/O physical access keeps the checked slow path");
                    CHECK(profile.helper_ld_word + profile.helper_st_word ==
                          ((op & 1u) == 0u ? 1u : 0u), "non-I/O word access helper attribution");
                }
                teardown_pair();
            }
        }
    }
}

/* One mapped non-RAM miss, BIOS literal, and fault-tolerant identity access.
 * The following replay isolates VA retention and CP15 fault semantics. */
static void case_checked_access_translation(void) {
    case_physical_access_once();
    const uint32_t ttb = DATA_ADDR + 0x3000u, va = 0x10000000u;
    const uint32_t program[] = {
        0xee02af10u, 0xee01bf10u, /* MCR TTB/control */
        0xe5902001u,             /* unaligned mapped MMIO LDR */
        0xe1d030b1u,             /* odd mapped MMIO LDRH */
        0xe51f516cu,             /* BIOS literal 0x2ac (pc=0x410) */
        0xe5986000u,             /* unmapped LDR -> tolerant identity */
        0xeafffffeu,
    };
    current_case = "checked-access-translation";
    setup_pair();
    arm920t_set_trace(cpu_ref, 1, NULL, NULL);
    load_both(program, GP32_ARRAY_COUNT(program));
    set_reg_both(0u, va); set_reg_both(8u, 0x13000000u);
    set_reg_both(10u, ttb); set_reg_both(11u, 1u);
    set_mem_both(ttb, 2u); /* BIOS identity */
    set_mem_both(ttb + 0x400u, IO_ADDR | 2u);
    set_mem_both(0x2acu, 0x76543210u);
    bus_jit.observe_cpu = cpu_jit; bus_ref.observe_cpu = cpu_ref;
    CHECK(arm920t_run(cpu_jit, 2u) == arm920t_run(cpu_ref, 2u), "MMU setup budget");
    compare_state();
    CHECK(arm920t_run(cpu_jit, 32u) == arm920t_run(cpu_ref, 32u), "MMU access budget");
    compare_state();
    CHECK(ref_reg(2u) == 0x0c000004u && ref_reg(3u) == 0xffffu &&
          ref_reg(5u) == 0x76543210u && ref_reg(6u) == UINT32_MAX,
          "mapped rotation, odd halfword, BIOS literal and identity fallback");
    CHECK(arm920t_get_cp15(cpu_jit, 5u) == arm920t_get_cp15(cpu_ref, 5u) &&
          arm920t_get_cp15(cpu_jit, 6u) == arm920t_get_cp15(cpu_ref, 6u) &&
          arm920t_get_cp15(cpu_ref, 6u) == 0x13000000u,
          "MMU fault status/address preserve existing tolerant behavior");
    gp32_cpu_profile_t mmu_profile;
    arm920t_get_cpu_profile(cpu_jit, &mmu_profile);
    if (mmu_profile.supported && mmu_profile.native_backend == 2u) {
        /* The reason probe runs before the access fills a TLB entry, so each
         * of the three single transfers is a miss here, never a non-RAM hit. */
        CHECK(mmu_profile.slow_bail_single_tlbmiss == 3u &&
              mmu_profile.slow_bail_single_nonram == 0u &&
              (mmu_profile.single_nonram_addresses[0].reads |
               mmu_profile.single_nonram_addresses[0].writes) == 0u,
              "cold-TLB miss outranks non-RAM attribution");
    }
    teardown_pair();
}

/* Real execution, with callback counts proving the A64 path was exercised.
 * The test-only read counter is not guest-visible; the certified value has
 * no read effects. Writes remain callbacks and change the next live load. */
static void case_live_read32(void) {
    const uint32_t va = 0x10007700u;
    const uint32_t masks[] = {0u, 0xfffu, 0x3ffu};
    const uint32_t program[] = {
        0xe5901000u, /* LDR r1,[r0] */
        0xe5802000u, /* STR r2,[r0]: callback updates the live word */
        0xe5903000u, /* LDR r3,[r0]: must read the updated word */
        0xe5905001u, /* unaligned LDR retains the rotating helper */
        0xe5d06000u, /* LDRB retains the ordinary bus */
        0xe5907004u, /* unadvertised address retains the ordinary bus */
        0xeafffffeu
    };
    for (unsigned k = 0; k < GP32_ARRAY_COUNT(masks); ++k) {
        current_case = k == 0 ? "live-word-MMU-off" : k == 1 ? "live-word-mapped" : "live-word-tiny-page";
        setup_pair();
        load_both(program, GP32_ARRAY_COUNT(program));
        uint32_t addr = k ? va : IO_ADDR;
        uint32_t pa = IO_ADDR | (k ? va & masks[k] : 0u);
        if (k) {
            arm920t_state_image_t *state = calloc(1, sizeof(*state));
            if (!state) exit(2);
            state->r[15] = CODE_ADDR; state->cpsr = 0xd3u; state->cp15[1] = 1u;
            state->tlb_valid[0] = 1; state->tlb_mask[0] = 0xfffu; /* BIOS code */
            unsigned idx = (va >> 12) & 0xfffu;
            state->tlb_valid[idx] = 1; state->tlb_mask[idx] = masks[k];
            state->tlb_va_base[idx] = va & ~masks[k]; state->tlb_pa_base[idx] = IO_ADDR;
            arm920t_state_apply(cpu_ref, state); arm920t_state_apply(cpu_jit, state);
            free(state);
            arm920t_set_jit(cpu_jit, 1);
        }
        set_reg_both(0, addr); set_reg_both(2, 0xa1b2c3d4u);
        bus_ref.live_pa = bus_jit.live_pa = pa;
        bus_ref.live_value = bus_jit.live_value = 0x12345678u;
        bus_ref.observe_cpu = cpu_ref; bus_jit.observe_cpu = cpu_jit;
        arm_live_read32_t reads[] = {{pa, &bus_jit.live_value}};
        CHECK(arm920t_set_live_read32(cpu_jit, reads, 1), "register live word");
        run_cache_pair(CODE_ADDR, 64u);
        CHECK(ref_reg(1) == 0x12345678u && ref_reg(3) == 0xa1b2c3d4u &&
              ref_reg(5) == 0xd4a1b2c3u && ref_reg(6) == 0xd4u && ref_reg(7) == UINT32_MAX,
              "fresh word, write callback, rotation and unsupported accesses");
        CHECK(bus_jit.live_writes == 1u && bus_ref.live_writes == 1u &&
              bus_jit.live_rebind_rejected == 1u && bus_ref.live_rebind_rejected == 1u,
              "writes remain callbacks; in-flight registration changes are rejected");
        gp32_cpu_profile_t profile;
        arm920t_get_cpu_profile(cpu_jit, &profile);
        if (profile.supported && profile.native_backend != 0u) {
            CHECK(profile.native_arm_insns >= 6u, "live word path uses native instructions");
            CHECK(bus_jit.live_reads == 2u && bus_ref.live_reads == 4u,
                  "only two aligned word reads bypass the bus callback");
        }
        /* Reuse compiled code with fresh external input, then revoke it. */
        bus_ref.live_value = bus_jit.live_value = 0x88776655u;
        run_cache_pair(CODE_ADDR, 64u);
        CHECK(ref_reg(1) == 0x88776655u, "compiled code reloads current input");
        CHECK(arm920t_set_live_read32(cpu_jit, NULL, 0), "disable live word");
        unsigned before = bus_jit.live_reads;
        run_cache_pair(CODE_ADDR, 64u);
        CHECK(bus_jit.live_reads - before == 4u, "disabling invalidates compiled pointers");
        arm_live_read32_t bad = {pa | 1u, &bus_jit.live_value};
        CHECK(!arm920t_set_live_read32(cpu_jit, &bad, 1) &&
              !arm920t_set_live_read32(cpu_jit, NULL, 1), "invalid registration rejected");
        teardown_pair();
    }
}


/* Direct identity-I/O word path (A64): after the RAM guard rejects the
 * address the emitter proves the current full-mask TLB entry (MMU on) or the
 * identity mapping (MMU off) plus the physical 0x14000000..0x16000000 window,
 * and the bus callback, the register commits and the exit condition stay with
 * the dedicated helper.  Word LDR/STR with pre/post index and writeback must
 * observe exactly one bus access at the proven physical address with the
 * callback-visible PC+4 and the pre-writeback base, run entirely native, and
 * still leave the block when the callback raises an IRQ. */
static void case_io_direct_word(void) {
    const struct { uint32_t insn; unsigned wb; unsigned off; } cases[] = {
        {0xe5942000u, 0u, 0u}, /* LDR r2,[r4] */
        {0xe5b42004u, 1u, 4u}, /* LDR r2,[r4,#4]! */
        {0xe4942004u, 1u, 0u}, /* LDR r2,[r4],#4 */
        {0xe5842000u, 0u, 0u}, /* STR r2,[r4] */
        {0xe5a42004u, 1u, 4u}, /* STR r2,[r4,#4]! */
        {0xe4842004u, 1u, 0u}, /* STR r2,[r4],#4 */
        {0xe7842081u, 0u, 4u}, /* STR r2,[r4,r1,lsl #1] with r1=2 */
    };
    const uint32_t va = 0x10008000u;
    const uint32_t masks[] = {0xfffu, 0x3ffu}; /* 4 KiB small page and 1 KiB tiny page */
    for (unsigned mmu = 0; mmu < 2u; ++mmu) {
        unsigned variants = mmu ? GP32_ARRAY_COUNT(masks) : 1u;
        for (unsigned v = 0; v < variants; ++v) {
            for (unsigned i = 0; i < GP32_ARRAY_COUNT(cases); ++i) {
                const int load = (cases[i].insn & (1u << 20)) != 0u;
                const uint32_t base = mmu ? va : IO_ADDR;
                const uint32_t pa = IO_ADDR + cases[i].off;
                const char *map = mmu ? (v ? "tiny" : "mapped") : "identity";
                const char *shape = load ? (cases[i].wb ? "ldr-wb" : "ldr")
                                         : (cases[i].wb ? "str-wb" : "str");
                char name[64];
                snprintf(name, sizeof(name), "io-direct-word-%s-%s", map, shape);
                current_case = name;
                setup_pair();
                arm920t_set_trace(cpu_ref, 1, NULL, NULL); /* instruction-at-a-time oracle */
                const uint32_t program[] = {cases[i].insn, 0xe3a06001u, 0xeafffffeu};
                load_both(program, GP32_ARRAY_COUNT(program));
                if (mmu) {
                    /* BIOS code page plus the mapped data page under test. */
                    arm920t_state_image_t *state = calloc(1, sizeof(*state));
                    if (!state) exit(2);
                    state->r[15] = CODE_ADDR; state->cpsr = 0x13u; state->cp15[1] = 1u;
                    state->tlb_valid[0] = 1; state->tlb_mask[0] = 0xfffu;
                    unsigned idx = (va >> 12) & 0xfffu;
                    state->tlb_valid[idx] = 1; state->tlb_mask[idx] = masks[v];
                    state->tlb_va_base[idx] = va & ~masks[v]; state->tlb_pa_base[idx] = IO_ADDR;
                    arm920t_state_apply(cpu_ref, state); arm920t_state_apply(cpu_jit, state);
                    free(state);
                    arm920t_set_jit(cpu_jit, 1);
                }
                set_reg_both(1u, 2u); set_reg_both(2u, 0x12345678u); set_reg_both(4u, base);
                bus_jit.observe_cpu = cpu_jit; bus_ref.observe_cpu = cpu_ref;
                bus_jit.mem_probe = bus_ref.mem_probe = 1u;
                CHECK(arm920t_run(cpu_jit, 32u) == arm920t_run(cpu_ref, 32u), "io-direct budget");
                compare_state();
                CHECK(bus_jit.mem_calls == 1u && bus_ref.mem_calls == 1u, "io-direct one bus access");
                CHECK(bus_ref.mem_addr == pa && bus_jit.mem_addr == pa,
                      "io-direct access at the proven physical address");
                CHECK(bus_jit.mem_pc == CODE_ADDR + 4u && bus_ref.mem_pc == CODE_ADDR + 4u,
                      "io-direct callback PC+4");
                CHECK(bus_jit.mem_base == base && bus_ref.mem_base == base,
                      "io-direct pre-writeback base");
                if (load) CHECK(ref_reg(2u) == 0x8877ff80u, "io-direct loaded callback value");
                else CHECK(bus_jit.mem_value == 0x12345678u && bus_ref.mem_value == 0x12345678u,
                           "io-direct store source");
                CHECK(ref_reg(4u) == (cases[i].wb ? base + 4u : 0x12345678u),
                      "io-direct writeback and callback register mutation");
                gp32_cpu_profile_t profile;
                arm920t_get_cpu_profile(cpu_jit, &profile);
                if (profile.supported && profile.native_backend == 2u) {
                    CHECK(profile.native_block_calls != 0u, "io-direct native coverage");
                    CHECK(profile.helper_ld_word == 0u && profile.helper_st_word == 0u &&
                          profile.helper_ld_byte == 0u && profile.helper_st_byte == 0u,
                          "io-direct word path bypasses the access helpers");
                    CHECK(profile.slow_bail_single_nonram == 0u && profile.slow_bail_single_tlbmiss == 0u,
                          "io-direct word path is not attributed as a slow bail");
                }
                printf("io-direct %s %s insn=%08" PRIx32 " addr=%08" PRIx32 " wb=%08" PRIx32 " r2=%08" PRIx32 "\n",
                       map, shape, cases[i].insn, bus_jit.mem_addr,
                       arm920t_get_reg(cpu_jit, 4u), arm920t_get_reg(cpu_jit, 2u));
                teardown_pair();
            }
        }
    }
    {
        /* A cold TLB entry must keep the checked helper for the same shape,
         * with its pre-access attribution intact. */
        const uint32_t program[] = {0xe5942000u, 0xe3a06001u, 0xeafffffeu};
        current_case = "io-direct-cold-tlb";
        setup_pair();
        arm920t_set_trace(cpu_ref, 1, NULL, NULL);
        load_both(program, GP32_ARRAY_COUNT(program));
        arm920t_state_image_t *state = calloc(1, sizeof(*state));
        if (!state) exit(2);
        state->r[15] = CODE_ADDR; state->cpsr = 0x13u; state->cp15[1] = 1u;
        state->tlb_valid[0] = 1; state->tlb_mask[0] = 0xfffu;
        arm920t_state_apply(cpu_ref, state); arm920t_state_apply(cpu_jit, state);
        free(state);
        arm920t_set_jit(cpu_jit, 1);
        set_reg_both(2u, 0x12345678u); set_reg_both(4u, va);
        bus_jit.observe_cpu = cpu_jit; bus_ref.observe_cpu = cpu_ref;
        bus_jit.mem_probe = bus_ref.mem_probe = 1u;
        CHECK(arm920t_run(cpu_jit, 32u) == arm920t_run(cpu_ref, 32u), "io-direct cold TLB budget");
        compare_state();
        CHECK(bus_jit.mem_calls == 1u && bus_ref.mem_calls == 1u && bus_ref.mem_addr == va,
              "cold TLB keeps one checked access");
        gp32_cpu_profile_t profile;
        arm920t_get_cpu_profile(cpu_jit, &profile);
        if (profile.supported && profile.native_backend == 2u) {
            CHECK(profile.slow_bail_single_tlbmiss == 1u && profile.slow_bail_single_nonram == 0u,
                  "cold TLB keeps the checked slow-reason attribution");
            CHECK(profile.helper_ld_word == 1u, "cold TLB keeps the checked access helper");
        }
        printf("io-direct cold-tlb addr=%08" PRIx32 " r2=%08" PRIx32 "\n",
               bus_jit.mem_addr, arm920t_get_reg(cpu_jit, 2u));
        teardown_pair();
    }
    {
        /* A live TLB entry whose physical target is outside the identity-I/O
         * window must be rejected by the direct probe: the checked helper owns
         * the bus access and the accessible mapping also reaches the non-RAM
         * attribution, so the window rejection is exercised with a proven
         * mapping instead of a cold entry. */
        const uint32_t insns[] = {0xe5942000u, 0xe5842000u}; /* LDR r2,[r4]; STR r2,[r4] */
        const uint32_t va = 0x10008000u, pa = 0x13000000u;
        for (unsigned i = 0; i < GP32_ARRAY_COUNT(insns); ++i) {
            const int load = (insns[i] & (1u << 20)) != 0u;
            current_case = load ? "io-direct-mapped-nonio-ldr" : "io-direct-mapped-nonio-str";
            setup_pair();
            arm920t_set_trace(cpu_ref, 1, NULL, NULL);
            const uint32_t program[] = {insns[i], 0xe3a06001u, 0xeafffffeu};
            load_both(program, GP32_ARRAY_COUNT(program));
            arm920t_state_image_t *state = calloc(1, sizeof(*state));
            if (!state) exit(2);
            state->r[15] = CODE_ADDR; state->cpsr = 0x13u; state->cp15[1] = 1u;
            state->tlb_valid[0] = 1; state->tlb_mask[0] = 0xfffu;
            unsigned idx = (va >> 12) & 0xfffu;
            state->tlb_valid[idx] = 1; state->tlb_mask[idx] = 0xfffu;
            state->tlb_va_base[idx] = va; state->tlb_pa_base[idx] = pa;
            arm920t_state_apply(cpu_ref, state); arm920t_state_apply(cpu_jit, state);
            free(state);
            arm920t_set_jit(cpu_jit, 1);
            set_reg_both(2u, 0x12345678u); set_reg_both(4u, va);
            bus_jit.observe_cpu = cpu_jit; bus_ref.observe_cpu = cpu_ref;
            bus_jit.mem_probe = bus_ref.mem_probe = 1u;
            CHECK(arm920t_run(cpu_jit, 32u) == arm920t_run(cpu_ref, 32u), "io-direct non-IO budget");
            compare_state();
            CHECK(bus_jit.mem_calls == 1u && bus_ref.mem_calls == 1u && bus_ref.mem_addr == pa,
                  "non-IO mapped target keeps one checked access");
            gp32_cpu_profile_t profile;
            arm920t_get_cpu_profile(cpu_jit, &profile);
            if (profile.supported && profile.native_backend == 2u) {
                CHECK(profile.helper_ld_word + profile.helper_st_word == 1u &&
                      profile.slow_bail_single_tlbmiss == 0u,
                      "non-IO mapped target keeps the checked access helper");
                CHECK(profile.slow_bail_single_nonram == 1u &&
                      profile.single_nonram_regions[pa >> 24] == 1u,
                      "non-IO mapped target keeps the non-RAM attribution");
            }
            printf("io-direct mapped non-IO %s insn=%08" PRIx32 " addr=%08" PRIx32 " r2=%08" PRIx32 "\n",
                   load ? "ldr" : "str", insns[i], bus_jit.mem_addr, arm920t_get_reg(cpu_jit, 2u));
            teardown_pair();
        }
    }
    {
        /* A store that raises an IRQ inside the callback must leave the block
         * after the committed transfer and writeback, exactly as the checked
         * path does. */
        const uint32_t handler[] = {
            0xe5896000u, /* STR r6,[r9]: capture state before the next guest MOV */
            0xe5883000u, /* STR r3,[r8]: acknowledge IRQ */
            0xe25ef004u, /* SUBS pc,lr,#4: resume after the transfer */
        };
        const uint32_t program[] = {0xe4842004u, 0xe3a06001u, 0xeafffffeu};
        current_case = "io-direct-irq-write";
        setup_pair();
        arm920t_set_trace(cpu_ref, 1, NULL, NULL);
        load_both(program, GP32_ARRAY_COUNT(program));
        for (unsigned i = 0; i < GP32_ARRAY_COUNT(handler); ++i)
            set_mem_both(0x18u + 4u * i, handler[i]);
        set_mem_both(DATA_ADDR, 0xdeadbeefu);
        set_reg_both(2u, 0x12345678u);
        set_reg_both(4u, IO_ADDR);
        set_reg_both(8u, IO_ADDR + 4u);
        set_reg_both(9u, DATA_ADDR);
        arm920t_set_cpsr(cpu_jit, 0x1fu); arm920t_set_cpsr(cpu_ref, 0x1fu);
        bus_jit.observe_cpu = cpu_jit; bus_ref.observe_cpu = cpu_ref;
        bus_jit.io_raise_at = bus_ref.io_raise_at = 1u;
        CHECK(arm920t_run(cpu_jit, 32u) == arm920t_run(cpu_ref, 32u), "io-direct IRQ budget");
        compare_state();
        CHECK(gp32_ld32le(bus_ptr(&bus_ref, DATA_ADDR, 4u)) == 0u &&
              gp32_ld32le(bus_ptr(&bus_jit, DATA_ADDR, 4u)) == 0u,
              "IRQ handler runs before the following MOV");
        CHECK(arm920t_get_reg(cpu_jit, 4u) == IO_ADDR + 4u && ref_reg(4u) == IO_ADDR + 4u,
              "writeback commits once before the IRQ");
        CHECK(bus_jit.io_count == 1u && bus_ref.io_count == 1u &&
              bus_jit.io_acks == 1u && bus_ref.io_acks == 1u, "one store and one IRQ acknowledge");
        CHECK(bus_jit.io_pc[0] == CODE_ADDR + 4u && bus_ref.io_pc[0] == CODE_ADDR + 4u,
              "IRQ write observes PC+4");
        CHECK(ref_reg(6u) == 1u && arm920t_get_pc(cpu_ref) == CODE_ADDR + 8u,
              "IRQ returns after the committed store");
        gp32_cpu_profile_t profile;
        arm920t_get_cpu_profile(cpu_jit, &profile);
        if (profile.supported && profile.native_backend == 2u) {
            CHECK(profile.helper_st_word == 0u && profile.slow_bail_single_nonram == 0u,
                  "direct IRQ write bypasses the checked helper");
        }
        printf("io-direct irq-write pc=%08" PRIx32 " wb=%08" PRIx32 " captured=%08" PRIx32 "\n",
               bus_jit.io_pc[0], arm920t_get_reg(cpu_jit, 4u),
               gp32_ld32le(bus_ptr(&bus_jit, DATA_ADDR, 4u)));
        teardown_pair();
    }
}

/* Compile-time constant folding must preserve intermediate values when a
 * run stops between the two ops, and commit cached values before callbacks. */
static void case_native_const_data(void) {
    const uint32_t positive[] = {
        0xe3a004aeu, 0xe1a00b40u, /* MOV #ae000000; ASR #22 */
        0xe3e01f52u, 0xe2411001u, /* MVN #148; SUB #1 */
        0xe3a02081u, 0xe1a02022u, /* LSR #32 */
        0xe3e03000u, 0xe1a03043u, /* ASR #32 sign-fill */
        0xe3a04008u, 0xe1a04264u, /* ROR #4 */
        0xe3a050ffu, 0xe2855001u, /* ADD */
        0xe3a060ffu, 0xe20660f0u, /* AND */
        0xe3a070ffu, 0xe227705au, /* EOR */
        0xe3a08020u, 0xe26880a0u, /* RSB */
        0xe3a0a0f0u, 0xe1e0a00au, /* register MVN */
        0xe88c05ffu,             /* record r0-r8,r10 before callback */
        0xe5890000u,             /* callback sees folded r0 at original PC */
        0xe3a044aeu, 0xe1a04b44u, /* replace callback's dirty r4 */
        0xe284b002u, 0xe58cb080u, /* next consumer uses folded value */
        0xeafffffeu
    };
    const uint32_t rejected[] = {
        0xe3a00003u, 0xe1a00060u, 0xe48c0004u, /* RRX depends on guest C */
        0xe3a01008u, 0xe1a01a11u, 0xe48c1004u, /* register shift uses r10 */
        0xe3a02081u, 0xe1b020a2u, 0xe10f8000u, 0xe8ac0104u, /* shifter C */
        0xe3a03011u, 0xe2533011u, 0xe10f8000u, 0xe8ac0108u, /* arithmetic S */
        0xe3a04007u, 0x12844001u, 0xe48c4004u, /* failed predicate */
        0xe3b05005u, 0xe3a05007u, 0xe10f8000u, 0xe8ac0120u, /* producer S */
        0xe3a05005u, 0xe1a05006u, 0xe48c5004u, /* other source register */
        0xe3a07007u, 0xe2a77001u, 0xe48c7004u, /* ADC keeps live carry */
        0xeafffffeu
    };
    const uint32_t expected[] = {0xfffffeb8u, 0xfffffeb6u, 0u, UINT32_MAX,
        0x80000000u, 0x100u, 0xf0u, 0xa5u, 0x80u, 0xffffff0fu};
    for (unsigned negative = 0; negative < 2u; ++negative)
    for (unsigned carry = 0; carry < 2u; ++carry)
    for (unsigned split = 0; split < 2u; ++split) {
        current_case = negative ? "const-data-boundaries" : "const-data-native";
        setup_pair();
        load_both(negative ? rejected : positive,
                  negative ? GP32_ARRAY_COUNT(rejected) : GP32_ARRAY_COUNT(positive));
        set_cpsr_both(0x13u | (carry ? 0x20000000u : 0u));
        set_reg_both(6u, 9u); set_reg_both(9u, IO_ADDR);
        set_reg_both(10u, 2u); set_reg_both(12u, DATA_ADDR);
        if (!negative) {
            bus_jit.observe_cpu = cpu_jit; bus_ref.observe_cpu = cpu_ref;
            bus_jit.mem_probe = bus_ref.mem_probe = 1u;
            bus_jit.mem_effect = bus_ref.mem_effect = 10u;
        }
        if (split) {
            const unsigned cuts[] = {1u, 1u, 2u, 5u, 55u};
            for (unsigned j = 0; j < GP32_ARRAY_COUNT(cuts); ++j) {
                CHECK(arm920t_run(cpu_jit, cuts[j]) == cuts[j], "folded native budget");
                CHECK(arm920t_run(cpu_ref, cuts[j]) == cuts[j], "folded reference budget");
                compare_state();
                if (!negative && j == 0u) CHECK(ref_reg(0u) == 0xae000000u,
                    "cut between pair retains the original producer result");
            }
        } else run_cache_pair(CODE_ADDR, 64u);
        if (!negative) {
            for (unsigned j = 0; j < GP32_ARRAY_COUNT(expected); ++j)
                CHECK(gp32_ld32le(bus_ref.ram + DATA_ADDR - RAM_BASE + 4u*j) == expected[j],
                      "folded fixture result");
            CHECK(bus_jit.mem_calls == 1u && bus_ref.mem_calls == 1u &&
                  bus_jit.mem_pc == CODE_ADDR + 88u && bus_ref.mem_pc == CODE_ADDR + 88u &&
                  bus_jit.mem_value == 0xfffffeb8u && bus_ref.mem_value == 0xfffffeb8u,
                  "callback observes original PC and folded value");
            CHECK(ref_reg(2u) == 0xabcdef01u && ref_reg(11u) == 0xfffffebau,
                  "callback mutation and following forwarded consumer survive");
        } else {
            CHECK(gp32_ld32le(bus_ref.ram + DATA_ADDR - RAM_BASE) ==
                  (carry ? 0x80000001u : 1u), "RRX uses input carry");
            CHECK(gp32_ld32le(bus_ref.ram + DATA_ADDR - RAM_BASE + 4u) == 32u,
                  "register-controlled shift uses live amount");
            CHECK(ref_reg(4u) == 7u && ref_reg(5u) == 9u && ref_reg(7u) == 9u,
                  "predicates, external operands and carry remain live");
        }
        gp32_cpu_profile_t profile;
        arm920t_get_cpu_profile(cpu_jit, &profile);
        if (!split && profile.supported && profile.native_backend) {
            CHECK(profile.native_block_calls != 0u, "constant-fold native coverage");
            if (!negative) CHECK(profile.helper_op_kinds[1] == 0u, "constant DATA needs no helper");
        }
        teardown_pair();
    }
}

/* The real GP32 GPIO leaf builds r1=0x15600000 with MOV/LSL/ORR/LSL.  A long
 * constant run folds into one native step, so every prefix budget boundary
 * must still retire the raw guest ops, the folded prefix must stop at the
 * carry (RRX), flags (S) and foreign-operand boundaries that follow it, and
 * the store callback must observe the folded constant at its own guest PC. */
static void case_native_const_chain(void) {
    const uint32_t chain[] = {
        0xe3a01015u, /* MOV r1,#0x15 */
        0xe1a01401u, /* MOV r1,r1,LSL #8  -> 0x1500 */
        0xe3811060u, /* ORR r1,r1,#0x60  -> 0x1560 */
        0xe1a01801u, /* MOV r1,r1,LSL #16 -> 0x15600000 */
        0xe5891000u, /* STR r1,[r9]: callback sees the folded run */
        0xe3a02015u, /* MOV r2,#0x15 */
        0xe1a02402u, /* MOV r2,r2,LSL #8  -> 0x1500 */
        0xe3822060u, /* ORR r2,r2,#0x60  -> 0x1560 (a 3-op fold) */
        0xe1a02062u, /* MOV r2,r2,RRX: carry boundary stops the fold */
        0xe0922003u, /* ADDS r2,r2,r3: flags and foreign operand boundary */
        0xe58c2080u, /* STR r2,[r12,#0x80] */
        0xeafffffeu
    };
    const uint32_t prefix[] = {0x15u, 0x1500u, 0x1560u, 0x15600000u, 0x15600000u};
    for (unsigned carry = 0; carry < 2u; ++carry)
    for (unsigned split = 0; split < 2u; ++split) {
        current_case = split ? "const-chain-boundaries" : "const-chain-native";
        setup_pair();
        load_both(chain, GP32_ARRAY_COUNT(chain));
        set_cpsr_both(0x13u | (carry ? 0x20000000u : 0u));
        uint32_t rrx = (carry ? 0x80000000u : 0u) | 0xab0u;
        set_reg_both(3u, 0x10u); set_reg_both(9u, IO_ADDR); set_reg_both(12u, DATA_ADDR);
        bus_jit.observe_cpu = cpu_jit; bus_ref.observe_cpu = cpu_ref;
        bus_jit.mem_probe = bus_ref.mem_probe = 1u;
        bus_jit.mem_effect = bus_ref.mem_effect = 10u;
        if (split) {
            for (unsigned j = 0; j < GP32_ARRAY_COUNT(chain); ++j) {
                CHECK(arm920t_run(cpu_jit, 1u) == 1u, "chain prefix budget");
                CHECK(arm920t_run(cpu_ref, 1u) == 1u, "chain prefix reference budget");
                compare_state();
                if (j < 5u) CHECK(ref_reg(1u) == prefix[j], "chain prefix value");
                else if (j < 8u) CHECK(ref_reg(2u) == prefix[j - 5u], "chain prefix value");
                else if (j == 8u) CHECK(ref_reg(2u) == rrx, "RRX boundary uses live carry");
                else if (j == 9u) CHECK(ref_reg(2u) == rrx + 0x10u &&
                                       (arm920t_get_cpsr(cpu_ref) & 0xf0000000u) == (carry ? 0x80000000u : 0u),
                                       "flags boundary stays live");
            }
        } else {
            run_cache_pair(CODE_ADDR, 64u);
        }
        CHECK(ref_reg(1u) == 0x15600000u && ref_reg(2u) == rrx + 0x10u &&
              gp32_ld32le(bus_ref.ram + DATA_ADDR - RAM_BASE + 0x80u) == rrx + 0x10u,
              "chain final values");
        CHECK(bus_jit.mem_calls == 1u && bus_ref.mem_calls == 1u &&
              bus_jit.mem_pc == CODE_ADDR + 20u && bus_ref.mem_pc == CODE_ADDR + 20u &&
              bus_jit.mem_value == 0x15600000u && bus_ref.mem_value == 0x15600000u,
              "callback sees the folded run at the original PC");
        gp32_cpu_profile_t profile;
        arm920t_get_cpu_profile(cpu_jit, &profile);
        if (!split && profile.supported && profile.native_backend)
            CHECK(profile.native_block_calls != 0u, "constant-chain native coverage");
        teardown_pair();
    }
}

int main(int argc, char **argv) {
    /* Native gate triage can isolate this mapped physical-boundary case
     * without rerunning unrelated differential workloads. */
    int ram_end_only = argc == 2 && !strcmp(argv[1], "--ram-end");
    int leaf_only = argc == 2 && !strcmp(argv[1], "--unframed-leaf");
    int nested_only = argc == 2 && !strcmp(argv[1], "--nested-leaf");
    int cold_only = argc == 2 && !strcmp(argv[1], "--cold-leaf");
    int chain_only = argc == 2 && !strcmp(argv[1], "--branch-chain");
    int forward_only = argc == 2 && !strcmp(argv[1], "--forward-loop");
    int callback_only = argc == 2 && !strcmp(argv[1], "--callback-pc");
    int loops_only = argc == 2 && !strcmp(argv[1], "--loop-fences");
    int irq_only = argc == 2 && !strcmp(argv[1], "--callback-irq");
    int block_only = argc == 2 && !strcmp(argv[1], "--block-callback");
    int portable_only = argc == 2 && !strcmp(argv[1], "--portable-callback");
    int pairs_only = argc == 2 && !strcmp(argv[1], "--block-pairs");
    int access_only = argc == 2 && !strcmp(argv[1], "--checked-access");
    int io_only = argc == 2 && !strcmp(argv[1], "--io-direct");
    int poll_only = argc == 2 && !strcmp(argv[1], "--poll-progress");
    int psr_only = argc == 2 && !strcmp(argv[1], "--psr");
    int terminal_coproc_only = argc == 2 && !strcmp(argv[1], "--terminal-coproc");
    int selfmove_only = argc == 2 && !strcmp(argv[1], "--self-move-nop");
    if (argc == 2 && !strcmp(argv[1], "--sflag-logic")) {
        case_native_sflag_logic();
    } else if (argc == 2 && !strcmp(argv[1], "--const-data")) {
        case_native_const_data();
        case_native_const_chain();
    } else if (argc == 2 && !strcmp(argv[1], "--word-probes")) {
        case_native_read_windows();
        case_live_read32();
        case_io_direct_word();
        case_checked_access_translation();
    } else if (argc == 2 && !strcmp(argv[1], "--read-windows")) {
        case_native_read_windows();
    } else if (argc == 2 && !strcmp(argv[1], "--live-read32")) {
        case_live_read32();
        case_io_direct_word();
    } else if (argc == 2 && !strcmp(argv[1], "--ram-page-tags")) {
        case_native_mapped_pages();
        case_native_mmu_mode_changes();
    } else if (argc == 2 && !strcmp(argv[1], "--exception-return")) {
        case_native_exception_return();
        case_native_spsr_exception_return();
    } else if (argc == 2 && !strcmp(argv[1], "--terminal-helper")) {
        case_terminal_swi_yield();
        case_native_alu_region();
        case_native_spsr_exception_return();
        case_cache_maintenance_native();
        case_native_cpsr();
    } else if (argc == 2 && !strcmp(argv[1], "--ldm-pc-native")) {
        case_native_ldm_pc();
        case_ldm_pc();
        case_native_mapped_block();
    } else if (argc == 2 && !strcmp(argv[1], "--ldr-pc-native")) {
        case_native_ldr_pc();
        case_native_ldr_pc_chain();
    } else if (argc == 2 && !strcmp(argv[1], "--msr-data-pc")) {
        case_native_cpsr();
        case_native_data_pc();
    } else if (argc == 2 && !strcmp(argv[1], "--carry-arith")) {
        case_native_carry_arith();
    } else if (selfmove_only) {
        case_native_self_move_noop();
    } else if (argc == 2 && !strcmp(argv[1], "--psr-blocks")) {
        case_native_psr_continuation();
    } else if (argc == 2 && !strcmp(argv[1], "--cpsr")) {
        case_native_cpsr();
    } else if (psr_only) {
        case_native_spsr();
        case_native_longmul_psr();
    } else if (terminal_coproc_only) {
        case_terminal_coproc_predicate();
    } else if (poll_only) {
        case_poll_progress();
    } else if (io_only) {
        case_io_direct_word();
    } else if (access_only) {
        case_checked_access();
        case_checked_access_translation();
        case_callback_irq_commit();
        case_native_mapped_ram_end();
    } else if (pairs_only) {
        case_block_pairs();
        case_native_mapped_block();
        case_ldm_pc();
        case_block_callback_exit();
        case_loop_smc_epoch();
        case_block_callback_yield();
    } else if (argc == 2 && !strcmp(argv[1], "--cache-maintenance")) {
        case_cache_maintenance_native();
        case_cache_unchanged();
        case_cache_modified(0);
        case_cache_modified(1);
    } else if (cold_only) {
        case_cold_leaf_mapping();
    } else if (nested_only) {
        case_nested_framed_leaf();
    } else if (portable_only) {
        portable_callbacks = 1;
        case_callback_irq_commit();
        case_block_callback_exit();
        case_loop_callback_trace();
        case_superblock_trace();
    } else if (block_only) {
        case_block_callback_exit();
        case_block_modes();
        case_ldm_pc();
    } else if (irq_only) {
        case_callback_irq_commit();
    } else if (loops_only) {
        case_loop_irq_fence();
        case_loop_smc_epoch();
        case_loop_callback_flush();
        case_loop_callback_trace();
        case_loop_framed_leaf();
    } else if (chain_only) {
        case_branch_chain();
    } else if (forward_only) {
        case_forward_loop();
    } else if (callback_only) {
        case_callback_pc();
    } else if (leaf_only) {
        case_unframed_leaf();
    } else if (ram_end_only) {
        case_native_mapped_ram_end();
    } else {
    case_native_read_windows();
    case_live_read32();
    case_io_direct_word();
    case_terminal_swi_yield();
    case_terminal_coproc_predicate();
    case_native_ldm_pc();
    case_native_ldr_pc();
    case_native_ldr_pc_chain();
    case_poll_progress();
    case_native_mapped_pages();
    case_native_literal_addresses();
    case_native_mmu_mode_changes();
    case_native_mapped_ram_end();
    case_cache_unchanged();
    case_cold_leaf_mapping();
    case_cache_modified(0);
    case_cache_modified(1);
    case_native_alu_region();
    case_native_const_data();
    case_native_const_chain();
    case_native_forwarding();
    case_native_self_move_noop();
    case_native_immediates();
    case_native_immshift();
    case_native_sflag_logic();
    case_native_condition_flags();
    case_native_regshift();
    case_native_carry_arith();
    case_native_longmul_psr();
    case_native_spsr();
    case_native_exception_return();
    case_native_psr_continuation();
    case_native_cpsr();
    case_native_data_pc();
    case_cache_maintenance_native();
    case_native_mapped_block();
    case_unframed_leaf();
    case_callback_pc();
    case_callback_irq_commit();
    case_block_callback_exit();
    case_block_callback_yield();
    case_checked_access();
    case_checked_access_translation();
    portable_callbacks = 1;
    case_callback_irq_commit();
    case_block_callback_exit();
    case_loop_callback_trace();
    portable_callbacks = 0;
    case_flags();
    case_shift();
    case_branch();
    case_branch_chain();
    case_forward_loop();
    case_bx();
    case_mem();
    case_half_modes();
    case_block_modes();
    case_block_pairs();
    case_ldm_pc();
    case_mul();
    case_seeded();
    case_budget();
    case_direct_dispatch();
    case_loop_irq_fence();
    case_loop_smc_epoch();
    case_loop_callback_flush();
    case_loop_callback_trace();
    case_loop_framed_leaf();
    case_nested_framed_leaf();
    case_superblock_trace();
    }
    current_case = "summary";
    if (jit_events == 0)
        fail("JIT translation path never engaged (jit=1 fell back every block)");
    if (failures) {
        fprintf(stderr, "arm jit: %d mismatches (jit events=%" PRIu64 " fallbacks=%" PRIu64 ")\n",
                failures, jit_events, jit_fallbacks);
        return 1;
    }
    printf("PASS: arm jit differential (%s), jit events=%" PRIu64 " fallbacks=%" PRIu64 "\n",
           (argc == 2 && !strcmp(argv[1], "--word-probes")) ? "word-probes" :
           (argc == 2 && !strcmp(argv[1], "--read-windows")) ? "read-windows" :
           (argc == 2 && !strcmp(argv[1], "--ram-page-tags")) ? "ram-page-tags" :
           (argc == 2 && !strcmp(argv[1], "--live-read32")) ? "live-read32" :
           (argc == 2 && !strcmp(argv[1], "--exception-return")) ? "exception-return" :
           (argc == 2 && !strcmp(argv[1], "--terminal-helper")) ? "terminal-helper" :
           (argc == 2 && !strcmp(argv[1], "--ldm-pc-native")) ? "ldm-pc-native" :
           (argc == 2 && !strcmp(argv[1], "--psr-blocks")) ? "psr-blocks" :
           (argc == 2 && !strcmp(argv[1], "--cpsr")) ? "cpsr" :
           (argc == 2 && !strcmp(argv[1], "--carry-arith")) ? "carry-arith" : selfmove_only ? "self-move-nop" : psr_only ? "spsr" : poll_only ? "poll-progress" : io_only ? "io-direct" : access_only ? "checked-access" : pairs_only ? "block-pairs/fences" : portable_only ? "portable-callback" : block_only ? "block-callback" : irq_only ? "callback-IRQ" : chain_only ? "branch-chain" : forward_only ? "forward-loop" : callback_only ? "callback-PC" : (leaf_only ? "unframed-leaf" : (ram_end_only ? "mapped-page-RAM-end" : (loops_only ? "loop-fences" : "flags/shift/branch/mem/half/block/mul/seeded/budget/loop-fences"))),
           jit_events, jit_fallbacks);
    return 0;
}
