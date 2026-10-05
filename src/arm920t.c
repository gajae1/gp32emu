/*
 * Small ARMv4T/ARM920T interpreter for gp32emu.
 * It is intentionally self-contained C11. The register model, CP15 IDs and
 * instruction grouping follow MAME's BSD-licensed ARM7/ARM9 core, but this is
 * a compact rewrite for a headless standalone GP32 target.
 */
#include "arm920t.h"

#if (defined(__aarch64__) || defined(_M_ARM64)) && !defined(__AARCH64EB__)
#define ARM_JIT_NATIVE_A64 1
#endif

/*
 * Optional low-overhead workload counters.  GP32EMU_CPU_PROFILE is a PUBLIC
 * compile definition on the gp32emu target (CMake option GP32EMU_CPU_PROFILE),
 * so the library and benchmark agree on the ABI.  When it is absent every
 * ARM_PROF_INC hook compiles to nothing and the per-instruction dispatch
 * path keeps its uninstrumented cost.
 */
#if defined(GP32EMU_CPU_PROFILE) && GP32EMU_CPU_PROFILE
#define ARM920T_PROFILING 1
#else
#define ARM920T_PROFILING 0
#endif
#if defined(ARM_JIT_NATIVE_A64)
#define ARM920T_NATIVE_BACKEND 2u
#elif defined(__x86_64__) || defined(_M_X64)
#define ARM920T_NATIVE_BACKEND 1u
#else
#define ARM920T_NATIVE_BACKEND 0u
#endif
#if ARM920T_NATIVE_BACKEND == 2u
#define ARM_JIT_NATIVE_MAX_BYTES 65536u
#else
#define ARM_JIT_NATIVE_MAX_BYTES 16384u
#endif
#if ARM920T_PROFILING
#define ARM_PROF_INC(c, field) do { (c)->prof.field++; } while (0)
#else
#define ARM_PROF_INC(c, field) ((void)0)
#endif

/* Cause tags for arm920t_jit_invalidate_all, folded into the profile. */
enum arm_jit_inv_cause {
    ARM_JIT_INV_RESET,
    ARM_JIT_INV_JIT_DISABLE,
    ARM_JIT_INV_API_FLUSH,
    ARM_JIT_INV_STATE_LOAD,
    ARM_JIT_INV_CP15_MMU,
    ARM_JIT_INV_CP15_CACHE,
    ARM_JIT_INV_CODE_RECYCLE
};

#if defined(__x86_64__) || defined(_M_X64) || defined(ARM_JIT_NATIVE_A64)
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif
#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif
#endif

#define N_FLAG 0x80000000u
#define Z_FLAG 0x40000000u
#define C_FLAG 0x20000000u
#define V_FLAG 0x10000000u
#define I_FLAG 0x00000080u
#define F_FLAG 0x00000040u
#define T_FLAG 0x00000020u
#define MODE_MASK 0x1fu
#define MODE_USR 0x10u
#define MODE_FIQ 0x11u
#define MODE_IRQ 0x12u
#define MODE_SVC 0x13u
#define MODE_ABT 0x17u
#define MODE_UND 0x1bu
#define MODE_SYS 0x1fu
#define HALT_END_RUN 2 /* Transient yield; retain the persistent halt bit. */

/*
 * Firebird-style dynamic ARM block translator.
 *
 * Firebird's native translator is tightly coupled to its global ARM926 state,
 * address cache and GPLv3 source tree.  The GP32 core keeps a private C11
 * ARM920T state and a callback bus, so this ports the useful policy instead:
 * cache ARM-state basic blocks, stop blocks at 1 KiB page boundaries, stop on
 * PC/CPSR/CP15/control-flow edges, and never translate ARM926/ARMv5TE-only
 * operations.  Every ARM block is first decoded into portable C11 bytecode;
 * the no-JIT interpreter executes that bytecode on any host architecture.
 * On x86_64 and little-endian AArch64, blocks may receive native host code;
 * unsupported or sensitive instructions fall through to the exact interpreter
 * for one instruction.
 */
#ifndef ARM_JIT_BLOCK_COUNT
#define ARM_JIT_BLOCK_COUNT 16384u
#endif
#define ARM_JIT_BLOCK_MASK  (ARM_JIT_BLOCK_COUNT - 1u)
/* Keep colliding hot PCs resident without growing the block/operation tables. */
#define ARM_JIT_BLOCK_WAYS  2u
#if ARM_JIT_BLOCK_COUNT < 2 || (ARM_JIT_BLOCK_COUNT & ARM_JIT_BLOCK_MASK)
#error ARM_JIT_BLOCK_COUNT must be a power of two and at least two
#endif
#ifndef ARM_JIT_MAX_INSNS
#define ARM_JIT_MAX_INSNS   96u
#endif
#define ARM_JIT_PAGE_MASK   0x3ffu
#define ARM_JIT_RAM_BASE_ADDR 0x0c000000u
#define ARM_JIT_RAM_SIZE_BYTES 0x00800000u
#define ARM_JIT_BIOS_SIZE_BYTES 0x00080000u

#if defined(_MSC_VER)
#define ARM_FORCE_INLINE static __forceinline
#define ARM_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define ARM_FORCE_INLINE static inline __attribute__((always_inline))
#define ARM_NOINLINE __attribute__((noinline))
#else
#define ARM_FORCE_INLINE static inline
#define ARM_NOINLINE
#endif

/* Preserve locality within a code page while distinguishing equal offsets
 * in different 64-KiB regions, then scale to the first slot of the PC's
 * ARM_JIT_BLOCK_WAYS-way set. The full PC/generation tags still establish
 * validity; this only selects where the set lives in the table. */
ARM_FORCE_INLINE uint32_t arm_jit_block_index(uint32_t pc) {
    return (((pc >> 2) ^ (pc >> 16)) & (ARM_JIT_BLOCK_MASK >> 1)) * ARM_JIT_BLOCK_WAYS;
}

ARM_FORCE_INLINE unsigned arm_ctz16(uint32_t v) {
#if defined(__GNUC__) || defined(__clang__)
    return (unsigned)__builtin_ctz(v);
#else
    unsigned n = 0;
    while (((v >> n) & 1u) == 0u && n < 16u) ++n;
    return n;
#endif
}

typedef enum arm_jit_kind {
    ARM_JIT_OP_INTERP = 0,
    ARM_JIT_OP_DATA,
    ARM_JIT_OP_PSR,
    ARM_JIT_OP_MUL,
    ARM_JIT_OP_SWP,
    ARM_JIT_OP_HALF,
    ARM_JIT_OP_SINGLE_DT,
    ARM_JIT_OP_BLOCK_DT,
    ARM_JIT_OP_BRANCH,
    ARM_JIT_OP_SWI,
    ARM_JIT_OP_COPROC,
    ARM_JIT_OP_UNDEFINED
} arm_jit_kind_t;

typedef struct arm_jit_op {
    uint32_t pc;
    uint32_t insn;
    uint32_t imm;
    uint8_t cond;
    uint8_t kind;
    uint8_t stop;
    uint8_t reserved;
    uint8_t a;
    uint8_t b;
    uint8_t c;
    uint8_t d;
    uint8_t e;
    uint8_t f;
    uint8_t g;
    uint8_t h;
} arm_jit_op_t;

typedef uint32_t (*arm_jit_native_fn)(arm920t_t *cpu, uint32_t cycles);

typedef struct arm_jit_block {
    uint32_t tag_pc;
    uint32_t tag_cache_epoch;
    uint32_t valid;
    uint32_t generation;
    uint8_t count;
    uint8_t native_ok; /* 0: unattempted; 1: compiled; 2: failed lazy attempt. */
    uint8_t poll_prefix;
    uint8_t poll_backedge; /* bit 0: backedge; bits 1..4: counted register + 1. */
    uint32_t deferred_inline_pc;
    arm_jit_native_fn native;
} arm_jit_block_t;

/* Packed mirror of tlb_va_base/tlb_pa_base/tlb_mask/tlb_valid: one 16-byte
 * entry per TLB index so a hit stays inside a single cache line instead of
 * touching four separately strided arrays.  It is a derived cache only; the
 * SoA arrays above stay authoritative, including for state images. */
typedef struct arm_tlb_entry {
    uint32_t va_base;
    uint32_t pa_base;
    uint32_t mask;
    uint32_t valid;
} arm_tlb_entry_t;

#if defined(ARM_JIT_NATIVE_A64)
/* Derived RAM-only page lookup for native accesses. A tag outside the 20-bit
 * VA-page range rejects invalid, tiny-page and non-RAM entries with one compare.
 * xor_offset maps a matching VA directly to an offset in the direct RAM window.
 * The authoritative TLB retains its original mask and saved-state layout. */
typedef struct arm_jit_tlb_page {
    uint32_t va_page;
    uint32_t xor_offset;
} arm_jit_tlb_page_t;
#endif

struct arm920t {
    uint32_t r[16];
    uint32_t cpsr;
    uint32_t bank_usr[7];  /* r8-r14 */
    uint32_t bank_fiq[7];
    uint32_t bank_svc[2];
    uint32_t bank_abt[2];
    uint32_t bank_irq[2];
    uint32_t bank_und[2];
    uint32_t spsr_fiq, spsr_svc, spsr_abt, spsr_irq, spsr_und;
    uint32_t cp15[16];
    uint32_t tlb_va_base[4096];
    uint32_t tlb_pa_base[4096];
    uint32_t tlb_mask[4096];
    uint8_t tlb_valid[4096];
    /* Entries are 16 bytes wide and the member starts on a 16-byte boundary of
     * the calloc'd struct, so a lookup stays inside one cache line.  Keep any
     * new small fields after this member to preserve that alignment. */
    arm_tlb_entry_t tlb_entry[4096];
#if defined(ARM_JIT_NATIVE_A64)
    arm_jit_tlb_page_t jit_tlb_page[4096];
#endif
    arm_bus_t bus;
    uint64_t cycles_total;
    int irq_line, fiq_line;
    int halted;
    int running; /* Runtime only; not part of the v0002 CPU wire image. */
    uint32_t run_limit; /* Absolute cycle budget within the current run only. */
    int trace;
    arm_log_fn log;
    void *log_user;
    arm_swi_fn swi;
    void *swi_user;
    arm_jit_block_t *jit_blocks;
    arm_jit_op_t (*jit_ops)[ARM_JIT_MAX_INSNS];
    uint8_t *jit_code;
    uint8_t *jit_ram_base;
    uint8_t *jit_bios_base;
    size_t jit_code_size;
    size_t jit_code_used;
    uint32_t jit_generation;
    uint32_t jit_cache_epoch;
    int jit_enabled;
    uint64_t jit_hits;
    uint64_t jit_misses;
    uint64_t jit_fallbacks;
    /* Transient workload counters (GP32EMU_CPU_PROFILE); never serialized. */
    gp32_cpu_profile_t prof;
    /* Runtime certificates; appended to retain all existing hot-field offsets. */
    const arm_live_read32_t *live_read32;
    size_t live_read32_count;
};

/* Native dispatch only needs the compact header. Decoded instructions live
 * in a separate stable allocation so C helpers can retain their addresses. */
ARM_FORCE_INLINE arm_jit_op_t *arm_jit_ops(const arm920t_t *c, const arm_jit_block_t *b) {
    return c->jit_ops[b - c->jit_blocks];
}

static uint32_t mode(const arm920t_t *c) { return c->cpsr & MODE_MASK; }
static int thumb(const arm920t_t *c) { return (c->cpsr & T_FLAG) != 0; }
static void arm920t_jit_invalidate_all(arm920t_t *c, unsigned cause);

static uint32_t *spsr_ptr(arm920t_t *c, uint32_t m) {
    switch (m) {
    case MODE_FIQ: return &c->spsr_fiq;
    case MODE_SVC: return &c->spsr_svc;
    case MODE_ABT: return &c->spsr_abt;
    case MODE_IRQ: return &c->spsr_irq;
    case MODE_UND: return &c->spsr_und;
    default: return NULL;
    }
}

static void save_banked(arm920t_t *c, uint32_t m) {
    if (m == MODE_FIQ) {
        for (int i = 8; i <= 14; i++) c->bank_fiq[i - 8] = c->r[i];
    } else {
        for (int i = 8; i <= 12; i++) c->bank_usr[i - 8] = c->r[i];
        switch (m) {
        case MODE_SVC: c->bank_svc[0] = c->r[13]; c->bank_svc[1] = c->r[14]; break;
        case MODE_ABT: c->bank_abt[0] = c->r[13]; c->bank_abt[1] = c->r[14]; break;
        case MODE_IRQ: c->bank_irq[0] = c->r[13]; c->bank_irq[1] = c->r[14]; break;
        case MODE_UND: c->bank_und[0] = c->r[13]; c->bank_und[1] = c->r[14]; break;
        default: c->bank_usr[5] = c->r[13]; c->bank_usr[6] = c->r[14]; break;
        }
    }
}

static void load_banked(arm920t_t *c, uint32_t m) {
    if (m == MODE_FIQ) {
        for (int i = 8; i <= 14; i++) c->r[i] = c->bank_fiq[i - 8];
    } else {
        for (int i = 8; i <= 12; i++) c->r[i] = c->bank_usr[i - 8];
        switch (m) {
        case MODE_SVC: c->r[13] = c->bank_svc[0]; c->r[14] = c->bank_svc[1]; break;
        case MODE_ABT: c->r[13] = c->bank_abt[0]; c->r[14] = c->bank_abt[1]; break;
        case MODE_IRQ: c->r[13] = c->bank_irq[0]; c->r[14] = c->bank_irq[1]; break;
        case MODE_UND: c->r[13] = c->bank_und[0]; c->r[14] = c->bank_und[1]; break;
        default: c->r[13] = c->bank_usr[5]; c->r[14] = c->bank_usr[6]; break;
        }
    }
}

static void switch_mode(arm920t_t *c, uint32_t new_mode) {
    uint32_t old = mode(c);
    if (old == new_mode) return;
    save_banked(c, old);
    c->cpsr = (c->cpsr & ~MODE_MASK) | new_mode;
    load_banked(c, new_mode);
}

static void set_cpsr_full(arm920t_t *c, uint32_t value) {
    uint32_t old_mode = mode(c);
    uint32_t new_mode = value & MODE_MASK;
    if (new_mode != old_mode) {
        save_banked(c, old_mode);
        c->cpsr = value;
        load_banked(c, new_mode);
    } else {
        c->cpsr = value;
    }
}

ARM_FORCE_INLINE uint8_t *fastmem(arm920t_t *c, uint32_t a, size_t bytes, int write) {
    return c->bus.fastmem ? c->bus.fastmem(c->bus.user, a, bytes, write) : NULL;
}
ARM_FORCE_INLINE uint32_t raw32(arm920t_t *c, uint32_t a) {
    uint8_t *p = fastmem(c, a, 4u, 0);
    return p ? gp32_ld32le(p) : c->bus.read32(c->bus.user, a);
}

#if defined(ARM_JIT_NATIVE_A64)
static void tlb_rebuild_jit_page(arm920t_t *c, unsigned idx) {
    const arm_tlb_entry_t *e = &c->tlb_entry[idx];
    arm_jit_tlb_page_t *p = &c->jit_tlb_page[idx];
    p->va_page = UINT32_MAX;
    p->xor_offset = 0u;
    /* Only the three full-page descriptor shapes are provable here. In
     * particular, do not widen a 1 KiB mapping to its containing 4 KiB page.
     * Alignment/index checks also keep unusual loaded TLB payloads on the
     * original helper path rather than changing their OR-based semantics. */
    uint32_t mask = e->mask;
    if (!e->valid || (mask != 0xfffu && mask != 0xffffu && mask != 0xfffffu) ||
        (e->va_base & mask) || (e->pa_base & mask)) return;
    uint32_t page_bits = ((uint32_t)idx << 12) & mask;
    uint32_t va = e->va_base | page_bits, pa = e->pa_base | page_bits;
    if (((va >> 12) & 0xfffu) != idx || pa < ARM_JIT_RAM_BASE_ADDR ||
        pa - ARM_JIT_RAM_BASE_ADDR > ARM_JIT_RAM_SIZE_BYTES - 0x1000u) return;
    p->va_page = va >> 12;
    p->xor_offset = va ^ (pa - ARM_JIT_RAM_BASE_ADDR);
}
#endif

/* Every path that clears tlb_valid must clear the mirror together with it, and
 * tlb_store must keep the matching entry in sync; mmu_translate then never
 * needs to fall back to the SoA arrays. */
static void tlb_flush_all(arm920t_t *c) {
    memset(c->tlb_valid, 0, sizeof(c->tlb_valid));
    memset(c->tlb_entry, 0, sizeof(c->tlb_entry));
#if defined(ARM_JIT_NATIVE_A64)
    memset(c->jit_tlb_page, 0xff, sizeof(c->jit_tlb_page));
#endif
}

static void tlb_rebuild_mirror(arm920t_t *c) {
    for (unsigned i = 0; i < 4096u; i++) {
        arm_tlb_entry_t e;
        /* Clear the payload too when the entry is dead: the SoA arrays keep
         * stale values for invalid indices, so copying them verbatim would
         * make the mirror depend on history rather than on live TLB state. */
        if (c->tlb_valid[i]) {
            e.va_base = c->tlb_va_base[i];
            e.pa_base = c->tlb_pa_base[i];
            e.mask = c->tlb_mask[i];
            e.valid = 1u;
        } else {
            e.va_base = 0u;
            e.pa_base = 0u;
            e.mask = 0u;
            e.valid = 0u;
        }
        c->tlb_entry[i] = e;
#if defined(ARM_JIT_NATIVE_A64)
        tlb_rebuild_jit_page(c, i);
#endif
    }
}

static void tlb_store(arm920t_t *c, uint32_t va, uint32_t pa_base, uint32_t mask) {
    unsigned idx = (va >> 12) & 0xfffu;
    c->tlb_valid[idx] = 1;
    c->tlb_va_base[idx] = va & ~mask;
    c->tlb_pa_base[idx] = pa_base;
    c->tlb_mask[idx] = mask;
    c->tlb_entry[idx].va_base = c->tlb_va_base[idx];
    c->tlb_entry[idx].pa_base = pa_base;
    c->tlb_entry[idx].mask = mask;
    c->tlb_entry[idx].valid = 1;
#if defined(ARM_JIT_NATIVE_A64)
    tlb_rebuild_jit_page(c, idx);
#endif
}

ARM_FORCE_INLINE uint32_t mmu_translate(arm920t_t *c, uint32_t va) {
    if (!(c->cp15[1] & 1u)) return va;
    unsigned idx = (va >> 12) & 0xfffu;
    const arm_tlb_entry_t *e = &c->tlb_entry[idx];
    if (e->valid) {
        uint32_t mask = e->mask;
        if ((va & ~mask) == e->va_base) return e->pa_base | (va & mask);
    }
    uint32_t ttb = c->cp15[2] & 0xffffc000u;
    uint32_t l1a = ttb | ((va >> 18) & 0x3ffcu);
    uint32_t d1 = raw32(c, l1a);
    switch (d1 & 3u) {
    case 2: { /* 1 MiB section */
        uint32_t mask = 0x000fffffu;
        uint32_t pa = d1 & 0xfff00000u;
        tlb_store(c, va, pa, mask);
        return pa | (va & mask);
    }
    case 1: { /* coarse second-level page table */
        uint32_t l2a = (d1 & 0xfffffc00u) | ((va >> 10) & 0x3fcu);
        uint32_t d2 = raw32(c, l2a);
        switch (d2 & 3u) {
        case 1: { uint32_t mask = 0x0000ffffu; uint32_t pa = d2 & 0xffff0000u; tlb_store(c, va, pa, mask); return pa | (va & mask); } /* large page */
        case 2: { uint32_t mask = 0x00000fffu; uint32_t pa = d2 & 0xfffff000u; tlb_store(c, va, pa, mask); return pa | (va & mask); } /* small page */
        case 3: { uint32_t mask = 0x000003ffu; uint32_t pa = d2 & 0xfffffc00u; tlb_store(c, va, pa, mask); return pa | (va & mask); } /* tiny page */
        default: break;
        }
        break;
    }
    default:
        break;
    }
    /* Keep early firmware tolerant: unmapped regions behave as identity instead of taking a data abort. */
    c->cp15[5] = 0x00000005u;
    c->cp15[6] = va;
    return va;
}

ARM_FORCE_INLINE uint8_t rb8(arm920t_t *c, uint32_t a) { uint32_t paddr = mmu_translate(c, a); uint8_t *p = fastmem(c, paddr, 1u, 0); return p ? p[0] : c->bus.read8(c->bus.user, paddr); }
ARM_FORCE_INLINE uint16_t rb16(arm920t_t *c, uint32_t a) { uint32_t paddr = mmu_translate(c, a); uint8_t *p = fastmem(c, paddr, 2u, 0); return p ? gp32_ld16le(p) : c->bus.read16(c->bus.user, paddr); }
ARM_FORCE_INLINE uint32_t rb32(arm920t_t *c, uint32_t a) { uint32_t paddr = mmu_translate(c, a); uint8_t *p = fastmem(c, paddr, 4u, 0); return p ? gp32_ld32le(p) : c->bus.read32(c->bus.user, paddr); }
ARM_FORCE_INLINE void wb8(arm920t_t *c, uint32_t a, uint8_t v) { uint32_t paddr = mmu_translate(c, a); uint8_t *p = fastmem(c, paddr, 1u, 1); if (p) p[0] = v; else c->bus.write8(c->bus.user, paddr, v); }
ARM_FORCE_INLINE void wb16(arm920t_t *c, uint32_t a, uint16_t v) { uint32_t paddr = mmu_translate(c, a); uint8_t *p = fastmem(c, paddr, 2u, 1); if (p) gp32_st16le(p, v); else c->bus.write16(c->bus.user, paddr, v); }
ARM_FORCE_INLINE void wb32(arm920t_t *c, uint32_t a, uint32_t v) { uint32_t paddr = mmu_translate(c, a); uint8_t *p = fastmem(c, paddr, 4u, 1); if (p) gp32_st32le(p, v); else c->bus.write32(c->bus.user, paddr, v); }

ARM_FORCE_INLINE void set_nz(arm920t_t *c, uint32_t v) {
    c->cpsr = (c->cpsr & ~(N_FLAG | Z_FLAG)) | (v & N_FLAG) | (v == 0 ? Z_FLAG : 0);
}
ARM_FORCE_INLINE void set_flag(arm920t_t *c, uint32_t f, int on) { if (on) c->cpsr |= f; else c->cpsr &= ~f; }
ARM_FORCE_INLINE uint32_t read_r(const arm920t_t *c, unsigned n) {
    if (n == 15) return c->r[15] + (thumb(c) ? 2u : 4u);
    return c->r[n];
}
ARM_FORCE_INLINE void write_r(arm920t_t *c, unsigned n, uint32_t v) {
    if (n == 15) {
        c->r[15] = v & (thumb(c) ? ~1u : ~3u);
    } else c->r[n] = v;
}
static void write_pc_x(arm920t_t *c, uint32_t v) {
    if (v & 1u) { c->cpsr |= T_FLAG; c->r[15] = v & ~1u; }
    else { c->cpsr &= ~T_FLAG; c->r[15] = v & ~3u; }
}

static uint32_t read_user_r(const arm920t_t *c, unsigned n) {
    uint32_t m = mode(c);
    if (n >= 16) return 0;
    if (m == MODE_USR || m == MODE_SYS) return n == 15 ? c->r[15] + (thumb(c) ? 2u : 4u) : c->r[n];
    if (n <= 7 || n == 15) return read_r(c, n);
    if (n <= 12) return (m == MODE_FIQ) ? c->bank_usr[n - 8] : c->r[n];
    return c->bank_usr[n - 8];
}
static void write_user_r(arm920t_t *c, unsigned n, uint32_t v) {
    uint32_t m = mode(c);
    if (n >= 16) return;
    if (m == MODE_USR || m == MODE_SYS) { write_r(c, n, v); return; }
    if (n <= 7 || n == 15) { write_r(c, n, v); return; }
    if (n <= 12) { if (m == MODE_FIQ) c->bank_usr[n - 8] = v; else c->r[n] = v; return; }
    c->bank_usr[n - 8] = v;
}

static const uint16_t arm_cond_lut[16] = {
    0xf0f0u, 0x0f0fu, 0xccccu, 0x3333u, 0xff00u, 0x00ffu, 0xaaaau, 0x5555u,
    0x0c0cu, 0xf3f3u, 0xaa55u, 0x55aau, 0x0a05u, 0xf5fau, 0xffffu, 0x0000u
};
ARM_FORCE_INLINE int cond_pass_cpsr(uint32_t cpsr, unsigned cond) {
    unsigned flags = (cpsr >> 28) & 0x0fu;
    return (arm_cond_lut[cond & 15u] >> flags) & 1u;
}
ARM_FORCE_INLINE int cond_pass(const arm920t_t *c, unsigned cond) {
    return cond_pass_cpsr(c->cpsr, cond);
}

static void exception_enter(arm920t_t *c, uint32_t m, uint32_t vector, uint32_t lr, uint32_t extra_flags) {
    uint32_t old = c->cpsr;
    switch_mode(c, m);
    uint32_t *spsr = spsr_ptr(c, m);
    if (spsr) *spsr = old;
    c->r[14] = lr;
    c->cpsr = (c->cpsr & ~(T_FLAG | MODE_MASK)) | m | I_FLAG | extra_flags;
    c->r[15] = vector | ((c->cp15[1] & (1u << 13)) ? 0xffff0000u : 0u);
}
static void maybe_irq(arm920t_t *c) {
    if (c->fiq_line && !(c->cpsr & F_FLAG)) exception_enter(c, MODE_FIQ, 0x1c, c->r[15] + 4, F_FLAG);
    else if (c->irq_line && !(c->cpsr & I_FLAG)) exception_enter(c, MODE_IRQ, 0x18, c->r[15] + 4, 0);
}

arm920t_t *arm920t_create(const arm_bus_t *bus) {
    if (!bus || !bus->read8 || !bus->read16 || !bus->read32 || !bus->write8 || !bus->write16 || !bus->write32) return NULL;
    arm920t_t *c = (arm920t_t *)calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->bus = *bus;
    c->jit_enabled = 0;
    arm920t_reset(c, 0);
    return c;
}
#if defined(__x86_64__) || defined(_M_X64) || defined(ARM_JIT_NATIVE_A64)
static uint8_t *arm_jit_alloc_exec(size_t bytes) {
    if (!bytes) return NULL;
#if defined(_WIN32)
    return (uint8_t *)VirtualAlloc(NULL, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
#else
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return (p == MAP_FAILED) ? NULL : (uint8_t *)p;
#endif
}

static void arm_jit_free_exec(uint8_t *ptr, size_t bytes) {
    if (!ptr) return;
#if defined(_WIN32)
    GP32_UNUSED(bytes);
    VirtualFree(ptr, 0, MEM_RELEASE);
#else
    munmap(ptr, bytes);
#endif
}

static void arm_jit_flush_exec(void *ptr, size_t bytes) {
    if (!ptr || !bytes) return;
#if defined(_WIN32)
    FlushInstructionCache(GetCurrentProcess(), ptr, bytes);
#elif defined(ARM_JIT_NATIVE_A64)
    /* Hand-rolled A64 maintenance. The linked __clear_cache (Zig 0.13
     * compiler-rt and comparable bare implementations) strides by the running
     * core's own CTR_EL0 line size and omits the completion DSB after IC IVAU.
     * On big.LITTLE parts different cores report different line sizes, so a
     * range written by one core can leave lines the invalidating core never
     * touches; either hazard yields stale instruction fetches. Stride by the
     * smaller of this core's line sizes, capped at 32 bytes so a migration to
     * a core with smaller lines cannot skip any, and complete each step with
     * the barriers the architecture requires. */
    uint64_t a64_ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(a64_ctr));
    size_t a64_istride = (size_t)4u << (unsigned)(a64_ctr & 0xfu);
    size_t a64_dstride = (size_t)4u << (unsigned)((a64_ctr >> 16) & 0xfu);
    size_t a64_stride = a64_istride < a64_dstride ? a64_istride : a64_dstride;
    if (a64_stride > 32u) a64_stride = 32u;
    char *a64_end = (char *)ptr + bytes;
    if (!(a64_ctr & (1ull << 28))) { /* CTR_EL0.IDC: no D-cache cleaning needed */
        for (uintptr_t a64_cur = (uintptr_t)ptr & ~(uintptr_t)(a64_stride - 1u);
             a64_cur < (uintptr_t)a64_end; a64_cur += a64_stride)
            __asm__ volatile("dc cvau, %0" ::"r"(a64_cur) : "memory");
    }
    /* Cleaning completes before invalidation; invalidation completes before
     * fetch synchronization. The trailing DSB also covers a new block
     * overwriting code from an earlier cache generation. */
    __asm__ volatile("dsb ish" ::: "memory");
    if (!(a64_ctr & (1ull << 29))) { /* CTR_EL0.DIC: no I-cache invalidation needed */
        for (uintptr_t a64_cur = (uintptr_t)ptr & ~(uintptr_t)(a64_stride - 1u);
             a64_cur < (uintptr_t)a64_end; a64_cur += a64_stride)
            __asm__ volatile("ic ivau, %0" ::"r"(a64_cur) : "memory");
    }
    __asm__ volatile("dsb ish\n\tisb" ::: "memory");
#elif defined(__GNUC__) || defined(__clang__)
    __builtin___clear_cache((char *)ptr, (char *)ptr + bytes);
#else
    GP32_UNUSED(ptr);
    GP32_UNUSED(bytes);
#endif
}
#endif

