/* AArch64 backend for the decoded ARMv4T blocks in arm920t.c.
 *
 * Every block runs inside one host frame that the shared rt_enter stub builds
 * when the dispatcher calls a block, and that rt_exit tears down when control
 * returns to it.  Inside that frame all blocks share one register convention
 * (x0-x17 are scratch, x18 is never touched):
 *   x19  CPU pointer
 *   x20  &c->jit_tlb_page[0]
 *   x21  c->jit_ram_base (a helper that changes it must leave the chain)
 *   w22  entry budget plus ops skipped in the current block (modulo 2^32)
 *   w23  earlier completed ops minus current-block skips (modulo 2^32)
 *   x24  {jit_generation, jit_cache_epoch} at the entry of the current block
 *   x25  &c->irq_line, the base of the CPU control fields
 *   x26-x29  write-through copies of up to four guest registers that the
 *        current block reads most; c->r[] stays authoritative and current
 * A RAM-page hit loads its tag and VA-to-RAM XOR with one word pair.
 * Ordinary ops commit guest data registers and CPSR after every instruction;
 * proved callback-free RAM loops may batch a bounded number of iterations.
 * PC is materialized at exits and by the classified helper before callbacks.
 * Slow instructions use the classified helper and validate its exit state:
 * exceptions, MMIO callbacks, mode changes and cache invalidation therefore
 * cannot continue executing an obsolete trace. No guest cycles are elided.
 *
 * Block linking: an exit whose path ran only native operations and helpers
 * that validated their continuation (no halt, trace, Thumb, mode, budget,
 * cache or RAM-base change, no deliverable interrupt) calls rt_hop with the
 * aligned next PC.  rt_hop applies the dispatcher's own fast-path acceptance
 * to the first way of that PC's set: no IRQ/FIQ line raised, tag and valid,
 * {generation, epoch} equal to the live pair, a compiled block that is neither
 * a counted poll nor a deferred inline target, and a remaining budget that
 * covers it.  Any rejection returns through rt_exit, so the dispatcher sees
 * exactly the state it would have seen after the first block and repeats the
 * same decision in C.  Every other exit returns through rt_exit directly.
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
/* Shared runtime stubs at the start of the arena: emitted with the arena and
 * never recycled, so block code reaches them with direct branches. */
#define A64_RT_ENTER 0u   /* byte offsets */
#define A64_RT_EXIT  64u
#define A64_RT_HOP   128u
#define A64_RT_BYTES 512u
/* Block prologue before the body: bti, budget cmp, b.lo, frame stp, bl. */
#define A64_BODY_WORDS 5u
/* Host registers holding cached guest registers (all callee-saved). */
#if defined(__APPLE__)
/* Apple requires x29 to remain a valid frame pointer even between calls.
 * Keep the dispatcher's frame chain intact while helpers run or a profiler
 * samples native code. A saved/restored guest value in x29 is not sufficient. */
#define A64_CACHE_REGS 3u
static const uint8_t a64_cache_pool[A64_CACHE_REGS] = {26u, 27u, 28u};
#else
#define A64_CACHE_REGS 4u
static const uint8_t a64_cache_pool[A64_CACHE_REGS] = {26u, 27u, 28u, 29u};
#endif
enum {
    A64_HELPER_ARM_JIT_EXEC_SWI_NATIVE,
    A64_HELPER_ARM_JIT_EXEC_CLASSIFIED,
    A64_HELPER_ARM_A64_OBSERVE_CHECKED,
    A64_HELPER_ARM_A64_EXEC_CHECKED,
    A64_HELPER_ARM_A64_LDWORD_CHECKED,
    A64_HELPER_ARM_A64_MEM_CHECKED,
    A64_HELPER_ARM_A64_IO_WORD_CHECKED,
    A64_HELPER_ARM_A64_IO_WORD_MAPPED_CHECKED,
    A64_HELPER_ARM_JIT_EXCEPTION_RETURN_HELPER,
    A64_HELPER_ARM_A64_MSR_CPSR,
    A64_HELPER_COUNT
};

enum { A64_EXT_EXIT = 0, A64_EXT_HOP, A64_EXT_ENTER };
#define A64_EXT_MAX (ARM_JIT_MAX_INSNS * 8u + 8u)
/* Compile-time switch for linked exits (rt_hop); 0 returns every exit. */
#ifndef ARM_JIT_A64_LINK
#define ARM_JIT_A64_LINK 1
#endif
_Static_assert(sizeof(arm_jit_block_t) * ARM_JIT_BLOCK_WAYS == 64u &&
               offsetof(arm_jit_block_t, tag_pc) == 0u && offsetof(arm_jit_block_t, valid) == 4u &&
               offsetof(arm_jit_block_t, generation) == 8u && offsetof(arm_jit_block_t, native) == 24u,
               "rt_hop indexes 64-byte sets and reads the block header as word pairs");

typedef struct a64_fetch_fix {
    uint32_t pc, pa, done;
    size_t branch, cont;
} a64_fetch_fix_t;

#include "arm920t_jit_a64_reloc.h"

#define A64_RELOC_MAX (ARM_JIT_MAX_INSNS * 4u)
_Static_assert(ARM_JIT_NATIVE_MAX_BYTES / sizeof(uint32_t) <= A64_RELOC_MAX_WORDS,
               "packed relocation source index fits");

typedef struct a64_emit {
    uint32_t *code;
    size_t pos, cap;
    a64_reloc_t *reloc; /* crossing edges only; runtime stubs use NULL */
    unsigned reloc_count; /* UINT_MAX: bounded records overflowed, scan scratch */
    int fail;
    int ram_read, ram_write, mmu, check_code_writes;
    int hop;         /* clean exits may link to a resident successor */
    int counted;     /* shape has per-load native observation support */
    /* Guest register -> host register holding a write-through copy, or 0.
     * Fixed for the whole block: every path keeps each copy equal to c->r[]
     * (stores through a64_put_guest update it, helpers reload it), so a copy
     * stays valid across conditional skips, cold chains and the backedge. */
    uint8_t cache_host[16];
    const uint32_t (*helper_code)[6];
    int bios_read;   /* jit_bios_base probes the whole read-only BIOS window */
    int open_bus;    /* bus promises open_bus_word32 outside every window */
    int io_read_callback, io_write_callback; /* proved no fastmem at device PAs */
    int nzcv_valid; /* host NZCV equals the committed guest flags */
    int fwd_guest;  /* guest register whose committed value is still in w2, -1 = none.
                     * A plain data op publishes it for the next data op or a
                     * pre-indexed store without writeback. Other emitters and
                     * all helper/cold continuations clear it. */
    uint32_t expected_next, generation;
    int want_nzcv;  /* the next decoded op is conditional */
    uint32_t loop_done, block_count;
    size_t loop_head;
    const arm920t_t *leaf_sample;
    const arm_leaf_write32_t *leaf_write32;
    size_t leaf_write32_count;
    unsigned leaf_write32_mask;
    const arm_live_read32_t *live_read32;
    size_t live_read32_count;
    struct a64_cold_op *cold; /* deferred cold chains, one per fast-path op */
    unsigned cold_count, cold_cap;
    a64_fetch_fix_t *fetch;
    unsigned fetch_count;
    unsigned ext_count;
    size_t entry_exit;        /* hot budget-check branch into the cold entry exit */
    size_t hot_end;           /* words [0, hot_end) are the hot body and epilogue */
    /* Appended fixups are initialized individually before ext_count grows. */
    uint32_t ext_at[A64_EXT_MAX]; /* B/BL sites that target a runtime stub */
    uint8_t ext_to[A64_EXT_MAX];  /* A64_EXT_* */
} a64_emit_t;

enum a64_cold_kind {
    A64_COLD_SINGLE_DT = 1, A64_COLD_HALF, A64_COLD_BLOCK_DT,
    /* The block-transfer two-page retry. It is entered from the hot page-cross
     * branch, re-runs the translation and the transfer for a span that crosses
     * one 4 KiB boundary, and resumes the hot continuation behind it. Its own
     * guard rejections join the sibling classified-helper chain. */
    A64_COLD_BLOCK_DT_XPAGE, A64_COLD_BX_THUMB
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
    uint16_t fixup[16]; /* bounded scratch-word indices, including cold XPAGE guards */
    unsigned sibling; /* chain that owns this chain's guard rejections (0 = none) */
} a64_cold_op_t;
_Static_assert(A64_RELOC_MAX_WORDS - 1u <= UINT16_MAX,
               "cold fixup indices fit the bounded scratch buffer");

ARM_FORCE_INLINE int arm_a64_can_continue(const arm920t_t *c, int stop,
                                         uint32_t expected_next, uint32_t generation,
                                         uint32_t status, uint32_t epoch, uint32_t run_limit,
                                         const uint8_t *ram_base) {
    /* All native bus/SWI callbacks pass these checked helpers. A reduced
     * deadline leaves after this instruction, before any native backedge can
     * reuse w22's old budget. Dispatch resumes with the new remaining budget;
     * callback-free chains retain their existing fast guards. */
    if (stop) return 0;
    /* These read-only identity checks all require equality. Combine their
     * differences before the control checks instead of branching on each. */
    uint32_t changed = (c->run_limit ^ run_limit) | (c->jit_generation ^ generation) |
                       (c->jit_cache_epoch ^ epoch) | ((c->cpsr & 0xffu) ^ status) |
                       ((c->r[15] & ~3u) ^ expected_next);
    /* All operands are ordinary CPU fields. Evaluate the same predicates
     * together so successful MMIO returns do not traverse a branch per field.
     * Normalize every int: callbacks are not required to write only 0 or 1. */
    return (changed == 0u) & !c->halted & !thumb(c) & !c->trace & !!c->jit_enabled &
           (c->jit_ram_base == ram_base) &
           (!c->irq_line | !!(c->cpsr & I_FLAG)) &
           (!c->fiq_line | !!(c->cpsr & F_FLAG)) &
           (!(c->cp15[1] & 1u) | (c->jit_fetch_page != UINT32_MAX));
}

static int arm_a64_exec_checked(arm920t_t *c, const arm_jit_op_t *op,
                                uint32_t expected_next, uint32_t generation) {
    int stop = op->stop;
    uint32_t status = c->cpsr & 0xffu, epoch = c->jit_cache_epoch, run_limit = c->run_limit;
    uint8_t *ram_base = c->jit_ram_base;
    arm_jit_exec_classified(c, op);
    return arm_a64_can_continue(c, stop, expected_next, generation, status, epoch, run_limit, ram_base);
}

/* The portable observer queries stability before committing PC+4 and
 * performing the real load. Keep that order and the checked helper's fence. */
