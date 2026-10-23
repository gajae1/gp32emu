#ifndef ARM_JIT_CHUNKS_INCLUDED
#define ARM_JIT_CHUNKS_INCLUDED
/* CPU-only placement helpers. The worker never receives an arena pointer.
 * A monotone high-water mark conservatively protects ALL earlier chunks,
 * including failed placements, reset generations and the resident stubs. */
static int arm_jit_chunk_open(arm920t_t *c, size_t chunk, size_t cs, size_t ce) {
    if (c->jit_code_unusable) return 0;
    if (!chunk || cs >= ce || ce > c->jit_code_size || cs % chunk) return 0;
    int opened = cs < c->jit_code_opened;
    if (ce > c->jit_code_opened) c->jit_code_opened = ce;
    if (opened || !cs || c->jit_chunk_disabled || ce - cs != chunk) return 1;
    uint8_t *p = arm_jit_storage_take_chunk(c->jit_storage_worker, chunk);
    if (!p) return 1; /* no wait, and this chunk is now permanently CPU-owned */
    if (mprotect(p, chunk, PROT_READ | PROT_WRITE | PROT_EXEC)) {
        munmap(p, chunk);
        c->jit_chunk_disabled = 1;
        return 1; /* destination has not been touched */
    }
    uint8_t *target = c->jit_code + cs;
    if (mremap(p, chunk, chunk, MREMAP_MAYMOVE | MREMAP_FIXED, target) != MAP_FAILED)
        return 1;
    munmap(p, chunk);
    c->jit_chunk_disabled = 1;
    /* A failed fixed remap may already have removed the virgin destination.
     * Recreate ONLY our owned range; never attempt to repair active code. */
    if (mmap(target, chunk, PROT_READ | PROT_WRITE | PROT_EXEC,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == target) return 1;
    /* Keep surviving native blocks resident but prohibit all later emission,
     * even after a guest reset or state load. Destroy can unmap an arena with
     * holes. No new block may be published into this failed range. */
    c->jit_code_unusable = 1;
    return 0;
}

static void arm_jit_chunk_request(arm920t_t *c, size_t chunk, size_t start, size_t cold) {
    if (c->jit_chunk_disabled || c->jit_code_unusable || !c->jit_storage_worker ||
        !chunk || (chunk & (chunk - 1u))) return;
    size_t cs = start & ~(chunk - 1u);
    if (cs > c->jit_code_size || c->jit_code_size - cs < chunk) return;
    size_t ce = cs + chunk;
    if (c->jit_code_size - ce < chunk || ce < c->jit_code_opened ||
        ce <= c->jit_chunk_requested || cold < cs || cold > ce ||
        c->jit_code_used < cs || c->jit_code_used > cold) return;
    if (c->jit_code_used - cs + ce - cold < chunk / 2u) return;
    c->jit_chunk_requested = ce; /* at most one try per new physical frontier */
    arm_jit_storage_request_chunk(c->jit_storage_worker, chunk);
}
#endif
