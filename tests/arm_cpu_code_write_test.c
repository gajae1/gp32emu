/* A decoded ARM block is not a guest instruction cache. With CP15.I clear,
 * CPU writes must make later instruction fetches observe the new RAM word.
 * The traced/raw CPU is the oracle: jit=0 alone still uses decoded blocks. */
#define main original_jit_main
#include "arm_jit_test.c"
#undef main

static unsigned cases;

enum store_shape { WORD, BYTE, HALF, SWAP_WORD, SWAP_BYTE, BLOCK, BLOCK_EDGE, HALF_EDGE,
                   BLOCK_LINE, HALF_LINE, BLOCK_INTERIOR, BLOCK_FRAGMENT0, BLOCK_FRAGMENT1 };
static const uint32_t stores[] = {
    0xe4871004u, /* STR r1,[r7],#4 */
    0xe4c71001u, /* STRB r1,[r7],#1 */
    0xe0c710b2u, /* STRH r1,[r7],#2 */
    0xe1073091u, /* SWP r3,r1,[r7] */
    0xe1473091u, /* SWPB r3,r1,[r7] */
    0xe8a70006u, /* STMIA r7!,{r1,r2} */
    0xe8a70006u,
    0xe0c710b2u,
    0xe8a70006u,
    0xe0c710b2u,
    0xe8a7001eu, /* four words: only interior words contain decoded code */
    0xe8a7007eu, /* six words crossing a page, code inside fragment 0 */
    0xe8a7007eu  /* six words crossing a page, code inside fragment 1 */
};

static void map_code_and_data(uint32_t code, uint32_t data, uint32_t pa) {
    for (arm920t_t *c = cpu_jit; c; c = c == cpu_jit ? cpu_ref : NULL) {
        c->cp15[1] |= 1u;
        itlb_store(c, 0u, 0u, 0xfffu);
        for (unsigned page = 0; page < 2; ++page) {
            itlb_store(c, code - 0x1000u + page * 0x1000u,
                       pa - 0x1000u + page * 0x1000u, 0xfffu);
            tlb_store_checked(c, data - 0x1000u + page * 0x1000u,
                              pa - 0x1000u + page * 0x1000u, 0xfffu, NULL);
        }
    }
}

static void prime_target_run(uint32_t target, int native, uint32_t cycles) {
    set_reg_both(12u, CODE_ADDR + 0x100u);
    set_reg_both(15u, target);
    if (native) prepare_native_block(cpu_jit, target);
    CHECK(arm920t_run(cpu_jit, cycles) == arm920t_run(cpu_ref, cycles), "prime budget");
    CHECK(cpu_jit->r[0] == 0x11u && cpu_ref->r[0] == 0x11u, "original code executed");
}
static void prime_target(uint32_t target, int native) { prime_target_run(target, native, 2u); }

