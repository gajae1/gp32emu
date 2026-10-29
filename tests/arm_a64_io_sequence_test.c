/* Exact, bounded peripheral regions. The bus deliberately changes its live
 * readback on EVERY write, including equal values. The interpreter is the
 * oracle for every callback-visible register, CPSR, PC and ragged boundary.
 * The initial hot-return assertion fails on the unfused baseline, proving
 * that passing state comparisons alone cannot hide an unused optimization. */
#if (defined(__aarch64__) || defined(_M_ARM64)) && !defined(__AARCH64EB__) && \
    (!defined(GP32EMU_DISABLE_JIT) || !GP32EMU_DISABLE_JIT)

#ifndef GP32EMU_CPU_PROFILE
#define GP32EMU_CPU_PROFILE 1
#endif
#ifndef CORE_UNDER_TEST
#define CORE_UNDER_TEST "../src/arm920t.c"
#endif
#include CORE_UNDER_TEST

#define SEQ_CODE 0x400u
#define SEQ_PORT 0x15600024u
#define SEQ_READ 0x1560000cu
#define SEQ_ALIAS 0x24000000u
#define SEQ_RETURN 0x800u

typedef struct seq_observation {
    uint32_t pa, value, r[16], cpsr;
    uint64_t cycles;
} seq_observation_t;
typedef struct seq_bus {
    uint8_t bios[ARM_JIT_BIOS_SIZE_BYTES], ram[ARM_JIT_RAM_SIZE_BYTES];
    arm920t_t *cpu;
    volatile uint32_t live;
    uint32_t latch;
    uint32_t port_pa, read_pa, stop_pa;
    seq_observation_t observation[8];
    unsigned writes, hot_writes;
    uintptr_t native_begin, native_end;
    int excludes;
    unsigned decorate;
} seq_bus_t;
static seq_bus_t buses[2];
static arm920t_t *cpus[2];
static unsigned failures, cases;
static const char *case_name;
static int negative_control;
static uint32_t code_start = SEQ_CODE;
static unsigned code_count = 6u;

static void check(int ok, const char *message) {
    if (ok) return;
    if (failures < 16u) fprintf(stderr, "FAIL[%s]: %s\n", case_name, message);
    ++failures;
}
static uint8_t *seq_fastmem(void *user, uint32_t pa, size_t bytes, int write) {
    seq_bus_t *b = user;
    if (!write && pa < sizeof(b->bios) && bytes <= sizeof(b->bios) - pa) return b->bios + pa;
    uint32_t off = pa - ARM_JIT_RAM_BASE_ADDR;
    if (off < sizeof(b->ram) && bytes <= sizeof(b->ram) - off) return b->ram + off;
    return NULL;
}
static int seq_excludes(void *user, uint32_t pa, size_t bytes, int write) {
    seq_bus_t *b = user;
    uint64_t end = (uint64_t)pa + bytes;
    if (!(b->excludes & (write ? 2u : 1u)) || !bytes || end > UINT64_C(0x100000000)) return 0;
    if (!write && pa < sizeof(b->bios)) return 0;
    return pa >= ARM_JIT_RAM_BASE_ADDR + ARM_JIT_RAM_SIZE_BYTES || end <= ARM_JIT_RAM_BASE_ADDR;
}
static uint32_t seq_read32(void *user, uint32_t pa) {
    seq_bus_t *b = user;
    uint8_t *p = seq_fastmem(user, pa, 4u, 0);
    if (p) return gp32_ld32le(p);
    if (pa == b->read_pa) return b->live;
    if (pa == b->port_pa) {
        if (b->decorate==2u) {
            b->cpu->cpsr ^= N_FLAG | Z_FLAG;
            b->cpu->r[7]=0x17ac3962u;
            arm920t_stop_run(b->cpu);
        }
        return b->latch;
    }
    return UINT32_MAX;
}
static uint16_t seq_read16(void *user, uint32_t pa) { return (uint16_t)seq_read32(user, pa); }
static uint8_t seq_read8(void *user, uint32_t pa) { return (uint8_t)seq_read32(user, pa); }
static ARM_NOINLINE void seq_write32(void *user, uint32_t pa, uint32_t value) {
    seq_bus_t *b = user;
    uint8_t *p = seq_fastmem(user, pa, 4u, 1);
    if (p) { gp32_st32le(p, value); return; }
    if (b->writes < GP32_ARRAY_COUNT(b->observation)) {
        seq_observation_t *o = &b->observation[b->writes];
        o->pa = pa; o->value = value;
        memcpy(o->r, b->cpu->r, sizeof(o->r)); o->cpsr = b->cpu->cpsr;
        o->cycles = b->cpu->cycles_total;
    }
    ++b->writes;
    b->latch = value;
    b->live = (b->live << 5) ^ (b->live >> 3) ^ (value + 0x8172e35bu);
    /* Only the separate, uncertified endpoint may stop this fixture. */
    if (pa == b->stop_pa) arm920t_stop_run(b->cpu);
    if (b->decorate && b->writes == 1u) {
        /* This bus is deliberately NOT certified. The ordinary checked
         * path must commit the access and respect its changed deadline. */
        b->cpu->r[0] = 0x6f7e8d9cu;
        b->cpu->cpsr ^= N_FLAG | Z_FLAG;
        arm920t_stop_run(b->cpu);
    }
#if defined(__GNUC__) || defined(__clang__)
    uintptr_t caller = (uintptr_t)__builtin_return_address(0);
    b->hot_writes += caller >= b->native_begin && caller < b->native_end;
#endif
}
static void seq_write16(void *user, uint32_t pa, uint16_t value) { seq_write32(user, pa, value); }
static void seq_write8(void *user, uint32_t pa, uint8_t value) { seq_write32(user, pa, value); }

