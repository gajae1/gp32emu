/* AArch64 backend for the decoded ARMv4T blocks in arm920t.c.
 *
 * x19 is the CPU pointer; x0-x17 are scratch (x18 is never touched).  When a
 * block contains fast memory accesses, x20 anchors &c->jit_tlb_page[0] and x21
 * holds c->jit_ram_base for the whole block. Both are callee-saved across
 * helpers, and a helper that changes the RAM base must leave the block.
 * A RAM-page hit loads its tag and VA-to-RAM XOR with one word pair.
 * Guest data registers and CPSR are committed after every instruction; PC is
 * materialized at exits and by the classified helper before callbacks.
 * Calls use AAPCS64 with a 16-byte-aligned stack and a saved frame/LR.
 * Slow instructions use the classified helper and validate its exit state:
 * exceptions, MMIO callbacks, mode changes and cache invalidation therefore
 * cannot continue executing an obsolete trace. No guest cycles are elided.
 * Eligible entry backedges retain w22 (remaining), w23 (completed prefixes),
 * w24 (entry epoch), x25 (CPU control-field base). These are callee-saved.
 *
 * Hot/cold layout: only the RAM fast path is emitted inline. Every sequence
 * the fast path skips (guard handlers, the live-read and identity-I/O probes,
 * the checked helpers) is recorded while the hot body is emitted and replayed
 * into a cold area after the block's epilogue, so no fetched hot line carries
 * cold bytes. Hot guards branch forward into their own cold chain and every
 * chain branches back to the hot continuation it was recorded with.
 */
#ifndef ARM_JIT_CODE_SIZE
#define ARM_JIT_CODE_SIZE (64u * 1024u * 1024u)
#endif
/* Arena chunk: hot bodies grow up from its start, cold chains down from its
 * end, so every hot guard (B.cond/CBZ/CBNZ, +-1 MiB) still reaches its cold
 * chain while consecutive hot bodies share lines instead of cold tails. */
#ifndef ARM_JIT_A64_CHUNK
#define ARM_JIT_A64_CHUNK (512u * 1024u)
#endif

typedef struct a64_emit {
    uint32_t *code;
    size_t pos, cap;
    int fail;
    int ram_read, ram_write, memory, mmu, leaf;
    int bios_read;   /* jit_bios_base probes the whole read-only BIOS window */
    int open_bus;    /* bus promises open_bus_word32 outside every window */
    int nzcv_valid; /* host NZCV equals the committed guest flags */
    int fwd_guest;  /* guest register whose committed value is still in w2, -1 = none.
                     * Only a plain register data op leaves it set; every other
                     * emitter and any helper/cold path clears it, because w2 is
                     * caller-saved across helpers and reused as scratch. */
    uint32_t expected_next, generation;
    uint32_t loop_done, block_count;
    size_t loop_head;
    const arm_live_read32_t *live_read32;
    size_t live_read32_count;
    struct a64_cold_op *cold; /* deferred cold chains, one per fast-path op */
    unsigned cold_count, cold_cap;
    int in_cold;              /* emitting a cold chain: never share the hot epilogue */
    size_t ret_fixup[ARM_JIT_MAX_INSNS + 4]; /* hot exit sites of the shared epilogue */
    unsigned ret_count;
    size_t entry_exit;        /* hot budget-check branch into the cold entry exit */
    size_t hot_end;           /* words [0, hot_end) are the hot body and epilogue */
} a64_emit_t;

enum a64_cold_kind {
    A64_COLD_SINGLE_DT = 1, A64_COLD_HALF, A64_COLD_BLOCK_DT,
    /* The block-transfer two-page retry. It is entered from the hot page-cross
     * branch, re-runs the translation and the transfer for a span that crosses
     * one 4 KiB boundary, and resumes the hot continuation behind it. Its own
     * guard rejections join the sibling classified-helper chain. */
    A64_COLD_BLOCK_DT_XPAGE
};

/* One deferred cold chain.  The hot body records what to emit and where the
 * guards sit; the chain is written after the block body and ends by branching
 * back to cont.  Nothing here changes what the chain executes: the op fields,
 * the continuation and the helper arguments are the same values the inline
 * emitters used, and the guard branches still leave the registers exactly as
 * the checked helper expects them. */
typedef struct a64_cold_op {
    const arm_jit_op_t *op;
    size_t cont; /* hot continuation label this chain resumes at */
    uint32_t done, expected_next, generation;
    uint8_t kind, load, wb, access, byte, live, ro, nfixup;
    size_t fixup[16]; /* hot branch sites that enter this chain */
    unsigned sibling; /* chain that owns this chain's guard rejections (0 = none) */
} a64_cold_op_t;

ARM_FORCE_INLINE int arm_a64_can_continue(const arm920t_t *c, int stop,
                                         uint32_t expected_next, uint32_t generation,
                                         uint32_t status, uint32_t epoch, uint32_t run_limit,
                                         const uint8_t *ram_base) {
    /* All native bus/SWI callbacks pass these checked helpers. A reduced
     * deadline leaves after this instruction, before any native backedge can
     * reuse w22's old budget. Dispatch resumes with the new remaining budget;
     * callback-free chains retain their existing fast guards. */
    return !stop && c->run_limit == run_limit && !c->halted && !thumb(c) && !c->trace && c->jit_enabled &&
           c->jit_generation == generation && c->jit_cache_epoch == epoch &&
           c->jit_ram_base == ram_base &&
           (c->cpsr & 0xffu) == status &&
           (c->r[15] & ~3u) == expected_next &&
           !(c->irq_line && !(c->cpsr & I_FLAG)) &&
           !(c->fiq_line && !(c->cpsr & F_FLAG));
}

static int arm_a64_exec_checked(arm920t_t *c, const arm_jit_op_t *op,
                                uint32_t expected_next, uint32_t generation) {
    int stop = op->stop;
    uint32_t status = c->cpsr & 0xffu, epoch = c->jit_cache_epoch, run_limit = c->run_limit;
    uint8_t *ram_base = c->jit_ram_base;
    arm_jit_exec_classified(c, op);
    return arm_a64_can_continue(c, stop, expected_next, generation, status, epoch, run_limit, ram_base);
}

/* Only used for a terminating MSR. No callback can observe an intermediate
 * state: the emitter commits PC and returns immediately after this call.
 * Keep bank switching in the shared implementation, but the field mask and
 * operand have already been decoded by the emitter. */
static void arm_a64_msr_cpsr(arm920t_t *c, uint32_t value, uint32_t mask) {
    ARM_PROF_INC(c, helper_interp_ops);
    ARM_PROF_INC(c, helper_op_kinds[ARM_JIT_OP_PSR]);
    ARM_PROF_INC(c, slow_bail_other);
    if (mode(c) == MODE_USR) mask &= 0xff000000u;
    set_cpsr_full(c, (c->cpsr & ~mask) | (value & mask));
}

static int arm_a64_mem_checked(arm920t_t *c, const arm_jit_op_t *op,
                               uint32_t addr, uint32_t writeback, uint32_t value,
                               uint32_t expected_next, uint32_t generation) {
    int stop = op->stop;
    uint32_t status = c->cpsr & 0xffu, epoch = c->jit_cache_epoch, run_limit = c->run_limit;
    uint8_t *ram_base = c->jit_ram_base;
    c->r[15] = op->pc + 4u; /* callback-visible PC, exactly as classified */
#if ARM920T_PROFILING
    /* Access-only helpers bypass arm_jit_exec_classified; retain the same
     * read-only pre-access attribution without counting an interpreted op. */
    arm_profile_helper_reason(c, op);
#endif
    arm_jit_mem_access(c, op, addr, writeback, value);
    return arm_a64_can_continue(c, stop, expected_next, generation, status, epoch, run_limit, ram_base);
}

/* The emitter already knows this is a word load without writeback. Avoid
 * decoding the transfer kind/size/writeback again on every MMIO access.
 * Retain translation, unaligned rotation and the complete callback guard. */
static int arm_a64_ldword_checked(arm920t_t *c, const arm_jit_op_t *op,
                                  uint32_t addr, uint32_t expected_next,
                                  uint32_t generation) {
    int stop = op->stop;
    unsigned rd = op->b; /* callbacks can invalidate decoded-op storage */
    uint32_t status = c->cpsr & 0xffu, epoch = c->jit_cache_epoch, run_limit = c->run_limit;
    uint8_t *ram_base = c->jit_ram_base;
    c->r[15] = op->pc + 4u;
#if ARM920T_PROFILING
    arm_profile_helper_reason(c, op);
#endif
    ARM_PROF_INC(c, helper_ld_word);
    uint32_t value = arm_bc_ld_word(c, addr & ~3u);
    if (addr & 3u) value = gp32_ror32(value, (addr & 3u) * 8u);
    write_r(c, rd, value);
    return arm_a64_can_continue(c, stop, expected_next, generation, status, epoch, run_limit, ram_base);
}

/* Direct word access into the identity-mapped device window, entered only
 * after the emitted RAM guard rejected the address and the emitted probe
 * proved the current TLB entry and the physical 0x14000000..0x16000000
 * window the checked helpers route straight to the bus callbacks.  It repeats
 * the observable steps of arm_a64_mem_checked/arm_jit_mem_access - the
 * callback-visible PC, one bus access, destination then writeback, then the
 * same continuation snapshot - while skipping the second mmu_translate, the
 * transfer decode and the physical-window probes that the emitter already
 * performed.  Cycle and timing state stay untouched exactly as there, and an
 * I/O write keeps the identical exit condition (interrupts, timers, halt,
 * trace, epoch/generation, RAM rebase).  Address and value are handed over
 * already computed: pa is 4-byte aligned and inside that window, rotation is
 * (VA & 3) * 8, writeback is base +/- offset (used only when the decoded
 * shape writes back).  No profiling counter is advanced: like the RAM
 * fast path this is a native fast path, not a checked-helper invocation. */
static int arm_a64_io_word_checked(arm920t_t *c, const arm_jit_op_t *op,
                                   uint32_t pa, uint32_t value, uint32_t writeback,
                                   uint32_t rotation, uint32_t expected_next,
                                   uint32_t generation) {
    int stop = op->stop;
    unsigned rd = op->b, rn = op->a; /* callbacks can invalidate decoded-op storage */
    int load = !!(op->d & ARM_BC_SD_L);
    int wb = !(op->d & ARM_BC_SD_P) || (op->d & ARM_BC_SD_W);
    uint32_t status = c->cpsr & 0xffu, epoch = c->jit_cache_epoch, run_limit = c->run_limit;
    uint8_t *ram_base = c->jit_ram_base;
    c->r[15] = op->pc + 4u; /* callback-visible PC, exactly as classified */
    if (load) {
        /* Same callback selection as arm_bc_ld_word_phys' identity-I/O arm. */
        uint32_t v = (c->bus.read32_io ? c->bus.read32_io : c->bus.read32)(c->bus.user, pa);
        if (rotation) v = gp32_ror32(v, rotation);
        write_r(c, rd, v);
    } else {
        /* Same callback selection as arm_bc_st_word_phys' identity-I/O arm. */
        (c->bus.write32_io ? c->bus.write32_io : c->bus.write32)(c->bus.user, pa, value);
    }
    if (wb) write_r(c, rn, writeback);
    return arm_a64_can_continue(c, stop, expected_next, generation, status, epoch, run_limit, ram_base);
}

static void a64_u32(a64_emit_t *e, uint32_t v) {
    if (e->pos < e->cap) e->code[e->pos++] = v;
    else e->fail = 1;
}

static void a64_imm(a64_emit_t *e, unsigned rd, uint32_t v) {
    if (!(v & 0xffffu))
        a64_u32(e, 0x52a00000u | ((v >> 16) << 5) | rd); /* movz W, lsl #16 */
    else {
        a64_u32(e, 0x52800000u | ((v & 0xffffu) << 5) | rd); /* movz W */
        if (v >> 16) a64_u32(e, 0x72a00000u | ((v >> 16) << 5) | rd);
    }
}

static void a64_ptr(a64_emit_t *e, unsigned rd, uintptr_t v) {
    a64_u32(e, 0xd2800000u | (((uint32_t)v & 0xffffu) << 5) | rd);
    for (unsigned i = 1; i < 4; ++i)
        if ((v >> (16u * i)) & 0xffffu)
            a64_u32(e, 0xf2800000u | (i << 21) |
                    ((uint32_t)((v >> (16u * i)) & 0xffffu) << 5) | rd);
}

static void a64_rr(a64_emit_t *e, uint32_t opcode, unsigned rd, unsigned rn, unsigned rm) {
    a64_u32(e, opcode | (rm << 16) | (rn << 5) | rd);
}

static void a64_mov(a64_emit_t *e, unsigned rd, unsigned rn) {
    a64_rr(e, 0x2a000000u, rd, 31, rn); /* orr Wd, wzr, Wn */
}

/* Materialize x19 + off in rd. A shifted ADD reaches any 4 KiB-aligned
 * offset below 16 MiB, so the large control-field offsets that follow the TLB
 * arrays cost one or two instructions instead of a movz/movk/add triple. */
static void a64_cpu_addr(a64_emit_t *e, unsigned rd, size_t off) {
    if (off <= 0xfffu) {
        a64_u32(e, 0x91000000u | ((uint32_t)off << 10) | (19u << 5) | rd);
    } else if ((off >> 12) <= 0xfffu) {
        a64_u32(e, 0x91400000u | ((uint32_t)(off >> 12) << 10) | (19u << 5) | rd);
        if (off & 0xfffu)
            a64_u32(e, 0x91000000u | ((uint32_t)(off & 0xfffu) << 10) | (rd << 5) | rd);
    } else {
        a64_imm(e, rd, (uint32_t)off);
        a64_rr(e, 0x8b000000u, rd, 19, rd);
    }
}

static void a64_cpu_mem(a64_emit_t *e, uint32_t opcode, unsigned rt, size_t off, unsigned scale) {
    unsigned rn = 19;
    const size_t control = offsetof(arm920t_t, irq_line);
    if (e->loop_done && off >= control &&
        !((off - control) & ((1u << scale) - 1u)) && ((off - control) >> scale) <= 4095u) {
        /* These fields follow the large TLB arrays. x25 is initialized once
         * before the loop, avoiding repeated address materialization while
         * still loading each current IRQ/cache/control value at every guard. */
        rn = 25;
        off -= control;
    } else if ((off & ((1u << scale) - 1u)) || (off >> scale) > 4095u) {
        /* Anchor one 4 KiB-aligned ADD below the field so the access keeps its
         * own scaled immediate: add x17, x19, #hi, lsl #12 then ldr/str [x17, #lo].
         * The reachable 16 MiB window covers every arm920t_t field. */
        size_t hi = off >> 12, lo = off & 0xfffu;
        if (hi <= 0xfffu && !(lo & ((1u << scale) - 1u)) && (lo >> scale) <= 4095u) {
            a64_u32(e, 0x91400000u | ((uint32_t)hi << 10) | (19u << 5) | 17u);
            rn = 17;
            off = lo;
        } else {
            a64_cpu_addr(e, 17, off);
            rn = 17;
            off = 0;
        }
    }
    a64_u32(e, opcode | ((uint32_t)(off >> scale) << 10) | (rn << 5) | rt);
}

