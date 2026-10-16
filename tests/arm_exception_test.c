/* ARMv4T instruction boundaries, flags and exception returns.
 * Assert architectural results in both execution modes;
 * a differential comparison alone would miss defects shared by both paths. */
#include "arm920t.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BIOS_SIZE 0x10000u
#define RAM_BASE  0x0c000000u
#define RAM_SIZE  0x800000u
#define CODE_ADDR 0x00000400u
#define TADDR     0x0c001000u
#define STACK_TOP 0x0c800000u

typedef struct {
    arm_bus_t bus;
    uint8_t bios[BIOS_SIZE];
    uint8_t ram[RAM_SIZE];
} test_bus_t;

static test_bus_t tb;
static arm920t_t *tb_cpu;
static int data_callbacks;
static uint32_t data_addresses[2], data_accesses;
static const char *current_case = "init";
static int failures;

static uint8_t *bus_ptr(test_bus_t *b, uint32_t a, size_t bytes) {
    if (a < BIOS_SIZE && bytes <= BIOS_SIZE - a) return b->bios + a;
    if (a >= RAM_BASE && (uint64_t)(a - RAM_BASE) + bytes <= RAM_SIZE) return b->ram + (a - RAM_BASE);
    return NULL;
}
static uint8_t rd8(void *u, uint32_t a)  { uint8_t *p = bus_ptr(u, a, 1); return p ? p[0] : 0xffu; }
static uint16_t rd16(void *u, uint32_t a){ uint8_t *p = bus_ptr(u, a, 2); return p ? gp32_ld16le(p) : 0xffffu; }
static void record_data_access(uint32_t a) {
    if (data_callbacks && a >= TADDR && a < TADDR + 16u) {
        if (data_accesses < 2u) data_addresses[data_accesses] = a;
        ++data_accesses;
    }
}
static uint32_t rd32(void *u, uint32_t a){ record_data_access(a); uint8_t *p = bus_ptr(u, a, 4); return p ? gp32_ld32le(p) : 0xffffffffu; }
static void wr8(void *u, uint32_t a, uint8_t v)  { uint8_t *p = bus_ptr(u, a, 1); if (p) p[0] = v; }
static void wr16(void *u, uint32_t a, uint16_t v){ uint8_t *p = bus_ptr(u, a, 2); if (p) gp32_st16le(p, v); }
#define IRQ_ACK_ADDR 0x0d000000u
static void wr32(void *u, uint32_t a, uint32_t v){
    record_data_access(a);
    uint8_t *p = bus_ptr(u, a, 4);
    if (p) gp32_st32le(p, v);
    else if (a == IRQ_ACK_ADDR && tb_cpu) arm920t_set_irq(tb_cpu, 0); /* guest ack */
}
static uint8_t *fm(void *u, uint32_t a, size_t bytes, int w) {
    (void)w;
    if (data_callbacks && a >= RAM_BASE) return NULL;
    return bus_ptr(u, a, bytes);
}

static void bios_w32(uint32_t a, uint32_t v) { gp32_st32le(tb.bios + a, v); }
static void ram_w32(uint32_t a, uint32_t v)  { gp32_st32le(tb.ram + (a - RAM_BASE), v); }

static void fail(const char *what, uint32_t got, uint32_t want) {
    fprintf(stderr, "FAIL[%s]: %s got=0x%08x want=0x%08x\n", current_case, what, got, want);
    ++failures;
}
#define CHECK(got, want, msg) do { uint32_t g_ = (uint32_t)(got), w_ = (uint32_t)(want); \
    if (g_ != w_) fail(msg, g_, w_); } while (0)

static arm920t_t *make_cpu(int jit) {
    memset(&tb, 0, sizeof(tb));
    data_accesses = 0;
    for (uint32_t a = 0; a < BIOS_SIZE; a += 4u) bios_w32(a, 0xEAFFFFFEu); /* B . */
    tb.bus.read8 = rd8; tb.bus.read16 = rd16; tb.bus.read32 = rd32;
    tb.bus.write8 = wr8; tb.bus.write16 = wr16; tb.bus.write32 = wr32;
    tb.bus.fastmem = fm; tb.bus.user = &tb; tb.bus.is_stable_read32 = NULL;
    arm920t_t *c = arm920t_create(&tb.bus);
    if (!c) { fprintf(stderr, "arm920t_create failed\n"); exit(2); }
    arm920t_reset(c, CODE_ADDR);
    arm920t_set_jit(c, jit);
    tb_cpu = c;
    return c;
}

/* ARM setup: SVC stack, clear I, then BX into Thumb at <entry|1>. */
static void load_setup(uint32_t thumb_entry_or1) {
    static const uint32_t setup[] = {
        0xE59FD010u, /* ldr sp, [pc, #0x10]  -> 0x418 literal */
        0xE321F053u, /* msr cpsr_c, #0x53   -> SVC, I=0, F=1   */
        0xE59F000Cu, /* ldr r0, [pc, #0x0c] -> 0x41c literal */
        0xE12FFF10u, /* bx  r0 */
        0xEAFFFFFEu, /* b . (pad) */
        0xEAFFFFFEu, /* b . (pad) */
    };
    for (unsigned i = 0; i < 6u; ++i) bios_w32(CODE_ADDR + i * 4u, setup[i]);
    bios_w32(CODE_ADDR + 0x18u, STACK_TOP);       /* literal @0x418 */
    bios_w32(CODE_ADDR + 0x1Cu, thumb_entry_or1); /* literal @0x41c */
}