void arm920t_destroy(arm920t_t *c) {
    if (!c) return;
#if defined(__x86_64__) || defined(_M_X64) || defined(ARM_JIT_NATIVE_A64)
    arm_jit_free_exec(c->jit_code, c->jit_code_size);
#endif
    free(c->jit_blocks);
    free(c->jit_ops);
    free(c);
}
void arm920t_set_trace(arm920t_t *c, int en, arm_log_fn log, void *user) { if (c) { c->trace = en; c->log = log; c->log_user = user; } }
void arm920t_set_swi_handler(arm920t_t *c, arm_swi_fn fn, void *user) { if (c) { c->swi = fn; c->swi_user = user; } }
void arm920t_set_reg(arm920t_t *c, unsigned reg, uint32_t value) {
    if (c && reg < 16u) {
        /* PC alignment follows the current instruction set, including when
         * entering or restoring a halfword-aligned Thumb HLE callback. */
        c->r[reg] = reg == 15u ? (value & ~(thumb(c) ? 1u : 3u)) : value;
    }
}
void arm920t_set_cpsr(arm920t_t *c, uint32_t value) { if (c) set_cpsr_full(c, value); }
void arm920t_reset(arm920t_t *c, uint32_t vector) {
    if (!c) return;
    memset(c->r, 0, sizeof(c->r));
    memset(c->bank_usr, 0, sizeof(c->bank_usr));
    memset(c->bank_fiq, 0, sizeof(c->bank_fiq));
    memset(c->bank_svc, 0, sizeof(c->bank_svc));
    memset(c->bank_abt, 0, sizeof(c->bank_abt));
    memset(c->bank_irq, 0, sizeof(c->bank_irq));
    memset(c->bank_und, 0, sizeof(c->bank_und));
    memset(c->cp15, 0, sizeof(c->cp15));
    tlb_flush_all(c);
    c->cpsr = MODE_SVC | I_FLAG | F_FLAG;
    c->cp15[0] = 0x41129200u; /* ARM920T-ish */
    c->cp15[1] = 0x00000070u; /* control */
    c->cp15[2] = 0;
    c->cp15[3] = 0;
    c->r[15] = vector & ~3u;
    c->irq_line = c->fiq_line = c->halted = 0;
    arm920t_jit_invalidate_all(c, ARM_JIT_INV_RESET);
}
void arm920t_set_irq(arm920t_t *c, int state) { if (c) c->irq_line = state != 0; }
void arm920t_set_fiq(arm920t_t *c, int state) { if (c) c->fiq_line = state != 0; }
void arm920t_set_jit(arm920t_t *c, int enabled) {
    if (c) {
        c->jit_enabled = enabled != 0;
        if (c->jit_enabled) {
            c->jit_ram_base = fastmem(c, 0x0c000000u, 1u, 0);
            c->jit_bios_base = fastmem(c, 0x00000000u, 1u, 0);
            if (!c->jit_ram_base || !c->jit_bios_base) c->jit_enabled = 0;
        } else arm920t_jit_invalidate_all(c, ARM_JIT_INV_JIT_DISABLE);
    }
}
void arm920t_flush_jit(arm920t_t *c) { arm920t_jit_invalidate_all(c, ARM_JIT_INV_API_FLUSH); }
int arm920t_set_live_read32(arm920t_t *c, const arm_live_read32_t *reads, size_t count) {
    if (!c || c->running || (!reads && count)) return 0;
    for (size_t i = 0; i < count; ++i)
        if ((reads[i].pa & 3u) || !reads[i].word || ((uintptr_t)reads[i].word & 3u)) return 0;
    arm920t_jit_invalidate_all(c, ARM_JIT_INV_API_FLUSH);
    c->live_read32 = reads;
    c->live_read32_count = count;
    return 1;
}
uint32_t arm920t_get_pc(const arm920t_t *c) { return c ? c->r[15] : 0; }
uint64_t arm920t_get_cycles(const arm920t_t *c) { return c ? c->cycles_total : 0; }
void arm920t_add_idle_cycles(arm920t_t *c, uint32_t cycles) { if (c) c->cycles_total += cycles; }
void arm920t_stop_run(arm920t_t *c) { if (c) c->halted |= HALT_END_RUN; }
void arm920t_limit_run(arm920t_t *c, uint32_t max_cycles_from_run_start) {
    if (c && c->running && max_cycles_from_run_start < c->run_limit)
        c->run_limit = max_cycles_from_run_start;
}
int arm920t_is_running(const arm920t_t *c) { return c && c->running; }
uint32_t arm920t_get_reg(const arm920t_t *c, unsigned r) { return c && r < 16 ? c->r[r] : 0; }
uint32_t arm920t_get_cpsr(const arm920t_t *c) { return c ? c->cpsr : 0; }
uint32_t arm920t_get_cp15(const arm920t_t *c, unsigned reg) { return c && reg < 16 ? c->cp15[reg] : 0; }
uint64_t arm920t_get_jit_hits(const arm920t_t *c) { return c ? c->jit_hits : 0; }
uint64_t arm920t_get_jit_misses(const arm920t_t *c) { return c ? c->jit_misses : 0; }
uint64_t arm920t_get_jit_fallbacks(const arm920t_t *c) { return c ? c->jit_fallbacks : 0; }

void arm920t_get_register_context(const arm920t_t *c, arm920t_register_context_t *out) {
    if (!c || !out) return;
    memcpy(out->r, c->r, sizeof(out->r));
    out->cpsr = c->cpsr;
    memcpy(out->bank_usr, c->bank_usr, sizeof(out->bank_usr));
    memcpy(out->bank_fiq, c->bank_fiq, sizeof(out->bank_fiq));
    memcpy(out->bank_svc, c->bank_svc, sizeof(out->bank_svc));
    memcpy(out->bank_abt, c->bank_abt, sizeof(out->bank_abt));
    memcpy(out->bank_irq, c->bank_irq, sizeof(out->bank_irq));
    memcpy(out->bank_und, c->bank_und, sizeof(out->bank_und));
    out->spsr_fiq = c->spsr_fiq; out->spsr_svc = c->spsr_svc;
    out->spsr_abt = c->spsr_abt; out->spsr_irq = c->spsr_irq; out->spsr_und = c->spsr_und;
}

void arm920t_set_register_context(arm920t_t *c, const arm920t_register_context_t *saved) {
    if (!c || !saved) return;
    /* Restore the complete bank representation directly: switching modes
     * here would first save the callback's registers over a restored bank. */
    memcpy(c->r, saved->r, sizeof(c->r));
    c->cpsr = saved->cpsr;
    memcpy(c->bank_usr, saved->bank_usr, sizeof(c->bank_usr));
    memcpy(c->bank_fiq, saved->bank_fiq, sizeof(c->bank_fiq));
    memcpy(c->bank_svc, saved->bank_svc, sizeof(c->bank_svc));
    memcpy(c->bank_abt, saved->bank_abt, sizeof(c->bank_abt));
    memcpy(c->bank_irq, saved->bank_irq, sizeof(c->bank_irq));
    memcpy(c->bank_und, saved->bank_und, sizeof(c->bank_und));
    c->spsr_fiq = saved->spsr_fiq; c->spsr_svc = saved->spsr_svc;
    c->spsr_abt = saved->spsr_abt; c->spsr_irq = saved->spsr_irq; c->spsr_und = saved->spsr_und;
}

static uint32_t shifter_operand(arm920t_t *c, uint32_t insn, int *carry_out) {
    if (insn & (1u << 25)) {
        uint32_t imm = insn & 0xffu;
        unsigned rot = ((insn >> 8) & 0xfu) * 2u;
        uint32_t res = gp32_ror32(imm, rot);
        if (carry_out) *carry_out = rot ? !!(res & N_FLAG) : !!(c->cpsr & C_FLAG);
        return res;
    }
    uint32_t rm = insn & 0xfu;
    uint32_t v = read_r(c, rm);
    unsigned type = (insn >> 5) & 3u;
    unsigned amount;
    if (insn & (1u << 4)) amount = read_r(c, (insn >> 8) & 0xfu) & 0xffu;
    else amount = (insn >> 7) & 0x1fu;
    int cin = !!(c->cpsr & C_FLAG);
    uint32_t res = v;
    int cout = cin;
    int reg_shift = !!(insn & (1u << 4));
    switch (type) {
    case 0: /* LSL */
        if (amount == 0) { res = v; cout = cin; }
        else if (amount < 32) { res = v << amount; cout = !!(v & (1u << (32u - amount))); }
        else if (amount == 32) { res = 0; cout = v & 1u; }
        else { res = 0; cout = 0; }
        break;
    case 1: /* LSR */
        if (amount == 0) {
            if (reg_shift) { res = v; cout = cin; }
            else { res = 0; cout = !!(v & N_FLAG); }
        } else if (amount < 32) { res = v >> amount; cout = !!(v & (1u << (amount - 1u))); }
        else if (amount == 32) { res = 0; cout = !!(v & N_FLAG); }
        else { res = 0; cout = 0; }
        break;
    case 2: /* ASR */
        if (amount == 0) {
            if (reg_shift) { res = v; cout = cin; }
            else { res = (v & N_FLAG) ? UINT32_MAX : 0; cout = !!(v & N_FLAG); }
        } else if (amount < 32) { res = (uint32_t)((int32_t)v >> amount); cout = !!(v & (1u << (amount - 1u))); }
        else { res = (v & N_FLAG) ? UINT32_MAX : 0; cout = !!(v & N_FLAG); }
        break;
    case 3: /* ROR/RRX */
        if (amount == 0) {
            if (reg_shift) { res = v; cout = cin; }
            else { res = ((uint32_t)cin << 31) | (v >> 1); cout = v & 1u; }
        } else { unsigned rot = amount & 31u; res = gp32_ror32(v, rot); cout = rot ? !!(res & N_FLAG) : !!(v & N_FLAG); }
        break;
    }
    if (carry_out) *carry_out = cout;
    return res;
}

ARM_FORCE_INLINE uint32_t nz_flags(uint32_t r) {
    return (r & N_FLAG) | (r == 0u ? Z_FLAG : 0u);
}

ARM_FORCE_INLINE void add_flags(arm920t_t *c, uint32_t a, uint32_t b, uint32_t r) {
    uint32_t flags = nz_flags(r) | (r < a ? C_FLAG : 0u) | ((~(a ^ b) & (a ^ r) & N_FLAG) ? V_FLAG : 0u);
    c->cpsr = (c->cpsr & ~(N_FLAG | Z_FLAG | C_FLAG | V_FLAG)) | flags;
}
ARM_FORCE_INLINE void sub_flags(arm920t_t *c, uint32_t a, uint32_t b, uint32_t r) {
    uint32_t flags = nz_flags(r) | (a >= b ? C_FLAG : 0u) | (((a ^ b) & (a ^ r) & N_FLAG) ? V_FLAG : 0u);
    c->cpsr = (c->cpsr & ~(N_FLAG | Z_FLAG | C_FLAG | V_FLAG)) | flags;
}

ARM_FORCE_INLINE void add_carry_flags(arm920t_t *c, uint32_t a, uint32_t b, uint32_t carry_in, uint32_t r) {
    uint64_t wide = (uint64_t)a + (uint64_t)b + (uint64_t)(carry_in & 1u);
    uint32_t flags = nz_flags(r) | ((wide >> 32) ? C_FLAG : 0u) | ((~(a ^ b) & (a ^ r) & N_FLAG) ? V_FLAG : 0u);
    c->cpsr = (c->cpsr & ~(N_FLAG | Z_FLAG | C_FLAG | V_FLAG)) | flags;
}
ARM_FORCE_INLINE void sub_borrow_flags(arm920t_t *c, uint32_t a, uint32_t b, uint32_t borrow_in, uint32_t r) {
    uint64_t sub = (uint64_t)b + (uint64_t)(borrow_in & 1u);
    uint32_t flags = nz_flags(r) | ((uint64_t)a >= sub ? C_FLAG : 0u) | (((a ^ b) & (a ^ r) & N_FLAG) ? V_FLAG : 0u);
    c->cpsr = (c->cpsr & ~(N_FLAG | Z_FLAG | C_FLAG | V_FLAG)) | flags;
}
static void restore_cpsr_from_spsr(arm920t_t *c) {
    uint32_t *s = spsr_ptr(c, mode(c));
    if (s) set_cpsr_full(c, *s);
}

static void op_data_proc(arm920t_t *c, uint32_t insn) {
    unsigned op = (insn >> 21) & 0xfu, s = GP32_BIT(insn, 20), rn = (insn >> 16) & 0xfu, rd = (insn >> 12) & 0xfu;
    int sh_c = !!(c->cpsr & C_FLAG);
    uint32_t op2 = shifter_operand(c, insn, &sh_c);
    uint32_t a = read_r(c, rn), r = 0;
    int write = 1;
    switch (op) {
    case 0x0: r = a & op2; if (s) { set_nz(c, r); set_flag(c, C_FLAG, sh_c); } break;
    case 0x1: r = a ^ op2; if (s) { set_nz(c, r); set_flag(c, C_FLAG, sh_c); } break;
    case 0x2: r = a - op2; if (s) sub_flags(c, a, op2, r); break;
    case 0x3: r = op2 - a; if (s) sub_flags(c, op2, a, r); break;
    case 0x4: r = a + op2; if (s) add_flags(c, a, op2, r); break;
    case 0x5: { uint32_t ci = (c->cpsr & C_FLAG) ? 1u : 0u; r = a + op2 + ci; if (s) add_carry_flags(c, a, op2, ci, r); break; }
    case 0x6: { uint32_t bi = (c->cpsr & C_FLAG) ? 0u : 1u; r = a - op2 - bi; if (s) sub_borrow_flags(c, a, op2, bi, r); } break;
    case 0x7: { uint32_t bi = (c->cpsr & C_FLAG) ? 0u : 1u; r = op2 - a - bi; if (s) sub_borrow_flags(c, op2, a, bi, r); } break;
    case 0x8: r = a & op2; set_nz(c, r); set_flag(c, C_FLAG, sh_c); write = 0; break;
    case 0x9: r = a ^ op2; set_nz(c, r); set_flag(c, C_FLAG, sh_c); write = 0; break;
    case 0xa: r = a - op2; sub_flags(c, a, op2, r); write = 0; break;
    case 0xb: r = a + op2; add_flags(c, a, op2, r); write = 0; break;
    case 0xc: r = a | op2; if (s) { set_nz(c, r); set_flag(c, C_FLAG, sh_c); } break;
    case 0xd: r = op2; if (s) { set_nz(c, r); set_flag(c, C_FLAG, sh_c); } break;
    case 0xe: r = a & ~op2; if (s) { set_nz(c, r); set_flag(c, C_FLAG, sh_c); } break;
    case 0xf: r = ~op2; if (s) { set_nz(c, r); set_flag(c, C_FLAG, sh_c); } break;
    }
    if (write) {
        if (rd == 15 && s) restore_cpsr_from_spsr(c);
        /* An exception return aligns PC for the restored ARM/Thumb state. */
        write_r(c, rd, r);
    }
}

static void op_psr(arm920t_t *c, uint32_t insn) {
    if ((insn & 0x0fbf0fff) == 0x010f0000) { /* MRS */
        uint32_t *s = spsr_ptr(c, mode(c));
        c->r[(insn >> 12) & 0xfu] = (insn & (1u << 22)) ? (s ? *s : c->cpsr) : c->cpsr;
        return;
    }
    if ((insn & 0x0db0f000) == 0x0120f000) { /* MSR */
        uint32_t val = (insn & (1u << 25)) ? gp32_ror32(insn & 0xffu, ((insn >> 8) & 0xfu) * 2u) : read_r(c, insn & 0xfu);
        uint32_t mask = 0;
        if (insn & (1u << 16)) mask |= 0x000000ffu;
        if (insn & (1u << 17)) mask |= 0x0000ff00u;
        if (insn & (1u << 18)) mask |= 0x00ff0000u;
        if (insn & (1u << 19)) mask |= 0xff000000u;
        if (!mask) mask = 0xf0000000u;
        if (insn & (1u << 22)) { uint32_t *s = spsr_ptr(c, mode(c)); if (s) *s = (*s & ~mask) | (val & mask); }
        else {
            /* User-mode MSR can write the flags byte only (DDI0100E A4-65). */
            if (mode(c) == MODE_USR) mask &= 0xff000000u;
            uint32_t newc = (c->cpsr & ~mask) | (val & mask);
            set_cpsr_full(c, newc);
        }
    }
}

static void op_mul(arm920t_t *c, uint32_t insn) {
    unsigned rd = (insn >> 16) & 0xfu, rn = (insn >> 12) & 0xfu, rs = (insn >> 8) & 0xfu, rm = insn & 0xfu;
    if (insn & (1u << 23)) { /* long */
        uint64_t a, b, res;
        unsigned rdhi = rd, rdlo = rn;
        if (insn & (1u << 22)) { a = (uint64_t)(int64_t)(int32_t)c->r[rm]; b = (uint64_t)(int64_t)(int32_t)c->r[rs]; }
        else { a = c->r[rm]; b = c->r[rs]; }
        res = a * b;
        if (insn & (1u << 21)) res += ((uint64_t)c->r[rdhi] << 32) | c->r[rdlo];
        c->r[rdlo] = (uint32_t)res;
        c->r[rdhi] = (uint32_t)(res >> 32);
        if (insn & (1u << 20)) {
            set_flag(c, N_FLAG, c->r[rdhi] & N_FLAG);
            set_flag(c, Z_FLAG, res == 0);
        }
    } else {
        uint32_t res = c->r[rm] * c->r[rs];
        if (insn & (1u << 21)) res += c->r[rn];
        c->r[rd] = res;
        if (insn & (1u << 20)) set_nz(c, res);
    }
}

static void op_halfword(arm920t_t *c, uint32_t insn) {
    unsigned rn = (insn >> 16) & 0xfu, rd = (insn >> 12) & 0xfu;
    int p = GP32_BIT(insn,24), u = GP32_BIT(insn,23), w = GP32_BIT(insn,21), l = GP32_BIT(insn,20);
    unsigned sh = (insn >> 5) & 3u;
    uint32_t off = (insn & (1u << 22)) ? (((insn >> 4) & 0xf0u) | (insn & 0xfu)) : c->r[insn & 0xfu];
    uint32_t base = read_r(c, rn), addr = p ? (u ? base + off : base - off) : base;
    uint32_t result = 0;
    if (l) {
        if (sh == 1) result = rb16(c, addr);
        else if (sh == 2) result = (uint32_t)(int32_t)(int8_t)rb8(c, addr);
        else if (sh == 3) result = (uint32_t)(int32_t)(int16_t)rb16(c, addr);
        write_r(c, rd, result);
    } else {
        if (sh == 1) wb16(c, addr, (uint16_t)c->r[rd]);
        else wb8(c, addr, (uint8_t)c->r[rd]);
    }
    if (!p || w) write_r(c, rn, u ? base + off : base - off);
}

static uint32_t addr_mode2_offset(arm920t_t *c, uint32_t insn) {
    if (!GP32_BIT(insn, 25)) return insn & 0xfffu;
    uint32_t v = read_r(c, insn & 0xfu);
    unsigned type = (insn >> 5) & 3u;
    unsigned amount = (insn >> 7) & 0x1fu;
    switch (type) {
    case 0: /* LSL */
        return amount < 32u ? (v << amount) : 0u;
    case 1: /* LSR; immediate #0 means #32 in address mode 2 */
        return amount ? (v >> amount) : 0u;
    case 2: /* ASR; immediate #0 means #32 in address mode 2 */
        return amount ? (uint32_t)((int32_t)v >> amount) : ((v & N_FLAG) ? UINT32_MAX : 0u);
    default: /* ROR; immediate #0 is RRX in address mode 2 */
        return amount ? gp32_ror32(v, amount) : (((c->cpsr & C_FLAG) ? 0x80000000u : 0u) | (v >> 1));
    }
}

static void op_single_dt(arm920t_t *c, uint32_t insn) {
    unsigned rn = (insn >> 16) & 0xfu, rd = (insn >> 12) & 0xfu;
    int p = GP32_BIT(insn,24), u = GP32_BIT(insn,23), b = GP32_BIT(insn,22), w = GP32_BIT(insn,21), l = GP32_BIT(insn,20);
    uint32_t base = read_r(c, rn), off = addr_mode2_offset(c, insn);
    uint32_t addr = p ? (u ? base + off : base - off) : base;
    if (l) {
        uint32_t v = b ? rb8(c, addr) : rb32(c, addr & ~3u);
        if (!b && (addr & 3u)) v = gp32_ror32(v, (addr & 3u) * 8u);
        write_r(c, rd, v);
    } else {
        uint32_t v = (rd == 15) ? (c->r[15] + 4u) : c->r[rd];
        if (b) wb8(c, addr, (uint8_t)v); else wb32(c, addr & ~3u, v);
    }
    if (!p || w) write_r(c, rn, u ? base + off : base - off);
}

static void op_block_dt(arm920t_t *c, uint32_t insn) {
    unsigned rn = (insn >> 16) & 0xfu;
    uint32_t list = insn & 0xffffu;
    int p = GP32_BIT(insn,24), u = GP32_BIT(insn,23), s = GP32_BIT(insn,22), w = GP32_BIT(insn,21), l = GP32_BIT(insn,20);
    unsigned count = 0; for (unsigned i=0;i<16;i++) if (list & (1u<<i)) count++;
    if (!count) count = 16;
    uint32_t base = read_r(c, rn);
    uint32_t addr;
    if (u) addr = p ? base + 4 : base;
    else addr = p ? base - 4u * count : base - 4u * count + 4u;
    uint32_t final_base = u ? base + 4u * count : base - 4u * count;
    int exception_return = l && s && (list & (1u << 15));
    int user_transfer = s && !exception_return;
    uint32_t loaded_pc = 0u;
    for (unsigned r=0;r<16;r++) if (list & (1u<<r)) {
        if (l) {
            uint32_t v = rb32(c, addr);
            if (user_transfer) write_user_r(c, r, v);
            else if (r == 15) { if (exception_return) loaded_pc = v; else write_r(c, 15u, v); } else c->r[r] = v;
        } else {
            uint32_t v = user_transfer ? read_user_r(c, r) : ((r == 15) ? c->r[15] + 4u : c->r[r]);
            wb32(c, addr, v);
        }
        addr += 4;
    }
    if (w && (!l || ((list & (1u << rn)) == 0))) write_r(c, rn, final_base);
    if (exception_return) {
        restore_cpsr_from_spsr(c);
        write_r(c, 15, loaded_pc);
    }
}

static uint32_t cp15_read(arm920t_t *c, unsigned crn, unsigned crm, unsigned op1, unsigned op2) {
    GP32_UNUSED(crm); GP32_UNUSED(op1); GP32_UNUSED(op2);
    switch (crn) {
    case 0: return 0x41129200u; /* Main ID */
    case 1: return c->cp15[1];
    case 2: return c->cp15[2];
    case 3: return c->cp15[3];
    case 5: return c->cp15[5];
    case 6: return c->cp15[6];
    case 13: return c->cp15[13];
    default: return c->cp15[crn & 15u];
    }
}
static void cp15_write(arm920t_t *c, unsigned crn, unsigned crm, unsigned op1, unsigned op2, uint32_t v) {
    GP32_UNUSED(op1);
    c->cp15[crn & 15u] = v;
    if (crn == 1 || crn == 2 || crn == 8 || crm == 8) {
        tlb_flush_all(c);
        arm920t_jit_invalidate_all(c, ARM_JIT_INV_CP15_MMU);
    } else if (crn == 7) {
        /* ARM920T c7 is cache/write-buffer maintenance.  D-cache clean/
           invalidate and write-buffer drain operations do not change the
           instruction stream or software TLB, so they should not discard the
           translated-code cache.  Keep the broad conservative I-cache/
           all-cache opcode handling for self-modifying code; this is not a
           claim that every c7 prefetch operation invalidates ARM920T caches. */
        if (crm == 5u || crm == 7u || (crm == 0u && op2 == 0u)) arm920t_jit_invalidate_all(c, ARM_JIT_INV_CP15_CACHE);
    }
}
static void op_coproc(arm920t_t *c, uint32_t insn) {
    unsigned cp = (insn >> 8) & 0xfu;
    if (cp != 15) return;
    if ((insn & 0x0f000010u) == 0x0e000010u) { /* MRC/MCR */
        unsigned op1 = (insn >> 21) & 7u, l = GP32_BIT(insn,20), crn = (insn >> 16) & 0xfu, rd = (insn >> 12) & 0xfu, op2 = (insn >> 5) & 7u, crm = insn & 0xfu;
        if (l) write_r(c, rd, cp15_read(c, crn, crm, op1, op2));
        else cp15_write(c, crn, crm, op1, op2, c->r[rd]);
    }
}

static int arm920t_armv4t_invalid_arm_insn(uint32_t insn) {
    /* ARM920T is ARMv4T.  Firebird targets ARM926/ARMv5TE and may translate
       BLX, CLZ, BKPT and DSP/saturating opcodes; those must be undefined on
       the GP32 CPU instead of being translated or treated as ARM926 CP15/cache
       behaviour. */
    return (insn & 0xfe000000u) == 0xfa000000u ||        /* BLX immediate */
           (insn & 0x0ffffff0u) == 0x012fff30u ||        /* BLX register */
           (insn & 0x0fff0ff0u) == 0x016f0f10u ||        /* CLZ */
           (insn & 0x0ff000f0u) == 0x01200070u ||        /* BKPT */
           (insn & 0x0f9000f0u) == 0x01000050u;          /* QADD/QSUB/QDADD/QDSUB */
}

static int arm920t_armv4t_rejects_arm(arm920t_t *c, uint32_t pc, uint32_t insn) {
    if (arm920t_armv4t_invalid_arm_insn(insn)) {
        exception_enter(c, MODE_UND, 0x04, pc + 4, 0);
        return 1;
    }
    return 0;
}

static int exec_arm_at(arm920t_t *c, uint32_t pc, uint32_t insn) {
    c->r[15] = pc + 4;
    if (c->trace && c->log) { char b[96]; snprintf(b, sizeof(b), "A %08" PRIx32 " %08" PRIx32 " CPSR=%08" PRIx32, pc, insn, c->cpsr); c->log(c->log_user, b); }
    if (!cond_pass(c, insn >> 28)) return 1;
    if (arm920t_armv4t_rejects_arm(c, pc, insn)) return 1;
    if ((insn & 0x0ffffff0u) == 0x012fff10u) { /* BX Rm */
        write_pc_x(c, c->r[insn & 0xfu]);
        return 1;
    }
    if ((insn & 0x0ffffff0u) == 0x012fff30u) { /* BLX Rm (ARMv5T interworking) */
        c->r[14] = pc + 4u;
        write_pc_x(c, c->r[insn & 0xfu]);
        return 1;
    }
    if ((insn & 0xfe000000u) == 0xfa000000u) { /* BLX immediate (ARMv5T) */
        int32_t off = gp32_sign_extend(((insn & 0x00ffffffu) << 2) | ((insn >> 23) & 2u), 26);
        c->r[14] = pc + 4u;
        c->cpsr |= T_FLAG;
        c->r[15] = (uint32_t)(pc + 8u + off) & ~1u;
        return 1;
    }
    if ((insn & 0x0fc000f0u) == 0x00000090u || (insn & 0x0f8000f0u) == 0x00800090u) { op_mul(c, insn); return 1; }
    if ((insn & 0x0fb00ff0u) == 0x01000090u) { unsigned rn=(insn>>16)&0xfu, rd=(insn>>12)&0xfu, rm=insn&0xfu; uint32_t a=c->r[rn]; uint32_t old=GP32_BIT(insn,22)?rb8(c,a):rb32(c,a); if(GP32_BIT(insn,22))wb8(c,a,(uint8_t)c->r[rm]);else wb32(c,a,c->r[rm]); c->r[rd]=old; return 1; }
    if ((insn & 0x0e000090u) == 0x00000090u) { op_halfword(c, insn); return 1; }
    if ((insn & 0x0c000000u) == 0x00000000u) {
        if ((insn & 0x01900000u) == 0x01000000u) { op_psr(c, insn); return 1; }
        op_data_proc(c, insn); return 1;
    }
    if ((insn & 0x0c000000u) == 0x04000000u) { op_single_dt(c, insn); return 1; }
    if ((insn & 0x0e000000u) == 0x08000000u) { op_block_dt(c, insn); return 1; }
    if ((insn & 0x0e000000u) == 0x0a000000u) {
        int32_t off = gp32_sign_extend((insn & 0x00ffffffu) << 2, 26);
        if (insn & (1u << 24)) c->r[14] = pc + 4;
        c->r[15] = (uint32_t)(pc + 8 + off);
        return 1;
    }
    if ((insn & 0x0f000000u) == 0x0f000000u) {
        uint32_t imm = insn & 0x00ffffffu;
        if (c->swi && c->swi(c->swi_user, c, imm, pc, 0)) return 1;
        exception_enter(c, MODE_SVC, 0x08, pc + 4, 0);
        return 1;
    }
    if ((insn & 0x0e000000u) == 0x0c000000u || (insn & 0x0f000000u) == 0x0e000000u) { op_coproc(c, insn); return 1; }
    exception_enter(c, MODE_UND, 0x04, pc + 4, 0);
    return 1;
}