static void case_store(unsigned shape, int native, int mmu, int cache, int separate_pages) {
    char name[96];
    snprintf(name, sizeof(name), "CPU code write shape=%u native=%d MMU=%d I=%d split_PA=%d",
             shape, native, mmu, cache, separate_pages);
    current_case = name;
    setup_pair();
    arm920t_set_jit(cpu_jit, native);
    arm920t_set_trace(cpu_ref, 1, NULL, NULL);
    const uint32_t within = shape == BLOCK_FRAGMENT0 ? 0u - 8u : shape == BLOCK_FRAGMENT1 ? 4u :
                            shape >= BLOCK_LINE ? 0x80u : 0u;
    const uint32_t pa = RAM_BASE + (separate_pages && shape == BLOCK_FRAGMENT1 ? 0x19000u : 0x9000u) + within;
    const uint32_t code = mmu ? 0x11009000u + within : pa;
    const uint32_t data = mmu ? 0x22009000u + within : pa;
    if (mmu) map_code_and_data(code - within, data - within, pa - within);
    if (separate_pages) {
        /* The data span crosses adjacent virtual pages backed by separated
         * physical pages. The instruction alias still maps the watched word.
         * Checking offset0+0x1000 instead of resolved offset1 misses its code. */
        for (arm920t_t *c = cpu_jit; c; c = c == cpu_jit ? cpu_ref : NULL) {
            if (shape == BLOCK_FRAGMENT0)
                tlb_store_checked(c, data - within, RAM_BASE + 0x19000u, 0xfffu, NULL);
            else
                tlb_store_checked(c, data - within - 0x1000u, RAM_BASE + 0x8000u, 0xfffu, NULL);
        }
    }
    if (cache) { cpu_jit->cp15[1] |= 1u << 12; cpu_ref->cp15[1] |= 1u << 12; }
    const uint32_t program[] = { stores[shape], 0xe1a0f008u }; /* store; MOV pc,r8 */
    load_both(program, GP32_ARRAY_COUNT(program));
    set_mem_both(pa - 4u, 0xe1a00000u);
    set_mem_both(pa, 0xe3a00011u);
    set_mem_both(pa + 4u, 0xe12fff1cu); /* BX r12 */
    prime_target(code, native);
    uint32_t address = data;
    uint32_t value = 0xe3a00022u, next = 0xe12fff1cu;
    if (shape == BYTE || shape == SWAP_BYTE) value = 0x22u;
    if (shape == HALF) value = 0x22u;
    if (shape == BLOCK_EDGE || shape == BLOCK_LINE) { address -= 4u; value = 0xe1a00000u; next = 0xe3a00022u; }
    if (shape == HALF_EDGE || shape == HALF_LINE) { --address; value = 0x22e1u; }
    if (shape == BLOCK_INTERIOR || shape == BLOCK_FRAGMENT0) {
        address -= 4u; value = 0xe1a00000u; next = 0xe3a00022u;
        set_reg_both(3u, 0xe12fff1cu); set_reg_both(4u, 0x12345678u);
    }
    if (shape == BLOCK_FRAGMENT1) {
        address -= 8u; value = next = 0xe1a00000u;
        set_reg_both(3u, 0xe3a00022u); set_reg_both(4u, 0xe12fff1cu);
    }
    set_reg_both(1u, value); set_reg_both(2u, next);
    set_reg_both(7u, address); set_reg_both(8u, code); set_reg_both(15u, CODE_ADDR);
    if (native) prepare_native_block(cpu_jit, CODE_ADDR);
    uint32_t epoch = cpu_jit->jit_cache_epoch;
    arm920t_reset_cpu_profile(cpu_jit);
    CHECK(arm920t_run(cpu_jit, 12u) == arm920t_run(cpu_ref, 12u), "store/call budget");
    CHECK(gp32_ld32le(bus_jit.ram + pa - RAM_BASE) == 0xe3a00022u, "CPU really replaced instruction in RAM");
    if (!cache) {
        CHECK(cpu_jit->r[0] == 0x22u, "next call executes replaced instruction");
        compare_state();
    } else {
        CHECK(cpu_jit->jit_cache_epoch == epoch, "I-enabled CPU write retains existing maintenance policy");
        CHECK(cpu_jit->r[0] == 0x11u, "I-enabled decoded code remains until guest maintenance");
        cp15_write(cpu_jit, 7u, 5u, 0u, 0u, 0u);
        cpu_jit->r[15] = code;
        CHECK(arm920t_run(cpu_jit, 2u) == 2u && cpu_jit->r[0] == 0x22u,
              "guest I-cache maintenance reveals replacement");
    }
#if ARM920T_NATIVE_BACKEND
    if (native) CHECK(cpu_jit->prof.native_arm_insns != 0u, "fixture executed native ARM");
#endif
    teardown_pair();
    ++cases;
}

/* The replacement is already part of the active native/portable trace.
 * Invalidating only the next block lookup would still execute the old MOV. */
static void case_active_trace(int native) {
    current_case = native ? "active native trace code write" : "active portable trace code write";
    setup_pair(); arm920t_set_jit(cpu_jit, native); arm920t_set_trace(cpu_ref, 1, NULL, NULL);
    const uint32_t pc = RAM_BASE + 0x10000u;
    const uint32_t program[] = {
        0xe4871004u, 0xe1a02002u, 0xe1a03003u, 0xe1a04004u,
        0xe3a00011u, 0xe12fff1cu
    };
    for (unsigned i = 0; i < GP32_ARRAY_COUNT(program); ++i) set_mem_both(pc + i * 4u, program[i]);
    set_reg_both(1u, 0xe3a00022u); set_reg_both(7u, pc + 16u);
    set_reg_both(12u, CODE_ADDR + 0x100u); set_reg_both(15u, pc);
    if (native) prepare_native_block(cpu_jit, pc);
    CHECK(arm920t_run(cpu_jit, 12u) == arm920t_run(cpu_ref, 12u), "active trace budget");
    CHECK(cpu_jit->r[0] == 0x22u && cpu_jit->r[7] == pc + 20u,
          "store completes writeback then exits stale trace");
    compare_state(); teardown_pair(); ++cases;
}