/* Thumb image A: swi at +0 (LR_svc = +2 == 2 mod 4), then markers. */
static void load_thumb_swi(void) {
    ram_w32(TADDR + 0u, 0x20AADF00u); /* swi #0 ; movs r0,#0xaa */
    ram_w32(TADDR + 4u, 0x224921BBu); /* movs r1,#0xbb ; movs r2,#0x49 */
    ram_w32(TADDR + 8u, 0x0000E7FEu); /* b . */
}

/* Thumb image C: wrong-land marker at +0, body at +2 (entry == 2 mod 4). */
static void load_thumb_irq(void) {
    ram_w32(TADDR + 0u, 0x20AA23EEu); /* movs r3,#0xee ; movs r0,#0xaa */
    ram_w32(TADDR + 4u, 0xE7FE21BBu); /* movs r1,#0xbb ; b . */
}

/* Case A: SWI -> SVC, return via LDM sp!,{pc}^.  SPSR.T=1 must select Thumb;
 * bit0 of the loaded (even) return address must not. */
static void case_ldm_exception_return(int jit) {
    current_case = jit ? "ldm-exc-return-thumb(jit)" : "ldm-exc-return-thumb";
    arm920t_t *c = make_cpu(jit);
    load_setup(TADDR | 1u);
    load_thumb_swi();
    bios_w32(0x08u, 0xE92D4000u); /* stmdb sp!, {lr} */
    bios_w32(0x0Cu, 0xE8FD8000u); /* ldmfd sp!, {pc}^ */
    arm920t_run(c, 7u);           /* ldr,msr,ldr,bx,swi,stmdb,ldmfd^ */
    CHECK(arm920t_get_pc(c), TADDR + 2u, "LDM^ must return to LR_svc");
    CHECK(arm920t_get_cpsr(c) & 0x20u, 0x20u, "LDM^ return must restore T from SPSR");
    arm920t_run(c, 57u);
    CHECK(arm920t_get_reg(c, 0), 0xAAu, "post-SWI Thumb marker r0");
    CHECK(arm920t_get_reg(c, 1), 0xBBu, "post-SWI Thumb marker r1");
    CHECK(arm920t_get_reg(c, 2), 0x49u, "post-SWI Thumb marker r2");
    CHECK(arm920t_get_pc(c), TADDR + 8u, "PC must park on the Thumb loop");
    arm920t_destroy(c);
}

/* Case B: SWI -> SVC, return via MOVS pc,lr to an LR == 2 mod 4 Thumb target.
 * The ALU result must be used under the restored T (mask ~1), not ~3. */
static void case_movs_pc_lr(int jit) {
    current_case = jit ? "movs-pc-lr-thumb(jit)" : "movs-pc-lr-thumb";
    arm920t_t *c = make_cpu(jit);
    load_setup(TADDR | 1u);
    load_thumb_swi();
    bios_w32(0x08u, 0xE1B0F00Eu); /* movs pc, lr */
    arm920t_run(c, 6u);           /* ldr,msr,ldr,bx,swi,movs */
    CHECK(arm920t_get_pc(c), TADDR + 2u, "MOVS pc,lr must land at LR_svc");
    CHECK(arm920t_get_cpsr(c) & 0x20u, 0x20u, "MOVS pc,lr must restore T");
    arm920t_run(c, 58u);
    CHECK(arm920t_get_reg(c, 0), 0xAAu, "post-SWI Thumb marker r0");
    CHECK(arm920t_get_pc(c), TADDR + 8u, "PC must park on the Thumb loop");
    arm920t_destroy(c);
}

/* Case C: IRQ during Thumb at an entry == 2 mod 4, return via SUBS pc,lr,#4.
 * Landing at +0 instead of +2 executes the marker halfword at +0. */
