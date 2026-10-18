/* Thumb PUSH must issue the same ascending register/address sequence as
 * STMDB SP!, including when each write has an observable device side effect.
 * Samsung S3C2400 manual 3-41 specifies the order for all STM address modes;
 * 4-31 defines PUSH as STMDB R13!. ARM920T TRM DDI0151C 4-12 also specifies
 * sequential bursts for STM to noncached, nonbuffered regions.
 *
 * This tests instruction-level bus callbacks, not cycle-accurate writeback
 * timing. Like the existing ARM transfer path, source SP remains visible
 * during callbacks and the final SP is committed after all transfers.
 * Defined LDMIA/STMIA cases independently check addresses, loaded/stored
 * values, base-register aliases, and instruction completion boundaries. */
#include "arm920t.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CODE 0x00000400u
#define DEVICE 0x0d000000u
#define BIOS_SIZE 0x10000u
#define FLAGS 0xb0000000u

static uint8_t bios[BIOS_SIZE];
static arm920t_t *cpu;
static uint32_t addresses[9], values[9], observed_sp[9], writes;
static uint32_t read_values[8], reads;
static uint32_t operand_a, operand_b, command_result;
static unsigned request_stop_irq, record_reads, failures, cases, current_list, current_low_bits;
static const char *current_case;

static uint8_t *fastmem(void *user, uint32_t address, size_t size, int write) {
    (void)user; (void)write;
    if (address < BIOS_SIZE && size <= BIOS_SIZE - address) return bios + address;
    return NULL; /* The stack fixture deliberately goes through device callbacks. */
}
static uint8_t read8(void *u, uint32_t a) {
    uint8_t *p = fastmem(u, a, 1u, 0); return p ? p[0] : 0xffu;
}
static uint16_t read16(void *u, uint32_t a) {
    uint8_t *p = fastmem(u, a, 2u, 0); return p ? gp32_ld16le(p) : 0xffffu;
}
static uint32_t read32(void *u, uint32_t a) {
    uint8_t *p = fastmem(u, a, 4u, 0);
    if (p) return gp32_ld32le(p);
    if (!record_reads) return 0xffffffffu;
    uint32_t v = a >= DEVICE && a - DEVICE < sizeof(read_values) && !(a & 3u)
                 ? read_values[(a - DEVICE) / 4u] : 0xffffffffu;
    if (reads < GP32_ARRAY_COUNT(addresses)) {
        addresses[reads] = a;
        values[reads] = v;
    }
    ++reads;
    if (reads == 1u && request_stop_irq) {
        arm920t_stop_run(cpu);
        arm920t_set_irq(cpu, 1);
    }
    return v;
}
static void write8(void *u, uint32_t a, uint8_t v) { (void)u; (void)a; (void)v; }
static void write16(void *u, uint32_t a, uint16_t v) { (void)u; (void)a; (void)v; }
static void write32(void *u, uint32_t a, uint32_t v) {
    (void)u;
    if (writes < GP32_ARRAY_COUNT(addresses)) {
        addresses[writes] = a;
        values[writes] = v;
        observed_sp[writes] = arm920t_get_reg(cpu, 13u);
    }
    ++writes;
    if (a == DEVICE) operand_a = v;
    if (a == DEVICE + 4u) operand_b = v;
    if (a == DEVICE + 8u) command_result = operand_a + operand_b;
    if (writes == 1u && request_stop_irq) {
        arm920t_stop_run(cpu);
        arm920t_set_irq(cpu, 1);
    }
}

static void check(uint32_t got, uint32_t want, const char *what) {
    if (got == want) return;
    if (failures < 20u)
        fprintf(stderr, "FAIL[%s list=%03x low=%u]: %s got=%08x want=%08x\n",
                current_case, current_list, current_low_bits, what, got, want);
    ++failures;
}