static void case_raw_writer(int use_thumb) {
    current_case = use_thumb ? "Thumb writer invalidates cached ARM" : "raw ARM writer invalidates cached ARM";
    setup_pair(); arm920t_set_trace(cpu_ref, 1, NULL, NULL);
    const uint32_t target = RAM_BASE + 0x9000u;
    set_mem_both(target, 0xe3a00011u); set_mem_both(target + 4u, 0xe12fff1cu);
    prime_target(target, 1);
    set_mem_both(CODE_ADDR, use_thumb ? 0x46c06039u : 0xe5871000u); /* STR r1,[r7]; Thumb STR r1,[r7,#0] */
    set_reg_both(1u, 0xe3a00022u); set_reg_both(7u, target); set_reg_both(15u, CODE_ADDR);
    if (use_thumb) { cpu_jit->cpsr |= T_FLAG; cpu_ref->cpsr |= T_FLAG; }
    else arm920t_set_trace(cpu_jit, 1, NULL, NULL);
    CHECK(arm920t_run(cpu_jit, 1u) == arm920t_run(cpu_ref, 1u), "raw store budget");
    arm920t_set_trace(cpu_jit, 0, NULL, NULL);
    cpu_jit->cpsr &= ~T_FLAG; cpu_ref->cpsr &= ~T_FLAG;
    set_reg_both(15u, target);
    CHECK(arm920t_run(cpu_jit, 2u) == arm920t_run(cpu_ref, 2u), "call after raw writer budget");
    CHECK(cpu_jit->r[0] == 0x22u, "raw CPU store revokes cached ARM code");
    compare_state(); teardown_pair(); ++cases;
}

static void case_data_store(int native, int mmu, int same_page) {
    current_case = "ordinary CPU data writes preserve translated blocks";
    setup_pair(); arm920t_set_jit(cpu_jit, native); arm920t_set_trace(cpu_ref, 1, NULL, NULL);
    const uint32_t target = RAM_BASE + 0x9000u;
    if (mmu) map_code_and_data(0x11009000u, 0x22009000u, target);
    const uint32_t program[] = {0xe4871004u, 0xeafffffeu};
    load_both(program, GP32_ARRAY_COUNT(program));
    set_mem_both(target, 0xe3a00011u); set_mem_both(target + 4u, 0xe12fff1cu);
    prime_target(mmu ? 0x11009000u : target, native);
    if (mmu) for (arm920t_t *c = cpu_jit; c; c = c == cpu_jit ? cpu_ref : NULL)
        tlb_store_checked(c, 0x2200a000u, target + 0x1000u, 0xfffu, NULL);
    uint32_t data = same_page == 2 ? target + 0x20u : same_page ? target + 0x200u : target + 0x1000u;
    if (mmu) data += 0x22009000u - target;
    set_reg_both(1u, 0x12345678u); set_reg_both(7u, data);
    set_reg_both(15u, CODE_ADDR);
    if (native) prepare_native_block(cpu_jit, CODE_ADDR);
    uint32_t epoch = cpu_jit->jit_cache_epoch, generation = cpu_jit->jit_generation;
    CHECK(arm920t_run(cpu_jit, 10u) == arm920t_run(cpu_ref, 10u), "data store budget");
    CHECK(cpu_jit->jit_cache_epoch == epoch && cpu_jit->jit_generation == generation,
          "unrelated data causes no code invalidation, including elsewhere on a code page");
    compare_state(); teardown_pair(); ++cases;
}

/* A changed I mapping can preserve the decoded/native block when its bytes
 * are identical. The replacement physical page must join the code-write
 * watch before a CPU write through a separate D alias changes it. */
