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

#define BIOS_SIZE 0x10000u
#define RAM_BASE  0x0c000000u           /* GP32 SDRAM window */
/* A64 enables direct RAM loads/stores only after validating the full GP32
 * window. A smaller mock would silently exercise helpers instead. */
#define RAM_SIZE  0x800000u
#define CODE_ADDR 0x00000400u           /* program base inside the BIOS region */
#define DATA_ADDR (RAM_BASE + 0x1000u)
#define MAX_REPORT 12

typedef struct {
    arm_bus_t bus;
    uint8_t bios[BIOS_SIZE];
    uint8_t ram[RAM_SIZE];
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
    return p ? gp32_ld32le(p) : 0xffffffffu;
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
static uint8_t *tb_fastmem(void *u, uint32_t a, size_t bytes, int write) {
    (void)write;
    return bus_ptr((test_bus_t *)u, a, bytes);   /* must satisfy the RAM+BIOS probes */
}
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
    arm920t_set_jit(cpu_jit, 1);
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
 * in-RAM path; write_pc_x's odd (Thumb) targets and every span-guard failure
 * bail to the classified helper before any guest register changes, so the
 * oracle and JIT must stay in lockstep either way.  SP-based pops cover the
 * common function-return shape; the BL-inlined leaf wrapper must keep its
 * helper semantics and land exactly on the BL fallthrough. */
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

    /* pc word with bit1 set (even but halfword-aligned): write_pc_x masks ~3. */
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

    /* Odd target: write_pc_x sets T and r15 = v & ~1.  The native preload
     * must bail to the helper before any guest store, so r4 still arrives
     * and Thumb entry is exact. */
    const uint32_t thumb[] = {
        0xE3A0140Cu, 0xE2811A01u,
        block_insn(0, 1, 0, 1, 1, 1, 0x8010u),  /* LDMIA r1!, {r4, pc}         */
        0xEAFFFFFEu,                            /* B . ; dead                  */
        /* +0x10: Thumb halfwords: movs r7, #0x77 ; b . */
        0xE7FE2777u,
    };
    current_case = "ldm-pc-thumb";
    setup_pair();
    load_both(thumb, GP32_ARRAY_COUNT(thumb));
    set_mem_both(DATA_ADDR + 0u, 0x44444444u);
    set_mem_both(DATA_ADDR + 4u, (CODE_ADDR + 0x10u) | 1u);
    run_chunks();
    CHECK(ref_reg(4) == 0x44444444u, "Thumb bail lost the r4 load");
    CHECK(ref_reg(7) == 0x77u, "Thumb code did not run");
    CHECK((arm920t_get_cpsr(cpu_ref) & 0x20u) != 0u, "LDM pc odd did not enter Thumb");
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
     * value, so write_pc_x enters Thumb at a deterministic nonsense address. */
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
    CHECK((arm920t_get_cpsr(cpu_ref) & 0x20u) != 0u,
          "out-of-window PC did not enter Thumb via helper");
    /* Later undefined Thumb instructions enter an ARM exception handler. */
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

/* Start with enough budget to execute the actual native block. The ragged
 * harness alone can interpret a short program before ever entering its JIT. */
static void run_native_case(void) {
    uint32_t j = arm920t_run(cpu_jit, 64u), r = arm920t_run(cpu_ref, 64u);
    CHECK(j == 64u && r == 64u, "native case instruction budget");
    compare_state();
}

/* A helper must observe every ALU result before exception entry banks SP/LR.
 * Comparing only the state after arm920t_run would miss stale callback state. */
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
                CHECK(gp32_ld32le(bus_ref.ram + DATA_ADDR - RAM_BASE + i * 4u) == expected[i],
                      "forwarding fixture result");
            CHECK(ref_reg(11) == DATA_ADDR + sizeof(expected), "recorded every boundary result");
            CHECK(gp32_ld32le(bus_ref.ram + DATA_ADDR - RAM_BASE + 0x200u) == 0x42u, "SWP performed its store");
            teardown_pair();
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
        0xeafffffeu
    };
    current_case = "native-longmul";
    setup_pair();
    set_reg_both(1, 0xffffffffu);
    set_reg_both(2, 2u);
    set_reg_both(8, 7u);
    load_both(mul, GP32_ARRAY_COUNT(mul));
    run_native_case();
    CHECK(ref_reg(3) == 0xfffffffcu && ref_reg(4) == 3u, "unsigned accumulate");
    CHECK(ref_reg(5) == 0xfffffffcu && ref_reg(6) == 0xffffffffu, "signed accumulate");
    CHECK(ref_reg(1) == 0xfffffffdu && ref_reg(8) == 9u, "multiply input alias");
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
            odd ? 0xe7fe2777u : 0xe3a07077u,
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
static uint32_t cache_branch(uint32_t pc, uint32_t target, int link) {
    return (link ? 0xeb000000u : 0xea000000u) | (((target - pc - 8u) >> 2) & 0x00ffffffu);
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
    CHECK(gp32_ld32le(bus_ref.ram + changed - RAM_BASE) == 0xe3a04022u, "guest code write");
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
static void case_native_mapped_pages(void) {
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

int main(int argc, char **argv) {
    /* Native gate triage can isolate this mapped physical-boundary case
     * without rerunning unrelated differential workloads. */
    int ram_end_only = argc == 2 && !strcmp(argv[1], "--ram-end");
    if (ram_end_only) {
        case_native_mapped_ram_end();
    } else {
    case_native_mapped_pages();
    case_native_mmu_mode_changes();
    case_native_mapped_ram_end();
    case_cache_unchanged();
    case_cache_modified(0);
    case_cache_modified(1);
    case_native_alu_region();
    case_native_forwarding();
    case_native_immediates();
    case_native_regshift();
    case_native_longmul_psr();
    case_native_mapped_block();
    case_flags();
    case_shift();
    case_branch();
    case_bx();
    case_mem();
    case_half_modes();
    case_block_modes();
    case_ldm_pc();
    case_mul();
    case_seeded();
    case_budget();
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
           ram_end_only ? "mapped-page-RAM-end" : "flags/shift/branch/mem/half/block/mul/seeded/budget",
           jit_events, jit_fallbacks);
    return 0;
}