static void case_irq_subs_pc(int jit) {
    current_case = jit ? "irq-subs-pc-thumb(jit)" : "irq-subs-pc-thumb";
    arm920t_t *c = make_cpu(jit);
    load_setup((TADDR + 2u) | 1u);
    load_thumb_irq();
    bios_w32(0x18u, 0xEA000004u); /* b 0x30 */
    bios_w32(0x30u, 0xE3A0940Du); /* mov r9, #0x0d000000 */
    bios_w32(0x34u, 0xE5899000u); /* str r9, [r9] -> acks the IRQ line */
    bios_w32(0x38u, 0xE25EF004u); /* subs pc, lr, #4 */
    arm920t_run(c, 4u);           /* ldr,msr,ldr,bx -> Thumb at +2 */
    CHECK(arm920t_get_pc(c), TADDR + 2u, "BX must enter Thumb at entry");
    CHECK(arm920t_get_cpsr(c) & 0x20u, 0x20u, "BX must set T");
    arm920t_set_irq(c, 1);        /* IRQ during Thumb: LR_irq = +6, SPSR.T=1 */
    arm920t_run(c, 4u);           /* b handler, mov r9, str (ack), subs pc,lr,#4 */
    CHECK(arm920t_get_pc(c), TADDR + 2u, "SUBS pc,lr,#4 must land at LR-4");
    CHECK(arm920t_get_cpsr(c) & 0x20u, 0x20u, "IRQ return must restore T");
    arm920t_run(c, 8u);
    CHECK(arm920t_get_reg(c, 0), 0xAAu, "IRQ return target r0");
    CHECK(arm920t_get_reg(c, 3), 0u, "SUBS pc,lr,#4 must not land at LR-4&~3");
    CHECK(arm920t_get_pc(c), TADDR + 6u, "PC must park on the Thumb loop");
    arm920t_destroy(c);
}

/* ARM920T TRM DDI0151C table 2-10: CP15 c1.V selects the
 * low (0) or high (0xffff0000) exception vector base, independently of M. */
static void case_vector_base(int jit) {
    for (unsigned high = 0; high < 2u; ++high) {
        current_case = high ? "SWI-high-vector" : "SWI-low-vector";
        arm920t_t *c = make_cpu(jit);
        bios_w32(CODE_ADDR, 0xee010f10u);      /* MCR p15,0,r0,c1,c0,0 */
        bios_w32(CODE_ADDR + 4u, 0xef000000u); /* SWI #0 */
        arm920t_set_reg(c, 0, 0x70u | (high << 13));
        arm920t_run(c, 2u);
        CHECK(arm920t_get_pc(c), (high ? 0xffff0000u : 0u) + 8u,
              "CP15 V must select the SWI vector base");
        CHECK(arm920t_get_reg(c, 14), CODE_ADDR + 8u, "SWI return address");
        CHECK(arm920t_get_cpsr(c) & 0xffu, 0xd3u, "SWI SVC ARM state");
        arm920t_destroy(c);
    }
}

/* ARM DDI0100I A7.1.49: Thumb POP never interworks on ARMv4T,
 * even when the loaded address is even. BX remains the interworking path. */
static void case_thumb_pop(int jit) {
    for (unsigned odd = 0; odd < 2u; ++odd) {
        current_case = "Thumb-POP-state";
        arm920t_t *c = make_cpu(jit);
        load_setup(TADDR | 1u);
        bios_w32(CODE_ADDR + 0x18u, STACK_TOP - 4u);
        ram_w32(TADDR, 0xe7febd00u); /* POP {pc}; B . */
        ram_w32(STACK_TOP - 4u, (TADDR + 0x42u) | odd);
        ram_w32(TADDR + 0x40u, 0x255a23eeu); /* wrong r3 marker; MOVS r5,#0x5a */
        ram_w32(TADDR + 0x44u, 0x0000e7feu);
        arm920t_run(c, 16u);
        CHECK(arm920t_get_cpsr(c) & 0x20u, 0x20u, "POP must preserve Thumb state");
        CHECK(arm920t_get_pc(c), TADDR + 0x44u, "POP target halfword alignment");
        CHECK(arm920t_get_reg(c, 5), 0x5au, "POP target marker");
        CHECK(arm920t_get_reg(c, 3), 0u, "POP must not align target to word");
        CHECK(arm920t_get_reg(c, 13), STACK_TOP, "POP stack writeback");
        arm920t_destroy(c);
    }
}

/* ARM DDI0100I A4.1.23: pre-v5 LDR PC retains ARM and ignores bits 1:0. */
static void case_ldr_pc(int jit) {
    for (unsigned low = 1; low < 4u; ++low) {
        current_case = "LDR-PC-state";
        arm920t_t *c = make_cpu(jit);
        bios_w32(CODE_ADDR, 0xe590f000u); /* LDR pc,[r0] */
        arm920t_set_reg(c, 0, STACK_TOP - 4u);
        ram_w32(STACK_TOP - 4u, (TADDR + 0x40u) | low);
        ram_w32(TADDR + 0x40u, 0xe3a0505au); /* MOV r5,#0x5a */
        ram_w32(TADDR + 0x44u, 0xeafffffeu);
        arm920t_run(c, 64u);
        CHECK(arm920t_get_cpsr(c) & 0x20u, 0u, "LDR PC must preserve ARM state");
        CHECK(arm920t_get_pc(c), TADDR + 0x44u, "LDR PC target word alignment");
        CHECK(arm920t_get_reg(c, 5), 0x5au, "LDR PC target marker");
        arm920t_destroy(c);
    }
}