static void case_remapped_code(int native, int long_span) {
    current_case = native ? "revalidated native code tracks new physical page" : "revalidated portable code tracks new physical page";
    setup_pair(); arm920t_set_jit(cpu_jit, native); arm920t_set_trace(cpu_ref, 1, NULL, NULL);
    const uint32_t pa = RAM_BASE + 0x9000u, replacement = RAM_BASE + 0x19000u;
    const uint32_t code = 0x11009000u, data = 0x22009000u;
    const uint32_t words = long_span ? 48u : 2u, patch = long_span ? 64u : 0u;
    map_code_and_data(code, data, pa);
    for (unsigned at = 0; at < 2; ++at) {
        uint32_t p = at ? replacement : pa;
        for (unsigned word = 0; word < words; ++word)
            set_mem_both(p + word * 4u, word == words - 1u ? 0xe12fff1cu : 0xe1a02002u);
        set_mem_both(p, 0xe3a00011u); set_mem_both(p + patch, 0xe3a00011u);
    }
    const uint32_t program[] = {stores[WORD], 0xe1a0f008u};
    load_both(program, GP32_ARRAY_COUNT(program));
    prime_target_run(code, native, words);
    uint64_t compilations = cpu_jit->prof.jit_blocks_compiled;
    uint32_t generation = cpu_jit->jit_generation;
    for (arm920t_t *c = cpu_jit; c; c = c == cpu_jit ? cpu_ref : NULL) {
        itlb_store(c, code, replacement, 0xfffu);
        tlb_store_checked(c, data, replacement, 0xfffu, NULL);
    }
    set_reg_both(15u, code);
    CHECK(arm920t_run(cpu_jit, words) == arm920t_run(cpu_ref, words), "revalidated call budget");
    CHECK(cpu_jit->prof.jit_blocks_compiled == compilations && cpu_jit->jit_generation == generation,
          "identical remap reuses existing decoded code");
    set_reg_both(1u, 0xe3a00022u); set_reg_both(7u, data + patch); set_reg_both(8u, code);
    set_reg_both(15u, CODE_ADDR);
    if (native) prepare_native_block(cpu_jit, CODE_ADDR);
    CHECK(arm920t_run(cpu_jit, words + 10u) == arm920t_run(cpu_ref, words + 10u), "write after remap budget");
    CHECK(cpu_jit->r[0] == 0x22u, "write to revalidated physical page invalidates old code");
    CHECK(gp32_ld32le(bus_jit.ram + pa + patch - RAM_BASE) == 0xe3a00011u,
          "write targets new physical page only");
    compare_state(); teardown_pair(); ++cases;
}

static void split_write16(void *user, uint32_t addr, uint16_t value) {
    /* Same unaligned boundary behavior as s3c2400_write16. */
    tb_write8(user, addr, (uint8_t)value);
    tb_write8(user, addr + 1u, (uint8_t)(value >> 8));
}

static void case_write_enters_ram(int native) {
    current_case = "halfword CPU write begins below RAM and changes first code byte";
    setup_pair(); arm920t_set_jit(cpu_jit, native); arm920t_set_trace(cpu_ref, 1, NULL, NULL);
    cpu_jit->bus.write16 = split_write16; cpu_ref->bus.write16 = split_write16;
    const uint32_t program[] = {stores[HALF], 0xe1a0f008u};
    load_both(program, GP32_ARRAY_COUNT(program));
    set_mem_both(RAM_BASE, 0xe3a00011u); set_mem_both(RAM_BASE + 4u, 0xe12fff1cu);
    prime_target(RAM_BASE, native);
    set_reg_both(1u, 0x22ffu); set_reg_both(7u, RAM_BASE - 1u);
    set_reg_both(8u, RAM_BASE); set_reg_both(15u, CODE_ADDR);
    if (native) prepare_native_block(cpu_jit, CODE_ADDR);
    CHECK(arm920t_run(cpu_jit, 12u) == arm920t_run(cpu_ref, 12u), "RAM-entering halfword budget");
    CHECK(gp32_ld32le(bus_jit.ram) == 0xe3a00022u && cpu_jit->r[0] == 0x22u,
          "partial boundary store revokes first RAM code page");
    compare_state(); teardown_pair(); ++cases;
}

/* The native watch guard bakes in CP15.I. Execute a real guest c1 write after
 * compiling an I-enabled store, then prime the target in the new I=0 state.
 * Reusing that store's old unguarded native body would leave the target stale. */
