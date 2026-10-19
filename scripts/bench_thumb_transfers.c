/* Isolated Thumb LDM/STM iteration experiment. No external ROMs or devices.
 * argv: Thumb (0/1), nonempty register mask (hex), guest instructions.
 * Compile this same harness against either source, with profiling OFF. */
#define _POSIX_C_SOURCE 200809L
#include "arm920t.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CODE 0x400u
#define DATA 0x0c003000u
#define RAM_BASE 0x0c000000u
#define MEM_SIZE 0x10000u
#define WARMUP 30000u
static uint8_t bios[MEM_SIZE], ram[MEM_SIZE];

static uint8_t *memory(void *user, uint32_t a, size_t size, int write) {
    (void)user; (void)write;
    if (a < MEM_SIZE && size <= MEM_SIZE - a) return bios + a;
    if (a >= RAM_BASE && a - RAM_BASE < MEM_SIZE && size <= MEM_SIZE - (a - RAM_BASE))
        return ram + a - RAM_BASE;
    return NULL;
}
static uint8_t read8(void *u, uint32_t a) {
    uint8_t *p = memory(u, a, 1u, 0); return p ? *p : 0xffu;
}
static uint16_t read16(void *u, uint32_t a) {
    uint8_t *p = memory(u, a, 2u, 0); return p ? gp32_ld16le(p) : 0xffffu;
}
static uint32_t read32(void *u, uint32_t a) {
    uint8_t *p = memory(u, a, 4u, 0); return p ? gp32_ld32le(p) : 0xffffffffu;
}
static void write8(void *u, uint32_t a, uint8_t v) {
    uint8_t *p = memory(u, a, 1u, 1); if (p) *p = v;
}
static void write16(void *u, uint32_t a, uint16_t v) {
    uint8_t *p = memory(u, a, 2u, 1); if (p) gp32_st16le(p, v);
}
static void write32(void *u, uint32_t a, uint32_t v) {
    uint8_t *p = memory(u, a, 4u, 1); if (p) gp32_st32le(p, v);
}

static int checked_state(arm920t_t *cpu, unsigned list, unsigned base_reg, uint32_t cpsr,
                         const uint32_t source[15], uint64_t *hash) {
    uint32_t expected[15];
    memcpy(expected, source, sizeof(expected));
    unsigned count = 0u;
    int ok = 1;
    for (unsigned r = 0u; r < 15u; ++r) if (list & (1u << r)) {
        uint32_t value = gp32_ld32le(ram + DATA - RAM_BASE + count * 4u);
        ok &= value == source[r];
        *hash = (*hash ^ value) * UINT64_C(1099511628211);
        ++count;
    }
    expected[base_reg] = list & (1u << base_reg) ? DATA : DATA + 4u * count;
    for (unsigned r = 0u; r < 15u; ++r) {
        uint32_t value = arm920t_get_reg(cpu, r);
        ok &= value == expected[r];
        *hash = (*hash ^ value) * UINT64_C(1099511628211);
    }
    ok &= arm920t_get_pc(cpu) == CODE && arm920t_get_cpsr(cpu) == cpsr;
    return ok;
}

int main(int argc, char **argv) {
    if (argc != 4) return 2;
    unsigned thumb = (unsigned)strtoul(argv[1], NULL, 0);
    unsigned list = (unsigned)strtoul(argv[2], NULL, 16);
    unsigned budget = (unsigned)strtoul(argv[3], NULL, 0);
    if (thumb > 1u || !list || list > 0xffu || !budget || budget % 5u) return 2;
    arm_bus_t bus = {.read8=read8, .read16=read16, .read32=read32,
        .write8=write8, .write16=write16, .write32=write32, .fastmem=memory};
    arm920t_t *cpu = arm920t_create(&bus);
    if (!cpu) return 2;
    arm920t_set_jit(cpu, 0);
    arm920t_set_trace(cpu, 1, NULL, NULL); /* Force raw ARM control. */
    uint32_t cpsr = 0xa00000d3u | (thumb ? 0x20u : 0u), source[15];
    arm920t_set_cpsr(cpu, cpsr);
    for (unsigned r = 0u; r < 15u; ++r) source[r] = 0x76540000u + r * 0x010203u;
    unsigned base_reg = thumb ? 0u : 12u;
    source[base_reg] = source[13] = DATA;
    for (unsigned r = 0u; r < 15u; ++r) arm920t_set_reg(cpu, r, source[r]);
    arm920t_set_reg(cpu, 15u, CODE);
    if (thumb) {
        const uint16_t program[] = {0x4668u, (uint16_t)(0xc000u | list),
            0x4668u, (uint16_t)(0xc800u | list), 0xe7fau};
        for (unsigned i = 0u; i < GP32_ARRAY_COUNT(program); ++i)
            gp32_st16le(bios + CODE + i * 2u, program[i]);
    } else {
        /* The raw ARM control uses r12, outside every selected list: unlike
         * Thumb LDMIA, ARM LDM writeback/base aliases can be UNPREDICTABLE. */
        const uint32_t program[] = {0xe1a0c00du, 0xe8ac0000u | list,
            0xe1a0c00du, 0xe8bc0000u | list, 0xeafffffau};
        for (unsigned i = 0u; i < GP32_ARRAY_COUNT(program); ++i)
            gp32_st32le(bios + CODE + i * 4u, program[i]);
    }
    uint64_t hash = UINT64_C(14695981039346656037);
    if (arm920t_run(cpu, WARMUP) != WARMUP || !checked_state(cpu, list, base_reg, cpsr, source, &hash)) {
        fputs("warmup state mismatch\n", stderr); arm920t_destroy(cpu); return 1;
    }
    struct timespec begin, end;
    if (clock_gettime(CLOCK_MONOTONIC, &begin)) { arm920t_destroy(cpu); return 2; }
    unsigned retired = arm920t_run(cpu, budget);
    if (clock_gettime(CLOCK_MONOTONIC, &end)) { arm920t_destroy(cpu); return 2; }
    double seconds = end.tv_sec - begin.tv_sec + (end.tv_nsec - begin.tv_nsec) * 1e-9;
    int ok = retired == budget && checked_state(cpu, list, base_reg, cpsr, source, &hash);
    printf("thumb=%u list=%04x retired=%u ns_per_insn=%.4f hash=%016" PRIx64 " checked=%d\n",
           thumb, list, retired, seconds * 1e9 / retired, hash, ok);
    arm920t_destroy(cpu);
    return !ok;
}