/* MSR cannot leave User mode or mask interrupts, but flags remain writable. */
static void case_user_msr(int jit) {
    for (unsigned reg = 0; reg < 2u; ++reg) {
        current_case = "User-MSR-privilege";
        arm920t_t *c = make_cpu(jit);
        bios_w32(CODE_ADDR, 0xe321f010u);      /* privileged MSR CPSR_c,#USR */
        bios_w32(CODE_ADDR + 4u, reg ? 0xe127f001u : 0xe321f0d3u);
        /* register CPSR_csx,r1 or immediate CPSR_c,#SVC|I|F */
        bios_w32(CODE_ADDR + 8u, 0xe328f20fu); /* MSR CPSR_f,#0xf0000000 */
        bios_w32(CODE_ADDR + 12u, 0xe10f0000u); /* MRS r0,CPSR */
        arm920t_set_reg(c, 1, 0x00ffffd3u);
        arm920t_run(c, 1u);
        CHECK(arm920t_get_cpsr(c), 0x10u, "privileged MSR must enter User mode");
        arm920t_run(c, 64u);
        CHECK(arm920t_get_cpsr(c), 0xf0000010u, "User MSR must preserve control/status");
        CHECK(arm920t_get_reg(c, 0), 0xf0000010u, "User flags write and MRS");
        CHECK(arm920t_get_pc(c), CODE_ADDR + 16u, "User MSR instruction flow");
        arm920t_destroy(c);
    }
}

/* Thumb shift encodings use zero for immediate LSR/ASR #32. Register ROR
 * takes its amount from Rs[7:0], with distinct carry behavior for 0 and 32.
 * Literal results keep this independent of the ARM interpreter/JIT oracle. */
static void case_thumb_alu_boundaries(int jit) {
    static const struct {
        const char *name;
        uint16_t insn;
        uint32_t r0, r1, carry, result, flags;
    } cases[] = {
        { "LSR-32-negative", 0x0808u, 0, 0x80000001u, 0, 0, 0x70000000u },
        { "LSR-32-positive", 0x0808u, 0, 0x7fffffffu, 1, 0, 0x50000000u },
        { "ASR-32-negative", 0x1008u, 0, 0x80000001u, 0, 0xffffffffu, 0xb0000000u },
        { "ASR-32-positive", 0x1008u, 0, 0x7fffffffu, 1, 0, 0x50000000u },
        { "LSR-31", 0x0fc8u, 0, 0x80000000u, 1, 1, 0x10000000u },
        { "ASR-31", 0x17c8u, 0, 0x80000000u, 1, 0xffffffffu, 0x90000000u },
        { "ROR-32-set-C", 0x41c8u, 0x80000001u, 32, 0, 0x80000001u, 0xb0000000u },
        { "ROR-64-clear-C", 0x41c8u, 0x40000001u, 64, 1, 0x40000001u, 0x10000000u },
        { "ROR-0-preserve-C", 0x41c8u, 0x80000001u, 0, 0, 0x80000001u, 0x90000000u },
        { "ROR-256-preserve-C", 0x41c8u, 0x40000001u, 256, 1, 0x40000001u, 0x30000000u },
        { "ROR-33", 0x41c8u, 0x80000001u, 33, 0, 0xc0000000u, 0xb0000000u },
        { "ROR-alias", 0x41c0u, 0x80000020u, 0, 0, 0x80000020u, 0xb0000000u },
        { "NEG-min", 0x4248u, 0, 0x80000000u, 1, 0x80000000u, 0x90000000u },
        { "NEG-zero", 0x4248u, 0, 0, 0, 0, 0x60000000u },
        { "NEG-one", 0x4248u, 0, 1, 1, 0xffffffffu, 0x80000000u },
    };
    for (unsigned i = 0; i < GP32_ARRAY_COUNT(cases); ++i) {
        current_case = cases[i].name;
        arm920t_t *c = make_cpu(jit);
        gp32_st16le(tb.bios + CODE_ADDR, cases[i].insn);
        arm920t_set_cpsr(c, 0x100000f3u | (cases[i].carry << 29));
        arm920t_set_reg(c, 0, cases[i].r0);
        arm920t_set_reg(c, 1, cases[i].r1);
        CHECK(arm920t_run(c, 1u), 1u, "one instruction");
        CHECK(arm920t_get_reg(c, 0), cases[i].result, "Thumb ALU result");
        CHECK(arm920t_get_cpsr(c), cases[i].flags | 0xf3u, "Thumb ALU flags/state");
        CHECK(arm920t_get_pc(c), CODE_ADDR + 2u, "Thumb ALU next PC");
        arm920t_destroy(c);
    }
}

/* With alignment checking disabled, LDM/STM round addresses down without
 * rotating the loaded words. Writeback retains the original base low bits.
 * Trace mode supplies a raw interpreter oracle independent of decoded blocks. */
