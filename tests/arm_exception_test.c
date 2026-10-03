/* ARMv4T exception returns take state from SPSR and align the return PC
 * after restoring CPSR. Assert architectural results in both execution modes;
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
static const char *current_case = "init";
static int failures;

static uint8_t *bus_ptr(test_bus_t *b, uint32_t a, size_t bytes) {
    if (a < BIOS_SIZE && bytes <= BIOS_SIZE - a) return b->bios + a;
    if (a >= RAM_BASE && (uint64_t)(a - RAM_BASE) + bytes <= RAM_SIZE) return b->ram + (a - RAM_BASE);
    return NULL;
}
static uint8_t rd8(void *u, uint32_t a)  { uint8_t *p = bus_ptr(u, a, 1); return p ? p[0] : 0xffu; }
static uint16_t rd16(void *u, uint32_t a){ uint8_t *p = bus_ptr(u, a, 2); return p ? gp32_ld16le(p) : 0xffffu; }
static uint32_t rd32(void *u, uint32_t a){ uint8_t *p = bus_ptr(u, a, 4); return p ? gp32_ld32le(p) : 0xffffffffu; }
static void wr8(void *u, uint32_t a, uint8_t v)  { uint8_t *p = bus_ptr(u, a, 1); if (p) p[0] = v; }
static void wr16(void *u, uint32_t a, uint16_t v){ uint8_t *p = bus_ptr(u, a, 2); if (p) gp32_st16le(p, v); }
#define IRQ_ACK_ADDR 0x0d000000u
static void wr32(void *u, uint32_t a, uint32_t v){
    uint8_t *p = bus_ptr(u, a, 4);
    if (p) gp32_st32le(p, v);
    else if (a == IRQ_ACK_ADDR && tb_cpu) arm920t_set_irq(tb_cpu, 0); /* guest ack */
}
static uint8_t *fm(void *u, uint32_t a, size_t bytes, int w) { (void)w; return bus_ptr(u, a, bytes); }

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

int main(void) {
    for (int jit = 0; jit <= 1; ++jit) {
        case_ldm_exception_return(jit);
        case_movs_pc_lr(jit);
        case_irq_subs_pc(jit);
    }
    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    puts("arm_exception_test: all checks passed");
    return 0;
}