static void exec_arm(arm920t_t *c) {
    uint32_t pc = c->r[15];
    uint32_t insn = rb32(c, pc);
    (void)exec_arm_at(c, pc, insn);
}

static void t_set_nz(arm920t_t *c, uint32_t v) { set_nz(c, v); }
static void thumb_alu_flags_add(arm920t_t *c, uint32_t a, uint32_t b, uint32_t r) { add_flags(c,a,b,r); }
static void thumb_alu_flags_sub(arm920t_t *c, uint32_t a, uint32_t b, uint32_t r) { sub_flags(c,a,b,r); }

static void exec_thumb(arm920t_t *c) {
    uint32_t pc = c->r[15];
    uint16_t op = rb16(c, pc);
    c->r[15] = pc + 2;
    if (c->trace && c->log) { char b[96]; snprintf(b, sizeof(b), "T %08" PRIx32 " %04x CPSR=%08" PRIx32, pc, op, c->cpsr); c->log(c->log_user, b); }
    if ((op & 0xe000u) == 0x0000u) {
        unsigned sub = (op >> 11) & 3u;
        if (sub < 3) {
            unsigned rd = op & 7u, rs = (op >> 3) & 7u, imm = (op >> 6) & 0x1fu;
            uint32_t v = c->r[rs], r = v;
            if (sub == 0) { r = v << imm; set_flag(c, C_FLAG, imm ? !!(v & (1u << (32u-imm))) : !!(c->cpsr & C_FLAG)); }
            else if (sub == 1) { if (!imm) imm = 32; r = v >> imm; set_flag(c, C_FLAG, !!(v & (1u << (imm-1)))); }
            else { if (!imm) imm = 32; r = (uint32_t)((int32_t)v >> imm); set_flag(c, C_FLAG, !!(v & (1u << (imm-1)))); }
            c->r[rd] = r; t_set_nz(c, r); return;
        } else {
            unsigned rd = op & 7u, rs = (op >> 3) & 7u, rn = (op >> 6) & 7u;
            uint32_t b = (op & (1u<<10)) ? rn : c->r[rn];
            uint32_t a = c->r[rs], r;
            if (op & (1u<<9)) { r = a - b; thumb_alu_flags_sub(c,a,b,r); } else { r = a + b; thumb_alu_flags_add(c,a,b,r); }
            c->r[rd] = r; return;
        }
    }
    if ((op & 0xe000u) == 0x2000u) { /* mov/cmp/add/sub immediate */
        unsigned rd = (op >> 8) & 7u, imm = op & 0xffu;
        switch ((op >> 11) & 3u) {
        case 0: c->r[rd] = imm; t_set_nz(c, imm); break;
        case 1: { uint32_t r = c->r[rd] - imm; thumb_alu_flags_sub(c, c->r[rd], imm, r); break; }
        case 2: { uint32_t r = c->r[rd] + imm; c->r[rd] = r; thumb_alu_flags_add(c, c->r[rd] - imm, imm, r); break; }
        case 3: { uint32_t r = c->r[rd] - imm; c->r[rd] = r; thumb_alu_flags_sub(c, c->r[rd] + imm, imm, r); break; }
        }
        return;
    }
    if ((op & 0xfc00u) == 0x4000u) { /* ALU */
        unsigned rd = op & 7u, rs = (op >> 3) & 7u, opc = (op >> 6) & 0xfu;
        uint32_t a = c->r[rd], b = c->r[rs], r = a;
        switch (opc) {
        case 0: r = a & b; c->r[rd]=r; t_set_nz(c,r); break;
        case 1: r = a ^ b; c->r[rd]=r; t_set_nz(c,r); break;
        case 2: { unsigned s=b&0xffu; if(s){ set_flag(c,C_FLAG,s<32?!!(a&(1u<<(32-s))):s==32?!!(a&1u):0); r=s<32?a<<s:0; } c->r[rd]=r; t_set_nz(c,r); break; }
        case 3: { unsigned s=b&0xffu; if(!s){} else if(s<32){set_flag(c,C_FLAG,!!(a&(1u<<(s-1)))); r=a>>s;} else {set_flag(c,C_FLAG,s==32?!!(a&N_FLAG):0); r=0;} c->r[rd]=r; t_set_nz(c,r); break; }
        case 4: { unsigned s=b&0xffu; if(!s){} else if(s<32){set_flag(c,C_FLAG,!!(a&(1u<<(s-1)))); r=(uint32_t)((int32_t)a>>s);} else {set_flag(c,C_FLAG,!!(a&N_FLAG)); r=(a&N_FLAG)?UINT32_MAX:0;} c->r[rd]=r; t_set_nz(c,r); break; }
        case 5: { uint32_t ci=(c->cpsr&C_FLAG)?1u:0u; r=a+b+ci; c->r[rd]=r; add_carry_flags(c,a,b,ci,r); break; }
        case 6: { uint32_t bi=(c->cpsr&C_FLAG)?0u:1u; r=a-b-bi; c->r[rd]=r; sub_borrow_flags(c,a,b,bi,r); break; }
        case 7: { unsigned s=b&31u; r=gp32_ror32(a,s); if(s)set_flag(c,C_FLAG,!!(r&N_FLAG)); c->r[rd]=r; t_set_nz(c,r); break; }
        case 8: r=a&b; t_set_nz(c,r); break;
        case 9: r=(uint32_t)(-((int32_t)b)); c->r[rd]=r; sub_flags(c,0,b,r); break;
        case 10: r=a-b; sub_flags(c,a,b,r); break;
        case 11: r=a+b; add_flags(c,a,b,r); break;
        case 12: r=a|b; c->r[rd]=r; t_set_nz(c,r); break;
        case 13: r=a*b; c->r[rd]=r; t_set_nz(c,r); break;
        case 14: r=a&~b; c->r[rd]=r; t_set_nz(c,r); break;
        case 15: r=~b; c->r[rd]=r; t_set_nz(c,r); break;
        }
        return;
    }
    if ((op & 0xfc00u) == 0x4400u) { /* high reg ops / bx */
        unsigned h1 = (op >> 7) & 1u, h2 = (op >> 6) & 1u, rs = ((op >> 3) & 7u) | (h2 << 3), rd = (op & 7u) | (h1 << 3);
        uint32_t a = (rd == 15) ? (pc + 4) : c->r[rd], b = (rs == 15) ? (pc + 4) : c->r[rs];
        switch ((op >> 8) & 3u) {
        case 0: write_r(c, rd, a + b); break;
        case 1: { uint32_t r = a - b; sub_flags(c,a,b,r); break; }
        case 2: write_r(c, rd, b); break;
        case 3:
            if (h1) { exception_enter(c, MODE_UND, 0x04, pc + 2, 0); return; } /* BLX Rm is ARMv5T, not ARM920T/ARMv4T. */
            write_pc_x(c, b);
            break;
        }
        return;
    }
    if ((op & 0xf800u) == 0x4800u) { unsigned rd=(op>>8)&7u; uint32_t addr=((pc+4)&~2u)+((op&0xffu)<<2); c->r[rd]=rb32(c,addr); return; }
    if ((op & 0xf000u) == 0x5000u) { /* reg offset load/store */
        unsigned rd=op&7u, rb=(op>>3)&7u, ro=(op>>6)&7u; uint32_t addr=c->r[rb]+c->r[ro]; unsigned kind=(op>>9)&7u;
        switch(kind){case 0: wb32(c,addr,c->r[rd]);break;case 1: wb16(c,addr,(uint16_t)c->r[rd]);break;case 2: wb8(c,addr,(uint8_t)c->r[rd]);break;case 3: c->r[rd]=(uint32_t)(int8_t)rb8(c,addr);break;case 4: c->r[rd]=rb32(c,addr);break;case 5: c->r[rd]=rb16(c,addr);break;case 6: c->r[rd]=rb8(c,addr);break;case 7: c->r[rd]=(uint32_t)(int16_t)rb16(c,addr);break;} return;
    }
    if ((op & 0xf000u) == 0x6000u) { unsigned rd=op&7u, rb=(op>>3)&7u, imm=(op>>6)&0x1fu; uint32_t addr=c->r[rb]+(imm<<2); if(op&0x0800)c->r[rd]=rb32(c,addr); else wb32(c,addr,c->r[rd]); return; }
    if ((op & 0xf000u) == 0x7000u) { unsigned rd=op&7u, rb=(op>>3)&7u, imm=(op>>6)&0x1fu; uint32_t addr=c->r[rb]+imm; if(op&0x0800)c->r[rd]=rb8(c,addr); else wb8(c,addr,(uint8_t)c->r[rd]); return; }
    if ((op & 0xf000u) == 0x8000u) { unsigned rd=op&7u, rb=(op>>3)&7u, imm=(op>>6)&0x1fu; uint32_t addr=c->r[rb]+(imm<<1); if(op&0x0800)c->r[rd]=rb16(c,addr); else wb16(c,addr,(uint16_t)c->r[rd]); return; }
    if ((op & 0xf000u) == 0x9000u) { unsigned rd=(op>>8)&7u; uint32_t addr=c->r[13]+((op&0xffu)<<2); if(op&0x0800)c->r[rd]=rb32(c,addr); else wb32(c,addr,c->r[rd]); return; }
    if ((op & 0xf000u) == 0xa000u) { unsigned rd=(op>>8)&7u; uint32_t imm=(op&0xffu)<<2; c->r[rd]=((op&0x0800)?c->r[13]:((pc+4)&~2u))+imm; return; }
    if ((op & 0xff00u) == 0xb000u) { uint32_t imm=(op&0x7fu)<<2; if(op&0x80)c->r[13]-=imm; else c->r[13]+=imm; return; }
    if ((op & 0xf600u) == 0xb400u) { /* push/pop */
        uint32_t list=op&0xffu; int pop=!!(op&0x0800), pc_lr=!!(op&0x0100);
        if(pop){ for(unsigned r=0;r<8;r++) if(list&(1u<<r)){c->r[r]=rb32(c,c->r[13]); c->r[13]+=4;} if(pc_lr){uint32_t v=rb32(c,c->r[13]); c->r[13]+=4; write_r(c,15u,v);} }
        else { if(pc_lr){c->r[13]-=4; wb32(c,c->r[13],c->r[14]);} for(int r=7;r>=0;r--) if(list&(1u<<r)){c->r[13]-=4; wb32(c,c->r[13],c->r[r]);} }
        return;
    }
    if ((op & 0xf000u) == 0xc000u) { unsigned rb=(op>>8)&7u; uint32_t list=op&0xffu, addr=c->r[rb]; if(op&0x0800){for(unsigned r=0;r<8;r++)if(list&(1u<<r)){c->r[r]=rb32(c,addr);addr+=4;}}else{for(unsigned r=0;r<8;r++)if(list&(1u<<r)){wb32(c,addr,c->r[r]);addr+=4;}} c->r[rb]=addr; return; }
    if ((op & 0xff00u) == 0xdf00u) {
        uint32_t imm = op & 0xffu;
        if (c->swi && c->swi(c->swi_user, c, imm, pc, 1)) return;
        exception_enter(c, MODE_SVC, 0x08, pc + 2, 0);
        return;
    }
    if ((op & 0xf000u) == 0xd000u) { unsigned cond=(op>>8)&0xfu; if(cond<14 && cond_pass(c,cond)) c->r[15]=(uint32_t)(pc+4+gp32_sign_extend((op&0xffu)<<1,9)); return; }
    if ((op & 0xf800u) == 0xe000u) { c->r[15]=(uint32_t)(pc+4+gp32_sign_extend((op&0x7ffu)<<1,12)); return; }
    if ((op & 0xf800u) == 0xf000u) { c->r[14]=(uint32_t)(pc+4+gp32_sign_extend((op&0x7ffu)<<12,23)); return; }
    if ((op & 0xf800u) == 0xe800u) { exception_enter(c, MODE_UND, 0x04, pc + 2, 0); return; } /* Thumb BLX suffix is ARMv5T. */
    if ((op & 0xf800u) == 0xf800u) { uint32_t target=c->r[14]+((op&0x7ffu)<<1); c->r[14]=(pc+2)|1u; c->r[15]=target&~1u; return; }
    exception_enter(c, MODE_UND, 0x04, pc + 2, 0);
}

/* Unlike rb32, speculative leaf fetches and cache revalidation must not walk
 * page tables, refill the TLB, update fault state or read an MMIO callback. */
ARM_FORCE_INLINE int arm_jit_peek_fetch(arm920t_t *c, uint32_t pc, uint32_t *insn) {
    uint32_t pa = pc;
    if (pa & 3u) return 0;
    if (c->cp15[1] & 1u) {
        const arm_tlb_entry_t *e = &c->tlb_entry[(pa >> 12) & 0xfffu];
        if (!e->valid || (pa & ~e->mask) != e->va_base || e->mask < 3u ||
            (pa & e->mask) > e->mask - 3u) return 0;
        pa = e->pa_base | (pa & e->mask);
    }
    const uint8_t *p = fastmem(c, pa, 4u, 0);
    if (!p) return 0;
    *insn = gp32_ld32le(p);
    return 1;
}

/* Prove every recorded fetch, including stitched branches and inlined leaves.
 * A missing mapping is unprovable, even if identity memory happens to match. */
static int arm_jit_fetch_unchanged(arm920t_t *c, const arm_jit_block_t *b) {
    if (!b->count || b->count > ARM_JIT_MAX_INSNS) return 0;
    for (unsigned i = 0; i < b->count; ++i) {
        const arm_jit_op_t *op = &arm_jit_ops(c, b)[i];
        uint32_t insn;
        if (!arm_jit_peek_fetch(c, op->pc, &insn) || insn != op->insn) return 0;
    }
    return 1;
}

static void arm920t_jit_invalidate_all(arm920t_t *c, unsigned cause) {
    if (!c) return;
    ARM_PROF_INC(c, jit_invalidations);
#if ARM920T_PROFILING
    /* Snapshot pre-flush occupancy so the high-water mark survives the
     * jit_code_used reset on every backend, including the AArch64 in-inc
     * reserve path that bypasses arm_jit_code_reserve. */
    if (c->jit_code_used > c->prof.jit_code_highwater)
        c->prof.jit_code_highwater = c->jit_code_used;
    switch (cause) {
    case ARM_JIT_INV_RESET: c->prof.jit_inv_reset++; break;
    case ARM_JIT_INV_JIT_DISABLE: c->prof.jit_inv_jit_disable++; break;
    case ARM_JIT_INV_API_FLUSH: c->prof.jit_inv_api_flush++; break;
    case ARM_JIT_INV_STATE_LOAD: c->prof.jit_inv_state_load++; break;
    case ARM_JIT_INV_CP15_MMU: c->prof.jit_inv_cp15_mmu++; break;
    case ARM_JIT_INV_CP15_CACHE: c->prof.jit_inv_cp15_cache++; break;
    case ARM_JIT_INV_CODE_RECYCLE: c->prof.jit_inv_code_recycle++; break;
    default: break;
    }
#endif
    if (cause == ARM_JIT_INV_CP15_CACHE) {
        /* Native memory helpers also depend on the cached physical windows.
         * A changed bus window needs the ordinary full flush and fresh bases. */
        uint8_t *ram = fastmem(c, 0x0c000000u, 1u, 0);
        uint8_t *bios = fastmem(c, 0x00000000u, 1u, 0);
        if (ram == c->jit_ram_base && bios == c->jit_bios_base) {
            /* MCR ends the active trace. Defer byte checks until a block is
             * next dispatched, while preserving its arena and baked native
             * generation. A wrapping epoch takes the full-flush path below. */
            if (++c->jit_cache_epoch) return;
        }
        c->jit_ram_base = ram;
        c->jit_bios_base = bios;
    }
    c->jit_cache_epoch = 1;
    c->jit_generation++;
    if (!c->jit_generation) {
        c->jit_generation = 1;
        if (c->jit_blocks) memset(c->jit_blocks, 0, ARM_JIT_BLOCK_COUNT * sizeof(c->jit_blocks[0]));
    }
    c->jit_code_used = 0;
}

static int arm_jit_is_local_cp15_op(uint32_t insn);
static int arm_jit_may_write_pc(uint32_t insn) {
    if ((insn & 0x0ffffff0u) == 0x012fff10u || (insn & 0x0ffffff0u) == 0x012fff30u) return 1;
    if ((insn & 0xfe000000u) == 0xfa000000u) return 1;
    if ((insn & 0x0e000000u) == 0x0a000000u) {
        /* Firebird-style fall-through traces: a conditional branch only ends
           the native block when it is taken.  Keep decoding the not-taken
           path so hot compare/Bcc/fallthrough sequences do not return to the
           dispatcher every time the branch fails. */
        return ((insn >> 28) == 14u);
    }
    if ((insn & 0x0f000000u) == 0x0f000000u) return 1;
    if ((insn & 0x0c000000u) == 0x00000000u) {
        if ((insn & 0x01900000u) == 0x01000000u) {
            /* Reading status into an ordinary register or writing a saved
             * status bank cannot change the active execution state. Keep
             * these in the trace; CPSR writes and unknown PSR encodings
             * retain their dispatch boundary. */
            if ((insn & 0x0fbf0fffu) == 0x010f0000u)
                return ((insn >> 12) & 0xfu) == 15u;
            if ((insn & 0x0db0f000u) == 0x0120f000u && (insn & (1u << 22)))
                return 0;
            return 1;
        }
        return (((insn >> 12) & 0xfu) == 15u);
    }
    if ((insn & 0x0c000000u) == 0x04000000u) return GP32_BIT(insn,20) && (((insn >> 12) & 0xfu) == 15u);
    if ((insn & 0x0e000000u) == 0x08000000u) return GP32_BIT(insn,20) && (insn & (1u << 15));
    if ((insn & 0x0e000090u) == 0x00000090u) return GP32_BIT(insn,20) && (((insn >> 12) & 0xfu) == 15u);
    if ((insn & 0x0f000000u) == 0x0e000000u) return !arm_jit_is_local_cp15_op(insn); /* Local p15 reads/data maintenance can stay in-trace. */
    return 0;
}

static int arm_jit_is_uncond_b_no_link(uint32_t insn) {
    return ((insn >> 28) == 14u) && ((insn & 0x0f000000u) == 0x0a000000u);
}

static int arm_jit_is_local_cp15_op(uint32_t insn) {
    if ((insn & 0x0f000010u) != 0x0e000010u) return 0;
    if (((insn >> 8) & 0xfu) != 15u || ((insn >> 12) & 0xfu) == 15u) return 0;
    if (GP32_BIT(insn, 20)) return 1;
    /* In this core, data-cache maintenance/drain only records c7. Match
     * cp15_write's behavior; I-cache invalidation and MMU/TLB writes must
     * still leave the trace. Do not generalize this to arbitrary MCRs. */
    unsigned crn = (insn >> 16) & 15u, crm = insn & 15u;
    return crn == 7u && (crm == 6u || crm == 10u || crm == 14u);
}

static int arm_jit_is_uncond_bl(uint32_t insn) {
    return ((insn >> 28) == 14u) && ((insn & 0x0f000000u) == 0x0b000000u);
}

static uint32_t arm_jit_branch_target(uint32_t pc, uint32_t insn) {
    int32_t off = gp32_sign_extend((insn & 0x00ffffffu) << 2, 26);
    return (uint32_t)(pc + 8u + off) & ~3u;
}

static int arm_jit_is_leaf_pop_pc(uint32_t insn) {
    return insn == 0xe8bd8000u || insn == 0xe49df004u; /* LDM / LDR pc,[sp],#4 */
}

static int arm_jit_is_leaf_push_lr(uint32_t insn) {
    return insn == 0xe92d4000u || insn == 0xe52de004u; /* STM / STR lr,[sp,#-4]! */
}

static int arm_jit_insn_mentions_sp(uint32_t insn) {
    unsigned cls = insn & 0x0c000000u;
    if (cls == 0x00000000u) {
        if (((insn >> 16) & 0xfu) == 13u || ((insn >> 12) & 0xfu) == 13u || (insn & 0xfu) == 13u) return 1;
        if ((insn & (1u << 4)) && (((insn >> 8) & 0xfu) == 13u)) return 1;
    } else if (cls == 0x04000000u) {
        if (((insn >> 16) & 0xfu) == 13u || ((insn >> 12) & 0xfu) == 13u) return 1;
        if ((insn & (1u << 25)) && (insn & 0xfu) == 13u) return 1;
    } else if ((insn & 0x0e000000u) == 0x08000000u) {
        if (((insn >> 16) & 0xfu) == 13u || (insn & (1u << 13))) return 1;
    } else if ((insn & 0x0e000090u) == 0x00000090u) {
        if (((insn >> 16) & 0xfu) == 13u || ((insn >> 12) & 0xfu) == 13u || (insn & 0xfu) == 13u) return 1;
    }
    return 0;
}

static int arm_jit_is_side_effect_free_nop(uint32_t insn) {
    if (insn == 0x00000000u) return 1;
    if ((insn & 0x0ffffff0u) == 0x01a00000u) {
        unsigned rd = (insn >> 12) & 0xfu;
        unsigned rm = insn & 0xfu;
        return rd == rm && rd != 15u;
    }
    return 0;
}

static arm_jit_kind_t arm_jit_classify(uint32_t insn) {
    if (arm920t_armv4t_invalid_arm_insn(insn)) return ARM_JIT_OP_UNDEFINED;
    if ((insn & 0x0ffffff0u) == 0x012fff10u) return ARM_JIT_OP_INTERP;
    if ((insn & 0x0fc000f0u) == 0x00000090u || (insn & 0x0f8000f0u) == 0x00800090u) return ARM_JIT_OP_MUL;
    if ((insn & 0x0fb00ff0u) == 0x01000090u) return ARM_JIT_OP_SWP;
    if ((insn & 0x0e000090u) == 0x00000090u) return ARM_JIT_OP_HALF;
    if ((insn & 0x0c000000u) == 0x00000000u) return ((insn & 0x01900000u) == 0x01000000u) ? ARM_JIT_OP_PSR : ARM_JIT_OP_DATA;
    if ((insn & 0x0c000000u) == 0x04000000u) return ARM_JIT_OP_SINGLE_DT;
    if ((insn & 0x0e000000u) == 0x08000000u) return ARM_JIT_OP_BLOCK_DT;
    if ((insn & 0x0e000000u) == 0x0a000000u) return ARM_JIT_OP_BRANCH;
    if ((insn & 0x0f000000u) == 0x0f000000u) return ARM_JIT_OP_SWI;
    if ((insn & 0x0e000000u) == 0x0c000000u || (insn & 0x0f000000u) == 0x0e000000u) return ARM_JIT_OP_COPROC;
    return ARM_JIT_OP_INTERP;
}


#define ARM_BC_DATA_S        0x01u
#define ARM_BC_DATA_IMM      0x02u
#define ARM_BC_DATA_REGSHIFT 0x04u
#define ARM_BC_DATA_IMM_C    0x20u
#define ARM_BC_DATA_IMM_CV   0x40u
#define ARM_BC_SD_P          0x01u
#define ARM_BC_SD_U          0x02u
#define ARM_BC_SD_B          0x04u
#define ARM_BC_SD_W          0x08u
#define ARM_BC_SD_L          0x10u
#define ARM_BC_SD_REGOFF     0x20u
#define ARM_BC_HALF_P        0x01u
#define ARM_BC_HALF_U        0x02u
#define ARM_BC_HALF_W        0x04u
#define ARM_BC_HALF_L        0x08u
#define ARM_BC_HALF_IMM      0x10u

static void arm_bc_decode_data(arm_jit_op_t *op, uint32_t insn) {
    unsigned rot;
    op->a = (uint8_t)((insn >> 21) & 0x0fu); /* opcode */
    op->b = (uint8_t)((insn >> 16) & 0x0fu); /* Rn */
    op->c = (uint8_t)((insn >> 12) & 0x0fu); /* Rd */
    op->d = (uint8_t)(GP32_BIT(insn, 20) ? ARM_BC_DATA_S : 0u);
    op->e = (uint8_t)(insn & 0x0fu);          /* Rm */
    op->f = (uint8_t)((insn >> 8) & 0x0fu);  /* Rs for register shifts */
    op->g = (uint8_t)((insn >> 5) & 0x03u);  /* shift type */
    op->h = (uint8_t)((insn >> 7) & 0x1fu);  /* immediate shift amount */
    if (GP32_BIT(insn, 25)) {
        op->d |= ARM_BC_DATA_IMM;
        rot = ((insn >> 8) & 0x0fu) * 2u;
        op->imm = gp32_ror32(insn & 0xffu, rot);
        if (rot) {
            op->d |= ARM_BC_DATA_IMM_CV;
            if (op->imm & N_FLAG) op->d |= ARM_BC_DATA_IMM_C;
        }
    } else {
        op->imm = 0;
        if (GP32_BIT(insn, 4)) op->d |= ARM_BC_DATA_REGSHIFT;
    }
}

static void arm_bc_decode_single_dt(arm_jit_op_t *op, uint32_t insn) {
    op->a = (uint8_t)((insn >> 16) & 0x0fu); /* Rn */
    op->b = (uint8_t)((insn >> 12) & 0x0fu); /* Rd */
    op->c = (uint8_t)(insn & 0x0fu);         /* Rm for register offset */
    op->d = 0;
    if (GP32_BIT(insn, 24)) op->d |= ARM_BC_SD_P;
    if (GP32_BIT(insn, 23)) op->d |= ARM_BC_SD_U;
    if (GP32_BIT(insn, 22)) op->d |= ARM_BC_SD_B;
    if (GP32_BIT(insn, 21)) op->d |= ARM_BC_SD_W;
    if (GP32_BIT(insn, 20)) op->d |= ARM_BC_SD_L;
    op->e = (uint8_t)((insn >> 5) & 0x03u);  /* shift type */
    op->f = (uint8_t)((insn >> 7) & 0x1fu);  /* shift amount */
    if (GP32_BIT(insn, 25)) {
        op->d |= ARM_BC_SD_REGOFF;
        op->imm = 0;
    } else {
        op->imm = insn & 0x0fffu;
    }
}

static void arm_bc_decode_half(arm_jit_op_t *op, uint32_t insn) {
    op->a = (uint8_t)((insn >> 16) & 0x0fu); /* Rn */
    op->b = (uint8_t)((insn >> 12) & 0x0fu); /* Rd */
    op->c = (uint8_t)(insn & 0x0fu);         /* Rm for register offset */
    op->d = 0;
    if (GP32_BIT(insn, 24)) op->d |= ARM_BC_HALF_P;
    if (GP32_BIT(insn, 23)) op->d |= ARM_BC_HALF_U;
    if (GP32_BIT(insn, 21)) op->d |= ARM_BC_HALF_W;
    if (GP32_BIT(insn, 20)) op->d |= ARM_BC_HALF_L;
    if (GP32_BIT(insn, 22)) {
        op->d |= ARM_BC_HALF_IMM;
        op->imm = (((insn >> 4) & 0xf0u) | (insn & 0x0fu));
    } else {
        op->imm = 0;
    }
    op->e = (uint8_t)((insn >> 5) & 0x03u);  /* 1=half, 2=sbyte, 3=shalf */
    op->f = op->g = op->h = 0;
}