static void case_block_alignment(void) {
    for (unsigned engine = 0; engine < 3u; ++engine) {
        for (unsigned callbacks = 0; callbacks < 2u; ++callbacks) {
            for (unsigned load = 0; load < 2u; ++load) {
                current_case = load ? "LDM-unaligned" : "STM-unaligned";
                data_callbacks = (int)callbacks;
                arm920t_t *c = make_cpu(engine == 2u);
                if (engine == 0u) arm920t_set_trace(c, 1, NULL, NULL);
                bios_w32(CODE_ADDR, load ? 0xe8b00006u : 0xe8a00006u); /* LDM/STMIA r0!,{r1,r2} */
                ram_w32(TADDR, 0x33221100u);
                ram_w32(TADDR + 4u, 0x77665544u);
                ram_w32(TADDR + 8u, 0xbbaa9988u);
                arm920t_set_reg(c, 0, TADDR + 2u);
                arm920t_set_reg(c, 1, 0x12345678u);
                arm920t_set_reg(c, 2, 0x90abcdefu);
                arm920t_run(c, 64u); /* transfer, then B . */
                CHECK(arm920t_get_reg(c, 0), TADDR + 10u, "unaligned base writeback");
                if (load) {
                    CHECK(arm920t_get_reg(c, 1), 0x33221100u, "LDM first word must not rotate");
                    CHECK(arm920t_get_reg(c, 2), 0x77665544u, "LDM second word must not rotate");
                } else {
                    CHECK(gp32_ld32le(tb.ram + (TADDR - RAM_BASE)), 0x12345678u, "STM first aligned word");
                    CHECK(gp32_ld32le(tb.ram + (TADDR - RAM_BASE) + 4u), 0x90abcdefu, "STM second aligned word");
                    CHECK(gp32_ld32le(tb.ram + (TADDR - RAM_BASE) + 8u), 0xbbaa9988u, "STM preserves next word");
                }
                if (callbacks) {
                    CHECK(data_accesses, 2u, "one bus access per word");
                    CHECK(data_addresses[0], TADDR, "first callback address");
                    CHECK(data_addresses[1], TADDR + 4u, "second callback address");
                }
                arm920t_destroy(c);
                data_callbacks = 0;
            }
        }
    }
}

static void case_thumb_ldm_base(int jit) {
    /* T1 LDM suppresses writeback when its base is also a destination. */
    for (unsigned base = 0; base < 3u; ++base) {
        current_case = "Thumb-LDM-base-list";
        arm920t_t *c = make_cpu(jit);
        gp32_st16le(tb.bios + CODE_ADDR, (uint16_t)(0xc803u | (base << 8)));
        ram_w32(TADDR, 0x12345678u);
        ram_w32(TADDR + 4u, 0x90abcdefu);
        arm920t_set_cpsr(c, 0xf3u);
        arm920t_set_reg(c, base, TADDR);
        arm920t_run(c, 1u);
        CHECK(arm920t_get_reg(c, 0), 0x12345678u, "LDM loaded r0 survives writeback");
        CHECK(arm920t_get_reg(c, 1), 0x90abcdefu, "LDM loaded r1 survives writeback");
        if (base == 2u) CHECK(arm920t_get_reg(c, 2), TADDR + 8u, "LDM absent base writeback");
        arm920t_destroy(c);
    }
}

static void case_thumb_multiple_alignment(int jit) {
    /* ARMv4T multiple transfers align the bus access, while base/SP
     * writeback retains its low bits. Include LR/PC in stack transfers. */
    static const uint16_t ops[] = {0xc306u, 0xcb06u, 0xb506u, 0xbd06u};
    for (unsigned callbacks = 0; callbacks < 2u; ++callbacks) {
        for (unsigned kind = 0; kind < GP32_ARRAY_COUNT(ops); ++kind) {
            current_case = "Thumb-multiple-alignment";
            data_callbacks = (int)callbacks;
            arm920t_t *c = make_cpu(jit);
            /* LDM/STM use r3 so the base does not alias r1/r2. */
            gp32_st16le(tb.bios + CODE_ADDR, ops[kind]);
            ram_w32(TADDR, 0x12345678u);
            ram_w32(TADDR + 4u, 0x90abcdefu);
            ram_w32(TADDR + 8u, CODE_ADDR + 0x22u);
            ram_w32(TADDR + 12u, 0xaabbccddu);
            arm920t_set_cpsr(c, 0xf3u);
            arm920t_set_reg(c, 3, TADDR + 1u);
            arm920t_set_reg(c, 13, TADDR + (kind == 2u ? 14u : 2u));
            arm920t_set_reg(c, 1, 0x11223344u);
            arm920t_set_reg(c, 2, 0x55667788u);
            arm920t_set_reg(c, 14, 0x99aabbccu);
            arm920t_run(c, 1u);
            if (kind & 1u) {
                CHECK(arm920t_get_reg(c, 1), 0x12345678u, "aligned first Thumb load");
                CHECK(arm920t_get_reg(c, 2), 0x90abcdefu, "aligned second Thumb load");
            } else {
                CHECK(gp32_ld32le(tb.ram + (TADDR - RAM_BASE)), 0x11223344u, "aligned first Thumb store");
                CHECK(gp32_ld32le(tb.ram + (TADDR - RAM_BASE) + 4u), 0x55667788u, "aligned second Thumb store");
                CHECK(gp32_ld32le(tb.ram + (TADDR - RAM_BASE) + (kind == 2u ? 12u : 8u)),
                      kind == 2u ? 0xaabbccddu : CODE_ADDR + 0x22u, "preserve following word");
            }
            if (kind < 2u) CHECK(arm920t_get_reg(c, 3), TADDR + 9u, "Thumb base low bits survive writeback");
            else CHECK(arm920t_get_reg(c, 13), TADDR + (kind == 2u ? 2u : 14u), "Thumb SP low bits survive writeback");
            if (kind == 2u) CHECK(gp32_ld32le(tb.ram + (TADDR - RAM_BASE) + 8u), 0x99aabbccu, "aligned LR store");
            if (kind == 3u) {
                CHECK(arm920t_get_pc(c), CODE_ADDR + 0x22u, "aligned POP PC load");
                CHECK(arm920t_get_cpsr(c) & 0x20u, 0x20u, "POP preserves Thumb state");
            }
            if (callbacks) {
                CHECK(data_accesses, kind < 2u ? 2u : 3u, "one callback per transferred word");
                CHECK(data_addresses[0], kind == 2u ? TADDR + 8u : TADDR, "aligned first bus access");
                CHECK(data_addresses[1], TADDR + 4u, "aligned second bus access");
            }
            arm920t_destroy(c);
            data_callbacks = 0;
        }
    }
}