static void a64_load(a64_emit_t *e, unsigned rt, size_t off) {
    a64_cpu_mem(e, 0xb9400000u, rt, off, 2);
}
static void a64_store(a64_emit_t *e, unsigned rt, size_t off) {
    a64_cpu_mem(e, 0xb9000000u, rt, off, 2);
}
static void a64_store_imm(a64_emit_t *e, size_t off, uint32_t v) {
    a64_imm(e, 16, v);
    a64_store(e, 16, off);
}
static void a64_guest(a64_emit_t *e, unsigned rt, unsigned r, uint32_t pc) {
    if (r == 15u) a64_imm(e, rt, pc + 8u);
    else a64_load(e, rt, offsetof(arm920t_t, r) + r * 4u);
}
static void a64_put_guest(a64_emit_t *e, unsigned r, unsigned rt) {
    a64_store(e, rt, offsetof(arm920t_t, r) + r * 4u);
}

/* Return packed immr/imms fields only; leave *encoding unchanged on failure. */
static int a64_logical_imm32(uint32_t mask, uint32_t *encoding) {
    if (!mask || mask == UINT32_MAX) return 0;
    /* W-form logical immediates repeat a rotated run of 1..width-1 ones
     * in a 2/4/8/16/32-bit element. N stays zero for the 32-bit form. */
    for (unsigned width = 2u; width <= 32u; width <<= 1) {
        uint32_t element_mask = UINT32_MAX >> (32u - width);
        uint32_t element = mask & element_mask, repeated = element;
        for (unsigned bit = width; bit < 32u; bit += width)
            repeated |= element << bit;
        if (repeated != mask) continue;
        /* A circular run has exactly one 0->1 transition. Locate that edge
         * directly: searching rotations is costly when games flush JIT code
         * frequently. Neither ctz input can be zero for a nonuniform run. */
        uint32_t previous = ((element << 1) | (element >> (width - 1u))) & element_mask;
        uint32_t starts = element & ~previous;
        if (!starts || (starts & (starts - 1u))) continue;
        unsigned first = 0;
#if defined(__GNUC__) || defined(__clang__)
        first = (unsigned)__builtin_ctz(starts);
#else
        for (uint32_t bits = starts; !(bits & 1u); bits >>= 1) ++first;
#endif
        uint32_t run = first ? ((element >> first) |
            (element << (width - first))) & element_mask : element;
        unsigned ones = 0;
#if defined(__GNUC__) || defined(__clang__)
        ones = (unsigned)__builtin_ctz(~run);
#else
        for (uint32_t bits = run; bits & 1u; bits >>= 1) ++ones;
#endif
        unsigned rotate = (width - first) & (width - 1u);
        uint32_t imms = ((0u - (width << 1)) | (ones - 1u)) & 63u;
        *encoding = (rotate << 16) | (imms << 10);
        return 1;
    }
    return 0;
}

static void a64_mask(a64_emit_t *e, unsigned rd, unsigned rn, uint32_t mask) {
    if (!mask) { a64_mov(e, rd, 31); return; }
    if (mask == UINT32_MAX) { a64_mov(e, rd, rn); return; }
    uint32_t encoding;
    if (a64_logical_imm32(mask, &encoding)) {
        a64_u32(e, 0x12000000u | encoding | (rn << 5) | rd); /* and W: NZCV intact */
        return;
    }
    a64_imm(e, 16, mask);
    a64_rr(e, 0x0a000000u, rd, rn, 16);
}
static void a64_ubfx(a64_emit_t *e, unsigned rd, unsigned rn, unsigned lsb, unsigned width) {
    a64_u32(e, 0x53000000u | (lsb << 16) | ((lsb + width - 1u) << 10) | (rn << 5) | rd);
}
/* BFXIL Wd, Wn, #0, #width: preserve the new upper flags in Wd while
 * replacing its low bits with the old CPSR in Wn. BFXIL always inserts at
 * destination bit zero; a nonzero source lsb would not splice upper flags. */
static void a64_bfxil_low(a64_emit_t *e, unsigned rd, unsigned rn, unsigned width) {
    a64_u32(e, 0x33000000u | ((width - 1u) << 10) | (rn << 5) | rd);
}
static void a64_lsl(a64_emit_t *e, unsigned rd, unsigned rn, unsigned amount) {
    a64_u32(e, 0x53000000u | (((32u - amount) & 31u) << 16) |
            ((31u - amount) << 10) | (rn << 5) | rd);
}

/* All fixups are forward within the temporary block (word indices).  A cold
 * chain returns to its hot continuation, which sits behind it, so that edge is
 * emitted directly by a64_branch_back instead of being patched. */
static size_t a64_jump(a64_emit_t *e, unsigned cond) {
    size_t p = e->pos;
    a64_u32(e, cond == 14u ? 0x14000000u : (0x54000000u | cond));
    return p;
}
static void a64_patch(a64_emit_t *e, size_t at, size_t target) {
    if (e->fail || at >= e->pos || target < at || target - at >= (1u << 18)) {
        e->fail = 1;
        return;
    }
    uint32_t delta = (uint32_t)(target - at);
    if ((e->code[at] & 0xfc000000u) == 0x14000000u) e->code[at] |= delta;
    else if ((e->code[at] & ~31u) == 0x36000000u) {
        if (delta >= (1u << 13)) { e->fail = 1; return; }
        e->code[at] |= delta << 5; /* tbz: imm14 */
    }
    else e->code[at] |= delta << 5;
}
/* Emit an unconditional backward branch to an already emitted position. */
static void a64_branch_back(a64_emit_t *e, size_t target) {
    if (e->fail || target > e->pos || e->pos - target >= (1u << 25)) { e->fail = 1; return; }
    a64_u32(e, 0x14000000u | ((uint32_t)(target - e->pos) & 0x03ffffffu));
}
/* Record one cold chain; the hot body has already reached its continuation. */
static a64_cold_op_t *a64_cold_open(a64_emit_t *e, const arm_jit_op_t *op,
                                    uint32_t done, unsigned kind) {
    if (e->fail || e->cold_count >= e->cold_cap) { e->fail = 1; return NULL; }
    a64_cold_op_t *c = &e->cold[e->cold_count++];
    memset(c, 0, sizeof(*c));
    c->op = op;
    c->cont = e->pos;
    c->done = done;
    c->expected_next = e->expected_next;
    c->generation = e->generation;
    c->kind = (uint8_t)kind;
    return c;
}
/* Defer one hot branch site to its chain entry (patched when the chain is
 * emitted).  Overflow is unreachable with the per-emitter fixup counts and
 * fails closed if a future emitter ever exceeds them. */
static void a64_cold_fix(a64_emit_t *e, a64_cold_op_t *c, size_t at) {
    if (e->fail || !c) return;
    if (c->nfixup >= sizeof(c->fixup) / sizeof(c->fixup[0])) { e->fail = 1; return; }
    c->fixup[c->nfixup++] = at;
}
static void a64_cmp(a64_emit_t *e, unsigned rn, unsigned rm) {
    a64_rr(e, 0x6b000000u, 31, rn, rm);
}
static void a64_cmp_imm(a64_emit_t *e, unsigned rn, unsigned imm) {
    a64_u32(e, 0x7100001fu | (imm << 10) | (rn << 5));
}
/* Materialize the completed-op count the dispatcher expects in w0. */
static void a64_ret_done(a64_emit_t *e, uint32_t done) {
    if (e->loop_done)
        a64_u32(e, 0x11000000u | (done << 10) | (23u << 5)); /* add w0, w23, #done */
    else a64_imm(e, 0, done);
}

/* Restore the host frame and return to dispatch. */
static void a64_ret_tail(a64_emit_t *e) {
    if (e->leaf) {
        a64_u32(e, 0xf84107f3u); /* ldr x19, [sp], #16 */
    } else if (e->memory) {
        a64_u32(e, 0xa9417bf5u); /* ldp x21, x30, [sp, #16] */
        a64_u32(e, 0xa8c253f3u); /* ldp x19, x20, [sp], #32 */
    } else a64_u32(e, 0xa8c17bf3u); /* ldp x19, x30, [sp], #16 */
    if (e->loop_done) {
        a64_u32(e, 0xa94167f8u); /* ldp x24, x25, [sp, #16] */
        a64_u32(e, 0xa8c25ff6u); /* ldp x22, x23, [sp], #32 */
    }
    a64_u32(e, 0xd65f03c0u); /* ret */
}

/* Leave the block.  A hot exit only materializes w0 and branches to the one
 * shared epilogue that a64_ret_flush emits at the end of the hot body, so the
 * frame restore and ret exist once per block instead of once per exit.  Cold
 * chains emit the whole sequence inline: they cannot branch back into the hot
 * body's tail.  Every exit still passes the same completed-op count, and the
 * fall-through exit reaches the same epilogue. */
static void a64_return(a64_emit_t *e, uint32_t done) {
    a64_ret_done(e, done);
    if (e->in_cold) { a64_ret_tail(e); return; }
    if (e->ret_count >= sizeof(e->ret_fixup) / sizeof(e->ret_fixup[0])) { e->fail = 1; return; }
    e->ret_fixup[e->ret_count++] = a64_jump(e, 14);
}

/* The shared hot epilogue: the fall-through exit falls into it and every hot
 * exit branch is patched to its first instruction. */
static void a64_ret_flush(a64_emit_t *e, uint32_t done) {
    a64_ret_done(e, done);
    size_t target = e->pos;
    for (unsigned i = 0; i < e->ret_count; ++i) a64_patch(e, e->ret_fixup[i], target);
    a64_ret_tail(e);
}

/* Only the first explicitly eligible entry backedge chains. A full decoded
 * block must fit again; a short remainder returns to portable dispatch.
 * Guards run after the real B retires and before accounting/re-entry. Thus
 * every rejected edge returns completed prefixes + this partial prefix. */
static void a64_selfloop(a64_emit_t *e, uint32_t done) {
    size_t exits[5];
    unsigned n = 0;
    /* Generation and epoch are two adjacent words, read as one 64-bit pair
     * and compared against the entry snapshot in x24. The dispatcher only
     * enters a block whose generation still matches the one its code was
     * built for, so the entry-time pair is exactly the {compiled generation,
     * entry epoch} the two separate compares used: one load, one compare. */
    _Static_assert(offsetof(arm920t_t, jit_cache_epoch) ==
                   offsetof(arm920t_t, jit_generation) + 4u &&
                   ((offsetof(arm920t_t, jit_generation) -
                     offsetof(arm920t_t, irq_line)) & 7u) == 0u,
                   "AArch64 generation/epoch fence pair layout");
    a64_cpu_mem(e, 0xf9400000u, 0, offsetof(arm920t_t, jit_generation), 3);
    a64_rr(e, 0xeb000000u, 31, 0, 24); /* cmp x0, x24 */
    exits[n++] = a64_jump(e, 1); /* NE */
    /* Allowed direct ops cannot change control state or RAM anchoring.
     * Every callback runs through the checked helper above, which exits on
     * halt/trace/JIT disable, mode/base/PC changes or pending interrupts.
     * Retain edge cache and interrupt fences; avoid rechecking the same
     * control state on every callback-free repetition. */
    _Static_assert(sizeof(((arm920t_t *)0)->irq_line) == 4u &&
                   sizeof(((arm920t_t *)0)->fiq_line) == 4u &&
                   offsetof(arm920t_t, fiq_line) == offsetof(arm920t_t, irq_line) + 4u,
                   "AArch64 interrupt pair layout");
    /* Both inactive is the common case. Test the complete pair, including
     * noncanonical nonzero values restored from a state image. Pending lines
     * still take the original independent CPSR-mask checks below. */
    a64_cpu_mem(e, 0xf9400000u, 0, offsetof(arm920t_t, irq_line), 3);
    size_t no_pending = e->pos;
    a64_u32(e, 0xb4000000u); /* cbz x0: both 32-bit lines are zero */
    a64_load(e, 3, offsetof(arm920t_t, cpsr));
    a64_load(e, 0, offsetof(arm920t_t, irq_line));
    a64_cmp_imm(e, 0, 0);
    size_t no_irq = a64_jump(e, 0);
    a64_mask(e, 0, 3, I_FLAG);
    a64_cmp_imm(e, 0, 0);
    exits[n++] = a64_jump(e, 0);
    a64_patch(e, no_irq, e->pos);
    a64_load(e, 0, offsetof(arm920t_t, fiq_line));
    a64_cmp_imm(e, 0, 0);
    size_t no_fiq = a64_jump(e, 0);
    a64_mask(e, 0, 3, F_FLAG);
    a64_cmp_imm(e, 0, 0);
    exits[n++] = a64_jump(e, 0);
    a64_patch(e, no_fiq, e->pos);
    a64_patch(e, no_pending, e->pos);
    /* Keep the remaining budget in w22 itself: every exit restores the host
     * frame through sp and reports the completed prefix in w0, so the extra
     * copy the old "sub w0, w22, #done; ...; mov w22, w0" order needed is one
     * word of the repeating body. The comparison is the same unsigned
     * remaining-budget test against the full block length. */
    a64_u32(e, 0x51000000u | (done << 10) | (22u << 5) | 22u); /* sub w22, w22, #done */
    a64_cmp_imm(e, 22, e->block_count);
    exits[n++] = a64_jump(e, 3); /* LO */
    a64_u32(e, 0x11000000u | (done << 10) | (23u << 5) | 23u);
    /* Dedicated signed imm26 backedge; existing forward fixups stay intact. */
    if (e->loop_head > e->pos || e->pos - e->loop_head >= (1u << 25)) e->fail = 1;
    a64_u32(e, 0x14000000u | ((uint32_t)(e->loop_head - e->pos) & 0x03ffffffu));
    for (unsigned i = 0; i < n; ++i) a64_patch(e, exits[i], e->pos);
    a64_return(e, done);
}
static void a64_slow(a64_emit_t *e, const arm_jit_op_t *op, uint32_t done) {
    /* Fail closed if the leaf allowlist ever disagrees with an emitter. */
    if (e->leaf) { e->fail = 1; return; }
    a64_rr(e, 0xaa000000u, 0, 31, 19);
    a64_ptr(e, 1, (uintptr_t)op);
    if (op->stop || op->kind == ARM_JIT_OP_SWI || op->kind == ARM_JIT_OP_UNDEFINED ||
        (op->kind == ARM_JIT_OP_COPROC && op->reserved != 6u)) {
        /* This path always returns to dispatch. The classified operation
         * commits callback/exception state; continuation snapshots and checks
         * cannot affect the decision to leave this block. */
        a64_ptr(e, 16, (uintptr_t)arm_jit_exec_classified);
        a64_u32(e, 0xd63f0200u);
        a64_return(e, done);
        return;
    }
    a64_imm(e, 2, e->expected_next);
    a64_imm(e, 3, e->generation);
    a64_ptr(e, 16, (uintptr_t)arm_a64_exec_checked);
    a64_u32(e, 0xd63f0200u); /* blr x16; helper commits PC and CPSR */
    a64_cmp_imm(e, 0, 0);
    size_t next = a64_jump(e, 1);
    a64_return(e, done);
    a64_patch(e, next, e->pos);
}

/* w12 retains the original VA before alignment/TLB/range guards; w2 retains
 * writeback. Neither is clobbered by a64_ram_offset. The access helper owns
 * the load/store and commits; no guest state is changed before this call. */
static void a64_mem_slow(a64_emit_t *e, const arm_jit_op_t *op, uint32_t done,
                          int load, int wb) {
    if (e->leaf) { e->fail = 1; return; }
    int word_load = load && !wb && op->kind == ARM_JIT_OP_SINGLE_DT && !(op->d & ARM_BC_SD_B);
    if (word_load) {
        a64_imm(e, 3, e->expected_next);
        a64_imm(e, 4, e->generation);
    } else {
        if (wb) a64_mov(e, 3, 2);
        else a64_imm(e, 3, 0u);
        if (load) a64_imm(e, 4, 0u);
        else a64_guest(e, 4, op->b, op->pc);
        a64_imm(e, 5, e->expected_next);
        a64_imm(e, 6, e->generation);
    }
    a64_mov(e, 2, 12);
    a64_rr(e, 0xaa000000u, 0, 31, 19);
    a64_ptr(e, 1, (uintptr_t)op);
    a64_ptr(e, 16, word_load ? (uintptr_t)arm_a64_ldword_checked : (uintptr_t)arm_a64_mem_checked);
    a64_u32(e, 0xd63f0200u);
    a64_cmp_imm(e, 0, 0);
    size_t next = a64_jump(e, 1);
    a64_return(e, done);
    a64_patch(e, next, e->pos);
}

