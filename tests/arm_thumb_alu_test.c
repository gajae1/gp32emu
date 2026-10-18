/* Independent ARMv4T Thumb arithmetic and shift expectations.
 * ARM DDI0100I A7 defines the tested operations and affected flags.
 * https://documentation-service.arm.com/static/5f8dacc8f86e16515cdb865a
 *
 * The oracle uses 64-bit sums and numeric signed-range checks for carry and
 * overflow; shifts use repeated one-bit steps instead of the interpreter's
 * zero/32/large-shift branches. All 16 initial NZCV combinations are covered.
 * No expected ALU result calls a production shifter, flag or rotate helper.
 *
 * 598,800 cases: 61,200 register ALU operand/alias cases, 245,760 register
 * shifts covering every low-byte amount with nonzero upper bits, 46,080
 * immediate shifts, and 245,760 immediate MOV/CMP/ADD/SUB cases.
 * MUL Rd,Rd and the post-MUL C flag are excluded because they are
 * UNPREDICTABLE on ARMv4T (DDI0100I A7-78). V is checked as preserved. */
#include "arm920t.h"
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CODE 0x1000u
#define N 0x80000000u
#define Z 0x40000000u
#define C 0x20000000u
#define V 0x10000000u
static uint8_t bios[65536];
static unsigned failures;
static uint64_t checked;
static uint16_t last_op;
static uint32_t last_a, last_b, last_flags;
static const char *phase;

static uint8_t *mem(void *u, uint32_t a, size_t n, int w) {
    (void)u; (void)w;
    return a < sizeof(bios) && n <= sizeof(bios) - a ? bios + a : NULL;
}
static uint8_t r8(void *u, uint32_t a) { uint8_t *p = mem(u,a,1,0); return p ? p[0] : 0; }
static uint16_t r16(void *u, uint32_t a) { uint8_t *p = mem(u,a,2,0); return p ? gp32_ld16le(p) : 0; }
static uint32_t r32(void *u, uint32_t a) { uint8_t *p = mem(u,a,4,0); return p ? gp32_ld32le(p) : 0; }
static void w8(void *u, uint32_t a, uint8_t v) { (void)u; (void)a; (void)v; }
static void w16(void *u, uint32_t a, uint16_t v) { (void)u; (void)a; (void)v; }
static void w32(void *u, uint32_t a, uint32_t v) { (void)u; (void)a; (void)v; }
static void check(uint32_t got, uint32_t want, const char *what) {
    if (got == want) return;
    if (failures < 24)
        fprintf(stderr, "%s op=%04x a=%08x b=%08x flags=%08x: %s got=%08x want=%08x\n",
                phase, last_op, last_a, last_b, last_flags, what, got, want);
    ++failures;
}
static int64_t signed32(uint32_t v) { return v <= INT32_MAX ? (int64_t)v : (int64_t)v - INT64_C(4294967296); }
static uint32_t nz(uint32_t r) { return (r >= UINT32_C(2147483648) ? N : 0) | (!r ? Z : 0); }
static uint32_t arith(uint32_t a, uint32_t b, unsigned carry, int subtract, uint32_t *flags) {
    int64_t exact = subtract ? signed32(a) - signed32(b) - carry : signed32(a) + signed32(b) + carry;
    uint64_t ua = a, ub = (uint64_t)b + carry;
    uint32_t r = subtract ? (uint32_t)(ua - ub) : (uint32_t)(ua + ub);
    *flags = nz(r) | ((subtract ? ua >= ub : ua + ub >= UINT64_C(4294967296)) ? C : 0)
                  | ((exact < INT32_MIN || exact > INT32_MAX) ? V : 0);
    return r;
}
/* Repeated one-bit steps make the oracle independent of the CPU's large-
 * shift cases. No C shift ever exceeds one, including ASR and ROR. */