static void case_mrc_pc_flags(int jit) {
    /* MRC with Rd=15 transfers NZCV, not a branch target (DDI0100I A4-71).
     * Opposite flag patterns cover setting and clearing every flag while
     * preserving the processor mode and interrupt masks. */
    static const uint32_t values[] = {0xa5a55a5au, 0x5a5aa5a5u};
    for (unsigned i = 0; i < GP32_ARRAY_COUNT(values); ++i) {
        current_case = jit ? "MRC-PC-flags-JIT" : "MRC-PC-flags-interpreter";
        arm920t_t *c = make_cpu(jit);
        bios_w32(CODE_ADDR,       0xee030f10u); /* mcr p15,0,r0,c3,c0,0 */
        bios_w32(CODE_ADDR + 4u,  0xee132f10u); /* mrc p15,0,r2,c3,c0,0 */
        bios_w32(CODE_ADDR + 8u,  0xee13ff10u); /* mrc p15,0,pc,c3,c0,0 */
        bios_w32(CODE_ADDR + 12u, 0x13a01011u); /* movne r1,#0x11 */
        bios_w32(CODE_ADDR + 16u, 0x03a01022u); /* moveq r1,#0x22 */
        bios_w32(CODE_ADDR + 20u, 0xea000000u); /* b +28 */
        bios_w32(CODE_ADDR + 28u, 0xeafffff9u); /* b +8: reuse MRC block */
        arm920t_set_reg(c, 0u, values[i]);
        arm920t_set_cpsr(c, ((~values[i]) & 0xf0000000u) | 0xd3u);
        arm920t_run(c, 503u); /* setup + 100 complete five-instruction loops + MRC */
        CHECK(arm920t_get_pc(c), CODE_ADDR + 12u, "MRC PC falls through");
        CHECK(arm920t_get_cpsr(c), (values[i] & 0xf0000000u) | 0xd3u, "only NZCV changes");
        CHECK(arm920t_get_reg(c, 1u), i ? 0x22u : 0x11u, "following predicates use new flags");
        CHECK(arm920t_get_reg(c, 2u), values[i], "ordinary MRC retains all bits");
        arm920t_destroy(c);
    }
}

static void case_arm_undefined_transfer(void) {
    /* ARM920T TRM 2.2.2: bits[27:25]=011 and bit[4]=1 are undefined,
     * not register-shifted LDR/STR. Failed conditions still suppress traps. */
    for (unsigned engine = 0; engine < 3u; ++engine) {
        for (unsigned load = 0; load < 2u; ++load) {
            for (unsigned failed = 0; failed < 2u; ++failed) {
                current_case = "ARM-undefined-transfer";
                arm920t_t *c = make_cpu(engine == 2u);
                data_callbacks = 1; /* enable callbacks after JIT RAM discovery */
                if (engine == 0u) arm920t_set_trace(c, 1, NULL, NULL);
                uint32_t insn = load ? 0xe7910012u : 0xe7810012u;
                if (failed) insn &= 0x0fffffffu; /* EQ with Z=0 */
                bios_w32(CODE_ADDR, insn);
                bios_w32(CODE_ADDR + 4u, 0xe3a03042u); /* mov r3,#0x42 */
                ram_w32(TADDR, 0x88776655u);
                arm920t_set_reg(c, 0u, 0x11223344u);
                arm920t_set_reg(c, 1u, TADDR);
                arm920t_set_cpsr(c, 0xa0000053u); /* SVC, Z=0, I=0, F=1 */
                arm920t_run(c, 64u); /* enough budget for native dispatch */
                CHECK(arm920t_get_pc(c), failed ? CODE_ADDR + 8u : 4u, "predicate selects fallthrough or UND vector");
                CHECK(arm920t_get_cpsr(c), failed ? 0xa0000053u : 0xa00000dbu, "undefined transfer exception state");
                if (!failed) CHECK(arm920t_get_reg(c, 14u), CODE_ADDR + 4u, "ARM undefined LR");
                CHECK(arm920t_get_reg(c, 0u), 0x11223344u, "undefined load preserves destination");
                CHECK(arm920t_get_reg(c, 3u), failed ? 0x42u : 0u, "only failed predicate reaches marker");
                CHECK(gp32_ld32le(tb.ram + (TADDR - RAM_BASE)), 0x88776655u, "undefined store preserves memory");
                CHECK(data_accesses, 0u, "undefined transfers never access data bus");
                arm920t_destroy(c);
                data_callbacks = 0;
            }
        }
    }
}

