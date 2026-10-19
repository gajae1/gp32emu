/* Isolated fastmem write-permission cost; no ROM or device timing.
 * Build: cc -std=c2x -O3 -Isrc -Iinclude scripts/bench_write_permission.c -o bench-write
 * BENCH_ARM_IMPL may name another checkout's src/arm920t.c for the baseline.
 * Argument: 0 = decoded portable, 1 = native requested. */
#define _XOPEN_SOURCE 700
#ifndef GP32EMU_CPU_PROFILE
#define GP32EMU_CPU_PROFILE 1
#endif
#ifndef BENCH_ARM_IMPL
#define BENCH_ARM_IMPL "../src/arm920t.c"
#endif
#include BENCH_ARM_IMPL
#include <time.h>
#include <stdio.h>
#define BRAM 0x0c000000u
#define BRAM_SIZE 0x800000u
#define BBIOS_SIZE 0x80000u
static uint8_t bram[BRAM_SIZE], bbios[BBIOS_SIZE];
static uint8_t *bench_ptr(void *u, uint32_t a, size_t n, int w) {
    (void)u;
    if (!w && a < BBIOS_SIZE && n <= BBIOS_SIZE-a) return bbios+a;
    if (a >= BRAM && (uint64_t)(a-BRAM)+n <= BRAM_SIZE) return bram+(a-BRAM);
    return NULL;
}
static uint8_t b8(void *u, uint32_t a) {uint8_t *p=bench_ptr(u,a,1,0);return p?*p:0;}
static uint16_t b16(void *u, uint32_t a) {uint8_t *p=bench_ptr(u,a,2,0);return p?gp32_ld16le(p):0;}
static uint32_t b32(void *u, uint32_t a) {uint8_t *p=bench_ptr(u,a,4,0);return p?gp32_ld32le(p):0;}
static void w8(void *u,uint32_t a,uint8_t v) {uint8_t*p=bench_ptr(u,a,1,1);if(p)*p=v;}
static void w16(void *u,uint32_t a,uint16_t v) {uint8_t*p=bench_ptr(u,a,2,1);if(p)gp32_st16le(p,v);}
static void w32(void *u,uint32_t a,uint32_t v) {uint8_t*p=bench_ptr(u,a,4,1);if(p)gp32_st32le(p,v);}
static uint64_t ns(void) {struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000u+t.tv_nsec;}
int main(int argc,char **argv) {
    unsigned jit=argc>1?(unsigned)atoi(argv[1]):1u;
    if(argc>2 || jit>1u) return 2;
    arm_bus_t bus={.read8=b8,.read16=b16,.read32=b32,.write8=w8,.write16=w16,.write32=w32,.fastmem=bench_ptr};
    const uint32_t program[]={0xe5810000u,0xe5c10004u,0xe1c100b6u,0xe881000du,0xe2800001u,0xeafffff9u};
    for(unsigned i=0;i<BBIOS_SIZE;i+=4u)gp32_st32le(bbios+i,0xeafffffeu);
    for(unsigned i=0;i<GP32_ARRAY_COUNT(program);++i)gp32_st32le(bbios+0x400u+4u*i,program[i]);
    arm920t_t *c=arm920t_create(&bus);
    if(!c)return 2;
    arm920t_reset(c,0x400u);arm920t_set_jit(c,jit);
    arm920t_set_reg(c,1u,BRAM+0x1000u);arm920t_set_reg(c,2u,0x11223344u);arm920t_set_reg(c,3u,0x55667788u);
    arm920t_run(c,600000u);arm920t_reset_cpu_profile(c);
    uint64_t begin=ns();
    for(unsigned i=0;i<1000u;++i)if(arm920t_run(c,60000u)!=60000u)return 3;
    uint64_t elapsed=ns()-begin;
    printf("%u %llu %u %08x %llu %llu\n",jit,(unsigned long long)elapsed,arm920t_get_reg(c,0u),gp32_ld32le(bram+0x1000u),(unsigned long long)c->prof.native_arm_insns,(unsigned long long)c->prof.block_interp_arm_insns);
    int ok=arm920t_get_reg(c,0u)==10100000u && gp32_ld32le(bram+0x1000u)==10099999u;
#if ARM920T_NATIVE_BACKEND && !GP32EMU_DISABLE_JIT
    if(jit) ok &= c->prof.native_arm_insns==60000000u;
#endif
    if(!jit) ok &= c->prof.block_interp_arm_insns==60000000u;
    arm920t_destroy(c);
    return !ok;
}