static const uint32_t program[] = {
    0xe5812024u, /* STR r2,[r1,#0x24] */
    0xe591000cu, /* LDR r0,[r1,#0xc] */
    0xe20000ffu, /* AND r0,r0,#0xff */
    0xe1822403u, /* ORR r2,r2,r3,LSL #8 */
    0xe5812024u, /* STR r2,[r1,#0x24] */
    0xe12fff1eu  /* BX lr */
};

static void register_certificates(unsigned certificates) {
    static arm_leaf_write32_t leaf;
    static arm_live_read32_t live;
    leaf = (arm_leaf_write32_t){buses[1].port_pa,seq_write32,&buses[1]};
    live = (arm_live_read32_t){.pa=buses[1].read_pa, .word=&buses[1].live};
    check(arm920t_set_leaf_write32(cpus[1], certificates & 1u ? &leaf : NULL,
                                 certificates & 1u ? 1u : 0u), "register leaf write");
    check(arm920t_set_live_read32(cpus[1], certificates & 2u ? &live : NULL,
                                certificates & 2u ? 1u : 0u), "register live read");
}
static void setup(unsigned certificates, unsigned mmu, unsigned equal_stores) {
    memset(buses, 0, sizeof(buses));
    code_start = SEQ_CODE; code_count = GP32_ARRAY_COUNT(program);
    for (unsigned side = 0; side < 2u; ++side) {
        seq_bus_t *b = &buses[side]; b->excludes = 3;
        b->port_pa = SEQ_PORT; b->read_pa = SEQ_READ;
        for (unsigned i = 0; i < GP32_ARRAY_COUNT(program); ++i)
            gp32_st32le(b->bios + SEQ_CODE + i * 4u, program[i]);
        gp32_st32le(b->bios + 0x18u, 0xeafffffeu);
        gp32_st32le(b->bios + 0x1cu, 0xeafffffeu);
        gp32_st32le(b->bios + SEQ_RETURN, 0xeafffffeu);
        arm_bus_t bus = {.read8=seq_read8,.read16=seq_read16,.read32=seq_read32,
            .write8=seq_write8,.write16=seq_write16,.write32=seq_write32,
            .read32_io=seq_read32,.write32_io=seq_write32,.fastmem=seq_fastmem,
            .fastmem_excludes=seq_excludes,.user=b};
        cpus[side] = b->cpu = arm920t_create(&bus);
        if (!cpus[side]) exit(2);
        arm920t_set_jit(cpus[side], side != 0u);
        for (unsigned r = 0; r < 15u; ++r) cpus[side]->r[r] = 0x10203040u + r;
        cpus[side]->r[1] = mmu ? SEQ_ALIAS : SEQ_PORT - 0x24u;
        cpus[side]->r[2] = 0xa1b20040u;
        cpus[side]->r[3] = equal_stores ? 0u : 1u;
        cpus[side]->r[14] = SEQ_RETURN;
        cpus[side]->r[15] = SEQ_CODE;
        cpus[side]->cpsr = 0xb00000d3u;
        b->live = 0x1298ac7bu;
        if (mmu) {
            cpus[side]->cp15[1] |= 1u;
            itlb_store(cpus[side], SEQ_CODE, 0u, 0xfffu);
            tlb_store_checked(cpus[side], SEQ_ALIAS, SEQ_PORT & ~0xfffu, mmu == 2u ? 0x3ffu : 0xfffu, NULL);
        }
    }
    /* Immutable until both CPUs have been destroyed, just like SoC-owned
     * descriptor storage. Only the native side consumes these certificates. */
    register_certificates(certificates);
}