static void case_push(unsigned list, unsigned low_bits, int arm, int stop_irq) {
    /* Bit 8 selects LR; bits 0..7 select the corresponding low registers. */
    unsigned count = 0;
    for (unsigned r = 0; r < 9u; ++r) count += !!(list & (1u << r));
    uint32_t source_sp = DEVICE + count * 4u + low_bits;
    uint32_t source[15];
    for (unsigned r = 0; r < GP32_ARRAY_COUNT(source); ++r) source[r] = 0x11110000u + r * 0x01010101u;
    source[13] = source_sp;
    current_case = arm ? "ARM STMDB control" : "Thumb PUSH";
    current_list = list;
    current_low_bits = low_bits;
    writes = operand_a = operand_b = command_result = 0;
    record_reads = reads = 0u;
    memset(addresses, 0, sizeof(addresses));
    memset(values, 0, sizeof(values));
    request_stop_irq = (unsigned)stop_irq;
    arm920t_set_irq(cpu, 0);
    /* Unmask IRQ only for the stop/IRQ callback case. */
    uint32_t cpsr = FLAGS | (stop_irq ? 0x53u : 0xd3u) | (arm ? 0u : 0x20u);
    arm920t_set_cpsr(cpu, cpsr);
    for (unsigned r = 0; r < GP32_ARRAY_COUNT(source); ++r) arm920t_set_reg(cpu, r, source[r]);
    arm920t_set_reg(cpu, 15u, CODE);
    if (arm) gp32_st32le(bios + CODE, 0xe92d0000u | (list & 0xffu) | ((list & 0x100u) << 6));
    else gp32_st16le(bios + CODE, (uint16_t)(0xb400u | list));
    check(arm920t_run(cpu, stop_irq ? 8u : 1u), 1u, "instruction completes before run stops");
    check(writes, count, "one write per selected register");
    unsigned i = 0;
    for (unsigned r = 0; r < 9u; ++r) if (list & (1u << r)) {
        check(addresses[i], DEVICE + i * 4u, "ascending bus address");
        check(values[i], source[r == 8u ? 14u : r], "ascending register value");
        check(observed_sp[i], source_sp, "callbacks see source SP until transfer completes");
        ++i;
    }
    check(arm920t_get_reg(cpu, 13u), DEVICE + low_bits, "final SP retains address low bits");
    for (unsigned r = 0; r < GP32_ARRAY_COUNT(source); ++r)
        if (r != 13u) check(arm920t_get_reg(cpu, r), source[r], "PUSH preserves source registers");
    check(arm920t_get_pc(cpu), CODE + (arm ? 4u : 2u), "next instruction PC");
    check(arm920t_get_cpsr(cpu), cpsr, "PUSH preserves flags and processor state");
    if (list == 0x191u) /* PUSH {r0,r4,r7,lr}: two operands, command, then LR. */
        check(command_result, source[0] + source[4], "command observes preceding operand stores");
    if (stop_irq) {
        /* The pending IRQ is taken on the next instruction boundary, after
         * every PUSH transfer and its final writeback, even with a long budget. */
        gp32_st32le(bios + 0x18u, 0xe1a00000u); /* ARM MOV r0,r0 */
        check(arm920t_run(cpu, 1u), 1u, "next run executes IRQ vector instruction");
        check(arm920t_get_cpsr(cpu) & 0xffu, 0xd2u, "IRQ enters ARM IRQ mode");
        check(arm920t_get_reg(cpu, 14u), CODE + (arm ? 8u : 6u), "IRQ return PC follows completed PUSH");
        check(arm920t_get_pc(cpu), 0x1cu, "IRQ vector instruction completed");
        arm920t_set_cpsr(cpu, cpsr);
        check(arm920t_get_reg(cpu, 13u), DEVICE + low_bits, "banked source SP retains completed writeback");
    }
    ++cases;
}

/* DDI0100I A7-44/A7-97: Thumb multiple transfers visit selected low
 * registers in ascending order; LDM with the base in the list retains the
 * loaded base. STM with a non-first base in the list has an UNPREDICTABLE
 * stored value and is deliberately excluded from this architectural oracle. */
