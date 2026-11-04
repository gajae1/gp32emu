/* Failed Windows cache publication must not make a native entry callable.
 * The OS flush still runs before its result is deliberately rejected, so a
 * regressed x64 dispatcher cannot execute genuinely unflushed test code.
 * The A64 variant builds the real emitter on this host but NEVER calls its
 * generated code: it checks publication, cleanup and arena ownership only. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned flush_calls, reject_flush_at, rejected_flushes;
static BOOL WINAPI publication_flush(HANDLE process, LPCVOID base, SIZE_T bytes) {
    BOOL result = FlushInstructionCache(process, base, bytes);
    ++flush_calls;
    if (!result) {
        fprintf(stderr, "FAIL: real FlushInstructionCache error %lu\n", GetLastError());
        exit(2);
    }
    if (flush_calls == reject_flush_at) {
        ++rejected_flushes;
        SetLastError(ERROR_GEN_FAILURE);
        return FALSE;
    }
    return result;
}

#ifdef GP32EMU_TEST_A64_EMITTER
/* Disable automatic host-backend selection, then enable only the portable
 * A64 emitter implementation. This is not native A64 execution coverage. */
#define GP32EMU_DISABLE_JIT 1
#define ARM_JIT_NATIVE_A64 1
#endif
#define GP32EMU_CPU_PROFILE 1
#define GP32EMU_ENABLE_THREADS 0
#define ARM_JIT_CODE_SIZE (2u * 512u * 1024u)
#define FlushInstructionCache publication_flush
#include "../src/arm920t.c"
#undef FlushInstructionCache

static unsigned failures;
#define CHECK(x, why) do { if (!(x)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, why); ++failures; \
} } while (0)
#define REQUIRE(x) do { if (!(x)) { \
    fprintf(stderr, "FAIL fixture line %d: %s\n", __LINE__, #x); exit(2); \
} } while (0)

static uint8_t ram[ARM_JIT_RAM_SIZE_BYTES], bios[ARM_JIT_BIOS_SIZE_BYTES];
static uint8_t *map(void *user, uint32_t addr, size_t bytes, int write) {
    (void)user;
    uint32_t off = addr - ARM_JIT_RAM_BASE_ADDR;
    if (off < sizeof(ram) && bytes <= sizeof(ram) - off) return ram + off;
    if (!write && addr < sizeof(bios) && bytes <= sizeof(bios) - addr) return bios + addr;
    return NULL;
}
static uint8_t rd8(void *u, uint32_t a) {
    uint8_t *p = map(u, a, 1u, 0); return p ? *p : 0u;
}
static uint16_t rd16(void *u, uint32_t a) {
    uint8_t *p = map(u, a, 2u, 0); return p ? gp32_ld16le(p) : 0u;
}
static uint32_t rd32(void *u, uint32_t a) {
    uint8_t *p = map(u, a, 4u, 0); return p ? gp32_ld32le(p) : 0u;
}
static void wr8(void *u, uint32_t a, uint8_t v) {
    uint8_t *p = map(u, a, 1u, 1); if (p) *p = v;
}
static void wr16(void *u, uint32_t a, uint16_t v) {
    uint8_t *p = map(u, a, 2u, 1); if (p) gp32_st16le(p, v);
}
static void wr32(void *u, uint32_t a, uint32_t v) {
    uint8_t *p = map(u, a, 4u, 1); if (p) gp32_st32le(p, v);
}
static arm920t_t *new_cpu(void) {
    arm_bus_t bus = {.read8=rd8, .read16=rd16, .read32=rd32,
                    .write8=wr8, .write16=wr16, .write32=wr32, .fastmem=map};
    arm920t_t *c = arm920t_create(&bus);
    REQUIRE(c && arm_jit_alloc(c));
    c->jit_ram_base = c->jit_ram_write_base = ram;
    c->jit_bios_base = bios;
    c->jit_enabled = 1;
    reject_flush_at = 0;
    return c;
}
/* Build ordinary decoded blocks without depending on promotion thresholds.
 * A short block is MOV r0,#42; MOV pc,lr. The large A64 fixture uses 32 real
 * LDRs to obtain resident cold chains whose bytes must survive later failure. */