static void prepare(void) {
#if ARM920T_NATIVE_BACKEND
    arm_jit_block_t *b = arm_jit_translate(cpus[1], code_start);
    check(b && b->count == code_count, "complete native fixture block");
    if (!b) return;
    if (b->native_ok >= 4u) b->native_ok = 0u;
    check(!arm_jit_lazy_compile(cpus[1], b) && b->native, "compile native block without guest warmup");
    buses[1].native_begin = (uintptr_t)b->native;
    buses[1].native_end = (uintptr_t)cpus[1]->jit_code + cpus[1]->jit_code_used;
#endif
}
static void expect_batches(unsigned batches) {
#if defined(ARM_JIT_NATIVE_A64) && ARM920T_PROFILING
    check(cpus[1]->prof.native_io_sequence_batches == batches &&
          cpus[1]->prof.native_io_sequence_insns == batches * 5u,
          "successful-region profile counts match original guest retirements");
#else
    (void)batches;
#endif
}
static void compare(void) {
    arm920t_register_context_t ref, native;
    memset(&ref, 0, sizeof(ref)); memset(&native, 0, sizeof(native));
    arm920t_get_register_context(cpus[0], &ref);
    arm920t_get_register_context(cpus[1], &native);
    check(!memcmp(&ref, &native, sizeof(ref)), "complete architectural register context");
    check(cpus[0]->cycles_total == cpus[1]->cycles_total, "cumulative guest cycles");
    check(buses[0].writes == buses[1].writes, "one callback per original store");
    check(!memcmp(buses[0].observation,buses[1].observation,sizeof(buses[0].observation)),
          "every callback sees the exact intermediate PC, registers, CPSR and value");
    check(buses[0].live == buses[1].live && buses[0].latch == buses[1].latch,
          "fresh readback and both peripheral mutations");
    check(!memcmp(buses[0].ram,buses[1].ram,sizeof(buses[0].ram)), "RAM unchanged or identical");
}
static void run_pair(unsigned budget) {
    uint32_t ref = arm920t_run(cpus[0], budget), native = arm920t_run(cpus[1], budget);
    check(ref == native && native == budget, "exact bounded retirement");
    compare();
}
static void teardown(void) {
    arm920t_destroy(cpus[0]); arm920t_destroy(cpus[1]); ++cases;
}
static void basic_cases(void) {
    for (unsigned mmu = 0; mmu < 3u; ++mmu)
      for (unsigned certificates = 0; certificates < 4u; ++certificates)
       for (unsigned equal = 0; equal < 2u; ++equal) {
        case_name = "certified-sequence";
        setup(negative_control ? 0u : certificates, mmu, equal);
        prepare(); run_pair(6u);
        check(buses[0].writes == 2u, "two strobes, including equal-value stores");
        check(cpus[0]->r[15] == SEQ_RETURN, "native suffix executed");
#if defined(__GNUC__) || defined(__clang__)
        /* Match the compiler support of the callback return-address probe.
         * Architectural comparisons and profiling checks run on every host. */
        check(buses[1].hot_writes == (certificates == 3u ? 2u : 0u),
              "both certified stores execute the native region, controls keep original path");
#endif
        expect_batches(certificates == 3u ? 1u : 0u);
        teardown();
      }
    for (unsigned first = 1u; first <= 6u; ++first) {
        case_name = "ragged-budget";
        setup(3u, 1u, first & 1u); prepare();
        run_pair(first);
        if (first < 6u) run_pair(6u - first);
        expect_batches(first == 6u ? 1u : 0u);
        check(cpus[0]->r[15] == SEQ_RETURN && buses[0].writes == 2u,
              "resume each original instruction boundary without duplicate writes");
        teardown();
    }
}

