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
 */
#ifndef ARM_JIT_CODE_SIZE
#define ARM_JIT_CODE_SIZE (64u * 1024u * 1024u)
#endif

typedef struct a64_emit {
    uint32_t *code;
    size_t pos, cap;
    int fail;
    int ram_read, ram_write, memory;
    uint32_t expected_next, generation;
} a64_emit_t;

static int arm_a64_exec_checked(arm920t_t *c, const arm_jit_op_t *op,
                                uint32_t expected_next, uint32_t generation) {
    int stop = op->stop;
    uint32_t status = c->cpsr & 0xffu;
    uint8_t *ram_base = c->jit_ram_base;
    arm_jit_exec_classified(c, op);
    return !stop && !c->halted && !thumb(c) && !c->trace &&
           c->jit_generation == generation && c->jit_ram_base == ram_base &&
           (c->cpsr & 0xffu) == status &&
           (c->r[15] & ~3u) == expected_next &&
           !(c->irq_line && !(c->cpsr & I_FLAG)) &&
           !(c->fiq_line && !(c->cpsr & F_FLAG));
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

static void a64_cpu_addr(a64_emit_t *e, unsigned rd, size_t off) {
    a64_imm(e, rd, (uint32_t)off);
    a64_rr(e, 0x8b000000u, rd, 19, rd);
}

static void a64_cpu_mem(a64_emit_t *e, uint32_t opcode, unsigned rt, size_t off, unsigned scale) {
    unsigned rn = 19;
    if ((off & ((1u << scale) - 1u)) || (off >> scale) > 4095u) {
        a64_cpu_addr(e, 17, off);
        rn = 17;
        off = 0;
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

/* All fixups are forward within the temporary block (word indices). */
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
static void a64_cmp(a64_emit_t *e, unsigned rn, unsigned rm) {
    a64_rr(e, 0x6b000000u, 31, rn, rm);
}
static void a64_cmp_imm(a64_emit_t *e, unsigned rn, unsigned imm) {
    a64_u32(e, 0x7100001fu | (imm << 10) | (rn << 5));
}
static void a64_return(a64_emit_t *e, uint32_t done) {
    a64_imm(e, 0, done);
    a64_u32(e, 0xf9400bf3u); /* ldr x19, [sp, #16] */
    if (e->memory) {
        a64_u32(e, 0xf9400ff4u); /* ldr x20, [sp, #24] */
        a64_u32(e, 0xf94013f5u); /* ldr x21, [sp, #32] */
    }
    a64_u32(e, 0xa8c37bfdu); /* ldp x29, x30, [sp], #48 */
    a64_u32(e, 0xd65f03c0u); /* ret */
}
static void a64_slow(a64_emit_t *e, const arm_jit_op_t *op, uint32_t done) {
    a64_rr(e, 0xaa000000u, 0, 31, 19);
    a64_ptr(e, 1, (uintptr_t)op);
    a64_imm(e, 2, e->expected_next);
    a64_imm(e, 3, e->generation);
    a64_ptr(e, 16, (uintptr_t)arm_a64_exec_checked);
    a64_u32(e, 0xd63f0200u); /* blr x16; helper commits PC and CPSR */
    a64_cmp_imm(e, 0, 0);
    size_t next = a64_jump(e, 1);
    a64_return(e, done);
    a64_patch(e, next, e->pos);
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

static int a64_data(a64_emit_t *e, const arm_jit_op_t *op, uint32_t done) {
    unsigned code = op->a;
    int test = code >= 8u && code <= 11u;
    int flags = test || (op->d & ARM_BC_DATA_S);
    int logical = code < 2u || code == 8u || code == 9u || code >= 12u;
    if ((op->d & ARM_BC_DATA_REGSHIFT) || (!test && op->c == 15u && flags)) return 0;
    int arith_imm = (op->d & ARM_BC_DATA_IMM) &&
        (code == 2u || code == 4u || code == 10u || code == 11u) &&
        (op->imm <= 0xfffu || (!(op->imm & 0xfffu) && (op->imm >> 12) <= 0xfffu));
    uint32_t imm_bits = 0;
    if (arith_imm)
        imm_bits = op->imm <= 0xfffu ? op->imm << 10 :
                   0x00400000u | ((op->imm >> 12) << 10);
    if (code != 13u && code != 15u) a64_guest(e, 0, op->b, op->pc);
    if (flags || (code >= 5u && code <= 7u) ||
        (!(op->d & ARM_BC_DATA_IMM) && op->g == 3u && !op->h))
        a64_load(e, 3, offsetof(arm920t_t, cpsr));
    if (op->d & ARM_BC_DATA_IMM) {
        if (!arith_imm) a64_imm(e, code == 13u ? 2u : 1u, op->imm);
        if (flags && logical) {
            if (op->d & ARM_BC_DATA_IMM_CV) a64_imm(e, 4, (op->d & ARM_BC_DATA_IMM_C) ? C_FLAG : 0u);
            else a64_mask(e, 4, 3, C_FLAG);
        }
    } else {
        a64_guest(e, 1, op->e, op->pc);
        a64_shift(e, op->g, op->h, flags && logical);
    }
    uint32_t s = flags ? 0x20000000u : 0u;
    switch (code) {
    case 0: case 8: a64_rr(e, 0x0a000000u, 2, 0, 1); break;
    case 1: case 9: a64_rr(e, 0x4a000000u, 2, 0, 1); break;
    case 2: case 10:
        if (arith_imm) a64_u32(e, 0x51000000u | s | imm_bits | 2u); /* sub[s] w2, w0, #imm */
        else a64_rr(e, 0x4b000000u | s, 2, 0, 1);
        break;
    case 3: a64_rr(e, 0x4b000000u | s, 2, 1, 0); break;
    case 4: case 11:
        if (arith_imm) a64_u32(e, 0x11000000u | s | imm_bits | 2u); /* add[s] w2, w0, #imm */
        else a64_rr(e, 0x0b000000u | s, 2, 0, 1);
        break;
    case 5: case 6: case 7:
        a64_u32(e, 0xd51b4203u); /* msr nzcv, x3: ARM/A64 carry is no-borrow */
        a64_rr(e, (code == 5u ? 0x1a000000u : 0x5a000000u) | s,
               2, code == 7u ? 1u : 0u, code == 7u ? 0u : 1u);
        break;
    case 12: a64_rr(e, 0x2a000000u, 2, 0, 1); break;
    case 13:
        if (!(op->d & ARM_BC_DATA_IMM)) a64_mov(e, 2, 1);
        break;
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
        if (op->c == 15u) a64_mask(e, 2, 2, ~3u);
        a64_put_guest(e, op->c, 2);
        if (op->c == 15u) a64_return(e, done);
    }
    return 1;
}

/* w0 = virtual address -> in-RAM offset; w2/w11 are preserved. The derived
 * page tag proves the exact current TLB mapping and an entire physical 4 KiB
 * RAM page. Misses, tiny pages, non-RAM and fault-tolerant identity mappings
 * still use the classified helper. MMU-off accesses retain the range guard.
 * Word/block callers align w0 first; only a halfword can straddle the end of
 * the prevalidated physical page, so it keeps a whole-window end check. */
static void a64_ram_offset(a64_emit_t *e, size_t *slow, unsigned *nslow, unsigned bytes) {
    _Static_assert(sizeof(arm_jit_tlb_page_t) == 8u &&
                   offsetof(arm_jit_tlb_page_t, va_page) == 0u &&
                   offsetof(arm_jit_tlb_page_t, xor_offset) == 4u,
                   "native RAM page lookup layout");
    a64_load(e, 6, offsetof(arm920t_t, cp15) + 4u);
    size_t identity = e->pos;
    a64_u32(e, 0x36000006u); /* tbz w6, #0 -> identity (MMU off) */
    a64_ubfx(e, 8, 0, 12, 20); /* full VA page tag */
    a64_mask(e, 6, 8, 0xfffu); /* original (VA >> 12) & 0xfff index */
    a64_rr(e, 0x8b000c00u, 10, 20, 6); /* add x10, x20, x6, lsl #3 */
    a64_u32(e, 0x29402547u); /* ldp w7, w9, [x10]: VA page, XOR offset */
    a64_cmp(e, 8, 7);
    slow[(*nslow)++] = a64_jump(e, 1);
    a64_rr(e, 0x4a000000u, 0, 0, 9); /* VA XOR offset -> direct RAM offset */
    size_t mapped = a64_jump(e, 14);
    a64_patch(e, identity, e->pos);
    a64_imm(e, 6, ARM_JIT_RAM_BASE_ADDR);
    a64_rr(e, 0x4b000000u, 0, 0, 6);
    /* A word-aligned start and same-page block span cannot overrun RAM,
     * whose start/end are both 4 KiB aligned. */
    if (bytes == 2u) a64_patch(e, mapped, e->pos);
    a64_imm(e, 6, ARM_JIT_RAM_SIZE_BYTES - bytes);
    a64_cmp(e, 0, 6);
    slow[(*nslow)++] = a64_jump(e, 8); /* HI also rejects identity below RAM */
    if (bytes != 2u) a64_patch(e, mapped, e->pos);
}

static int a64_single_dt(a64_emit_t *e, const arm_jit_op_t *op, uint32_t done) {
    unsigned d = op->d;
    int load = !!(d & ARM_BC_SD_L), byte = !!(d & ARM_BC_SD_B);
    int wb = !(d & ARM_BC_SD_P) || (d & ARM_BC_SD_W);
    if (!(load ? e->ram_read : e->ram_write) || (wb && op->a == 15u)) return 0;
    size_t slow[4];
    unsigned nslow = 0;
    if (d & ARM_BC_SD_REGOFF) {
        a64_guest(e, 1, op->c, op->pc);
        if (op->e == 3u && !op->f) a64_load(e, 3, offsetof(arm920t_t, cpsr));
        a64_shift(e, op->e, op->f, 0);
    } else a64_imm(e, 1, op->imm);
    a64_guest(e, 0, op->a, op->pc);
    a64_rr(e, (d & ARM_BC_SD_U) ? 0x0b000000u : 0x4b000000u, 2, 0, 1);
    if (d & ARM_BC_SD_P) a64_mov(e, 0, 2);
    if (!byte) {
        a64_mask(e, 11, 0, 3u); /* retain ARM unaligned word rotation */
        a64_lsl(e, 11, 11, 3);
        a64_mask(e, 0, 0, ~3u);
    }
    a64_ram_offset(e, slow, &nslow, byte ? 1u : 4u);
    if (load) {
        /* w0 is already the in-RAM offset; register-offset addressing folds
         * the jit_ram_base add into the access itself. */
        a64_u32(e, (byte ? 0x38606800u : 0xb8606800u) | (0u << 16) | (21u << 5) | 1u);
        if (!byte) a64_rr(e, 0x1ac02c00u, 1, 1, 11); /* rorv w1, w1, w11 */
        /* Match bytecode LDR PC: ARM state, word alignment, no interworking. */
        if (op->b == 15u) a64_mask(e, 1, 1, ~3u);
        a64_put_guest(e, op->b, 1);
    } else {
        a64_guest(e, 1, op->b, op->pc);
        a64_u32(e, (byte ? 0x38206800u : 0xb8206800u) | (0u << 16) | (21u << 5) | 1u);
    }
    if (wb) a64_put_guest(e, op->a, 2); /* includes the helper's Rn==Rd order */
    if (load && op->b == 15u) a64_return(e, done);
    size_t next = a64_jump(e, 14);
    for (unsigned i = 0; i < nslow; ++i) a64_patch(e, slow[i], e->pos);
    a64_slow(e, op, done);
    a64_patch(e, next, e->pos);
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
 * slow jump lands before the first guest-visible store, so the helper
 * re-executes the whole instruction on unmodified state. */
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
    a64_rr(e, (d & ARM_BC_HALF_U) ? 0x0b000000u : 0x4b000000u, 2, 0, 1); /* w2 = base +/- off */
    if (p) a64_mov(e, 0, 2); /* pre-indexed accesses target the offset address */
    a64_ram_offset(e, slow, &nslow, (load && sh == 2u) ? 1u : 2u);
    if (load) {
        /* ldrh/ldrsb/ldrsh w1, [x21, x0]: A64 already yields the exact
         * zero/sign extension op_halfword_bc produces. */
        a64_u32(e, (sh == 1u ? 0x78606800u : sh == 2u ? 0x38e06800u : 0x78e06800u) |
                 (0u << 16) | (21u << 5) | 1u);
        a64_put_guest(e, op->b, 1);
    } else {
        a64_guest(e, 1, op->b, op->pc);
        a64_u32(e, 0x78206800u | (0u << 16) | (21u << 5) | 1u); /* strh w1, [x21, x0] */
    }
    if (wb) a64_put_guest(e, op->a, 2);
    size_t next = a64_jump(e, 14);
    for (unsigned i = 0; i < nslow; ++i) a64_patch(e, slow[i], e->pos);
    a64_slow(e, op, done);
    a64_patch(e, next, e->pos);
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
 * even (ARM) target -
 * write_pc_x sends odd values into Thumb state, which native blocks cannot
 * represent, so those re-execute on the helper with unmodified guest state.
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
         * Otherwise branch to a two-page path; with count <= 16 (64 bytes),
         * an aligned span can cross at most one 4 KiB boundary. */
        a64_u32(e, 0x11000000u | ((4u * (count - 1u)) << 10) | (0u << 5) | 5u); /* w5 = last */
        a64_rr(e, 0x4a000000u, 6, 0, 5); /* eor w6, w0, w5 */
        a64_ubfx(e, 6, 6, 12, 20);
        xpage = e->pos;
        a64_u32(e, 0x35000000u | 6u); /* cbnz w6 -> two-page path */
    }
    /* The span stays within this 4 KiB page, so proving its aligned first
     * word in a full RAM page also proves every transferred word. */
    a64_ram_offset(e, slow, &nslow, 4u);
    a64_rr(e, 0x8b000000u, 12, 21, 0); /* x12 = jit_ram_base + in-RAM offset */
    a64_block_dt_transfer(e, op, count, 0, slow, &nslow);
    if (w && (!load || !(list & (1u << rn))))
        a64_put_guest(e, rn, 2); /* a load containing Rn suppresses this */
    a64_block_dt_exit(e, op, done);
    size_t single_done = a64_jump(e, 14);
    if (count > 1u) a64_patch(e, xpage, e->pos);

    /* w11 retains virtual addr0 and w2 retains offset0 across the second
     * a64_ram_offset call. w2's normal writeback value is reconstructed after
     * both translations, so using it as a preserved temporary is safe. */
    a64_mov(e, 11, 0);
    a64_ram_offset(e, slow, &nslow, 4u);
    a64_mov(e, 2, 0);
    a64_mov(e, 0, 11);
    a64_u32(e, 0x11000000u | (0xfffu << 10)); /* add w0, w0, #0xfff */
    a64_mask(e, 0, 0, ~0xfffu); /* w0 = next 4 KiB boundary */
    a64_ram_offset(e, slow, &nslow, 4u);
    a64_mov(e, 4, 0); /* w4 = offset1 */

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
    a64_block_dt_transfer(e, op, count, 1, slow, &nslow);
    if (w && (!load || !(list & (1u << rn))))
        a64_put_guest(e, rn, 2);
    a64_block_dt_exit(e, op, done);

    size_t next = a64_jump(e, 14);
    for (unsigned i = 0; i < nslow; ++i) a64_patch(e, slow[i], e->pos);
    a64_slow(e, op, done);
    a64_patch(e, next, e->pos);
    /* Patch after next so both paths land after the helper, rather than
     * entering a64_slow a second time after a successful single-page move. */
    a64_patch(e, single_done, e->pos);
    return 1;
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
        a64_u32(e, 0x35000000u | 6u); /* cbnz w6 -> helper on Thumb target */
    }
    unsigned idx = 0;
    for (unsigned r = 0; r < 16u; ++r) {
        if (!(list & (1u << r))) continue;
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
                /* Even target: write_pc_x stores r15 = v & ~3 and clears T.
                 * The clear is a no-op because native blocks only run in ARM
                 * state. */
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
static int a64_mrc_p15(a64_emit_t *e, const arm_jit_op_t *op, uint32_t done);
static int a64_psr_mov(a64_emit_t *e, const arm_jit_op_t *op, uint32_t done);

static int a64_one(a64_emit_t *e, const arm_jit_op_t *op, uint32_t done) {
    if (op->kind == ARM_JIT_OP_DATA) {
        if (op->d & ARM_BC_DATA_REGSHIFT) return a64_data_regshift(e, op, done);
        return a64_data(e, op, done);
    }
    if (op->kind == ARM_JIT_OP_MUL) {
        if (op->insn & (1u << 23)) return a64_long_mul(e, op, done);
        return a64_mul(e, op, done);
    }
    if (op->kind == ARM_JIT_OP_HALF) return a64_half(e, op, done);
    if (op->kind == ARM_JIT_OP_SINGLE_DT) return a64_single_dt(e, op, done);
    if (op->kind == ARM_JIT_OP_BLOCK_DT) return a64_block_dt(e, op, done);
    if (op->kind == ARM_JIT_OP_PSR) return a64_psr_mov(e, op, done);
    if (op->kind == ARM_JIT_OP_COPROC) return a64_mrc_p15(e, op, done);
    if (op->kind == ARM_JIT_OP_BRANCH) {
        if (op->insn & (1u << 24))
            a64_store_imm(e, offsetof(arm920t_t, r) + 14u * 4u, op->pc + 4u);
        a64_store_imm(e, offsetof(arm920t_t, r) + 15u * 4u, arm_jit_branch_target(op->pc, op->insn));
        /* Inlined BL and stitched unconditional B follow the decoded trace. */
        if (op->reserved != 2u && (op->stop || !arm_jit_is_uncond_b_no_link(op->insn))) a64_return(e, done);
        return 1;
    }
    return 0;
}

static void arm_jit_compile_native(arm920t_t *c, arm_jit_block_t *b) {
    if (!c || !b || !b->count) return;
    uint32_t tmp[ARM_JIT_NATIVE_MAX_BYTES / sizeof(uint32_t)];
    a64_emit_t e = {0};
    e.code = tmp;
    e.cap = sizeof(tmp) / sizeof(tmp[0]);
    /* Validate the complete direct window, including write permission.
     * Tiny or discontinuous test buses must retain their callback path. */
    e.ram_read = c->jit_ram_base && fastmem(c, ARM_JIT_RAM_BASE_ADDR, ARM_JIT_RAM_SIZE_BYTES, 0) == c->jit_ram_base;
    e.ram_write = c->jit_ram_base && fastmem(c, ARM_JIT_RAM_BASE_ADDR, ARM_JIT_RAM_SIZE_BYTES, 1) == c->jit_ram_base;
    if (e.ram_read || e.ram_write) {
        for (unsigned i = 0; i < b->count; ++i)
            if (arm_jit_ops(c, b)[i].kind == ARM_JIT_OP_SINGLE_DT || arm_jit_ops(c, b)[i].kind == ARM_JIT_OP_HALF ||
                arm_jit_ops(c, b)[i].kind == ARM_JIT_OP_BLOCK_DT) { e.memory = 1; break; }
    }
    a64_u32(&e, 0xd503245fu); /* bti c (NOP on ARMv8.0) */
    a64_u32(&e, 0xa9bd7bfdu); /* stp x29, x30, [sp, #-48]! */
    a64_u32(&e, 0xf9000bf3u); /* str x19, [sp, #16] */
    /* Only memory fast paths use x20/x21; helpers preserve them by AAPCS64.
     * Keep the same aligned frame and x19/LR slots even without their saves. */
    if (e.memory) {
        a64_u32(&e, 0xf9000ff4u); /* str x20, [sp, #24] */
        a64_u32(&e, 0xf90013f5u); /* str x21, [sp, #32] */
    }
    a64_u32(&e, 0x910003fdu); /* mov x29, sp */
    a64_rr(&e, 0xaa000000u, 19, 31, 0);
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
    size_t enough = a64_jump(&e, 2); /* HS */
    a64_return(&e, 0);
    a64_patch(&e, enough, e.pos);
    for (unsigned i = 0; i < b->count; ++i) {
        const arm_jit_op_t *op = &arm_jit_ops(c, b)[i];
        e.expected_next = i + 1u < b->count ? arm_jit_ops(c, b)[i + 1u].pc : op->pc + 4u;
        e.generation = b->generation;
        size_t skip = 0;
        if (op->cond == 15u) {
            /* ARMv4 NV is never, unlike A64's condition 15 (always). */
            a64_store_imm(&e, offsetof(arm920t_t, r) + 15u * 4u, op->pc + 4u);
        } else {
            if (op->cond != 14u) {
                a64_load(&e, 3, offsetof(arm920t_t, cpsr));
                a64_u32(&e, 0xd51b4203u);
                skip = a64_jump(&e, op->cond ^ 1u);
            }
            if (!a64_one(&e, op, i + 1u)) a64_slow(&e, op, i + 1u);
            if (skip) a64_patch(&e, skip, e.pos);
            /* The next instruction sees its own pipeline PC; only exits and
             * helpers require a PC store. Taken control flow returned above. */
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
    a64_return(&e, b->count);
    if (e.fail) return;
    size_t bytes = e.pos * sizeof(tmp[0]);
    if (!c->jit_code) {
        c->jit_code_size = ARM_JIT_CODE_SIZE;
        c->jit_code = arm_jit_alloc_exec(c->jit_code_size);
        if (!c->jit_code) { c->jit_code_size = c->jit_code_used = 0; ARM_PROF_INC(c, jit_code_alloc_failures); return; }
    }
    size_t start = (c->jit_code_used + 15u) & ~(size_t)15u;
    if (bytes > c->jit_code_size || start > c->jit_code_size - bytes) { ARM_PROF_INC(c, jit_code_full_events); return; }
    void *dst = c->jit_code + start;
    memcpy(dst, tmp, bytes);
    arm_jit_flush_exec(dst, bytes);
    c->jit_code_used = start + bytes;
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
 * The two shape guards are identical: a flag-setting write to PC keeps the
 * helper's SPSR restore, and Rm/Rs == 15 keeps the helper's pipeline-PC read. */
static int a64_data_regshift(a64_emit_t *e, const arm_jit_op_t *op, uint32_t done) {
    unsigned code = op->a;
    int test = code >= 8u && code <= 11u;
    int flags = test || (op->d & ARM_BC_DATA_S);
    int logical = code < 2u || code == 8u || code == 9u || code >= 12u;
    if (!(op->d & ARM_BC_DATA_REGSHIFT) || op->e == 15u || op->f == 15u) return 0;
    if (!test && op->c == 15u && flags) return 0;
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
        if (op->c == 15u) a64_mask(e, 2, 2, ~3u);
        a64_put_guest(e, op->c, 2);
        if (op->c == 15u) a64_return(e, done);
    }
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

/* ARM_JIT_OP_COPROC p15 read, the reserved == 6 shape that
 * arm_jit_is_safe_cp15_mrc accepts (MRC, L=1, cp15, Rd != 15).  op_coproc
 * performs write_r(Rd, cp15_read(crn, crm, op1, op2)); cp15_read ignores
 * crm/op1/op2 and returns the Main ID constant for crn 0 and c->cp15[crn] for
 * every other crn, so the read is a load plus a guest-register store.  MCR and
 * every other coprocessor encoding keep the helper. */
static int a64_mrc_p15(a64_emit_t *e, const arm_jit_op_t *op, uint32_t done) {
    const uint32_t insn = op->insn;
    unsigned cp = (insn >> 8) & 0xfu, l = (insn >> 20) & 1u,
             crn = (insn >> 16) & 0xfu, rd = (insn >> 12) & 0xfu;
    if ((insn & 0x0f000010u) != 0x0e000010u || cp != 15u || !l || rd == 15u) return 0;
    if (crn == 0u) a64_store_imm(e, offsetof(arm920t_t, r) + rd * 4u, 0x41129200u);
    else {
        a64_load(e, 1, offsetof(arm920t_t, cp15) + crn * 4u);
        a64_put_guest(e, rd, 1);
    }
    GP32_UNUSED(done);
    return 1;
}

/* ARM_JIT_OP_PSR subset.  op_psr covers MRS/MSR for CPSR and SPSR; only a read
 * of CPSR and a flag-field-only (field mask 0x8) write to CPSR are inlined.
 * The write mask is 0xff000000, which cannot change the mode, so
 * set_cpsr_full reduces to a plain assignment; SPSR, non-f fields and any
 * other PSR encoding keep the helper. */
static int a64_psr_mov(a64_emit_t *e, const arm_jit_op_t *op, uint32_t done) {
    const uint32_t insn = op->insn;
    unsigned rd = (insn >> 12) & 0xfu;
    if ((insn & 0x0fbf0fffu) == 0x010f0000u) {             /* MRS Rd, CPSR */
        if ((insn & (1u << 22)) || rd == 15u) return 0;    /* SPSR keeps the helper */
        a64_load(e, 1, offsetof(arm920t_t, cpsr));
        a64_put_guest(e, rd, 1);
        GP32_UNUSED(done);
        return 1;
    }
    if ((insn & 0x0db0f000u) == 0x0120f000u) {             /* MSR fields, ... */
        unsigned field = (insn >> 16) & 0xfu;
        if ((insn & (1u << 22)) || field != 0x8u) return 0; /* SPSR / non-f */
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
