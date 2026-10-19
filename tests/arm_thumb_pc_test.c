/* Independent ARMv4T Thumb PC, branch and register-alias expectations.
 * Reference: ARM DDI0100I A7 (high-register ADD/CMP/MOV, BX, ADR, BL,
 * conditional/unconditional branches, SP arithmetic, and ADD/SUB forms).
 * https://documentation-service.arm.com/static/5f8dacc8f86e16515cdb865a
 *
 * 6,573,424 instruction cases: 4,032,000 high-register operations, 114,688
 * conditional branches, 65,536 unconditional branches, 6,000 valid BX cases,
 * 131,072 ADR/literal loads, 552,960 SP calculations, 1,179,648 BL halfword
 * steps, and 491,520 small ADD/SUB register-alias cases. All 16 initial NZCV
 * combinations are included; PC-relative cases cover both halfword positions.
 * BL covers all low offset fields, signed high offset boundaries, and a
 * 4-KiB boundary. The BL halves use separate instruction budgets without
 * interrupt injection; exception handling is tested by arm_exception_test.
 *
 * Exclude architecturally UNPREDICTABLE high-operation low/low register
 * encodings, CMP with PC as its first operand, and BX to an ARM target with
 * address bits[1:0]=10. Expected flags use wide arithmetic and signed bounds;
 * offsets use signed numeric ranges instead of production sign-extension. */
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
static arm920t_t *cpu;
static void prepare(uint32_t before[16], uint32_t after[16], uint32_t a, uint32_t b) {
    for (unsigned r = 0; r < 15; ++r) before[r] = 0x01020304u * (r + 1u);
    before[0] = a; before[1] = b; before[15] = CODE;
    memcpy(after, before, 16 * sizeof(*after)); after[15] += 2;
}
static const uint32_t edges[] = {
    0,1,2,31,32,33,255,256,0x7fffffffu,0x80000000u,0x80000001u,
    0xfffffffeu,0xffffffffu,0x55555555u,0xaaaaaaaau
};

static void execute_at(uint16_t op, const uint32_t before[16], uint32_t cpsr,
                       const uint32_t after[16], uint32_t expected_cpsr) {
    last_op = op; last_a = before[0]; last_b = before[1]; last_flags = cpsr;
    gp32_st16le(bios + before[15], op);
    arm920t_set_cpsr(cpu, cpsr);
    for (unsigned r = 0; r < 16; ++r) arm920t_set_reg(cpu, r, before[r]);
    check(arm920t_run(cpu, 1), 1, "one instruction");
    for (unsigned r = 0; r < 16; ++r) {
        uint32_t got = arm920t_get_reg(cpu, r);
        if (got != after[r]) {
            char what[16]; snprintf(what, sizeof(what), "r%u", r);
            check(got, after[r], what);
        }
    }
    check(arm920t_get_cpsr(cpu), expected_cpsr, "CPSR");
    ++checked;
}

static void high_regs(void) {
    phase = "High-register ADD/CMP/MOV";
    for (unsigned op = 0; op < 3; ++op)
        for (unsigned rd = 0; rd < 16; ++rd)
            for (unsigned rs = 0; rs < 16; ++rs) {
                if ((rd < 8 && rs < 8) || (op == 1 && rd == 15)) continue;
                for (unsigned ai = 0; ai < GP32_ARRAY_COUNT(edges); ++ai)
                    for (unsigned bi = 0; bi < GP32_ARRAY_COUNT(edges); ++bi)
                        for (unsigned flags = 0; flags < 16; ++flags)
                            for (unsigned half = 0; half < 2; ++half) {
                                uint32_t before[16], after[16], f = flags << 28;
                                prepare(before, after, 0, 0);
                                if (rd != 15) before[rd] = edges[ai];
                                if (rs != 15) before[rs] = edges[bi];
                                before[15] = CODE + half * 2;
                                memcpy(after, before, sizeof(after)); after[15] += 2;
                                uint32_t a = rd == 15 ? before[15] + 4 : before[rd];
                                uint32_t b = rs == 15 ? before[15] + 4 : before[rs];
                                if (op == 1) arith(a, b, 0, 1, &f);
                                else {
                                    uint32_t r = op ? b : (uint32_t)((uint64_t)a + b);
                                    after[rd] = rd == 15 ? r & ~1u : r;
                                }
                                execute_at((uint16_t)(0x4400 | (op << 8) | ((rd & 8) << 4) |
                                           (rs << 3) | (rd & 7)), before, (flags << 28) | 0xf3,
                                           after, f | 0xf3);
                            }
            }
}