/* Direct word transfer into the identity-mapped device window, emitted after
 * the RAM guard rejected the address.  w12 still holds the unaligned VA, w2 the
 * base +/- offset the shape would write back and w11 the ARM unaligned word
 * rotation amount.  Nothing is committed before the proof: the mapping must be
 * the current full-mask TLB entry (MMU on) or the identity mapping (MMU off),
 * and the aligned physical word must lie inside the device window
 * 0x14000000..0x15ffffff, whose top seven address bits are 0b0001010.  The
 * checked helper still owns
 * every miss, tiny or unusual page, fault-tolerant identity fallback, byte
 * access and any other target.  On success arm_a64_io_word_checked performs the
 * bus callback with this physical address plus the same register commits and
 * continuation check; the proven access resumes the recorded hot continuation
 * (cont), which is behind this cold chain. */
static void a64_io_word(a64_emit_t *e, const arm_jit_op_t *op, uint32_t done,
                        int load, int wb, size_t *cold, unsigned *ncold, size_t cont) {
    a64_mask(e, 3, 12, ~3u);  /* the checked path translates the aligned word */
    if (e->mmu) {
        _Static_assert(sizeof(arm_tlb_entry_t) == 16u &&
                       offsetof(arm_tlb_entry_t, pa_base) == 4u &&
                       offsetof(arm_tlb_entry_t, mask) == 8u &&
                       offsetof(arm_tlb_entry_t, valid) == 12u,
                       "direct I/O packed TLB layout");
        /* Read the authoritative TLB; never fill it or widen a restored mask. */
        a64_ubfx(e, 6, 3, 12, 12);
        a64_cpu_addr(e, 8, offsetof(arm920t_t, tlb_entry));
        a64_rr(e, 0x8b001000u, 8, 8, 6); /* add x8, x8, x6, lsl #4 */
        a64_u32(e, 0x29400000u | (9u << 10) | (8u << 5) | 7u); /* ldp w7, w9, [x8]: VA, PA */
        a64_u32(e, 0x29410000u | (6u << 10) | (8u << 5) | 5u); /* ldp w5, w6, [x8, #8]: mask, valid */
        a64_u32(e, 0x34000000u | 6u); /* cbz w6, cold */
        cold[(*ncold)++] = e->pos - 1u;
        a64_rr(e, 0x0a200000u, 6, 3, 5); /* bic w6, w3, w5: outside the mapped page? */
        a64_cmp(e, 6, 7);
        cold[(*ncold)++] = a64_jump(e, 1); /* NE */
        a64_rr(e, 0x0a000000u, 6, 3, 5); /* and w6, w3, w5 */
        a64_rr(e, 0x2a000000u, 0, 6, 9); /* orr w0, w6, w9: physical word address */
    } else a64_mov(e, 0, 3);
    /* 0x14000000..0x15ffffff is exactly bit 31..25 == 0b0001010 == 0xA; the
     * address is already word aligned, so the last word 0x15fffffc is the
     * window's inclusive end and 0x16000000 is already outside. */
    a64_ubfx(e, 6, 0, 25, 7);
    a64_cmp_imm(e, 6, 0x0au);
    /* Every rejection still leaves w12 (the original VA) and w2 (base +/- the
     * decoded offset) exactly as the checked helper expects them. */
    cold[(*ncold)++] = a64_jump(e, 1); /* NE: not identity I/O */
    if (wb) a64_mov(e, 4, 2);             /* writeback value for this helper */
    a64_mov(e, 2, 0);                     /* proven physical address */
    if (load) a64_mov(e, 5, 11);          /* unaligned rotation the helper applies */
    else a64_guest(e, 3, op->b, op->pc);  /* store source, read before the callback */
    a64_imm(e, 6, e->expected_next);
    a64_imm(e, 7, e->generation);
    a64_rr(e, 0xaa000000u, 0, 31, 19);
    a64_ptr(e, 1, (uintptr_t)op);
    a64_ptr(e, 16, (uintptr_t)arm_a64_io_word_checked);
    a64_u32(e, 0xd63f0200u); /* blr x16 */
    size_t keep = e->pos;
    a64_u32(e, 0x35000000u); /* cbnz w0: commits are done, may continue */
    a64_return(e, done);
    a64_patch(e, keep, e->pos);
    a64_branch_back(e, cont);
}

/* Immediate ARM shifter: w1 is the value, w3 the old CPSR, w4 the
 * resulting carry at bit 29 when requested. Register shifts use the separate
 * a64_regshift_op2 path to retain ARMv4's amount >=32 semantics. */
static void a64_shift(a64_emit_t *e, unsigned type, unsigned amount, int carry) {
    if (carry) {
        if (type == 0u && !amount) a64_mask(e, 4, 3, C_FLAG);
        else {
            unsigned bit = type == 0u ? 32u - amount : amount ? amount - 1u : type == 3u ? 0u : 31u;
            a64_ubfx(e, 4, 1, bit, 1);
            a64_lsl(e, 4, 4, 29);
        }
    }
    switch (type) {
    case 0:
        if (amount) a64_lsl(e, 1, 1, amount);
        break;
    case 1:
        if (amount) a64_ubfx(e, 1, 1, amount, 32u - amount);
        else a64_mov(e, 1, 31);
        break;
    case 2:
        a64_u32(e, 0x13007c21u | ((amount ? amount : 31u) << 16)); /* asr w1 */
        break;
    default:
        if (amount) a64_u32(e, 0x13810021u | (amount << 10)); /* ror w1 */
        else {
            a64_ubfx(e, 5, 3, 29, 1);
            a64_lsl(e, 5, 5, 31);
            a64_ubfx(e, 1, 1, 1, 31);
            a64_rr(e, 0x2a000000u, 1, 1, 5);
        }
        break;
    }
}

static void a64_exception_return(a64_emit_t *e, unsigned result, uint32_t done) {
    a64_mov(e, 1, result); /* unaligned result, from the old register bank */
    a64_rr(e, 0xaa000000u, 0, 31, 19); /* x0 = CPU */
    a64_ptr(e, 16, (uintptr_t)arm_jit_exception_return_helper);
    a64_u32(e, 0xd63f0200u); /* blr x16 */
    e->nzcv_valid = 0;
    a64_return(e, done); /* restored mode/IRQ/Thumb state reaches dispatch */
}

static int a64_data(a64_emit_t *e, const arm_jit_op_t *op, uint32_t done) {
    unsigned code = op->a;
    int test = code >= 8u && code <= 11u;
    int flags = test || (op->d & ARM_BC_DATA_S);
    int logical = code < 2u || code == 8u || code == 9u || code >= 12u;
    /* w2 still holds the committed result of the previous data op (see the
     * fwd_guest note): reuse it instead of reloading that guest register. */
    int fwd = e->fwd_guest;
    e->fwd_guest = -1;
    if (op->d & ARM_BC_DATA_REGSHIFT) return 0;
    if (!test && op->c == 15u && flags && e->leaf) return 0;
    int arith_imm = (op->d & ARM_BC_DATA_IMM) &&
        (code == 2u || code == 4u || code == 10u || code == 11u) &&
        (op->imm <= 0xfffu || (!(op->imm & 0xfffu) && (op->imm >> 12) <= 0xfffu));
    /* A64 shifted-register ALU forms absorb a separate guest shifter op.
     * Keep RRX and encoded LSR/ASR #32 on the explicit path. Arithmetic
     * carry comes from add/sub, whereas flag-setting logical ops need the
     * ARM shifter carry and therefore retain a64_shift. RSB shifts its first
     * source; ADC/SBC/RSC have no shifted-register host form. */
    int folded_shift = !(op->d & ARM_BC_DATA_IMM) &&
        (op->g == 0u || op->h != 0u) &&
        ((logical && !flags) ||
         ((code == 2u || code == 4u || code == 10u || code == 11u) && op->g < 3u));
    uint32_t shift_bits = folded_shift ? (op->g << 22) | (op->h << 10) : 0u;
    uint32_t imm_bits = 0;
    if (arith_imm)
        imm_bits = op->imm <= 0xfffu ? op->imm << 10 :
                   0x00400000u | ((op->imm >> 12) << 10);
    /* Operand reloads are skipped when the value already sits in w2. The
     * second operand only qualifies while it reaches the ALU unchanged: the
     * folded shift travels inside the instruction, a zero-amount LSL emits
     * nothing, and every other a64_shift rewrites w1 and reads it for the
     * carry. The first operand is never transformed in place. Only MOV/MVN
     * may drop their first operand (the ARM encoding ignores Rn there); a
     * PC (r15) operand in either slot keeps its load, which is how the
     * pipeline value pc + 8 reaches w0/w1 (see a64_guest). */
    unsigned rn = 0u, rm = 1u;
    int reads_rn = code != 13u && code != 15u;
    int load_rn = reads_rn;
    int load_rm = !(op->d & ARM_BC_DATA_IMM);
    int plain_op2 = folded_shift || (!op->g && !op->h);
    if (load_rn && op->b == (unsigned)fwd) { load_rn = 0; rn = 2u; }
    if (load_rm && op->e == (unsigned)fwd && plain_op2) { load_rm = 0; rm = 2u; }
    /* Both operands naming one guest register need a single reload. */
    else if (load_rm && reads_rn && op->b == op->e && plain_op2) { load_rm = 0; rm = rn; }
    /* A rotated-immediate logical operand is itself an A64 logical immediate,
     * so the ALU instruction carries it directly and the materialization into
     * w1/w2 disappears. BIC and MVN use the complement, which is encodable for
     * some values only; unencodable values keep the general path. MOV/MVN name
     * the zero register instead of a first operand. */
    uint32_t logic_op = 0u, logic_enc = 0u;
    if ((op->d & ARM_BC_DATA_IMM) && logical && op->imm) {
        uint32_t v = code == 14u || code == 15u ? ~op->imm : op->imm;
        if (a64_logical_imm32(v, &logic_enc))
            logic_op = code == 1u || code == 9u ? 0x52000000u :
                       code == 12u || code == 13u || code == 15u ? 0x32000000u :
                                                                   0x12000000u;
    }
    if (load_rn) a64_guest(e, 0, op->b, op->pc);
    if (flags || (code >= 5u && code <= 7u) ||
        (!(op->d & ARM_BC_DATA_IMM) && op->g == 3u && !op->h))
        a64_load(e, 3, offsetof(arm920t_t, cpsr));
    if (op->d & ARM_BC_DATA_IMM) {
        if (!arith_imm && !logic_op) a64_imm(e, code == 13u ? 2u : 1u, op->imm);
        if (flags && logical) {
            if (op->d & ARM_BC_DATA_IMM_CV) a64_imm(e, 4, (op->d & ARM_BC_DATA_IMM_C) ? C_FLAG : 0u);
            else a64_mask(e, 4, 3, C_FLAG);
        }
    } else {
        if (load_rm) a64_guest(e, 1, op->e, op->pc);
        if (!folded_shift) a64_shift(e, op->g, op->h, flags && logical);
    }
    uint32_t s = flags ? 0x20000000u : 0u;
    /* and/eor/orr W2, <first operand or wzr>, #imm */
    if (logic_op)
        a64_u32(e, logic_op | logic_enc |
                    ((code == 13u || code == 15u ? 31u : rn) << 5) | 2u);
    else switch (code) {
    case 0: case 8: a64_rr(e, 0x0a000000u | shift_bits, 2, rn, rm); break;
    case 1: case 9: a64_rr(e, 0x4a000000u | shift_bits, 2, rn, rm); break;
    case 2: case 10:
        if (arith_imm) a64_u32(e, 0x51000000u | s | imm_bits | (rn << 5) | 2u); /* sub[s] w2, rn, #imm */
        else a64_rr(e, 0x4b000000u | s | shift_bits, 2, rn, rm);
        break;
    case 3: a64_rr(e, 0x4b000000u | s, 2, rm, rn); break;
    case 4: case 11:
        if (arith_imm) a64_u32(e, 0x11000000u | s | imm_bits | (rn << 5) | 2u); /* add[s] w2, rn, #imm */
        else a64_rr(e, 0x0b000000u | s | shift_bits, 2, rn, rm);
        break;
    case 5: case 6: case 7:
        a64_u32(e, 0xd51b4203u); /* msr nzcv, x3: ARM/A64 carry is no-borrow */
        a64_rr(e, (code == 5u ? 0x1a000000u : 0x5a000000u) | s,
               2, code == 7u ? rm : rn, code == 7u ? rn : rm);
        break;
    case 12: a64_rr(e, 0x2a000000u | shift_bits, 2, rn, rm); break;
    case 13:
        if (!(op->d & ARM_BC_DATA_IMM)) a64_rr(e, 0x2a000000u | shift_bits, 2, 31, rm);
        break;
    case 14: a64_rr(e, 0x0a200000u | shift_bits, 2, rn, rm); break;
    case 15: a64_rr(e, 0x2a200000u | shift_bits, 2, 31, rm); break;
    }
    if (flags) {
        if (logical) a64_cmp_imm(e, 2, 0);
        a64_u32(e, 0xd53b4205u); /* mrs x5, nzcv */
        if (logical) {
            a64_mask(e, 5, 5, N_FLAG | Z_FLAG);
            a64_rr(e, 0x2a000000u, 5, 5, 4);
        }
        a64_bfxil_low(e, 5, 3, logical ? 29u : 28u);
        a64_store(e, 5, offsetof(arm920t_t, cpsr));
        e->nzcv_valid = !logical;
    }
    if (!test) {
        if (op->c == 15u && flags) {
            /* Keep ALU flags for bankless USER/SYS before restoring SPSR. */
            a64_exception_return(e, 2, done);
            return 1;
        }
        if (op->c == 15u) a64_mask(e, 2, 2, ~3u);
        a64_put_guest(e, op->c, 2);
        if (op->c == 15u) {
            if (op->reserved == 7u) {
                /* Retire the real MOV pc,lr; an unexpected LR exits with its
                 * actual aligned PC instead of following a stale caller. */
                a64_imm(e, 6, e->expected_next);
                a64_cmp(e, 2, 6);
                size_t matched = a64_jump(e, 0); /* EQ */
                a64_return(e, done);
                a64_patch(e, matched, e->pos);
            } else a64_return(e, done);
        }
    }
    /* An unconditional plain register write leaves its committed value in w2
     * for the next op; a conditional op's fall-through reaches the same store
     * without executing it, so only cond==14 may publish the result. */
    if (!test && op->cond == 14u && op->c != 15u) e->fwd_guest = (int)op->c;
    return 1;
}

/* w0 = virtual address -> w9 = in-RAM offset; w0 keeps the original virtual
 * address for the cold chains and w2/w11 are preserved. The derived page tag
 * proves the exact current TLB mapping and an entire physical 4 KiB RAM page.
 * Misses, tiny pages, non-RAM and fault-tolerant identity mappings still use
 * the classified helper. MMU-off accesses retain the range guard. Word callers
 * ask for a 4-byte-aligned offset; only a halfword can straddle the end of the
 * prevalidated physical page, so it keeps a whole-window end check. */