static void case_multiple(unsigned list, unsigned low_bits, unsigned base_reg,
                          int load, int stop_irq) {
    unsigned count = 0u;
    for (unsigned r = 0; r < 8u; ++r) count += !!(list & (1u << r));
    uint32_t source[15], expected[15], base = DEVICE + low_bits;
    for (unsigned r = 0; r < GP32_ARRAY_COUNT(source); ++r)
        source[r] = 0x22220000u + r * 0x01010101u;
    source[base_reg] = base;
    memcpy(expected, source, sizeof(expected));
    for (unsigned i = 0; i < GP32_ARRAY_COUNT(read_values); ++i)
        read_values[i] = 0x98760000u + i * 0x010203u;
    char name[32];
    snprintf(name, sizeof(name), "Thumb %s r%u", load ? "LDMIA" : "STMIA", base_reg);
    current_case = name;
    current_list = list;
    current_low_bits = low_bits;
    record_reads = (unsigned)load;
    reads = writes = operand_a = operand_b = command_result = 0u;
    memset(addresses, 0, sizeof(addresses));
    memset(values, 0, sizeof(values));
    request_stop_irq = (unsigned)stop_irq;
    arm920t_set_irq(cpu, 0);
    uint32_t cpsr = FLAGS | (stop_irq ? 0x73u : 0xf3u);
    arm920t_set_cpsr(cpu, cpsr);
    for (unsigned r = 0; r < GP32_ARRAY_COUNT(source); ++r)
        arm920t_set_reg(cpu, r, source[r]);
    arm920t_set_reg(cpu, 15u, CODE);
    gp32_st16le(bios + CODE, (uint16_t)(0xc000u | (load ? 0x0800u : 0u) |
                                      (base_reg << 8) | list));
    check(arm920t_run(cpu, stop_irq ? 8u : 1u), 1u, "multiple transfer completes before run stops");
    check(reads, load ? count : 0u, "one read per selected register in LDM only");
    check(writes, load ? 0u : count, "one write per selected register in STM only");
    unsigned i = 0u;
    for (unsigned r = 0; r < 8u; ++r) if (list & (1u << r)) {
        check(addresses[i], DEVICE + i * 4u, "multiple transfer uses ascending aligned bus addresses");
        check(values[i], load ? read_values[i] : source[r], "multiple transfer uses ascending register lanes");
        if (load) expected[r] = read_values[i];
        ++i;
    }
    if (!load || !(list & (1u << base_reg))) expected[base_reg] = base + count * 4u;
    for (unsigned r = 0; r < GP32_ARRAY_COUNT(expected); ++r)
        check(arm920t_get_reg(cpu, r), expected[r], "multiple transfer final register value");
    check(arm920t_get_pc(cpu), CODE + 2u, "multiple transfer next PC");
    check(arm920t_get_cpsr(cpu), cpsr, "multiple transfer preserves flags and processor state");
    if (stop_irq) {
        gp32_st32le(bios + 0x18u, 0xe1a00000u); /* ARM MOV r0,r0 */
        check(arm920t_run(cpu, 1u), 1u, "next run executes IRQ vector instruction");
        check(arm920t_get_cpsr(cpu) & 0xffu, 0xd2u, "multiple transfer IRQ enters ARM IRQ mode");
        check(arm920t_get_reg(cpu, 14u), CODE + 6u, "IRQ return PC follows completed multiple transfer");
        check(arm920t_get_pc(cpu), 0x1cu, "multiple transfer IRQ vector instruction completed");
        arm920t_set_cpsr(cpu, cpsr);
        for (unsigned r = 0; r < GP32_ARRAY_COUNT(expected); ++r)
            check(arm920t_get_reg(cpu, r), expected[r], "IRQ preserves completed multiple transfer state");
    }
    ++cases;
}

int main(void) {
    arm_bus_t bus = {.read8=read8, .read16=read16, .read32=read32,
        .write8=write8, .write16=write16, .write32=write32, .fastmem=fastmem};
    cpu = arm920t_create(&bus);
    if (!cpu) { fputs("arm920t_create failed\n", stderr); return 2; }
    arm920t_set_jit(cpu, 0);
    arm920t_set_trace(cpu, 1, NULL, NULL); /* Explicit raw ARM control. */
    for (unsigned arm = 0; arm < 2u; ++arm) {
        unsigned before_failures = failures;
        /* Every legal nonempty Thumb PUSH list, including LR-only and sparse
         * low-register lists. Empty lists are architecturally unpredictable. */
        for (unsigned list = 1; list < 512u; ++list) case_push(list, 0u, (int)arm, 0);
        for (unsigned low = 1; low < 4u; ++low) case_push(0x191u, low, (int)arm, 0);
        case_push(0x191u, 0u, (int)arm, 1);
        if (failures)
            printf("%s: %u failures\n", arm ? "ARM STMDB control" : "Thumb PUSH", failures - before_failures);
    }
    for (unsigned load = 0; load < 2u; ++load) {
        unsigned before_failures = failures;
        for (unsigned base = 0; base < 8u; ++base) {
            for (unsigned list = 1; list < 256u; ++list) {
                if (!load && (list & (1u << base)) && (list & ((1u << base) - 1u))) continue;
                for (unsigned low = 0; low < 4u; ++low)
                    case_multiple(list, low, base, (int)load, 0);
            }
        }
        case_multiple(0x95u, 3u, 1u, (int)load, 1); /* base outside the list */
        case_multiple(0x95u, 0u, 0u, (int)load, 1); /* first register is the base */
        if (failures)
            printf("Thumb %s: %u failures\n", load ? "LDMIA" : "STMIA", failures - before_failures);
    }
    arm920t_destroy(cpu);
    printf("arm_thumb_transfer_test: %u cases, %u failures\n", cases, failures);
    return failures != 0u;
}