static void rejection_cases(void) {
    static const struct { unsigned index; uint32_t insn; } ineligible[] = {
        {0u,0x05812024u}, /* conditional store */
        {0u,0xe5c12024u}, /* byte store */
        {1u,0xe5d1000cu}, /* byte load */
        {1u,0xe5b1000cu}, /* load with base writeback */
        {1u,0xe591100cu}, /* loaded value overwrites shared base */
        {2u,0xe21000ffu}, /* flag-setting AND */
        {2u,0xe20040ffu}, /* AND writes another register */
        {3u,0xe1922403u}, /* flag-setting ORR */
        {3u,0xe1822423u}, /* ORR with LSR, rather than LSL */
        {3u,0xe1822313u}, /* register-controlled shift */
        {3u,0xe1822400u}, /* shift source aliases the loaded register */
        {4u,0xe5812028u}  /* final store has another address */
    };
    for (unsigned i = 0; i < GP32_ARRAY_COUNT(ineligible); ++i) {
        case_name = "ineligible-instruction-shape";
        setup(3u, 0u, 0u);
        for (unsigned side = 0; side < 2u; ++side)
            gp32_st32le(buses[side].bios + SEQ_CODE + ineligible[i].index * 4u, ineligible[i].insn);
        prepare(); run_pair(6u); expect_batches(0u); teardown();
    }
    for (unsigned permission = 0; permission < 3u; ++permission) {
        case_name = "missing-access-certificate";
        setup(3u, 0u, 0u);
        buses[1].excludes = (int)permission;
        prepare(); run_pair(6u); expect_batches(0u); teardown();
    }
    for (unsigned variant = 0; variant < 4u; ++variant) {
        case_name = "current-address-rejection";
        setup(3u, variant >= 2u, 0u); prepare();
        for (unsigned side = 0; side < 2u; ++side) {
            if (variant == 0u) cpus[side]->r[1] += 4u; /* exact endpoint changed */
            if (variant == 1u) cpus[side]->r[1] += 1u; /* unaligned original VA */
            if (variant == 2u) { /* live alias now maps to ordinary RAM */
                tlb_store_checked(cpus[side], SEQ_ALIAS, ARM_JIT_RAM_BASE_ADDR, 0xfffu, NULL);
                gp32_st32le(buses[side].ram + 0x0cu, 0x71625344u);
            }
            if (variant == 3u) { /* stale PA is not a valid data translation */
                unsigned slot = (SEQ_ALIAS >> 12) & 0xfffu;
                cpus[side]->tlb_valid[slot] = 0u;
                cpus[side]->tlb_entry[slot].valid = 0u;
            }
        }
        run_pair(6u); expect_batches(0u); teardown();
    }
    case_name = "code-watch-installed-after-compile";
    setup(3u, 0u, 0u); prepare();
    buses[1].latch = buses[0].latch = 0xe3a07003u;
    (void)arm_jit_translate(cpus[1], SEQ_PORT);
    check((cpus[1]->jit_code_nonram & ARM_JIT_NONRAM_OTHER) != 0u,
          "real later peripheral fetch installs code watch");
    run_pair(6u); expect_batches(0u); teardown();

    case_name = "decorated-bus-without-opt-in";
    setup(0u, 0u, 0u); prepare();
    buses[0].decorate = buses[1].decorate = 1u;
    uint32_t ref = arm920t_run(cpus[0], 6u), native = arm920t_run(cpus[1], 6u);
    check(ref == 1u && native == 1u, "callback deadline ends after first store");
    compare(); expect_batches(0u);
    run_pair(5u); expect_batches(0u); teardown();

    for (unsigned fiq = 0; fiq < 2u; ++fiq) {
        case_name = fiq ? "pending-FIQ" : "pending-IRQ";
        setup(3u, 0u, 0u); prepare();
        for (unsigned side = 0; side < 2u; ++side) {
            cpus[side]->cpsr &= ~(fiq ? F_FLAG : I_FLAG);
            if (fiq) cpus[side]->fiq_line = 1; else cpus[side]->irq_line = 1;
        }
        run_pair(6u); expect_batches(0u);
        check(!buses[0].writes, "deliverable interrupt precedes region entry"); teardown();
    }
    case_name = "certificate-revocation";
    setup(3u, 0u, 0u); prepare(); register_certificates(0u);
    prepare(); run_pair(6u); expect_batches(0u); teardown();

    case_name = "notified-instruction-change";
    setup(3u, 0u, 0u); prepare();
    for (unsigned side = 0; side < 2u; ++side) {
        gp32_st32le(buses[side].bios + SEQ_CODE + 8u, 0xe200000fu); /* AND r0,r0,#0xf */
        arm920t_invalidate_icache_range(cpus[side], SEQ_CODE + 8u, 4u);
    }
    prepare(); run_pair(6u); expect_batches(1u);
    check(cpus[0]->r[0] <= 15u, "changed instruction takes effect before fresh region execution");
    teardown();
}