static void a64_ram_offset(a64_emit_t *e, size_t *slow, unsigned *nslow, unsigned bytes,
                           int align4) {
    _Static_assert(sizeof(arm_jit_tlb_page_t) == 8u &&
                   offsetof(arm_jit_tlb_page_t, va_page) == 0u &&
                   offsetof(arm_jit_tlb_page_t, xor_offset) == 4u,
                   "native RAM page lookup layout");
    /* CP15 control writes and state loads invalidate the block generation;
     * a helper that changes it exits through arm_a64_exec_checked. The MMU
     * enable bit is therefore constant throughout this compiled block. */
    if (e->mmu) {
        /* The pair is 8 bytes wide and jit_tlb_page[] starts on an 8-byte
         * boundary of the CPU struct, so one 64-bit load fetches its
         * {VA page, XOR offset} words where a masked index, a scaled add and
         * a paired load used to sit. */
        _Static_assert((offsetof(arm920t_t, jit_tlb_page) & 7u) == 0u,
                       "native RAM page lookup alignment");
        a64_ubfx(e, 6, 0, 12, 12); /* original (VA >> 12) & 0xfff index */
        a64_u32(e, 0xf8665a87u); /* ldr x7, [x20, w6, uxtw #3]: VA page, XOR offset */
        a64_u32(e, 0x6b4030ffu); /* cmp w7, w0, lsr #12: full VA page tag */
        slow[(*nslow)++] = a64_jump(e, 1);
        /* One shifted-register EOR lifts the loaded pair's second word (the
         * XOR offset) and writes it next to the untouched VA: the mirror only
         * publishes entries whose low 12 bits are zero (a published page keeps
         * the VA's own offset bits), so aligning the result below is exactly
         * the w0 alignment the old order applied before the XOR. */
        a64_u32(e, 0xca478009u); /* eor x9, x0, x7, lsr #32: RAM offset */
    } else {
        a64_imm(e, 6, ARM_JIT_RAM_BASE_ADDR);
        a64_rr(e, 0x4b000000u, 9, 0, 6); /* sub w9, w0, w6 */
    }
    if (align4) a64_mask(e, 9, 9, ~3u);
    /* A word-aligned start and same-page block span cannot overrun RAM,
     * whose start/end are both 4 KiB aligned. */
    if (!e->mmu || bytes == 2u) {
        a64_imm(e, 6, ARM_JIT_RAM_SIZE_BYTES - bytes);
        a64_cmp(e, 9, 6);
        slow[(*nslow)++] = a64_jump(e, 8); /* HI also rejects identity below RAM */
    }
}

/* Cold path only: preserve the original VA in w12 for the checked fallback.
 * Read the authoritative TLB, without filling it or widening a restored mask.
 * Certificates contain live storage, never a cached value or skipped read.
 * A matched certificate resumes the hot continuation of its own access. */
static void a64_live_read32(a64_emit_t *e, unsigned rd, size_t cont) {
    if (!e->live_read32_count) return;
    size_t reject[3];
    unsigned nreject = 0;
    a64_mask(e, 6, 12, 3u);
    a64_cmp(e, 6, 31);
    reject[nreject++] = a64_jump(e, 1);
    if (e->mmu) {
        _Static_assert(sizeof(arm_tlb_entry_t) == 16u &&
                       offsetof(arm_tlb_entry_t, pa_base) == 4u &&
                       offsetof(arm_tlb_entry_t, mask) == 8u &&
                       offsetof(arm_tlb_entry_t, valid) == 12u,
                       "live read packed TLB layout");
        a64_ubfx(e, 6, 12, 12, 12);
        a64_cpu_addr(e, 10, offsetof(arm920t_t, tlb_entry));
        a64_rr(e, 0x8b001000u, 10, 10, 6); /* add x10, x10, x6, lsl #4 */
        a64_u32(e, 0x29400000u | (9u << 10) | (10u << 5) | 7u); /* VA, PA */
        a64_u32(e, 0x29410000u | (8u << 10) | (10u << 5) | 6u); /* mask, valid */
        a64_cmp(e, 8, 31);
        reject[nreject++] = a64_jump(e, 0);
        a64_rr(e, 0x0a200000u, 8, 12, 6); /* bic w8, w12, w6 */
        a64_cmp(e, 8, 7);
        reject[nreject++] = a64_jump(e, 1);
        a64_rr(e, 0x0a000000u, 0, 12, 6);
        a64_rr(e, 0x2a000000u, 0, 0, 9);
        a64_mask(e, 0, 0, ~3u);
    } else a64_mov(e, 0, 12);
    for (size_t i = 0; i < e->live_read32_count && !e->fail; ++i) {
        a64_imm(e, 6, e->live_read32[i].pa);
        a64_cmp(e, 0, 6);
        size_t unmatched = a64_jump(e, 1);
        a64_ptr(e, 10, (uintptr_t)e->live_read32[i].word);
        a64_u32(e, 0xb9400000u | (10u << 5) | 1u); /* fresh ldr w1, [x10] */
        a64_put_guest(e, rd, 1);
        /* Cold area: the continuation is behind this branch. */
        a64_branch_back(e, cont);
        a64_patch(e, unmatched, e->pos);
    }
    for (unsigned i = 0; i < nreject; ++i) a64_patch(e, reject[i], e->pos);
}

/* Cold chain probe: aligned word load inside a proven non-RAM read window,
 * entered after the RAM guard rejected the address.  w12 still holds the
 * unaligned VA, w2 the writeback for writeback shapes and w11 the ARM
 * unaligned rotation for a computed address.  Proves the same TLB/mapping
 * the identity-I/O probe proves, then:
 *   - a physical word below ARM_JIT_BIOS_SIZE_BYTES reads jit_bios_base[pa];
 *     the window is read-only to guests and fixed before blocks translate,
 *     so no callback runs and no exit check applies;
 *   - a physical word outside every window (RAM, BIOS, identity I/O) reads
 *     the bus-open constant, when the bus declares one (open_bus_word32);
 * the loaded word keeps the ARM unaligned rotation, Rd commits first and the
 * writeback second (the checked helpers own Rd==Rn ordering), then the
 * recorded hot continuation resumes.  Every rejection falls to the next
 * probe with the registers exactly as it found them. */
static void a64_ro_word(a64_emit_t *e, const arm_jit_op_t *op, size_t cont) {
    size_t reject[6];
    unsigned nreject = 0;
    a64_mask(e, 3, 12, ~3u); /* w3 = aligned VA, as the checked helpers use */
    if (e->mmu) {
        _Static_assert(sizeof(arm_tlb_entry_t) == 16u &&
                       offsetof(arm_tlb_entry_t, pa_base) == 4u &&
                       offsetof(arm_tlb_entry_t, mask) == 8u &&
                       offsetof(arm_tlb_entry_t, valid) == 12u,
                       "read-only window probe packed TLB layout");
        /* Read the authoritative TLB; never fill it or widen a mask. */
        a64_ubfx(e, 6, 3, 12, 12);
        a64_cpu_addr(e, 8, offsetof(arm920t_t, tlb_entry));
        a64_rr(e, 0x8b001000u, 8, 8, 6); /* add x8, x8, x6, lsl #4 */
        a64_u32(e, 0x29400000u | (9u << 10) | (8u << 5) | 7u); /* ldp w7, w9, [x8]: VA, PA */
        a64_u32(e, 0x29410000u | (6u << 10) | (8u << 5) | 5u); /* ldp w5, w6, [x8, #8]: mask, valid */
        reject[nreject++] = e->pos;
        a64_u32(e, 0x34000000u | 6u); /* cbz w6: no live mapping */
        a64_rr(e, 0x0a200000u, 6, 3, 5); /* bic w6, w3, w5 */
        a64_cmp(e, 6, 7);
        reject[nreject++] = a64_jump(e, 1); /* NE: VA outside this page */
        a64_rr(e, 0x0a000000u, 6, 3, 5); /* and w6, w3, w5 */
        a64_rr(e, 0x2a000000u, 0, 6, 9); /* orr w0, w6, w9: aligned physical word */
    } else a64_mov(e, 0, 3);
    if (e->bios_read) {
        /* pa < BIOS_SIZE already implies the aligned word fits: pa is a
         * multiple of four, so pa <= BIOS_SIZE - 4. */
        a64_imm(e, 6, ARM_JIT_BIOS_SIZE_BYTES);
        a64_cmp(e, 0, 6);
        size_t not_bios = a64_jump(e, 2); /* HS: pa outside the BIOS window */
        a64_cpu_mem(e, 0xf9400000u, 7, offsetof(arm920t_t, jit_bios_base), 3);
        a64_u32(e, 0xb8606800u | (0u << 16) | (7u << 5) | 1u); /* ldr w1, [x7, w0, uxtw] */
        if (op->a == 15u && !(op->d & ARM_BC_SD_REGOFF)) {
            uint32_t va = op->pc + 8u + ((op->d & ARM_BC_SD_U) ? op->imm : 0u - op->imm);
            if (va & 3u)
                a64_u32(e, 0x13810021u | (((va & 3u) * 8u) << 10)); /* ror w1 */
        } else {
            a64_rr(e, 0x1ac02c00u, 1, 1, 11); /* rorv w1, w1, w11 */
        }
        a64_put_guest(e, op->b, 1);
        if (!(op->d & ARM_BC_SD_P) || (op->d & ARM_BC_SD_W)) a64_put_guest(e, op->a, 2);
        a64_branch_back(e, cont);
        a64_patch(e, not_bios, e->pos);
    }
    if (e->open_bus) {
        /* Reject every windowed address; what remains is open bus. */
        a64_imm(e, 6, ARM_JIT_BIOS_SIZE_BYTES);
        a64_cmp(e, 0, 6);
        reject[nreject++] = a64_jump(e, 3); /* LO: pa < BIOS_SIZE */
        a64_imm(e, 6, ARM_JIT_RAM_BASE_ADDR);
        a64_rr(e, 0x4b000000u, 6, 0, 6); /* sub w6, w0, w6 */
        a64_imm(e, 7, ARM_JIT_RAM_SIZE_BYTES);
        a64_cmp(e, 6, 7);
        reject[nreject++] = a64_jump(e, 3); /* LO: pa inside the RAM window */
        a64_imm(e, 6, 0x14000000u);
        a64_rr(e, 0x4b000000u, 6, 0, 6); /* sub w6, w0, w6 */
        a64_imm(e, 7, 0x02000000u);
        a64_cmp(e, 6, 7);
        reject[nreject++] = a64_jump(e, 3); /* LO: pa inside the I/O window */
        a64_load(e, 1, offsetof(arm920t_t, bus) + offsetof(arm_bus_t, open_bus_word32));
        if (op->a == 15u && !(op->d & ARM_BC_SD_REGOFF)) {
            uint32_t va = op->pc + 8u + ((op->d & ARM_BC_SD_U) ? op->imm : 0u - op->imm);
            if (va & 3u)
                a64_u32(e, 0x13810021u | (((va & 3u) * 8u) << 10)); /* ror w1 */
        } else {
            a64_rr(e, 0x1ac02c00u, 1, 1, 11); /* rorv w1, w1, w11 */
        }
        a64_put_guest(e, op->b, 1);
        if (!(op->d & ARM_BC_SD_P) || (op->d & ARM_BC_SD_W)) a64_put_guest(e, op->a, 2);
        a64_branch_back(e, cont);
    }
    for (unsigned i = 0; i < nreject; ++i) a64_patch(e, reject[i], e->pos);
}

static int a64_single_dt(a64_emit_t *e, const arm_jit_op_t *op, uint32_t done) {
    unsigned d = op->d;
    int load = !!(d & ARM_BC_SD_L), byte = !!(d & ARM_BC_SD_B);
    int wb = !(d & ARM_BC_SD_P) || (d & ARM_BC_SD_W);
    if (!(load ? e->ram_read : e->ram_write) || (wb && op->a == 15u)) return 0;
    int access = op->b != 15u; /* PC load/store stays on whole-op fallback */
    size_t slow[4];
    unsigned nslow = 0;
    /* PC-relative immediate addresses are fixed by the decoded instruction.
     * Fold address/word alignment only, never the loaded memory contents. */
    int literal = op->a == 15u && !(d & ARM_BC_SD_REGOFF);
    uint32_t literal_addr = op->pc + 8u + ((d & ARM_BC_SD_U) ? op->imm : 0u - op->imm);
    if (literal) {
        a64_imm(e, 0, literal_addr);
    } else {
        if (d & ARM_BC_SD_REGOFF) {
            a64_guest(e, 1, op->c, op->pc);
            if (op->e == 3u && !op->f) a64_load(e, 3, offsetof(arm920t_t, cpsr));
            a64_shift(e, op->e, op->f, 0);
        }
        a64_guest(e, 0, op->a, op->pc);
        /* A pre-indexed access without writeback never needs the base+offset sum
         * as a guest value, so form it directly in the address register instead of
         * copying w2 into w0. Post-indexed and writeback forms keep w2. */
        int folded = (d & ARM_BC_SD_P) && !wb;
        unsigned sum = folded ? 0u : 2u;
        if (d & ARM_BC_SD_REGOFF)
            a64_rr(e, (d & ARM_BC_SD_U) ? 0x0b000000u : 0x4b000000u, sum, 0, 1);
        else if (op->imm)
            /* ARM's 12-bit transfer offset fits ADD/SUB W immediate exactly.
             * Neither form sets flags; W arithmetic preserves address wrap. */
            a64_u32(e, ((d & ARM_BC_SD_U) ? 0x11000000u : 0x51000000u) |
                       (op->imm << 10) | sum);
        else if (!folded)
            a64_mov(e, 2, 0); /* retain the original base for writeback */
        if ((d & ARM_BC_SD_P) && !folded) a64_mov(e, 0, 2);
        if (!byte && load) {
            /* Retain the ARM unaligned word rotation. Only a load consumes it, and
             * UBFIZ w11, w0, #3, #2 builds ((addr & 3) << 3) in one instruction.
             * The word alignment moves to the in-RAM offset, so w0 stays the
             * untouched VA the cold chains and the checked helpers expect. */
            a64_u32(e, 0x53000000u | (29u << 16) | (1u << 10) | 11u);
        }
    }
    a64_ram_offset(e, slow, &nslow, byte ? 1u : 4u, !byte);
    if (load) {
        /* w9 is already the in-RAM offset; register-offset addressing folds
         * the jit_ram_base add into the access itself. */
        a64_u32(e, (byte ? 0x38606800u : 0xb8606800u) | (9u << 16) | (21u << 5) | 1u);
        if (!byte) {
            if (!literal) a64_rr(e, 0x1ac02c00u, 1, 1, 11); /* rorv w1, w1, w11 */
            else if (literal_addr & 3u)
                a64_u32(e, 0x13810021u | (((literal_addr & 3u) * 8u) << 10)); /* ror w1 */
        }
        /* Match bytecode LDR PC: ARM state, word alignment, no interworking. */
        if (op->b == 15u) a64_mask(e, 1, 1, ~3u);
        a64_put_guest(e, op->b, 1);
    } else {
        a64_guest(e, 1, op->b, op->pc);
        a64_u32(e, (byte ? 0x38206800u : 0xb8206800u) | (9u << 16) | (21u << 5) | 1u);
    }
    if (wb) a64_put_guest(e, op->a, 2); /* includes the helper's Rn==Rd order */
    if (load && op->b == 15u) {
        if (op->reserved == 3u || op->reserved == 5u) {
            /* Keep the real loaded return, as for an inlined LDM PC. */
            a64_imm(e, 6, e->expected_next);
            a64_cmp(e, 1, 6);
            size_t matched = a64_jump(e, 0);
            a64_return(e, done);
            a64_patch(e, matched, e->pos);
        } else a64_return(e, done);
    }
    /* The fast path is complete and continues straight into the next
     * instruction.  Everything below it is recorded for the block's cold
     * area: the guard handlers, the live-read certificate (the authoritative
     * TLB is only read there), the direct identity-I/O probe with the address
     * this instruction already computed, and the checked helper that every
     * rejection lands on. */
    a64_cold_op_t *cold = a64_cold_open(e, op, done, A64_COLD_SINGLE_DT);
    if (cold) {
        cold->load = (uint8_t)load;
        cold->wb = (uint8_t)wb;
        cold->access = (uint8_t)access;
        cold->byte = (uint8_t)byte;
        cold->live = (uint8_t)(access && load && !byte && !wb);
        cold->ro = (uint8_t)(access && load && !byte &&
                             (e->bios_read || e->open_bus));
        for (unsigned i = 0; i < nslow; ++i) a64_cold_fix(e, cold, slow[i]);
    }
    return 1;
}

