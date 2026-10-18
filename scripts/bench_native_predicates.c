/* Compound-predicate synthetic kernel, with deterministic independent result
 * checks. Args: mode (0 cached, 1 native, 2 raw, 3 compile), condition (0..14),
 * pattern (0 skipped, 1 executed, 2 mixed), elements, passes, verify (0/1).
 * verify=1 disables the clock. Profiling defaults off; a separate profiling
 * build requires every retired instruction in mode 1 to execute natively. */
#ifndef GP32EMU_CPU_PROFILE
#define GP32EMU_CPU_PROFILE 0
#endif
#ifndef CORE_UNDER_TEST
#define CORE_UNDER_TEST "../src/arm920t.c"
#endif
#include CORE_UNDER_TEST
#include <time.h>

#define PRED_PC 0x400u
#define PRED_DATA (ARM_JIT_RAM_BASE_ADDR + 0x1000u)
#define PRED_COMPARE 0x12345678u
static uint8_t pred_bios[ARM_JIT_BIOS_SIZE_BYTES], pred_ram[ARM_JIT_RAM_SIZE_BYTES];
static unsigned swi_calls;

static uint8_t *pred_ptr(void *u, uint32_t a, size_t n, int write) {
    GP32_UNUSED(u);
    if (!write && a < sizeof(pred_bios) && n <= sizeof(pred_bios) - a) return pred_bios + a;
    uint32_t off = a - ARM_JIT_RAM_BASE_ADDR;
    return off < sizeof(pred_ram) && n <= sizeof(pred_ram) - off ? pred_ram + off : NULL;
}
static uint8_t pred_r8(void *u, uint32_t a) { uint8_t *p=pred_ptr(u,a,1,0);return p ? *p : 0; }
static uint16_t pred_r16(void *u, uint32_t a) { uint8_t *p=pred_ptr(u,a,2,0);return p ? gp32_ld16le(p) : 0; }
static uint32_t pred_r32(void *u, uint32_t a) { uint8_t *p=pred_ptr(u,a,4,0);return p ? gp32_ld32le(p) : 0; }
static void pred_w8(void *u, uint32_t a, uint8_t v) { uint8_t *p=pred_ptr(u,a,1,1);if(p)*p=v; }
static void pred_w16(void *u, uint32_t a, uint16_t v) { uint8_t *p=pred_ptr(u,a,2,1);if(p)gp32_st16le(p,v); }
static void pred_w32(void *u, uint32_t a, uint32_t v) { uint8_t *p=pred_ptr(u,a,4,1);if(p)gp32_st32le(p,v); }
static int pred_swi(void *u, arm920t_t *c, uint32_t imm, uint32_t pc, int is_thumb) {
    GP32_UNUSED(u);
    if (imm != 0x7ffdu || pc != PRED_PC + 40u || is_thumb || c->r[15] != pc + 4u) abort();
    ++swi_calls;arm920t_stop_run(c);return 1;
}
static uint64_t pred_ns(void) {
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}
static int pred_expected(unsigned cond, uint32_t lhs) {
    uint32_t result=lhs-PRED_COMPARE;
    int n=!!(result&0x80000000u),z=result==0u,c=lhs>=PRED_COMPARE;
    int v=!!((lhs^PRED_COMPARE)&(lhs^result)&0x80000000u);
    const int yes[15]={z,!z,c,!c,n,!n,v,!v,c&&!z,!c||z,n==v,n!=v,!z&&n==v,z||n!=v,1};
    return yes[cond];
}
static uint32_t pred_pass(arm920t_t *c, unsigned elements) {
    c->r[2]=PRED_DATA;c->r[4]=c->r[6]=c->r[8]=c->r[9]=c->r[11]=0u;
    c->r[5]=0x314u;c->r[10]=elements;c->r[12]=PRED_COMPARE;c->r[15]=PRED_PC;
    return arm920t_run(c,UINT32_MAX);
}