static void arm_bc_decode_op(arm_jit_op_t *op) {
    switch ((arm_jit_kind_t)op->kind) {
    case ARM_JIT_OP_DATA: arm_bc_decode_data(op, op->insn); break;
    case ARM_JIT_OP_SINGLE_DT: arm_bc_decode_single_dt(op, op->insn); break;
    case ARM_JIT_OP_HALF: arm_bc_decode_half(op, op->insn); break;
    case ARM_JIT_OP_BLOCK_DT: {
        uint32_t list = op->insn & 0xffffu;
        unsigned count = 0;
        for (unsigned i = 0; i < 16u; ++i) if (list & (1u << i)) ++count;
        if (!count) count = 16u;
        op->a = (uint8_t)((op->insn >> 16) & 0x0fu);
        op->d = (uint8_t)(((op->insn >> 20) & 0x1fu));
        op->imm = list;
        op->g = (uint8_t)count;
        op->b = op->c = op->e = op->f = op->h = 0;
        break;
    }
    default:
        op->imm = 0; op->a = op->b = op->c = op->d = op->e = op->f = op->g = op->h = 0;
        break;
    }
}

static int arm_jit_alloc(arm920t_t *c) {
    if (c->jit_blocks) return 1;
    c->jit_blocks = (arm_jit_block_t *)calloc(ARM_JIT_BLOCK_COUNT, sizeof(c->jit_blocks[0]));
    if (!c->jit_blocks) return 0;
    c->jit_ops = calloc(ARM_JIT_BLOCK_COUNT, sizeof(c->jit_ops[0]));
    if (!c->jit_ops) {
        free(c->jit_blocks);
        c->jit_blocks = NULL;
        return 0;
    }
    return 1;
}

static int arm_jit_is_unframed_leaf_data(uint32_t insn) {
    if ((insn >> 28) == 15u || arm_jit_classify(insn) != ARM_JIT_OP_DATA ||
        arm_jit_may_write_pc(insn)) return 0;
    arm_jit_op_t op;
    arm_bc_decode_data(&op, insn);
    int test = op.a >= 8u && op.a <= 11u;
    if (!test && op.c >= 13u) return 0; /* no SP/LR/PC writes */
    if (op.a != 13u && op.a != 15u && op.b == 13u) return 0;
    if (!(op.d & ARM_BC_DATA_IMM) &&
        (op.e == 13u || ((op.d & ARM_BC_DATA_REGSHIFT) && op.f == 13u))) return 0;
    /* Read-only LR and pipeline PC operands use the existing op->pc paths.
     * Only ALU flags can change CPSR; there are no memory/status callbacks. */
    return 1;
}

static void arm_jit_compile_native(arm920t_t *c, arm_jit_block_t *b);

/* Flatten a short framed call, optionally containing one level of framed
 * calls. Every instruction and stack transfer remains real: the emitters
 * must check a loaded return against the decoded successor. Translation
 * peeks only mapped code, never speculative MMIO, and each callee stays on
 * its own fetch page. A failed collection publishes no partial trace. */
static unsigned arm_jit_collect_framed(arm920t_t *c, uint32_t pc,
                                      arm_jit_op_t *out, unsigned capacity,
                                      unsigned depth) {
    unsigned count = 0;
    int no_sp_refs = 1;
    int nested_call = 0;
    for (unsigned i = 0; i < (depth ? 8u : 16u); ++i) {
        uint32_t cur = pc + i * 4u, insn;
        if (count == capacity || ((cur ^ pc) & ~ARM_JIT_PAGE_MASK) ||
            !arm_jit_peek_fetch(c, cur, &insn)) return 0;
        if (!i && !arm_jit_is_leaf_push_lr(insn)) return 0;
        arm_jit_op_t *op = &out[count++];
        *op = (arm_jit_op_t){.pc = cur, .insn = insn,
            .cond = (uint8_t)(insn >> 28), .kind = (uint8_t)arm_jit_classify(insn),
            .stop = (uint8_t)arm_jit_may_write_pc(insn)};
        arm_bc_decode_op(op);
        if (arm_jit_is_leaf_pop_pc(insn)) {
            if (i >= 8u && !nested_call) return 0;
            op->stop = 0;
            op->reserved = no_sp_refs ? 5u : 3u;
            if (no_sp_refs) out[0].reserved = 4u;
            return count;
        }
        if (!depth && out[0].insn == 0xe52de004u && arm_jit_is_uncond_bl(insn)) {
            unsigned nested = arm_jit_collect_framed(c,
                arm_jit_branch_target(cur, insn), out + count, capacity - count, 1u);
            if (!nested) return 0;
            op->stop = 0;
            op->reserved = 2u;
            count += nested;
            nested_call = 1;
            no_sp_refs = 0; /* nested frames are not a stable-poll proof */
            continue;
        }
        if (op->kind == ARM_JIT_OP_UNDEFINED || op->kind == ARM_JIT_OP_COPROC ||
            op->kind == ARM_JIT_OP_SWI || op->kind == ARM_JIT_OP_BRANCH ||
            op->kind == ARM_JIT_OP_PSR || op->kind == ARM_JIT_OP_SWP || op->stop)
            return 0;
        if (i && arm_jit_insn_mentions_sp(insn)) no_sp_refs = 0;
    }
    return 0;
}

/* Prefer the taken edge only when a short forward arm closes this loop.
 * This is a trace-layout hint, not permission to skip any guest operation.
 * Keep plain loops on their existing path; joining them can create costly
 * portable polling candidates where native execution was already effective.
 * Speculative inspection must neither touch MMIO nor populate a mapping. */
static int arm_jit_forward_loop(arm920t_t *c, uint32_t entry,
                                uint32_t pc, uint32_t insn) {
    if ((insn >> 28) >= 14u || (insn & 0x0f000000u) != 0x0a000000u) return 0;
    uint32_t target = arm_jit_branch_target(pc, insn);
    if (target <= pc || target - pc <= 4u ||
        ((target ^ entry) & ~ARM_JIT_PAGE_MASK)) return 0;
    int call = 0;
    for (unsigned i = 0; i < 8u; ++i) {
        uint32_t cur = target + i * 4u, next;
        if (((cur ^ entry) & ~ARM_JIT_PAGE_MASK) ||
            !arm_jit_peek_fetch(c, cur, &next)) break;
        if ((next & 0x0f000000u) == 0x0a000000u) {
            return call && (next >> 28) != 15u &&
                   arm_jit_branch_target(cur, next) == entry;
        }
        if (arm_jit_is_uncond_bl(next)) call = 1;
        if (arm_jit_may_write_pc(next) && !arm_jit_is_uncond_bl(next)) break;
    }
    return 0;
}

/* Prove only a straight-line, ordinary-register/load loop with one backedge.
 * A conditional branch may leave the loop, but may not bypass the progress
 * instruction and rejoin it. Calls, stitched traces and leaf wrappers retain
 * the polling path. Count ALL possible writes, including those before the
 * step: a counter reloaded/reset on each iteration is not progress.
 * A single unconditional self ADD/SUB by a decoded nonzero immediate changes
 * its register modulo 2^32, even on overflow and with S set. Flag-only tests
 * do not write Rd; loads do, including Rd==Rn. No SP/LR/PC proof is attempted. */
static int arm_jit_poll_has_progress(arm920t_t *c, arm_jit_block_t *b) {
    unsigned edge = 0, edges = 0;
    const arm_jit_op_t *ops = arm_jit_ops(c, b);
    for (unsigned i = 0; i < b->poll_prefix; ++i) {
        const arm_jit_op_t *op = &ops[i];
        if (op->kind == ARM_JIT_OP_BRANCH && op->cond != 15u &&
            !(op->insn & (1u << 24)) &&
            arm_jit_branch_target(op->pc, op->insn) == b->tag_pc) {
            edge = i;
            ++edges;
        }
    }
    if (edges != 1u || (uint64_t)b->tag_pc + edge * 4u > UINT32_MAX) return 0;
    uint32_t written = 0, repeated = 0, progress = 0;
    for (unsigned i = 0; i <= edge; ++i) {
        const arm_jit_op_t *op = &ops[i];
        if (op->reserved || op->pc != b->tag_pc + i * 4u) return 0;
        uint32_t dest = 0;
        if (op->kind == ARM_JIT_OP_BRANCH) {
            if (op->insn & (1u << 24)) return 0;
            if (i != edge) {
                uint32_t target = arm_jit_branch_target(op->pc, op->insn);
                if (op->cond >= 14u || (target >= b->tag_pc && target <= ops[edge].pc)) return 0;
            }
        } else if (op->kind == ARM_JIT_OP_DATA) {
            if (op->a >= 8u && op->a <= 11u) continue; /* TST/TEQ/CMP/CMN */
            if (op->c >= 13u) return 0;
            dest = 1u << op->c;
            if (op->cond == 14u && (op->a == 2u || op->a == 4u) &&
                op->b == op->c && (op->d & ARM_BC_DATA_IMM) && op->imm)
                progress |= dest;
        } else if (op->kind == ARM_JIT_OP_SINGLE_DT) {
            /* Loads only, without writeback, byte or register offsets. */
            if ((op->d & ~ARM_BC_SD_U) != (ARM_BC_SD_P | ARM_BC_SD_L) ||
                op->b >= 13u) return 0;
            dest = 1u << op->b;
        } else return 0;
        repeated |= written & dest;
        written |= dest;
    }
    return (progress & ~repeated) != 0u;
}

/* Narrow counted poll: CMP Rn,#0; BLE outside; {LDR/AND}*;
 * SUB Rn,Rn,#positive; B entry. No counter-dependent body, flags, writes,
 * changing load bases, conditional body, wrappers, or PC/SP/LR operands.
 * This is a structural proof only: stable loads and a body fixed point must
 * still be observed in THIS run before any repetition may be omitted. */
static uint8_t arm_jit_counted_poll(arm920t_t *c, arm_jit_block_t *b) {
    if (b->count < 5u || b->poll_prefix != b->count || !b->poll_backedge ||
        (uint64_t)b->tag_pc + (b->count - 1u) * 4u > UINT32_MAX) return 0;
    const arm_jit_op_t *ops = arm_jit_ops(c, b);
    const arm_jit_op_t *cmp = &ops[0], *exit = &ops[1];
    const arm_jit_op_t *sub = &ops[b->count - 2u], *edge = &ops[b->count - 1u];
    unsigned reg = cmp->b;
    if (cmp->kind != ARM_JIT_OP_DATA || cmp->cond != 14u || cmp->a != 10u ||
        reg >= 13u || cmp->c != 0u || cmp->imm != 0u ||
        (cmp->d & (ARM_BC_DATA_IMM | ARM_BC_DATA_S | ARM_BC_DATA_REGSHIFT)) !=
            (ARM_BC_DATA_IMM | ARM_BC_DATA_S) ||
        exit->kind != ARM_JIT_OP_BRANCH || exit->cond != 13u ||
        (exit->insn & (1u << 24)) ||
        sub->kind != ARM_JIT_OP_DATA || sub->cond != 14u || sub->a != 2u ||
        sub->b != reg || sub->c != reg || !sub->imm || sub->imm > INT32_MAX ||
        (sub->d & (ARM_BC_DATA_IMM | ARM_BC_DATA_S | ARM_BC_DATA_REGSHIFT)) != ARM_BC_DATA_IMM ||
        !arm_jit_is_uncond_b_no_link(edge->insn) ||
        arm_jit_branch_target(edge->pc, edge->insn) != b->tag_pc) return 0;
    uint32_t target = arm_jit_branch_target(exit->pc, exit->insn);
    if (target >= b->tag_pc && target <= edge->pc) return 0;
    uint32_t written = 0, bases = 0;
    unsigned loads = 0;
    for (unsigned i = 0; i < b->count; ++i) {
        const arm_jit_op_t *op = &ops[i];
        if (op->reserved || op->pc != b->tag_pc + i * 4u) return 0;
        if (i < 2u || i >= b->count - 2u) continue;
        if (op->cond != 14u) return 0;
        if (op->kind == ARM_JIT_OP_SINGLE_DT) {
            if ((op->d & ~ARM_BC_SD_U) != (ARM_BC_SD_P | ARM_BC_SD_L) ||
                op->a >= 13u || op->b >= 13u || op->a == reg || op->b == reg) return 0;
            bases |= 1u << op->a;
            written |= 1u << op->b;
            ++loads;
        } else if (op->kind == ARM_JIT_OP_DATA) {
            if (op->a != 0u || op->b != op->c || op->c >= 13u || op->c == reg ||
                (op->d & (ARM_BC_DATA_S | ARM_BC_DATA_REGSHIFT))) return 0;
            if (!(op->d & ARM_BC_DATA_IMM) &&
                (op->e >= 13u || op->e == reg || op->g || op->h)) return 0;
            written |= 1u << op->c;
        } else return 0;
    }
    return loads && !(written & bases) ? (uint8_t)(reg + 1u) : 0;
}

static int arm_jit_needs_poll(arm920t_t *c, arm_jit_block_t *b) {
    return c->bus.is_stable_read32 && b->poll_backedge &&
           !arm_jit_poll_has_progress(c, b);
}

/* Keep translation (including the native emitter's large scratch buffer) out
 * of the dispatch loop. Inlining it inflates the hot frame and register spills
 * even when every block lookup hits already compiled code. */
static ARM_NOINLINE arm_jit_block_t *arm_jit_translate(arm920t_t *c, uint32_t pc) {
    if (!arm_jit_alloc(c)) { ARM_PROF_INC(c, jit_translate_failures); return NULL; }
    /* No native block is executing here. Invalidate before binding the slot:
     * generation wrap clears the table, and old code pointers must be stale
     * before reusing the arena. Reserve the emitter's bound plus alignment. */
    if (c->jit_enabled && c->jit_code && c->jit_code_used &&
        c->jit_code_size > ARM_JIT_NATIVE_MAX_BYTES + 15u &&
        (c->jit_code_used > c->jit_code_size ||
         c->jit_code_size - c->jit_code_used < ARM_JIT_NATIVE_MAX_BYTES + 15u))
        arm920t_jit_invalidate_all(c, ARM_JIT_INV_CODE_RECYCLE);
    /* Pick the way to (re)fill. An unused or stale way is free, and a block
     * for this same PC is the code-changed case; only a set whose two ways
     * both hold live blocks for other PCs is a table conflict. */
    uint32_t set = arm_jit_block_index(pc);
    arm_jit_block_t *b = &c->jit_blocks[set];
    if (b->valid && b->generation == c->jit_generation && b->tag_pc != pc) {
        arm_jit_block_t *alt = &c->jit_blocks[set + 1u];
        if (!alt->valid || alt->generation != c->jit_generation || alt->tag_pc == pc)
            b = alt;
#if ARM920T_PROFILING
        else c->prof.jit_block_conflicts++;
#endif
    }
    b->valid = 0;
    b->tag_pc = pc;
    b->tag_cache_epoch = c->jit_cache_epoch;
    b->generation = c->jit_generation;
    b->count = 0;
    b->native_ok = 0;
    b->native = NULL;
    b->deferred_inline_pc = 0;
    uint32_t cur = pc & ~3u;
    for (uint8_t i = 0; i < ARM_JIT_MAX_INSNS; ++i) {
        if (i && ((cur ^ pc) & ~ARM_JIT_PAGE_MASK)) break;
        uint32_t insn = rb32(c, cur);
        arm_jit_op_t *op = &arm_jit_ops(c, b)[i];
        op->pc = cur;
        op->insn = insn;
        op->cond = (uint8_t)(insn >> 28);
        op->kind = (uint8_t)arm_jit_classify(insn);
        op->stop = (uint8_t)arm_jit_may_write_pc(insn);
        op->reserved = (op->kind == ARM_JIT_OP_COPROC && arm_jit_is_local_cp15_op(insn)) ? 6u : 0u;
        arm_bc_decode_op(op);
        b->count = (uint8_t)(i + 1u);

        if (arm_jit_is_uncond_bl(insn) && i + 8u < ARM_JIT_MAX_INSNS) {
            uint32_t tpc = arm_jit_branch_target(cur, insn);
            uint32_t first = 0;
            /* A cold callee may share a TLB slot with firmware. Keep the
             * ordinary call until execution has supplied its mapping, then
             * retry collection without a speculative page-table walk. */
            if (!arm_jit_peek_fetch(c, tpc, &first) && tpc)
                b->deferred_inline_pc = tpc;
            if (arm_jit_is_leaf_push_lr(first)) {
                arm_jit_op_t leaf[32];
                unsigned nleaf = arm_jit_collect_framed(c, tpc, leaf,
                    GP32_ARRAY_COUNT(leaf), 0u);
                if (nleaf && i + nleaf < ARM_JIT_MAX_INSNS) {
                    op->stop = 0;
                    op->reserved = 2u;
                    memcpy(&arm_jit_ops(c, b)[i + 1u], leaf, nleaf * sizeof(leaf[0]));
                    i = (uint8_t)(i + nleaf);
                    b->count = (uint8_t)(i + 1u);
                    cur += 4u;
                    continue;
                }
            } else {
                uint32_t leaf[8];
                uint8_t nleaf = 0;
                int ok = 0;
                for (; nleaf < GP32_ARRAY_COUNT(leaf); ++nleaf) {
                    uint32_t cpc = tpc + (uint32_t)nleaf * 4u;
                    if (((cpc ^ tpc) & ~ARM_JIT_PAGE_MASK) ||
                        !arm_jit_peek_fetch(c, cpc, &leaf[nleaf])) break;
                    if (leaf[nleaf] == 0xe1a0f00eu || leaf[nleaf] == 0xe12fff1eu) {
                        ok = 1; ++nleaf; break; /* unconditional MOV pc,lr / BX lr */
                    }
                    if (!arm_jit_is_unframed_leaf_data(leaf[nleaf])) break;
                }
                if (ok && i + nleaf < ARM_JIT_MAX_INSNS) {
                    op->stop = 0;
                    op->reserved = 2u; /* retain the real BL link and callee PCs */
                    for (uint8_t k = 0; k < nleaf; ++k) {
                        arm_jit_op_t *cop = &arm_jit_ops(c, b)[++i];
                        cop->pc = tpc + (uint32_t)k * 4u;
                        cop->insn = leaf[k];
                        cop->cond = (uint8_t)(leaf[k] >> 28);
                        cop->kind = (uint8_t)arm_jit_classify(leaf[k]);
                        cop->stop = 0;
                        cop->reserved = k + 1u == nleaf ? 7u : 0u;
                        arm_bc_decode_op(cop);
                    }
                    b->count = (uint8_t)(i + 1u);
                    /* Marker 7 keeps the real return instruction. Native
                     * MOV guards its aligned PC; BX uses the checked helper.
                     * Portable execution checks PC/Thumb after every op. */
                    cur += 4u;
                    continue;
                }
            }
        }

        if (i + 1u < ARM_JIT_MAX_INSNS && arm_jit_forward_loop(c, pc, cur, insn)) {
            uint32_t target = arm_jit_branch_target(cur, insn);
            int seen = 0;
            for (uint8_t j = 0; j < i; ++j)
                if (arm_jit_ops(c, b)[j].pc == target) { seen = 1; break; }
            if (!seen) {
                op->reserved = 8u; /* taken trace; failed condition exits at PC+4 */
                cur = target;
                continue;
            }
        }

        if (arm_jit_is_uncond_b_no_link(insn)) {
            uint32_t target = arm_jit_branch_target(cur, insn);
            int seen = 0;
            for (uint8_t j = 0; j < i; ++j) if ((arm_jit_ops(c, b)[j].pc & ~3u) == target) { seen = 1; break; }
            if (!seen && ((target ^ pc) & ~ARM_JIT_PAGE_MASK) == 0u) {
                op->stop = 0;
                cur = target;
                continue;
            }
        }

        if (op->stop || op->kind == ARM_JIT_OP_UNDEFINED || (op->kind == ARM_JIT_OP_COPROC && op->reserved != 6u) || op->kind == ARM_JIT_OP_SWI) break;
        cur += 4u;
    }
    if (!b->count) { ARM_PROF_INC(c, jit_translate_failures); return NULL; }
    /* A polling candidate may only read stable words and change ordinary
     * registers/flags. Word stores must be proven idempotent RAM writes at
     * runtime; exceptions and status writes remain excluded. */
    b->poll_prefix = 0;
    b->poll_backedge = 0;
    for (uint8_t i = 0; i < b->count; ++i) {
        const arm_jit_op_t *op = &arm_jit_ops(c, b)[i];
        if (op->kind == ARM_JIT_OP_DATA) {
            if (op->c == 15u && op->reserved != 7u) break;
        } else if (op->kind == ARM_JIT_OP_SINGLE_DT) {
            if ((op->d & ~(ARM_BC_SD_U | ARM_BC_SD_L)) != ARM_BC_SD_P || op->b == 15u) break;
        } else if (op->kind == ARM_JIT_OP_BRANCH) {
            if ((op->insn & (1u << 24)) && op->reserved != 2u) break;
        } else if (op->kind == ARM_JIT_OP_BLOCK_DT) {
            /* Only the already-classified balanced leaf wrapper. Its push
             * writes the same BL return address on each complete iteration. */
            if (op->reserved != 4u && op->reserved != 5u) break;
        } else break;
        b->poll_prefix = (uint8_t)(i + 1u);
        /* Only a non-link branch back to this trace's entry can close a
         * polling repetition. A data-only prefix or an unrelated branch is
         * not enough to select portable execution. NV never takes an edge. */
        if (op->kind == ARM_JIT_OP_BRANCH && op->cond != 15u &&
            !(op->insn & (1u << 24)) &&
            arm_jit_branch_target(op->pc, op->insn) == b->tag_pc)
            b->poll_backedge = 1;
    }
    b->poll_backedge |= (uint8_t)(arm_jit_counted_poll(c, b) << 1);
    b->valid = 1;
    ARM_PROF_INC(c, jit_blocks_compiled);
    /* Counted candidates first observe real portable reads. Do not allocate
     * unused native code for stable loops; rejected observations may compile
     * once later at dispatch. Other blocks keep their existing publication. */
    if (c->jit_enabled && !(b->poll_backedge >> 1) && !arm_jit_needs_poll(c, b)) {
        arm_jit_compile_native(c, b);
#if ARM920T_PROFILING && ARM920T_NATIVE_BACKEND
        /* Both backends count actual reserve failures at the allocation site. */
        if (b->native_ok) c->prof.jit_native_compiled++;
        else c->prof.jit_native_failed++;
#endif
    }
    return b;
}




ARM_FORCE_INLINE int arm_jit_addr_in_ram(uint32_t addr, size_t bytes) {
    return addr >= ARM_JIT_RAM_BASE_ADDR && bytes <= ARM_JIT_RAM_SIZE_BYTES &&
           (uint64_t)(addr - ARM_JIT_RAM_BASE_ADDR) + bytes <= ARM_JIT_RAM_SIZE_BYTES;
}
ARM_FORCE_INLINE int arm_jit_addr_in_bios(uint32_t addr, size_t bytes) {
    return bytes <= ARM_JIT_BIOS_SIZE_BYTES && (uint64_t)addr + bytes <= ARM_JIT_BIOS_SIZE_BYTES;
}
ARM_FORCE_INLINE int arm_jit_addr_in_identity_io(uint32_t addr, size_t bytes) {
    return addr >= 0x14000000u && bytes <= 0x02000000u &&
           (uint64_t)(addr - 0x14000000u) + bytes <= 0x02000000u;
}

ARM_FORCE_INLINE uint32_t arm_bc_phys_word_addr(arm920t_t *c, uint32_t addr) {
    return (mmu_translate(c, addr & ~3u) & ~3u) | (addr & 3u);
}

ARM_FORCE_INLINE uint32_t arm_bc_ld_word_phys(arm920t_t *c, uint32_t addr) {
    uint32_t a = addr & ~3u;
    uint32_t v;
    if (c->jit_ram_base && arm_jit_addr_in_ram(a, 4u)) v = gp32_ld32le(c->jit_ram_base + (a - ARM_JIT_RAM_BASE_ADDR));
    else if (c->jit_bios_base && arm_jit_addr_in_bios(a, 4u)) v = gp32_ld32le(c->jit_bios_base + a);
    else if (arm_jit_addr_in_identity_io(a, 4u)) v = (c->bus.read32_io ? c->bus.read32_io : c->bus.read32)(c->bus.user, a);
    else {
        /* This address is already physical, including outside direct windows. */
        uint8_t *p = fastmem(c, a, 4u, 0);
        v = p ? gp32_ld32le(p) : c->bus.read32(c->bus.user, a);
    }
    return (addr & 3u) ? gp32_ror32(v, (addr & 3u) * 8u) : v;
}
ARM_FORCE_INLINE uint32_t arm_bc_ld_byte_phys(arm920t_t *c, uint32_t addr) {
    if (c->jit_ram_base && arm_jit_addr_in_ram(addr, 1u)) return c->jit_ram_base[addr - ARM_JIT_RAM_BASE_ADDR];
    if (c->jit_bios_base && arm_jit_addr_in_bios(addr, 1u)) return c->jit_bios_base[addr];
    if (arm_jit_addr_in_identity_io(addr, 1u)) return c->bus.read8(c->bus.user, addr);
    uint8_t *p = fastmem(c, addr, 1u, 0);
    return p ? p[0] : c->bus.read8(c->bus.user, addr);
}
ARM_FORCE_INLINE uint32_t arm_bc_ld_half_phys(arm920t_t *c, uint32_t addr) {
    if (c->jit_ram_base && arm_jit_addr_in_ram(addr, 2u)) return gp32_ld16le(c->jit_ram_base + (addr - ARM_JIT_RAM_BASE_ADDR));
    if (c->jit_bios_base && arm_jit_addr_in_bios(addr, 2u)) return gp32_ld16le(c->jit_bios_base + addr);
    if (arm_jit_addr_in_identity_io(addr, 2u)) return c->bus.read16(c->bus.user, addr);
    /* arm_bc_ld_half already translated the VA. A failed direct-window
     * probe must read this PA, not translate it a second time as a VA. */
    uint8_t *p = fastmem(c, addr, 2u, 0);
    return p ? gp32_ld16le(p) : c->bus.read16(c->bus.user, addr);
}
ARM_FORCE_INLINE void arm_bc_st_word_phys(arm920t_t *c, uint32_t addr, uint32_t v) {
    uint32_t a = addr & ~3u;
    if (c->jit_ram_base && arm_jit_addr_in_ram(a, 4u)) { gp32_st32le(c->jit_ram_base + (a - ARM_JIT_RAM_BASE_ADDR), v); return; }
    if (arm_jit_addr_in_identity_io(a, 4u)) { c->bus.write32(c->bus.user, a, v); return; }
    uint8_t *p = fastmem(c, a, 4u, 1);
    if (p) gp32_st32le(p, v);
    else c->bus.write32(c->bus.user, a, v);
}
ARM_FORCE_INLINE void arm_bc_st_byte_phys(arm920t_t *c, uint32_t addr, uint32_t v) {
    if (c->jit_ram_base && arm_jit_addr_in_ram(addr, 1u)) { c->jit_ram_base[addr - ARM_JIT_RAM_BASE_ADDR] = (uint8_t)v; return; }
    if (arm_jit_addr_in_identity_io(addr, 1u)) { c->bus.write8(c->bus.user, addr, (uint8_t)v); return; }
    uint8_t *p = fastmem(c, addr, 1u, 1);
    if (p) p[0] = (uint8_t)v;
    else c->bus.write8(c->bus.user, addr, (uint8_t)v);
}
ARM_FORCE_INLINE void arm_bc_st_half_phys(arm920t_t *c, uint32_t addr, uint32_t v) {
    if (c->jit_ram_base && arm_jit_addr_in_ram(addr, 2u)) { gp32_st16le(c->jit_ram_base + (addr - ARM_JIT_RAM_BASE_ADDR), (uint16_t)v); return; }
    if (arm_jit_addr_in_identity_io(addr, 2u)) { c->bus.write16(c->bus.user, addr, (uint16_t)v); return; }
    /* addr is physical even when a halfword straddles the RAM window. */
    uint8_t *p = fastmem(c, addr, 2u, 1);
    if (p) gp32_st16le(p, (uint16_t)v);
    else c->bus.write16(c->bus.user, addr, (uint16_t)v);
}
ARM_FORCE_INLINE uint32_t arm_bc_ld_word(arm920t_t *c, uint32_t addr) { return arm_bc_ld_word_phys(c, arm_bc_phys_word_addr(c, addr)); }
ARM_FORCE_INLINE uint32_t arm_bc_ld_byte(arm920t_t *c, uint32_t addr) { return arm_bc_ld_byte_phys(c, mmu_translate(c, addr)); }
ARM_FORCE_INLINE uint32_t arm_bc_ld_half(arm920t_t *c, uint32_t addr) { return arm_bc_ld_half_phys(c, mmu_translate(c, addr)); }
ARM_FORCE_INLINE void arm_bc_st_word(arm920t_t *c, uint32_t addr, uint32_t v) { arm_bc_st_word_phys(c, arm_bc_phys_word_addr(c, addr), v); }
ARM_FORCE_INLINE void arm_bc_st_byte(arm920t_t *c, uint32_t addr, uint32_t v) { arm_bc_st_byte_phys(c, mmu_translate(c, addr), v); }
ARM_FORCE_INLINE void arm_bc_st_half(arm920t_t *c, uint32_t addr, uint32_t v) { arm_bc_st_half_phys(c, mmu_translate(c, addr), v); }