/* ARM_JIT_OP_HALF: LDRH/LDRSB/LDRSH/STRH through the same TLB + direct RAM
 * window contract as a64_single_dt, sized for the actual access (LDRSB is a
 * byte).  Rd==PC (loads mask and redirect, stores read raw r15) and Rn==PC
 * writeback (a control-flow change that lacks the stop bit) stay on the
 * classified helper, as does the interpreter's byte-store aliasing for
 * S=1,L=0 encodings.  The register offset is the raw pipeline PC - the
 * helpers run with r15 = pc+4 - unlike the base register, which follows
 * read_r's pc+8.  Writeback (base +/- off) is computed before the access
 * and committed after it, matching op_halfword_bc even when Rd==Rn.  Every
 * slow jump lands before the first guest-visible store. The access helper
 * consumes the computed VA/writeback and performs the same checked commit. */
static int a64_half(a64_emit_t *e, const arm_jit_op_t *op, uint32_t done) {
    unsigned d = op->d, sh = op->e;
    int load = !!(d & ARM_BC_HALF_L), p = !!(d & ARM_BC_HALF_P);
    int wb = !p || !!(d & ARM_BC_HALF_W);
    if (!(load ? e->ram_read : e->ram_write) || op->b == 15u ||
        (wb && op->a == 15u) || (load ? !sh : sh != 1u)) return 0;
    size_t slow[4];
    unsigned nslow = 0;
    if (d & ARM_BC_HALF_IMM) a64_imm(e, 1, op->imm);
    else if (op->c == 15u) a64_imm(e, 1, op->pc + 4u);
    else a64_guest(e, 1, op->c, op->pc);
    a64_guest(e, 0, op->a, op->pc);
    /* w2 = base +/- off; only a writeback ever republishes it, so pre-indexed
     * accesses without writeback form the target address in w0 directly. */
    int folded_half = p && !wb;
    a64_rr(e, (d & ARM_BC_HALF_U) ? 0x0b000000u : 0x4b000000u, folded_half ? 0u : 2u, 0, 1);
    if (p && !folded_half) a64_mov(e, 0, 2); /* pre-indexed accesses target the offset address */
    a64_ram_offset(e, slow, &nslow, (load && sh == 2u) ? 1u : 2u, 0);
    if (load) {
        /* ldrh/ldrsb/ldrsh w1, [x21, w9]: A64 already yields the exact
         * zero/sign extension op_halfword_bc produces. */
        a64_u32(e, (sh == 1u ? 0x78606800u : sh == 2u ? 0x38e06800u : 0x78e06800u) |
                 (9u << 16) | (21u << 5) | 1u);
        a64_put_guest(e, op->b, 1);
    } else {
        a64_guest(e, 1, op->b, op->pc);
        a64_u32(e, 0x78206800u | (9u << 16) | (21u << 5) | 1u); /* strh w1, [x21, x9] */
    }
    if (wb) a64_put_guest(e, op->a, 2);
    /* As for a64_single_dt: the fast path is complete, and the guard
     * handlers plus the checked access helper move to the cold area. */
    a64_cold_op_t *cold = a64_cold_open(e, op, done, A64_COLD_HALF);
    if (cold) {
        cold->load = (uint8_t)load;
        cold->wb = (uint8_t)wb;
        for (unsigned i = 0; i < nslow; ++i) a64_cold_fix(e, cold, slow[i]);
    }
    return 1;
}

static void a64_block_dt_transfer(a64_emit_t *e, const arm_jit_op_t *op,
                                   unsigned count, int two_page,
                                   size_t *slow, unsigned *nslow);
static void a64_block_dt_exit(a64_emit_t *e, const arm_jit_op_t *op,
                              uint32_t done);

/* ARM_JIT_OP_BLOCK_DT: one-shot RAM fast path for the hot stack and block
 * transfers (STMDB!/STMIA!/LDMIA!/LDMDB).  The compile-time subset is the
 * part that still collapses to plain word moves: S=0, a non-empty list and
 * Rn != PC.  Rn may itself be transferred.  Stores materialize r15's pipeline
 * value (op->pc + 8); loads let a transferred Rn override its old value and,
 * exactly like op_block_dt_bc, suppress final Rn writeback.  A one-page span
 * uses the original single translation.  A span crossing one 4 KiB boundary
 * saves addr0 in w11, resolves both page representatives while preserving
 * offset0 in w2 (the documented a64_ram_offset survivors), and reconstructs
 * writeback from addr0 afterward.
 * Runtime, before the first guest-visible mutation: addr0 must be word
 * aligned, each touched virtual page must resolve to a full RAM page (a 1 KiB
 * tiny page could remap mid-fragment), and a loaded PC must be an
 * even target. Odd targets conservatively re-execute on the helper with
 * unmodified guest state. ARMv4T LDM uses write_r, not BX/write_pc_x, so the
 * helper masks both low bits without exchanging into Thumb state.
 * Every rejection jumps to the classified helper before any mutation;
 * writeback commits last, except that a load containing Rn intentionally
 * leaves Rn at its memory-loaded value. */
static int a64_block_dt(a64_emit_t *e, const arm_jit_op_t *op, uint32_t done) {
    unsigned d = op->d, rn = op->a;
    uint32_t list = op->imm;
    int load = !!(d & 0x01u), p = !!(d & 0x10u), u = !!(d & 0x08u), w = !!(d & 0x02u);
    if ((op->reserved && op->reserved != 3u && op->reserved != 4u && op->reserved != 5u) ||
        (d & 0x04u) || !list || rn == 15u || !op->g) return 0;
    if (!(load ? e->ram_read : e->ram_write)) return 0;
    unsigned count = op->g; /* 1..16 when the full list is present */
    size_t slow[16];
    unsigned nslow = 0;
    size_t xpage = 0;
    a64_guest(e, 0, rn, op->pc); /* w0 = base; Rn != PC, so it is a plain r[] load */
    if (w) /* w2 = final_base = base +/- 4*count; w2 survives a64_ram_offset */
        a64_u32(e, (u ? 0x11000000u : 0x51000000u) | ((4u * count) << 10) | (0u << 5) | 2u);
    /* w0 = addr0, the first (lowest) transferred address. */
    if (u) {
        if (p) a64_u32(e, 0x11000000u | (4u << 10) | (0u << 5) | 0u); /* add w0, w0, #4 */
    } else {
        a64_u32(e, 0x51000000u | ((4u * count - (p ? 0u : 4u)) << 10) | (0u << 5) | 0u);
    }
    a64_mask(e, 6, 0, 3u);
    slow[nslow++] = e->pos;
    a64_u32(e, 0x35000000u | 6u); /* cbnz w6 -> helper on unaligned span */
    if (count > 1u) {
        /* Same virtual 4 KiB page takes the original single-translation path.
         * A span crossing a 4 KiB boundary branches to the cold two-page retry
         * behind the block instead of walking an inline second copy; with
         * count <= 16 (64 bytes) an aligned span crosses at most one boundary. */
        a64_u32(e, 0x11000000u | ((4u * (count - 1u)) << 10) | (0u << 5) | 5u); /* w5 = last */
        a64_rr(e, 0x4a000000u, 6, 0, 5); /* eor w6, w0, w5 */
        a64_ubfx(e, 6, 6, 12, 20);
        xpage = e->pos;
        a64_u32(e, 0x35000000u | 6u); /* cbnz w6 -> cold two-page retry */
    }
    /* The span stays within this 4 KiB page, so proving its aligned first
     * word in a full RAM page also proves every transferred word. */
    a64_ram_offset(e, slow, &nslow, 4u, 0);
    a64_rr(e, 0x8b000000u, 12, 21, 9); /* x12 = jit_ram_base + in-RAM offset */
    a64_block_dt_transfer(e, op, count, 0, slow, &nslow);
    if (w && (!load || !(list & (1u << rn))))
        a64_put_guest(e, rn, 2); /* a load containing Rn suppresses this */
    a64_block_dt_exit(e, op, done);
    /* The single-page path is complete and falls through to the op's
     * continuation, which is also where the cold two-page retry resumes.
     * Everything the fast paths skip moves behind the block: the two-page
     * retry as its own chain (first, so its own guard rejections can be
     * forward-patched into the classified-helper chain that follows) and the
     * ordinary guard handlers, odd-load-PC prechecks and classified fallback
     * in the sibling chain. */
    a64_cold_op_t *xcold = NULL;
    if (count > 1u) {
        xcold = a64_cold_open(e, op, done, A64_COLD_BLOCK_DT_XPAGE);
        if (xcold) a64_cold_fix(e, xcold, xpage);
    }
    a64_cold_op_t *cold = a64_cold_open(e, op, done, A64_COLD_BLOCK_DT);
    if (xcold && cold) xcold->sibling = (unsigned)(cold - e->cold);
    if (cold) for (unsigned i = 0; i < nslow; ++i) a64_cold_fix(e, cold, slow[i]);
    return 1;
}

/* Cold two-page retry for a block transfer whose aligned span crosses one
 * 4 KiB boundary.  This is the sequence the inline layout used to carry (and
 * skip) inside the hot body; it runs with the same registers the single-page
 * path held at its page-cross branch: w0 = addr0, w2 = final_base (writeback
 * shapes), w11 = free.  Guard rejections are collected in slow[] and join the
 * sibling classified-helper chain, so a rejected retry re-executes the whole
 * op on the checked path with the guest state untouched. */
static void a64_block_dt_xpage(a64_emit_t *e, const arm_jit_op_t *op, uint32_t done,
                               size_t *slow, unsigned *nslow) {
    unsigned d = op->d, rn = op->a;
    uint32_t list = op->imm;
    int load = !!(d & 0x01u), p = !!(d & 0x10u), u = !!(d & 0x08u), w = !!(d & 0x02u);
    unsigned count = op->g;
    /* w11 retains virtual addr0 and w2 retains offset0 across the second
     * a64_ram_offset call. w2's normal writeback value is reconstructed
     * after both translations, so using it as a preserved temporary is
     * safe. */
    a64_mov(e, 11, 0); /* w11 = addr0 */
    a64_ram_offset(e, slow, nslow, 4u, 0);
    a64_mov(e, 2, 9); /* w2 = offset0 */
    a64_u32(e, 0x11000000u | (0xfffu << 10)); /* add w0, w0, #0xfff */
    a64_mask(e, 0, 0, ~0xfffu); /* w0 = next 4 KiB boundary */
    a64_ram_offset(e, slow, nslow, 4u, 0);
    a64_mov(e, 4, 9); /* w4 = offset1 */

    /* w5 = number of words in the first page. */
    a64_mask(e, 5, 11, 0xfffu);
    a64_imm(e, 6, 0x1000u);
    a64_rr(e, 0x4b000000u, 5, 6, 5); /* w5 = 0x1000 - addr0 page offset */
    a64_ubfx(e, 5, 5, 2, 30); /* w5 /= 4 */

    /* Each fragment ends at or before its already-proved RAM page edge. */
    a64_rr(e, 0x8b000000u, 12, 21, 2); /* x12 = RAM pointer for page 0 */
    a64_rr(e, 0x8b000000u, 15, 21, 4); /* x15 = RAM pointer for page 1 */
    /* Bias page 1 by the page-0 word count so both pointers use the same
     * ascending transfer index. Only indices >= w5 dereference this base. */
    a64_rr(e, 0xcb000800u, 15, 15, 5); /* sub x15, x15, x5, lsl #2 */

    if (w) { /* Reconstruct final_base from the saved virtual addr0. */
        a64_mov(e, 2, 11);
        if (u) {
            uint32_t adjust = 4u * count - (p ? 4u : 0u);
            a64_u32(e, 0x11000000u | (adjust << 10) | (11u << 5) | 2u);
        } else if (!p) {
            a64_u32(e, 0x51000000u | (4u << 10) | (11u << 5) | 2u);
        }
    }
    a64_block_dt_transfer(e, op, count, 1, slow, nslow);
    if (w && (!load || !(list & (1u << rn))))
        a64_put_guest(e, rn, 2);
    a64_block_dt_exit(e, op, done);
}

static void a64_block_dt_transfer(a64_emit_t *e, const arm_jit_op_t *op,
                                   unsigned count, int two_page,
                                   size_t *slow, unsigned *nslow) {
    uint32_t list = op->imm;
    int load = !!(op->d & 0x01u);
    int pc_load = load && (list & 0x8000u);
    if (pc_load) {
        /* PC is the top list bit and therefore the final ascending word.  A
         * two-page transfer always places that word in page 1; its biased
         * x15 makes this offset count-1-w5 from the actual page-1 base.
         * Preload and test it before any guest mutation. */
        unsigned base = two_page ? 15u : 12u;
        a64_u32(e, 0xb9400000u | ((count - 1u) << 10) | (base << 5) | 14u);
        a64_mask(e, 6, 14, 1u);
        slow[(*nslow)++] = e->pos;
        a64_u32(e, 0x35000000u | 6u); /* cbnz w6 -> conservative odd-target helper */
    }
    unsigned idx = 0;
    for (unsigned r = 0; r < 16u; ++r) {
        if (!(list & (1u << r))) continue;
        if (!two_page && r < 14u) {
            uint32_t remaining = list & 0x7fffu & ~((1u << (r + 1u)) - 1u);
            if (remaining) {
                unsigned next = arm_ctz16(remaining);
                /* The complete aligned span was proved to be direct RAM
                 * before any mutation. Pair adjacent transfer slots, even
                 * for sparse guest lists; PC retains its precheck and scalar
                 * pipeline semantics. Cross-page fragments stay scalar.
                 * w2 (writeback), w5 (page split) and w14 (PC) survive. */
                uint32_t ram_pair = (load ? 0x29400000u : 0x29000000u) |
                                    (idx << 15) | (3u << 10) | (12u << 5) | 1u;
                uint32_t guest_pair = (load ? 0x29000000u : 0x29400000u) |
                    ((uint32_t)(offsetof(arm920t_t, r) / 4u + r) << 15) |
                    (3u << 10) | (19u << 5) | 1u;
                if (load) {
                    a64_u32(e, ram_pair); /* ldp w1, w3, [x12, #idx*4] */
                    if (next == r + 1u) a64_u32(e, guest_pair);
                    else {
                        a64_put_guest(e, r, 1);
                        a64_put_guest(e, next, 3);
                    }
                } else {
                    if (next == r + 1u) a64_u32(e, guest_pair);
                    else {
                        a64_guest(e, 1, r, op->pc);
                        a64_guest(e, 3, next, op->pc);
                    }
                    a64_u32(e, ram_pair); /* stp w1, w3, [x12, #idx*4] */
                }
                r = next;
                idx += 2u;
                continue;
            }
        }
        unsigned base = 12u, off = idx;
        if (two_page) {
            /* cmp w5, #idx; csel x13, x12, x15, HI selects page 0 exactly
             * when idx < first-page word count (w5). */
            a64_cmp_imm(e, 5, idx);
            a64_u32(e, 0x9a8f818du); /* csel x13, x12, x15, HI */
            base = 13u;
        }
        if (load) {
            if (r == 15u) {
                /* ARM-state write_r masks both low bits and preserves CPSR. */
                a64_mask(e, 14, 14, ~3u);
                a64_put_guest(e, 15u, 14);
            } else {
                a64_u32(e, 0xb9400000u | (off << 10) | (base << 5) | 1u);
                a64_put_guest(e, r, 1);
            }
        } else {
            a64_guest(e, 1, r, op->pc); /* r15 materializes pc + 8 exactly */
            a64_u32(e, 0xb9000000u | (off << 10) | (base << 5) | 1u);
        }
        ++idx;
    }
}

