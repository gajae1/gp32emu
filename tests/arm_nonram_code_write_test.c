/* A generic ARM bus can provide executable and writable memory outside the
 * standard GP32 RAM window, including writable BIOS. With CP15.I=0 a CPU
 * write must affect the next instruction fetch. Literal MOV instructions
 * supply independent before/after values for every executor. */
#ifndef GP32EMU_CPU_PROFILE
#define GP32EMU_CPU_PROFILE 1
#endif
#ifndef CORE_UNDER_TEST
#define CORE_UNDER_TEST "../src/arm920t.c"
#endif
#include <inttypes.h>
#include CORE_UNDER_TEST

#define WRITER 0x400u
#define RETURN_TO 0x800u
#define EXTRA_SIZE 256u
typedef struct {
    uint8_t bios[ARM_JIT_BIOS_SIZE_BYTES], ram[ARM_JIT_RAM_SIZE_BYTES], extra[EXTRA_SIZE];
    uint32_t code_pa;
    unsigned callback_write, writes;
} fixture_t;
static fixture_t f;
static unsigned cases,failures,execution,shape,mmu_mode,cache_mode;
static unsigned cases_by_mode[3],failures_by_mode[3];
enum test_group { REPLACEMENT, ACTIVE, EXTERNAL, ORDINARY_DATA, BOUNDARY, SPANS, PUBLIC_BL, GROUP_COUNT };
static unsigned current_group;
static struct { const char *name; unsigned cases,failures; } groups[GROUP_COUNT]={
    {"replacement",0u,0u},{"active trace",0u,0u},{"external replacement",0u,0u},
    {"ordinary data",0u,0u},{"halfword boundaries",0u,0u},{"external clipped spans",0u,0u},
    {"public BL dispatch",0u,0u}
};
static uint64_t cached_insns,native_insns;
static void complete_case(void) {
    ++cases; ++cases_by_mode[execution]; ++groups[current_group].cases;
}
static void finish_cpu(arm920t_t *cpu) {
    cached_insns+=cpu->prof.block_interp_arm_insns;
    native_insns+=cpu->prof.native_arm_insns;
    arm920t_destroy(cpu);
}

