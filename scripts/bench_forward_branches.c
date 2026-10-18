/* Synthetic transparent byte-copy loops; no ROM or device timing.
 * Build with -std=c2x -O3 -D_XOPEN_SOURCE=700 -Isrc -Iinclude.
 * CORE_UNDER_TEST can select a baseline arm920t.c. Profiling defaults off;
 * a separate -DGP32EMU_CPU_PROFILE=1 build verifies actual native coverage.
 * Args: mode (0 cached, 1 native, 2 raw, 3 translate/compile), MMU, zero threshold (0..256),
 * opaque-path width (1,2,4,8), forward branch (0,1), pixels, passes, verify.
 * verify=1 suppresses the clock so functional setup needs no timing window.
 * Width 1 only stores a pixel; later widths add counts and checksums. */
#ifndef GP32EMU_CPU_PROFILE
#define GP32EMU_CPU_PROFILE 0
#endif
#ifndef CORE_UNDER_TEST
#define CORE_UNDER_TEST "../src/arm920t.c"
#endif
#include CORE_UNDER_TEST
#include <time.h>

#define BENCH_RAM 0x0c000000u
#define BENCH_RAM_SIZE 0x800000u
#define BENCH_BIOS_SIZE 0x80000u
#define BENCH_PC 0x400u
#define BENCH_SRC (BENCH_RAM + 0x1000u)
#define BENCH_DST (BENCH_RAM + 0x20000u)
static uint8_t bench_ram[BENCH_RAM_SIZE], bench_bios[BENCH_BIOS_SIZE];
static unsigned swi_calls;
static uint32_t expected_swi;

static uint8_t *bench_ptr(void *user, uint32_t a, size_t n, int write) {
    GP32_UNUSED(user);
    if (!write && a < BENCH_BIOS_SIZE && n <= BENCH_BIOS_SIZE - a) return bench_bios + a;
    if (a >= BENCH_RAM && (uint64_t)(a - BENCH_RAM) + n <= BENCH_RAM_SIZE)
        return bench_ram + a - BENCH_RAM;
    return NULL;
}
static uint8_t bench_read8(void *u, uint32_t a) { uint8_t *p = bench_ptr(u,a,1,0); return p ? *p : 0; }
static uint16_t bench_read16(void *u, uint32_t a) { uint8_t *p = bench_ptr(u,a,2,0); return p ? gp32_ld16le(p) : 0; }
static uint32_t bench_read32(void *u, uint32_t a) { uint8_t *p = bench_ptr(u,a,4,0); return p ? gp32_ld32le(p) : 0; }
static void bench_write8(void *u, uint32_t a, uint8_t v) { uint8_t *p = bench_ptr(u,a,1,1); if (p) *p = v; }
static void bench_write16(void *u, uint32_t a, uint16_t v) { uint8_t *p = bench_ptr(u,a,2,1); if (p) gp32_st16le(p,v); }
static void bench_write32(void *u, uint32_t a, uint32_t v) { uint8_t *p = bench_ptr(u,a,4,1); if (p) gp32_st32le(p,v); }
static int bench_swi(void *user, arm920t_t *c, uint32_t imm, uint32_t pc, int is_thumb) {
    GP32_UNUSED(user);
    if (imm != 0x7ffeu || pc != expected_swi || is_thumb || c->r[15] != pc + 4u) abort();
    ++swi_calls;
    arm920t_stop_run(c);
    return 1;
}
static uint64_t bench_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}
static uint32_t bench_pass(arm920t_t *c, unsigned pixels) {
    c->r[2] = BENCH_SRC; c->r[3] = BENCH_DST; c->r[10] = pixels; c->r[15] = BENCH_PC;
    c->r[4] = c->r[5] = c->r[6] = c->r[8] = c->r[9] = c->r[11] = c->r[12] = 0u;
    return arm920t_run(c, UINT32_MAX);
}