static void case_cp15_access(void) {
    /* CP15 is privileged (DDI0151C 2.4). Reuse the same translated code as
     * execution moves SVC -> USER -> SYS -> USER; test live, not cached mode. */
    static const struct { uint32_t insn; unsigned crn; int write, flags; } cases[] = {
        {0xee100f10u, 0u, 0, 0}, /* MRC p15,0,r0,c0,c0,0: native read */
        {0xee070f16u, 7u, 1, 0}, /* MCR p15,0,r0,c7,c6,0: native data maintenance */
        {0xee10ff10u, 0u, 0, 1}, /* MRC p15,0,pc,c0,c0,0: helper flags read */
        {0xee030f10u, 3u, 1, 0}, /* MCR p15,0,r0,c3,c0,0: helper write */
    };
    static const uint32_t modes[] = {0x13u, 0x10u, 0x1fu, 0x10u};
    for (unsigned engine = 0; engine < 3u; ++engine) {
        for (unsigned k = 0; k < GP32_ARRAY_COUNT(cases); ++k) {
            for (unsigned failed = 0; failed < 2u; ++failed) {
                current_case = "CP15-live-privilege";
                arm920t_t *c = make_cpu(engine == 2u);
                if (engine == 0u) arm920t_set_trace(c, 1, NULL, NULL);
                bios_w32(CODE_ADDR, 0xe3a04024u); /* mov r4,#0x24 */
                bios_w32(CODE_ADDR + 4u, failed ? cases[k].insn & 0x0fffffffu : cases[k].insn);
                bios_w32(CODE_ADDR + 8u, 0x13a03042u); /* movne r3,#0x42 */
                bios_w32(CODE_ADDR + 12u, 0x03a05077u); /* moveq r5,#0x77 */
                for (unsigned phase = 0; phase < GP32_ARRAY_COUNT(modes); ++phase) {
                    int traps = modes[phase] == 0x10u && !failed;
                    uint32_t old_cp = arm920t_get_cp15(c, cases[k].crn);
                    uint32_t value = 0x11223344u + phase;
                    uint32_t cpsr = 0xa0000040u | modes[phase]; /* Z=0, I=0, F=1 */
                    arm920t_set_cpsr(c, cpsr);
                    arm920t_set_reg(c, 15u, CODE_ADDR);
                    arm920t_set_reg(c, 0u, value);
                    arm920t_set_reg(c, 3u, 0u);
                    arm920t_set_reg(c, 4u, 0u);
                    arm920t_set_reg(c, 5u, 0u);
                    arm920t_run(c, 64u);
                    uint32_t expected_cpsr = traps ? 0xa00000dbu :
                        (cases[k].flags && !failed ? 0x40000040u | modes[phase] : cpsr);
                    CHECK(arm920t_get_pc(c), traps ? 4u : CODE_ADDR + 16u, "CP15 trap or continuation");
                    CHECK(arm920t_get_cpsr(c), expected_cpsr, "CP15 privilege and flags");
                    CHECK(arm920t_get_reg(c, 4u), 0x24u, "prefix before CP15 retires");
                    CHECK(arm920t_get_reg(c, 0u), !traps && !failed && !cases[k].write && !cases[k].flags ? 0x41129200u : value, "CP15 destination");
                    CHECK(arm920t_get_cp15(c, cases[k].crn), !traps && !failed && cases[k].write ? value : old_cp, "rejected CP15 write preserves register");
                    CHECK(arm920t_get_reg(c, 3u), !traps && !(expected_cpsr & 0x40000000u) ? 0x42u : 0u, "following NE predicate");
                    CHECK(arm920t_get_reg(c, 5u), !traps && (expected_cpsr & 0x40000000u) ? 0x77u : 0u, "following EQ predicate");
                    if (traps) CHECK(arm920t_get_reg(c, 14u), CODE_ADDR + 8u, "CP15 exception LR after prefix");
                }
                gp32_cpu_profile_t profile;
                arm920t_get_cpu_profile(c, &profile);
                if (engine == 2u && profile.supported && profile.native_backend)
                    CHECK(profile.native_block_calls != 0u, 1u, "CP15 exercised native blocks");
                arm920t_destroy(c);
            }
        }
    }
}