static uint8_t *backing(uint32_t a, size_t bytes) {
    if (a < sizeof(f.bios) && bytes <= sizeof(f.bios)-a) return f.bios+a;
    uint32_t off=a-ARM_JIT_RAM_BASE_ADDR;
    if (off < sizeof(f.ram) && bytes <= sizeof(f.ram)-off) return f.ram+off;
    off=a-f.code_pa;
    if (off < sizeof(f.extra) && bytes <= sizeof(f.extra)-off) return f.extra+off;
    return NULL;
}
static int overlaps_code(uint32_t a, size_t bytes) {
    return bytes && (uint64_t)a < (uint64_t)f.code_pa+EXTRA_SIZE &&
           (uint64_t)f.code_pa < (uint64_t)a+bytes;
}
static uint8_t *mapped(void *u, uint32_t a, size_t bytes, int write) {
    (void)u;
    if (write && f.callback_write && overlaps_code(a,bytes)) return NULL;
    return backing(a,bytes);
}
static uint8_t read8(void *u,uint32_t a) { (void)u; uint8_t *p=backing(a,1u); return p ? *p : 0u; }
static uint16_t read16(void *u,uint32_t a) { (void)u; uint8_t *p=backing(a,2u); return p ? gp32_ld16le(p) : 0u; }
static uint32_t read32(void *u,uint32_t a) { (void)u; uint8_t *p=backing(a,4u); return p ? gp32_ld32le(p) : 0u; }
static void write8(void *u,uint32_t a,uint8_t v) { (void)u; uint8_t *p=backing(a,1u); if(p)*p=v; if(overlaps_code(a,1u))++f.writes; }
static void write16(void *u,uint32_t a,uint16_t v) {
    (void)u;
    uint8_t *p=backing(a,2u);
    if(p)gp32_st16le(p,v);
    else {
        /* Same two-byte callback semantics as a bus that crosses windows
         * or wraps its 32-bit physical address at the final byte. */
        p=backing(a,1u); if(p)*p=(uint8_t)v;
        p=backing(a+1u,1u); if(p)*p=(uint8_t)(v>>8);
    }
    if(overlaps_code(a,2u)||overlaps_code(a+1u,1u))++f.writes;
}
static void write32(void *u,uint32_t a,uint32_t v) { (void)u; uint8_t *p=backing(a,4u); if(p)gp32_st32le(p,v); if(overlaps_code(a,4u))++f.writes; }
static void check(uint32_t got,uint32_t want,const char *what) {
    if(got==want)return;
    if(failures<32u)fprintf(stderr,"mode=%u shape=%u MMU=%u I=%u PA=%08x callback=%u %s got=%08x want=%08x\n",
        execution,shape,mmu_mode,cache_mode,f.code_pa,f.callback_write,what,got,want);
    ++failures;
    ++failures_by_mode[execution];
    ++groups[current_group].failures;
}
static void prepare_code(arm920t_t *cpu,uint32_t pc) {
    /* Cold non-RAM dispatch deliberately executes raw. Explicitly collect
     * a derived block here to exercise cached entry as well as native entry.
     * Separate stitched-leaf cases below use only ordinary public dispatch. */
    arm_jit_block_t *b=arm_jit_translate(cpu,pc);
    check(b!=NULL,1u,"derived block");
#if ARM920T_NATIVE_BACKEND
    if(b && execution==2u) {
        if(b->native_ok>=4u)b->native_ok=0u;
        check((uint32_t)arm_jit_lazy_compile(cpu,b),0u,"native arena retained");
        check(b->native!=NULL,1u,"native entry");
    }
#endif
}
static arm920t_t *setup(uint32_t pa,unsigned mode,unsigned mmu,unsigned cache,unsigned callback) {
    memset(&f,0,sizeof(f));
    f.code_pa=pa; f.callback_write=callback;
    execution=mode; mmu_mode=mmu; cache_mode=cache;
    arm_bus_t bus={.read8=read8,.read16=read16,.read32=read32,
        .write8=write8,.write16=write16,.write32=write32,.fastmem=mapped,
        .read32_io=read32,.write32_io=write32};
    arm920t_t *cpu=arm920t_create(&bus);
    if(!cpu)exit(2);
    if(mmu || cache) {
        gp32_st32le(f.bios+0x4000u,0xc02u);
        gp32_st32le(f.bios+0x4000u+0x0c0u*4u,ARM_JIT_RAM_BASE_ADDR|0xc02u);
        gp32_st32le(f.bios+0x4000u+0x140u*4u,0x14000c02u);
        gp32_st32le(f.bios+0x4000u+0x240u*4u,(pa&0xfff00000u)|0xc02u);
        gp32_st32le(f.bios+0x4000u+0x340u*4u,(pa&0xfff00000u)|0xc02u);
        const uint32_t setup_code[]={0xee020f10u,0xee031f10u,0xee012f10u};
        for(unsigned i=0;i<GP32_ARRAY_COUNT(setup_code);++i)gp32_st32le(f.bios+0x100u+i*4u,setup_code[i]);
        arm920t_set_trace(cpu,1,NULL,NULL);
        arm920t_set_reg(cpu,0u,0x4000u); arm920t_set_reg(cpu,1u,UINT32_MAX);
        arm920t_set_reg(cpu,2u,0x70u|mmu|(cache<<12)); arm920t_set_reg(cpu,15u,0x100u);
        check(arm920t_run(cpu,3u),3u,"guest MMU/I setup");
        arm920t_set_trace(cpu,0,NULL,NULL);
    }
    arm920t_set_jit(cpu,mode==2u);
    if(mode==0u)arm920t_set_trace(cpu,1,NULL,NULL);
    arm920t_set_reg(cpu,11u,RETURN_TO); arm920t_set_reg(cpu,12u,RETURN_TO);
    return cpu;
}
static uint32_t code_va(void) { return mmu_mode ? 0x34000000u+(f.code_pa&0xfffffu) : f.code_pa; }
static uint32_t data_va(void) { return mmu_mode ? 0x24000000u+(f.code_pa&0xfffffu) : f.code_pa; }
static void target(arm920t_t *cpu,uint32_t value) {
    arm920t_set_reg(cpu,15u,code_va());
    check(arm920t_run(cpu,2u),2u,"target retirement");
    check(arm920t_get_reg(cpu,0u),value,"target MOV immediate");
    check(arm920t_get_reg(cpu,15u),RETURN_TO,"target return");
    check(arm920t_get_cpsr(cpu),0xd3u,"target state");
}
static const uint32_t stores[]={
    0xe5871000u, /* STR r1,[r7] */
    0xe5c71000u, /* STRB r1,[r7] */
    0xe1c710b0u, /* STRH r1,[r7] */
    0xe1073091u, /* SWP r3,r1,[r7] */
    0xe1473091u, /* SWPB r3,r1,[r7] */
    0xe8870006u  /* STMIA r7,{r1,r2} */
};
static void replacement(uint32_t pa,unsigned mode,unsigned mmu,unsigned cache,unsigned callback,
                         unsigned store,unsigned raw_writer) {
    current_group=REPLACEMENT; shape=store+raw_writer*8u;
    arm920t_t *cpu=setup(pa,mode,mmu,cache,callback);
    gp32_st32le(backing(pa,4u),0xe3a00011u);
    gp32_st32le(backing(pa+4u,4u),0xe12fff1cu);
    if(mode)prepare_code(cpu,code_va());
    target(cpu,0x11u);
    gp32_st32le(f.bios+WRITER,stores[store]);
    gp32_st32le(f.bios+WRITER+4u,0xe12fff1bu); /* BX r11 */
    uint32_t value=(store==1u || store==2u || store==4u) ? 0x22u : 0xe3a00022u;
    arm920t_set_reg(cpu,1u,value); arm920t_set_reg(cpu,2u,0xe12fff1cu);
    arm920t_set_reg(cpu,7u,data_va()); arm920t_set_reg(cpu,15u,WRITER);
    if(raw_writer==2u) {
        gp32_st16le(f.bios+WRITER,0x6039u); /* Thumb STR r1,[r7,#0] */
        arm920t_set_cpsr(cpu,0xf3u);
    } else if(raw_writer==1u)arm920t_set_trace(cpu,1,NULL,NULL);
    else if(mode)prepare_code(cpu,WRITER);
    uint32_t epoch=cpu->jit_cache_epoch;
    check(arm920t_run(cpu,raw_writer==2u ? 1u : 2u),raw_writer==2u ? 1u : 2u,"writer retirement");
    check(read32(NULL,pa),0xe3a00022u,"physical instruction replaced");
    check(f.writes,callback ? (store==5u ? 2u : 1u) : 0u,"write callbacks");
    if(raw_writer==2u)arm920t_set_cpsr(cpu,0xd3u);
    if(raw_writer==1u)arm920t_set_trace(cpu,mode==0u,NULL,NULL);
    if(cache && mode) {
        check(cpu->jit_cache_epoch,epoch,"I=1 retains maintenance policy");
        target(cpu,0x11u);
        gp32_st32le(f.bios+0x100u,0xee070f15u); /* MCR p15,0,r0,c7,c5,0 */
        arm920t_set_reg(cpu,15u,0x100u);
        check(arm920t_run(cpu,1u),1u,"guest I-cache maintenance");
    }
    target(cpu,0x22u);
#if ARM920T_NATIVE_BACKEND
    if(mode==2u)check(cpu->prof.native_arm_insns!=0u,1u,"positive native execution");
#endif
    finish_cpu(cpu); complete_case();
}
static void active(uint32_t pa,unsigned mode,unsigned mmu,unsigned callback) {
    current_group=ACTIVE; shape=24u;
    arm920t_t *cpu=setup(pa,mode,mmu,0u,callback);
    const uint32_t words[]={0xe5871000u,0xe1a02002u,0xe1a03003u,0xe3a00011u,0xe12fff1cu};
    for(unsigned i=0;i<GP32_ARRAY_COUNT(words);++i)gp32_st32le(backing(pa+i*4u,4u),words[i]);
    arm920t_set_reg(cpu,1u,0xe3a00022u); arm920t_set_reg(cpu,7u,data_va()+12u);
    arm920t_set_reg(cpu,15u,code_va());
    if(mode)prepare_code(cpu,code_va());
    check(arm920t_run(cpu,5u),5u,"active trace retirement");
    check(arm920t_get_reg(cpu,0u),0x22u,"active trace sees replacement");
    check(read32(NULL,pa+12u),0xe3a00022u,"active physical replacement");
    check(arm920t_get_reg(cpu,15u),RETURN_TO,"active trace return");
    check(f.writes,callback, "active write callbacks");
#if ARM920T_NATIVE_BACKEND
    if(mode==2u)check(cpu->prof.native_arm_insns!=0u,1u,"active native execution");
#endif
    finish_cpu(cpu); complete_case();
}
static void external(uint32_t pa,unsigned mode,unsigned mmu,unsigned cache,unsigned bytes) {
    current_group=EXTERNAL; shape=32u+bytes;
    arm920t_t *cpu=setup(pa,mode,mmu,cache,0u);
    gp32_st32le(backing(pa,4u),0xe3a00011u);
    gp32_st32le(backing(pa+4u,4u),0xe12fff1cu);
    if(mode)prepare_code(cpu,code_va());
    target(cpu,0x11u);
    if(bytes==1u)*backing(pa,1u)=0x22u;
    else if(bytes==2u)gp32_st16le(backing(pa,2u),0x22u);
    else gp32_st32le(backing(pa,4u),0xe3a00022u);
    arm920t_invalidate_icache_range(cpu,pa,bytes);
    target(cpu,0x22u);
    finish_cpu(cpu); complete_case();
}
static void ordinary_data(uint32_t pa,unsigned mode,unsigned mmu,unsigned cache,uint32_t data) {
    current_group=ORDINARY_DATA; shape=40u;
    arm920t_t *cpu=setup(pa,mode,mmu,cache,0u);
    gp32_st32le(backing(pa,4u),0xe3a00011u);
    gp32_st32le(backing(pa+4u,4u),0xe12fff1cu);
    if(mode)prepare_code(cpu,code_va());
    target(cpu,0x11u);
    gp32_st32le(f.bios+WRITER,stores[0]);
    gp32_st32le(f.bios+WRITER+4u,0xe12fff1bu);
    arm920t_set_reg(cpu,1u,0x76543210u); arm920t_set_reg(cpu,7u,data);
    arm920t_set_reg(cpu,15u,WRITER);
    if(mode)prepare_code(cpu,WRITER);
    uint32_t epoch=cpu->jit_cache_epoch,generation=cpu->jit_generation;
    check(arm920t_run(cpu,2u),2u,"ordinary data retirement");
    check(cpu->jit_cache_epoch,epoch,"ordinary RAM/MMIO preserves epoch");
    check(cpu->jit_generation,generation,"ordinary RAM/MMIO preserves generation");
    if(data==ARM_JIT_RAM_BASE_ADDR+0x6000u)
        check(read32(NULL,data),0x76543210u,"ordinary RAM data stored");
    if(!mmu && pa<ARM_JIT_RAM_BASE_ADDR) {
        arm920t_invalidate_icache_range(cpu,ARM_JIT_RAM_BASE_ADDR+0x6000u,4u);
        check(cpu->jit_cache_epoch,epoch,"BIOS watch preserves external RAM data epoch");
    }
    finish_cpu(cpu); complete_case();
}
static void boundary_halfword(uint32_t pa,unsigned mode,unsigned cache) {
    current_group=BOUNDARY; shape=48u;
    arm920t_t *cpu=setup(pa,mode,0u,cache,1u);
    gp32_st32le(backing(pa,4u),0xe3a00011u);
    gp32_st32le(backing(pa+4u,4u),0xe12fff1cu);
    if(mode)prepare_code(cpu,pa);
    target(cpu,0x11u);
    uint32_t epoch=cpu->jit_cache_epoch;
    /* Exercise the existing physical byte-callback contract directly.
     * Odd-address STRH is not an architectural oracle on ARMv4; these
     * controls only require notification for the bytes this bus writes. */
    wb16(cpu,pa-1u,0x22a5u);
    check(read32(NULL,pa),0xe3a00022u,"crossing halfword replacement");
    check(f.writes,1u,"crossing halfword callback");
    if(cache && mode) {
        check(cpu->jit_cache_epoch,epoch,"crossing I=1 preserves epoch");
        target(cpu,0x11u);
        gp32_st32le(f.bios+0x100u,0xee070f15u);
        arm920t_set_reg(cpu,15u,0x100u);
        check(arm920t_run(cpu,1u),1u,"crossing guest cache maintenance");
    }
    target(cpu,0x22u);
    finish_cpu(cpu); complete_case();
}
static void external_spans(uint32_t pa,unsigned mode,unsigned cache) {
    current_group=SPANS; shape=49u;
    arm920t_t *cpu=setup(pa,mode,0u,cache,0u);
    gp32_st32le(backing(pa,4u),0xe3a00011u);
    gp32_st32le(backing(pa+4u,4u),0xe12fff1cu);
    prepare_code(cpu,pa);
    target(cpu,0x11u);
    /* Independent interval table for the documented conservative regions.
     * This fixture has no MMU and no other fetched instructions. */
    uint64_t lo[2],hi[2]; unsigned intervals=1u;
    if(pa<0x80000u) { lo[0]=0u; hi[0]=0x80000u; }
    else if(pa>=0x0c000000u && pa<0x0c800000u) { lo[0]=pa; hi[0]=(uint64_t)pa+8u; }
    else { lo[0]=0x80000u; hi[0]=0x0c000000u; lo[1]=0x0c800000u; hi[1]=UINT64_C(4294967296); intervals=2u; }
    const uint32_t boundaries[]={0u,0x80000u,0x0c000000u,0x0c800000u,UINT32_MAX,pa,pa+8u};
    const uint32_t lengths[]={0u,1u,2u,3u,4u,5u,8u,16u,128u,0x80000u,0x0c000000u,UINT32_MAX};
    for(unsigned edge=0;edge<GP32_ARRAY_COUNT(boundaries);++edge)
        for(int delta=-4;delta<=4;++delta)
            for(unsigned length=0;length<GP32_ARRAY_COUNT(lengths);++length) {
                uint32_t addr=(uint32_t)((int64_t)boundaries[edge]+delta),len=lengths[length];
                uint64_t end=(uint64_t)addr+len;
                if(end>UINT64_C(4294967296))end=UINT64_C(4294967296);
                unsigned hit=0u;
                for(unsigned i=0;i<intervals;++i)
                    if(len && (uint64_t)addr<hi[i] && lo[i]<end)hit=1u;
                uint32_t epoch=cpu->jit_cache_epoch;
                arm920t_invalidate_icache_range(cpu,addr,len);
                check(cpu->jit_cache_epoch!=epoch,hit,"external clipped interval");
                complete_case();
            }
    finish_cpu(cpu);
}
static void stitched_leaf(uint32_t pa,unsigned mode,unsigned mmu,unsigned cache,unsigned callback,unsigned external_write) {
    current_group=PUBLIC_BL; shape=50u+external_write;
    arm920t_t *cpu=setup(pa,mode,mmu,cache,callback);
    uint32_t caller=!mmu && pa<0x02000000u ? 0x1000u : ARM_JIT_RAM_BASE_ADDR+0x1000u;
    uint32_t leaf=pa;
    if(mmu) {
        leaf=0x0c900000u+(pa&0xfffffu);
        gp32_st32le(f.bios+0x4000u+0x0c9u*4u,(pa&0xfff00000u)|0xc02u);
    }
    int64_t displacement=(int64_t)leaf-caller-8;
    check(displacement>=-33554432 && displacement<=33554428,1u,"representable BL displacement");
    gp32_st32le(backing(caller,4u),0xeb000000u|((uint32_t)(displacement/4)&0xffffffu));
    gp32_st32le(backing(caller+4u,4u),0xe12fff1cu);
    gp32_st32le(backing(pa,4u),0xe3a00011u);
    gp32_st32le(backing(pa+4u,4u),0xe12fff1eu); /* BX lr */
    /* Only public CPU entry points prime this path. A normal RAM/BIOS BL
     * produces a cached trace containing the extra-memory callee. */
    for(unsigned warm=0;warm<ARM_JIT_COLD_PORTABLE_OBSERVATIONS+2u;++warm) {
        arm920t_set_reg(cpu,15u,caller);
        check(arm920t_run(cpu,4u),4u,"stitched warm-up retirement");
        check(arm920t_get_reg(cpu,0u),0x11u,"stitched old immediate");
        check(arm920t_get_reg(cpu,15u),RETURN_TO,"stitched warm-up return");
    }
    if(mode) {
        unsigned found=0u;
        arm_jit_block_t *first=&cpu->jit_blocks[arm_jit_block_index(caller)];
        for(unsigned way=0;way<ARM_JIT_BLOCK_WAYS;++way) {
            arm_jit_block_t *b=first+way;
            if(!b->valid || b->tag_pc!=caller)continue;
            const arm_jit_op_t *ops=arm_jit_ops(cpu,b);
            for(unsigned op=0;op<b->count;++op)found|=ops[op].pc==leaf;
        }
        check(found,1u,"ordinary dispatcher included extra leaf");
        if(mode==1u)check(cpu->prof.block_interp_arm_insns!=0u,1u,"public cached coverage");
#if ARM920T_NATIVE_BACKEND
        if(mode==2u)check(cpu->prof.native_arm_insns!=0u,1u,"public native coverage");
#endif
    }
    uint32_t epoch=cpu->jit_cache_epoch;
    if(external_write) {
        gp32_st32le(backing(pa,4u),0xe3a00022u);
        arm920t_invalidate_icache_range(cpu,pa,4u);
    } else {
        gp32_st32le(f.bios+WRITER,stores[0]);
        gp32_st32le(f.bios+WRITER+4u,0xe12fff1bu);
        arm920t_set_reg(cpu,1u,0xe3a00022u); arm920t_set_reg(cpu,7u,data_va());
        arm920t_set_reg(cpu,15u,WRITER);
        check(arm920t_run(cpu,2u),2u,"public writer retirement");
        check(f.writes,callback,"public writer callbacks");
    }
    check(read32(NULL,pa),0xe3a00022u,"public physical replacement");
    if(cache && mode && !external_write)check(cpu->jit_cache_epoch,epoch,"public I=1 maintenance policy");
    arm920t_set_reg(cpu,15u,caller);
    check(arm920t_run(cpu,4u),4u,"public call after write");
    check(arm920t_get_reg(cpu,0u),cache && mode && !external_write ? 0x11u : 0x22u,"public call observes required code");
    check(arm920t_get_reg(cpu,15u),RETURN_TO,"public return after write");
    finish_cpu(cpu); complete_case();
}
int main(void) {
    const uint32_t locations[]={0x2000u,ARM_JIT_RAM_BASE_ADDR+0x2000u,
        0x01002000u,0x14002000u,0x20002000u,0xffffff00u};
    for(unsigned location=0;location<GP32_ARRAY_COUNT(locations);++location)
        for(unsigned mode=0;mode<3u;++mode)
            for(unsigned mmu=0;mmu<2u;++mmu)
                for(unsigned callback=0;callback<2u;++callback) {
                    for(unsigned cache=0;cache<2u;++cache)
                        for(unsigned store=0;store<GP32_ARRAY_COUNT(stores);++store)
                            replacement(locations[location],mode,mmu,cache,callback,store,0u);
                    active(locations[location],mode,mmu,callback);
                    if(mode)for(unsigned raw=1u;raw<3u;++raw)
                        replacement(locations[location],mode,mmu,0u,callback,0u,raw);
                }
    const uint32_t crossing[]={0u,0x80000u,ARM_JIT_RAM_BASE_ADDR,ARM_JIT_RAM_BASE_ADDR+ARM_JIT_RAM_SIZE_BYTES};
    for(unsigned edge=0;edge<GP32_ARRAY_COUNT(crossing);++edge)
        for(unsigned mode=0;mode<3u;++mode)
            for(unsigned cache=0;cache<2u;++cache)boundary_halfword(crossing[edge],mode,cache);
    const uint32_t span_code[]={0x2000u,0x01002000u,ARM_JIT_RAM_BASE_ADDR+0x2000u};
    for(unsigned location=0;location<GP32_ARRAY_COUNT(span_code);++location)
        for(unsigned mode=1u;mode<3u;++mode)
            for(unsigned cache=0;cache<2u;++cache)external_spans(span_code[location],mode,cache);
    const uint32_t leaves[]={0x2000u,0x01002000u,0x0bfff000u,0x0c802000u};
    for(unsigned location=0;location<GP32_ARRAY_COUNT(leaves);++location)
        for(unsigned mode=0;mode<3u;++mode)
            for(unsigned mmu=0;mmu<2u;++mmu)
                for(unsigned cache=0;cache<2u;++cache)
                    for(unsigned callback=0;callback<2u;++callback)
                        for(unsigned host=0;host<2u;++host)
                            stitched_leaf(leaves[location],mode,mmu,cache,callback,host);
    for(unsigned location=0;location<GP32_ARRAY_COUNT(locations);++location)
        for(unsigned mode=0;mode<3u;++mode)
            for(unsigned mmu=0;mmu<2u;++mmu)
                for(unsigned cache=0;cache<2u;++cache)
                    for(unsigned bytes=1u;bytes<=4u;bytes*=2u)
                        external(locations[location],mode,mmu,cache,bytes);
    const uint32_t ordinary_code[]={0x2000u,ARM_JIT_RAM_BASE_ADDR+0x2000u,0x14002000u};
    for(unsigned location=0;location<GP32_ARRAY_COUNT(ordinary_code);++location)
        for(unsigned mode=0;mode<3u;++mode)
            for(unsigned mmu=0;mmu<2u;++mmu)
                for(unsigned cache=0;cache<2u;++cache) {
                    ordinary_data(ordinary_code[location],mode,mmu,cache,ARM_JIT_RAM_BASE_ADDR+0x6000u);
                    if(location<2u)ordinary_data(ordinary_code[location],mode,mmu,cache,0x14006000u);
                }
    printf("arm_nonram_code_write_test: %u cases, %u failures\n",cases,failures);
    for(unsigned mode=0;mode<3u;++mode)
        printf("mode %u: %u cases, %u failures\n",mode,cases_by_mode[mode],failures_by_mode[mode]);
    for(unsigned group=0;group<GROUP_COUNT;++group)
        printf("%s: %u cases, %u failures\n",groups[group].name,groups[group].cases,groups[group].failures);
    printf("positive cached=%" PRIu64 " native=%" PRIu64 " instructions\n",cached_insns,native_insns);
    return failures!=0u;
}