int main(int argc, char **argv) {
    unsigned mode=argc>1 ? (unsigned)strtoul(argv[1],NULL,10) : 1u;
    unsigned cond=argc>2 ? (unsigned)strtoul(argv[2],NULL,10) : 12u;
    unsigned pattern=argc>3 ? (unsigned)strtoul(argv[3],NULL,10) : 2u;
    unsigned elements=argc>4 ? (unsigned)strtoul(argv[4],NULL,10) : 32768u;
    unsigned passes=argc>5 ? (unsigned)strtoul(argv[5],NULL,10) : 32u;
    unsigned verify=argc>6 ? (unsigned)strtoul(argv[6],NULL,10) : 0u;
    if(argc>7 || mode>3u || cond>14u || pattern>2u || (cond==14u&&pattern!=1u) ||
       !elements || elements>65536u || !passes || passes>1000000u || verify>1u ||
       (mode==3u&&passes>10000u))return 2;
    uint32_t program[]={0xe4927004u,0xe157000cu,0xe2844001u,
        (cond<<28)|0x02845003u,(cond<<28)|0x00266005u,
        (cond<<28)|0x02888005u,(cond<<28)|0x00899008u,
        0xe08bb005u,0xe25aa001u,0x1afffff5u,0xef007ffdu};
    for(unsigned i=0;i<GP32_ARRAY_COUNT(program);++i)gp32_st32le(pred_bios+PRED_PC+4u*i,program[i]);
    const uint32_t pool[]={0u,PRED_COMPARE-1u,PRED_COMPARE,PRED_COMPARE+1u,
        0x7fffffffu,0x80000000u,UINT32_MAX,0x92345678u};
    uint32_t input[2]={0,0};unsigned found[2]={0,0};
    for(unsigned i=0;i<GP32_ARRAY_COUNT(pool);++i){unsigned yes=(unsigned)pred_expected(cond,pool[i]);input[yes]=pool[i];found[yes]=1u;}
    if(!found[1] || (cond!=14u&&!found[0]))return 2;
    uint32_t r5=0x314u,r6=0u,r8=0u,r9=0u,r11=0u,state=0x1c9f731du,taken=0u;
    for(unsigned i=0;i<elements;++i){
        state^=state<<13;state^=state>>17;state^=state<<5;
        unsigned yes=pattern==2u ? state&1u : pattern;
        gp32_st32le(pred_ram+PRED_DATA-ARM_JIT_RAM_BASE_ADDR+4u*i,input[yes]);
        if(pred_expected(cond,input[yes])){r5=i+4u;r6^=r5;r8+=5u;r9+=r8;++taken;}
        r11+=r5;
    }
    arm_bus_t bus={.read8=pred_r8,.read16=pred_r16,.read32=pred_r32,.write8=pred_w8,
        .write16=pred_w16,.write32=pred_w32,.fastmem=pred_ptr};
    arm920t_t *c=arm920t_create(&bus);if(!c)return 3;
    arm920t_reset(c,PRED_PC);arm920t_set_jit(c,mode==1u||mode==3u);
    arm920t_set_trace(c,mode==2u,NULL,NULL);arm920t_set_swi_handler(c,pred_swi,NULL);
    int ok=1;uint64_t elapsed,retired=0u;size_t block_bytes=0;
    if(mode==3u){
        for(unsigned i=0;i<2u;++i){arm_jit_block_t *b=arm_jit_translate(c,PRED_PC);if(!b)return 3;arm_jit_compile_host(c,b);ok&=b->native!=NULL;}
        uint64_t begin=verify ? 0u : pred_ns();
        for(unsigned i=0;i<passes;++i){
            arm_jit_block_t *b=arm_jit_translate(c,PRED_PC);if(!b)return 3;
            size_t before=c->jit_code_used;arm_jit_compile_host(c,b);block_bytes=c->jit_code_used-before;
            ok&=b->native!=NULL;
        }
        elapsed=verify ? 0u : pred_ns()-begin;
        ok&=c->cycles_total==0u && c->r[15]==PRED_PC && swi_calls==0u;
    }else{
        uint32_t count=elements*10u+1u;
        for(unsigned i=0;i<2u;++i)ok&=pred_pass(c,elements)==count;
        arm920t_reset_cpu_profile(c);
        uint64_t begin=verify ? 0u : pred_ns();
        for(unsigned i=0;i<passes;++i)retired+=pred_pass(c,elements);
        elapsed=verify ? 0u : pred_ns()-begin;
        ok&=retired==(uint64_t)count*passes && swi_calls==passes+2u;
        ok&=c->r[2]==PRED_DATA+elements*4u && c->r[4]==elements && c->r[5]==r5 &&
            c->r[6]==r6 && c->r[8]==r8 && c->r[9]==r9 && c->r[10]==0u && c->r[11]==r11 &&
            c->r[15]==PRED_PC+44u && c->cpsr==0x600000d3u;
        if(mode==1u){
            arm_jit_block_t *b=&c->jit_blocks[arm_jit_block_index(PRED_PC)];
            ok&=b->native!=NULL && b->native_ok==1u;
#if ARM920T_PROFILING
            ok&=c->prof.native_arm_insns==retired && c->prof.native_block_calls>0u;
#endif
        }
    }
    double units=mode==3u ? passes : (double)passes*elements;
    printf("{\"mode\":%u,\"cond\":%u,\"pattern\":%u,\"elements\":%u,\"passes\":%u,"
        "\"taken\":%u,\"elapsed_ns\":%" PRIu64 ",\"ns_per_unit\":%.6f,\"unit\":\"%s\","
        "\"retired\":%" PRIu64 ",\"native_insns\":%" PRIu64 ",\"native_calls\":%" PRIu64 ","
        "\"code_bytes\":%zu,\"last_compile_bytes\":%zu,\"checksum\":\"%08x:%08x:%08x\",\"ok\":%s}\n",
        mode,cond,pattern,elements,passes,taken,elapsed,(double)elapsed/units,mode==3u ? "compile" : "element",
        retired,c->prof.native_arm_insns,c->prof.native_block_calls,c->jit_code_used,block_bytes,r6,r9,r11,ok ? "true" : "false");
    arm920t_destroy(c);return !ok;
}
