/* Independent ARMv4T multiply expectations, ARM ARM DDI0100I A4.1.34,
 * A4.1.40, A4.1.76, A4.1.87, A4.1.128 and A4.1.129:
 * https://documentation-service.arm.com/static/5f8dacc8f86e16515cdb865a
 *
 * Products use unsigned magnitude and repeated addition, not the production
 * signed-conversion/multiply expression. No expected result calls a CPU
 * shifter, flag, arithmetic or condition helper. ARMv4 short multiply's C,
 * and long multiply's C/V, are UNPREDICTABLE when S=1 and are not asserted.
 * Historical Rd==Rm restrictions, equal long destinations and R15 operands
 * are excluded. Other source/destination/accumulator aliases are included. */
#ifndef GP32EMU_CPU_PROFILE
#define GP32EMU_CPU_PROFILE 1
#endif
#ifndef CORE_UNDER_TEST
#define CORE_UNDER_TEST "../src/arm920t.c"
#endif
#include CORE_UNDER_TEST

#define TEST_CODE 0x400u
#define TEST_RETURN 0x1000u
static uint8_t bios[ARM_JIT_BIOS_SIZE_BYTES], ram[ARM_JIT_RAM_SIZE_BYTES];
static arm920t_t *cpus[3];
static uint64_t cases[3], native_insns;
static unsigned failures, current_mode;
static uint32_t current_op, current_a, current_b, current_flags;

static uint8_t *memory(void *user, uint32_t a, size_t bytes, int write) {
    (void)user; (void)write;
    if (a < sizeof(bios) && bytes <= sizeof(bios) - a) return bios + a;
    uint32_t off = a - ARM_JIT_RAM_BASE_ADDR;
    if (off < sizeof(ram) && bytes <= sizeof(ram) - off) return ram + off;
    return NULL;
}
static uint8_t read8(void *u, uint32_t a) { uint8_t *p=memory(u,a,1u,0); return p ? p[0] : 0u; }
static uint16_t read16(void *u, uint32_t a) { uint8_t *p=memory(u,a,2u,0); return p ? gp32_ld16le(p) : 0u; }
static uint32_t read32(void *u, uint32_t a) { uint8_t *p=memory(u,a,4u,0); return p ? gp32_ld32le(p) : 0u; }
static void write8(void *u, uint32_t a, uint8_t v) { (void)u; (void)a; (void)v; }
static void write16(void *u, uint32_t a, uint16_t v) { (void)u; (void)a; (void)v; }
static void write32(void *u, uint32_t a, uint32_t v) { (void)u; (void)a; (void)v; }
static void check(uint32_t got, uint32_t want, const char *what) {
    if (got == want) return;
    if (failures < 24u)
        fprintf(stderr,"mode=%u op=%08x a=%08x b=%08x flags=%08x %s got=%08x want=%08x\n",
                current_mode,current_op,current_a,current_b,current_flags,what,got,want);
    ++failures;
}

static uint64_t product(uint32_t a, uint32_t b, unsigned sign) {
    uint64_t aa = a, bb = b, result = 0u;
    unsigned negative = 0u;
    if (sign && a >= UINT32_C(2147483648)) { aa=UINT64_C(4294967296)-a; negative^=1u; }
    if (sign && b >= UINT32_C(2147483648)) { bb=UINT64_C(4294967296)-b; negative^=1u; }
    while (bb) {
        if (bb % 2u) result += aa;
        aa += aa;
        bb /= 2u;
    }
    return negative ? UINT64_C(0)-result : result;
}
static unsigned condition(unsigned cond, uint32_t flags) {
    unsigned n=!!(flags&0x80000000u), z=!!(flags&0x40000000u);
    unsigned c=!!(flags&0x20000000u), v=!!(flags&0x10000000u);
    const unsigned pass[15] = {z,!z,c,!c,n,!n,v,!v,c&&!z,!c||z,n==v,n!=v,!z&&(n==v),z||(n!=v),1u};
    return pass[cond];
}
typedef struct { unsigned rm,rs,lo,hi; } registers_t;
static const registers_t short_registers[] = {
    {0u,1u,2u,3u}, {0u,2u,2u,3u}, {0u,0u,2u,3u},
    {0u,1u,3u,3u}, {0u,1u,2u,0u}, {0u,1u,2u,1u},
    {0u,2u,2u,2u}, {0u,0u,2u,0u}, {0u,1u,14u,13u}
};
static const registers_t long_registers[] = {
    {0u,1u,2u,3u}, {0u,2u,2u,3u}, {0u,3u,2u,3u},
    {0u,0u,2u,3u}, {0u,1u,3u,2u}, {0u,14u,8u,13u},
    {14u,0u,13u,8u}
};
/* MUL, MLA, UMULL, UMLAL, SMULL, SMLAL. */
static const uint32_t encodings[] = {0x00000090u,0x00200090u,0x00800090u,0x00a00090u,0x00c00090u,0x00e00090u};
static const uint32_t edges[] = {
    0u,1u,2u,0xffu,0x100u,0xffffu,0x10000u,0x7fffffffu,
    0x80000000u,0x80000001u,0xfffffffeu,0xffffffffu,
    0x55555555u,0xaaaaaaaau,0x01010101u
};