static void boundary_cases(void) {
    case_name = "semantic-registers-offsets-and-immediates";
    setup(3u, 0u, 0u);
    const uint32_t renamed[] = {
        0xe5045024u, /* STR r5,[r4,#-0x24] */
        0xe514603cu, /* LDR r6,[r4,#-0x3c] */
        0xe2066a0fu, /* AND r6,r6,#0xf000 */
        0xe1855f87u, /* ORR r5,r5,r7,LSL #31 */
        0xe5045024u, /* STR r5,[r4,#-0x24] */
        0xe12fff1eu
    };
    for (unsigned side = 0; side < 2u; ++side) {
        for (unsigned i = 0; i < GP32_ARRAY_COUNT(renamed); ++i)
            gp32_st32le(buses[side].bios + SEQ_CODE + i * 4u, renamed[i]);
        cpus[side]->r[4] = SEQ_PORT + 0x24u;
        cpus[side]->r[5] = 0x12345678u;
        cpus[side]->r[7] = 1u;
    }
    prepare(); run_pair(6u); expect_batches(1u);
    check(cpus[0]->r[5] == 0x92345678u && !(cpus[0]->r[6] & ~0xf000u),
          "semantic dataflow accepts renamed registers, subtract offsets and rotated immediates");
    teardown();

    /* Distinct data TLB slots: proving the store's current mapping says
     * nothing about the load, including when a remap occurs after compile. */
    for (unsigned remap = 0; remap < 3u; ++remap) {
        case_name = "independent-data-mappings";
        setup(0u, 1u, 0u);
        for (unsigned side = 0; side < 2u; ++side) {
            cpus[side]->r[1] = SEQ_ALIAS + 0xfe0u;
            buses[side].port_pa = 0x15601004u;
            buses[side].read_pa = 0x15600fecu;
            tlb_store_checked(cpus[side], SEQ_ALIAS + 0x1000u, 0x15601000u, 0xfffu, NULL);
        }
        register_certificates(3u); prepare();
        if (remap) for (unsigned side = 0; side < 2u; ++side) {
            uint32_t pa = remap == 1u ? ARM_JIT_RAM_BASE_ADDR : 0x15700000u;
            tlb_store_checked(cpus[side], SEQ_ALIAS, pa, 0xfffu, NULL);
            gp32_st32le(buses[side].ram + 0xfecu, 0x11223344u);
        }
        run_pair(6u); expect_batches(!remap); teardown();
    }
    case_name = "prefix-and-native-suffix";
    setup(3u, 0u, 0u);
    for (unsigned side = 0; side < 2u; ++side) {
        memmove(buses[side].bios + SEQ_CODE + 8u, buses[side].bios + SEQ_CODE, sizeof(program));
        gp32_st32le(buses[side].bios + SEQ_CODE, 0xe3a06035u); /* MOV r6,#0x35 */
        gp32_st32le(buses[side].bios + SEQ_CODE + 4u, 0xe3560035u); /* CMP r6,#0x35 */
    }
    code_count = 8u; prepare(); run_pair(8u); expect_batches(1u); teardown();

    for (unsigned taken = 0; taken < 2u; ++taken) {
        case_name = "forward-entry-into-middle";
        setup(3u, 0u, 0u);
        for (unsigned side = 0; side < 2u; ++side) {
            memmove(buses[side].bios + SEQ_CODE + 8u, buses[side].bios + SEQ_CODE, sizeof(program));
            gp32_st32le(buses[side].bios + SEQ_CODE, 0xe3540000u); /* CMP r4,#0 */
            gp32_st32le(buses[side].bios + SEQ_CODE + 4u, 0x0a000001u); /* BEQ to AND */
            cpus[side]->r[4] = taken ? 0u : 1u;
        }
        code_count = 8u; prepare(); run_pair(8u); expect_batches(!taken);
        check(buses[0].writes == (taken ? 1u : 2u), "middle entry retains original suffix"); teardown();
    }
    case_name = "region-at-block-end";
    setup(3u, 0u, 0u);
    for (unsigned side = 0; side < 2u; ++side) {
        for (unsigned i = 0; i < ARM_JIT_MAX_INSNS - 5u; ++i)
            gp32_st32le(buses[side].bios + SEQ_CODE + i * 4u, 0xe1a04004u); /* MOV r4,r4 */
        for (unsigned i = 0; i < GP32_ARRAY_COUNT(program); ++i)
            gp32_st32le(buses[side].bios + SEQ_CODE + (ARM_JIT_MAX_INSNS - 5u + i) * 4u, program[i]);
    }
    code_count = ARM_JIT_MAX_INSNS;
    prepare(); run_pair(ARM_JIT_MAX_INSNS); expect_batches(1u);
    check(cpus[0]->r[15] == SEQ_CODE + ARM_JIT_MAX_INSNS * 4u, "decode-limit region keeps exact PC"); teardown();

    case_name = "instruction-page-boundary-rejection";
    setup(3u, 1u, 0u); code_start = 0x3f8u;
    for (unsigned side = 0; side < 2u; ++side) {
        for (unsigned i = 0; i < GP32_ARRAY_COUNT(program); ++i)
            gp32_st32le(buses[side].bios + code_start + i * 4u, program[i]);
        cpus[side]->r[15] = code_start;
    }
    prepare(); run_pair(6u); expect_batches(0u);
    check(cpus[0]->r[15] == SEQ_RETURN, "cross-page sequence retains original return"); teardown();
}