#if defined(ARM_JIT_NATIVE_A64)
/* The single/half RAM emitters have already computed these operands, but a
 * failed RAM guard has not committed anything. Keep the existing VA bus chain
 * (including its MMU/fault behavior) and the classified executor's commit
 * order. Only emitter-proven shapes with Rd != PC and no PC writeback enter.
 * Capture all decoded fields before callbacks can invalidate the op storage. */
ARM_FORCE_INLINE void arm_jit_mem_access(arm920t_t *c, const arm_jit_op_t *op,
                                        uint32_t addr, uint32_t writeback,
                                        uint32_t value) {
    unsigned rn = op->a, rd = op->b, d = op->d;
    int single = op->kind == ARM_JIT_OP_SINGLE_DT;
    int load = !!(d & (single ? ARM_BC_SD_L : ARM_BC_HALF_L));
    int wb = single ? (!(d & ARM_BC_SD_P) || (d & ARM_BC_SD_W)) :
                      (!(d & ARM_BC_HALF_P) || (d & ARM_BC_HALF_W));
    unsigned sh = op->e;
    if (single) {
        if (load) {
            if (d & ARM_BC_SD_B) {
                ARM_PROF_INC(c, helper_ld_byte);
                value = arm_bc_ld_byte(c, addr);
            } else {
                ARM_PROF_INC(c, helper_ld_word);
                value = arm_bc_ld_word(c, addr & ~3u);
                if (addr & 3u) value = gp32_ror32(value, (addr & 3u) * 8u);
            }
        } else if (d & ARM_BC_SD_B) {
            ARM_PROF_INC(c, helper_st_byte);
            arm_bc_st_byte(c, addr, value);
        } else {
            ARM_PROF_INC(c, helper_st_word);
            arm_bc_st_word(c, addr & ~3u, value);
        }
    } else if (load) {
        if (sh == 1u) {
            ARM_PROF_INC(c, helper_ld_half);
            value = arm_bc_ld_half(c, addr);
        } else if (sh == 2u) {
            ARM_PROF_INC(c, helper_ld_sbyte);
            value = (uint32_t)(int32_t)(int8_t)arm_bc_ld_byte(c, addr);
        } else {
            ARM_PROF_INC(c, helper_ld_shalf);
            value = (uint32_t)(int32_t)(int16_t)arm_bc_ld_half(c, addr);
        }
    } else {
        ARM_PROF_INC(c, helper_st_half);
        arm_bc_st_half(c, addr, value);
    }
    if (load) write_r(c, rd, value);
    if (wb) write_r(c, rn, writeback);
}
#endif

ARM_FORCE_INLINE uint32_t arm_bc_shifter_operand(arm920t_t *c, const arm_jit_op_t *op, int *carry_out) {
    if (op->d & ARM_BC_DATA_IMM) {
        if (carry_out) *carry_out = (op->d & ARM_BC_DATA_IMM_CV) ? !!(op->d & ARM_BC_DATA_IMM_C) : !!(c->cpsr & C_FLAG);
        return op->imm;
    }
    uint32_t v = read_r(c, op->e);
    unsigned type = op->g & 3u;
    unsigned amount = (op->d & ARM_BC_DATA_REGSHIFT) ? (read_r(c, op->f) & 0xffu) : op->h;
    int cin = !!(c->cpsr & C_FLAG);
    uint32_t res = v;
    int cout = cin;
    int reg_shift = !!(op->d & ARM_BC_DATA_REGSHIFT);
    switch (type) {
    case 0:
        if (amount == 0) { res = v; cout = cin; }
        else if (amount < 32) { res = v << amount; cout = !!(v & (1u << (32u - amount))); }
        else if (amount == 32) { res = 0; cout = v & 1u; }
        else { res = 0; cout = 0; }
        break;
    case 1:
        if (amount == 0) {
            if (reg_shift) { res = v; cout = cin; }
            else { res = 0; cout = !!(v & N_FLAG); }
        } else if (amount < 32) { res = v >> amount; cout = !!(v & (1u << (amount - 1u))); }
        else if (amount == 32) { res = 0; cout = !!(v & N_FLAG); }
        else { res = 0; cout = 0; }
        break;
    case 2:
        if (amount == 0) {
            if (reg_shift) { res = v; cout = cin; }
            else { res = (v & N_FLAG) ? UINT32_MAX : 0; cout = !!(v & N_FLAG); }
        } else if (amount < 32) { res = (uint32_t)((int32_t)v >> amount); cout = !!(v & (1u << (amount - 1u))); }
        else { res = (v & N_FLAG) ? UINT32_MAX : 0; cout = !!(v & N_FLAG); }
        break;
    default:
        if (amount == 0) {
            if (reg_shift) { res = v; cout = cin; }
            else { res = ((uint32_t)cin << 31) | (v >> 1); cout = v & 1u; }
        } else { unsigned rot = amount & 31u; res = gp32_ror32(v, rot); cout = rot ? !!(res & N_FLAG) : !!(v & N_FLAG); }
        break;
    }
    if (carry_out) *carry_out = cout;
    return res;
}

ARM_FORCE_INLINE void op_data_proc_bc(arm920t_t *c, const arm_jit_op_t *op) {
    unsigned code = op->a, rn = op->b, rd = op->c;
    int sflag = !!(op->d & ARM_BC_DATA_S);
    int sh_c = !!(c->cpsr & C_FLAG);
    uint32_t op2 = arm_bc_shifter_operand(c, op, &sh_c);
    uint32_t a = read_r(c, rn), r = 0;
    int write = 1;
    switch (code) {
    case 0x0: r = a & op2; if (sflag) { set_nz(c, r); set_flag(c, C_FLAG, sh_c); } break;
    case 0x1: r = a ^ op2; if (sflag) { set_nz(c, r); set_flag(c, C_FLAG, sh_c); } break;
    case 0x2: r = a - op2; if (sflag) sub_flags(c, a, op2, r); break;
    case 0x3: r = op2 - a; if (sflag) sub_flags(c, op2, a, r); break;
    case 0x4: r = a + op2; if (sflag) add_flags(c, a, op2, r); break;
    case 0x5: { uint32_t ci = (c->cpsr & C_FLAG) ? 1u : 0u; r = a + op2 + ci; if (sflag) add_carry_flags(c, a, op2, ci, r); break; }
    case 0x6: { uint32_t bi = (c->cpsr & C_FLAG) ? 0u : 1u; r = a - op2 - bi; if (sflag) sub_borrow_flags(c, a, op2, bi, r); } break;
    case 0x7: { uint32_t bi = (c->cpsr & C_FLAG) ? 0u : 1u; r = op2 - a - bi; if (sflag) sub_borrow_flags(c, op2, a, bi, r); } break;
    case 0x8: r = a & op2; set_nz(c, r); set_flag(c, C_FLAG, sh_c); write = 0; break;
    case 0x9: r = a ^ op2; set_nz(c, r); set_flag(c, C_FLAG, sh_c); write = 0; break;
    case 0xa: r = a - op2; sub_flags(c, a, op2, r); write = 0; break;
    case 0xb: r = a + op2; add_flags(c, a, op2, r); write = 0; break;
    case 0xc: r = a | op2; if (sflag) { set_nz(c, r); set_flag(c, C_FLAG, sh_c); } break;
    case 0xd: r = op2; if (sflag) { set_nz(c, r); set_flag(c, C_FLAG, sh_c); } break;
    case 0xe: r = a & ~op2; if (sflag) { set_nz(c, r); set_flag(c, C_FLAG, sh_c); } break;
    case 0xf: r = ~op2; if (sflag) { set_nz(c, r); set_flag(c, C_FLAG, sh_c); } break;
    }
    if (write) {
        if (rd == 15u && sflag) restore_cpsr_from_spsr(c);
        write_r(c, rd, r);
    }
}

ARM_FORCE_INLINE uint32_t arm_bc_addr_mode2_offset(arm920t_t *c, const arm_jit_op_t *op) {
    if (!(op->d & ARM_BC_SD_REGOFF)) return op->imm;
    uint32_t v = read_r(c, op->c);
    unsigned amount = op->f;
    switch (op->e & 3u) {
    case 0: return amount < 32u ? (v << amount) : 0u;
    case 1: return amount ? (v >> amount) : 0u;
    case 2: return amount ? (uint32_t)((int32_t)v >> amount) : ((v & N_FLAG) ? UINT32_MAX : 0u);
    default: return amount ? gp32_ror32(v, amount) : (((c->cpsr & C_FLAG) ? 0x80000000u : 0u) | (v >> 1));
    }
}

ARM_FORCE_INLINE void op_halfword_bc(arm920t_t *c, const arm_jit_op_t *op) {
    unsigned rn = op->a, rd = op->b;
    int p = !!(op->d & ARM_BC_HALF_P), u = !!(op->d & ARM_BC_HALF_U), w = !!(op->d & ARM_BC_HALF_W), l = !!(op->d & ARM_BC_HALF_L);
    uint32_t off = (op->d & ARM_BC_HALF_IMM) ? op->imm : c->r[op->c];
    uint32_t base = read_r(c, rn);
    uint32_t addr = p ? (u ? base + off : base - off) : base;
    if (l) {
        uint32_t result = 0;
        if (op->e == 1u) result = arm_bc_ld_half(c, addr);
        else if (op->e == 2u) result = (uint32_t)(int32_t)(int8_t)arm_bc_ld_byte(c, addr);
        else if (op->e == 3u) result = (uint32_t)(int32_t)(int16_t)arm_bc_ld_half(c, addr);
        write_r(c, rd, result);
    } else {
        if (op->e == 1u) arm_bc_st_half(c, addr, c->r[rd]);
        else arm_bc_st_byte(c, addr, c->r[rd]);
    }
    if (!p || w) write_r(c, rn, u ? base + off : base - off);
}

ARM_FORCE_INLINE void op_single_dt_bc(arm920t_t *c, const arm_jit_op_t *op) {
    unsigned rn = op->a, rd = op->b;
    uint32_t d = op->d;

    /* Portable-interpreter hot path: most GP32 BIOS/game memory traffic is
       immediate pre-indexed LDR/STR without writeback.  Avoid rebuilding the
       addressing-mode decision tree on every bytecode execution. */
    if ((d & (ARM_BC_SD_P | ARM_BC_SD_W | ARM_BC_SD_REGOFF)) == ARM_BC_SD_P) {
        uint32_t base = read_r(c, rn);
        uint32_t addr = (d & ARM_BC_SD_U) ? base + op->imm : base - op->imm;
        if (d & ARM_BC_SD_L) {
            uint32_t v;
            if (d & ARM_BC_SD_B) {
                v = arm_bc_ld_byte(c, addr);
            } else {
                v = arm_bc_ld_word(c, addr & ~3u);
                if (addr & 3u) v = gp32_ror32(v, (addr & 3u) * 8u);
            }
            write_r(c, rd, v);
        } else {
            uint32_t v = (rd == 15u) ? (c->r[15] + 4u) : c->r[rd];
            if (d & ARM_BC_SD_B) arm_bc_st_byte(c, addr, v);
            else arm_bc_st_word(c, addr & ~3u, v);
        }
        return;
    }

    int p = !!(d & ARM_BC_SD_P), u = !!(d & ARM_BC_SD_U), b = !!(d & ARM_BC_SD_B), w = !!(d & ARM_BC_SD_W), l = !!(d & ARM_BC_SD_L);
    uint32_t base = read_r(c, rn), off = arm_bc_addr_mode2_offset(c, op);
    uint32_t addr = p ? (u ? base + off : base - off) : base;
    if (l) {
        uint32_t v = b ? arm_bc_ld_byte(c, addr) : arm_bc_ld_word(c, addr & ~3u);
        if (!b && (addr & 3u)) v = gp32_ror32(v, (addr & 3u) * 8u);
        write_r(c, rd, v);
    } else {
        uint32_t v = (rd == 15u) ? (c->r[15] + 4u) : c->r[rd];
        if (b) arm_bc_st_byte(c, addr, v); else arm_bc_st_word(c, addr & ~3u, v);
    }
    if (!p || w) write_r(c, rn, u ? base + off : base - off);
}


ARM_FORCE_INLINE void op_block_dt_bc(arm920t_t *c, const arm_jit_op_t *op) {
    unsigned rn = op->a;
    uint32_t list = op->imm;
    int p = !!(op->d & 0x10u), u = !!(op->d & 0x08u), s = !!(op->d & 0x04u), w = !!(op->d & 0x02u), l = !!(op->d & 0x01u);
    unsigned count = op->g ? op->g : 16u;
    uint32_t base = read_r(c, rn);
    uint32_t addr;
    if (u) addr = p ? base + 4u : base;
    else addr = p ? base - 4u * count : base - 4u * count + 4u;
    uint32_t final_base = u ? base + 4u * count : base - 4u * count;
    int exception_return = l && s && (list & (1u << 15));
    int user_transfer = s && !exception_return;
    uint32_t loaded_pc = 0u;
    for (uint32_t regs = list; regs; regs &= regs - 1u) {
        unsigned r = arm_ctz16(regs);
        if (l) {
            uint32_t v = arm_bc_ld_word(c, addr);
            if (user_transfer) write_user_r(c, r, v);
            else if (r == 15u) { if (exception_return) loaded_pc = v; else write_r(c, 15u, v); } else c->r[r] = v;
        } else {
            uint32_t v = user_transfer ? read_user_r(c, r) : ((r == 15u) ? c->r[15] + 4u : c->r[r]);
            arm_bc_st_word(c, addr, v);
        }
        addr += 4u;
    }
    if (w && (!l || ((list & (1u << rn)) == 0))) write_r(c, rn, final_base);
    if (exception_return) {
        restore_cpsr_from_spsr(c);
        write_r(c, 15, loaded_pc);
    }
}

ARM_FORCE_INLINE void arm_jit_exec_classified_bc(arm920t_t *c, const arm_jit_op_t *op) {
    const uint32_t pc = op->pc;
    const uint32_t insn = op->insn;
    c->r[15] = pc + 4u;
    if (c->trace && c->log) { char b[96]; snprintf(b, sizeof(b), "AJ %08" PRIx32 " %08" PRIx32 " CPSR=%08" PRIx32, pc, insn, c->cpsr); c->log(c->log_user, b); }
    if (op->cond != 14u && !cond_pass_cpsr(c->cpsr, op->cond)) return;
    switch ((arm_jit_kind_t)op->kind) {
    case ARM_JIT_OP_DATA: op_data_proc_bc(c, op); return;
    case ARM_JIT_OP_PSR: op_psr(c, insn); return;
    case ARM_JIT_OP_MUL: op_mul(c, insn); return;
    case ARM_JIT_OP_SWP: {
        unsigned rn=(insn>>16)&0xfu, rd=(insn>>12)&0xfu, rm=insn&0xfu;
        uint32_t a=c->r[rn]; uint32_t old=GP32_BIT(insn,22)?rb8(c,a):rb32(c,a);
        if(GP32_BIT(insn,22)) wb8(c,a,(uint8_t)c->r[rm]); else wb32(c,a,c->r[rm]);
        c->r[rd]=old;
        return;
    }
    case ARM_JIT_OP_HALF: op_halfword_bc(c, op); return;
    case ARM_JIT_OP_SINGLE_DT: op_single_dt_bc(c, op); return;
    case ARM_JIT_OP_BLOCK_DT: op_block_dt_bc(c, op); return;
    case ARM_JIT_OP_BRANCH: {
        int32_t off = gp32_sign_extend((insn & 0x00ffffffu) << 2, 26);
        if (insn & (1u << 24)) c->r[14] = pc + 4u;
        c->r[15] = (uint32_t)(pc + 8u + off);
        return;
    }
    case ARM_JIT_OP_SWI: {
        uint32_t imm = insn & 0x00ffffffu;
        if (c->swi && c->swi(c->swi_user, c, imm, pc, 0)) return;
        exception_enter(c, MODE_SVC, 0x08, pc + 4u, 0);
        return;
    }
    case ARM_JIT_OP_COPROC: op_coproc(c, insn); return;
    case ARM_JIT_OP_UNDEFINED: exception_enter(c, MODE_UND, 0x04, pc + 4u, 0); return;
    case ARM_JIT_OP_INTERP:
    default:
        c->jit_fallbacks++;
        ARM_PROF_INC(c, jit_fallbacks);
        (void)exec_arm_at(c, pc, insn);
        return;
    }
}

/* Entry reached from generated native code (x64 call-helper op and the AArch64
 * arm_a64_exec_checked slow path) for insns the emitters cannot inline.  The
 * portable dispatcher loop calls arm_jit_exec_classified_bc directly, so
 * helper-call counts and portable-loop counts stay attributable. */
#if ARM920T_PROFILING && defined(ARM_JIT_NATIVE_A64)
/* Read-only TLB probe for the slow-reason classifier: mirrors the valid/tag
 * check of a64_translate/mmu_translate without walking tables or touching
 * CP15 fault registers. */
static uint32_t arm_prof_lookup_phys(const arm920t_t *c, uint32_t va, uint32_t *mask_out) {
    if (!(c->cp15[1] & 1u)) { if (mask_out) *mask_out = 0xffffffffu; return va; }
    const arm_tlb_entry_t *e = &c->tlb_entry[(va >> 12) & 0xfffu];
    if (e->valid && (va & ~e->mask) == e->va_base) {
        if (mask_out) *mask_out = e->mask;
        return e->pa_base | (va & e->mask);
    }
    if (mask_out) *mask_out = 0u;
    return va; /* unknown mapping; callers treat as a TLB miss */
}
static int arm_prof_tlb_hit(const arm920t_t *c, uint32_t va) {
    if (!(c->cp15[1] & 1u)) return 1;
    const arm_tlb_entry_t *e = &c->tlb_entry[(va >> 12) & 0xfffu];
    return e->valid && (va & ~e->mask) == e->va_base;
}
/* Attribute each native slow-op call to a compile-time gate or the first
 * runtime guard it would have failed, replicating the a64_* emitter gates.
 * Runs only in profile builds; pre-op guest state is already committed. */
static void arm_profile_helper_reason(arm920t_t *c, const arm_jit_op_t *op) {
    if (op->cond != 14u && !cond_pass_cpsr(c->cpsr, op->cond)) return;
    switch ((arm_jit_kind_t)op->kind) {
    case ARM_JIT_OP_DATA: {
        if ((op->d & ARM_BC_DATA_REGSHIFT) && (op->e == 15u || op->f == 15u))
            c->prof.slow_gate_data_regshift++;
        else c->prof.slow_bail_other++;
        return;
    }
    case ARM_JIT_OP_MUL: {
        int pc_reg = ((op->insn >> 16) & 0xfu) == 15u || ((op->insn >> 12) & 0xfu) == 15u ||
                     ((op->insn >> 8) & 0xfu) == 15u || (op->insn & 0xfu) == 15u;
        if (pc_reg) c->prof.slow_gate_mul++;
        else c->prof.slow_bail_other++;
        return;
    }
    case ARM_JIT_OP_SINGLE_DT: {
        unsigned d = op->d;
        int wb = !(d & ARM_BC_SD_P) || (d & ARM_BC_SD_W);
        if (!c->jit_ram_base || (wb && op->a == 15u)) { c->prof.slow_gate_single_shape++; return; }
        uint32_t base = (op->a == 15u) ? op->pc + 8u : c->r[op->a];
        uint32_t off = (d & ARM_BC_SD_REGOFF) ? arm_bc_addr_mode2_offset(c, op) : op->imm;
        uint32_t addr = (d & ARM_BC_SD_P) ? ((d & ARM_BC_SD_U) ? base + off : base - off) : base;
        unsigned bytes = (d & ARM_BC_SD_B) ? 1u : 4u;
        if (bytes == 4u) addr &= ~3u;
        if (!arm_prof_tlb_hit(c, addr)) { c->prof.slow_bail_single_tlbmiss++; return; }
        uint32_t phys = arm_prof_lookup_phys(c, addr, NULL);
        /* A64 single transfers currently inline RAM only, including for reads. */
        if (!arm_jit_addr_in_ram(phys, bytes)) {
            c->prof.slow_bail_single_nonram++;
            c->prof.single_nonram_regions[phys >> 24]++;
            unsigned slot;
            for (slot = 0; slot < GP32_CPU_PROFILE_MEMORY_SLOTS; ++slot) {
                gp32_memory_profile_t *entry = &c->prof.single_nonram_addresses[slot];
                if (!(entry->reads | entry->writes)) {
                    entry->physical_address = phys;
                    entry->first_pc = op->pc;
                } else if (entry->physical_address != phys) continue;
                if (d & ARM_BC_SD_L) entry->reads++; else entry->writes++;
                break;
            }
            if (slot == GP32_CPU_PROFILE_MEMORY_SLOTS) c->prof.single_nonram_address_overflow++;
        } else c->prof.slow_bail_single_other++;
        return;
    }
    case ARM_JIT_OP_BLOCK_DT: {
        unsigned d = op->d, rn = op->a;
        uint32_t list = op->imm;
        int load = !!(d & 0x01u), p = !!(d & 0x10u), u = !!(d & 0x08u);
        int pc_load = load && (list & 0x8000u);
        if ((op->reserved && op->reserved != 3u && op->reserved != 4u && op->reserved != 5u) ||
            (d & 0x04u) || !list || rn == 15u || !op->g || !c->jit_ram_base)
            { c->prof.slow_gate_block_shape++; return; }
        unsigned count = op->g;
        uint32_t base = c->r[rn];
        uint32_t addr = u ? (p ? base + 4u : base)
                          : base - 4u * count + (p ? 0u : 4u);
        uint32_t last = addr + 4u * count - 4u;
        if (addr & 3u) { c->prof.slow_bail_block_unaligned++; return; }
        int split = !!((addr ^ last) & ~0xfffu);
        unsigned first_bytes = split ? 0x1000u - (addr & 0xfffu) : 4u * count;
        if (!arm_prof_tlb_hit(c, addr)) { c->prof.slow_bail_block_tlbmiss++; return; }
        uint32_t mask;
        uint32_t phys = arm_prof_lookup_phys(c, addr, &mask);
        if (mask < 4095u) { c->prof.slow_bail_block_tinypage++; return; }
        uint32_t phys1 = 0u;
        if (split) {
            uint32_t next_page = addr + first_bytes;
            if (!arm_prof_tlb_hit(c, next_page)) { c->prof.slow_bail_block_tlbmiss++; return; }
            phys1 = arm_prof_lookup_phys(c, next_page, &mask);
            if (mask < 4095u) { c->prof.slow_bail_block_tinypage++; return; }
        }
        if (!arm_jit_addr_in_ram(phys, first_bytes) ||
            (split && !arm_jit_addr_in_ram(phys1, 4u * count - first_bytes)))
            { c->prof.slow_bail_block_nonram++; return; }
        if (pc_load && c->jit_ram_base) {
            uint32_t pc_phys = split ? phys1 + 4u * count - first_bytes - 4u
                                     : phys + 4u * (count - 1u);
            uint32_t pcw = gp32_ld32le(c->jit_ram_base + (pc_phys - ARM_JIT_RAM_BASE_ADDR));
            if (pcw & 1u) { c->prof.slow_bail_block_pcodd++; return; }
        }
        c->prof.slow_bail_block_other++;
        return;
    }
    default:
        c->prof.slow_bail_other++;
        return;
    }
}
#endif
static inline void arm_profile_helper_op(arm920t_t *c, const arm_jit_op_t *op) {
    ARM_PROF_INC(c, helper_interp_ops);
#if ARM920T_PROFILING
    c->prof.helper_op_kinds[op->kind < GP32_CPU_PROFILE_OP_KINDS ? op->kind : 0u]++;
#if defined(ARM_JIT_NATIVE_A64)
    arm_profile_helper_reason(c, op);
#endif
#else
    GP32_UNUSED(c);
    GP32_UNUSED(op);
#endif
}
ARM_FORCE_INLINE void arm_jit_exec_classified(arm920t_t *c, const arm_jit_op_t *op) {
    arm_profile_helper_op(c, op);
    arm_jit_exec_classified_bc(c, op);
}



static void arm_jit_write_pc_x_helper(arm920t_t *c, uint32_t v) { ARM_PROF_INC(c, helper_write_pc); write_pc_x(c, v); }

/* Native ALU and flags are already committed. USER/SYS have no SPSR, so
 * their ALU flags survive; exception modes restore the shared banked state.
 * Align only after restoring CPSR, using its ARM/Thumb state. */
static void arm_jit_exception_return_helper(arm920t_t *c, uint32_t result) {
    ARM_PROF_INC(c, helper_write_pc);
    restore_cpsr_from_spsr(c);
    write_r(c, 15u, result);
}

#if defined(__x86_64__) || defined(_M_X64)
/* Read-only status access has no bus callback or active-mode side effect.
 * The x64 emitter can consume the value without a checked interpreter call. */
static uint32_t arm_x64_read_spsr(arm920t_t *c) {
    ARM_PROF_INC(c, helper_interp_ops);
    ARM_PROF_INC(c, helper_op_kinds[ARM_JIT_OP_PSR]);
    uint32_t *saved = spsr_ptr(c, mode(c));
    return saved ? *saved : c->cpsr;
}

#ifndef ARM_JIT_CODE_SIZE
#define ARM_JIT_CODE_SIZE (64u * 1024u * 1024u)
#endif

enum x64_reg32 {
    X64_EAX = 0, X64_ECX = 1, X64_EDX = 2, X64_EBX = 3,
    X64_ESP = 4, X64_EBP = 5, X64_ESI = 6, X64_EDI = 7,
    X64_R8D = 8, X64_R9D = 9, X64_R10D = 10, X64_R11D = 11,
    X64_R12D = 12, X64_R13D = 13, X64_R14D = 14, X64_R15D = 15
};

typedef struct x64_emit {
    uint8_t *b;
    size_t cap;
    size_t pos;
    int fail, mmu, ram_read, ram_write;
    uint32_t expected_next, generation, done, guest_pc;
} x64_emit_t;

static int arm_x64_exec_checked(arm920t_t *c, const arm_jit_op_t *op,
                                uint32_t expected_next, uint32_t generation) {
    int stop = op->stop;
    uint32_t status = c->cpsr & 0xffu;
    uint32_t epoch = c->jit_cache_epoch, run_limit = c->run_limit;
    uint8_t *ram_base = c->jit_ram_base, *bios_base = c->jit_bios_base;
    arm_jit_exec_classified(c, op);
    return !stop && c->run_limit == run_limit && !c->halted && !thumb(c) && !c->trace && c->jit_enabled &&
           c->jit_generation == generation && c->jit_cache_epoch == epoch &&
           c->jit_ram_base == ram_base &&
           c->jit_bios_base == bios_base && (c->cpsr & 0xffu) == status &&
           (c->r[15] & ~3u) == expected_next &&
           !(c->irq_line && !(c->cpsr & I_FLAG)) &&
           !(c->fiq_line && !(c->cpsr & F_FLAG));
}

static void x64_u8(x64_emit_t *e, uint8_t v) { if (e->pos < e->cap) e->b[e->pos++] = v; else e->fail = 1; }
static void x64_u32(x64_emit_t *e, uint32_t v) { for (unsigned i = 0; i < 4; ++i) x64_u8(e, (uint8_t)(v >> (i * 8u))); }
static void x64_u64(x64_emit_t *e, uint64_t v) { for (unsigned i = 0; i < 8; ++i) x64_u8(e, (uint8_t)(v >> (i * 8u))); }
static void x64_rex(x64_emit_t *e, int w, int r, int x, int b) {
    uint8_t rex = (uint8_t)(0x40u | (w ? 8u : 0u) | ((r & 8) ? 4u : 0u) | ((x & 8) ? 2u : 0u) | ((b & 8) ? 1u : 0u));
    if (rex != 0x40u) x64_u8(e, rex);
}
static void x64_modrm(x64_emit_t *e, int mod, int reg, int rm) { x64_u8(e, (uint8_t)(((mod & 3) << 6) | ((reg & 7) << 3) | (rm & 7))); }
static uint32_t arm_reg_off(unsigned r) { return (uint32_t)offsetof(arm920t_t, r) + r * 4u; }
static uint32_t cpsr_off(void) { return (uint32_t)offsetof(arm920t_t, cpsr); }
static uint32_t jit_ram_base_off(void) { return (uint32_t)offsetof(arm920t_t, jit_ram_base); }
static uint32_t jit_bios_base_off(void) { return (uint32_t)offsetof(arm920t_t, jit_bios_base); }