static int condition(unsigned cond, uint32_t flags) {
    int n = !!(flags & N), z = !!(flags & Z), c = !!(flags & C), v = !!(flags & V);
    switch (cond) {
    case 0: return z; case 1: return !z; case 2: return c; case 3: return !c;
    case 4: return n; case 5: return !n; case 6: return v; case 7: return !v;
    case 8: return c && !z; case 9: return !c || z; case 10: return n == v;
    case 11: return n != v; case 12: return !z && n == v; case 13: return z || n != v;
    }
    return 0;
}

static void branches(void) {
    phase = "Thumb conditional branches";
    for (unsigned cond = 0; cond < 14; ++cond)
        for (unsigned imm = 0; imm < 256; ++imm)
            for (unsigned flags = 0; flags < 16; ++flags)
                for (unsigned half = 0; half < 2; ++half) {
                    uint32_t before[16], after[16]; prepare(before, after, 0, 0);
                    before[15] = CODE + half * 2; after[15] = before[15] + 2;
                    int displacement = (imm < 128 ? (int)imm : (int)imm - 256) * 2;
                    if (condition(cond, flags << 28)) after[15] = (uint32_t)((int64_t)before[15] + 4 + displacement);
                    execute_at((uint16_t)(0xd000 | (cond << 8) | imm), before, (flags << 28) | 0xf3,
                               after, (flags << 28) | 0xf3);
                }
    phase = "Thumb unconditional branches";
    for (unsigned imm = 0; imm < 2048; ++imm)
        for (unsigned flags = 0; flags < 16; ++flags)
            for (unsigned half = 0; half < 2; ++half) {
                uint32_t before[16], after[16]; prepare(before, after, 0, 0);
                before[15] = CODE + half * 2;
                int displacement = (imm < 1024 ? (int)imm : (int)imm - 2048) * 2;
                after[15] = (uint32_t)((int64_t)before[15] + 4 + displacement);
                execute_at((uint16_t)(0xe000 | imm), before, (flags << 28) | 0xf3,
                           after, (flags << 28) | 0xf3);
            }
    phase = "Thumb BX";
    for (unsigned rs = 0; rs < 16; ++rs)
        for (unsigned value = 0; value < GP32_ARRAY_COUNT(edges); ++value)
            for (unsigned flags = 0; flags < 16; ++flags)
                for (unsigned half = 0; half < 2; ++half) {
                    uint32_t before[16], after[16]; prepare(before, after, 0, 0);
                    if (rs < 15) before[rs] = edges[value];
                    before[15] = CODE + half * 2;
                    memcpy(after, before, sizeof(after));
                    uint32_t target = rs == 15 ? before[15] + 4 : before[rs];
                    if ((target & 3) == 2) continue; /* Unaligned ARM target is UNPREDICTABLE. */
                    after[15] = target & ~1u;
                    execute_at((uint16_t)(0x4700 | (rs << 3)), before, (flags << 28) | 0xf3,
                               after, (flags << 28) | 0xd3 | ((target & 1) ? 0x20 : 0));
                }
}

static void relative_address(void) {
    phase = "Thumb ADR and literal LDR";
    for (unsigned load = 0; load < 2; ++load)
        for (unsigned rd = 0; rd < 8; ++rd)
            for (unsigned imm = 0; imm < 256; ++imm)
                for (unsigned flags = 0; flags < 16; ++flags)
                    for (unsigned half = 0; half < 2; ++half) {
                        uint32_t before[16], after[16]; prepare(before, after, 0, 0);
                        before[15] = CODE + half * 2; after[15] = before[15] + 2;
                        uint32_t addr = (before[15] + 4) / 4 * 4 + imm * 4;
                        uint32_t literal = addr ^ 0xdeadbeefu;
                        if (load) gp32_st32le(bios + addr, literal);
                        after[rd] = load ? literal : addr;
                        execute_at((uint16_t)((load ? 0x4800 : 0xa000) | (rd << 8) | imm), before,
                                   (flags << 28) | 0xf3, after, (flags << 28) | 0xf3);
                    }
    phase = "Thumb SP-relative ADD and SP adjustment";
    for (unsigned kind = 0; kind < 3; ++kind)
        for (unsigned rd = 0; rd < (kind ? 1 : 8); ++rd)
            for (unsigned imm = 0; imm < (kind ? 128 : 256); ++imm)
                for (unsigned value = 0; value < GP32_ARRAY_COUNT(edges); ++value)
                    for (unsigned flags = 0; flags < 16; ++flags) {
                        uint32_t before[16], after[16]; prepare(before, after, 0, 0);
                        before[13] = after[13] = edges[value];
                        unsigned dest = kind ? 13 : rd;
                        after[dest] = kind == 2 ? before[13] - imm * 4 : before[13] + imm * 4;
                        execute_at((uint16_t)(kind ? 0xb000 | (kind == 2 ? 0x80 : 0) | imm :
                                                       0xa800 | (rd << 8) | imm),
                                   before, (flags << 28) | 0xf3, after, (flags << 28) | 0xf3);
                    }
}