static void a64_block_dt_exit(a64_emit_t *e, const arm_jit_op_t *op,
                              uint32_t done) {
    int pc_load = (op->d & 0x01u) && (op->imm & 0x8000u);
    if (!pc_load) return;
    if (op->reserved == 3u || op->reserved == 5u) {
        /* Inlined leaf returns can continue only at the decoded next PC.
         * The real stack word was loaded above; a modified return address
         * leaves the trace, and every stack write remains observable. */
        a64_imm(e, 6, e->expected_next);
        a64_cmp(e, 14, 6);
        size_t matched = a64_jump(e, 0); /* EQ */
        a64_return(e, done);
        a64_patch(e, matched, e->pos);
    } else a64_return(e, done);
}

static int a64_mul(a64_emit_t *e, const arm_jit_op_t *op, uint32_t done) {
    const uint32_t insn = op->insn;
    if (insn & (1u << 23)) return 0; /* long multiply stays on precise C path */
    unsigned rd = (insn >> 16) & 0xfu, rn = (insn >> 12) & 0xfu,
             rs = (insn >> 8) & 0xfu, rm = insn & 0xfu;
    if (rd == 15u || rn == 15u || rs == 15u || rm == 15u) return 0;
    a64_guest(e, 0, rm, op->pc);
    a64_guest(e, 1, rs, op->pc);
    if (insn & (1u << 21)) {
        a64_guest(e, 3, rn, op->pc);
        a64_u32(e, 0x1b000000u | (1u << 16) | (3u << 10) | (0u << 5) | 2u); /* madd w2, w0, w1, w3 */
    } else {
        a64_u32(e, 0x1b007c00u | (1u << 16) | (0u << 5) | 2u); /* mul w2, w0, w1 */
    }
    a64_put_guest(e, rd, 2);
    if (insn & (1u << 20)) {
        /* set_nz: rebuild only N/Z in CPSR, preserving C/V. */
        a64_load(e, 3, offsetof(arm920t_t, cpsr));
        a64_cmp_imm(e, 2, 0); /* subs wzr, w2, #0 */
        a64_u32(e, 0xd53b4205u); /* mrs x5, nzcv */
        a64_bfxil_low(e, 5, 3, 30u); /* N/Z from w5; C/V and control from w3. */
        a64_store(e, 5, offsetof(arm920t_t, cpsr));
    }
    GP32_UNUSED(done);
    return 1;
}

/* Fast paths appended at the end of this file; declared here because a64_one
 * dispatches to them.  Each returns 0 to keep the classified helper. */
static void a64_regshift_op2(a64_emit_t *e, unsigned type, int carry);
static int a64_data_regshift(a64_emit_t *e, const arm_jit_op_t *op, uint32_t done);
static int a64_long_mul(a64_emit_t *e, const arm_jit_op_t *op, uint32_t done);
static int a64_local_p15(a64_emit_t *e, const arm_jit_op_t *op, uint32_t done);
static int a64_psr_mov(a64_emit_t *e, const arm_jit_op_t *op, uint32_t done);

static int a64_one(a64_emit_t *e, const arm_jit_op_t *op, uint32_t done) {
    /* Only an explicitly proven producer below may make host flags reusable.
     * Memory guards, helpers and partial logical flag writes are barriers. */
    e->nzcv_valid = 0;
    if (op->kind == ARM_JIT_OP_DATA) {
        if (op->d & ARM_BC_DATA_REGSHIFT) {
            /* The register-shift form builds its operands in w0/w1/w6/w7 and
             * writes its own result to w2, so a forwarded value cannot cross
             * it (it publishes its own result under the same conditions). */
            e->fwd_guest = -1;
            return a64_data_regshift(e, op, done);
        }
        return a64_data(e, op, done);
    }
    /* Every other emitter writes w2 (address formation, transfer value,
     * writeback) or calls a helper that clobbers it: the forwarded result of
     * the previous op can never survive one of them. */
    e->fwd_guest = -1;
    if (op->kind == ARM_JIT_OP_MUL) {
        if (op->insn & (1u << 23)) return a64_long_mul(e, op, done);
        return a64_mul(e, op, done);
    }
    if (op->kind == ARM_JIT_OP_HALF) return a64_half(e, op, done);
    if (op->kind == ARM_JIT_OP_SINGLE_DT) return a64_single_dt(e, op, done);
    if (op->kind == ARM_JIT_OP_BLOCK_DT) return a64_block_dt(e, op, done);
    if (op->kind == ARM_JIT_OP_PSR) return a64_psr_mov(e, op, done);
    if (op->kind == ARM_JIT_OP_COPROC) return a64_local_p15(e, op, done);
    if (op->kind == ARM_JIT_OP_BRANCH) {
        if (op->insn & (1u << 24))
            a64_store_imm(e, offsetof(arm920t_t, r) + 14u * 4u, op->pc + 4u);
        a64_store_imm(e, offsetof(arm920t_t, r) + 15u * 4u, arm_jit_branch_target(op->pc, op->insn));
        if (e->loop_done == done) { a64_selfloop(e, done); return 1; }
        /* Inlined BL and stitched unconditional B follow the decoded trace. */
        if (op->reserved != 2u && (op->stop || !arm_jit_is_uncond_b_no_link(op->insn))) a64_return(e, done);
        return 1;
    }
    return 0;
}

/* Exclude ordinary calls, status/coprocessor operations,
 * exceptions and PC-writing data/memory from the repeating prefix. The tail
 * after a conditional backedge keeps every existing exit/helper path.
 *
 * A plain poll candidate is compiled here only after the dispatcher promoted
 * it, which the portable path only does after many repetitions that never
 * proved a skip, and any candidate that ever skipped stays portable. Such a
 * block must run its whole repetition natively: a promoted block that returns
 * to the dispatcher after every poll body re-pays the trace entry, the
 * dispatcher lookup and the checked access helper on each repetition, which
 * for the four-instruction BIOS device poll cancelled the promotion on the
 * measured LGM loading window. The self backedge repeats exactly the decoded
 * prefix with the existing budget/generation/epoch/interrupt fences, so every
 * repetition still executes and no skip decision moves to native code. */
static uint32_t a64_selfloop_prefix(arm920t_t *c, arm_jit_block_t *b) {
    if (arm_jit_needs_poll(c, b) &&
        !(c->jit_poll && (c->jit_poll[b - c->jit_blocks].flags & ARM_JIT_POLL_NATIVE)))
        return 0;
    for (unsigned i = 0; i < b->count; ++i) {
        const arm_jit_op_t *op = &arm_jit_ops(c, b)[i];
        /* Existing register-only leaf inlining proves LR/SP/mode unchanged.
         * Its decoded return is materialized directly by the main emitter. */
        if (op->reserved == 7u) continue;
        /* The decoder has already validated these framed leaf operations.
         * Keep their real stack reads/writes and existing return-target guard;
         * a changed/Thumb return exits before reaching the self backedge. */
        if (op->kind == ARM_JIT_OP_BLOCK_DT &&
            (op->reserved == 3u || op->reserved == 4u || op->reserved == 5u))
            continue;
        if (op->kind == ARM_JIT_OP_SINGLE_DT &&
            (op->reserved == 3u || op->reserved == 5u)) continue;
        if (op->kind == ARM_JIT_OP_COPROC && op->reserved == 6u) continue;
        if (op->reserved == 8u) continue; /* checked conditional taken successor */
        if (op->reserved && op->reserved != 2u) return 0;
        if (op->kind == ARM_JIT_OP_BRANCH) {
            if ((op->insn & (1u << 24)) && op->reserved != 2u) return 0;
            if (!(op->insn & (1u << 24)) && op->cond != 15u &&
                arm_jit_branch_target(op->pc, op->insn) == b->tag_pc)
                return i + 1u;
        } else if (op->kind == ARM_JIT_OP_DATA) {
            if (op->c == 15u) return 0;
        } else if (op->kind == ARM_JIT_OP_MUL) {
            if (((op->insn >> 16) & 15u) == 15u || ((op->insn >> 12) & 15u) == 15u ||
                ((op->insn >> 8) & 15u) == 15u || (op->insn & 15u) == 15u) return 0;
        } else if (op->kind == ARM_JIT_OP_SINGLE_DT) {
            int wb = !(op->d & ARM_BC_SD_P) || (op->d & ARM_BC_SD_W);
            if (((op->d & ARM_BC_SD_L) && op->b == 15u) || (wb && op->a == 15u)) return 0;
        } else if (op->kind == ARM_JIT_OP_HALF) {
            int wb = !(op->d & ARM_BC_HALF_P) || (op->d & ARM_BC_HALF_W);
            if (op->b == 15u || (wb && op->a == 15u)) return 0;
        } else if (op->kind == ARM_JIT_OP_BLOCK_DT) {
            if (op->a == 15u || !op->imm || (op->imm & 0x8000u) || (op->d & 0x04u)) return 0;
        } else return 0;
        if (op->stop && op->kind != ARM_JIT_OP_BRANCH) return 0;
    }
    return 0;
}

/* Replay the recorded cold chains after the block's hot body, so every line
 * the RAM fast path walks is free of cold bytes.  A chain is entered only
 * through its recorded hot branches and resumes at the continuation that was
 * recorded with it, which is the point the inline layout fell through to.  The
 * executed sequence itself is unchanged: the same guard handlers, probes and
 * checked helpers run, at the same point of the same instruction, with the
 * same registers and the same continuation.  Host NZCV never has to survive a
 * chain by construction: only a flag-setting data op leaves nzcv_valid set and
 * those ops emit no cold chain, so no chain can sit between a flag producer
 * and the conditional code that consumes its flags. */
static void a64_cold_flush(a64_emit_t *e) {
    e->in_cold = 1;
    for (unsigned i = 0; i < e->cold_count && !e->fail; ++i) {
        a64_cold_op_t *c = &e->cold[i];
        size_t entry = e->pos;
        for (unsigned j = 0; j < c->nfixup; ++j) a64_patch(e, c->fixup[j], entry);
        e->expected_next = c->expected_next;
        e->generation = c->generation;
        switch (c->kind) {
        case A64_COLD_BLOCK_DT_XPAGE: {
            /* The hot page-cross branch lands here instead of walking an
             * inline second copy of the whole transfer. The retry's own guard
             * rejections belong to the sibling classified-helper chain, which
             * is emitted after this one, so every fixup stays a forward
             * patch. Its continuation is the hot fall-through behind the
             * single-page path. */
            size_t xslow[8];
            unsigned nxs = 0;
            a64_cold_op_t *sib = c->sibling < e->cold_count ? &e->cold[c->sibling] : NULL;
            a64_block_dt_xpage(e, c->op, c->done, xslow, &nxs);
            if (sib) for (unsigned j = 0; j < nxs; ++j) a64_cold_fix(e, sib, xslow[j]);
            a64_branch_back(e, c->cont);
            break;
        }
        case A64_COLD_SINGLE_DT: {
            /* The inline order: certificate, direct identity-I/O attempt,
             * then the checked helper that every rejection lands on. The hot
             * body keeps the VA in w0; the cold sequences work on the
             * established w12 handover, which is restored here (cold bytes,
             * never walked by the fast path). */
            a64_mov(e, 12, 0);
            if (c->live) a64_live_read32(e, c->op->b, c->cont);
            /* The read-only window probe claims BIOS-window and, when the
             * bus declares one, open-bus words; every rejection falls to the
             * identity-I/O attempt with the registers it found. */
            if (c->ro) a64_ro_word(e, c->op, c->cont);
            size_t io_cold[4];
            unsigned nio = 0;
            if (c->access && !c->byte)
                a64_io_word(e, c->op, c->done, c->load, c->wb, io_cold, &nio, c->cont);
            for (unsigned j = 0; j < nio; ++j) a64_patch(e, io_cold[j], e->pos);
            if (c->access) a64_mem_slow(e, c->op, c->done, c->load, c->wb);
            else a64_slow(e, c->op, c->done);
            break;
        }
        case A64_COLD_HALF:
            a64_mov(e, 12, 0); /* the hot body keeps the VA in w0 */
            a64_mem_slow(e, c->op, c->done, c->load, c->wb);
            break;
        default:
            a64_slow(e, c->op, c->done);
            break;
        }
        /* Both checked helpers end with a "may continue" edge that lands
         * exactly here; this is the recorded fall-through of the inline
         * layout, now a backward branch into the hot body. */
        a64_branch_back(e, c->cont);
    }
    e->in_cold = 0;
}

/* Copy one emitted block into the arena with its hot words [0, hot) at
 * hot_dst and its cold words [hot, n) at cold_dst.  The buffer holds only
 * instructions (no literals), so every PC-relative branch is found by its
 * encoding; only branches crossing the hot/cold boundary change, and each is
 * re-encoded for the new distance.  Returns 0 if one would not reach. */
static int a64_place(uint32_t *hot_dst, uint32_t *cold_dst, const uint32_t *src,
                     size_t hot, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        uint32_t w = src[i], mask;
        unsigned bits;
        if ((w & 0x7c000000u) == 0x14000000u) { bits = 26; mask = 0x03ffffffu; }
        else if ((w & 0xff000010u) == 0x54000000u || (w & 0x7e000000u) == 0x34000000u) { bits = 19; mask = 0x7ffffu << 5; }
        else if ((w & 0x7e000000u) == 0x36000000u) { bits = 14; mask = 0x3fffu << 5; }
        else bits = 0, mask = 0;
        if (bits) {
            unsigned shift = bits == 26 ? 0u : 5u;
            int64_t imm = (int64_t)((w & mask) >> shift);
            if (imm & ((int64_t)1 << (bits - 1))) imm -= (int64_t)1 << bits;
            int64_t t = (int64_t)i + imm;
            if (t < 0 || t > (int64_t)n) return 0;
            if ((i < hot) != ((size_t)t < hot)) {
                intptr_t from = i < hot ? (intptr_t)(hot_dst + i) : (intptr_t)(cold_dst + (i - hot));
                intptr_t to = (size_t)t < hot ? (intptr_t)(hot_dst + t) : (intptr_t)(cold_dst + ((size_t)t - hot));
                int64_t d = (int64_t)(to - from) / 4;
                if (d >= ((int64_t)1 << (bits - 1)) || d < -((int64_t)1 << (bits - 1))) return 0;
                w = (w & ~mask) | (((uint32_t)d << shift) & mask);
            }
        }
        if (i < hot) hot_dst[i] = w;
        else cold_dst[i - hot] = w;
    }
    return 1;
}