static arm_jit_block_t *block(arm920t_t *c, unsigned id, unsigned loads) {
    uint32_t pc = ARM_JIT_RAM_BASE_ADDR + 0x100u + id * 0x100u;
    arm_jit_block_t *b = &c->jit_blocks[arm_jit_block_index(pc)];
    REQUIRE(!b->valid && loads + 2u <= ARM_JIT_MAX_INSNS);
    REQUIRE(arm_jit_alloc_ops(c, b));
    *b = (arm_jit_block_t){.tag_pc=pc, .valid=1, .generation=c->jit_generation,
                          .tag_cache_epoch=c->jit_cache_epoch, .count=(uint8_t)(loads + 2u)};
    arm_jit_op_t *ops = arm_jit_ops(c, b);
    for (unsigned i = 0; i < b->count; ++i) {
        uint32_t insn = i < loads ? 0xe5910000u :
                        i == loads ? 0xe3a0002au : 0xe1a0f00eu;
        ops[i] = (arm_jit_op_t){.pc=pc + i * 4u, .insn=insn, .cond=14u,
                               .kind=(uint8_t)arm_jit_classify(insn),
                               .stop=(uint8_t)(i + 1u == b->count)};
        arm_bc_decode_op(&ops[i]);
        gp32_st32le(ram + pc - ARM_JIT_RAM_BASE_ADDR + i * 4u, insn);
    }
    return b;
}