static void ram_update_callback_cases(void) {
    const uint32_t update[]={0xe5912024u,0xe2822001u,0xe5812024u,0xe12fff1eu};
    for(unsigned stop=1;stop<=2u;++stop) {
        case_name="RAM-region-fallback-callback-deadline";
        setup(0u,0u,0u);code_count=4u;
        for(unsigned side=0;side<2u;++side) {
            for(unsigned i=0;i<4u;++i) gp32_st32le(buses[side].bios+SEQ_CODE+i*4u,update[i]);
            buses[side].decorate=stop;
            buses[side].latch=0x1786a3ffu;
        }
        prepare();
        uint32_t ref=arm920t_run(cpus[0],4u),native=arm920t_run(cpus[1],4u);
        check(ref==native && ref==(stop==2u ? 1u : 3u),"stop exactly at the load or store callback");
        compare();
        buses[0].decorate=buses[1].decorate=0u;
        run_pair(4u-ref);
        check(cpus[0]->r[15]==SEQ_RETURN,"resume after callback without duplicate operation");
        teardown();
    }
}

static void ram_update_control_cases(void) {
    const uint32_t update[]={0xe5912024u,0xe2822001u,0xe5812024u,0xe12fff1eu};
    for (unsigned taken=0;taken<2u;++taken) {
        case_name="RAM-update-middle-entry";
        setup(0u,0u,0u);code_count=6u;
        for(unsigned side=0;side<2u;++side) {
            cpus[side]->r[1]=ARM_JIT_RAM_BASE_ADDR+0x1000u;
            cpus[side]->r[4]=taken ? 0u : 1u;
            gp32_st32le(buses[side].ram+0x1024u,7u);
            gp32_st32le(buses[side].bios+SEQ_CODE,0xe3540000u);
            gp32_st32le(buses[side].bios+SEQ_CODE+4u,0x0a000000u);
            for(unsigned i=0;i<4u;++i) gp32_st32le(buses[side].bios+SEQ_CODE+8u+i*4u,update[i]);
        }
        prepare();run_pair(taken ? 5u : 6u);
        check(cpus[0]->r[15]==SEQ_RETURN,"middle entry uses original update and store");
        teardown();
    }
    case_name="RAM-update-watch-after-compile";
    setup(0u,0u,0u);code_count=4u;
    for(unsigned side=0;side<2u;++side) {
        cpus[side]->r[1]=ARM_JIT_RAM_BASE_ADDR+0x1000u;
        gp32_st32le(buses[side].ram+0x1024u,0xe3a04001u); /* MOV r4,#1 -> #2 */
        gp32_st32le(buses[side].ram+0x1028u,0xeafffffeu);
        for(unsigned i=0;i<4u;++i) gp32_st32le(buses[side].bios+SEQ_CODE+i*4u,update[i]);
    }
    prepare();
    check(arm_jit_translate(cpus[1],ARM_JIT_RAM_BASE_ADDR+0x1024u)!=NULL,"watch a real later instruction fetch");
    run_pair(4u);
    for(unsigned side=0;side<2u;++side) cpus[side]->r[15]=ARM_JIT_RAM_BASE_ADDR+0x1024u;
    run_pair(1u);
    check(cpus[0]->r[4]==2u && cpus[1]->r[4]==2u,"updated code executes after guarded fallback");
    teardown();
}