static uint32_t shift(uint32_t v, unsigned amount, unsigned kind, uint32_t *flags) {
    unsigned carry = !!(*flags & C);
    for (unsigned step = 0; step < amount; ++step) {
        if (!kind) { carry = v >= UINT32_C(2147483648); v *= 2u; }
        else {
            carry = v % 2u;
            uint32_t top = kind == 2 ? (v & N) : kind == 3 && carry ? N : 0;
            v = v / 2u + top;
        }
    }
    *flags = nz(v) | (carry ? C : 0) | (*flags & V);
    return v;
}
static arm920t_t *cpu;
static void execute(uint16_t op, const uint32_t before[16], uint32_t flags,
                    const uint32_t after[16], uint32_t expected_flags, uint32_t flags_mask) {
    last_op = op; last_a = before[0]; last_b = before[1]; last_flags = flags;
    gp32_st16le(bios + CODE, op);
    arm920t_set_cpsr(cpu, flags | 0xf3u);
    for (unsigned r = 0; r < 16; ++r) arm920t_set_reg(cpu, r, before[r]);
    check(arm920t_run(cpu, 1), 1, "one instruction");
    for (unsigned r = 0; r < 16; ++r) {
        char what[16]; snprintf(what, sizeof(what), "r%u", r);
        check(arm920t_get_reg(cpu, r), after[r], what);
    }
    check(arm920t_get_cpsr(cpu) & flags_mask, (expected_flags | 0xf3u) & flags_mask, "CPSR");
    ++checked;
}
static void prepare(uint32_t before[16], uint32_t after[16], uint32_t a, uint32_t b) {
    for (unsigned r = 0; r < 15; ++r) before[r] = 0x01020304u * (r + 1u);
    before[0] = a; before[1] = b; before[15] = CODE;
    memcpy(after, before, 16 * sizeof(*after)); after[15] += 2;
}
static const uint32_t edges[] = {
    0,1,2,31,32,33,255,256,0x7fffffffu,0x80000000u,0x80000001u,
    0xfffffffeu,0xffffffffu,0x55555555u,0xaaaaaaaau
};
static void alu_case(unsigned op, unsigned alias, uint32_t a, uint32_t b, uint32_t flags) {
    uint32_t before[16], after[16], f = flags, mask = UINT32_MAX;
    prepare(before, after, a, b);
    if (alias) b = a;
    uint32_t result = a; int store = 1;
    switch (op) {
    case 0: result = a & b; f = nz(result) | (flags & (C | V)); break;
    case 1: result = a ^ b; f = nz(result) | (flags & (C | V)); break;
    case 2: result = shift(a, b % 256u, 0, &f); break;
    case 3: result = shift(a, b % 256u, 1, &f); break;
    case 4: result = shift(a, b % 256u, 2, &f); break;
    case 5: result = arith(a, b, !!(flags & C), 0, &f); break;
    case 6: result = arith(a, b, !(flags & C), 1, &f); break;
    case 7: result = shift(a, b % 256u, 3, &f); break;
    case 8: result = a & b; f = nz(result) | (flags & (C | V)); store = 0; break;
    case 9: result = arith(0, b, 0, 1, &f); break;
    case 10: result = arith(a, b, 0, 1, &f); store = 0; break;
    case 11: result = arith(a, b, 0, 0, &f); store = 0; break;
    case 12: result = a | b; f = nz(result) | (flags & (C | V)); break;
    case 13: result = (uint32_t)((uint64_t)a * b); f = nz(result) | (flags & V); mask &= ~C; break;
    case 14: result = a & ~b; f = nz(result) | (flags & (C | V)); break;
    case 15: result = ~b; f = nz(result) | (flags & (C | V)); break;
    }
    if (store) after[0] = result;
    execute((uint16_t)(0x4000u | (op << 6) | (alias ? 0 : 8)), before, flags, after, f, mask);
}
static void arithmetic(void) {
    phase = "Thumb ALU";
    for (unsigned op = 0; op < 16; ++op)
        for (unsigned alias = 0; alias < 2; ++alias) {
            /* MUL Rd,Rd is UNPREDICTABLE on ARMv4T (DDI0100I A7-78). */
            if (op == 13 && alias) continue;
            for (unsigned ai = 0; ai < GP32_ARRAY_COUNT(edges); ++ai)
                for (unsigned bi = 0; bi < (alias ? 1u : GP32_ARRAY_COUNT(edges)); ++bi)
                    for (unsigned flags = 0; flags < 16; ++flags)
                        alu_case(op, alias, edges[ai], edges[bi], flags << 28);
        }
    phase = "Thumb register shifts, all low-byte amounts";
    static const unsigned shift_ops[] = {2,3,4,7};
    for (unsigned op = 0; op < GP32_ARRAY_COUNT(shift_ops); ++op)
        for (unsigned ai = 0; ai < GP32_ARRAY_COUNT(edges); ++ai)
            for (unsigned amount = 0; amount < 256; ++amount)
                for (unsigned flags = 0; flags < 16; ++flags)
                    alu_case(shift_ops[op], 0, edges[ai], 0xa5ffff00u | amount, flags << 28);
    phase = "Thumb immediate shifts";
    for (unsigned kind = 0; kind < 3; ++kind)
        for (unsigned amount = 0; amount < 32; ++amount)
            for (unsigned ai = 0; ai < GP32_ARRAY_COUNT(edges); ++ai)
                for (unsigned flags = 0; flags < 16; ++flags)
                    for (unsigned alias = 0; alias < 2; ++alias) {
                        uint32_t before[16], after[16], f = flags << 28;
                        prepare(before, after, edges[ai], edges[ai]);
                        after[0] = shift(edges[ai], !amount && kind ? 32u : amount, kind, &f);
                        execute((uint16_t)((kind << 11) | (amount << 6) | (alias ? 0 : 8)),
                                before, flags << 28, after, f, UINT32_MAX);
                    }
    phase = "Thumb immediate MOV/CMP/ADD/SUB";
    for (unsigned op = 0; op < 4; ++op)
        for (unsigned imm = 0; imm < 256; ++imm)
            for (unsigned ai = 0; ai < GP32_ARRAY_COUNT(edges); ++ai)
                for (unsigned flags = 0; flags < 16; ++flags) {
                    uint32_t before[16], after[16], f = flags << 28, result;
                    prepare(before, after, edges[ai], 0);
                    if (!op) { result = imm; f = nz(result) | (f & (C | V)); }
                    else result = arith(edges[ai], imm, 0, op != 2, &f);
                    if (op != 1) after[0] = result;
                    execute((uint16_t)(0x2000u | (op << 11) | imm), before, flags << 28, after, f, UINT32_MAX);
                }
}
int main(void) {
    arm_bus_t bus = {.read8=r8, .read16=r16, .read32=r32, .write8=w8,
        .write16=w16, .write32=w32, .fastmem=mem};
    cpu = arm920t_create(&bus);
    if (!cpu) return 2;
    arm920t_set_jit(cpu, 0);
    arithmetic();
    printf("arm_thumb_alu_test: %" PRIu64 " cases, %u failures\n", checked, failures);
    arm920t_destroy(cpu);
    return failures != 0;
}