static void set_program(unsigned kind, registers_t regs, unsigned s, unsigned cond) {
    current_op = (cond << 28) | encodings[kind] | (s << 20) |
        (regs.rs << 8) | regs.rm;
    if (kind >= 2u) current_op |= (regs.hi << 16) | (regs.lo << 12);
    else current_op |= (regs.lo << 16) | (kind ? regs.hi << 12 : 0u);
    gp32_st32le(bios+TEST_CODE,current_op);
    gp32_st32le(bios+TEST_CODE+4u,0xe12fff1cu); /* BX r12 */
    for (unsigned mode=0;mode<3u;++mode) {
        arm920t_flush_jit(cpus[mode]);
#if ARM920T_NATIVE_BACKEND
        if (mode == 2u) {
            current_mode=mode;
            arm_jit_block_t *b=arm_jit_translate(cpus[mode],TEST_CODE);
            check(b != NULL,1u,"native block collected");
            if (b) {
                if (b->native_ok >= 4u) b->native_ok=0u;
                check((uint32_t)arm_jit_lazy_compile(cpus[mode],b),0u,"native arena retained");
                check(b->native != NULL,1u,"native entry prepared");
            }
        }
#endif
    }
}
static void execute(unsigned kind, registers_t regs, unsigned s, unsigned cond,
                    uint32_t a, uint32_t b, uint64_t accumulator, uint32_t flags) {
    uint32_t before[16], expected[16], expected_flags=flags|0xd3u;
    for (unsigned r=0;r<16u;++r) before[r]=0x31415926u+r*0x01010101u;
    before[regs.lo]=(uint32_t)accumulator;
    before[regs.hi]=(uint32_t)(accumulator >> 32);
    before[regs.rm]=a;
    if (regs.rs != regs.rm) before[regs.rs]=b;
    before[12]=TEST_RETURN; before[15]=TEST_CODE;
    memcpy(expected,before,sizeof(expected)); expected[15]=TEST_RETURN;
    current_a=before[regs.rm]; current_b=before[regs.rs]; current_flags=flags;
    uint32_t mask=UINT32_MAX;
    if (condition(cond,flags)) {
        uint64_t result=product(before[regs.rm],before[regs.rs],kind>=4u);
        if (kind==1u) result+=before[regs.hi];
        else if (kind==3u || kind==5u)
            result+=(uint64_t)before[regs.hi]*UINT64_C(4294967296)+before[regs.lo];
        expected[regs.lo]=(uint32_t)result;
        if (kind>=2u) expected[regs.hi]=(uint32_t)(result/UINT64_C(4294967296));
        if (s) {
            uint32_t high=kind>=2u ? expected[regs.hi] : expected[regs.lo];
            unsigned zero=kind>=2u ? result==0u : expected[regs.lo]==0u;
            expected_flags=(expected_flags&0x3fffffffu)|(high&0x80000000u)|(zero ? 0x40000000u : 0u);
            mask&=kind>=2u ? ~0x30000000u : ~0x20000000u;
        }
    }
    for (unsigned mode=0;mode<3u;++mode) {
        current_mode=mode;
        arm920t_t *cpu=cpus[mode];
        arm920t_set_cpsr(cpu,flags|0xd3u);
        for (unsigned r=0;r<16u;++r) arm920t_set_reg(cpu,r,before[r]);
        uint64_t native_before=cpu->prof.native_arm_insns;
        check(arm920t_run(cpu,2u),2u,"retirement");
        for (unsigned r=0;r<16u;++r) check(arm920t_get_reg(cpu,r),expected[r],"register");
        check(arm920t_get_cpsr(cpu)&mask,expected_flags&mask,"CPSR");
#if ARM920T_NATIVE_BACKEND
        if (mode==2u) check(cpu->prof.native_arm_insns>native_before,1u,"native execution");
#endif
        native_insns+=cpu->prof.native_arm_insns-native_before;
        ++cases[mode];
    }
}
int main(void) {
    arm_bus_t bus={.read8=read8,.read16=read16,.read32=read32,
        .write8=write8,.write16=write16,.write32=write32,.fastmem=memory};
    for (unsigned mode=0;mode<3u;++mode) {
        cpus[mode]=arm920t_create(&bus);
        if (!cpus[mode]) return 2;
        arm920t_set_jit(cpus[mode],mode==2u);
        arm920t_set_trace(cpus[mode],mode==0u,NULL,NULL);
    }
    for (unsigned kind=0;kind<GP32_ARRAY_COUNT(encodings);++kind) {
        unsigned layouts=kind>=2u ? GP32_ARRAY_COUNT(long_registers) :
                         kind==1u ? GP32_ARRAY_COUNT(short_registers) : 3u;
        for (unsigned layout=0;layout<layouts;++layout) {
            registers_t regs=kind>=2u ? long_registers[layout] : short_registers[layout];
            for (unsigned s=0;s<2u;++s) {
                set_program(kind,regs,s,14u);
                for (unsigned ai=0;ai<GP32_ARRAY_COUNT(edges);++ai)
                    for (unsigned bi=0;bi<GP32_ARRAY_COUNT(edges);++bi) {
                        uint64_t accumulator=(uint64_t)edges[(ai+bi)%GP32_ARRAY_COUNT(edges)]*UINT64_C(4294967296) |
                                             edges[(ai*3u+bi*5u)%GP32_ARRAY_COUNT(edges)];
                        for (unsigned flags=0;flags<16u;++flags)
                            execute(kind,regs,s,14u,edges[ai],edges[bi],accumulator,flags<<28);
                    }
            }
        }
        for (unsigned cond=0;cond<14u;++cond)
            for (unsigned s=0;s<2u;++s) {
                registers_t regs=kind>=2u ? long_registers[0] : short_registers[0];
                set_program(kind,regs,s,cond);
                for (unsigned flags=0;flags<16u;++flags)
                    execute(kind,regs,s,cond,0x80000000u,0xffffffffu,UINT64_C(0x7fffffffffffffff),flags<<28);
            }
    }
    for (unsigned mode=0;mode<3u;++mode) {
        printf("mode %u: %" PRIu64 " cases\n",mode,cases[mode]);
        current_mode=mode;
        /* AArch64 uses a checked helper for BX r12. Require multiplication
         * itself to be native without conflating that branch with its body. */
        check((uint32_t)cpus[mode]->prof.helper_op_kinds[ARM_JIT_OP_MUL],0u,
              "multiply avoids semantic helpers");
        if (mode==2u) printf("native multiply helpers: %" PRIu64 ", other helpers: %" PRIu64 "\n",
            cpus[mode]->prof.helper_op_kinds[ARM_JIT_OP_MUL],cpus[mode]->prof.helper_interp_ops);
        arm920t_destroy(cpus[mode]);
    }
    printf("arm_multiply_test: %" PRIu64 " cases, %u failures, %" PRIu64 " native instructions\n",
           cases[0]+cases[1]+cases[2],failures,native_insns);
    return failures != 0u;
}