static void arm_jit_compile_native(arm920t_t *c, arm_jit_block_t *b) {
    if (!c || !b || !b->count) return;
    uint32_t tmp[ARM_JIT_NATIVE_MAX_BYTES / sizeof(uint32_t)];
    a64_cold_op_t cold[ARM_JIT_MAX_INSNS];
    a64_emit_t e = {0};
    e.code = tmp;
    e.cap = sizeof(tmp) / sizeof(tmp[0]);
    e.cold = cold;
    e.cold_cap = ARM_JIT_MAX_INSNS;
    e.fwd_guest = -1;
    e.mmu = !!(c->cp15[1] & 1u);
    e.live_read32 = c->live_read32;
    e.live_read32_count = c->live_read32_count;
    e.loop_done = a64_selfloop_prefix(c, b);
#if ARM920T_PROFILING
    c->profile_native_loop[b - c->jit_blocks] = e.loop_done != 0;
#endif
    e.block_count = b->count;
    /* Validate the complete direct window, including write permission.
     * Tiny or discontinuous test buses must retain their callback path. */
    e.ram_read = c->jit_ram_base && fastmem(c, ARM_JIT_RAM_BASE_ADDR, ARM_JIT_RAM_SIZE_BYTES, 0) == c->jit_ram_base;
    e.ram_write = c->jit_ram_base && fastmem(c, ARM_JIT_RAM_BASE_ADDR, ARM_JIT_RAM_SIZE_BYTES, 1) == c->jit_ram_base;
    /* The read-only probes inline a physical word only when its whole window
     * is backed exactly as the checked helpers will find it.  The open-bus
     * contract additionally needs no fastmem mapping outside the windows: the
     * only map s3c2400-style buses extend is RAM itself, so probe one byte past
     * the window.  ram_size changes happen through state load, which kills
     * every block, so the check cannot go stale inside compiled code. */
    e.bios_read = c->jit_bios_base && fastmem(c, 0, ARM_JIT_BIOS_SIZE_BYTES, 0) == c->jit_bios_base;
    e.open_bus = c->bus.open_bus_valid &&
                 !fastmem(c, ARM_JIT_RAM_BASE_ADDR + ARM_JIT_RAM_SIZE_BYTES, 1u, 0);
    if (e.ram_read || e.ram_write) {
        for (unsigned i = 0; i < b->count; ++i)
            if (arm_jit_ops(c, b)[i].kind == ARM_JIT_OP_SINGLE_DT || arm_jit_ops(c, b)[i].kind == ARM_JIT_OP_HALF ||
                arm_jit_ops(c, b)[i].kind == ARM_JIT_OP_BLOCK_DT) { e.memory = 1; break; }
    }
    /* Only simple ALU/branch traces can omit the host frame/LR save. Memory
     * guards can call helpers even when the usual runtime path hits RAM. */
    e.leaf = !e.memory;
    for (unsigned i = 0; e.leaf && i < b->count; ++i) {
        const arm_jit_op_t *op = &arm_jit_ops(c, b)[i];
        if (op->reserved == 7u && i + 1u < b->count) continue;
        if (op->cond == 15u || op->kind == ARM_JIT_OP_BRANCH) continue;
        int test = op->a >= 8u && op->a <= 11u;
        if (op->kind != ARM_JIT_OP_DATA || (op->d & ARM_BC_DATA_REGSHIFT) ||
            (!test && op->c == 15u && (op->d & ARM_BC_DATA_S))) e.leaf = 0;
    }
    a64_u32(&e, 0xd503245fu); /* bti c (NOP on ARMv8.0) */
    if (e.loop_done) {
        a64_u32(&e, 0xa9be5ff6u); /* stp x22, x23, [sp, #-32]! */
        a64_u32(&e, 0xa90167f8u); /* stp x24, x25, [sp, #16] */
    }
    /* Leaf traces never call a helper, so a single x19 slot and the untouched
     * x29/x30 suffice. Other traces save the LR (helpers clobber it) in a
     * 16-byte-aligned frame; only memory paths use x20/x21. No frame pointer
     * is kept: the epilogue restores through sp, so x29 stays the caller's
     * value and neither a slot nor a "mov x29, sp" is emitted. */
    if (e.leaf) {
        a64_u32(&e, 0xf81f0ff3u); /* str x19, [sp, #-16]! */
    } else if (e.memory) {
        a64_u32(&e, 0xa9be53f3u); /* stp x19, x20, [sp, #-32]! */
        a64_u32(&e, 0xa9017bf5u); /* stp x21, x30, [sp, #16] */
    } else {
        a64_u32(&e, 0xa9bf7bf3u); /* stp x19, x30, [sp, #-16]! */
    }
    a64_rr(&e, 0xaa000000u, 19, 31, 0);
    if (e.loop_done) {
        a64_mov(&e, 22, 1);
        a64_mov(&e, 23, 31);
        a64_cpu_addr(&e, 25, offsetof(arm920t_t, irq_line));
        /* x24 = the {generation, epoch} pair the self backedge verifies. */
        a64_cpu_mem(&e, 0xf9400000u, 24, offsetof(arm920t_t, jit_generation), 3);
    }
    if (e.memory) {
        /* x20 = &c->jit_tlb_page[0] for the whole block. */
        size_t va = offsetof(arm920t_t, jit_tlb_page);
        if (va <= 4095u)
            a64_u32(&e, 0x91000000u | ((uint32_t)va << 10) | (19u << 5) | 20u);
        else
            a64_cpu_addr(&e, 20, va);
    }
    if (e.memory)
        a64_cpu_mem(&e, 0xf9400000u, 21, offsetof(arm920t_t, jit_ram_base), 3);
    /* The dispatcher normally guarantees this. Keep the function itself safe
     * for short budgets too, leaving the partial block to portable bytecode. */
    a64_cmp_imm(&e, 1, b->count);
    /* The dispatcher normally guarantees a full block budget. Its rare
     * rejection leaves through the cold entry exit behind the block body, so
     * no never-executed return sequence sits in front of the hot code. */
    e.entry_exit = a64_jump(&e, 3); /* LO: too short a budget leaves through the cold exit */
    e.loop_head = e.pos;
    for (unsigned i = 0; i < b->count; ++i) {
        const arm_jit_op_t *op = &arm_jit_ops(c, b)[i];
        e.expected_next = i + 1u < b->count ? arm_jit_ops(c, b)[i + 1u].pc : op->pc + 4u;
        e.generation = b->generation;
        /* A register-only leaf preserves its BL link and execution state.
         * Keep the architectural PC visible to subsequent callbacks, but a
         * decoded caller successor needs no dynamic return-target guard. The
         * guest op/cycle count and guarded boundary returns remain intact. */
        if (op->reserved == 7u && i + 1u < b->count) {
            a64_store_imm(&e, offsetof(arm920t_t, r) + 15u * 4u, e.expected_next);
            continue;
        }
        if (op->reserved == 8u) {
            if (!e.nzcv_valid) {
                a64_load(&e, 3, offsetof(arm920t_t, cpsr));
                a64_u32(&e, 0xd51b4203u);
            }
            size_t taken = a64_jump(&e, op->cond);
            a64_store_imm(&e, offsetof(arm920t_t, r) + 15u * 4u, op->pc + 4u);
            a64_return(&e, i + 1u);
            a64_patch(&e, taken, e.pos);
            e.nzcv_valid = 0;
            continue;
        }
        size_t skip = 0;
        if (op->cond == 15u) {
            /* ARMv4 NV is never, unlike A64's condition 15 (always). */
            e.nzcv_valid = 0;
            a64_store_imm(&e, offsetof(arm920t_t, r) + 15u * 4u, op->pc + 4u);
        } else {
            if (op->cond != 14u) {
                /* An immediately preceding unconditional arithmetic flag
                 * producer already left precisely these flags in NZCV. */
                if (!e.nzcv_valid) {
                    a64_load(&e, 3, offsetof(arm920t_t, cpsr));
                    a64_u32(&e, 0xd51b4203u);
                }
                skip = a64_jump(&e, op->cond ^ 1u);
            }
            if (!a64_one(&e, op, i + 1u)) a64_slow(&e, op, i + 1u);
            if (skip) {
                a64_patch(&e, skip, e.pos);
                e.nzcv_valid = 0; /* taken/skipped paths join here */
            }
            /* PC remains materialized at exits/helpers and taken branches,
             * as on the original path. Taken edges returned or re-entered. */
        }
        if (op->stop) {
            a64_store_imm(&e, offsetof(arm920t_t, r) + 15u * 4u, op->pc + 4u);
            a64_return(&e, i + 1u);
            break;
        }
    }
    /* The last op can itself be a stitched branch at the block-size limit. */
    const arm_jit_op_t *last = &arm_jit_ops(c, b)[b->count - 1u];
    uint32_t final_pc = last->pc + 4u;
    if (last->kind == ARM_JIT_OP_BRANCH && last->cond == 14u)
        final_pc = arm_jit_branch_target(last->pc, last->insn);
    a64_store_imm(&e, offsetof(arm920t_t, r) + 15u * 4u, final_pc);
    a64_ret_flush(&e, b->count);
    e.hot_end = e.pos;
    if (e.entry_exit) {
        a64_patch(&e, e.entry_exit, e.pos);
        e.in_cold = 1;
        a64_ret_done(&e, 0);
        a64_ret_tail(&e);
        e.in_cold = 0;
    }
    a64_cold_flush(&e);
    if (e.fail) return;
    size_t hot_bytes = e.hot_end * sizeof(tmp[0]);
    size_t cold_bytes = (e.pos - e.hot_end) * sizeof(tmp[0]);
    if (!c->jit_code) {
        c->jit_code_size = ARM_JIT_CODE_SIZE;
        c->jit_code = arm_jit_alloc_exec(c->jit_code_size);
        if (!c->jit_code) { c->jit_code_size = c->jit_code_used = 0; ARM_PROF_INC(c, jit_code_alloc_failures); return; }
        c->jit_cold_floor = 0;
    }
    size_t start = (c->jit_code_used + 15u) & ~(size_t)15u, cold_at;
    for (;;) {
        size_t cs = start & ~(size_t)(ARM_JIT_A64_CHUNK - 1u);
        size_t ce = c->jit_code_size - cs > ARM_JIT_A64_CHUNK ? cs + ARM_JIT_A64_CHUNK : c->jit_code_size;
        size_t floor = c->jit_cold_floor;
        if (floor <= cs || floor > ce) floor = ce;
        if (start <= floor && cold_bytes <= floor - start && hot_bytes <= floor - cold_bytes - start) {
            cold_at = floor - cold_bytes;
            c->jit_cold_floor = cold_at;
            break;
        }
        if (ce >= c->jit_code_size) {
            /* Full: make the next translation recycle the arena. */
            c->jit_code_used = c->jit_code_size;
            ARM_PROF_INC(c, jit_code_full_events);
            return;
        }
        start = ce;
    }
    uint32_t *hot_dst = (uint32_t *)(void *)(c->jit_code + start);
    uint32_t *cold_dst = (uint32_t *)(void *)(c->jit_code + cold_at);
    if (!a64_place(hot_dst, cold_dst, tmp, e.hot_end, e.pos)) {
        c->jit_cold_floor = cold_at + cold_bytes;
        return;
    }
    arm_jit_flush_exec(hot_dst, hot_bytes);
    if (cold_bytes) arm_jit_flush_exec(cold_dst, cold_bytes);
    c->jit_code_used = start + hot_bytes;
    void *dst = hot_dst;
    union { void *p; arm_jit_native_fn f; } cvt;
    cvt.p = dst;
    b->native = cvt.f;
    b->native_ok = 1;
}

/* -------------------------------------------------------------------------
 * Appended fast paths.  Each is reached from a64_one and returns 0 to keep the
 * classified helper whenever its compile-time shape is not covered.
 * ---------------------------------------------------------------------- */

/* Register-shifted operand2 for ARM data processing (insn bit25=0, bit4=1).
 *
 * Mirrors arm_bc_shifter_operand's register-shift branch exactly.  Rm (op->e)
 * and Rs (op->f) are compile-time register numbers that a64_data_regshift
 * rejects when 15, because read_r substitutes the pipeline PC (the helper runs
 * with c->r[15] = pc+4).  The runtime amount is Rs & 0xff, i.e. 0..255:
 *   LSL/LSR: amount 0 keeps the value, 1..31 shifts, >= 32 yields 0;
 *   ASR:     amount 0 keeps the value, 1..31 shifts, >= 32 sign-fills;
 *   ROR:     amount & 31, so 0 and 32 both keep the value.
 * A zero amount preserves the incoming C flag for every type (the C code's
 * reg_shift rule), which differs from the immediate form's per-type rule.
 *
 * In : w6 = Rm value, w7 = Rs & 0xff, w3 = CPSR (read only for the carry).
 * Out: w1 = operand2, and when carry is requested w4 = carry-out at C_FLAG.
 * Only w0-w7 plus the a64_* helpers' w16 scratch are used.
 */
static void a64_regshift_op2(a64_emit_t *e, unsigned type, int carry) {
    size_t hi, join;
    switch (type) {
    case 0u: /* LSL */
        a64_cmp_imm(e, 7, 32);
        hi = a64_jump(e, 2);                     /* HS: amt >= 32 -> 0 */
        a64_u32(e, 0x1ac720c1u);                 /* lslv w1, w6, w7 */
        join = a64_jump(e, 14);
        a64_patch(e, hi, e->pos);
        a64_imm(e, 1, 0);
        a64_patch(e, join, e->pos);
        break;
    case 1u: /* LSR */
        a64_cmp_imm(e, 7, 32);
        hi = a64_jump(e, 2);
        a64_u32(e, 0x1ac724c1u);                 /* lsrv w1, w6, w7 */
        join = a64_jump(e, 14);
        a64_patch(e, hi, e->pos);
        a64_imm(e, 1, 0);
        a64_patch(e, join, e->pos);
        break;
    case 2u: /* ASR */
        a64_cmp_imm(e, 7, 32);
        hi = a64_jump(e, 2);
        a64_u32(e, 0x1ac728c1u);                 /* asrv w1, w6, w7 */
        join = a64_jump(e, 14);
        a64_patch(e, hi, e->pos);
        a64_u32(e, 0x131f7cc1u);                 /* asr w1, w6, #31: sign fill */
        a64_patch(e, join, e->pos);
        break;
    default: /* ROR: rorv already reduces the register amount mod 32 */
        a64_u32(e, 0x1ac72cc1u);                 /* rorv w1, w6, w7 */
        break;
    }
    if (!carry) return;
    /* Carry-out at bit 29; amount 0 keeps the incoming C (w3 bit 29). */
    a64_cmp_imm(e, 7, 0);
    size_t zero = a64_jump(e, 0);                /* EQ: amt == 0 -> cin */
    if (type == 0u) {                            /* LSL: bit(v, (-amt) & 31) */
        a64_cmp_imm(e, 7, 32);
        size_t big = a64_jump(e, 8);             /* HI: amt > 32 -> 0 */
        a64_u32(e, 0x4b0703e5u);                 /* sub w5, wzr, w7 */
        a64_mask(e, 5, 5, 31u);
        a64_u32(e, 0x1ac524c5u);                 /* lsrv w5, w6, w5 */
        a64_mask(e, 5, 5, 1u);
        size_t mid = a64_jump(e, 14);
        a64_patch(e, big, e->pos);
        a64_imm(e, 5, 0);
        a64_patch(e, mid, e->pos);
    } else if (type == 1u) {                     /* LSR: bit(v, amt-1) */
        a64_cmp_imm(e, 7, 32);
        size_t big = a64_jump(e, 8);             /* HI: amt > 32 -> 0 */
        a64_u32(e, 0x510004e5u);                 /* sub w5, w7, #1 */
        a64_u32(e, 0x1ac524c5u);                 /* lsrv w5, w6, w5 */
        a64_mask(e, 5, 5, 1u);
        size_t mid = a64_jump(e, 14);
        a64_patch(e, big, e->pos);
        a64_imm(e, 5, 0);
        a64_patch(e, mid, e->pos);
    } else if (type == 2u) {                     /* ASR: bit(v, amt-1) or bit 31 */
        a64_cmp_imm(e, 7, 32);
        size_t big = a64_jump(e, 2);             /* HS: amt >= 32 -> bit 31 */
        a64_u32(e, 0x510004e5u);                 /* sub w5, w7, #1 */
        a64_u32(e, 0x1ac524c5u);                 /* lsrv w5, w6, w5 */
        a64_mask(e, 5, 5, 1u);
        size_t mid = a64_jump(e, 14);
        a64_patch(e, big, e->pos);
        a64_ubfx(e, 5, 6, 31, 1);
        a64_patch(e, mid, e->pos);
    } else {                                     /* ROR: top bit of the result */
        a64_ubfx(e, 5, 1, 31, 1);
    }
    size_t over = a64_jump(e, 14);
    a64_patch(e, zero, e->pos);
    a64_ubfx(e, 5, 3, 29, 1);
    a64_patch(e, over, e->pos);
    a64_lsl(e, 4, 5, 29);
}