static void case_coproc_invalid_operation(void) {
    static const uint32_t insns[] = {
        0xee000f00u, /* CDP p15,0,c0,c0,c0,0 */
        0xedb10f01u, /* LDC p15,c0,[r1,#4]! */
        0xeda10f01u, /* STC p15,c0,[r1,#4]! */
        0xee100110u, /* MRC p1,0,r0,c0,c0,0: absent FPA register interface */
        0xee000a10u, /* MCR p10,0,r0,c0,c0,0: absent VFP */
        0xee000000u, /* CDP p0,0,c0,c0,c0,0 */
        0xedb10801u, /* LDC p8,c0,[r1,#4]! */
        0xeda10d01u, /* STC p13,c0,[r1,#4]! */
    };
    for (unsigned engine = 0; engine < 3u; ++engine) {
        for (unsigned k = 0; k < GP32_ARRAY_COUNT(insns); ++k) {
            for (unsigned failed = 0; failed < 2u; ++failed) {
                current_case = "coprocessor-unsupported-operation";
                arm920t_t *c = make_cpu(engine == 2u);
                data_callbacks = 1;
                if (engine == 0u) arm920t_set_trace(c, 1, NULL, NULL);
                bios_w32(CODE_ADDR, failed ? insns[k] & 0x0fffffffu : insns[k]);
                arm920t_set_cpsr(c, 0xa0000053u); /* SVC, Z=0 */
                arm920t_set_reg(c, 0u, 0x55667788u);
                arm920t_set_reg(c, 1u, TADDR);
                ram_w32(TADDR + 4u, 0x11223344u);
                arm920t_run(c, 64u);
                CHECK(arm920t_get_pc(c), failed ? CODE_ADDR + 4u : 4u, "unsupported coprocessor operation traps when condition passes");
                CHECK(arm920t_get_cpsr(c), failed ? 0xa0000053u : 0xa00000dbu, "unsupported coprocessor operation state");
                CHECK(arm920t_get_reg(c, 0u), 0x55667788u, "absent coprocessor preserves destination");
                CHECK(arm920t_get_reg(c, 1u), TADDR, "rejected LDC/STC never writes back");
                CHECK(gp32_ld32le(tb.ram + (TADDR - RAM_BASE) + 4u), 0x11223344u, "rejected STC preserves memory");
                CHECK(data_accesses, 0u, "rejected LDC/STC has no data access");
                arm920t_destroy(c);
                data_callbacks = 0;
            }
        }
    }
}

static void case_thumb_undefined(int jit) {
    /* Conditional-branch condition 1110 is undefined on ARMv4T; 1111 is SWI.
     * Check exception entry and return, independently of the shared decoder. */
    static const uint16_t insns[] = {0xde00u, 0xdeffu};
    for (unsigned i = 0; i < GP32_ARRAY_COUNT(insns); ++i) {
        current_case = jit ? "Thumb-undefined-JIT" : "Thumb-undefined-interpreter";
        arm920t_t *c = make_cpu(jit);
        gp32_st16le(tb.bios + CODE_ADDR, insns[i]);
        gp32_st16le(tb.bios + CODE_ADDR + 2u, 0x2042u); /* movs r0,#0x42 */
        bios_w32(0x04u, 0xe1b0f00eu); /* movs pc,lr */
        arm920t_set_cpsr(c, 0xa0000073u); /* SVC, Thumb, F=1, I=0 */
        CHECK(arm920t_run(c, 1u), 1u, "one undefined instruction");
        CHECK(arm920t_get_pc(c), 0x04u, "undefined vector");
        CHECK(arm920t_get_cpsr(c), 0xa00000dbu, "UND ARM state with IRQ masked");
        CHECK(arm920t_get_reg(c, 14u), CODE_ADDR + 2u, "Thumb exception LR");
        arm920t_run(c, 1u);
        CHECK(arm920t_get_pc(c), CODE_ADDR + 2u, "return skips undefined halfword");
        CHECK(arm920t_get_cpsr(c), 0xa0000073u, "return restores SPSR");
        arm920t_run(c, 1u);
        CHECK(arm920t_get_reg(c, 0u), 0x42u, "resumed Thumb instruction");
        arm920t_destroy(c);
    }
}

int main(void) {
    case_coproc_invalid_operation();
    case_cp15_access();
    case_arm_undefined_transfer();
    case_block_alignment();
    for (int jit = 0; jit <= 1; ++jit) {
        case_thumb_undefined(jit);
        case_mrc_pc_flags(jit);
        case_thumb_multiple_alignment(jit);
        case_thumb_ldm_base(jit);
        case_thumb_alu_boundaries(jit);
        case_user_msr(jit);
        case_thumb_pop(jit);
        case_ldr_pc(jit);
        case_vector_base(jit);
        case_ldm_exception_return(jit);
        case_movs_pc_lr(jit);
        case_irq_subs_pc(jit);
    }
    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    puts("arm_exception_test: all checks passed");
    return 0;
}