static int arm_a64_observe_checked(arm920t_t *c, const arm_jit_op_t *op,
                                   uint32_t expected_next, uint32_t generation) {
    int stop = op->stop;
    uint32_t status = c->cpsr & 0xffu, epoch = c->jit_cache_epoch, run_limit = c->run_limit;
    uint8_t *ram_base = c->jit_ram_base;
    if (c->jit_counted_observing && c->jit_counted_stable) {
        c->r[15] = op->pc;
        if (!arm_poll_read_stable(c, op)) c->jit_counted_stable = 0;
    }
    /* The emitter selects this helper only for SINGLE_DT. Share its actual
     * transfer and instruction prelude without duplicating other opcodes. */
    arm_profile_helper_op(c, op);
    if (arm_jit_begin_classified(c, op, op->pc, op->insn)) op_single_dt_bc(c, op);
    int next = arm_a64_can_continue(c, stop, expected_next, generation, status, epoch, run_limit, ram_base);
    if (!next) c->jit_counted_stable = 0;
    return next;
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
 * performed. Generic buses retain their complete physical fastmem probe;
 * certified buses skip it. Cycle and timing state stay untouched exactly as there, and an
 * I/O write keeps the identical exit condition (interrupts, timers, halt,
 * trace, epoch/generation, RAM rebase).  Address and value are handed over
 * already computed: pa is 4-byte aligned and inside that window, rotation is
 * (VA & 3) * 8, writeback is base +/- offset (used only when the decoded
 * shape writes back).  No profiling counter is advanced: like the RAM
 * fast path this is a native fast path, not a checked-helper invocation. */
ARM_FORCE_INLINE int arm_a64_io_word_access(arm920t_t *c, const arm_jit_op_t *op,
                                            uint32_t pa, uint32_t value, uint32_t writeback,
                                            uint32_t rotation, uint32_t expected_next,
                                            uint32_t generation, int probe_fastmem) {
    int stop = op->stop;
    unsigned rd = op->b, rn = op->a; /* callbacks can invalidate decoded-op storage */
    int load = !!(op->d & ARM_BC_SD_L);
    int wb = !(op->d & ARM_BC_SD_P) || (op->d & ARM_BC_SD_W);
    uint32_t status = c->cpsr & 0xffu, epoch = c->jit_cache_epoch, run_limit = c->run_limit;
    uint8_t *ram_base = c->jit_ram_base;
    c->r[15] = op->pc + 4u; /* callback-visible PC, exactly as classified */
    uint8_t *direct = probe_fastmem ? fastmem(c, pa, 4u, !load) : NULL;
    if (load) {
        /* A generic bus may approve direct memory inside the device window.
         * Preserve the complete span and independent read/write permission. */
        uint32_t v = direct ? gp32_ld32le(direct) :
            (c->bus.read32_io ? c->bus.read32_io : c->bus.read32)(c->bus.user, pa);
        if (rotation) v = gp32_ror32(v, rotation);
        write_r(c, rd, v);
    } else {
        if (direct) gp32_st32le(direct, value);
        else (c->bus.write32_io ? c->bus.write32_io : c->bus.write32)(c->bus.user, pa, value);
        arm920t_cpu_code_write(c, pa, 4u);
    }
    if (wb) write_r(c, rn, writeback);
    return arm_a64_can_continue(c, stop, expected_next, generation, status, epoch, run_limit, ram_base);
}

static int arm_a64_io_word_checked(arm920t_t *c, const arm_jit_op_t *op,
                                   uint32_t pa, uint32_t value, uint32_t writeback,
                                   uint32_t rotation, uint32_t expected_next,
                                   uint32_t generation) {
    return arm_a64_io_word_access(c, op, pa, value, writeback, rotation,
                                 expected_next, generation, 0);
}

static int arm_a64_io_word_mapped_checked(arm920t_t *c, const arm_jit_op_t *op,
                                          uint32_t pa, uint32_t value, uint32_t writeback,
                                          uint32_t rotation, uint32_t expected_next,
                                          uint32_t generation) {
    return arm_a64_io_word_access(c, op, pa, value, writeback, rotation,
                                 expected_next, generation, 1);
}

static void a64_u32(a64_emit_t *e, uint32_t v) {
    if (e->pos < e->cap) e->code[e->pos++] = v;
    else e->fail = 1;
}

static int a64_logical_imm32(uint32_t mask, uint32_t *encoding);

/* Call only for PC-relative B/BL, B.cond, CBZ/CBNZ and TBZ/TBNZ. */
static void a64_branch_word(a64_emit_t *e, uint32_t word) {
    a64_u32(e, word);
}
static void a64_imm(a64_emit_t *e, unsigned rd, uint32_t v) {
    if (!(v & 0xffffu))
        a64_u32(e, 0x52a00000u | ((v >> 16) << 5) | rd); /* movz W, lsl #16 */
    else if ((v >> 16) == 0xffffu)
        a64_u32(e, 0x12800000u | ((~v & 0xffffu) << 5) | rd); /* movn W */
    else if ((v & 0xffffu) == 0xffffu)
        a64_u32(e, 0x12a00000u | ((~v >> 16) << 5) | rd); /* movn W, lsl #16 */
    else {
        uint32_t encoding;
        /* Keep one-word MOVZ constants cheap; reuse the existing bitmask
         * encoder only when it can replace two move-wide instructions. All
         * W forms clear the upper half and preserve host condition flags. */
        if ((v >> 16) && a64_logical_imm32(v, &encoding))
            a64_u32(e, 0x320003e0u | encoding | rd); /* orr W, wzr, #imm */
        else if (!(v >> 16))
            a64_u32(e, 0x52800000u | ((v & 0xffffu) << 5) | rd); /* movz W */
        else if (e->pos <= e->cap && e->cap - e->pos >= 2u) {
            /* The two-word MOVZ+MOVK pair has room: write both words and
             * publish the final position once. Near the buffer end, retain
             * the original partial-write/failure path. */
            e->code[e->pos] = 0x52800000u | ((v & 0xffffu) << 5) | rd; /* movz W */
            e->code[e->pos + 1u] = 0x72a00000u | ((v >> 16) << 5) | rd; /* movk W, lsl #16 */
            e->pos += 2u;
        } else {
            a64_u32(e, 0x52800000u | ((v & 0xffffu) << 5) | rd); /* movz W */
            a64_u32(e, 0x72a00000u | ((v >> 16) << 5) | rd); /* movk W, lsl #16 */
        }
    }
}

static void a64_ptr(a64_emit_t *e, unsigned rd, uintptr_t v) {
    /* The common path has room for all four possible move-wide words.
     * Write through one local cursor, then publish its final position once.
     * Near the buffer end, retain the original partial-write/failure path. */
    if (e->pos <= e->cap && e->cap - e->pos >= 4u) {
        uint32_t *out = e->code + e->pos;
        *out++ = 0xd2800000u | (((uint32_t)v & 0xffffu) << 5) | rd;
        for (unsigned i = 1; i < 4; ++i)
            if ((v >> (16u * i)) & 0xffffu)
                *out++ = 0xf2800000u | (i << 21) |
                         ((uint32_t)((v >> (16u * i)) & 0xffffu) << 5) | rd;
        e->pos = (size_t)(out - e->code);
        return;
    }

    a64_u32(e, 0xd2800000u | (((uint32_t)v & 0xffffu) << 5) | rd);
    for (unsigned i = 1; i < 4; ++i)
        if ((v >> (16u * i)) & 0xffffu)
            a64_u32(e, 0xf2800000u | (i << 21) |
                    ((uint32_t)((v >> (16u * i)) & 0xffffu) << 5) | rd);
}

static void a64_init_helpers(arm920t_t *c) {
    if (c->jit_helpers_ready) return;
    const uintptr_t helpers[] = {
        (uintptr_t)arm_jit_exec_swi_native,
        (uintptr_t)arm_jit_exec_classified,
        (uintptr_t)arm_a64_observe_checked,
        (uintptr_t)arm_a64_exec_checked,
        (uintptr_t)arm_a64_ldword_checked,
        (uintptr_t)arm_a64_mem_checked,
        (uintptr_t)arm_a64_io_word_checked,
        (uintptr_t)arm_a64_io_word_mapped_checked,
        (uintptr_t)arm_jit_exception_return_helper,
        (uintptr_t)arm_a64_msr_cpsr
    };
    _Static_assert(sizeof(c->jit_helper_code) / sizeof(c->jit_helper_code[0]) == A64_HELPER_COUNT,
                   "one instruction template per fixed host helper");
    for (unsigned i = 0; i < A64_HELPER_COUNT; ++i) {
        a64_emit_t tmp;
        tmp.code = c->jit_helper_code[i] + 1;
        tmp.pos = 0;
        tmp.cap = 5;
        tmp.fail = 0;
        a64_ptr(&tmp, 16, helpers[i]);
        a64_u32(&tmp, 0xd63f0200u); /* blr x16, same as per-block emission */
        c->jit_helper_code[i][0] = (uint32_t)tmp.pos;
    }
    c->jit_helpers_ready = 1;
}

static void a64_helper_call(a64_emit_t *e, unsigned id) {
    const uint32_t *code = e->helper_code[id];
    size_t n = code[0];
    if (e->pos <= e->cap && n <= e->cap - e->pos) {
        memcpy(e->code + e->pos, code + 1, n * sizeof(*code));
        e->pos += n;
    } else {
        for (size_t i = 0; i < n; ++i) a64_u32(e, code[i + 1u]);
    }
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
    if (off >= control &&
        !((off - control) & ((1u << scale) - 1u)) && ((off - control) >> scale) <= 4095u) {
        /* These fields follow the large TLB arrays. x25 anchors them for
         * every block, avoiding repeated address materialization while still
         * loading each current IRQ/cache/control value at every guard. */
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
    else if (e->cache_host[r]) a64_mov(e, rt, e->cache_host[r]);
    else a64_load(e, rt, offsetof(arm920t_t, r) + r * 4u);
}
/* Keep a cached copy equal to a guest register that was just written from rt
 * by a store that does not pass a64_put_guest. */
static void a64_cache_sync(a64_emit_t *e, unsigned r, unsigned rt) {
    if (r < 15u && e->cache_host[r] && e->cache_host[r] != rt)
        a64_mov(e, e->cache_host[r], rt);
}
static void a64_put_guest(a64_emit_t *e, unsigned r, unsigned rt) {
    a64_store(e, rt, offsetof(arm920t_t, r) + r * 4u);
    a64_cache_sync(e, r, rt);
}
/* A helper that continues may have written any guest register: refresh every
 * cached copy from c->r[] before native code reads one again. */
static void a64_cache_reload(a64_emit_t *e) {
    for (unsigned r = 0; r < 15u; ++r)
        if (e->cache_host[r]) a64_load(e, e->cache_host[r], offsetof(arm920t_t, r) + r * 4u);
}

/* Return packed immr/imms fields only; leave *encoding unchanged on failure. */
static int a64_logical_imm32(uint32_t mask, uint32_t *encoding) {
    if (!mask || mask == UINT32_MAX) return 0;
    /* Rotation by a power-of-two width preserves exactly the masks made by
     * repeating that element. Find the smallest period without rebuilding
     * the 32-bit mask for every candidate width. */
    unsigned width = 32u;
    for (unsigned w = 2u; w < 32u; w <<= 1)
        if (((mask << w) | (mask >> (32u - w))) == mask) {
            width = w;
            break;
        }
    uint32_t element_mask = UINT32_MAX >> (32u - width);
    uint32_t element = mask & element_mask;
    uint32_t previous = ((element << 1) | (element >> (width - 1u))) & element_mask;
    uint32_t starts = element & ~previous;
    /* A larger repeating element only multiplies its circular transitions;
     * it cannot turn a rejected minimal period into a single run of ones. */
    if (!starts || (starts & (starts - 1u))) return 0;
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
    a64_branch_word(e, cond == 14u ? 0x14000000u : (0x54000000u | cond));
    return p;
}
/* hot_end is bound before any deferred cold chain is emitted. At that
 * point all hot-local labels are already patched; only a hot/cold crossing
 * needs a relocation record. No array clear or per-branch bitmap is needed.
 * On overflow retain the original contiguous scratch for the scanner. */
static void a64_record_reloc(a64_emit_t *e, size_t at, size_t target, unsigned kind) {
    if (e->fail || !e->hot_end || (at < e->hot_end) == (target < e->hot_end)) return;
    if (e->reloc_count >= A64_RELOC_MAX) { e->reloc_count = UINT_MAX; return; }
    a64_reloc_t *r = &e->reloc[e->reloc_count++];
    r->at_kind = (uint16_t)(at | (kind << A64_RELOC_INDEX_BITS));
    r->target = (uint16_t)target;
}
static void a64_patch(a64_emit_t *e, size_t at, size_t target) {
    if (e->fail || at >= e->pos || target < at || target - at >= (1u << 18)) {
        e->fail = 1;
        return;
    }
    uint32_t delta = (uint32_t)(target - at);
    if ((e->code[at] & 0xfc000000u) == 0x14000000u) {
        e->code[at] |= delta;
    } else if ((e->code[at] & 0x7e000000u) == 0x36000000u) {
        if (delta >= (1u << 13)) { e->fail = 1; return; }
        e->code[at] |= delta << 5; /* tbz/tbnz: imm14 */
    } else {
        e->code[at] |= delta << 5;
    }
}
/* Emit an unconditional backward branch to an already emitted position. */
static void a64_branch_back(a64_emit_t *e, size_t target) {
    if (e->fail || target > e->pos || e->pos - target >= (1u << 25)) { e->fail = 1; return; }
    size_t at = e->pos;
    a64_branch_word(e, 0x14000000u | ((uint32_t)(target - at) & 0x03ffffffu));
}

/* Deferred entries may also receive a cold-to-cold XPAGE retry. Keep the
 * split check for those sites; local patching elsewhere does no bookkeeping. */
static void a64_patch_cold(a64_emit_t *e, size_t at, size_t target) {
    a64_patch(e, at, target);
    if (e->fail || at >= e->hot_end) return;
    uint32_t word = e->code[at];
    unsigned kind = (word & 0xfc000000u) == 0x14000000u ? A64_RELOC_B26 :
                    (word & 0x7e000000u) == 0x36000000u ? A64_RELOC_B14 : A64_RELOC_B19;
    a64_record_reloc(e, at, target, kind);
}
static void a64_branch_hot(a64_emit_t *e, size_t target) {
    size_t at = e->pos;
    a64_branch_back(e, target);
    a64_record_reloc(e, at, target, A64_RELOC_B26);
}
/* Record one cold chain; the hot body has already reached its continuation. */
static a64_cold_op_t *a64_cold_open(a64_emit_t *e, const arm_jit_op_t *op,
                                    uint32_t done, unsigned kind) {
    if (e->fail || e->cold_count >= e->cold_cap) { e->fail = 1; return NULL; }
    a64_cold_op_t *c = &e->cold[e->cold_count++];
    /* Keep every zero default the full-struct memset provided except fixup[]:
     * a64_cold_fix fills slots 0..nfixup-1 in order and a64_cold_flush reads
     * exactly that prefix, so the array is written before it is ever read. */
    memset(c, 0, offsetof(a64_cold_op_t, fixup));
    memset(&c->sibling, 0, sizeof(*c) - offsetof(a64_cold_op_t, sibling));
    c->op = op;
    c->cont = e->pos;
    c->done = done;
    c->expected_next = e->expected_next;
    c->generation = e->generation;
    c->kind = (uint8_t)kind;
    return c;
}
/* Defer one branch site to its chain entry (patched when the chain is
 * emitted).  Overflow is unreachable with the per-emitter fixup counts and
 * fails closed if a future emitter ever exceeds them. */
static void a64_cold_fix(a64_emit_t *e, a64_cold_op_t *c, size_t at) {
    if (e->fail || !c) return;
    if (c->nfixup >= sizeof(c->fixup) / sizeof(c->fixup[0]) ||
        at >= A64_RELOC_MAX_WORDS) { e->fail = 1; return; }
    c->fixup[c->nfixup++] = (uint16_t)at;
}
static void a64_cmp(a64_emit_t *e, unsigned rn, unsigned rm) {
    a64_rr(e, 0x6b000000u, 31, rn, rm);
}
static void a64_cmp_imm(a64_emit_t *e, unsigned rn, unsigned imm) {
    a64_u32(e, 0x7100001fu | (imm << 10) | (rn << 5));
}
/* A B or BL to a runtime stub, linked after the block is placed. */
static void a64_ext(a64_emit_t *e, uint32_t opcode, unsigned to) {
    if (e->fail || e->ext_count >= A64_EXT_MAX) { e->fail = 1; return; }
    e->ext_at[e->ext_count] = (uint32_t)e->pos;
    e->ext_to[e->ext_count++] = (uint8_t)to;
    /* The external link pass owns this placeholder. It has no target within
     * tmp, so it needs no ordinary hot/cold relocation record. */
    a64_u32(e, opcode);
}

/* Leave the call: w0 = ops completed by this block, rt_exit adds w23. */
static void a64_return(a64_emit_t *e, uint32_t done) {
    a64_imm(e, 0, done);
    a64_ext(e, 0x14000000u, A64_EXT_EXIT); /* b rt_exit */
}

/* A clean exit with w16 = aligned next PC, already committed to r15. rt_hop
 * returns the successor body in x17 when the dispatcher would run that block
 * next on its fast path, otherwise rt_exit. Each site keeps its own indirect
 * branch, so the host predicts the usual successor of every exit. */
static void a64_hop(a64_emit_t *e, uint32_t done) {
    if (!e->hop) { a64_return(e, done); return; }
    a64_imm(e, 0, done);
    a64_ext(e, 0x94000000u, A64_EXT_HOP); /* bl rt_hop */
    a64_u32(e, 0xd61f0220u);              /* br x17 */
}

/* Clean exit to a constant PC. */
static void a64_exit_const(a64_emit_t *e, uint32_t pc, uint32_t done) {
    a64_imm(e, 16, pc);
    a64_store(e, 16, offsetof(arm920t_t, r) + 15u * 4u);
    a64_hop(e, done);
}

/* Clean exit to the PC a native op has just committed from guest register
 * value w(reg); the dispatcher looks up that value with its low bits clear. */
static void a64_hop_reg(a64_emit_t *e, unsigned reg, uint32_t done) {
    if (!e->hop) { a64_return(e, done); return; }
    a64_mask(e, 16, reg, ~3u);
    a64_hop(e, done);
}

/* Only the first explicitly eligible entry backedge chains. A full decoded
 * block must fit again; a short remainder returns to portable dispatch.
 * Guards run after the real B retires and before accounting/re-entry. Thus
 * every rejected edge returns completed prefixes + this partial prefix; w22
 * and w23 are only advanced once the next pass is accepted. */
/* Residency checks have no bus access or TLB mutation. Only a complete
 * live 1KiB span may update the fetch certificate without dispatcher work. */
static void a64_fetch_resident(a64_emit_t *e, uint32_t pc, uint32_t pa, uint32_t done) {
    size_t off = offsetof(arm920t_t, itlb_entry) +
                 arm_itlb_index(pc) * sizeof(arm_tlb_entry_t);
    a64_load(e, 16, off + offsetof(arm_tlb_entry_t, valid));
    a64_cmp_imm(e, 16, 0);
    size_t invalid = a64_jump(e, 0);
    a64_load(e, 16, off + offsetof(arm_tlb_entry_t, mask));
    a64_mask(e, 17, 16, ARM_JIT_PAGE_MASK);
    a64_cmp_imm(e, 17, ARM_JIT_PAGE_MASK);
    size_t partial = a64_jump(e, 1);
    a64_imm(e, 17, pc);
    a64_rr(e, 0x0a200000u, 15, 17, 16); /* bic w15,w17,w16 */
    a64_load(e, 16, off + offsetof(arm_tlb_entry_t, va_base));
    a64_cmp(e, 15, 16);
    size_t conflict = a64_jump(e, 1);
    a64_load(e, 16, off + offsetof(arm_tlb_entry_t, mask));
    a64_imm(e, 17, pc);
    a64_rr(e, 0x0a000000u, 15, 17, 16); /* and w15,w17,w16 */
    a64_load(e, 16, off + offsetof(arm_tlb_entry_t, pa_base));
    a64_rr(e, 0x2a000000u, 15, 15, 16);
    a64_imm(e, 17, pa);
    a64_cmp(e, 15, 17);
    size_t remapped = a64_jump(e, 1);
    a64_store_imm(e, offsetof(arm920t_t, jit_fetch_page), pc >> 10);
    size_t resident = a64_jump(e, 14);
    a64_patch(e, invalid, e->pos);
    a64_patch(e, partial, e->pos);
    a64_patch(e, conflict, e->pos);
    a64_patch(e, remapped, e->pos);
    a64_store_imm(e, offsetof(arm920t_t, jit_fetch_page), UINT32_MAX);
    a64_store_imm(e, offsetof(arm920t_t, r) + 15u * 4u, pc);
    a64_return(e, done);
    a64_patch(e, resident, e->pos);
}

/* Keep an already certified page on the contiguous hot path. Residency
 * checks retain the original instruction sequence in the cold area. */
static void a64_fetch_guard(arm920t_t *c, a64_emit_t *e, uint32_t pc, uint32_t done) {
    if (e->fail || e->fetch_count >= ARM_JIT_MAX_INSNS) { e->fail = 1; return; }
    uint32_t pa, mask;
    if (!arm_jit_peek_translate(c, pc, &pa, &mask)) { e->fail = 1; return; }
    a64_load(e, 16, offsetof(arm920t_t, jit_fetch_page));
    a64_imm(e, 17, pc >> 10);
    a64_cmp(e, 16, 17);
    size_t branch = a64_jump(e, 1);
    e->fetch[e->fetch_count++] = (a64_fetch_fix_t){pc, pa, done, branch, e->pos};
    e->nzcv_valid = 0;
    e->fwd_guest = -1;
}

static void a64_fetch_flush(a64_emit_t *e) {
    for (unsigned i = 0; i < e->fetch_count && !e->fail; ++i) {
        const a64_fetch_fix_t *f = &e->fetch[i];
        a64_patch_cold(e, f->branch, e->pos);
        a64_fetch_resident(e, f->pc, f->pa, f->done);
        a64_branch_hot(e, f->cont);
    }
}

static void a64_selfloop(a64_emit_t *e, uint32_t done, uint32_t target) {
    /* A repetition reaches this edge only through direct native ops and
     * checked helpers. Every helper that resumes native code has already
     * required, via arm_a64_can_continue, the compiled generation, the entry
     * cache epoch, an unchanged mode/IRQ/FIQ mask byte, run limit and RAM base,
     * and no deliverable IRQ/FIQ. Direct ops cannot change any of them, so a
     * callback-free repetition leaves the state the block was entered with.
     * The pass fits when the budget left after this prefix still covers the
     * full block: remaining - done >= count, compared without changing w22.
     * PC is needed only when control leaves the block. */
    a64_cmp_imm(e, 22, done + e->block_count);
    size_t leave = a64_jump(e, 3); /* LO */
    a64_u32(e, 0x51000000u | (done << 10) | (22u << 5) | 22u); /* sub w22, w22, #done */
    a64_u32(e, 0x11000000u | (done << 10) | (23u << 5) | 23u); /* add w23, w23, #done */
    /* Dedicated signed imm26 backedge; existing forward fixups stay intact. */
    if (e->loop_head > e->pos || e->pos - e->loop_head >= (1u << 25)) e->fail = 1;
    a64_branch_word(e, 0x14000000u | ((uint32_t)(e->loop_head - e->pos) & 0x03ffffffu));
    a64_patch(e, leave, e->pos);
    a64_imm(e, 16, target);
    a64_store(e, 16, offsetof(arm920t_t, r) + 15u * 4u);
    a64_return(e, done);
}
static void a64_slow(a64_emit_t *e, const arm_jit_op_t *op, uint32_t done) {
    a64_rr(e, 0xaa000000u, 0, 31, 19);
    a64_ptr(e, 1, (uintptr_t)op);
    if (op->kind == ARM_JIT_OP_SWI) {
        a64_helper_call(e, A64_HELPER_ARM_JIT_EXEC_SWI_NATIVE);
        a64_return(e, done);
        return;
    }
    if (op->stop || op->kind == ARM_JIT_OP_UNDEFINED ||
        (op->kind == ARM_JIT_OP_COPROC && op->reserved != 6u)) {
        /* This path always returns to dispatch. The classified operation
         * commits callback/exception state; continuation snapshots and checks
         * cannot affect the decision to leave this block. */
        a64_helper_call(e, A64_HELPER_ARM_JIT_EXEC_CLASSIFIED);
        a64_return(e, done);
        return;
    }
    a64_imm(e, 2, e->expected_next);
    a64_imm(e, 3, e->generation);
    a64_helper_call(e, e->counted && op->kind == ARM_JIT_OP_SINGLE_DT ?
        A64_HELPER_ARM_A64_OBSERVE_CHECKED : A64_HELPER_ARM_A64_EXEC_CHECKED);
    a64_cmp_imm(e, 0, 0);
    size_t next = a64_jump(e, 1);
    a64_return(e, done);
    a64_patch(e, next, e->pos);
    a64_cache_reload(e);
}

/* w12 retains the original VA before alignment/TLB/range guards; w2 retains
 * writeback. Neither is clobbered by a64_ram_offset. The access helper owns
 * the load/store and commits; no guest state is changed before this call. */
static void a64_mem_slow(a64_emit_t *e, const arm_jit_op_t *op, uint32_t done,
                          int load, int wb) {
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
    a64_helper_call(e, word_load ?
        A64_HELPER_ARM_A64_LDWORD_CHECKED : A64_HELPER_ARM_A64_MEM_CHECKED);
    a64_cmp_imm(e, 0, 0);
    size_t next = a64_jump(e, 1);
    a64_return(e, done);
    a64_patch(e, next, e->pos);
    a64_cache_reload(e);
}

/* Establish w0 as the aligned physical word using the authoritative TLB.
 * Failed proofs skip all physical-window probes and reach the checked helper.
 * Live-word, read-only and I/O probes share this proof: no rejected window
 * test or read-only success runs a callback before that decision. Preserve
 * w12 (original VA), w2 (writeback), and w11 (unaligned load rotation). */
static void a64_word_pa(a64_emit_t *e, size_t *cold, unsigned *ncold) {
    a64_mask(e, 3, 12, ~3u);  /* the checked path translates the aligned word */
    if (e->mmu) {
        _Static_assert(sizeof(arm_tlb_entry_t) == 16u &&
                       offsetof(arm_tlb_entry_t, pa_base) == 4u &&
                       offsetof(arm_tlb_entry_t, mask) == 8u &&
                       offsetof(arm_tlb_entry_t, valid) == 12u,
                       "cold word probe packed TLB layout");
        /* Read the authoritative TLB; never fill it or widen a restored mask. */
        a64_ubfx(e, 6, 3, 12, 12);
        a64_cpu_addr(e, 8, offsetof(arm920t_t, tlb_entry));
        a64_rr(e, 0x8b001000u, 8, 8, 6); /* add x8, x8, x6, lsl #4 */
        a64_u32(e, 0x29400000u | (9u << 10) | (8u << 5) | 7u); /* ldp w7, w9, [x8]: VA, PA */
        a64_u32(e, 0x29410000u | (6u << 10) | (8u << 5) | 5u); /* ldp w5, w6, [x8, #8]: mask, valid */
        a64_branch_word(e, 0x34000000u | 6u); /* cbz w6, cold */
        cold[(*ncold)++] = e->pos - 1u;
        a64_rr(e, 0x0a200000u, 6, 3, 5); /* bic w6, w3, w5: outside the mapped page? */
        a64_cmp(e, 6, 7);
        cold[(*ncold)++] = a64_jump(e, 1); /* NE */
        a64_rr(e, 0x0a000000u, 6, 3, 5); /* and w6, w3, w5 */
        a64_rr(e, 0x2a000000u, 0, 6, 9); /* orr w0, w6, w9: physical word address */
    } else a64_mov(e, 0, 3);
}

/* Direct transfer after a64_word_pa has proved the aligned physical word.
 * Every out-of-window address still reaches the original checked helper;
 * arm_a64_io_word_checked owns the callback, commits and continuation fence. */
static void a64_io_word_pa(a64_emit_t *e, const arm_jit_op_t *op, uint32_t done,
                           int load, int wb, size_t *cold, unsigned *ncold, size_t cont) {
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
    int callback_only = load ? e->io_read_callback : e->io_write_callback;
    a64_helper_call(e, callback_only ?
        A64_HELPER_ARM_A64_IO_WORD_CHECKED : A64_HELPER_ARM_A64_IO_WORD_MAPPED_CHECKED);
    size_t keep = e->pos;
    a64_branch_word(e, 0x35000000u); /* cbnz w0: commits are done, may continue */
    a64_return(e, done);
    a64_patch(e, keep, e->pos);
    a64_cache_reload(e);
    a64_branch_hot(e, cont);
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
    a64_helper_call(e, A64_HELPER_ARM_JIT_EXCEPTION_RETURN_HELPER);
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
    /* Write a cached destination directly. Its old value remains available
     * to operand selection until the ALU instruction retires. Uncached and
     * PC destinations retain w2 and the existing one-instruction forwarding. */
    unsigned result = !test && op->c < 15u && e->cache_host[op->c] ? e->cache_host[op->c] : 2u;
    int direct_mov = code == 13u && !flags && !(op->d & ARM_BC_DATA_IMM) && !op->g && !op->h;
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
    /* A cached guest register is read straight from its host copy. The
     * second operand qualifies only while it reaches the ALU unchanged. */
    if (load_rn && op->b < 15u && e->cache_host[op->b]) {
        load_rn = 0;
        if (rm == rn && !load_rm) rm = e->cache_host[op->b];
        rn = e->cache_host[op->b];
    }
    if (load_rm && plain_op2 && op->e < 15u && e->cache_host[op->e]) {
        load_rm = 0;
        rm = e->cache_host[op->e];
    }
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
        if (!arith_imm && !logic_op) a64_imm(e, code == 13u ? result : 1u, op->imm);
        if (flags && logical) {
            if (op->d & ARM_BC_DATA_IMM_CV) a64_imm(e, 4, (op->d & ARM_BC_DATA_IMM_C) ? C_FLAG : 0u);
            else a64_mask(e, 4, 3, C_FLAG);
        }
    } else {
        if (load_rm) {
            /* A plain MOV has no second input or shifter side effect. Load
             * directly into its result, including the PC+8 pipeline value. */
            if (direct_mov) { a64_guest(e, result, op->e, op->pc); rm = result; }
            else a64_guest(e, 1, op->e, op->pc);
        }
        if (!folded_shift) a64_shift(e, op->g, op->h, flags && logical);
    }
    uint32_t s = flags ? 0x20000000u : 0u;
    /* and/eor/orr result, <first operand or wzr>, #imm */
    if (logic_op)
        a64_u32(e, logic_op | logic_enc |
                    ((code == 13u || code == 15u ? 31u : rn) << 5) | result);
    else switch (code) {
    case 0: case 8: a64_rr(e, 0x0a000000u | shift_bits, result, rn, rm); break;
    case 1: case 9: a64_rr(e, 0x4a000000u | shift_bits, result, rn, rm); break;
    case 2: case 10:
        if (arith_imm) a64_u32(e, 0x51000000u | s | imm_bits | (rn << 5) | result);
        else a64_rr(e, 0x4b000000u | s | shift_bits, result, rn, rm);
        break;
    case 3: a64_rr(e, 0x4b000000u | s, result, rm, rn); break;
    case 4: case 11:
        if (arith_imm) a64_u32(e, 0x11000000u | s | imm_bits | (rn << 5) | result);
        else a64_rr(e, 0x0b000000u | s | shift_bits, result, rn, rm);
        break;
    case 5: case 6: case 7:
        a64_u32(e, 0xd51b4203u); /* msr nzcv, x3: ARM/A64 carry is no-borrow */
        a64_rr(e, (code == 5u ? 0x1a000000u : 0x5a000000u) | s,
               result, code == 7u ? rm : rn, code == 7u ? rn : rm);
        break;
    case 12: a64_rr(e, 0x2a000000u | shift_bits, result, rn, rm); break;
    case 13:
        if (!(op->d & ARM_BC_DATA_IMM) && (!direct_mov || rm != result))
            a64_rr(e, 0x2a000000u | shift_bits, result, 31, rm);
        break;
    case 14: a64_rr(e, 0x0a200000u | shift_bits, result, rn, rm); break;
    case 15: a64_rr(e, 0x2a200000u | shift_bits, result, 31, rm); break;
    }
    if (flags) {
        if (logical) a64_cmp_imm(e, result, 0);
        a64_u32(e, 0xd53b4205u); /* mrs x5, nzcv */
        if (logical) {
            a64_mask(e, 5, 5, N_FLAG | Z_FLAG);
            a64_rr(e, 0x2a000000u, 5, 5, 4);
        }
        a64_bfxil_low(e, 5, 3, logical ? 29u : 28u);
        a64_store(e, 5, offsetof(arm920t_t, cpsr));
        /* Logical flags merge the old C/V; when the next op is conditional,
         * hand it the merged flags in NZCV instead of a CPSR reload. */
        if (logical && e->want_nzcv) a64_u32(e, 0xd51b4205u); /* msr nzcv, x5 */
        e->nzcv_valid = !logical || e->want_nzcv;
    }
    if (!test) {
        if (op->c == 15u && flags) {
            /* Keep ALU flags for bankless USER/SYS before restoring SPSR. */
            a64_exception_return(e, 2, done);
            return 1;
        }
        if (op->c == 15u) a64_mask(e, 2, 2, ~3u);
        a64_put_guest(e, op->c, result);
        if (op->c == 15u) {
            if (op->reserved == ARM_JIT_RETURN_GUARDED) {
                /* Retire the real MOV pc,lr; an unexpected LR exits with its
                 * actual aligned PC instead of following a stale caller. */
                a64_imm(e, 6, e->expected_next);
                a64_cmp(e, 2, 6);
                size_t matched = a64_jump(e, 0); /* EQ */
                a64_hop_reg(e, 2, done);
                a64_patch(e, matched, e->pos);
            } else a64_hop_reg(e, 2, done);
        }
    }
    /* An unconditional plain register write leaves its committed value in w2
     * for the next op; a conditional op's fall-through reaches the same store
     * without executing it, so only cond==14 may publish the result. */
    if (!test && result == 2u && op->cond == 14u && op->c != 15u) e->fwd_guest = (int)op->c;
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

static void a64_code_watch_base(a64_emit_t *e) {
    size_t off = offsetof(arm920t_t, jit_code_watches) - offsetof(arm920t_t, irq_line);
    if (off <= 0xfffu)
        a64_u32(e, 0x91000000u | ((uint32_t)off << 10) | (25u << 5) | 17u);
    else a64_cpu_addr(e, 17, offsetof(arm920t_t, jit_code_watches));
}

/* A proved, aligned STM fragment contains at most 16 instruction words.
 * The padded bitmap permits one 64-bit window covering any such range,
 * including one ending at the final RAM word. count=0 takes its length from
 * count_reg; both dynamic fragments of a two-page transfer have 1..15 words.
 * Only x6/x7/x17 are scratch, preserving both physical offsets and w5's split. */
static void a64_guard_code_words(a64_emit_t *e, size_t *slow, unsigned *nslow,
                                  unsigned offset, unsigned count, unsigned count_reg) {
    if (!e->check_code_writes) return;
    a64_code_watch_base(e);
    a64_ubfx(e, 6, offset, 7u, 25u);
    a64_rr(e, 0x8b000800u, 6, 17, 6); /* x6 = bitmap + (offset >> 7) * 4 */
    a64_u32(e, 0xf9400000u | (6u << 5) | 7u); /* ldr x7,[x6], including next bitmap word */
    a64_ubfx(e, 6, offset, 2u, 5u);
    a64_rr(e, 0x9ac02400u, 7, 7, 6); /* lsr x7,x7,x6 */
    if (count) {
        a64_mask(e, 7, 7, (1u << count) - 1u);
    } else {
        a64_imm(e, 6, 1u);
        a64_rr(e, 0x1ac02000u, 6, 6, count_reg); /* lsl w6,w6,count */
        a64_u32(e, 0x51000400u | (6u << 5) | 6u); /* sub w6,w6,#1 */
        a64_rr(e, 0x0a000000u, 7, 7, 6);
    }
    slow[(*nslow)++] = e->pos;
    a64_branch_word(e, 0x35000000u | 7u);
}

/* w9 is a proved physical RAM offset. Reject watched-span stores before any
 * guest mutation and let the checked helper finish the instruction before
 * its epoch fence leaves this trace. Keep w0/w2/w11 for its cold operands.
 * Control writes flush native blocks, so no guard is emitted with CP15.I=1. */
static void a64_guard_code_write(a64_emit_t *e, size_t *slow, unsigned *nslow,
                                  unsigned last_delta) {
    if (!e->check_code_writes) return;
    if (last_delta >= 4u) {
        a64_guard_code_words(e, slow, nslow, 9u, last_delta / 4u + 1u, 0u);
        return;
    }
    a64_code_watch_base(e);
    for (unsigned last = 0; last < (last_delta ? 2u : 1u); ++last) {
        unsigned offset = 9u;
        if (last) {
            a64_u32(e, 0x11000000u | (last_delta << 10) | (9u << 5) | 16u);
            offset = 16u;
        }
        a64_ubfx(e, 6, offset, ARM_JIT_CODE_WATCH_SHIFT + 5u, 32u - ARM_JIT_CODE_WATCH_SHIFT - 5u);
        a64_u32(e, 0xb8605800u | (6u << 16) | (17u << 5) | 7u); /* ldr w7,[x17,w6,uxtw #2] */
        a64_ubfx(e, 6, offset, ARM_JIT_CODE_WATCH_SHIFT, 5u);
        a64_rr(e, 0x1ac02400u, 7, 7, 6); /* lsr w7,w7,w6 */
        a64_mask(e, 7, 7, 1u);
        slow[(*nslow)++] = e->pos;
        a64_branch_word(e, 0x35000000u | 7u); /* cbnz w7 -> checked instruction */
    }
}

/* Only the already translated, aligned physical endpoint is selected.
 * The original VA alignment is tested separately. No-writeback word stores
 * need no guest state retained in caller-saved registers over the call. */
/* Entry values only choose whether extra cold code is worth emitting.
 * They never authorize an access: the generated path still proves the live
 * mapping/alignment/endpoint/watch on EVERY execution. A missed prediction
 * retains the ordinary checked helper, including after a base value changes.
 * No table walk or bus callback may be performed for this size heuristic. */
static int a64_leaf_write32_likely(const a64_emit_t *e, const arm_jit_op_t *op) {
    if (!e->leaf_write32_mask || (op->d & ARM_BC_SD_REGOFF) || op->a == 15u) return 0;
    const arm920t_t *c = e->leaf_sample;
    uint32_t va = c->r[op->a];
    va = (op->d & ARM_BC_SD_U) ? va + op->imm : va - op->imm;
    if (va & 3u) return 0;
    uint32_t pa = va;
    if (e->mmu) {
        const arm_tlb_entry_t *t = &c->tlb_entry[(va >> 12) & 0xfffu];
        if (!t->valid || (va & ~t->mask) != t->va_base) return 0;
        pa = t->pa_base | (va & t->mask);
    }
    for (size_t i = 0; i < e->leaf_write32_count; ++i)
        if ((e->leaf_write32_mask & (1u << i)) && pa == e->leaf_write32[i].pa) return 1;
    return 0;
}

static void a64_leaf_write32_pa(a64_emit_t *e, const arm_jit_op_t *op, size_t cont) {
    if (!a64_leaf_write32_likely(e, op)) return;
    a64_mask(e, 6, 12, 3u);
    a64_cmp(e, 6, 31);
    size_t unaligned = a64_jump(e, 1);
    size_t code_watch = SIZE_MAX;
    if (e->check_code_writes) {
        /* A later non-RAM fetch can add this watch after compilation.
         * Retain the ordinary write/invalidation fence for that case. */
        a64_cpu_mem(e, 0x39400000u, 6, offsetof(arm920t_t, jit_code_nonram), 0);
        code_watch = e->pos;
        a64_branch_word(e, 0x37080006u); /* tbnz w6, #1 -> checked path */
    }
    for (size_t i = 0; i < e->leaf_write32_count && !e->fail;) {
        const arm_leaf_write32_t *w = &e->leaf_write32[i];
        size_t end = i + 1u;
        while (end < e->leaf_write32_count &&
               e->leaf_write32[end].write == w->write &&
               e->leaf_write32[end].user == w->user) ++end;
        size_t matched[8];
        unsigned nm = 0;
        for (size_t j = i; j < end; ++j) {
            if (!(e->leaf_write32_mask & (1u << j))) continue;
            a64_imm(e, 6, e->leaf_write32[j].pa);
            a64_cmp(e, 0, 6);
            matched[nm++] = a64_jump(e, 0);
        }
        i = end;
        if (!nm) continue;
        size_t unmatched = e->pos;
        a64_branch_word(e, 0x14000000u);
        for (unsigned j = 0; j < nm; ++j) a64_patch(e, matched[j], e->pos);
        a64_mov(e, 1, 0);
        a64_guest(e, 2, op->b, op->pc + 4u);
        a64_imm(e, 6, op->pc + 4u);
        a64_store(e, 6, offsetof(arm920t_t, r) + 15u * 4u);
        a64_ptr(e, 0, (uintptr_t)w->user);
        a64_ptr(e, 16, (uintptr_t)w->write);
        a64_u32(e, 0xd63f0200u);
        a64_branch_hot(e, cont);
        a64_patch(e, unmatched, e->pos);
    }
    a64_patch(e, unaligned, e->pos);
    if (code_watch != SIZE_MAX) a64_patch(e, code_watch, e->pos);
}

/* A contiguous STR/LDR/AND/ORR/STR region can amortize physical endpoint
 * proofs only through the existing peripheral-only write and live-read
 * contracts. Both writes remain separate callbacks, and the live word is
 * loaded after the first one. No callback may change CPU state, a deadline,
 * a mapping or executable memory, so the second store's first proof remains
 * valid. Every rejected guard falls into the original five native ops below.
 * The original ops also remain entry points for in-block forward branches.
 *
 * The shared base and disjoint data registers make both addresses invariant.
 * Descriptor offsets choose an economical candidate, never authorize a bus
 * access: each current VA is translated and checked independently at runtime.
 * No guest address, firmware bytes, title or fixed PC selects this region. */
#ifndef ARM_JIT_A64_IO_SEQUENCE
#define ARM_JIT_A64_IO_SEQUENCE 1
#endif
static int a64_io_sequence_shape(const arm_jit_op_t *op) {
    /* Most positions are not a store; reject before touching four more ops. */
    if (op[0].kind != ARM_JIT_OP_SINGLE_DT ||
        (op[0].d & ~ARM_BC_SD_U) != ARM_BC_SD_P) return 0;
    for (unsigned i = 0; i < 5u; ++i)
        if (op[i].cond != 14u || op[i].stop || op[i].reserved ||
            op[i].pc != op[0].pc + i * 4u) return 0;
    if ((op[0].pc >> 10) != (op[4].pc >> 10)) return 0;
    if (op[1].kind != ARM_JIT_OP_SINGLE_DT ||
        (op[1].d & ~ARM_BC_SD_U) != (ARM_BC_SD_P | ARM_BC_SD_L) ||
        op[4].kind != ARM_JIT_OP_SINGLE_DT || op[4].insn != op[0].insn ||
        op[1].a != op[0].a) return 0;
    if (op[2].kind != ARM_JIT_OP_DATA || op[2].a != 0u ||
        (op[2].d & (ARM_BC_DATA_IMM | ARM_BC_DATA_S | ARM_BC_DATA_REGSHIFT)) != ARM_BC_DATA_IMM ||
        op[2].b != op[1].b || op[2].c != op[1].b ||
        op[3].kind != ARM_JIT_OP_DATA || op[3].a != 12u ||
        op[3].d || op[3].g || op[3].b != op[0].b || op[3].c != op[0].b) return 0;
    unsigned base = op[0].a, value = op[0].b, loaded = op[1].b, shift = op[3].e;
    return base < 15u && value < 15u && loaded < 15u && shift < 15u &&
        base != value && base != loaded && base != shift &&
        value != loaded && value != shift && loaded != shift;
}

static void a64_io_sequence_store(a64_emit_t *e, const arm_jit_op_t *op,
                                  const arm_leaf_write32_t *write) {
    a64_guest(e, 2, op->b, op->pc);
    a64_store_imm(e, offsetof(arm920t_t, r) + 15u * 4u, op->pc + 4u);
    a64_ptr(e, 0, (uintptr_t)write->user);
    a64_imm(e, 1, write->pa);
    a64_ptr(e, 16, (uintptr_t)write->write);
    a64_u32(e, 0xd63f0200u); /* blr x16: every original store still happens */
}

static int a64_io_sequence(arm920t_t *c, a64_emit_t *e, const arm_jit_op_t *op,
                           uint32_t before, size_t *success) {
    if (!ARM_JIT_A64_IO_SEQUENCE || e->counted || !e->leaf_write32_mask ||
        !e->live_read32_count || e->live_read32_count > 8u ||
        !a64_io_sequence_shape(op)) return 0;
    uint32_t store_offset = (op[0].d & ARM_BC_SD_U) ? op[0].imm : 0u - op[0].imm;
    uint32_t read_offset = (op[1].d & ARM_BC_SD_U) ? op[1].imm : 0u - op[1].imm;
    const arm_leaf_write32_t *write = NULL;
    const arm_live_read32_t *read = NULL;
    for (size_t i = 0; !write && i < e->leaf_write32_count; ++i) {
        if (!(e->leaf_write32_mask & (1u << i))) continue;
        for (size_t j = 0; j < e->live_read32_count; ++j) {
            if (e->live_read32[j].pa - e->leaf_write32[i].pa != read_offset - store_offset ||
                !fastmem_excludes(c, e->live_read32[j].pa, 4u, 0)) continue;
            write = &e->leaf_write32[i]; read = &e->live_read32[j]; break;
        }
    }
    if (!write) return 0;

    e->nzcv_valid = 0;
    e->fwd_guest = -1;
    size_t reject[12]; unsigned nreject = 0;
    /* Entry already requires the whole block. Retain an explicit local
     * bound as well: w22 includes earlier forward skips, before includes
     * their original positions, so this proves all five actual retirements. */
    a64_cmp_imm(e, 22, before + 5u);
    reject[nreject++] = a64_jump(e, 3); /* LO */
    /* Watches can be installed after compilation. Even a certified write
     * must retain the normal code-invalidation path when its region was
     * fetched as instructions; no new watch can appear inside this region. */
    a64_cpu_mem(e, 0x39400000u, 6, offsetof(arm920t_t, jit_code_nonram), 0);
    reject[nreject++] = e->pos;
    a64_branch_word(e, 0x37080006u); /* tbnz w6, #1 */
    for (unsigned access = 0; access < 2u; ++access) {
        const arm_jit_op_t *mem = &op[access];
        a64_guest(e, 12, mem->a, mem->pc);
        if (mem->imm)
            a64_u32(e, ((mem->d & ARM_BC_SD_U) ? 0x11000000u : 0x51000000u) |
                       (mem->imm << 10) | (12u << 5) | 12u);
        a64_mask(e, 6, 12, 3u);
        reject[nreject++] = e->pos;
        a64_branch_word(e, 0x35000006u); /* cbnz w6: original VA must be aligned */
        a64_word_pa(e, reject, &nreject);
        a64_imm(e, 6, access ? read->pa : write->pa);
        a64_cmp(e, 0, 6);
        reject[nreject++] = a64_jump(e, 1); /* NE: exact registered endpoint */
    }
    /* Nothing above mutated guest or peripheral state or filled a TLB.
     * Both callbacks see write-through registers and their own original PC.
     * The leaf contract preserves the callee-saved native register cache. */
    a64_io_sequence_store(e, &op[0], write);
    a64_ptr(e, 10, (uintptr_t)read->word);
    a64_u32(e, 0xb9400142u); /* ldr w2, [x10]: fresh volatile read after store */
    a64_put_guest(e, op[1].b, 2);
    (void)a64_data(e, &op[2], before + 3u);
    (void)a64_data(e, &op[3], before + 4u);
    a64_io_sequence_store(e, &op[4], write);
#if ARM920T_PROFILING
    _Static_assert(offsetof(gp32_cpu_profile_t, native_io_sequence_insns) ==
                   offsetof(gp32_cpu_profile_t, native_io_sequence_batches) + sizeof(uint64_t),
                   "paired peripheral-region profile counters");
    a64_cpu_addr(e, 17, offsetof(arm920t_t, prof.native_io_sequence_batches));
    a64_u32(e, 0xa9401e26u); /* ldp x6, x7, [x17] */
    a64_u32(e, 0x910004c6u); /* add x6, x6, #1 */
    a64_u32(e, 0x910014e7u); /* add x7, x7, #5 */
    a64_u32(e, 0xa9001e26u); /* stp x6, x7, [x17] */
#endif
    *success = a64_jump(e, 14u);
    for (unsigned i = 0; i < nreject; ++i) a64_patch(e, reject[i], e->pos);
    /* Rejected paths still enter the original first store with unchanged
     * guest state. No caller-saved forwarding or host flags survive guards. */
    e->fwd_guest = -1;
    e->nzcv_valid = 0;
    return 1;
}

/* The shared word probe has established the current physical address in w0.
 * Certificates require an aligned original VA and use the aligned physical
 * word; preserve w0 for the following ROM/open-bus/I/O probes on rejection.
 * No callback separates these checks. Each certificate still reads its live
 * storage afresh and resumes only this access's hot continuation. */
static void a64_live_read32_pa(a64_emit_t *e, unsigned rd, size_t cont) {
    if (!e->live_read32_count) return;
    a64_mask(e, 6, 12, 3u);
    a64_cmp(e, 6, 31);
    size_t unaligned = a64_jump(e, 1);
    a64_mask(e, 7, 0, ~3u);
    for (size_t i = 0; i < e->live_read32_count && !e->fail; ++i) {
        a64_imm(e, 6, e->live_read32[i].pa);
        a64_cmp(e, 7, 6);
        size_t unmatched = a64_jump(e, 1);
        a64_ptr(e, 10, (uintptr_t)e->live_read32[i].word);
        a64_u32(e, 0xb9400000u | (10u << 5) | 1u); /* fresh ldr w1, [x10] */
        a64_put_guest(e, rd, 1);
        /* Cold area: the continuation is behind this branch. */
        a64_branch_hot(e, cont);
        a64_patch(e, unmatched, e->pos);
    }
    a64_patch(e, unaligned, e->pos);
}

/* Cold chain probe after a64_word_pa has established the aligned physical
 * word in w0. w12 still holds the unaligned VA, w2 the writeback for writeback
 * shapes and w11 the unaligned rotation for a computed address:
 *   - a physical word below ARM_JIT_BIOS_SIZE_BYTES reads jit_bios_base[pa];
 *     the window is read-only to guests and fixed before blocks translate,
 *     so no callback runs and no exit check applies;
 *   - a physical word outside every window (RAM, BIOS, identity I/O) reads
 *     the bus-open constant, when the bus declares one (open_bus_word32);
 * the loaded word keeps the ARM unaligned rotation, Rd commits first and the
 * writeback second (the checked helpers own Rd==Rn ordering), then the
 * recorded hot continuation resumes.  Every rejection falls to the next
 * probe with w0, w12, w2 and w11 unchanged. */
static void a64_ro_word_pa(a64_emit_t *e, const arm_jit_op_t *op, size_t cont) {
    size_t reject[3];
    unsigned nreject = 0;
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
        a64_branch_hot(e, cont);
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
        a64_branch_hot(e, cont);
    }
    for (unsigned i = 0; i < nreject; ++i) a64_patch(e, reject[i], e->pos);
}

static int a64_single_dt(a64_emit_t *e, const arm_jit_op_t *op, uint32_t done) {
    unsigned d = op->d;
    int load = !!(d & ARM_BC_SD_L), byte = !!(d & ARM_BC_SD_B);
    int wb = !(d & ARM_BC_SD_P) || (d & ARM_BC_SD_W);
    int fwd_store = !load && !wb && op->b == (unsigned)e->fwd_guest;
    /* A pre-indexed address without writeback and the RAM guards preserve
     * w2. Consume the committed data result here; the cold helper rereads the
     * guest source, and no following op may reuse its caller-saved scratch. */
    e->fwd_guest = -1;
    if (!(load ? e->ram_read : e->ram_write) || (wb && op->a == 15u)) return 0;
    int access = op->b != 15u; /* PC load/store stays on whole-op fallback */
    size_t slow[4];
    unsigned nslow = 0;
    /* PC-relative immediate addresses are fixed by the decoded instruction.
     * Fold address/word alignment only, never the loaded memory contents. */
    int literal = op->a == 15u && !(d & ARM_BC_SD_REGOFF);
    uint32_t literal_addr = op->pc + 8u + ((d & ARM_BC_SD_U) ? op->imm : 0u - op->imm);
    if (literal) {
        /* Start the independent live-page load before materializing the VA. */
        if (e->mmu)
            a64_u32(e, 0xf9400000u | (((literal_addr >> 12) & 0xfffu) << 10) |
                       (20u << 5) | 7u); /* ldr x7, [x20, #slot*8] */
        a64_imm(e, 0, literal_addr);
    } else {
        if (d & ARM_BC_SD_REGOFF) {
            a64_guest(e, 1, op->c, op->pc);
            if (op->e == 3u && !op->f) a64_load(e, 3, offsetof(arm920t_t, cpsr));
            a64_shift(e, op->e, op->f, 0);
        }
        /* A pre-indexed access without writeback never needs the base+offset sum
         * as a guest value, so form it directly in the address register instead of
         * copying w2 into w0. Post-indexed and writeback forms keep w2. */
        int folded = (d & ARM_BC_SD_P) && !wb;
        unsigned sum = folded ? 0u : 2u;
        /* That sum may read a cached base in place; every other shape needs
         * the base itself in w0. */
        unsigned base = 0u;
        if (folded && ((d & ARM_BC_SD_REGOFF) || op->imm) && op->a < 15u && e->cache_host[op->a])
            base = e->cache_host[op->a];
        else a64_guest(e, 0, op->a, op->pc);
        if (d & ARM_BC_SD_REGOFF)
            a64_rr(e, (d & ARM_BC_SD_U) ? 0x0b000000u : 0x4b000000u, sum, base, 1);
        else if (op->imm)
            /* ARM's 12-bit transfer offset fits ADD/SUB W immediate exactly.
             * Neither form sets flags; W arithmetic preserves address wrap. */
            a64_u32(e, ((d & ARM_BC_SD_U) ? 0x11000000u : 0x51000000u) |
                       (op->imm << 10) | (base << 5) | sum);
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
    if (literal && e->mmu) {
        /* The instruction fixes the lookup slot, not its live mapping or
         * contents. Keep the same tag check and cold fallback; an aligned
         * literal also needs no runtime word-alignment mask. */
        a64_u32(e, 0x6b4030ffu); /* cmp w7, w0, lsr #12 */
        slow[nslow++] = a64_jump(e, 1);
        a64_u32(e, 0xca478009u); /* eor x9, x0, x7, lsr #32 */
        if (!byte && (literal_addr & 3u)) a64_mask(e, 9, 9, ~3u);
    } else a64_ram_offset(e, slow, &nslow, byte ? 1u : 4u, !byte);
    if (!load) a64_guard_code_write(e, slow, &nslow, 0u);
    /* Keep the authoritative guest store, but avoid moving its value through
     * w1 when a write-through register is already assigned. Guard failures
     * still enter the unchanged cold path before any guest value is written. */
    unsigned value = e->cache_host[op->b] ? e->cache_host[op->b] : 1u;
    if (load) {
        /* w9 is already the in-RAM offset; register-offset addressing folds
         * the jit_ram_base add into the access itself. */
        a64_u32(e, (byte ? 0x38606800u : 0xb8606800u) | (9u << 16) | (21u << 5) | value);
        if (!byte) {
            if (!literal) a64_rr(e, 0x1ac02c00u, value, value, 11);
            else if (literal_addr & 3u)
                a64_u32(e, 0x13800000u | (value << 16) |
                           (((literal_addr & 3u) * 8u) << 10) | (value << 5) | value);
        }
        /* Match bytecode LDR PC: ARM state, word alignment, no interworking. */
        if (op->b == 15u) {
            /* Keep the committed alignment in the hop argument register.
             * Writeback below only stores Rn/cached copies and preserves w16. */
            a64_mask(e, 16, value, ~3u);
            value = 16u;
        }
        a64_put_guest(e, op->b, value);
    } else {
        if (fwd_store) value = 2u;
        else if (value == 1u) a64_guest(e, value, op->b, op->pc + 4u); /* STR PC = instruction+12 */
        a64_u32(e, (byte ? 0x38206800u : 0xb8206800u) | (9u << 16) | (21u << 5) |
                   value);
    }
    if (wb) a64_put_guest(e, op->a, 2); /* includes the helper's Rn==Rd order */
    if (load && op->b == 15u) {
        if (op->reserved == 3u || op->reserved == 5u) {
            /* Keep the real loaded return, as for an inlined LDM PC. */
            a64_imm(e, 6, e->expected_next);
            a64_cmp(e, value, 6);
            size_t matched = a64_jump(e, 0);
            a64_hop(e, done);
            a64_patch(e, matched, e->pos);
        } else a64_hop(e, done);
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
 * byte).  Rd==PC (loads mask and redirect, stores read instruction+12) and Rn==PC
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
    size_t slow[6];
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
    if (!load) a64_guard_code_write(e, slow, &nslow, 1u);
    unsigned value = e->cache_host[op->b] ? e->cache_host[op->b] : 1u;
    if (load) {
        /* ldrh/ldrsb/ldrsh: A64 already yields the exact
         * zero/sign extension op_halfword_bc produces. */
        a64_u32(e, (sh == 1u ? 0x78606800u : sh == 2u ? 0x38e06800u : 0x78e06800u) |
                 (9u << 16) | (21u << 5) | value);
        a64_put_guest(e, op->b, value);
    } else {
        if (value == 1u) a64_guest(e, value, op->b, op->pc);
        a64_u32(e, 0x78206800u | (9u << 16) | (21u << 5) | value);
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
 * value (op->pc + 12); loads let a transferred Rn override its old value and,
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
    a64_branch_word(e, 0x35000000u | 6u); /* cbnz w6 -> helper on unaligned span */
    if (count > 1u) {
        /* Same virtual 4 KiB page takes the original single-translation path.
         * A span crossing a 4 KiB boundary branches to the cold two-page retry
         * behind the block instead of walking an inline second copy; with
         * count <= 16 (64 bytes) an aligned span crosses at most one boundary. */
        a64_u32(e, 0x11000000u | ((4u * (count - 1u)) << 10) | (0u << 5) | 5u); /* w5 = last */
        a64_rr(e, 0x4a000000u, 6, 0, 5); /* eor w6, w0, w5 */
        a64_ubfx(e, 6, 6, 12, 20);
        xpage = e->pos;
        a64_branch_word(e, 0x35000000u | 6u); /* cbnz w6 -> cold two-page retry */
    }
    /* The span stays within this 4 KiB page, so proving its aligned first
     * word in a full RAM page also proves every transferred word. */
    a64_ram_offset(e, slow, &nslow, 4u, 0);
    if (!load) a64_guard_code_write(e, slow, &nslow, 4u * (count - 1u));
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

    if (!load && e->check_code_writes) {
        /* Virtual neighbors can map to unrelated physical pages. Test all
         * words in each resolved fragment, before any transfer or writeback. */
        a64_guard_code_words(e, slow, nslow, 2u, 0u, 5u);
        a64_imm(e, 16, count);
        a64_rr(e, 0x4b000000u, 16, 16, 5); /* w16 = count - first-page count */
        a64_guard_code_words(e, slow, nslow, 4u, 0u, 16u);
    }

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
    /* w2 holds final writeback. Only a non-first base source uses it;
     * choose at translation time, with no runtime condition added. */
    unsigned updated_rn = (op->d & 2u) && (list & ((1u << op->a) - 1u)) ? op->a : 16u;
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
        a64_branch_word(e, 0x35000000u | 6u); /* cbnz w6 -> conservative odd-target helper */
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
                    if (next == r + 1u) {
                        a64_u32(e, guest_pair);
                        a64_cache_sync(e, r, 1);
                        a64_cache_sync(e, next, 3);
                    } else {
                        a64_put_guest(e, r, 1);
                        a64_put_guest(e, next, 3);
                    }
                } else {
                    if (next == r + 1u) a64_u32(e, guest_pair);
                    else {
                        a64_guest(e, 1, r, op->pc);
                        a64_guest(e, 3, next, op->pc);
                    }
                    if (r == updated_rn) a64_mov(e, 1, 2);
                    if (next == updated_rn) a64_mov(e, 3, 2);
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
            if (r == updated_rn) a64_mov(e, 1, 2);
            else a64_guest(e, 1, r, op->pc + 4u); /* STM PC = instruction+12 */
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
        a64_hop_reg(e, 14, done);
        a64_patch(e, matched, e->pos);
    } else a64_hop_reg(e, 14, done);
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

/* BX only changes PC and the Thumb bit; it cannot call the bus or alter
 * mappings, interrupt masks, RAM ownership or the run deadline. An ARM
 * target keeps the state already proved at native entry/checked helpers and
 * uses the same guarded hop as MOV pc. Thumb targets return to dispatch. */
static ARM_NOINLINE int a64_bx(a64_emit_t *e, const arm_jit_op_t *op, uint32_t done) {
    if ((op->insn & 0x0ffffff0u) != 0x012fff10u) return 0;
    unsigned rm = op->insn & 15u;
    a64_guest(e, 0, rm, op->pc); /* BX PC reads instruction address + 8. */
    if (rm != 15u) {
        /* The Thumb exit is not part of an ARM successor's hot body. Use
         * B.cond's range: a cold span can be farther away than TB(N)Z reaches. */
        a64_u32(e, 0x7200001fu); /* tst w0, #1; guest flags stay in CPSR */
        size_t thumb_target = a64_jump(e, 1);
        a64_cold_op_t *cold = a64_cold_open(e, op, done, A64_COLD_BX_THUMB);
        a64_cold_fix(e, cold, thumb_target);
    }
    a64_mask(e, 16, 0, ~3u);
    a64_store(e, 16, offsetof(arm920t_t, r) + 15u * 4u);
    size_t matched = 0;
    if (op->reserved == ARM_JIT_RETURN_GUARDED) {
        /* A preceding callback may replace LR. Follow the decoded caller
         * only for its real aligned ARM target, as the checked helper did. */
        a64_imm(e, 6, e->expected_next);
        a64_cmp(e, 16, 6);
        matched = a64_jump(e, 0);
    }
    a64_hop(e, done);
    if (matched) a64_patch(e, matched, e->pos);
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
    /* This emitter consumes a forwarded store value before clearing it. */
    if (op->kind == ARM_JIT_OP_SINGLE_DT) {
        if (!e->counted) return a64_single_dt(e, op, done);
        e->fwd_guest = -1;
        a64_load(e, 16, offsetof(arm920t_t, jit_counted_observing));
        a64_cmp_imm(e, 16, 0u);
        size_t normal = a64_jump(e, 0);
        a64_slow(e, op, done);
        size_t join = a64_jump(e, 14);
        a64_patch(e, normal, e->pos);
        if (!a64_single_dt(e, op, done)) a64_slow(e, op, done);
        a64_patch(e, join, e->pos);
        e->fwd_guest = -1;
        e->nzcv_valid = 0;
        return 1;
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
    if (op->kind == ARM_JIT_OP_BLOCK_DT) return a64_block_dt(e, op, done);
    if (op->kind == ARM_JIT_OP_INTERP) return a64_bx(e, op, done);
    if (op->kind == ARM_JIT_OP_PSR) return a64_psr_mov(e, op, done);
    if (op->kind == ARM_JIT_OP_COPROC) return a64_local_p15(e, op, done);
    if (op->kind == ARM_JIT_OP_BRANCH) {
        if (op->insn & (1u << 24)) {
            a64_imm(e, 16, op->pc + 4u);
            a64_put_guest(e, 14u, 16);
        }
        if (e->loop_done == done) {
            a64_selfloop(e, done, arm_jit_branch_target(op->pc, op->insn));
            return 1;
        }
        /* w16 keeps the committed target for a linked exit below. */
        a64_imm(e, 16, arm_jit_branch_target(op->pc, op->insn));
        a64_store(e, 16, offsetof(arm920t_t, r) + 15u * 4u);
        /* Inlined BL and stitched unconditional B follow the decoded trace. */
        if (op->reserved != 2u && (op->stop || !arm_jit_is_uncond_b_no_link(op->insn))) a64_hop(e, done);
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
        /* General trace returns check live LR and execution state. Proven
         * register-only leaves materialize their known caller directly. */
        if (op->reserved == ARM_JIT_RETURN_GUARDED || op->reserved == ARM_JIT_RETURN_LEAF) continue;
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

/* Recognize a register-renamable masked multiply blend, not a guest address.
 * Four independent words share the RAM/mapping/watch proof and are evaluated
 * in caller-saved NEON lanes. Short budgets and unusual mappings retain every
 * original instruction. The final lane supplies both live scratch registers. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((cold))
#endif
static ARM_NOINLINE int a64_packed_word_loop(a64_emit_t *e, const arm_jit_op_t *ops) {
    if (e->loop_done != 24u || e->counted || !e->ram_read || !e->ram_write ||
        ops[0].kind != ARM_JIT_OP_SINGLE_DT) return 0;
    unsigned ptr = (ops[0].insn >> 16) & 15u, val = (ops[0].insn >> 12) & 15u;
    unsigned ctr = (ops[1].insn >> 16) & 15u, other = (ops[2].insn >> 12) & 15u;
    unsigned scale = ops[9].insn & 15u;
    unsigned regs[] = {ptr, val, ctr, other, scale};
    for (unsigned i = 0; i < GP32_ARRAY_COUNT(regs); ++i) {
        if (regs[i] == 15u) return 0;
        for (unsigned j = 0; j < i; ++j) if (regs[i] == regs[j]) return 0;
    }
    for (unsigned i = 0; i < 24u; ++i)
        if (ops[i].reserved || ops[i].pc != ops[0].pc + 4u * i ||
            ops[i].cond != (i == 23u ? 11u : 14u)) return 0;
    if (ops[0].insn != (0xe5900000u | (ptr << 16) | (val << 12)) ||
        ops[1].insn != (0xe2800001u | (ctr << 16) | (ctr << 12)) ||
        ops[9].insn != (0xe0000090u | (val << 16) | (val << 8) | scale) ||
        ops[11].insn != (0xe0000090u | (other << 16) | (other << 8) | scale) ||
        ops[20].insn != (0xe1800000u | (other << 16) | (val << 12) | val) ||
        ops[21].insn != (0xe4800004u | (ptr << 16) | (val << 12)) ||
        (ops[22].insn & 0xfffff000u) != (0xe3500000u | (ctr << 16)) ||
        ops[23].insn != 0xbaffffe7u) return 0;
    unsigned shift_a = (ops[10].insn >> 7) & 31u;
    unsigned shift_b = (ops[12].insn >> 7) & 31u;
    if (ops[10].insn != (0xe1a00020u | (other << 12) | (shift_a << 7) | other) ||
        ops[12].insn != (0xe1a00020u | (val << 12) | (shift_b << 7) | val)) return 0;
    if (!shift_a) shift_a = 32u;
    if (!shift_b) shift_b = 32u;
    static const uint8_t indices[4][4] = {{3,4,5,6}, {2,7,8,255}, {13,14,17,18}, {15,16,19,255}};
    uint32_t masks[4] = {UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX};
    for (unsigned chain = 0; chain < 4u; ++chain) {
        unsigned rd = chain & 1u ? other : val;
        for (unsigned j = 0; j < 4u && indices[chain][j] != 255u; ++j) {
            unsigned i = indices[chain][j], rn = i == 2u ? val : rd;
            if (ops[i].kind != ARM_JIT_OP_DATA ||
                (ops[i].insn & 0xfffff000u) != (0xe3c00000u | (rn << 16) | (rd << 12))) return 0;
            masks[chain] &= ~ops[i].imm;
        }
    }
    uint32_t limit = ops[22].imm;
    if (limit < 4u || limit > INT32_MAX) return 0;

    size_t slow[12]; unsigned nslow = 0;
    a64_cmp_imm(e, 22, 96u);
    slow[nslow++] = a64_jump(e, 3); /* less than four complete iterations */
    a64_guest(e, 10, ctr, ops[0].pc);
    a64_imm(e, 11, limit - 4u);
    a64_cmp(e, 10, 11);
    slow[nslow++] = a64_jump(e, 8); /* unsigned check also rejects negative counters */
    a64_guest(e, 12, ptr, ops[0].pc);
    a64_mask(e, 6, 12, 3u);
    slow[nslow++] = e->pos;
    a64_branch_word(e, 0x35000006u);
    a64_mov(e, 0, 12);
    a64_ram_offset(e, slow, &nslow, 16u, 0);
    a64_mask(e, 6, 9, 0xfffu);
    a64_cmp_imm(e, 6, 4096u - 16u);
    slow[nslow++] = a64_jump(e, 8);
    a64_guard_code_words(e, slow, &nslow, 9u, 4u, 0u);

    /* Whole-block fetch validation already proves all instruction spans.
     * There are no callbacks or TLB mutations in this RAM-only batch. */
    a64_rr(e, 0x8b204000u, 14, 21, 9); /* host pointer */
    for (unsigned i = 0; i < 4u; ++i) {
        a64_imm(e, 6, masks[i]);
        a64_u32(e, 0x4e040cc0u | (3u + i)); /* dup v3..v6.4s,w6 */
    }
    a64_guest(e, 11, scale, ops[0].pc);
    a64_u32(e, 0x4e040d67u); /* dup v7.4s,w11 */
    a64_u32(e, 0x3dc001c0u); /* ldr q0,[x14] */
    a64_u32(e, 0x4e231c01u); /* and v1.16b,v0.16b,v3.16b */
    a64_u32(e, 0x4e241c02u);
    a64_u32(e, 0x4ea79c21u); /* mul v1.4s,v1.4s,v7.4s */
    a64_u32(e, 0x6f000442u | ((64u - shift_a) << 16));
    a64_u32(e, 0x4ea79c42u);
    a64_u32(e, 0x6f000421u | ((64u - shift_b) << 16));
    a64_u32(e, 0x4e251c21u);
    a64_u32(e, 0x4e261c42u);
    a64_u32(e, 0x4ea21c20u); /* orr v0.16b,v1.16b,v2.16b */
    a64_u32(e, 0x3d8001c0u); /* str q0,[x14] */
    a64_u32(e, 0x0e1c3c02u); /* umov w2,v0.s[3] */
    a64_u32(e, 0x0e1c3c43u); /* umov w3,v2.s[3] */
    a64_u32(e, 0x1100418cu); /* add w12,w12,#16 */
    a64_u32(e, 0x1100114au); /* add w10,w10,#4 */
    a64_put_guest(e, ptr, 12);
    a64_put_guest(e, ctr, 10);
    a64_put_guest(e, val, 2);
    a64_put_guest(e, other, 3);
#if ARM920T_PROFILING
    a64_cpu_mem(e, 0xf9400000u, 6, offsetof(arm920t_t, prof.native_packed_word_batches), 3);
    a64_u32(e, 0x910004c6u);
    a64_cpu_mem(e, 0xf9000000u, 6, offsetof(arm920t_t, prof.native_packed_word_batches), 3);
#endif
    a64_imm(e, 11, limit);
    a64_cmp(e, 10, 11);
    a64_u32(e, 0xd53b4205u);
    a64_load(e, 3, offsetof(arm920t_t, cpsr));
    a64_bfxil_low(e, 5, 3, 28u);
    a64_store(e, 5, offsetof(arm920t_t, cpsr));
    a64_u32(e, 0x510182d6u); /* sub w22,w22,#96 */
    a64_u32(e, 0x110182f7u); /* add w23,w23,#96 */
    size_t remains = a64_jump(e, 11); /* signed LT uses the final guest CMP */
    a64_exit_const(e, ops[23].pc + 4u, 0u);
    a64_patch(e, remains, e->pos);
    a64_cmp_imm(e, 22, e->block_count);
    size_t leave = a64_jump(e, 3);
    a64_branch_back(e, e->loop_head);
    a64_patch(e, leave, e->pos);
    a64_exit_const(e, ops[0].pc, 0u);
    for (unsigned i = 0; i < nslow; ++i) a64_patch(e, slow[i], e->pos);
    e->nzcv_valid = 0;
    e->fwd_guest = -1;
    return 1;
}

/* Bound a contiguous halfword-copy idiom by the guest instruction budget.
 * Every load still precedes its store, including overlapping forward copies.
 * A failed proof reaches the ordinary instruction sequence without changing
 * architectural state. No callback, page crossing or watched code write is
 * batched. I-cache-enabled writes retain the ordinary native store policy. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((cold)) /* Rare translation work, kept apart from CPU execution. */
#endif
static ARM_NOINLINE int a64_copy_loop(a64_emit_t *e, const arm_jit_op_t *ops) {
    if (!e->ram_read || !e->ram_write || e->loop_done != 5u || e->counted ||
        (ops[0].pc >> 10) != (ops[4].pc >> 10) ||
        (ops[0].insn & 0xfff00fffu) != 0xe0d000b2u ||
        (ops[1].insn & 0xfff00fffu) != 0xe2400001u ||
        (ops[2].insn & 0xfff0ffffu) != 0xe3500000u ||
        (ops[3].insn & 0xfff00fffu) != 0xe0c000b2u ||
        ops[4].insn != 0xcafffffau) return 0;
    unsigned src = (ops[0].insn >> 16) & 15u;
    unsigned val = (ops[0].insn >> 12) & 15u;
    unsigned dst = (ops[3].insn >> 16) & 15u;
    unsigned cnt = (ops[1].insn >> 16) & 15u;
    if (src == 15u || val == 15u || dst == 15u || cnt == 15u ||
        src == val || src == dst || src == cnt || val == dst || val == cnt || dst == cnt ||
        ((ops[1].insn >> 12) & 15u) != cnt || ((ops[2].insn >> 16) & 15u) != cnt ||
        ((ops[3].insn >> 12) & 15u) != val) return 0;
    for (unsigned i = 0; i < 5u; ++i)
        if (ops[i].reserved || ops[i].pc != ops[0].pc + i * 4u) return 0;

    size_t slow[16];
    unsigned nslow = 0;
    a64_guest(e, 10, cnt, ops[0].pc);
    a64_cmp_imm(e, 10, 0);
    slow[nslow++] = a64_jump(e, 13); /* LE: preserve signed wrap semantics */
    a64_guest(e, 12, src, ops[0].pc);
    a64_guest(e, 13, dst, ops[0].pc);
    a64_rr(e, 0x2a000000u, 6, 12, 13);
    a64_mask(e, 6, 6, 1u);
    slow[nslow++] = e->pos;
    a64_branch_word(e, 0x35000006u); /* CBNZ w6: leave odd accesses alone */
    a64_mov(e, 0, 12);
    a64_ram_offset(e, slow, &nslow, 2u, 0);
    a64_mov(e, 14, 9);
    a64_mov(e, 0, 13);
    a64_ram_offset(e, slow, &nslow, 2u, 0);
    a64_mov(e, 15, 9);
    /* The same page proof covers every byte in at most 16 iterations. The
     * conservative full window also rejects a short copy near a page end. */
    for (unsigned r = 14; r <= 15; ++r) {
        a64_mask(e, 6, r, 0xfffu);
        a64_cmp_imm(e, 6, 4096u - 32u);
        slow[nslow++] = a64_jump(e, 8); /* HI */
    }
    /* A halfword-aligned 32-byte window covers at most nine code words.
     * The padded watch bitmap permits the conservative final-word probe. */
    a64_guard_code_words(e, slow, &nslow, 15u, 9u, 0u);
    a64_imm(e, 6, 5u);
    a64_rr(e, 0x1ac00800u, 11, 22, 6); /* UDIV w11,w22,w6 */
    a64_cmp(e, 11, 10);
    a64_u32(e, 0x1a800000u | (10u << 16) | (9u << 12) | (11u << 5) | 11u);
    a64_imm(e, 6, 16u);
    a64_cmp(e, 11, 6);
    a64_u32(e, 0x1a800000u | (6u << 16) | (9u << 12) | (11u << 5) | 11u);
    a64_rr(e, 0x8b204000u, 0, 21, 14); /* ADD x0,x21,w14,UXTW */
    a64_rr(e, 0x8b204000u, 1, 21, 15);
    a64_mov(e, 3, 11);
    size_t copy = e->pos;
    a64_u32(e, 0x78402402u); /* LDRH w2,[x0],#2 */
    a64_u32(e, 0x78002422u); /* STRH w2,[x1],#2 */
    a64_u32(e, 0x71000463u); /* SUBS w3,w3,#1 */
    a64_branch_word(e, 0x54000001u | (((uint32_t)(copy - e->pos) & 0x7ffffu) << 5));
    a64_rr(e, 0x0b000400u, 12, 12, 11); /* source += n * 2 */
    a64_rr(e, 0x0b000400u, 13, 13, 11);
    a64_rr(e, 0x4b000000u, 10, 10, 11);
    a64_put_guest(e, src, 12);
    a64_put_guest(e, dst, 13);
    a64_put_guest(e, cnt, 10);
    a64_put_guest(e, val, 2);
    a64_cmp_imm(e, 10, 0);
    a64_u32(e, 0xd53b4205u);
    a64_load(e, 3, offsetof(arm920t_t, cpsr));
    a64_bfxil_low(e, 5, 3, 28u);
    a64_store(e, 5, offsetof(arm920t_t, cpsr));
    a64_rr(e, 0x0b000800u, 16, 11, 11); /* five guest ops per iteration */
    a64_rr(e, 0x4b000000u, 22, 22, 16);
    a64_rr(e, 0x0b000000u, 23, 23, 16);
    size_t remaining = a64_jump(e, 1); /* CMP flags survive non-S accounting */
    a64_exit_const(e, ops[4].pc + 4u, 0u);
    a64_patch(e, remaining, e->pos);
    a64_cmp_imm(e, 22, e->block_count);
    size_t leave = a64_jump(e, 3);
    a64_branch_back(e, e->loop_head);
    a64_patch(e, leave, e->pos);
    a64_exit_const(e, ops[0].pc, 0u);
    for (unsigned i = 0; i < nslow; ++i) a64_patch(e, slow[i], e->pos);
    return 1;
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
/* Keep cold emission out of the hot-body generator: inlining expands its
 * working set without removing work from executed guest blocks. */
static ARM_NOINLINE void a64_cold_flush(a64_emit_t *e) {
    for (unsigned i = 0; i < e->cold_count && !e->fail; ++i) {
        a64_cold_op_t *c = &e->cold[i];
        size_t entry = e->pos;
        for (unsigned j = 0; j < c->nfixup; ++j) a64_patch_cold(e, c->fixup[j], entry);
        e->expected_next = c->expected_next;
        e->generation = c->generation;
        switch (c->kind) {
        case A64_COLD_BX_THUMB:
            /* The hot bit test leaves the original target in w0. This exit
             * preserves all guest flags/mode bits and never resumes ARM code. */
            a64_load(e, 3, offsetof(arm920t_t, cpsr));
            a64_u32(e, 0x321b0063u); /* orr w3, w3, #T_FLAG */
            a64_store(e, 3, offsetof(arm920t_t, cpsr));
            a64_mask(e, 16, 0, ~1u);
            a64_store(e, 16, offsetof(arm920t_t, r) + 15u * 4u);
            a64_return(e, c->done);
            continue;
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
            break;
        }
        case A64_COLD_SINGLE_DT: {
            /* The inline order: certificate, direct identity-I/O attempt,
             * then the checked helper that every rejection lands on. The hot
             * body keeps the VA in w0; the cold sequences work on the
             * established w12 handover, which is restored here (cold bytes,
             * never walked by the fast path). */
            a64_mov(e, 12, 0);
            size_t io_cold[4];
            unsigned nio = 0;
            if (c->ro || (c->access && !c->byte)) {
                /* No callback separates the live, read-only and I/O probes.
                 * Prove the physical word once; a TLB miss bypasses all three
                 * to the checked helper. Live certificates only cover word
                 * loads without writeback, a subset of this access path. */
                a64_word_pa(e, io_cold, &nio);
                if (!c->load && !c->wb && !c->byte)
                    a64_leaf_write32_pa(e, c->op, c->cont);
                if (c->live) a64_live_read32_pa(e, c->op->b, c->cont);
                if (c->ro) a64_ro_word_pa(e, c->op, c->cont);
                if (c->access && !c->byte)
                    a64_io_word_pa(e, c->op, c->done, c->load, c->wb, io_cold, &nio, c->cont);
            }
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
        /* The two-page retry falls through here; both checked helpers end
         * with a "may continue" edge that lands here. Emit one shared return
         * to the recorded hot continuation for each cold chain. */
        a64_branch_hot(e, c->cont);
    }
}

/* Emit rt_enter, rt_exit and rt_hop at the start of a fresh arena.
 *
 * rt_enter (bl from a block entry, which already pushed {x19, x30} into a
 * 96-byte frame): saves x20-x29, establishes the register convention for
 * x0 = CPU and w1 = budget, and returns into the block body.
 * rt_exit (b): w0 = ops of the current block; returns w0 + w23 to dispatch.
 * rt_hop (bl from a clean exit): w0 = ops of the current block, w16 = aligned
 * committed next PC. Returns x17 = successor body after accounting for the
 * current block, or x17 = rt_exit with w0 and w23 unchanged. */
_Static_assert(ARM_JIT_CODE_SIZE <= (1u << 27), "B/BL from every block reach the runtime stubs");
/* Relative branches retain the same offsets in RW and RX views. Only callable
 * entries and eviction address comparisons use RX; all emission uses RW. */
static uint8_t *a64_exec_addr(arm920t_t *c, size_t offset) {
#if defined(__SWITCH__)
    return (uint8_t *)jitGetRxAddr(&c->jit_memory) + offset;
#elif defined(ARM_JIT_IOS_A64)
    return c->jit_ios_rx + offset;
#else
    return c->jit_code + offset;
#endif
}

#if defined(__SWITCH__)
static void a64_disable_exec(arm920t_t *c) {
    c->jit_memory_failed = 1;
    /* A failed permission transition may leave the entire RX view unmapped.
     * No previously compiled entry may remain callable in that case. Keep
     * decoded blocks and host accelerators available to the interpreter. */
    for (size_t i = 0; i < ARM_JIT_BLOCK_COUNT; ++i) {
        arm_jit_block_t *b = &c->jit_blocks[i];
        if (b->native_ok == 1u) {
            b->native = NULL;
            b->native_ok = 2u;
        }
    }
}
#endif

static int a64_prepare_write(arm920t_t *c) {
#if defined(ARM_JIT_IOS_A64)
    return c->jit_ios_ready;
#elif defined(__SWITCH__)
    if (R_FAILED(jitTransitionToWritable(&c->jit_memory))) {
        a64_disable_exec(c);
        return 0;
    }
#else
    GP32_UNUSED(c);
#if defined(ARM_JIT_DARWIN_A64)
    pthread_jit_write_protect_np(0);
#endif
#endif
    return 1;
}

static int a64_publish(arm920t_t *c, size_t hot, size_t hot_bytes,
                       size_t cold, size_t cold_bytes) {
#if defined(ARM_JIT_IOS_A64)
    if (!c->jit_ios_ready) return 0;
    if (hot_bytes) {
        sys_dcache_flush(c->jit_code + hot, hot_bytes);
        sys_icache_invalidate(a64_exec_addr(c, hot), hot_bytes);
    }
    if (cold_bytes) {
        sys_dcache_flush(c->jit_code + cold, cold_bytes);
        sys_icache_invalidate(a64_exec_addr(c, cold), cold_bytes);
    }
#elif defined(__SWITCH__)
    Jit *j = &c->jit_memory;
    if (j->type == JitType_CodeMemory) {
        /* libnx's transition flushes the entire allocation. CodeMemory keeps
         * both mappings live, so only publish the newly written ranges. */
        if (hot_bytes) armDCacheFlush(c->jit_code + hot, hot_bytes);
        if (cold_bytes) armDCacheFlush(c->jit_code + cold, cold_bytes);
        if (hot_bytes) armICacheInvalidate(a64_exec_addr(c, hot), hot_bytes);
        if (cold_bytes) armICacheInvalidate(a64_exec_addr(c, cold), cold_bytes);
        j->is_executable = true;
    } else if (R_FAILED(jitTransitionToExecutable(j))) {
        a64_disable_exec(c);
        return 0;
    }
#else
#if defined(ARM_JIT_DARWIN_A64)
    pthread_jit_write_protect_np(1);
#endif
    arm_jit_flush_exec(c->jit_code + hot, hot_bytes);
    if (cold_bytes) arm_jit_flush_exec(c->jit_code + cold, cold_bytes);
#endif
    return 1;
}

/* Retire every block whose code lives in [cs, ce) of the arena before that
 * range is overwritten. Runs only while compiling from the dispatcher, when
 * no native code is active. An evicted slot is invalid, so its next lookup
 * misses and retranslates from current guest memory, exactly as a block that
 * was never cached; linked exits already reject invalid slots. Accelerator
 * entries are host functions outside the arena and stay. */
static void a64_evict_chunk(arm920t_t *c, size_t cs, size_t ce) {
    uintptr_t lo = (uintptr_t)a64_exec_addr(c, cs), hi = (uintptr_t)a64_exec_addr(c, ce);
    for (size_t i = 0; i < ARM_JIT_BLOCK_COUNT; ++i) {
        arm_jit_block_t *b = &c->jit_blocks[i];
        union { arm_jit_native_fn f; uintptr_t p; } cvt;
        cvt.p = 0;
        cvt.f = b->native;
        if (!b->native || cvt.p < lo || cvt.p >= hi) continue;
        b->valid = 0;
        b->native = NULL;
        b->native_ok = 0;
    }
}

/* Guest registers an op reads, from its ARM encoding. This only ranks which
 * registers are worth a cached copy; correctness never depends on it. */
ARM_FORCE_INLINE uint32_t a64_op_reads(const arm_jit_op_t *op) {
    uint32_t insn = op->insn, m = 0;
    unsigned rn = (insn >> 16) & 15u, rd = (insn >> 12) & 15u, rs = (insn >> 8) & 15u, rm = insn & 15u;
    switch (op->kind) {
    case ARM_JIT_OP_DATA: {
        unsigned opc = (insn >> 21) & 15u;
        if (opc != 13u && opc != 15u) m |= 1u << rn;
        if (!(insn & (1u << 25))) {
            m |= 1u << rm;
            if (insn & (1u << 4)) m |= 1u << rs;
        }
        break;
    }
    case ARM_JIT_OP_MUL:
        m |= (1u << rm) | (1u << rs);
        if (insn & (1u << 21)) m |= (insn & (1u << 23)) ? (1u << rd) | (1u << rn) : 1u << rd;
        break;
    case ARM_JIT_OP_SINGLE_DT:
        m |= 1u << rn;
        if (insn & (1u << 25)) m |= 1u << rm;
        if (!(insn & (1u << 20))) m |= 1u << rd;
        break;
    case ARM_JIT_OP_HALF:
        m |= 1u << rn;
        if (!(insn & (1u << 22))) m |= 1u << rm;
        if (!(insn & (1u << 20))) m |= 1u << rd;
        break;
    case ARM_JIT_OP_BLOCK_DT:
        m |= 1u << rn;
        if (!(insn & (1u << 20))) m |= insn & 0xffffu;
        break;
    default:
        break;
    }
    return m & 0x7fffu;
}

/* Cache registers read at least twice. Prefer reads in the repeating prefix
 * over a long fallthrough tail; use whole-trace reads to break ties. A trace
 * without a native self backedge keeps the original ordering. All copies
 * remain write-through and helpers still reload them before continuing. */
static ARM_NOINLINE void a64_pick_cache_short(a64_emit_t *e,
                                           const arm_jit_op_t *ops,
                                           unsigned count) {
    /* With two ops, only their common reads qualify. Each eligible register
     * has exactly two whole-trace reads and the same prefix count, so the
     * existing tie order is simply the ascending guest register number. */
    uint32_t common = count == 2u ? a64_op_reads(&ops[0]) & a64_op_reads(&ops[1]) : 0u;
    for (unsigned k = 0; common && k < A64_CACHE_REGS; ++k) {
        unsigned r = arm_ctz16(common);
        e->cache_host[r] = a64_cache_pool[k];
        common &= common - 1u;
    }
}

/* Keep register ranking out of the large block-emission function. */
static ARM_NOINLINE void a64_pick_cache(a64_emit_t *e, const arm_jit_op_t *ops, unsigned count) {
    if (count <= 2u) {
        a64_pick_cache_short(e, ops, count);
        return;
    }
    /* Five register counts fit in separate bytes of one 64-bit word. Each
     * op contributes at most one to a lane, so no lane can carry into the
     * next within the maximum block length. Keep the repeating-prefix counts
     * as a snapshot; unpack once for the unchanged ranking below. The
     * multiplier has bits at 0,7,14,21,28: multiplying five input bits and
     * masking byte bit 0 spreads bit i to bit 8*i without overlapping terms. */
    _Static_assert(ARM_JIT_MAX_INSNS < 256u, "byte-wide register counts");
    uint64_t lo = 0, mid = 0, hi = 0, loop_lo = 0, loop_mid = 0, loop_hi = 0;
    const unsigned prefix = e->loop_done;
    for (unsigned i = 0; i < count; ++i) {
        if (i == prefix) { loop_lo = lo; loop_mid = mid; loop_hi = hi; }
        uint32_t m = a64_op_reads(&ops[i]);
        lo += ((uint64_t)(m & 31u) * UINT64_C(0x10204081)) & UINT64_C(0x0101010101);
        mid += ((uint64_t)((m >> 5) & 31u) * UINT64_C(0x10204081)) & UINT64_C(0x0101010101);
        hi += ((uint64_t)(m >> 10) * UINT64_C(0x10204081)) & UINT64_C(0x0101010101);
    }
    if (prefix >= count) { loop_lo = lo; loop_mid = mid; loop_hi = hi; }
    unsigned reads[15], loop_reads[15];
    for (unsigned r = 0; r < 5u; ++r) {
        reads[r] = (unsigned)(lo >> (r * 8u)) & 255u;
        reads[r + 5u] = (unsigned)(mid >> (r * 8u)) & 255u;
        reads[r + 10u] = (unsigned)(hi >> (r * 8u)) & 255u;
        loop_reads[r] = (unsigned)(loop_lo >> (r * 8u)) & 255u;
        loop_reads[r + 5u] = (unsigned)(loop_mid >> (r * 8u)) & 255u;
        loop_reads[r + 10u] = (unsigned)(loop_hi >> (r * 8u)) & 255u;
    }
    for (unsigned k = 0; k < A64_CACHE_REGS; ++k) {
        unsigned best = 15u;
        for (unsigned r = 0; r < 15u; ++r)
            if (!e->cache_host[r] && reads[r] >= 2u && (best == 15u || loop_reads[r] > loop_reads[best] ||
                 (loop_reads[r] == loop_reads[best] && reads[r] > reads[best]))) best = r;
        if (best == 15u) break;
        e->cache_host[best] = a64_cache_pool[k];
    }
}

static int a64_emit_runtime(arm920t_t *c) {
    /* Build privately so failed emission never leaves a JIT mapping writable. */
    uint32_t runtime[A64_RT_BYTES / sizeof(uint32_t)] = {0};
    a64_emit_t e;
    memset(&e, 0, offsetof(a64_emit_t, ext_at));
    e.code = runtime;
    e.cap = A64_RT_BYTES / sizeof(uint32_t);
    const size_t ctl = offsetof(arm920t_t, irq_line);
    size_t fail[8];
    unsigned nf = 0;

    a64_u32(&e, 0xa90157f4u); /* stp x20, x21, [sp, #16] */
    a64_u32(&e, 0xa9025ff6u); /* stp x22, x23, [sp, #32] */
    a64_u32(&e, 0xa90367f8u); /* stp x24, x25, [sp, #48] */
    a64_u32(&e, 0xa9046ffau); /* stp x26, x27, [sp, #64] */
    a64_u32(&e, 0xa90577fcu); /* stp x28, x29, [sp, #80] */
    a64_rr(&e, 0xaa000000u, 19, 31, 0); /* mov x19, x0 */
    a64_cpu_addr(&e, 20, offsetof(arm920t_t, jit_tlb_page));
    a64_cpu_addr(&e, 25, ctl);
    a64_cpu_mem(&e, 0xf9400000u, 21, offsetof(arm920t_t, jit_ram_base), 3);
    a64_cpu_mem(&e, 0xf9400000u, 24, offsetof(arm920t_t, jit_generation), 3);
    a64_mov(&e, 22, 1);
    a64_mov(&e, 23, 31);
    a64_u32(&e, 0xd65f03c0u); /* ret into the body */
    if (e.pos > A64_RT_EXIT / 4u) return 0;
    while (e.pos < A64_RT_EXIT / 4u) a64_u32(&e, 0xd4200000u); /* brk #0 */

    a64_rr(&e, 0x0b000000u, 0, 0, 23); /* add w0, w0, w23 */
    a64_u32(&e, 0xa94577fcu); /* ldp x28, x29, [sp, #80] */
    a64_u32(&e, 0xa9446ffau); /* ldp x26, x27, [sp, #64] */
    a64_u32(&e, 0xa94367f8u); /* ldp x24, x25, [sp, #48] */
    a64_u32(&e, 0xa9425ff6u); /* ldp x22, x23, [sp, #32] */
    a64_u32(&e, 0xa94157f4u); /* ldp x20, x21, [sp, #16] */
    a64_u32(&e, 0xa8c67bf3u); /* ldp x19, x30, [sp], #96 */
    a64_u32(&e, 0xd65f03c0u); /* ret to dispatch */
    if (e.pos > A64_RT_HOP / 4u) return 0;
    while (e.pos < A64_RT_HOP / 4u) a64_u32(&e, 0xd4200000u);

    /* The dispatcher's loop and fast path, in its order of evidence: */
    a64_u32(&e, 0xf9400329u); /* ldr x9, [x25]: irq_line | fiq_line */
    a64_cpu_mem(&e, 0xf9400000u, 12, offsetof(arm920t_t, jit_blocks), 3);
    a64_rr(&e, 0x4b000000u, 10, 22, 0); /* w10 = budget left after this block */
    /* x12 = first way of arm_jit_block_index(pc) */
    a64_ubfx(&e, 11, 16, 16, 16);
    a64_u32(&e, 0x4a400000u | (16u << 16) | (2u << 10) | (11u << 5) | 11u); /* eor w11, w11, w16, lsr #2 */
    a64_mask(&e, 11, 11, ARM_JIT_BLOCK_MASK >> 1);
    a64_u32(&e, 0x8b000000u | (11u << 16) | (6u << 10) | (12u << 5) | 12u); /* add x12, x12, x11, lsl #6 */
    fail[nf++] = e.pos;
    a64_branch_word(&e, 0xb5000000u | 9u); /* cbnz x9: an interrupt line is raised */
    a64_u32(&e, 0xa940398du); /* ldp x13, x14, [x12]: {tag, valid}, {generation, epoch} */
    a64_cpu_mem(&e, 0xf9400000u, 9, offsetof(arm920t_t, jit_generation), 3); /* live pair */
    a64_u32(&e, 0xf940098fu); /* ldr x15, [x12, #16]: count, native_ok, poll, deferred */
    a64_u32(&e, 0xb260020bu); /* orr x11, x16, #(1 << 32): {pc, valid = 1} */
    a64_u32(&e, 0xeb0b01bfu); /* cmp x13, x11 */
    a64_u32(&e, 0xfa4901c0u); /* ccmp x14, x9, #0, eq */
    fail[nf++] = a64_jump(&e, 1); /* NE: another block, stale or invalid */
    a64_u32(&e, 0xf26799ffu); /* tst x15, #0xfffffffffe000000 */
    fail[nf++] = a64_jump(&e, 1); /* NE: counted poll or deferred inline target */
    a64_mask(&e, 13, 15, 0xff00u);
    a64_cmp_imm(&e, 13, 0x100u);
    fail[nf++] = a64_jump(&e, 1); /* native_ok != 1: uncompiled, failed or accelerator */
    a64_mask(&e, 13, 15, 0xffu);
    a64_cmp(&e, 10, 13);
    fail[nf++] = a64_jump(&e, 3); /* LO: the budget does not cover the block */
#if ARM920T_PROFILING
    a64_cpu_mem(&e, 0xf9400000u, 13, offsetof(arm920t_t, prof.native_chain_hops), 3);
    a64_u32(&e, 0x910005adu); /* add x13, x13, #1 */
    a64_cpu_mem(&e, 0xf9000000u, 13, offsetof(arm920t_t, prof.native_chain_hops), 3);
#endif
    a64_u32(&e, 0xf9400d91u); /* ldr x17, [x12, #24]: native entry */
    a64_mov(&e, 22, 10);
    a64_rr(&e, 0x0b000000u, 23, 23, 0); /* add w23, w23, w0 */
    a64_rr(&e, 0xaa000000u, 24, 31, 9); /* mov x24, x9 */
    a64_u32(&e, 0x91000000u | ((A64_BODY_WORDS * 4u) << 10) | (17u << 5) | 17u); /* body */
    a64_u32(&e, 0xd65f03c0u); /* ret */
    for (unsigned i = 0; i < nf; ++i) a64_patch(&e, fail[i], e.pos);
    int32_t rel = (int32_t)A64_RT_EXIT - (int32_t)(e.pos * 4u);
    a64_u32(&e, 0x10000000u | (((uint32_t)rel & 3u) << 29) |
                ((((uint32_t)rel >> 2) & 0x7ffffu) << 5) | 17u); /* adr x17, rt_exit */
    a64_u32(&e, 0xd65f03c0u); /* ret */
    if (e.fail) return 0;
    if (!a64_prepare_write(c)) return 0;
    memcpy(c->jit_code, runtime, sizeof(runtime));
    return a64_publish(c, 0, A64_RT_BYTES, 0, 0);
}

/* Translation must not be inlined into the guest execution dispatcher. */
static ARM_NOINLINE void arm_jit_compile_host(arm920t_t *c, arm_jit_block_t *b) {
    if (!c || !b || !b->count) return;
#if defined(__SWITCH__)
    if (c->jit_memory_failed) return;
#endif
    uint32_t tmp[ARM_JIT_NATIVE_MAX_BYTES / sizeof(uint32_t)];
    a64_reloc_t reloc[A64_RELOC_MAX];
    a64_cold_op_t cold[ARM_JIT_MAX_INSNS];
    a64_fetch_fix_t fetch[ARM_JIT_MAX_INSNS];
    a64_emit_t e;
    memset(&e, 0, offsetof(a64_emit_t, ext_at));
    e.code = tmp;
    e.reloc = reloc;
    e.cap = sizeof(tmp) / sizeof(tmp[0]);
    e.cold = cold;
    e.fetch = fetch;
    e.cold_cap = ARM_JIT_MAX_INSNS;
    e.fwd_guest = -1;
    e.mmu = !!(c->cp15[1] & 1u);
    e.check_code_writes = !(c->cp15[1] & (1u << 12));
    e.leaf_sample = c;
    e.leaf_write32 = c->leaf_write32;
    e.leaf_write32_count = c->leaf_write32_count <= 8u ? c->leaf_write32_count : 0u;
    for (size_t i = 0; i < e.leaf_write32_count; ++i)
        if (arm_jit_nonram_region(e.leaf_write32[i].pa) == ARM_JIT_NONRAM_OTHER &&
            fastmem_excludes(c, e.leaf_write32[i].pa, 4u, 1)) e.leaf_write32_mask |= 1u << i;
    e.live_read32 = c->live_read32;
    e.live_read32_count = c->live_read32_count;
    e.loop_done = a64_selfloop_prefix(c, b);
#if ARM920T_PROFILING
    c->profile_native_variable_count[b - c->jit_blocks] = e.loop_done != 0;
#endif
    e.block_count = b->count;
    e.counted = (b->poll_backedge >> 1) != 0;
    /* Validate the complete direct window, including write permission.
     * Tiny or discontinuous test buses must retain their callback path. */
    e.ram_read = c->jit_ram_base && fastmem(c, ARM_JIT_RAM_BASE_ADDR, ARM_JIT_RAM_SIZE_BYTES, 0) == c->jit_ram_base;
    e.ram_write = c->jit_ram_base && fastmem(c, ARM_JIT_RAM_BASE_ADDR, ARM_JIT_RAM_SIZE_BYTES, 1) == c->jit_ram_base;
    /* A whole-window read probe authorizes BIOS loads. Open-bus inlining also
     * needs every possible RAM extension excluded: one rejected byte cannot
     * prove the absence of mappings at later addresses or for wider reads.
     * Requery the certificate whenever mapping flushes force compilation. */
    e.bios_read = c->jit_bios_base && fastmem(c, 0, ARM_JIT_BIOS_SIZE_BYTES, 0) == c->jit_bios_base;
    e.open_bus = c->bus.open_bus_valid &&
        fastmem_excludes(c, ARM_JIT_RAM_BASE_ADDR + ARM_JIT_RAM_SIZE_BYTES,
            (size_t)UINT32_MAX - (ARM_JIT_RAM_BASE_ADDR + ARM_JIT_RAM_SIZE_BYTES) + 1u, 0);
    e.io_read_callback = fastmem_excludes(c, 0x14000000u, 0x02000000u, 0);
    e.io_write_callback = fastmem_excludes(c, 0x14000000u, 0x02000000u, 1);
    a64_init_helpers(c);
    e.helper_code = c->jit_helper_code;
    /* Counted and plain poll sources keep returning to the dispatcher, whose
     * general path owns their observation and skip bookkeeping. */
    e.hop = ARM_JIT_A64_LINK && !b->poll_backedge;
    a64_u32(&e, 0xd503245fu); /* bti c (NOP on ARMv8.0) */
    /* The dispatcher normally guarantees a full block budget. Keep the
     * function itself safe for short budgets too: that rare rejection leaves
     * through the cold entry exit behind the block, before any frame exists,
     * and leaves the partial block to portable bytecode. */
    a64_cmp_imm(&e, 1, b->count);
    e.entry_exit = a64_jump(&e, 3); /* LO */
    /* The caller's x19/LR open the shared frame; rt_enter saves the rest,
     * establishes the register convention and returns into the body. */
    a64_u32(&e, 0xa9ba7bf3u); /* stp x19, x30, [sp, #-96]! */
    a64_ext(&e, 0x94000000u, A64_EXT_ENTER); /* bl rt_enter */
    if (e.pos != A64_BODY_WORDS) e.fail = 1;
    /* The body starts here for the dispatcher and for linked exits alike.
     * Load the cached guest registers once; the self backedge re-enters
     * behind these loads with every copy still equal to c->r[]. */
    a64_pick_cache(&e, arm_jit_ops(c, b), b->count);
    for (unsigned r = 0; r < 15u; ++r)
        if (e.cache_host[r]) a64_load(&e, e.cache_host[r], offsetof(arm920t_t, r) + r * 4u);
    e.loop_head = e.pos;
    int copy_loop = e.loop_done == 5u && a64_copy_loop(&e, arm_jit_ops(c, b));
    if (e.loop_done == 24u) a64_packed_word_loop(&e, arm_jit_ops(c, b));
#if ARM920T_PROFILING
    if (copy_loop) c->profile_native_variable_count[b - c->jit_blocks] = 1u;
#else
    (void)copy_loop;
#endif
    int stopped = 0;
    /* Pending in-block forward branches: site and target op index. Poll
     * sources keep their dispatcher-owned repetition bookkeeping, which
     * compares a returned count with the full block, so they never skip. */
    size_t fwd_site[ARM_JIT_MAX_INSNS];
    uint16_t fwd_to[ARM_JIT_MAX_INSNS];
    unsigned nfwd = 0;
    const int fwd_ok = !b->poll_backedge;
    for (unsigned i = 0; i < b->count; ++i) {
        const arm_jit_op_t *op = &arm_jit_ops(c, b)[i];
        int joined = 0;
        for (unsigned k = 0; k < nfwd; ++k)
            if (fwd_to[k] == i) {
                a64_patch(&e, fwd_site[k], e.pos);
                fwd_to[k] = UINT16_MAX;
                joined = 1;
            }
        if (joined) {
            /* Both paths meet here with only c->r[]/CPSR and the
             * write-through register copies in common. */
            e.nzcv_valid = 0;
            e.fwd_guest = -1;
        }
        e.expected_next = i + 1u < b->count ? arm_jit_ops(c, b)[i + 1u].pc : op->pc + 4u;
        e.generation = b->generation;
        e.want_nzcv = i + 1u < b->count && arm_jit_ops(c, b)[i + 1u].cond < 14u;
        /* No TLB mutation on a resident I-side hit; semantic eviction
         * changes the epoch and checked helpers leave before continuing. */
        if (!b->poll_backedge && i + 5u <= b->count) {
            size_t sequence_success;
            if (a64_io_sequence(c, &e, op, i, &sequence_success)) {
                fwd_site[nfwd] = sequence_success;
                fwd_to[nfwd++] = (uint16_t)(i + 5u);
            }
        }
        uint32_t folded;
        unsigned nfold = arm_jit_fold_const_data(arm_jit_ops(c, b), b->count, i, &folded);
        if (e.mmu && nfold && (arm_jit_ops(c, b)[i + nfold - 1u].pc >> 10) != (op->pc >> 10)) nfold = 0;
        for (unsigned k = 0; nfold && k < nfwd; ++k)
            if (fwd_to[k] > i && fwd_to[k] < i + nfold) nfold = 0; /* keep the join point */
        if (nfold) {
            a64_imm(&e, 2, folded);
            a64_put_guest(&e, op->c, 2);
            e.fwd_guest = (int)op->c;
            e.nzcv_valid = 0;
            i += nfold - 1u; /* Every raw op keeps its PC and clock. */
            continue;
        }
        /* A register-only leaf preserves its BL link and execution state.
         * Keep the architectural PC visible to subsequent callbacks, but a
         * decoded caller successor needs no dynamic return-target guard. The
         * guest op/cycle count and guarded boundary returns remain intact. */
        if (op->reserved == ARM_JIT_RETURN_LEAF && i + 1u < b->count) {
            a64_store_imm(&e, offsetof(arm920t_t, r) + 15u * 4u, e.expected_next);
            continue;
        }
        if (op->reserved == 8u) {
            if (!e.nzcv_valid) {
                a64_load(&e, 3, offsetof(arm920t_t, cpsr));
                a64_u32(&e, 0xd51b4203u);
            }
            size_t taken = a64_jump(&e, op->cond);
            a64_exit_const(&e, op->pc + 4u, i + 1u);
            a64_patch(&e, taken, e.pos);
            e.nzcv_valid = 0;
            continue;
        }
        /* A self move preserves the guest value and flags; any preceding
         * forwarded result remains valid. Counts and exit PC commits stay. */
        if (arm_jit_is_side_effect_free_nop(op->insn)) continue;
        unsigned fwd = fwd_ok ? arm_jit_fwd_target(arm_jit_ops(c, b), b->count, i, e.loop_done) : 0u;
        if (fwd) {
            /* The skipped ops never run. The taken path returns them to the
             * entry budget (w22) and the completed count (w23), so every later
             * exit keeps its constant op count and still reports exactly the
             * instructions that ran. With prefix P and skips S, w23+P is the
             * retired count and w22-P is the remaining budget. P >= S at
             * every exit, so 32-bit wrap restores the correct unsigned value.
             * A wrapped w22 can only make the self-loop budget guard return
             * early; dispatch then resumes with the real remaining budget. */
            unsigned skipped = fwd - i - 1u;
            if (!skipped) continue; /* a branch to the next op changes nothing */
#if ARM920T_PROFILING
            c->profile_native_variable_count[b - c->jit_blocks] = 1u;
#endif
            if (!e.nzcv_valid) {
                a64_load(&e, 3, offsetof(arm920t_t, cpsr));
                a64_u32(&e, 0xd51b4203u);
            }
            size_t fall = a64_jump(&e, op->cond ^ 1u);
            a64_u32(&e, 0x11000000u | (skipped << 10) | (22u << 5) | 22u); /* add w22, w22, #skipped */
            a64_u32(&e, 0x51000000u | (skipped << 10) | (23u << 5) | 23u); /* sub w23, w23, #skipped */
            fwd_site[nfwd] = a64_jump(&e, 14u);
            fwd_to[nfwd++] = (uint16_t)fwd;
            a64_patch(&e, fall, e.pos);
            e.nzcv_valid = 1; /* the fall-through still holds the committed flags */
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
            /* A branch reaches this exit only when its condition failed; a
             * never-executed op changes nothing. Other stop ops (PSR writes,
             * SWI, coprocessor) may have changed control state here, so the
             * dispatcher observes them before anything else runs. */
            /* A non-stitched LDM-to-PC also always exits when executed,
             * through a64_block_dt_exit or the terminating classified helper.
             * Its epilogue is reached only on a failed condition. Reuse that
             * epilogue for a guarded hop; no extra executed-stop tail is needed. */
            int skipped_ldm_pc = op->cond < 14u && !op->reserved &&
                op->kind == ARM_JIT_OP_BLOCK_DT && (op->d & 1u) && (op->imm & 0x8000u);
            if (op->kind == ARM_JIT_OP_BRANCH || op->cond == 15u || skipped_ldm_pc) {
                a64_exit_const(&e, op->pc + 4u, i + 1u);
            } else {
                a64_store_imm(&e, offsetof(arm920t_t, r) + 15u * 4u, op->pc + 4u);
                a64_return(&e, i + 1u);
            }
            stopped = 1;
            break;
        }
    }
    for (unsigned k = 0; k < nfwd; ++k) {
        /* A region ending at the decode limit joins the ordinary block exit. */
        if (fwd_to[k] == b->count) {
            a64_patch(&e, fwd_site[k], e.pos);
            fwd_to[k] = UINT16_MAX;
        }
        if (fwd_to[k] != UINT16_MAX) e.fail = 1; /* every target is a later op */
    }
    if (!stopped) {
        /* The last op can itself be a stitched branch at the block-size limit. */
        const arm_jit_op_t *last = &arm_jit_ops(c, b)[b->count - 1u];
        uint32_t final_pc = last->pc + 4u;
        if (last->kind == ARM_JIT_OP_BRANCH && last->cond == 14u)
            final_pc = arm_jit_branch_target(last->pc, last->insn);
        a64_exit_const(&e, final_pc, b->count);
    }
    e.hot_end = e.pos;
    if (e.entry_exit) {
        a64_patch_cold(&e, e.entry_exit, e.pos);
        a64_imm(&e, 0, 0);
        a64_u32(&e, 0xd65f03c0u); /* ret: no frame was opened */
    }
    a64_fetch_flush(&e);
    a64_cold_flush(&e);
    if (e.fail) return;
    size_t hot_bytes = e.hot_end * sizeof(tmp[0]);
    size_t cold_bytes = (e.pos - e.hot_end) * sizeof(tmp[0]);
    if (!c->jit_code) {
        c->jit_code_size = ARM_JIT_CODE_SIZE;
        c->jit_code = arm_jit_alloc_exec(c, c->jit_code_size);
        if (!c->jit_code) { c->jit_code_size = c->jit_code_used = 0; ARM_PROF_INC(c, jit_code_alloc_failures); return; }
        c->jit_cold_floor = 0;
        if (!a64_emit_runtime(c)) {
            arm_jit_free_exec(c, c->jit_code, c->jit_code_size);
            c->jit_code = NULL;
            c->jit_code_size = c->jit_code_used = 0;
            ARM_PROF_INC(c, jit_code_alloc_failures);
            return;
        }
    }
    size_t start = (c->jit_code_used + 15u) & ~(size_t)15u, cold_at;
    if (start < A64_RT_BYTES) start = A64_RT_BYTES; /* the runtime stubs stay resident */
    int wrapped_here = 0;
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
        c->jit_cold_floor = 0;
        if (ce >= c->jit_code_size) {
            /* Full: wrap to the first chunk instead of flushing every block.
             * From now on each chunk is evicted just before it is reused, so
             * a full arena retranslates one chunk's blocks at a time. */
            if (wrapped_here) return; /* cannot fit even an empty chunk */
            wrapped_here = 1;
            ARM_PROF_INC(c, jit_code_full_events);
            c->jit_arena_wrapped = 1;
            start = A64_RT_BYTES;
        } else {
            start = ce;
        }
        if (c->jit_arena_wrapped) {
            size_t ns = start & ~(size_t)(ARM_JIT_A64_CHUNK - 1u);
            a64_evict_chunk(c, ns, c->jit_code_size - ns > ARM_JIT_A64_CHUNK ? ns + ARM_JIT_A64_CHUNK : c->jit_code_size);
        }
    }
    if (!a64_prepare_write(c)) return;
    uint32_t *hot_dst = (uint32_t *)(void *)(c->jit_code + start);
    uint32_t *cold_dst = (uint32_t *)(void *)(c->jit_code + cold_at);
    if (!a64_place(hot_dst, cold_dst, tmp, e.hot_end, e.pos, reloc, e.reloc_count)) {
        c->jit_cold_floor = cold_at + cold_bytes;
        /* Restore old entries on permission-based libnx after a placement
         * failure. No new entry is published. */
        a64_publish(c, 0, 0, 0, 0);
        return;
    }
    /* Link the runtime-stub branches at their final addresses. The arena is
     * smaller than the +-128 MiB B/BL range, so every link reaches. */
    for (unsigned k = 0; k < e.ext_count; ++k) {
        static const uint32_t stub[3] = {A64_RT_EXIT, A64_RT_HOP, A64_RT_ENTER};
        size_t i = e.ext_at[k];
        uint32_t *at = i < e.hot_end ? hot_dst + i : cold_dst + (i - e.hot_end);
        intptr_t d = ((intptr_t)(c->jit_code + stub[e.ext_to[k]]) - (intptr_t)at) / 4;
        *at = (*at & 0xfc000000u) | ((uint32_t)d & 0x03ffffffu);
    }
    if (!a64_publish(c, start, hot_bytes, cold_at, cold_bytes)) return;
    c->jit_code_used = start + hot_bytes;
    void *dst = a64_exec_addr(c, start);
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
    a64_guest(e, 0, op->b, op->pc + 4u);                  /* Rn PC = instruction+12 for register shifts */
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
        if (op->c == 15u) a64_hop_reg(e, 2, done);
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

/* Privileged local p15 operations have no bus callback or control-flow effects. Reads
 * mirror cp15_read; allowed MCR data-cache/drain operations only record c7,
 * matching cp15_write. I-cache/MMU maintenance retains the checked helper. */
static int a64_local_p15(a64_emit_t *e, const arm_jit_op_t *op, uint32_t done) {
    const uint32_t insn = op->insn;
    if (!arm_jit_is_local_cp15_op(insn)) return 0;
    /* A cached block can run in USER mode after it was compiled in SVC/SYS.
     * No CP15 effect is allowed before this live check. */
    a64_load(e, 1, offsetof(arm920t_t, cpsr));
    a64_mask(e, 1, 1, MODE_MASK);
    a64_cmp_imm(e, 1, MODE_USR);
    size_t privileged = a64_jump(e, 1u); /* NE */
    a64_rr(e, 0xaa000000u, 0, 31, 19);
    a64_ptr(e, 1, (uintptr_t)op);
    a64_helper_call(e, A64_HELPER_ARM_JIT_EXEC_CLASSIFIED);
    a64_return(e, done);
    a64_patch(e, privileged, e->pos);
    unsigned crn = (insn >> 16) & 0xfu, rd = (insn >> 12) & 0xfu;
    if (!GP32_BIT(insn, 20)) {
        a64_guest(e, 1, rd, op->pc);
        a64_store(e, 1, offsetof(arm920t_t, cp15) + crn * 4u);
    } else if (crn == 0u) {
        a64_imm(e, 16, ((insn >> 5) & 7u) == 1u ? 0x0d172172u : 0x41129200u);
        a64_put_guest(e, rd, 16);
    } else {
        a64_load(e, 1, offsetof(arm920t_t, cp15) + crn * 4u);
        a64_put_guest(e, rd, 1);
    }
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
            if (!op->stop) return 0;
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
                a64_branch_word(e, 0x35000006u); /* cbnz w6 -> shared bank-switch helper */
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
                a64_helper_call(e, A64_HELPER_ARM_A64_MSR_CPSR);
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