/* ARM_JIT_OP_DATA with a register-shifted operand2.  The opcode, flag and
 * PC-write structure is a64_data's; that emitter refuses this subset outright.
 * Rm/Rs == 15 keeps the classified helper's pipeline-PC read. Flag-setting
 * PC writes use the dedicated restore helper after native ALU/flags. */
static int a64_data_regshift(a64_emit_t *e, const arm_jit_op_t *op, uint32_t done) {
    unsigned code = op->a;
    int test = code >= 8u && code <= 11u;
    int flags = test || (op->d & ARM_BC_DATA_S);
    int logical = code < 2u || code == 8u || code == 9u || code >= 12u;
    if (!(op->d & ARM_BC_DATA_REGSHIFT) || op->e == 15u || op->f == 15u) return 0;
    if (!test && op->c == 15u && flags && e->leaf) return 0;
    a64_guest(e, 0, op->b, op->pc);                        /* w0 = Rn */
    a64_guest(e, 6, op->e, op->pc);                        /* w6 = Rm */
    a64_guest(e, 7, op->f, op->pc);                        /* w7 = Rs */
    a64_mask(e, 7, 7, 0xffu);                              /* amount = Rs & 0xff */
    if (flags || (code >= 5u && code <= 7u))
        a64_load(e, 3, offsetof(arm920t_t, cpsr));
    a64_regshift_op2(e, op->g, flags && logical);
    uint32_t s = flags ? 0x20000000u : 0u;
    switch (code) {
    case 0: case 8: a64_rr(e, 0x0a000000u, 2, 0, 1); break;
    case 1: case 9: a64_rr(e, 0x4a000000u, 2, 0, 1); break;
    case 2: case 10: a64_rr(e, 0x4b000000u | s, 2, 0, 1); break;
    case 3: a64_rr(e, 0x4b000000u | s, 2, 1, 0); break;
    case 4: case 11: a64_rr(e, 0x0b000000u | s, 2, 0, 1); break;
    case 5: case 6: case 7:
        a64_u32(e, 0xd51b4203u); /* msr nzcv, x3: ARM/A64 carry is no-borrow */
        a64_rr(e, (code == 5u ? 0x1a000000u : 0x5a000000u) | s,
               2, code == 7u ? 1u : 0u, code == 7u ? 0u : 1u);
        break;
    case 12: a64_rr(e, 0x2a000000u, 2, 0, 1); break;
    case 13: a64_mov(e, 2, 1); break;
    case 14: a64_rr(e, 0x0a200000u, 2, 0, 1); break;
    case 15: a64_rr(e, 0x2a200000u, 2, 31, 1); break;
    }
    if (flags) {
        if (logical) a64_cmp_imm(e, 2, 0);
        a64_u32(e, 0xd53b4205u); /* mrs x5, nzcv */
        if (logical) {
            a64_mask(e, 5, 5, N_FLAG | Z_FLAG);
            a64_rr(e, 0x2a000000u, 5, 5, 4);
        }
        a64_bfxil_low(e, 5, 3, logical ? 29u : 28u);
        a64_store(e, 5, offsetof(arm920t_t, cpsr));
    }
    if (!test) {
        if (op->c == 15u && flags) {
            a64_exception_return(e, 2, done);
            return 1;
        }
        if (op->c == 15u) a64_mask(e, 2, 2, ~3u);
        a64_put_guest(e, op->c, 2);
        if (op->c == 15u) a64_return(e, done);
    }
    if (!test && op->cond == 14u && op->c != 15u) e->fwd_guest = (int)op->c;
    return 1;
}

/* ARM_JIT_OP_MUL long form (bit23): UMULL/SMULL/UMLAL/SMLAL.  op_mul forms a
 * 64-bit product from the sign- or zero-extended source words, adds the
 * RdHi:RdLo pair for the accumulate forms, stores RdLo first and RdHi second,
 * and for S sets N from RdHi bit 31 and Z from the whole 64-bit result while
 * preserving C/V.  Any operand register 15 keeps the helper, which reads the
 * pipeline PC through c->r[15]. */
static int a64_long_mul(a64_emit_t *e, const arm_jit_op_t *op, uint32_t done) {
    const uint32_t insn = op->insn;
    unsigned rdhi = (insn >> 16) & 0xfu, rdlo = (insn >> 12) & 0xfu,
             rs = (insn >> 8) & 0xfu, rm = insn & 0xfu;
    int smul = !!(insn & (1u << 22)), accum = !!(insn & (1u << 21));
    if (!(insn & (1u << 23)) || rdhi == 15u || rdlo == 15u || rs == 15u || rm == 15u) return 0;
    a64_guest(e, 0, rm, op->pc);                           /* w0 = Rm */
    a64_guest(e, 1, rs, op->pc);                           /* w1 = Rs */
    if (accum) {
        a64_guest(e, 4, rdlo, op->pc);                     /* x4 = RdHi:RdLo */
        a64_guest(e, 5, rdhi, op->pc);
        a64_u32(e, 0xaa058084u);                           /* orr x4, x4, x5, lsl #32 */
    }
    /* smaddl/umaddl extend w0*w1 and add x4 (x31 reads as zero). */
    a64_u32(e, (smul ? 0x9b200000u : 0x9ba00000u) |
            ((accum ? 4u : 31u) << 10) | (1u << 5) | 2u);
    a64_u32(e, 0xd360fc43u);                               /* lsr x3, x2, #32 */
    a64_put_guest(e, rdlo, 2);
    a64_put_guest(e, rdhi, 3);
    if (insn & (1u << 20)) {
        a64_load(e, 5, offsetof(arm920t_t, cpsr));
        a64_rr(e, 0x2a000000u, 6, 2, 3);                   /* w6 = RdLo | RdHi */
        a64_cmp_imm(e, 6, 0);                              /* Z: 64-bit result zero */
        a64_ubfx(e, 6, 3, 31, 1);                          /* N = RdHi bit 31 */
        a64_lsl(e, 6, 6, 31);
        a64_u32(e, 0xd53b4207u);                           /* mrs x7, nzcv */
        a64_mask(e, 7, 7, Z_FLAG);
        a64_rr(e, 0x2a000000u, 6, 6, 7);
        a64_bfxil_low(e, 6, 5, 30u);                      /* C/V and control kept */
        a64_store(e, 6, offsetof(arm920t_t, cpsr));
    }
    GP32_UNUSED(done);
    return 1;
}

/* Local p15 operations have no bus callback or control-flow effects. Reads
 * mirror cp15_read; allowed MCR data-cache/drain operations only record c7,
 * matching cp15_write. I-cache/MMU maintenance retains the checked helper. */
static int a64_local_p15(a64_emit_t *e, const arm_jit_op_t *op, uint32_t done) {
    const uint32_t insn = op->insn;
    if (!arm_jit_is_local_cp15_op(insn)) return 0;
    unsigned crn = (insn >> 16) & 0xfu, rd = (insn >> 12) & 0xfu;
    if (!GP32_BIT(insn, 20)) {
        a64_guest(e, 1, rd, op->pc);
        a64_store(e, 1, offsetof(arm920t_t, cp15) + crn * 4u);
    } else if (crn == 0u) a64_store_imm(e, offsetof(arm920t_t, r) + rd * 4u, 0x41129200u);
    else {
        a64_load(e, 1, offsetof(arm920t_t, cp15) + crn * 4u);
        a64_put_guest(e, rd, 1);
    }
    GP32_UNUSED(done);
    return 1;
}

/* A zero offset denotes a mode without SPSR. Resolve the bank at execution
 * time: decoded blocks can be reused in different exception modes. */
static const uint32_t a64_spsr_offsets[32] = {
    [MODE_FIQ] = offsetof(arm920t_t, spsr_fiq),
    [MODE_SVC] = offsetof(arm920t_t, spsr_svc),
    [MODE_ABT] = offsetof(arm920t_t, spsr_abt),
    [MODE_IRQ] = offsetof(arm920t_t, spsr_irq),
    [MODE_UND] = offsetof(arm920t_t, spsr_und),
};

/* Return the bank offset in w3, preserving w1/w2 for the value and mask. */
static void a64_spsr_offset(a64_emit_t *e) {
    a64_load(e, 3, offsetof(arm920t_t, cpsr));
    a64_mask(e, 3, 3, 31u);
    a64_lsl(e, 3, 3, 2u);
    a64_ptr(e, 4, (uintptr_t)a64_spsr_offsets);
    a64_u32(e, 0xb8636883u); /* ldr w3, [x4, x3] */
}

/* SPSR access cannot change the active mode or interrupt masks. Terminating
 * CPSR writes can keep same-mode updates native. */
static int a64_psr_mov(a64_emit_t *e, const arm_jit_op_t *op, uint32_t done) {
    const uint32_t insn = op->insn;
    unsigned rd = (insn >> 12) & 0xfu;
    if ((insn & 0x0fbf0fffu) == 0x010f0000u) {             /* MRS */
        if (rd == 15u) return 0;
        a64_load(e, 1, offsetof(arm920t_t, cpsr));
        if (insn & (1u << 22)) {
            a64_spsr_offset(e);
            a64_cmp_imm(e, 3, 0u);
            size_t no_spsr = a64_jump(e, 0u);
            a64_u32(e, 0xb8636a61u); /* ldr w1, [x19, x3] */
            a64_patch(e, no_spsr, e->pos); /* op_psr returns CPSR without a bank */
        }
        a64_put_guest(e, rd, 1);
        GP32_UNUSED(done);
        return 1;
    }
    if ((insn & 0x0db0f000u) == 0x0120f000u) {             /* MSR fields, ... */
        unsigned field = (insn >> 16) & 0xfu;
        if (insn & (1u << 22)) {
            uint32_t mask = 0u;
            for (unsigned byte = 0; byte < 4u; ++byte)
                if (field & (1u << byte)) mask |= 0xffu << (byte * 8u);
            if (!mask) mask = 0xf0000000u; /* existing op_psr zero-field behavior */
            if (insn & (1u << 25))
                a64_imm(e, 1, gp32_ror32(insn & 0xffu, ((insn >> 8) & 0xfu) * 2u));
            else a64_guest(e, 1, insn & 0xfu, op->pc);
            a64_mask(e, 1, 1, mask);
            a64_spsr_offset(e);
            a64_cmp_imm(e, 3, 0u);
            size_t no_spsr = a64_jump(e, 0u); /* absent bank: no write */
            if (mask != UINT32_MAX) {
                a64_u32(e, 0xb8636a62u); /* ldr w2, [x19, x3] */
                a64_mask(e, 2, 2, ~mask);
                a64_rr(e, 0x2a000000u, 1, 1, 2);
            }
            a64_u32(e, 0xb8236a61u); /* str w1, [x19, x3] */
            a64_patch(e, no_spsr, e->pos);
            return 1;
        }
        if (field != 0x8u) {
            /* PSR operations terminate decoded blocks. Require that proof
             * here even if a future decoder starts stitching across them:
             * mode, Thumb and interrupt-mask changes must reach dispatch. */
            if (!op->stop || e->leaf) return 0;
            uint32_t mask = 0u;
            for (unsigned byte = 0; byte < 4u; ++byte)
                if (field & (1u << byte)) mask |= 0xffu << (byte * 8u);
            if (!mask) mask = 0xf0000000u;
            if (insn & (1u << 25))
                a64_imm(e, 1, gp32_ror32(insn & 0xffu, ((insn >> 8) & 0xfu) * 2u));
            else a64_guest(e, 1, insn & 0xfu, op->pc);
            a64_imm(e, 2, mask);
            /* Keep w1/w2 intact for the bank-switch fallback. The effective
             * mask comes from the live mode, never the block's compile mode. */
            a64_load(e, 3, offsetof(arm920t_t, cpsr));
            a64_rr(e, 0x4a000000u, 5, 1, 3);
            size_t switch_bank = 0;
            if (mask & MODE_MASK) {
                /* Reject a different source mode before preparing the live
                 * privilege mask. USER writes that request another mode also
                 * take the shared helper, which enforces flags-only access. */
                a64_mask(e, 6, 5, MODE_MASK);
                switch_bank = e->pos;
                a64_u32(e, 0x35000006u); /* cbnz w6 -> shared bank-switch helper */
            }
            unsigned effective_mask = 2u;
            if (mask & 0x00ffffffu) {
                a64_mask(e, 6, 3, MODE_MASK);
                a64_cmp_imm(e, 6, MODE_USR);
                a64_mask(e, 4, 2, 0xff000000u);
                a64_u32(e, 0x1a820084u); /* csel w4, w4, w2, EQ: USER flags only */
                effective_mask = 4u;
            }
            /* delta = (operand ^ old CPSR) & effective mask. XORing it back
             * preserves every unselected bit, including zero-field NZCV. */
            a64_rr(e, 0x0a000000u, 5, 5, effective_mask);
            a64_rr(e, 0x4a000000u, 3, 3, 5);
            a64_store(e, 3, offsetof(arm920t_t, cpsr));
            if (mask & MODE_MASK) {
                size_t updated = a64_jump(e, 14u);
                a64_patch(e, switch_bank, e->pos);
                a64_rr(e, 0xaa000000u, 0, 31, 19);
                a64_ptr(e, 16, (uintptr_t)arm_a64_msr_cpsr);
                a64_u32(e, 0xd63f0200u); /* blr x16: original operand and mask */
                a64_patch(e, updated, e->pos);
            }
            e->nzcv_valid = 0; /* compare/helper flags are not guest NZCV */
            /* Both paths keep the caller's stop epilogue: commit PC, return,
             * then let dispatch observe Thumb and pending IRQ/FIQ changes. */
            return 1;
        }
        if (insn & (1u << 25)) {                           /* immediate form */
            uint32_t v = gp32_ror32(insn & 0xffu, ((insn >> 8) & 0xfu) * 2u);
            a64_load(e, 1, offsetof(arm920t_t, cpsr));
            a64_imm(e, 2, v & 0xff000000u);
        } else {                                           /* register form */
            unsigned rm = insn & 0xfu;
            if (rm == 15u) return 0;
            a64_guest(e, 2, rm, op->pc);
            a64_load(e, 1, offsetof(arm920t_t, cpsr));
        }
        a64_bfxil_low(e, 2, 1, 24u);                      /* top byte from source */
        a64_store(e, 2, offsetof(arm920t_t, cpsr));
        GP32_UNUSED(done);
        return 1;
    }
    return 0;
}