static void ram_update_skipped_prefix_cases(void) {
    const uint32_t words[] = {
        0xe3540000u, 0x0a000001u, /* CMP r4,#0; BEQ to LDR (skip two ops) */
        0xe3a07001u, 0xe3a07002u,
        0xe5912024u, 0xe2822001u, 0xe5812024u,
        0xe5886024u, 0xe12fff1eu /* observable store stops, then BX lr */
    };
    for (unsigned wide = 0; wide < 2u; ++wide) {
        case_name = "RAM-update-skipped-prefix-budget";
        setup(0u, 0u, 0u); code_count = GP32_ARRAY_COUNT(words);
        for (unsigned side = 0; side < 2u; ++side) {
            for (unsigned i = 0; i < code_count; ++i)
                gp32_st32le(buses[side].bios + SEQ_CODE + i * 4u, words[i]);
            cpus[side]->r[1] = ARM_JIT_RAM_BASE_ADDR + 0x1000u;
            cpus[side]->r[4] = 0u;
            cpus[side]->r[8] = SEQ_PORT - 0x24u;
            gp32_st32le(buses[side].ram + 0x1024u, UINT32_MAX);
            buses[side].decorate = 1u;
        }
        prepare();
        uint32_t budget = wide ? UINT32_MAX : code_count;
        uint32_t ref = arm920t_run(cpus[0], budget);
        uint32_t native = arm920t_run(cpus[1], budget);
        check(ref == 6u && native == ref, "skipped instructions never retire, even after unsigned refund wrap");
        compare();
        check(cpus[1]->r[2] == 0u && gp32_ld32le(buses[1].ram + 0x1024u) == 0u,
              "RAM addition wraps once before the observing callback");
        buses[0].decorate = buses[1].decorate = 0u;
        run_pair(1u);
        check(cpus[1]->r[15] == SEQ_RETURN, "resume at BX without repeating either store");
        teardown();
    }
}

static void ram_update_cases(void) {
    for (unsigned variant=0;variant<10u;++variant)
      for (unsigned first=1;first<=4u;++first) {
        case_name="RAM-load-update-store";
        setup(0u,variant % 3u,0u);
        code_count=4u;
        const uint32_t words[]={0xe5912024u,0xe2822001u,0xe5812024u,0xe12fff1eu};
        for(unsigned side=0;side<2u;++side) {
            arm920t_t *c=cpus[side];
            for(unsigned i=0;i<4u;++i) gp32_st32le(buses[side].bios+SEQ_CODE+i*4u,words[i]);
            c->r[1]=(variant%3u ? SEQ_ALIAS : ARM_JIT_RAM_BASE_ADDR)+0x1000u;
            if(variant%3u) tlb_store_checked(c,c->r[1],ARM_JIT_RAM_BASE_ADDR+0x1000u,
                variant%3u==2u ? 0x3ffu : 0xfffu,NULL);
            gp32_st32le(buses[side].ram+0x1024u,0xffffffffu);
            if(variant==3u) c->r[1]+=1u; /* unaligned rotate + store */
            if(variant==4u) { /* current TLB now resolves to a different word */
                tlb_store_checked(c,c->r[1],ARM_JIT_RAM_BASE_ADDR+0x2000u,0xfffu,NULL);
                gp32_st32le(buses[side].ram+0x2024u,0x7fffffffu);
            }
            if(variant==5u) { /* ordinary I/O must retain each callback */
                tlb_store_checked(c,c->r[1],SEQ_PORT&~0xfffu,0xfffu,NULL);
                buses[side].latch=0x17263544u;
            }
            if(variant==6u) gp32_st32le(buses[side].bios+SEQ_CODE+4u,0xe2422001u); /* SUB */
            if(variant==7u) gp32_st32le(buses[side].bios+SEQ_CODE+4u,0xe2822401u); /* large ADD */
            if(variant==8u) gp32_st32le(buses[side].bios+SEQ_CODE+4u,0xe2922001u); /* ADDS excluded */
            if(variant==9u) gp32_st32le(buses[side].bios+SEQ_CODE+4u,0x02822001u); /* conditional excluded */
        }
        prepare();run_pair(first);
        if(first<4u)run_pair(4u-first);
        check(cpus[0]->r[15]==SEQ_RETURN,"RAM update returns after every ragged boundary");
        teardown();
      }
}