static void x64_mov_r32_imm(x64_emit_t *e, int dst, uint32_t imm) { x64_rex(e, 0, 0, 0, dst); x64_u8(e, (uint8_t)(0xb8u + (dst & 7))); x64_u32(e, imm); }
static void x64_mov_r64_imm(x64_emit_t *e, int dst, uint64_t imm) { x64_rex(e, 1, 0, 0, dst); x64_u8(e, (uint8_t)(0xb8u + (dst & 7))); x64_u64(e, imm); }
static void x64_mov_r64_r64(x64_emit_t *e, int dst, int src) { x64_rex(e, 1, src, 0, dst); x64_u8(e, 0x89); x64_modrm(e, 3, src, dst); }
static void x64_mov_r32_r32(x64_emit_t *e, int dst, int src) { x64_rex(e, 0, src, 0, dst); x64_u8(e, 0x89); x64_modrm(e, 3, src, dst); }
static void x64_mov_r32_mem_cpu(x64_emit_t *e, int dst, uint32_t off) { x64_rex(e, 0, dst, 0, X64_EBX); x64_u8(e, 0x8b); x64_modrm(e, 2, dst, X64_EBX); x64_u32(e, off); }
static void x64_mov_mem_cpu_r32(x64_emit_t *e, uint32_t off, int src) { x64_rex(e, 0, src, 0, X64_EBX); x64_u8(e, 0x89); x64_modrm(e, 2, src, X64_EBX); x64_u32(e, off); }
static void x64_mov_mem_cpu_imm(x64_emit_t *e, uint32_t off, uint32_t imm) { x64_u8(e, 0xc7); x64_modrm(e, 2, 0, X64_EBX); x64_u32(e, off); x64_u32(e, imm); }
static void x64_mov_r64_mem_cpu(x64_emit_t *e, int dst, uint32_t off) { x64_rex(e, 1, dst, 0, X64_EBX); x64_u8(e, 0x8b); x64_modrm(e, 2, dst, X64_EBX); x64_u32(e, off); }
static void x64_sib(x64_emit_t *e, int scale, int index, int base) { x64_u8(e, (uint8_t)(((scale & 3) << 6) | ((index & 7) << 3) | (base & 7))); }
static void x64_mov_r32_membase_index(x64_emit_t *e, int dst, int base, int index) { x64_rex(e, 0, dst, index, base); x64_u8(e, 0x8b); x64_modrm(e, 0, dst, X64_ESP); x64_sib(e, 0, index, base); }
static void x64_mov_membase_index_r32(x64_emit_t *e, int base, int index, int src) { x64_rex(e, 0, src, index, base); x64_u8(e, 0x89); x64_modrm(e, 0, src, X64_ESP); x64_sib(e, 0, index, base); }
static void x64_movzx_r32_membase_index8(x64_emit_t *e, int dst, int base, int index) { x64_rex(e, 0, dst, index, base); x64_u8(e, 0x0f); x64_u8(e, 0xb6); x64_modrm(e, 0, dst, X64_ESP); x64_sib(e, 0, index, base); }
static void x64_mov_membase_index8_r32(x64_emit_t *e, int base, int index, int src) { x64_rex(e, 0, src, index, base); x64_u8(e, 0x88); x64_modrm(e, 0, src, X64_ESP); x64_sib(e, 0, index, base); }
static void x64_movzx_r32_membase_index16(x64_emit_t *e, int dst, int base, int index) { x64_rex(e, 0, dst, index, base); x64_u8(e, 0x0f); x64_u8(e, 0xb7); x64_modrm(e, 0, dst, X64_ESP); x64_sib(e, 0, index, base); }
static void x64_movsx_r32_membase_index8(x64_emit_t *e, int dst, int base, int index) { x64_rex(e, 0, dst, index, base); x64_u8(e, 0x0f); x64_u8(e, 0xbe); x64_modrm(e, 0, dst, X64_ESP); x64_sib(e, 0, index, base); }
static void x64_movsx_r32_membase_index16(x64_emit_t *e, int dst, int base, int index) { x64_rex(e, 0, dst, index, base); x64_u8(e, 0x0f); x64_u8(e, 0xbf); x64_modrm(e, 0, dst, X64_ESP); x64_sib(e, 0, index, base); }
static void x64_mov_membase_index16_r32(x64_emit_t *e, int base, int index, int src) { x64_u8(e, 0x66); x64_rex(e, 0, src, index, base); x64_u8(e, 0x89); x64_modrm(e, 0, src, X64_ESP); x64_sib(e, 0, index, base); }
static void x64_test_r32_imm(x64_emit_t *e, int reg, uint32_t imm) { x64_rex(e, 0, 0, 0, reg); x64_u8(e, 0xf7); x64_modrm(e, 3, 0, reg); x64_u32(e, imm); }
static void x64_alu_r32_r32(x64_emit_t *e, uint8_t op, int dst, int src) { x64_rex(e, 0, src, 0, dst); x64_u8(e, op); x64_modrm(e, 3, src, dst); }
static void x64_alu_r32_imm(x64_emit_t *e, unsigned alu, int dst, uint32_t imm) { x64_rex(e, 0, 0, 0, dst); if (imm <= 0x7fu || imm >= 0xffffff80u) { x64_u8(e, 0x83); x64_modrm(e, 3, (int)alu, dst); x64_u8(e, (uint8_t)imm); } else { x64_u8(e, 0x81); x64_modrm(e, 3, (int)alu, dst); x64_u32(e, imm); } }
static void x64_shift_r32_imm(x64_emit_t *e, unsigned subop, int dst, unsigned imm) { x64_rex(e, 0, 0, 0, dst); x64_u8(e, 0xc1); x64_modrm(e, 3, (int)subop, dst); x64_u8(e, (uint8_t)imm); }
static void x64_shift_r32_cl(x64_emit_t *e, unsigned subop, int dst) { x64_rex(e, 0, 0, 0, dst); x64_u8(e, 0xd3); x64_modrm(e, 3, (int)subop, dst); }
static void x64_not_r32(x64_emit_t *e, int dst) { x64_rex(e, 0, 0, 0, dst); x64_u8(e, 0xf7); x64_modrm(e, 3, 2, dst); }
static void x64_imul_r32_r32(x64_emit_t *e, int dst, int src) { x64_rex(e, 0, dst, 0, src); x64_u8(e, 0x0f); x64_u8(e, 0xaf); x64_modrm(e, 3, dst, src); }
static void x64_movzx_ecx_ah(x64_emit_t *e) { x64_u8(e, 0x0f); x64_u8(e, 0xb6); x64_u8(e, 0xcc); }
static void x64_movzx_esi_dl(x64_emit_t *e) { x64_u8(e, 0x0f); x64_u8(e, 0xb6); x64_u8(e, 0xf2); }
static void x64_call_abs(x64_emit_t *e, uintptr_t fn) {
#if defined(_WIN32)
    /* Win64 ABI: every nested call needs 32 bytes of home/shadow space.
       The generated block prologue keeps RSP 16-byte aligned before calls,
       so subtracting 32 preserves the required alignment. */
    x64_u8(e, 0x48); x64_u8(e, 0x83); x64_u8(e, 0xec); x64_u8(e, 0x20);
#endif
    x64_mov_r64_imm(e, X64_R11D, (uint64_t)fn);
    x64_rex(e, 0, 0, 0, X64_R11D);
    x64_u8(e, 0xff);
    x64_modrm(e, 3, 2, X64_R11D);
#if defined(_WIN32)
    x64_u8(e, 0x48); x64_u8(e, 0x83); x64_u8(e, 0xc4); x64_u8(e, 0x20);
#endif
}

#if defined(_WIN32)
#define X64_HOST_ARG0 X64_ECX
#define X64_HOST_ARG1 X64_EDX
#define X64_HOST_ARG2 X64_R8D
#define X64_HOST_ARG3 X64_R9D
#define X64_NATIVE_ARG0 X64_ECX
#define X64_NATIVE_ARG1 X64_EDX
#else
#define X64_HOST_ARG0 X64_EDI
#define X64_HOST_ARG1 X64_ESI
#define X64_HOST_ARG2 X64_EDX
#define X64_HOST_ARG3 X64_ECX
#define X64_NATIVE_ARG0 X64_EDI
#define X64_NATIVE_ARG1 X64_ESI
#endif

static void x64_emit_arg0_cpu(x64_emit_t *e) { x64_mov_r64_r64(e, X64_HOST_ARG0, X64_EBX); }
static void x64_emit_arg1_ptr_imm(x64_emit_t *e, uintptr_t ptr) { x64_mov_r64_imm(e, X64_HOST_ARG1, (uint64_t)ptr); }
static size_t x64_jcc32(x64_emit_t *e, uint8_t cc) { x64_u8(e, 0x0f); x64_u8(e, (uint8_t)(0x80u | (cc & 15u))); size_t at = e->pos; x64_u32(e, 0); return at; }
static size_t x64_jmp32(x64_emit_t *e) { x64_u8(e, 0xe9); size_t at = e->pos; x64_u32(e, 0); return at; }
static void x64_patch32(x64_emit_t *e, size_t at, size_t target) {
    int64_t rel = (int64_t)target - (int64_t)(at + 4u);
    if (at + 4u <= e->cap && rel >= INT32_MIN && rel <= INT32_MAX) {
        uint32_t v = (uint32_t)(int32_t)rel;
        for (unsigned i = 0; i < 4; ++i) e->b[at + i] = (uint8_t)(v >> (i * 8u));
    } else e->fail = 1;
}

static void x64_emit_return_imm(x64_emit_t *e, uint32_t done) {
    x64_mov_r32_imm(e, X64_EAX, done);
#if defined(_WIN32)
    x64_u8(e, 0x5f);                   /* pop rdi */
    x64_u8(e, 0x5e);                   /* pop rsi */
#endif
    x64_u8(e, 0x41); x64_u8(e, 0x5f); /* pop r15 */
    x64_u8(e, 0x41); x64_u8(e, 0x5e); /* pop r14 */
    x64_u8(e, 0x5b);                   /* pop rbx */
    x64_u8(e, 0xc3);                   /* ret */
}

static void x64_emit_call_helper_op(x64_emit_t *e, const arm_jit_op_t *op) {
    /* Helpers execute the interpreter semantic path, which expects r15 to
       contain the normal ARM pipeline-visible next PC.  Directly emitted
       instructions do not need this per-instruction store. */
    x64_mov_mem_cpu_imm(e, arm_reg_off(15), op->pc + 4u);
    x64_emit_arg0_cpu(e);
    x64_emit_arg1_ptr_imm(e, (uintptr_t)op);
    if (op->stop || op->kind == ARM_JIT_OP_SWI || op->kind == ARM_JIT_OP_UNDEFINED ||
        (op->kind == ARM_JIT_OP_COPROC && op->reserved != 6u)) {
        /* The dispatcher rechecks the committed state after every terminal
         * helper. No native successor can consume a stale mapping/budget. */
        x64_call_abs(e, (uintptr_t)arm_jit_exec_classified);
        x64_emit_return_imm(e, e->done);
        return;
    }
    x64_mov_r32_imm(e, X64_HOST_ARG2, e->expected_next);
    x64_mov_r32_imm(e, X64_HOST_ARG3, e->generation);
    x64_call_abs(e, (uintptr_t)arm_x64_exec_checked);
    x64_alu_r32_r32(e, 0x85, X64_EAX, X64_EAX);
    size_t next = x64_jcc32(e, 0x5);
    x64_emit_return_imm(e, e->done);
    x64_patch32(e, next, e->pos);
}

static void x64_emit_nzcv_from_x86_flags(x64_emit_t *e, int invert_carry) {
    x64_u8(e, 0x0f); x64_u8(e, 0x90); x64_u8(e, 0xc2); /* seto dl */
    x64_u8(e, 0x9f);                                   /* lahf */
    x64_movzx_ecx_ah(e);                                /* ecx = AH */
    x64_mov_r32_mem_cpu(e, X64_EAX, cpsr_off());
    x64_alu_r32_imm(e, 4, X64_EAX, ~(N_FLAG | Z_FLAG | C_FLAG | V_FLAG));
    x64_mov_r32_r32(e, X64_ESI, X64_ECX);
    x64_alu_r32_imm(e, 4, X64_ESI, 0x000000c0u);
    x64_shift_r32_imm(e, 4, X64_ESI, 24);              /* SF/ZF -> N/Z */
    x64_alu_r32_r32(e, 0x09, X64_EAX, X64_ESI);        /* or eax, esi */
    x64_mov_r32_r32(e, X64_ESI, X64_ECX);
    if (invert_carry) x64_not_r32(e, X64_ESI);
    x64_alu_r32_imm(e, 4, X64_ESI, 1u);
    x64_shift_r32_imm(e, 4, X64_ESI, 29);              /* CF or !CF -> C */
    x64_alu_r32_r32(e, 0x09, X64_EAX, X64_ESI);
    x64_movzx_esi_dl(e);
    x64_shift_r32_imm(e, 4, X64_ESI, 28);              /* OF -> V */
    x64_alu_r32_r32(e, 0x09, X64_EAX, X64_ESI);
    x64_mov_mem_cpu_r32(e, cpsr_off(), X64_EAX);
}

static void x64_emit_set_nz(x64_emit_t *e, int value_reg, int shifter_carry) {
    /* R8D holds the ARM shifter carry at bit 29 when requested. Capture
     * N/Z from the logical result; preserve V and all control bits. */
    x64_alu_r32_r32(e, 0x85, value_reg, value_reg); /* test r/m, r */
    x64_u8(e, 0x9f);
    x64_movzx_ecx_ah(e);
    x64_mov_r32_mem_cpu(e, X64_EAX, cpsr_off());
    x64_alu_r32_imm(e, 4, X64_EAX, ~(N_FLAG | Z_FLAG | (shifter_carry ? C_FLAG : 0u)));
    x64_mov_r32_r32(e, X64_ESI, X64_ECX);
    x64_alu_r32_imm(e, 4, X64_ESI, 0x000000c0u);
    x64_shift_r32_imm(e, 4, X64_ESI, 24);
    x64_alu_r32_r32(e, 0x09, X64_EAX, X64_ESI);
    if (shifter_carry) x64_alu_r32_r32(e, 0x09, X64_EAX, X64_R8D);
    x64_mov_mem_cpu_r32(e, cpsr_off(), X64_EAX);
}

static void x64_emit_load_arm_reg(x64_emit_t *e, int dst, unsigned r, uint32_t pc) {
    if (r == 15u) x64_mov_r32_imm(e, dst, pc + 8u);
    else x64_mov_r32_mem_cpu(e, dst, arm_reg_off(r));
}
static void x64_emit_store_arm_reg(x64_emit_t *e, unsigned r, int src) { x64_mov_mem_cpu_r32(e, arm_reg_off(r), src); }

static int x64_emit_op2_to_ecx(x64_emit_t *e, uint32_t insn, uint32_t pc, int carry) {
    if (insn & (1u << 25)) {
        uint32_t imm = insn & 0xffu;
        unsigned rot = ((insn >> 8) & 0xfu) * 2u;
        x64_mov_r32_imm(e, X64_ECX, gp32_ror32(imm, rot));
        return 1;
    }
    unsigned rm = insn & 0xfu;
    unsigned type = (insn >> 5) & 3u;
    if (insn & (1u << 4)) {
        if (carry) return 0; /* logical S register-specified shifts use the helper */
        unsigned rs = (insn >> 8) & 0xfu;
        size_t done, zero, small, signfill;
        if (rm == 15u || rs == 15u) return 0;
        x64_emit_load_arm_reg(e, X64_EAX, rm, pc);
        x64_emit_load_arm_reg(e, X64_ECX, rs, pc);
        x64_alu_r32_imm(e, 4, X64_ECX, 0xffu);
        switch (type) {
        case 0:
            x64_alu_r32_imm(e, 7, X64_ECX, 32u);
            zero = x64_jcc32(e, 0x3);
            x64_alu_r32_r32(e, 0x85, X64_ECX, X64_ECX);
            done = x64_jcc32(e, 0x4);
            x64_shift_r32_cl(e, 4, X64_EAX);
            small = x64_jmp32(e);
            x64_patch32(e, zero, e->pos);
            x64_mov_r32_imm(e, X64_EAX, 0);
            x64_patch32(e, done, e->pos);
            x64_patch32(e, small, e->pos);
            break;
        case 1:
            x64_alu_r32_imm(e, 7, X64_ECX, 32u);
            zero = x64_jcc32(e, 0x3);
            x64_alu_r32_r32(e, 0x85, X64_ECX, X64_ECX);
            done = x64_jcc32(e, 0x4);
            x64_shift_r32_cl(e, 5, X64_EAX);
            small = x64_jmp32(e);
            x64_patch32(e, zero, e->pos);
            x64_mov_r32_imm(e, X64_EAX, 0);
            x64_patch32(e, done, e->pos);
            x64_patch32(e, small, e->pos);
            break;
        case 2:
            x64_alu_r32_imm(e, 7, X64_ECX, 32u);
            signfill = x64_jcc32(e, 0x3);
            x64_alu_r32_r32(e, 0x85, X64_ECX, X64_ECX);
            done = x64_jcc32(e, 0x4);
            x64_shift_r32_cl(e, 7, X64_EAX);
            small = x64_jmp32(e);
            x64_patch32(e, signfill, e->pos);
            x64_shift_r32_imm(e, 7, X64_EAX, 31);
            x64_patch32(e, done, e->pos);
            x64_patch32(e, small, e->pos);
            break;
        default:
            x64_alu_r32_imm(e, 4, X64_ECX, 31u);
            x64_alu_r32_r32(e, 0x85, X64_ECX, X64_ECX);
            done = x64_jcc32(e, 0x4);
            x64_shift_r32_cl(e, 1, X64_EAX);
            x64_patch32(e, done, e->pos);
            break;
        }
        x64_mov_r32_r32(e, X64_ECX, X64_EAX);
        return 1;
    }
    unsigned amount = (insn >> 7) & 0x1fu;
    x64_emit_load_arm_reg(e, X64_ECX, rm, pc);
    if (carry) {
        if (type == 0u && !amount) {
            x64_mov_r32_mem_cpu(e, X64_R8D, cpsr_off());
            x64_alu_r32_imm(e, 4, X64_R8D, C_FLAG);
        } else {
            /* Extract before shifting: host shifts mask #32 to zero, and
             * subsequent logical instructions destroy host CF. */
            unsigned bit = type == 0u ? 32u - amount :
                           amount ? amount - 1u : type == 3u ? 0u : 31u;
            x64_mov_r32_r32(e, X64_R8D, X64_ECX);
            if (bit) x64_shift_r32_imm(e, 5, X64_R8D, bit);
            x64_alu_r32_imm(e, 4, X64_R8D, 1u);
            x64_shift_r32_imm(e, 4, X64_R8D, 29);
        }
    }
    if (amount == 0u) {
        if (type == 0u) return 1;
        if (type == 1u) { x64_mov_r32_imm(e, X64_ECX, 0); return 1; }
        if (type == 2u) { x64_shift_r32_imm(e, 7, X64_ECX, 31); return 1; }
        if (!carry) return 0; /* non-S RRX retains the existing helper */
        x64_mov_r32_mem_cpu(e, X64_EDX, cpsr_off());
        x64_alu_r32_imm(e, 4, X64_EDX, C_FLAG);
        x64_shift_r32_imm(e, 4, X64_EDX, 2); /* old C -> bit 31 */
        x64_shift_r32_imm(e, 5, X64_ECX, 1);
        x64_alu_r32_r32(e, 0x09, X64_ECX, X64_EDX);
        return 1;
    }
    switch (type) {
    case 0: x64_shift_r32_imm(e, 4, X64_ECX, amount); return 1; /* SHL */
    case 1: x64_shift_r32_imm(e, 5, X64_ECX, amount); return 1; /* SHR */
    case 2: x64_shift_r32_imm(e, 7, X64_ECX, amount); return 1; /* SAR */
    default: x64_shift_r32_imm(e, 1, X64_ECX, amount); return 1; /* ROR */
    }
}

static void x64_emit_return_pc_reg(x64_emit_t *e, int src, uint32_t done) {
    x64_alu_r32_imm(e, 4, src, ~3u);
    x64_mov_mem_cpu_r32(e, arm_reg_off(15), src);
    x64_emit_return_imm(e, done);
}

static void x64_emit_data_result(x64_emit_t *e, unsigned opc, unsigned s,
                                 unsigned rd, uint32_t done, int shifter_carry) {
    /* Flag emitters clobber EAX/ECX/EDX/ESI. Preserve the unaligned PC
     * result in R10D until the helper arguments are set up. */
    if (rd == 15u) x64_mov_r32_r32(e, X64_R10D, X64_EAX);
    else x64_emit_store_arm_reg(e, rd, X64_EAX);
    if (s && (opc == 0x2u || opc == 0x4u))
        x64_emit_nzcv_from_x86_flags(e, opc == 0x2u);
    else if (s)
        x64_emit_set_nz(e, X64_EAX, shifter_carry);
    if (rd == 15u) {
        x64_emit_arg0_cpu(e);
        x64_mov_r32_r32(e, X64_HOST_ARG1, X64_R10D);
        x64_call_abs(e, (uintptr_t)arm_jit_exception_return_helper);
        x64_emit_return_imm(e, done);
    }
}

static int x64_emit_data_proc(x64_emit_t *e, const arm_jit_op_t *op, uint32_t done) {
    const uint32_t insn = op->insn;
    unsigned opc = (insn >> 21) & 0xfu;
    unsigned s = GP32_BIT(insn, 20), rn = (insn >> 16) & 0xfu, rd = (insn >> 12) & 0xfu;
    int logical_s = s && (opc < 2u || opc == 8u || opc == 9u || opc >= 12u);
    if (rd == 15u) {
        /* Common ARM return sequence: MOV pc,lr / MOV pc,Rm.  Keep the exact
           write_r(pc) alignment semantics and return immediately after the
           control-flow change. Flag-setting writes reuse the supported ALU
           and flag paths below before the dedicated SPSR/PC helper. */
        if (!s && opc == 0xdu && !(insn & (1u << 25)) && !(insn & (1u << 4)) && (((insn >> 5) & 3u) == 0u) && (((insn >> 7) & 0x1fu) == 0u)) {
            unsigned rm = insn & 0xfu;
            x64_emit_load_arm_reg(e, X64_EAX, rm, op->pc);
            if (op->reserved == 7u) {
                x64_alu_r32_imm(e, 4, X64_EAX, ~3u);
                x64_mov_mem_cpu_r32(e, arm_reg_off(15), X64_EAX);
                x64_alu_r32_imm(e, 7, X64_EAX, e->expected_next);
                size_t matched = x64_jcc32(e, 0x4); /* EQ */
                x64_emit_return_imm(e, done);
                x64_patch32(e, matched, e->pos);
            } else x64_emit_return_pc_reg(e, X64_EAX, done);
            return 1;
        }
        if (!s || (opc >= 0x8u && opc <= 0xbu)) return 0;
    }
    if (s && !logical_s && !(opc == 2u || opc == 4u || opc == 10u || opc == 11u)) return 0;
    if (logical_s && !(insn & (1u << 25)) && (insn & (1u << 4))) return 0;
    if (insn & (1u << 25)) {
        uint32_t imm8 = insn & 0xffu;
        unsigned rot = ((insn >> 8) & 0xfu) * 2u;
        uint32_t imm = gp32_ror32(imm8, rot);
        if (logical_s) {
            if (rot) x64_mov_r32_imm(e, X64_R8D, (imm >> 31) << 29);
            else {
                x64_mov_r32_mem_cpu(e, X64_R8D, cpsr_off());
                x64_alu_r32_imm(e, 4, X64_R8D, C_FLAG);
            }
        }
        switch (opc) {
        case 0x0: x64_emit_load_arm_reg(e, X64_EAX, rn, op->pc); x64_alu_r32_imm(e, 4, X64_EAX, imm); break;
        case 0x1: x64_emit_load_arm_reg(e, X64_EAX, rn, op->pc); x64_alu_r32_imm(e, 6, X64_EAX, imm); break;
        case 0x2: x64_emit_load_arm_reg(e, X64_EAX, rn, op->pc); x64_alu_r32_imm(e, 5, X64_EAX, imm); break;
        case 0x3: x64_mov_r32_imm(e, X64_EAX, imm); x64_emit_load_arm_reg(e, X64_ECX, rn, op->pc); x64_alu_r32_r32(e, 0x29, X64_EAX, X64_ECX); break;
        case 0x4: x64_emit_load_arm_reg(e, X64_EAX, rn, op->pc); x64_alu_r32_imm(e, 0, X64_EAX, imm); break;
        case 0x5:
            if (s) return 0;
            x64_emit_load_arm_reg(e, X64_EAX, rn, op->pc);
            x64_mov_r32_mem_cpu(e, X64_EDX, cpsr_off());
            x64_shift_r32_imm(e, 5, X64_EDX, 29);
            x64_alu_r32_imm(e, 4, X64_EDX, 1u);
            x64_alu_r32_imm(e, 0, X64_EAX, imm);
            x64_alu_r32_r32(e, 0x01, X64_EAX, X64_EDX);
            break;
        case 0x6:
            if (s) return 0;
            x64_emit_load_arm_reg(e, X64_EAX, rn, op->pc);
            x64_mov_r32_mem_cpu(e, X64_EDX, cpsr_off());
            x64_shift_r32_imm(e, 5, X64_EDX, 29);
            x64_alu_r32_imm(e, 4, X64_EDX, 1u);
            x64_alu_r32_imm(e, 6, X64_EDX, 1u);
            x64_alu_r32_imm(e, 5, X64_EAX, imm);
            x64_alu_r32_r32(e, 0x29, X64_EAX, X64_EDX);
            break;
        case 0x8: x64_emit_load_arm_reg(e, X64_EAX, rn, op->pc); x64_alu_r32_imm(e, 4, X64_EAX, imm); x64_emit_set_nz(e, X64_EAX, logical_s); return 1;
        case 0x9: x64_emit_load_arm_reg(e, X64_EAX, rn, op->pc); x64_alu_r32_imm(e, 6, X64_EAX, imm); x64_emit_set_nz(e, X64_EAX, logical_s); return 1;
        case 0xa: x64_emit_load_arm_reg(e, X64_EAX, rn, op->pc); x64_alu_r32_imm(e, 7, X64_EAX, imm); x64_emit_nzcv_from_x86_flags(e, 1); return 1;
        case 0xb: x64_emit_load_arm_reg(e, X64_EAX, rn, op->pc); x64_alu_r32_imm(e, 0, X64_EAX, imm); x64_emit_nzcv_from_x86_flags(e, 0); return 1;
        case 0xc: x64_emit_load_arm_reg(e, X64_EAX, rn, op->pc); x64_alu_r32_imm(e, 1, X64_EAX, imm); break;
        case 0xd: x64_mov_r32_imm(e, X64_EAX, imm); break;
        case 0xe: x64_emit_load_arm_reg(e, X64_EAX, rn, op->pc); x64_alu_r32_imm(e, 4, X64_EAX, ~imm); break;
        case 0xf: x64_mov_r32_imm(e, X64_EAX, ~imm); break;
        default: return 0;
        }
        x64_emit_data_result(e, opc, s, rd, done, logical_s);
        return 1;
    }
    if (!x64_emit_op2_to_ecx(e, insn, op->pc, logical_s)) return 0;
    x64_emit_load_arm_reg(e, X64_EAX, rn, op->pc);
    switch (opc) {
    case 0x0: x64_alu_r32_r32(e, 0x21, X64_EAX, X64_ECX); break; /* AND */
    case 0x1: x64_alu_r32_r32(e, 0x31, X64_EAX, X64_ECX); break; /* EOR */
    case 0x2: x64_alu_r32_r32(e, 0x29, X64_EAX, X64_ECX); break; /* SUB */
    case 0x3: x64_alu_r32_r32(e, 0x29, X64_ECX, X64_EAX); x64_mov_r32_r32(e, X64_EAX, X64_ECX); break; /* RSB */
    case 0x4: x64_alu_r32_r32(e, 0x01, X64_EAX, X64_ECX); break; /* ADD */
    case 0x5: /* ADC, no flag form */
        if (s) return 0;
        x64_mov_r32_mem_cpu(e, X64_EDX, cpsr_off());
        x64_shift_r32_imm(e, 5, X64_EDX, 29);
        x64_alu_r32_imm(e, 4, X64_EDX, 1u);
        x64_alu_r32_r32(e, 0x01, X64_EAX, X64_ECX);
        x64_alu_r32_r32(e, 0x01, X64_EAX, X64_EDX);
        break;
    case 0x6: /* SBC, no flag form */
        if (s) return 0;
        x64_mov_r32_mem_cpu(e, X64_EDX, cpsr_off());
        x64_shift_r32_imm(e, 5, X64_EDX, 29);
        x64_alu_r32_imm(e, 4, X64_EDX, 1u);
        x64_alu_r32_imm(e, 6, X64_EDX, 1u); /* xor edx,1 */
        x64_alu_r32_r32(e, 0x29, X64_EAX, X64_ECX);
        x64_alu_r32_r32(e, 0x29, X64_EAX, X64_EDX);
        break;
    case 0x8: /* TST */
        x64_alu_r32_r32(e, 0x21, X64_EAX, X64_ECX);
        x64_emit_set_nz(e, X64_EAX, logical_s);
        return 1;
    case 0x9: /* TEQ */
        x64_alu_r32_r32(e, 0x31, X64_EAX, X64_ECX);
        x64_emit_set_nz(e, X64_EAX, logical_s);
        return 1;
    case 0xa: /* CMP */
        x64_alu_r32_r32(e, 0x39, X64_EAX, X64_ECX);
        x64_emit_nzcv_from_x86_flags(e, 1);
        return 1;
    case 0xb: /* CMN */
        x64_alu_r32_r32(e, 0x01, X64_EAX, X64_ECX);
        x64_emit_nzcv_from_x86_flags(e, 0);
        return 1;
    case 0xc: x64_alu_r32_r32(e, 0x09, X64_EAX, X64_ECX); break; /* ORR */
    case 0xd: x64_mov_r32_r32(e, X64_EAX, X64_ECX); break; /* MOV */
    case 0xe: x64_not_r32(e, X64_ECX); x64_alu_r32_r32(e, 0x21, X64_EAX, X64_ECX); break; /* BIC */
    case 0xf: x64_mov_r32_r32(e, X64_EAX, X64_ECX); x64_not_r32(e, X64_EAX); break; /* MVN */
    default: return 0;
    }
    x64_emit_data_result(e, opc, s, rd, done, logical_s);
    return 1;
}