static void case_disable_icache(int native) {
    current_case = "guest MCR disabling I-cache revokes compiled store assumptions";
    setup_pair(); arm920t_set_jit(cpu_jit, native); arm920t_set_trace(cpu_ref, 1, NULL, NULL);
    cpu_jit->cp15[1] |= 1u << 12; cpu_ref->cp15[1] |= 1u << 12;
    const uint32_t target = RAM_BASE + 0x9000u;
    const uint32_t program[] = {stores[WORD], 0xe1a0f008u};
    load_both(program, GP32_ARRAY_COUNT(program));
    set_mem_both(CODE_ADDR + 0x200u, 0xee019f10u); /* MCR p15,0,r9,c1,c0,0 */
    set_mem_both(target, 0xe3a00011u); set_mem_both(target + 4u, 0xe12fff1cu);
    if (native) prepare_native_block(cpu_jit, CODE_ADDR);
    else CHECK(arm_jit_translate(cpu_jit, CODE_ADDR) != NULL, "prepare I-enabled decoded store");
    set_reg_both(9u, 0x70u); set_reg_both(15u, CODE_ADDR + 0x200u);
    CHECK(arm920t_run(cpu_jit, 1u) == arm920t_run(cpu_ref, 1u), "guest c1 write budget");
    CHECK(cpu_jit->cp15[1] == 0x70u, "guest really disabled instruction cache");
    prime_target(target, native);
    set_reg_both(1u, 0xe3a00022u); set_reg_both(7u, target); set_reg_both(8u, target);
    set_reg_both(15u, CODE_ADDR);
    CHECK(arm920t_run(cpu_jit, 12u) == arm920t_run(cpu_ref, 12u), "write after disabling I-cache budget");
    CHECK(cpu_jit->r[0] == 0x22u, "store now participates in I-disabled coherence");
    compare_state(); teardown_pair(); ++cases;
}

/* The native STM bitmap window loads two uint32_t elements. Its final valid
 * starting element must be followed by allocated, zero padding, including
 * when the transfer ends at RAM's final instruction word. */
static void case_final_ram_words(int native, int mmu) {
    current_case = "16-word STM at final RAM word uses bounded padded bitmap window";
    setup_pair(); arm920t_set_jit(cpu_jit, native); arm920t_set_trace(cpu_ref, 1, NULL, NULL);
    const uint32_t end = RAM_BASE + RAM_SIZE, target = end - 8u;
    const uint32_t program[] = {0xe887ffffu, 0xe1a0f008u}; /* STMIA r7,{r0-r15}; MOV pc,r8 */
    load_both(program, GP32_ARRAY_COUNT(program));
    if (mmu) map_code_and_data(end - 0x1000u, end - 0x1000u, end - 0x1000u);
    set_mem_both(target, 0xe3a00011u); set_mem_both(target + 4u, 0xeafffffeu);
    prime_target_run(target, native, 1u);
    set_reg_both(7u, end - 64u); set_reg_both(8u, target); set_reg_both(14u, 0xe3a00022u);
    set_reg_both(15u, CODE_ADDR);
    if (native) prepare_native_block(cpu_jit, CODE_ADDR);
    arm920t_reset_cpu_profile(cpu_jit);
    CHECK(arm920t_run(cpu_jit, 3u) == arm920t_run(cpu_ref, 3u), "RAM-end STM/call budget");
    CHECK(cpu_jit->r[0] == 0x22u && gp32_ld32le(bus_jit.ram + RAM_SIZE - 8u) == 0xe3a00022u,
          "interior word at RAM end invalidates before next fetch");
    CHECK(cpu_jit->jit_code_watches[ARM_JIT_CODE_WATCHES / 32u] == 0u, "bitmap padding remains zero");
#if ARM920T_NATIVE_BACKEND
    if (native) CHECK(cpu_jit->prof.native_arm_insns != 0u, "RAM-end fixture executed native ARM");
#endif
    compare_state(); teardown_pair(); ++cases;
}

int main(void) {
    for (unsigned shape = WORD; shape <= BLOCK_FRAGMENT1; ++shape)
        for (int native = 0; native < 2; ++native)
            for (int mmu = 0; mmu < 2; ++mmu) case_store(shape, native, mmu, 0, 0);
    for (int native = 0; native < 2; ++native) {
        case_store(WORD, native, 0, 1, 0);
        case_store(WORD, native, 1, 1, 0);
        case_store(BLOCK_FRAGMENT0, native, 1, 0, 1);
        case_store(BLOCK_FRAGMENT1, native, 1, 0, 1);
        case_active_trace(native);
        case_remapped_code(native, 0); case_remapped_code(native, 1);
        case_write_enters_ram(native);
        case_disable_icache(native);
        for (int mmu = 0; mmu < 2; ++mmu) {
            case_final_ram_words(native, mmu);
            for (int same_page = 0; same_page < 3; ++same_page) case_data_store(native, mmu, same_page);
        }
    }
    case_raw_writer(0); case_raw_writer(1);
    printf("CPU code write cases=%u failures=%d\n", cases, failures);
    return failures != 0;
}