static void io_sequence_skipped_prefix_cases(void) {
    for (unsigned wide = 0; wide < 2u; ++wide) {
        case_name = "IO-sequence-skipped-prefix-budget";
        setup(3u, 0u, 0u);
        code_count = 4u + GP32_ARRAY_COUNT(program);
        for (unsigned side = 0; side < 2u; ++side) {
            seq_bus_t *b = &buses[side];
            memmove(b->bios + SEQ_CODE + 16u, b->bios + SEQ_CODE, sizeof(program));
            gp32_st32le(b->bios + SEQ_CODE, 0xe3540000u); /* CMP r4,#0 */
            gp32_st32le(b->bios + SEQ_CODE + 4u, 0x0a000001u); /* skip two MOVs */
            gp32_st32le(b->bios + SEQ_CODE + 8u, 0xe3a07001u);
            gp32_st32le(b->bios + SEQ_CODE + 12u, 0xe3a07002u);
            cpus[side]->r[4] = 0u;
            b->stop_pa = SEQ_PORT + 4u;
            gp32_st32le(b->bios + SEQ_RETURN, 0xe5812028u); /* uncertified stop */
            gp32_st32le(b->bios + SEQ_RETURN + 4u, 0xe12fff1eu);
        }
        prepare();
        uint32_t budget = wide ? UINT32_MAX : code_count;
        uint32_t ref = arm920t_run(cpus[0], budget);
        uint32_t native = arm920t_run(cpus[1], budget);
        check(ref == 9u && native == ref, "only executed prefix, region, return and stop retire");
        compare();
        check(buses[1].writes == 3u, "both certified writes precede the checked stop");
        expect_batches(1u);
        teardown();
    }
}

static void io_sequence_wrapped_base_cases(void) {
    for (unsigned changed = 0; changed < 2u; ++changed) {
        case_name = "IO-sequence-wrapped-identity-base";
        setup(0u, 0u, 0u);
        for (unsigned side = 0; side < 2u; ++side) {
            seq_bus_t *b = &buses[side];
            cpus[side]->r[1] = 0x10u;
            b->port_pa = 0xffffffecu;
            b->read_pa = 0xffffffd4u;
            gp32_st32le(b->bios + SEQ_CODE, 0xe5012024u); /* STR r2,[r1,-0x24] */
            gp32_st32le(b->bios + SEQ_CODE + 4u, 0xe511003cu); /* LDR r0,[r1,-0x3c] */
            gp32_st32le(b->bios + SEQ_CODE + 16u, 0xe5012024u);
        }
        register_certificates(3u); prepare();
        if (changed) cpus[0]->r[1] = cpus[1]->r[1] = 0x14u;
        run_pair(6u);
        expect_batches(!changed);
        check(buses[1].writes == 2u && buses[1].observation[0].pa ==
              (changed ? 0xfffffff0u : 0xffffffecu), "current base keeps wrapped store address");
        if (changed) check(cpus[1]->r[0] == 0xffu, "changed base uses ordinary unmapped read");
        teardown();
    }
}

int main(int argc, char **argv) {
    negative_control = argc > 1 && !strcmp(argv[1], "--negative-control");
    ram_update_callback_cases();
    ram_update_control_cases();
    ram_update_skipped_prefix_cases();
    ram_update_cases();
    io_sequence_skipped_prefix_cases();
    io_sequence_wrapped_base_cases();
    basic_cases();
    rejection_cases();
    boundary_cases();
    printf("arm_a64_io_sequence_test: %u cases, %u failures, negative=%d\n", cases, failures, negative_control);
    return failures != 0u;
}

#else
#include <stdio.h>
int main(void) {
    puts("SKIP: certified peripheral-region execution requires the A64 backend");
    return 77;
}
#endif