static void x64_emit_add_signed_imm_to_eax(x64_emit_t *e, int32_t off) {
    if (off > 0) x64_alu_r32_imm(e, 0, X64_EAX, (uint32_t)off);
    else if (off < 0) x64_alu_r32_imm(e, 5, X64_EAX, (uint32_t)(-off));
}

/* EDX is the VA (word callers align it first), and becomes a RAM offset.
 * R9/R10 survive for writeback and the original unaligned address. Use the
 * shared packed mirror: the A64-only derived page table is not available here.
 * Like that table, reject cold entries, tiny pages and unusual saved masks or
 * bases. Matching the complete masked VA prevents colliding TLB-index aliases.
 * All rejections precede guest mutations and re-execute the classified op. */
static void x64_emit_mmu_ram_offset(x64_emit_t *e, unsigned bytes,
                                   size_t *slow, unsigned *nslow) {
    _Static_assert(sizeof(arm_tlb_entry_t) == 16u, "x64 packed TLB stride");
    x64_mov_r32_r32(e, X64_R8D, X64_EDX);
    x64_shift_r32_imm(e, 5, X64_R8D, 12);
    x64_alu_r32_imm(e, 4, X64_R8D, 0xfffu);
    x64_shift_r32_imm(e, 4, X64_R8D, 4);
    /* mov dst, [rbx + r8 + offsetof(tlb_entry) + field] */
#define X64_TLB_LOAD(dst, field) do { \
    x64_rex(e, 0, (dst), X64_R8D, X64_EBX); x64_u8(e, 0x8b); \
    x64_modrm(e, 2, (dst), X64_ESP); x64_sib(e, 0, X64_R8D, X64_EBX); \
    x64_u32(e, (uint32_t)(offsetof(arm920t_t, tlb_entry) + offsetof(arm_tlb_entry_t, field))); \
} while (0)
    X64_TLB_LOAD(X64_EAX, valid);
    x64_alu_r32_r32(e, 0x85, X64_EAX, X64_EAX);
    slow[(*nslow)++] = x64_jcc32(e, 0x4);
    X64_TLB_LOAD(X64_R11D, mask);
    x64_alu_r32_imm(e, 7, X64_R11D, 0xfffu);
    size_t small = x64_jcc32(e, 0x4);
    x64_alu_r32_imm(e, 7, X64_R11D, 0xffffu);
    size_t large = x64_jcc32(e, 0x4);
    x64_alu_r32_imm(e, 7, X64_R11D, 0xfffffu);
    slow[(*nslow)++] = x64_jcc32(e, 0x5);
    x64_patch32(e, small, e->pos);
    x64_patch32(e, large, e->pos);
    X64_TLB_LOAD(X64_EAX, va_base);
    x64_mov_r32_r32(e, X64_ECX, X64_EDX);
    x64_not_r32(e, X64_R11D);
    x64_alu_r32_r32(e, 0x21, X64_ECX, X64_R11D);
    x64_alu_r32_r32(e, 0x39, X64_ECX, X64_EAX);
    slow[(*nslow)++] = x64_jcc32(e, 0x5);
    x64_not_r32(e, X64_R11D);
    X64_TLB_LOAD(X64_EAX, pa_base);
#undef X64_TLB_LOAD
    x64_alu_r32_r32(e, 0x85, X64_EAX, X64_R11D);
    slow[(*nslow)++] = x64_jcc32(e, 0x5); /* preserve OR-based saved-state semantics */
    x64_alu_r32_r32(e, 0x21, X64_EDX, X64_R11D);
    x64_alu_r32_r32(e, 0x09, X64_EDX, X64_EAX);
    x64_alu_r32_imm(e, 5, X64_EDX, ARM_JIT_RAM_BASE_ADDR);
    x64_alu_r32_imm(e, 7, X64_EDX, ARM_JIT_RAM_SIZE_BYTES - bytes);
    slow[(*nslow)++] = x64_jcc32(e, 0x7);
}

static void x64_emit_memory_slow(x64_emit_t *e, const arm_jit_op_t *op,
                             size_t *slow, unsigned nslow) {
    size_t next = x64_jmp32(e);
    for (unsigned i = 0; i < nslow; ++i) x64_patch32(e, slow[i], e->pos);
    x64_emit_call_helper_op(e, op);
    x64_patch32(e, next, e->pos);
}

/* Prove the entire aligned RAM span before the first transfer/writeback.
 * Any rejected span executes the whole instruction through the checked helper,
 * so callbacks finish all lanes and writeback before IRQ/flush/disable exits.
 * With MMU on, a virtual page crossing also needs per-word translation. */
static int x64_emit_ram_block_dt(x64_emit_t *e, const arm_jit_op_t *op, uint32_t done) {
    uint32_t list = op->imm;
    unsigned rn = op->a, count = op->g;
    int l = !!(op->d & 1u), w = !!(op->d & 2u);
    int u = !!(op->d & 8u), p = !!(op->d & 16u);
    int block_pc = l && (list & 0x8000u);
    if ((op->reserved && op->reserved != 3u && op->reserved != 4u &&
         op->reserved != 5u) || (op->d & 4u) || !list || !count || rn == 15u ||
        !(l ? e->ram_read : e->ram_write)) return 0;
    size_t slow[8];
    unsigned nslow = 0;
    x64_emit_load_arm_reg(e, X64_EAX, rn, op->pc);
    x64_mov_r32_r32(e, X64_R9D, X64_EAX);
    if (w) x64_alu_r32_imm(e, u ? 0 : 5, X64_R9D, 4u * count);
    x64_emit_add_signed_imm_to_eax(e, u ? (p ? 4 : 0) : -(int32_t)(4u * count) + (p ? 0 : 4));
    x64_mov_r32_r32(e, X64_R10D, X64_EAX);
    x64_test_r32_imm(e, X64_EAX, 3u);
    slow[nslow++] = x64_jcc32(e, 0x5);
    if (e->mmu) {
        x64_alu_r32_imm(e, 4, X64_EAX, 0xfffu);
        x64_alu_r32_imm(e, 7, X64_EAX, 0x1000u - 4u * count);
        slow[nslow++] = x64_jcc32(e, 0x7);
    }
    x64_mov_r32_r32(e, X64_EDX, X64_R10D);
    if (e->mmu) x64_emit_mmu_ram_offset(e, 4u * count, slow, &nslow);
    else {
        x64_alu_r32_imm(e, 5, X64_EDX, ARM_JIT_RAM_BASE_ADDR);
        x64_alu_r32_imm(e, 7, X64_EDX, ARM_JIT_RAM_SIZE_BYTES - 4u * count);
        slow[nslow++] = x64_jcc32(e, 0x7);
    }
    for (unsigned r = 0; r < 16u; ++r) {
        if (!(list & (1u << r))) continue;
        if (l) {
            /* ARMv4T LDM uses write_r, not BX semantics: all target low
             * bits are discarded in ARM state. No target-dependent bailout
             * remains after the complete RAM span has been checked. Save PC
             * until writeback commits; no callback can observe this path. */
            if (r == 15u) x64_mov_r32_membase_index(e, X64_R11D, X64_R14D, X64_EDX);
            else {
                x64_mov_r32_membase_index(e, X64_EAX, X64_R14D, X64_EDX);
                x64_emit_store_arm_reg(e, r, X64_EAX);
            }
        } else {
            x64_emit_load_arm_reg(e, X64_ECX, r, op->pc);
            x64_mov_membase_index_r32(e, X64_R14D, X64_EDX, X64_ECX);
        }
        x64_alu_r32_imm(e, 0, X64_EDX, 4u);
    }
    if (w && (!l || !(list & (1u << rn)))) x64_emit_store_arm_reg(e, rn, X64_R9D);
    if (block_pc) {
        size_t leaf_continue = 0;
        if (op->reserved == 3u || op->reserved == 5u) {
            /* Inlined leaf return. Keep the real stack word and continue only
             * at this trace's decoded successor; any other target leaves the
             * block with the loaded and aligned PC committed. */
            x64_alu_r32_imm(e, 4, X64_R11D, ~3u);
            x64_mov_mem_cpu_r32(e, arm_reg_off(15), X64_R11D);
            x64_alu_r32_imm(e, 7, X64_R11D, e->expected_next);
            size_t matched = x64_jcc32(e, 0x4); /* EQ */
            x64_emit_return_imm(e, done);
            x64_patch32(e, matched, e->pos);
            leaf_continue = x64_jmp32(e);
        } else x64_emit_return_pc_reg(e, X64_R11D, done);
        /* Only failed prechecks reach the whole-op helper. Neither exit can
         * fall through and overwrite the loaded PC with a sequential PC. */
        for (unsigned i = 0; i < nslow; ++i) x64_patch32(e, slow[i], e->pos);
        x64_emit_call_helper_op(e, op);
        x64_emit_return_imm(e, done);
        if (leaf_continue) x64_patch32(e, leaf_continue, e->pos);
    } else x64_emit_memory_slow(e, op, slow, nslow);
    return 1;
}

static int x64_emit_mul(x64_emit_t *e, const arm_jit_op_t *op) {
    const uint32_t insn = op->insn;
    if (insn & (1u << 23)) return 0; /* long multiply stays on precise C path */
    unsigned rd = (insn >> 16) & 0xfu, rn = (insn >> 12) & 0xfu, rs = (insn >> 8) & 0xfu, rm = insn & 0xfu;
    if (rd == 15u || rn == 15u || rs == 15u || rm == 15u) return 0;
    x64_emit_load_arm_reg(e, X64_EAX, rm, op->pc);
    x64_emit_load_arm_reg(e, X64_ECX, rs, op->pc);
    x64_imul_r32_r32(e, X64_EAX, X64_ECX);
    if (insn & (1u << 21)) {
        x64_emit_load_arm_reg(e, X64_ECX, rn, op->pc);
        x64_alu_r32_r32(e, 0x01, X64_EAX, X64_ECX);
    }
    x64_emit_store_arm_reg(e, rd, X64_EAX);
    if (insn & (1u << 20)) x64_emit_set_nz(e, X64_EAX, 0);
    return 1;
}

static int x64_emit_block_dt(x64_emit_t *e, const arm_jit_op_t *op, uint32_t done) {
    /* Framed leaf push/pop wrappers carry markers 3/4/5, but they are still
     * ordinary single-word stack transfers: the direct-RAM path keeps their
     * exact ordering, writeback suppression and decoded-return guard. */
    /* Retain the conservative non-MMU base-in-list gates. Unusual forms
     * also stay on the whole-op semantic helper. */
    if ((op->imm & (1u << op->a)) && (op->d & 3u)) return 0;
    return x64_emit_ram_block_dt(e, op, done);
}

static int x64_emit_addrmode2_offset_to_ecx(x64_emit_t *e, uint32_t insn, uint32_t pc) {
    if (!(insn & (1u << 25))) {
        x64_mov_r32_imm(e, X64_ECX, insn & 0xfffu);
        return 1;
    }
    if (insn & (1u << 4)) return 0; /* register-count offset shifts stay on C path */
    unsigned rm = insn & 0xfu;
    unsigned type = (insn >> 5) & 3u;
    unsigned amount = (insn >> 7) & 0x1fu;
    x64_emit_load_arm_reg(e, X64_ECX, rm, pc);
    if (amount == 0u) {
        if (type == 0u) return 1;                    /* LSL #0 */
        if (type == 1u) { x64_mov_r32_imm(e, X64_ECX, 0); return 1; } /* LSR #32 */
        if (type == 2u) { x64_shift_r32_imm(e, 7, X64_ECX, 31); return 1; } /* ASR #32 */
        return 0; /* RRX needs C; uncommon for the GP32 hot path */
    }
    switch (type) {
    case 0: x64_shift_r32_imm(e, 4, X64_ECX, amount); return 1;
    case 1: x64_shift_r32_imm(e, 5, X64_ECX, amount); return 1;
    case 2: x64_shift_r32_imm(e, 7, X64_ECX, amount); return 1;
    default: x64_shift_r32_imm(e, 1, X64_ECX, amount); return 1;
    }
}

/* Single/half transfers commit only after a proved RAM hit. BIOS,
 * devices, cold misses and rejected spans retain the whole-op helper. Word
 * translation aligns the VA before lookup and rotates by the original VA;
 * halfwords instead access contiguous physical bytes, including page edges.
 * A word load into PC commits the architected write_r result and leaves the
 * trace, since its target is not this trace's decoded successor; a framed
 * leaf return (markers 3/5) instead continues when the loaded target is that
 * successor. Byte loads into PC and unusual forms keep the whole-op helper. */
static int x64_emit_ram_dt(x64_emit_t *e, const arm_jit_op_t *op, uint32_t done) {
    const uint32_t insn = op->insn;
    unsigned rn = (insn >> 16) & 15u, rd = (insn >> 12) & 15u;
    int half = op->kind == ARM_JIT_OP_HALF;
    int p = GP32_BIT(insn,24), u = GP32_BIT(insn,23), w = GP32_BIT(insn,21), l = GP32_BIT(insn,20);
    unsigned sh = (insn >> 5) & 3u;
    unsigned bytes = half ? ((l && sh == 2u) ? 1u : 2u) : (GP32_BIT(insn,22) ? 1u : 4u);
    /* A framed leaf return loads its return address from the stack; keep the
     * architected write_r alignment and leave the trace unless the loaded
     * target is this trace's decoded successor. */
    int leaf_return = !half && l && rd == 15u &&
                      (op->reserved == 3u || op->reserved == 5u);
    /* Ordinary word load into PC: same write_r commit, but the target is an
     * unknown branch, so the block always leaves with the loaded address.
     * Only the RAM span is proved here; every rejected span re-executes the
     * whole instruction through the checked helper before the exit. */
    int pc_store = !half && l && rd == 15u && !GP32_BIT(insn, 22);
    if (!(l ? e->ram_read : e->ram_write) || (l && rd == 15u && !pc_store) ||
        ((!p || w) && rn == 15u) || (half && (rd == 15u || (l ? !sh : sh != 1u)))) return 0;
    if (half) {
        if (GP32_BIT(insn,22)) x64_mov_r32_imm(e, X64_ECX, op->imm);
        else if (op->c == 15u) x64_mov_r32_imm(e, X64_ECX, op->pc + 4u);
        else x64_emit_load_arm_reg(e, X64_ECX, op->c, op->pc);
    } else if (!x64_emit_addrmode2_offset_to_ecx(e, insn, op->pc)) return 0;
    x64_emit_load_arm_reg(e, X64_EAX, rn, op->pc);
    x64_mov_r32_r32(e, X64_R9D, X64_EAX);
    x64_alu_r32_r32(e, u ? 0x01 : 0x29, X64_R9D, X64_ECX); /* final base survives lookup */
    x64_mov_r32_r32(e, X64_R10D, p ? X64_R9D : X64_EAX);
    x64_mov_r32_r32(e, X64_EDX, X64_R10D);
    if (bytes == 4u) x64_alu_r32_imm(e, 4, X64_EDX, ~3u);
    size_t slow[5];
    unsigned nslow = 0;
    if (e->mmu) x64_emit_mmu_ram_offset(e, bytes, slow, &nslow);
    else {
        /* Reject the entire physical access before any guest mutation. A miss
         * executes the whole instruction once and checks callback exit state;
         * callback-free RAM hits need no per-instruction trace/status check. */
        x64_alu_r32_imm(e, 5, X64_EDX, ARM_JIT_RAM_BASE_ADDR);
        x64_alu_r32_imm(e, 7, X64_EDX, ARM_JIT_RAM_SIZE_BYTES - bytes);
        slow[nslow++] = x64_jcc32(e, 0x7);
    }
    if (l) {
        if (bytes == 4u) {
            x64_mov_r32_membase_index(e, X64_EAX, X64_R14D, X64_EDX);
            x64_mov_r32_r32(e, X64_ECX, X64_R10D);
            x64_alu_r32_imm(e, 4, X64_ECX, 3u);
            x64_shift_r32_imm(e, 4, X64_ECX, 3);
            x64_shift_r32_cl(e, 1, X64_EAX);
        } else if (bytes == 2u) {
            if (sh == 3u) x64_movsx_r32_membase_index16(e, X64_EAX, X64_R14D, X64_EDX);
            else x64_movzx_r32_membase_index16(e, X64_EAX, X64_R14D, X64_EDX);
        } else if (half) x64_movsx_r32_membase_index8(e, X64_EAX, X64_R14D, X64_EDX);
        else x64_movzx_r32_membase_index8(e, X64_EAX, X64_R14D, X64_EDX);
        if (!pc_store) x64_emit_store_arm_reg(e, rd, X64_EAX);
    } else {
        x64_emit_load_arm_reg(e, X64_ECX, rd, op->pc);
        if (bytes == 4u) x64_mov_membase_index_r32(e, X64_R14D, X64_EDX, X64_ECX);
        else if (bytes == 2u) x64_mov_membase_index16_r32(e, X64_R14D, X64_EDX, X64_ECX);
        else x64_mov_membase_index8_r32(e, X64_R14D, X64_EDX, X64_ECX);
    }
    if (!p || w) x64_emit_store_arm_reg(e, rn, X64_R9D);
    if (pc_store && !leaf_return) {
        /* The loaded target is an unknown branch, not this trace's decoded
         * successor: commit it and retire here, exactly like the other
         * PC-writing natives. A rejected span re-executes the whole
         * instruction through the checked helper and still retires. Both
         * arms end in an explicit return so the block never falls into the
         * shared sequential epilogue. */
        x64_alu_r32_imm(e, 4, X64_EAX, ~3u);
        x64_mov_mem_cpu_r32(e, arm_reg_off(15), X64_EAX);
        x64_emit_return_imm(e, done);
        for (unsigned i = 0; i < nslow; ++i) x64_patch32(e, slow[i], e->pos);
        x64_emit_call_helper_op(e, op);
        x64_emit_return_imm(e, done);
        return 1;
    }
    size_t leaf_continue = 0;
    if (leaf_return) {
        /* write_r(15) in ARM state: a framed leaf return commits the loaded
         * word with its low bits masked and continues only on the decoded
         * successor. */
        x64_alu_r32_imm(e, 4, X64_EAX, ~3u);
        x64_mov_mem_cpu_r32(e, arm_reg_off(15), X64_EAX);
        x64_alu_r32_imm(e, 7, X64_EAX, e->expected_next);
        size_t matched = x64_jcc32(e, 0x4); /* EQ */
        x64_emit_return_imm(e, done);
        x64_patch32(e, matched, e->pos);
        leaf_continue = x64_jmp32(e);
    }
    x64_emit_memory_slow(e, op, slow, nslow);
    if (leaf_continue) x64_patch32(e, leaf_continue, e->pos);
    return 1;
}

static void x64_emit_cond_skip(x64_emit_t *e, unsigned cond, size_t *patch, unsigned *npatch) {
    *npatch = 0;
    if (cond == 14u) return;
    x64_mov_r32_mem_cpu(e, X64_EAX, cpsr_off());
    switch (cond) {
    case 0: x64_alu_r32_imm(e, 4, X64_EAX, Z_FLAG); patch[(*npatch)++] = x64_jcc32(e, 0x4); break; /* !Z -> skip */
    case 1: x64_alu_r32_imm(e, 4, X64_EAX, Z_FLAG); patch[(*npatch)++] = x64_jcc32(e, 0x5); break;
    case 2: x64_alu_r32_imm(e, 4, X64_EAX, C_FLAG); patch[(*npatch)++] = x64_jcc32(e, 0x4); break;
    case 3: x64_alu_r32_imm(e, 4, X64_EAX, C_FLAG); patch[(*npatch)++] = x64_jcc32(e, 0x5); break;
    case 4: x64_alu_r32_imm(e, 4, X64_EAX, N_FLAG); patch[(*npatch)++] = x64_jcc32(e, 0x4); break;
    case 5: x64_alu_r32_imm(e, 4, X64_EAX, N_FLAG); patch[(*npatch)++] = x64_jcc32(e, 0x5); break;
    case 6: x64_alu_r32_imm(e, 4, X64_EAX, V_FLAG); patch[(*npatch)++] = x64_jcc32(e, 0x4); break;
    case 7: x64_alu_r32_imm(e, 4, X64_EAX, V_FLAG); patch[(*npatch)++] = x64_jcc32(e, 0x5); break;
    case 8: /* HI: C && !Z */
        x64_alu_r32_imm(e, 4, X64_EAX, C_FLAG); patch[(*npatch)++] = x64_jcc32(e, 0x4);
        x64_mov_r32_mem_cpu(e, X64_EAX, cpsr_off()); x64_alu_r32_imm(e, 4, X64_EAX, Z_FLAG); patch[(*npatch)++] = x64_jcc32(e, 0x5); break;
    case 9: { /* LS: !C || Z; skip if C && !Z */
        size_t pass;
        x64_alu_r32_imm(e, 4, X64_EAX, C_FLAG); pass = x64_jcc32(e, 0x4);
        x64_mov_r32_mem_cpu(e, X64_EAX, cpsr_off()); x64_alu_r32_imm(e, 4, X64_EAX, Z_FLAG); patch[(*npatch)++] = x64_jcc32(e, 0x4);
        x64_patch32(e, pass, e->pos);
        break;
    }
    case 10: case 11: case 12: case 13: {
        size_t pass_patch = 0;
        int has_pass_patch = 0;
        if (cond == 12u || cond == 13u) {
            x64_alu_r32_imm(e, 4, X64_EAX, Z_FLAG);
            if (cond == 12u) patch[(*npatch)++] = x64_jcc32(e, 0x5); /* GT: Z set means fail */
            else { pass_patch = x64_jcc32(e, 0x5); has_pass_patch = 1; } /* LE: Z set means pass */
        }
        x64_mov_r32_mem_cpu(e, X64_EAX, cpsr_off());
        x64_mov_r32_r32(e, X64_ECX, X64_EAX);
        x64_shift_r32_imm(e, 5, X64_EAX, 31);
        x64_shift_r32_imm(e, 5, X64_ECX, 28);
        x64_alu_r32_r32(e, 0x31, X64_EAX, X64_ECX);
        x64_alu_r32_imm(e, 4, X64_EAX, 1u);
        if (cond == 10u || cond == 12u) patch[(*npatch)++] = x64_jcc32(e, 0x5); /* GE/GT fail on N^V */
        else patch[(*npatch)++] = x64_jcc32(e, 0x4);                         /* LT/LE fail on !(N^V) */
        if (has_pass_patch) x64_patch32(e, pass_patch, e->pos);
        break;
    }
    default: patch[(*npatch)++] = x64_jmp32(e); break;
    }
}

static void *arm_jit_code_reserve(arm920t_t *c, size_t bytes) {
    const size_t align = 16u;
    if (!c->jit_code) {
        c->jit_code_size = ARM_JIT_CODE_SIZE;
        c->jit_code = arm_jit_alloc_exec(c->jit_code_size);
        if (!c->jit_code) { c->jit_code_size = c->jit_code_used = 0; ARM_PROF_INC(c, jit_code_alloc_failures); return NULL; }
    }
    size_t p = (c->jit_code_used + align - 1u) & ~(align - 1u);
    if (bytes > c->jit_code_size || p > c->jit_code_size - bytes) { ARM_PROF_INC(c, jit_code_full_events); return NULL; }
    c->jit_code_used = p + bytes;
    return c->jit_code + p;
}

static int x64_emit_bx(x64_emit_t *e, const arm_jit_op_t *op, uint32_t done) {
    const uint32_t insn = op->insn;
    if ((insn & 0x0ffffff0u) != 0x012fff10u) return 0;
    x64_emit_arg0_cpu(e);
    /* Interpreter semantics: exec_arm_at commits r15 = pc+4 before executing
     * and BX reads that raw value, so BX r15 targets pc+4 rather than the
     * architectural pc+8 a register operand would produce. */
    if ((insn & 0xfu) == 15u) x64_mov_r32_imm(e, X64_HOST_ARG1, op->pc + 4u);
    else x64_emit_load_arm_reg(e, X64_HOST_ARG1, insn & 0xfu, op->pc);
    x64_call_abs(e, (uintptr_t)arm_jit_write_pc_x_helper);
    x64_emit_return_imm(e, done);
    return 1;
}

static int x64_emit_coproc_local(x64_emit_t *e, const arm_jit_op_t *op, uint32_t done) {
    const uint32_t insn = op->insn;
    if (!arm_jit_is_local_cp15_op(insn)) return 0;
    unsigned crn = (insn >> 16) & 0xfu;
    unsigned rd = (insn >> 12) & 0xfu;
    if (!GP32_BIT(insn, 20)) {
        x64_emit_load_arm_reg(e, X64_EAX, rd, op->pc);
        x64_mov_mem_cpu_r32(e, (uint32_t)offsetof(arm920t_t, cp15) + crn * 4u, X64_EAX);
    } else {
        if (crn == 0u) x64_mov_r32_imm(e, X64_EAX, 0x41129200u);
        else x64_mov_r32_mem_cpu(e, X64_EAX, (uint32_t)offsetof(arm920t_t, cp15) + crn * 4u);
        x64_emit_store_arm_reg(e, rd, X64_EAX);
    }
    if (op->reserved != 6u) x64_emit_return_imm(e, done);
    return 1;
}