int main(int argc, char **argv) {
    unsigned mode = argc > 1 ? (unsigned)strtoul(argv[1],NULL,10) : 1u;
    unsigned mmu = argc > 2 ? (unsigned)strtoul(argv[2],NULL,10) : 0u;
    unsigned zeros = argc > 3 ? (unsigned)strtoul(argv[3],NULL,10) : 128u;
    unsigned width = argc > 4 ? (unsigned)strtoul(argv[4],NULL,10) : 2u;
    unsigned forward = argc > 5 ? (unsigned)strtoul(argv[5],NULL,10) : 1u;
    unsigned pixels = argc > 6 ? (unsigned)strtoul(argv[6],NULL,10) : 32768u;
    unsigned passes = argc > 7 ? (unsigned)strtoul(argv[7],NULL,10) : 64u;
    unsigned verify = argc > 8 ? (unsigned)strtoul(argv[8],NULL,10) : 0u;
    if (argc > 9 || mode > 3u || mmu > 1u || zeros > 256u ||
        (width != 1u && width != 2u && width != 4u && width != 8u) || forward > 1u ||
        (!forward && zeros) || pixels < 16u || pixels > 65536u || !passes || passes > 1000000u || verify > 1u)
        return 2;
    const uint32_t opaque_ops[] = {0xe5c37000u, 0xe2866001u, 0xe0288007u, 0xe0899007u,
                                    0xe28bb001u, 0xe02cc007u, 0xe0844007u, 0xe0255007u};
    uint32_t program[15] = {0xe4d27001u, 0xe3977000u,
        forward ? 0x0a000000u | (width - 1u) : 0xe1a00000u};
    memcpy(program + 3u, opaque_ops, width * sizeof(uint32_t));
    program[3u + width] = 0xe2833001u; /* join: ADD dst,dst,#1 */
    program[4u + width] = 0xe25aa001u; /* SUBS pixels,pixels,#1 */
    program[5u + width] = 0x1a000000u | ((0u - (7u + width)) & 0xffffffu);
    program[6u + width] = 0xef007ffeu;
    expected_swi = BENCH_PC + (6u + width) * 4u;
    for (unsigned i = 0; i < BENCH_BIOS_SIZE; i += 4u) gp32_st32le(bench_bios + i, 0xeafffffeu);
    for (unsigned i = 0; i < 7u + width; ++i) gp32_st32le(bench_bios + BENCH_PC + 4u * i, program[i]);
    uint32_t opaque = 0u, sum = 0u, xor_value = 0u;
    for (unsigned i = 0; i < pixels; ++i) {
        uint8_t value = (((i * 37u) ^ (i >> 7)) & 255u) < zeros ? 0u : (uint8_t)((i * 29u + 31u) | 1u);
        bench_ram[BENCH_SRC - BENCH_RAM + i] = value;
        bench_ram[BENCH_DST - BENCH_RAM + i] = 0xa5u;
        if (value) { ++opaque; sum += value; xor_value ^= value; }
    }
    arm_bus_t bus = {.read8=bench_read8,.read16=bench_read16,.read32=bench_read32,
        .write8=bench_write8,.write16=bench_write16,.write32=bench_write32,.fastmem=bench_ptr};
    arm920t_t *c = arm920t_create(&bus);
    if (!c) return 3;
    arm920t_reset(c, BENCH_PC);
    arm920t_set_jit(c, mode == 1u || mode == 3u);
    arm920t_set_trace(c, mode == 2u, NULL, NULL);
    arm920t_set_swi_handler(c, bench_swi, NULL);
    c->cp15[1] = 0x70u | mmu;
    if (mmu) {
        itlb_store(c, BENCH_PC, 0u, 0xfffu);
        tlb_store_checked(c, BENCH_RAM, BENCH_RAM, 0xfffffu, NULL);
    }
    if (mode == 3u) {
        /* Repeatedly discard the decoded/native entry while retaining the
         * arena allocation. This measures collection plus emission, with
         * no guest execution or speculative callback-backed reads. */
        int ok = 1;
        for (unsigned i = 0; i < 2u; ++i) {
            arm_jit_block_t *b = arm_jit_translate(c, BENCH_PC);
            if (!b) { arm920t_destroy(c); return 3; }
            arm_jit_compile_host(c, b);
            ok &= b->native && b->native_ok == 1u;
        }
        uint64_t begin = verify ? 0u : bench_ns();
        for (unsigned i = 0; i < passes; ++i) {
            arm_jit_block_t *b = arm_jit_translate(c, BENCH_PC);
            if (!b) { arm920t_destroy(c); return 3; }
            arm_jit_compile_host(c, b);
            ok &= b->native && b->native_ok == 1u;
        }
        uint64_t elapsed = verify ? 0u : bench_ns() - begin;
        ok &= c->cycles_total == 0u && c->r[15] == BENCH_PC && swi_calls == 0u;
        printf("{\"mode\":3,\"mmu\":%u,\"zeros\":%u,\"width\":%u,\"forward\":%u,"
               "\"pixels\":%u,\"passes\":%u,\"elapsed_ns\":%" PRIu64 ",\"ns_per_unit\":%.6f,"
               "\"unit\":\"compile\",\"retired\":0,\"native_insns\":0,\"native_calls\":0,"
               "\"code_bytes\":%zu,\"checksum\":\"0000000000000000\",\"ok\":%s}\n",
               mmu,zeros,width,forward,pixels,passes,elapsed,(double)elapsed / passes,
               c->jit_code_used,ok ? "true" : "false");
        arm920t_destroy(c);
        return !ok;
    }
    const uint32_t instructions = 6u * pixels + width * opaque + 1u;
    const unsigned warmup = 2u;
    int ok = 1;
    for (unsigned i = 0; i < warmup; ++i) ok &= bench_pass(c, pixels) == instructions;
    arm920t_reset_cpu_profile(c);
    uint64_t retired = 0u, begin = verify ? 0u : bench_ns();
    for (unsigned i = 0; i < passes; ++i) retired += bench_pass(c, pixels);
    uint64_t elapsed = verify ? 0u : bench_ns() - begin;
    ok &= retired == (uint64_t)instructions * passes && swi_calls == warmup + passes;
    ok &= c->r[2] == BENCH_SRC + pixels && c->r[3] == BENCH_DST + pixels && c->r[10] == 0u;
    ok &= c->r[15] == expected_swi + 4u && (c->cpsr & 0xf0000000u) == 0x60000000u;
    ok &= c->r[6] == (width >= 2u ? opaque : 0u);
    ok &= c->r[8] == (width >= 4u ? xor_value : 0u) && c->r[9] == (width >= 4u ? sum : 0u);
    ok &= c->r[11] == (width >= 8u ? opaque : 0u) && c->r[12] == (width >= 8u ? xor_value : 0u);
    ok &= c->r[4] == (width >= 8u ? sum : 0u) && c->r[5] == (width >= 8u ? xor_value : 0u);
    uint64_t checksum = UINT64_C(14695981039346656037);
    for (unsigned i = 0; i < pixels; ++i) {
        uint8_t source = bench_ram[BENCH_SRC - BENCH_RAM + i];
        uint8_t value = bench_ram[BENCH_DST - BENCH_RAM + i];
        ok &= value == (source ? source : 0xa5u);
        checksum = (checksum ^ value) * UINT64_C(1099511628211);
    }
    if (mode == 1u) {
        arm_jit_block_t *b = c->jit_blocks ? &c->jit_blocks[arm_jit_block_index(BENCH_PC)] : NULL;
        ok &= b && b->native && b->native_ok == 1u;
#if ARM920T_PROFILING
        ok &= c->prof.native_arm_insns == retired;
#endif
    }
    printf("{\"mode\":%u,\"mmu\":%u,\"zeros\":%u,\"width\":%u,\"forward\":%u,"
           "\"pixels\":%u,\"passes\":%u,\"elapsed_ns\":%" PRIu64 ",\"ns_per_unit\":%.6f,\"unit\":\"pixel\","
           "\"retired\":%" PRIu64 ",\"native_insns\":%" PRIu64 ",\"native_calls\":%" PRIu64 ","
           "\"code_bytes\":%zu,\"checksum\":\"%016" PRIx64 "\",\"ok\":%s}\n",
           mode,mmu,zeros,width,forward,pixels,passes,elapsed,(double)elapsed / ((double)pixels * passes),
           retired,c->prof.native_arm_insns,c->prof.native_block_calls,c->jit_code_used,checksum,ok ? "true" : "false");
    arm920t_destroy(c);
    return !ok;
}