#ifdef GP32EMU_TEST_A64_EMITTER
static void runtime_failure(void) {
    arm920t_t *c = new_cpu();
    arm_jit_block_t *b = block(c, 0u, 0u);
    unsigned before = rejected_flushes;
    reject_flush_at = flush_calls + 1u;
    arm_jit_lazy_compile(c, b);
    CHECK(rejected_flushes == before + 1u, "runtime publication fault was not reached");
    CHECK(!b->native && b->native_ok == 2u, "failed runtime published an A64 entry");
    CHECK(!c->jit_code && !c->jit_code_size && !c->jit_code_used,
          "failed runtime retained an arena without published stubs");
    reject_flush_at = 0;
    arm_jit_block_t *next = block(c, 1u, 0u);
    arm_jit_lazy_compile(c, next);
    CHECK(next->native && next->native_ok == 1u, "fresh runtime could not recover");
    arm920t_destroy(c);
}
static void block_failure(unsigned failing_span) {
    arm920t_t *c = new_cpu();
    arm_jit_block_t *resident = block(c, 0u, 32u);
    arm_jit_lazy_compile(c, resident);
    REQUIRE(resident->native && resident->native_ok == 1u);
    arm_jit_native_fn old_entry = resident->native;
    size_t cold_at = c->jit_cold_floor;
    REQUIRE(cold_at < ARM_JIT_A64_CHUNK && cold_at > c->jit_code_used + 16u);
    size_t cold_bytes = ARM_JIT_A64_CHUNK - cold_at;
    uint8_t *saved = malloc(cold_bytes);
    REQUIRE(saved);
    memcpy(saved, c->jit_code + cold_at, cold_bytes);
    /* Simulate the occupied hot frontier: the next block must move to chunk
     * two. This leaves real resident cold code at the end of chunk one. */
    c->jit_code_used = cold_at - 16u;
    arm_jit_block_t *failed = block(c, 1u, 0u);
    unsigned before = rejected_flushes;
    reject_flush_at = flush_calls + failing_span;
    arm_jit_lazy_compile(c, failed);
    CHECK(rejected_flushes == before + 1u, "hot/cold publication fault was not reached");
    CHECK(!failed->native && failed->native_ok == 2u, "failed block published an A64 entry");
    CHECK(resident->valid && resident->native == old_entry, "failure retired unrelated resident code");
    reject_flush_at = 0;
    arm_jit_block_t *next = block(c, 2u, 0u);
    arm_jit_lazy_compile(c, next);
    CHECK(next->native && next->native_ok == 1u, "post-failure A64 emission did not recover");
    CHECK(!memcmp(saved, c->jit_code + cold_at, cold_bytes),
          "post-failure placement overwrote resident cold code in the previous chunk");
    if (next->native) {
        union { arm_jit_native_fn f; uintptr_t p; } entry = {.f=next->native};
        CHECK(entry.p >= (uintptr_t)c->jit_code + ARM_JIT_A64_CHUNK,
              "post-failure placement returned to an occupied previous chunk");
    }
    printf("A64 span=%u resident_cold_bytes=%zu checked without execution\n", failing_span, cold_bytes);
    free(saved);
    arm920t_destroy(c);
}
int main(void) {
    runtime_failure();
    block_failure(1u);
    block_failure(2u);
    printf("A64 emitter-only: flush_calls=%u rejected=%u failures=%u (no A64 execution)\n",
           flush_calls, rejected_flushes, failures);
    return failures ? 1 : 0;
}
#elif defined(ARM_JIT_NATIVE_X64)
static void run_short(arm920t_t *c, arm_jit_block_t *b) {
    uint64_t cycles = c->cycles_total;
    c->r[0] = 0;
    c->r[14] = ARM_JIT_RAM_BASE_ADDR + 0x3000u;
    c->r[15] = b->tag_pc;
    CHECK(arm920t_run(c, 2u) == 2u, "publication failure changed retirement budget");
    CHECK(c->r[0] == 42u && c->r[15] == ARM_JIT_RAM_BASE_ADDR + 0x3000u,
          "publication fallback changed MOV/return semantics");
    CHECK(c->cycles_total == cycles + 2u, "publication fallback changed guest cycles");
}
int main(void) {
    arm920t_t *c = new_cpu();
    arm_jit_block_t *resident = block(c, 0u, 0u);
    run_short(c, resident);
    REQUIRE(resident->native && resident->native_ok == 1u);
    arm_jit_native_fn old_entry = resident->native;
    uint64_t native_calls = c->prof.native_block_calls;
    arm_jit_block_t *failed = block(c, 1u, 0u);
    reject_flush_at = flush_calls + 1u;
    run_short(c, failed);
    CHECK(rejected_flushes == 1u, "publication fault was not reached");
    CHECK(!failed->native && failed->native_ok == 2u, "failed flush published an x64 entry");
    CHECK(c->prof.native_block_calls == native_calls, "failed publication executed native code");
    unsigned calls_after_failure = flush_calls;
    native_calls = c->prof.native_block_calls;
    run_short(c, failed);
    CHECK(flush_calls == calls_after_failure, "failed block retried compilation on every dispatch");
    CHECK(c->prof.native_block_calls == native_calls, "failed block did not stay portable");
    CHECK(resident->valid && resident->native == old_entry, "failure changed a resident entry");
    reject_flush_at = 0;
    native_calls = c->prof.native_block_calls;
    run_short(c, resident);
    CHECK(c->prof.native_block_calls == native_calls + 1u, "resident entry stopped executing");
    arm_jit_block_t *next = block(c, 2u, 0u);
    native_calls = c->prof.native_block_calls;
    run_short(c, next);
    CHECK(next->native && next->native_ok == 1u, "new x64 block did not recover");
    CHECK(c->prof.native_block_calls == native_calls + 1u, "recovery did not execute native code");
    arm920t_destroy(c);
    printf("Windows/x64 publication: flush_calls=%u rejected=%u failures=%u\n",
           flush_calls, rejected_flushes, failures);
    return failures ? 1 : 0;
}
#else
int main(void) { puts("SKIP: x64 publication execution requires the x64 backend"); return 77; }
#endif