static int x64_emit_one(x64_emit_t *e, const arm_jit_op_t *op, uint32_t done) {
    if (op->kind == ARM_JIT_OP_INTERP && op->reserved == 7u) {
        /* BX lr must commit interworking and prove PC/status/interrupt state
         * before following the caller. Reuse the precise checked helper. */
        x64_emit_call_helper_op(e, op);
        return 1;
    }
    if (op->kind == ARM_JIT_OP_INTERP && x64_emit_bx(e, op, done)) return 1;
    switch ((arm_jit_kind_t)op->kind) {
    case ARM_JIT_OP_DATA: return x64_emit_data_proc(e, op, done);
    case ARM_JIT_OP_MUL: return x64_emit_mul(e, op);
    case ARM_JIT_OP_HALF:
    case ARM_JIT_OP_SINGLE_DT: return x64_emit_ram_dt(e, op, done);
    case ARM_JIT_OP_BLOCK_DT: return e->mmu ? x64_emit_ram_block_dt(e, op, done) : x64_emit_block_dt(e, op, done);
    case ARM_JIT_OP_BRANCH: {
        int32_t off = gp32_sign_extend((op->insn & 0x00ffffffu) << 2, 26);
        if (op->insn & (1u << 24)) x64_mov_mem_cpu_imm(e, arm_reg_off(14), op->pc + 4u);
        if (op->reserved == 2u || (!op->stop && arm_jit_is_uncond_b_no_link(op->insn))) { GP32_UNUSED(done); return 1; }
        x64_mov_mem_cpu_imm(e, arm_reg_off(15), (uint32_t)(op->pc + 8u + off));
        x64_emit_return_imm(e, done);
        return 1;
    }
    case ARM_JIT_OP_PSR: {
        unsigned rd = (op->insn >> 12) & 15u;
        if ((op->insn & 0x0fbf0fffu) == 0x010f0000u && rd != 15u) {
            if (op->insn & (1u << 22)) {
                x64_emit_arg0_cpu(e);
                x64_call_abs(e, (uintptr_t)arm_x64_read_spsr);
            } else x64_mov_r32_mem_cpu(e, X64_EAX, cpsr_off());
            x64_mov_mem_cpu_r32(e, arm_reg_off(rd), X64_EAX);
            return 1;
        }
        x64_emit_call_helper_op(e, op);
        if (op->stop) x64_emit_return_imm(e, done);
        return 1;
    }
    case ARM_JIT_OP_COPROC:
        if (x64_emit_coproc_local(e, op, done)) return 1;
        [[fallthrough]]; /* a non-local CP15 op uses the shared helper dispatch */
    case ARM_JIT_OP_UNDEFINED:
    case ARM_JIT_OP_INTERP:
    case ARM_JIT_OP_SWP:
    case ARM_JIT_OP_SWI:
    default:
        x64_emit_call_helper_op(e, op);
        if (op->stop || op->kind == ARM_JIT_OP_UNDEFINED || op->kind == ARM_JIT_OP_SWI || (op->kind == ARM_JIT_OP_COPROC && op->reserved != 6u)) x64_emit_return_imm(e, done);
        return 1;
    }
}

static void arm_jit_compile_native(arm920t_t *c, arm_jit_block_t *b) {
    if (!c || !b || !b->count) return;
    uint8_t tmp[ARM_JIT_NATIVE_MAX_BYTES];
    x64_emit_t e;
    memset(&e, 0, sizeof(e));
    e.b = tmp;
    e.cap = sizeof(tmp);
    e.mmu = !!(c->cp15[1] & 1u);
    e.generation = b->generation;
    /* A read pointer alone does not authorize stores or a complete window. */
    e.ram_read = c->jit_ram_base && fastmem(c, ARM_JIT_RAM_BASE_ADDR, ARM_JIT_RAM_SIZE_BYTES, 0) == c->jit_ram_base;
    e.ram_write = c->jit_ram_base && fastmem(c, ARM_JIT_RAM_BASE_ADDR, ARM_JIT_RAM_SIZE_BYTES, 1) == c->jit_ram_base;
    x64_u8(&e, 0x53);                         /* push rbx */
    x64_u8(&e, 0x41); x64_u8(&e, 0x56);       /* push r14 */
    x64_u8(&e, 0x41); x64_u8(&e, 0x57);       /* push r15 */
#if defined(_WIN32)
    x64_u8(&e, 0x56);                         /* push rsi: scratch in flags code, nonvolatile on Win64 */
    x64_u8(&e, 0x57);                         /* push rdi: keep an odd push count and preserve nonvolatile */
#endif
    x64_mov_r64_r64(&e, X64_EBX, X64_NATIVE_ARG0); /* rbx = cpu */
    x64_alu_r32_imm(&e, 7, X64_NATIVE_ARG1, b->count);
    size_t enough = x64_jcc32(&e, 0x3); /* unsigned budget >= block count */
    x64_emit_return_imm(&e, 0);
    x64_patch32(&e, enough, e.pos);
    x64_mov_r64_mem_cpu(&e, X64_R14D, jit_ram_base_off());
    x64_mov_r64_mem_cpu(&e, X64_R15D, jit_bios_base_off());
    for (uint8_t i = 0; i < b->count; ++i) {
        const arm_jit_op_t *op = &arm_jit_ops(c, b)[i];
        e.expected_next = i + 1u < b->count ? arm_jit_ops(c, b)[i + 1u].pc : op->pc + 4u;
        e.done = (uint32_t)i + 1u;
        e.guest_pc = op->pc;
        size_t patches[3];
        unsigned npatch = 0;
        /* A real BL and register-only leaf preserve the link/execution state.
         * Elide its known return guard, retaining PC and guest cycle indices.
         * Boundary returns still execute their guarded real-PC path. */
        if (op->reserved == 7u && i + 1u < b->count) {
            x64_mov_mem_cpu_imm(&e, arm_reg_off(15), e.expected_next);
            continue;
        }
        if (op->reserved == 8u) {
            x64_emit_cond_skip(&e, op->cond ^ 1u, patches, &npatch);
            x64_mov_mem_cpu_imm(&e, arm_reg_off(15), op->pc + 4u);
            x64_emit_return_imm(&e, (uint32_t)i + 1u);
            for (unsigned j = 0; j < npatch; ++j) x64_patch32(&e, patches[j], e.pos);
            continue;
        }
        if (arm_jit_is_side_effect_free_nop(op->insn)) continue;
        if (op->stop) x64_mov_mem_cpu_imm(&e, arm_reg_off(15), op->pc + 4u);
        x64_emit_cond_skip(&e, op->cond, patches, &npatch);
        if (!x64_emit_one(&e, op, (uint32_t)i + 1u)) {
            x64_emit_call_helper_op(&e, op);
            if (op->stop || op->kind == ARM_JIT_OP_UNDEFINED || op->kind == ARM_JIT_OP_SWI || (op->kind == ARM_JIT_OP_COPROC && op->reserved != 6u)) x64_emit_return_imm(&e, (uint32_t)i + 1u);
        }
        if (op->stop || op->kind == ARM_JIT_OP_UNDEFINED || op->kind == ARM_JIT_OP_SWI || (op->kind == ARM_JIT_OP_COPROC && op->reserved != 6u)) {
            for (unsigned j = 0; j < npatch; ++j) x64_patch32(&e, patches[j], e.pos);
            if (npatch || e.pos < 2 || e.b[e.pos - 1u] != 0xc3u) x64_emit_return_imm(&e, (uint32_t)i + 1u);
            break;
        }
        for (unsigned j = 0; j < npatch; ++j) x64_patch32(&e, patches[j], e.pos);
    }
    /* Always leave an explicit fall-through epilogue.  A conditional branch
       with stop==0 emits an early return for the taken path, but the failed
       condition jumps past that return and still needs a normal epilogue.
       Extra epilogues after unconditional returns are unreachable and benign. */
    {
        const arm_jit_op_t *last = &arm_jit_ops(c, b)[b->count - 1u];
        uint32_t final_pc = last->pc + 4u;
        /* A stitched branch may occupy the final slot at trace capacity. */
        if (last->kind == ARM_JIT_OP_BRANCH && last->cond == 14u)
            final_pc = arm_jit_branch_target(last->pc, last->insn);
        x64_mov_mem_cpu_imm(&e, arm_reg_off(15), final_pc);
        x64_emit_return_imm(&e, b->count);
    }
    if (e.fail) return;
    void *dst = arm_jit_code_reserve(c, e.pos);
    if (!dst) return;
    memcpy(dst, tmp, e.pos);
    arm_jit_flush_exec(dst, e.pos);
    union { void *p; arm_jit_native_fn f; } cvt;
    cvt.p = dst;
    b->native = cvt.f;
    b->native_ok = 1;
}
#elif defined(ARM_JIT_NATIVE_A64)
#include "arm920t_jit_a64.inc"
#else
static void arm_jit_compile_native(arm920t_t *c, arm_jit_block_t *b) { GP32_UNUSED(c); GP32_UNUSED(b); }
#endif

/* A poll proof must not create extra page-table reads while checking a load,
 * including a partial counted iteration decoded at a different entry PC. */
static int arm_poll_load_mapping_cached(const arm920t_t *c, const arm_jit_op_t *op) {
    if (!(c->cp15[1] & 1u) || op->kind != ARM_JIT_OP_SINGLE_DT) return 1;
    uint32_t base = op->a == 15u ? op->pc + 8u : c->r[op->a];
    uint32_t va = (op->d & ARM_BC_SD_U) ? base + op->imm : base - op->imm;
    const arm_tlb_entry_t *e = &c->tlb_entry[(va >> 12) & 0xfffu];
    if (!e->valid || (va & ~e->mask) != e->va_base) return 0;
    uint32_t pa = (e->pa_base | (va & e->mask)) & ~3u;
    /* Keep poll skipping within the existing direct RAM/BIOS/I/O windows.
     * Other physical fallback accesses now translate only once too, but
     * widening the skip proof is a separate change from that correctness fix. */
    return (c->jit_ram_base && arm_jit_addr_in_ram(pa, 4u)) ||
           (c->jit_bios_base && arm_jit_addr_in_bios(pa, 4u)) ||
           arm_jit_addr_in_identity_io(pa, 4u);
}

static int arm_poll_read_stable(arm920t_t *c, const arm_jit_op_t *op) {
    if (op->kind == ARM_JIT_OP_BLOCK_DT) {
        /* The leaf wrapper's idempotent stack write must target ordinary RAM,
         * never a device register whose repeated writes may have effects. */
        uint32_t addr = c->r[13] - (op->reserved == 4u ? 4u : 0u);
        uint32_t phys = mmu_translate(c, addr);
        return !(addr & 3u) && arm_jit_addr_in_ram(phys, 4u) &&
               fastmem(c, phys, 4u, 1) != NULL;
    }
    if (op->kind != ARM_JIT_OP_SINGLE_DT) return 1;
    /* Evaluate the actual address before this instruction, including literal
     * loads and pointer chains. A restored base alone is not a proof that
     * intermediate accesses are side-effect-free. */
    uint32_t base = op->a == 15u ? op->pc + 8u : c->r[op->a];
    uint32_t addr = (op->d & ARM_BC_SD_U) ? base + op->imm : base - op->imm;
    if (!(op->d & ARM_BC_SD_L)) {
        if (addr & 3u) return 0;
        uint32_t phys = mmu_translate(c, addr);
        if (!arm_jit_addr_in_ram(phys, 4u)) return 0;
        const uint8_t *p = fastmem(c, phys, 4u, 1);
        /* A stable endpoint alone cannot prove intermediate writes harmless.
         * Every store in the observed repetition must already be a no-op. */
        return p && gp32_ld32le(p) == c->r[op->b];
    }
    return !(addr & 3u) && arm_poll_load_mapping_cached(c, op) && c->bus.is_stable_read32 &&
           c->bus.is_stable_read32(c->bus.user, mmu_translate(c, addr));
}

/* No native block is active at lazy compilation. Recycle before emitting,
 * then restart dispatch: wrap may have cleared the block being considered.
 * Keep the emitter's large scratch frame out of the hot dispatch function.
 * Failed attempts are derived block state, reset only by retranslation. */
static ARM_NOINLINE int arm_counted_poll_compile(arm920t_t *c, arm_jit_block_t *b) {
#if ARM920T_NATIVE_BACKEND
    if (!c->jit_enabled || b->native_ok || arm_jit_needs_poll(c, b)) return 0;
    if (c->jit_code && c->jit_code_used &&
        c->jit_code_size > ARM_JIT_NATIVE_MAX_BYTES + 15u &&
        (c->jit_code_used > c->jit_code_size ||
         c->jit_code_size - c->jit_code_used < ARM_JIT_NATIVE_MAX_BYTES + 15u)) {
        arm920t_jit_invalidate_all(c, ARM_JIT_INV_CODE_RECYCLE);
        return 1;
    }
    b->native_ok = 2u;
    arm_jit_compile_native(c, b);
#if ARM920T_PROFILING
    if (b->native) c->prof.jit_native_compiled++;
    else c->prof.jit_native_failed++;
#endif
#else
    GP32_UNUSED(c); GP32_UNUSED(b);
#endif
    return 0;
}

/* The classified interpreter is much larger than native block dispatch. Keep
 * its register pressure and instruction footprint on the fallback path. */
static ARM_NOINLINE uint32_t arm_jit_run_portable(arm920t_t *c,
                                                 arm_jit_block_t *b,
                                                 uint32_t budget,
                                                 uint32_t run_done,
                                                 int *stable_reads) {
    uint32_t done = 0;
    const uint32_t generation = c->jit_generation;
    const uint32_t epoch = c->jit_cache_epoch;
    /* Decoded ops live in one stable allocation for the CPU's lifetime; the
     * generation/epoch fences below guard in-place reuse of the block. */
    const arm_jit_op_t *ops = arm_jit_ops(c, b);
    for (uint8_t i = 0; i < b->count && done < budget && run_done + done < c->run_limit; ++i) {
        const arm_jit_op_t *op = &ops[i];
        if ((c->r[15] & ~3u) != op->pc || thumb(c)) break;
        uint32_t expected_next = (i + 1u < b->count) ? (ops[i + 1u].pc & ~3u) : (op->pc + 4u);
        if (*stable_reads && (i >= b->poll_prefix || !arm_poll_read_stable(c, op))) *stable_reads = 0;
        arm_jit_exec_classified_bc(c, op);
        ARM_PROF_INC(c, block_interp_arm_insns);
        done++;
        /* jit=0 still uses decoded blocks. A bus callback can invalidate the
         * next decoded instruction or raise an interrupt just as in native
         * code. Finish this instruction, then return to the dispatcher. */
        if (c->trace || c->jit_generation != generation || c->jit_cache_epoch != epoch ||
            (c->irq_line && !(c->cpsr & I_FLAG)) ||
            (c->fiq_line && !(c->cpsr & F_FLAG))) {
            *stable_reads = 0;
            break;
        }
        if (op->stop || thumb(c) || c->halted || (c->r[15] & ~3u) != expected_next) break;
    }
    return done;
}

/* Equality excluding the counter proves a fixed point of the deterministic
 * body, not permission to ignore other changing registers. Fixed bases plus
 * current TLB hits exclude walks, TLB replacement and CP15 fault changes in
 * omitted iterations. No proof data survives this arm_jit_run invocation. */
static uint32_t arm_counted_poll_repeats(arm920t_t *c, const arm_jit_block_t *b,
                                       const uint32_t previous[16], uint32_t budget) {
    unsigned reg = (b->poll_backedge >> 1) - 1u;
    const arm_jit_op_t *ops = arm_jit_ops(c, b);
    uint32_t step = ops[b->count - 2u].imm, n = c->r[reg];
    if (!n || n > INT32_MAX || previous[reg] > INT32_MAX ||
        previous[reg] - n != step ||
        memcmp(previous, c->r, reg * sizeof(uint32_t)) ||
        memcmp(previous + reg + 1u, c->r + reg + 1u, (15u - reg) * sizeof(uint32_t))) return 0;
    if (c->cp15[1] & 1u) {
        for (unsigned i = 2u; i < b->count - 2u; ++i) {
            const arm_jit_op_t *op = &ops[i];
            if (op->kind != ARM_JIT_OP_SINGLE_DT) continue;
            if (!arm_poll_load_mapping_cached(c, op)) return 0;
        }
    }
    /* Every omitted CMP sees a positive signed counter: NZCV=0010.
     * ceil(n/step) full iterations may reach zero or a negative value; the
     * subsequent exit CMP/BLE (and every partial iteration) execute normally.
     * n,step <= INT32_MAX exclude signed SUB overflow even on the final step.
     * Divide before multiplication to stay inside the live uint32 budget. */
    uint32_t repeats = budget / b->count;
    uint32_t until_exit = (n - 1u) / step + 1u;
    if (repeats > until_exit) repeats = until_exit;
    c->r[reg] -= (uint32_t)((uint64_t)repeats * step);
    return repeats;
}

static uint32_t arm_jit_run(arm920t_t *c, uint32_t run_done) {
    if (!c || run_done >= c->run_limit || thumb(c) || c->trace) return 0;
    if (!c->jit_ram_base) c->jit_ram_base = fastmem(c, ARM_JIT_RAM_BASE_ADDR, 1u, 0);
    if (!c->jit_bios_base) c->jit_bios_base = fastmem(c, 0x00000000u, 1u, 0);
    uint32_t total = run_done;
    uint32_t poll_pc = UINT32_MAX, poll_count = 0, poll_cpsr = 0;
    uint32_t poll_regs[16];
    /* Rejection authorizes only actual native execution, never skipping.
     * It is transient, and cache/state changes revoke this run's decision. */
    uint32_t rejected_pc = UINT32_MAX, rejected_generation = 0, rejected_epoch = 0;
    /* A checked native helper can enable tracing during a bus callback.
     * Return to arm920t_run so the next instruction uses exec_arm's logger. */
    while (total < c->run_limit && !c->halted && !thumb(c) && !c->trace) {
        if (total != run_done && (c->irq_line || c->fiq_line)) {
            maybe_irq(c);
            if (c->halted || thumb(c) || c->trace) break;
        }
        uint32_t pc = c->r[15] & ~3u;
        uint32_t tag = c->jit_cache_epoch;
        arm_jit_block_t *b = c->jit_blocks ? &c->jit_blocks[arm_jit_block_index(pc)] : NULL;
        /* Keep the first-way hit as short as the direct-mapped lookup. Only
         * consult the other way after a miss; revalidate changed epochs before
         * accepting either way, exactly as for the original single slot. */
        if (!b || !((b->valid != 0u) & (b->tag_pc == pc) &
                    (b->tag_cache_epoch == tag) & (b->generation == c->jit_generation))) {
            arm_jit_block_t *alt = b ? b + 1 : NULL;
            if (b && b->valid && b->tag_pc == pc &&
                b->generation == c->jit_generation && arm_jit_fetch_unchanged(c, b)) {
                b->tag_cache_epoch = tag;
                c->jit_hits++;
                ARM_PROF_INC(c, jit_hits);
            } else if (alt && alt->valid && alt->tag_pc == pc &&
                       alt->generation == c->jit_generation &&
                       (alt->tag_cache_epoch == tag || arm_jit_fetch_unchanged(c, alt))) {
                b = alt;
                b->tag_cache_epoch = tag;
                c->jit_hits++;
                ARM_PROF_INC(c, jit_hits);
            } else {
                c->jit_misses++;
                ARM_PROF_INC(c, jit_misses);
                b = arm_jit_translate(c, pc);
                if (!b) break;
            }
        } else {
            c->jit_hits++;
            ARM_PROF_INC(c, jit_hits);
        }

        if (b->deferred_inline_pc) {
            uint32_t first;
            if (arm_jit_peek_fetch(c, b->deferred_inline_pc, &first)) {
                b = arm_jit_translate(c, pc);
                if (!b) break;
            }
        }

        uint32_t done = 0;
        int stable_reads = 0;
        /* Fetch/translation callbacks may also shorten the current run. */
        if (total >= c->run_limit) break;
        int counted = (b->poll_backedge >> 1) != 0;
        /* Only counted observations need invalidation snapshots across the
         * execution call. Keep that bookkeeping off ordinary native blocks. */
        uint32_t generation = counted ? c->jit_generation : 0;
        uint32_t epoch = counted ? c->jit_cache_epoch : 0;
        /* Use the existing per-read portable observation, not a second
         * speculative stability preflight. A missing contract rejects at
         * once; otherwise observe a full real repetition in this run first.
         * Native self-chains still execute every instruction and load. */
        int native_allowed = !counted || !c->bus.is_stable_read32 ||
            (rejected_pc == pc && rejected_generation == generation && rejected_epoch == epoch);
        if (ARM920T_NATIVE_BACKEND && counted && c->jit_enabled && native_allowed &&
            (c->run_limit - total) >= b->count && !b->native_ok) {
            if (arm_counted_poll_compile(c, b)) {
                poll_pc = rejected_pc = UINT32_MAX;
                continue;
            }
        }
        if (b->native && c->jit_enabled && native_allowed && (c->run_limit - total) >= b->count) {
            done = b->native(c, c->run_limit - total);
#if ARM920T_PROFILING
            c->prof.native_block_calls++;
            c->prof.native_arm_insns += done;
            if (!done) c->prof.native_bail_calls++;
#endif
        }

        if (!done) {
            stable_reads = c->bus.is_stable_read32 && b->poll_prefix;
            /* A real complete repetition may already be a fixed point of
             * its entry state. Seed that comparison once, rather than always
             * requiring a second observed repetition. Per-access stability,
             * cached mappings and all post-execution fences still prove it;
             * Only the counted load/AND body has no hidden memory writes:
             * leaf wrappers may initialize a stack slot on their first pass. */
            if (stable_reads && counted && poll_pc != pc) {
                poll_pc = pc;
                poll_count = b->count;
                poll_cpsr = c->cpsr;
                memcpy(poll_regs, c->r, sizeof(poll_regs));
            }
            done = arm_jit_run_portable(c, b, c->run_limit - total, total, &stable_reads);
        }
        if (!done) break;
        total += done;
        if (counted && !stable_reads && c->jit_generation == generation && c->jit_cache_epoch == epoch &&
            done == b->count && c->r[15] == pc && !thumb(c) && !c->halted && !c->trace) {
            rejected_pc = pc;
            rejected_generation = generation;
            rejected_epoch = epoch;
        }
        if (stable_reads && done <= b->poll_prefix && c->r[15] == pc && !thumb(c) && !c->halted &&
            !(c->irq_line && !(c->cpsr & I_FLAG)) && !(c->fiq_line && !(c->cpsr & F_FLAG))) {
            if (poll_pc == pc && poll_count == done && poll_cpsr == c->cpsr) {
                /* A callback can move the deadline behind this instruction.
                 * Saturate before subtracting, and never skip past the live
                 * deadline using the budget captured on entry. */
                uint32_t budget = total < c->run_limit ? c->run_limit - total : 0;
                uint32_t repeats = 0;
                if ((b->poll_backedge >> 1) && done == b->count)
                    repeats = arm_counted_poll_repeats(c, b, poll_regs, budget);
                else if (memcmp(poll_regs, c->r, sizeof(poll_regs)) == 0)
                    repeats = budget / done;
                total += repeats * done;
                c->jit_hits += repeats;
#if ARM920T_PROFILING
                c->prof.jit_hits += repeats;
                c->prof.poll_skip_events++;
                c->prof.poll_skipped_insns += (uint64_t)repeats * done;
#endif
            }
            poll_pc = pc;
            poll_count = done;
            poll_cpsr = c->cpsr;
            memcpy(poll_regs, c->r, sizeof(poll_regs));
        } else {
            poll_pc = UINT32_MAX;
        }
    }
    return total - run_done;
}

uint32_t arm920t_run(arm920t_t *c, uint32_t cycles) {
    if (!c) return 0;
    c->running = 1;
    c->run_limit = cycles;
    uint32_t done = 0;
    while (done < c->run_limit && !c->halted) {
        maybe_irq(c);
        uint32_t batch = 0;
        if (!thumb(c)) batch = arm_jit_run(c, done);
        if (batch) done += batch;
        else if (done >= c->run_limit) break;
        else if (thumb(c)) { ARM_PROF_INC(c, interp_thumb_insns); exec_thumb(c); done += 1; }
        else { ARM_PROF_INC(c, interp_arm_insns); exec_arm(c); done += 1; }
    }
    c->halted &= ~HALT_END_RUN;
    c->cycles_total += done;
    c->running = 0;
    c->run_limit = 0;
    return done;
}

void arm920t_get_cpu_profile(const arm920t_t *c, gp32_cpu_profile_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->supported = ARM920T_PROFILING;
    out->native_backend = ARM920T_NATIVE_BACKEND;
    if (!c) return;
    out->jit_code_size = c->jit_code_size;
    out->jit_code_used = c->jit_code_used;
    out->jit_block_capacity = ARM_JIT_BLOCK_COUNT;
#if ARM920T_PROFILING
    uint64_t used = out->jit_code_used;
    *out = c->prof;
    out->supported = 1;
    out->native_backend = ARM920T_NATIVE_BACKEND;
    out->jit_code_size = c->jit_code_size;
    out->jit_code_used = used;
    out->jit_code_highwater = c->prof.jit_code_highwater > used ? c->prof.jit_code_highwater : used;
    out->jit_block_capacity = ARM_JIT_BLOCK_COUNT;
#endif
}

void arm920t_reset_cpu_profile(arm920t_t *c) {
#if ARM920T_PROFILING
    if (c) memset(&c->prof, 0, sizeof(c->prof));
#else
    GP32_UNUSED(c);
#endif
}

int arm920t_state_save_io(const arm920t_t *c, state_io_t *io) {
    if (!c || !io) return 0;
    arm920t_state_image_t st;
    memset(&st, 0, sizeof(st));
    memcpy(st.r, c->r, sizeof(st.r));
    st.cpsr = c->cpsr;
    memcpy(st.bank_usr, c->bank_usr, sizeof(st.bank_usr));
    memcpy(st.bank_fiq, c->bank_fiq, sizeof(st.bank_fiq));
    memcpy(st.bank_svc, c->bank_svc, sizeof(st.bank_svc));
    memcpy(st.bank_abt, c->bank_abt, sizeof(st.bank_abt));
    memcpy(st.bank_irq, c->bank_irq, sizeof(st.bank_irq));
    memcpy(st.bank_und, c->bank_und, sizeof(st.bank_und));
    st.spsr_fiq = c->spsr_fiq; st.spsr_svc = c->spsr_svc; st.spsr_abt = c->spsr_abt; st.spsr_irq = c->spsr_irq; st.spsr_und = c->spsr_und;
    memcpy(st.cp15, c->cp15, sizeof(st.cp15));
    memcpy(st.tlb_va_base, c->tlb_va_base, sizeof(st.tlb_va_base));
    memcpy(st.tlb_pa_base, c->tlb_pa_base, sizeof(st.tlb_pa_base));
    memcpy(st.tlb_mask, c->tlb_mask, sizeof(st.tlb_mask));
    memcpy(st.tlb_valid, c->tlb_valid, sizeof(st.tlb_valid));
    st.cycles_total = c->cycles_total;
    st.irq_line = c->irq_line; st.fiq_line = c->fiq_line; st.halted = c->halted;
    return state_io_write(io, &st, sizeof(st));
}

void arm920t_state_apply(arm920t_t *c, const arm920t_state_image_t *st) {
    memcpy(c->r, st->r, sizeof(c->r));
    c->cpsr = st->cpsr;
    memcpy(c->bank_usr, st->bank_usr, sizeof(c->bank_usr));
    memcpy(c->bank_fiq, st->bank_fiq, sizeof(c->bank_fiq));
    memcpy(c->bank_svc, st->bank_svc, sizeof(c->bank_svc));
    memcpy(c->bank_abt, st->bank_abt, sizeof(c->bank_abt));
    memcpy(c->bank_irq, st->bank_irq, sizeof(c->bank_irq));
    memcpy(c->bank_und, st->bank_und, sizeof(c->bank_und));
    c->spsr_fiq = st->spsr_fiq; c->spsr_svc = st->spsr_svc; c->spsr_abt = st->spsr_abt; c->spsr_irq = st->spsr_irq; c->spsr_und = st->spsr_und;
    memcpy(c->cp15, st->cp15, sizeof(c->cp15));
    memcpy(c->tlb_va_base, st->tlb_va_base, sizeof(c->tlb_va_base));
    memcpy(c->tlb_pa_base, st->tlb_pa_base, sizeof(c->tlb_pa_base));
    memcpy(c->tlb_mask, st->tlb_mask, sizeof(c->tlb_mask));
    memcpy(c->tlb_valid, st->tlb_valid, sizeof(c->tlb_valid));
    tlb_rebuild_mirror(c);
    c->cycles_total = st->cycles_total;
    c->irq_line = st->irq_line; c->fiq_line = st->fiq_line; c->halted = st->halted;
    /* The SoC savestate loader can replace the RAM allocation after the CPU
     * object has already cached fastmem pointers for native JIT helpers.
     * Drop those cached bases so the next JIT run reacquires current RAM/BIOS
     * pointers through the bus instead of writing into freed state-load RAM. */
    c->jit_ram_base = NULL;
    c->jit_bios_base = NULL;
    arm920t_jit_invalidate_all(c, ARM_JIT_INV_STATE_LOAD);
}

int arm920t_state_load_io(arm920t_t *c, state_io_t *io) {
    if (!c || !io) return 0;
    arm920t_state_image_t st;
    if (!state_io_read(io, &st, sizeof(st))) return 0;
    arm920t_state_apply(c, &st);
    return 1;
}

int arm920t_state_save(const arm920t_t *c, FILE *f) {
    state_io_t io = state_io_file(f);
    return arm920t_state_save_io(c, &io);
}

int arm920t_state_load(arm920t_t *c, FILE *f) {
    state_io_t io = state_io_file(f);
    return arm920t_state_load_io(c, &io);
}