static void long_branch(void) {
    phase = "Thumb BL halves, link and range boundaries";
    static const unsigned high[] = {0,1,0x3ff,0x400,0x7fe,0x7ff};
    static const uint32_t locations[] = {CODE, CODE+2, 0x1ffe};
    for (unsigned hi = 0; hi < GP32_ARRAY_COUNT(high); ++hi)
        for (unsigned lo = 0; lo < 2048; ++lo)
            for (unsigned flags = 0; flags < 16; ++flags)
                for (unsigned loc = 0; loc < GP32_ARRAY_COUNT(locations); ++loc) {
                    uint32_t before[16], middle[16], after[16]; prepare(before, middle, 0, 0);
                    before[15] = locations[loc]; middle[15] = before[15] + 2;
                    int64_t displacement = (high[hi] < 1024 ? (int64_t)high[hi] : (int64_t)high[hi] - 2048) * 4096;
                    middle[14] = (uint32_t)((int64_t)before[15] + 4 + displacement);
                    execute_at((uint16_t)(0xf000 | high[hi]), before, (flags << 28) | 0xf3,
                               middle, (flags << 28) | 0xf3);
                    memcpy(after, middle, sizeof(after));
                    after[15] = (uint32_t)((int64_t)before[15] + 4 + displacement + lo * 2);
                    after[14] = (before[15] + 4) | 1u;
                    execute_at((uint16_t)(0xf800 | lo), middle, (flags << 28) | 0xf3,
                               after, (flags << 28) | 0xf3);
                }
}

static void small_add_sub(void) {
    phase = "Thumb small ADD/SUB register aliases";
    for (unsigned sub = 0; sub < 2; ++sub)
        for (unsigned immediate = 0; immediate < 2; ++immediate)
            for (unsigned rd = 0; rd < 8; ++rd)
                for (unsigned rs = 0; rs < 8; ++rs)
                    for (unsigned rn = 0; rn < 8; ++rn)
                        for (unsigned value = 0; value < GP32_ARRAY_COUNT(edges); ++value)
                            for (unsigned flags = 0; flags < 16; ++flags) {
                                uint32_t before[16], after[16], f;
                                prepare(before, after, 0, 0);
                                for (unsigned r = 0; r < 8; ++r) before[r] = edges[(value + r) % GP32_ARRAY_COUNT(edges)];
                                memcpy(after, before, sizeof(after)); after[15] += 2;
                                after[rd] = arith(before[rs], immediate ? rn : before[rn], 0, (int)sub, &f);
                                execute_at((uint16_t)(0x1800 | (sub << 9) | (immediate << 10) |
                                           (rn << 6) | (rs << 3) | rd), before, (flags << 28) | 0xf3,
                                           after, f | 0xf3);
                            }
}

int main(void) {
    arm_bus_t bus = {.read8=r8, .read16=r16, .read32=r32, .write8=w8,
        .write16=w16, .write32=w32, .fastmem=mem};
    cpu = arm920t_create(&bus); if (!cpu) return 2;
    arm920t_set_jit(cpu, 0); arm920t_set_trace(cpu, 1, NULL, NULL);
    high_regs(); branches(); relative_address(); long_branch(); small_add_sub();
    printf("arm_thumb_pc_test: %" PRIu64 " instruction cases, %u failures\n", checked, failures);
    arm920t_destroy(cpu); return failures != 0;
}
