#define GP32EMU_CPU_PROFILE 1
#include "../src/arm920t.c"
#define main original_main
#include "arm_jit_test.c"
#undef main

static uint32_t translate(arm920t_t *c,uint32_t va,int instruction) {
    return instruction?instruction_translate(c,va):mmu_translate(c,va);
}
static void case_conflicting_refill(void) {
    const uint32_t va=0x10000000u,ttb=RAM_BASE+0x4000u,l2=RAM_BASE+0x8000u;
    for(int variant=0;variant<4;++variant) {
        int instruction=variant&1, preserved=variant>>1;
        current_case="mapping size change preserves reloadable machine state";
        setup_pair();cpu_ref->cp15[1]=1;cpu_ref->cp15[2]=ttb;
        if(preserved)cp15_write(cpu_ref,10u,0u,0u,instruction,1u);
        set_mem_both(ttb+0x400u,l2|1u);set_mem_both(l2,(RAM_BASE+0x30000u)|2u);
        CHECK(translate(cpu_ref,va,instruction)==RAM_BASE+0x30000u,"prime old small page");
        if(preserved)cp15_write(cpu_ref,10u,0u,0u,instruction,0x04100000u);
        uint32_t epoch=cpu_ref->jit_cache_epoch;
        uint32_t generation=cpu_ref->jit_generation;
        set_mem_both(ttb+0x400u,(RAM_BASE+0x100000u)|2u);
        CHECK(translate(cpu_ref,va+0x1000u,instruction)==RAM_BASE+0x101000u,"miss discovers new section");
        CHECK(translate(cpu_ref,va,instruction)==(preserved?RAM_BASE+0x30000u:RAM_BASE+0x100000u),"conflict policy preserves locked mappings only");
        if(instruction)CHECK(((cpu_ref->jit_cache_epoch==epoch) && (cpu_ref->jit_generation==generation))==preserved,"removed instruction mapping revokes native proof");
        if(preserved)CHECK(cp15_read(cpu_ref,10u,0u,0u,instruction)==0x04100000u,"uncached refill leaves lockdown and victim unchanged");
        set_mem_both(RAM_BASE+0x101000u,0xe3a00007u);
        if(instruction && preserved) {
            CHECK(arm_jit_prepare_fetch(cpu_ref,va+0x1000u)==1,"uncached instruction executes through checked fallback");
            CHECK(cpu_ref->r[0]==7 && cpu_ref->r[15]==va+0x1004u,"fallback executes fetched instruction once");
        }
        state_io_t count=state_io_counter();CHECK(arm920t_state_save_io(cpu_ref,&count),"size conflict state");
        uint8_t *saved=malloc(count.pos);CHECK(saved!=NULL,"allocate conflict state");
        if(saved) {
            state_io_t io=state_io_writer(saved,count.pos);
            CHECK(arm920t_state_save_io(cpu_ref,&io),"write actual runtime state");
            io=state_io_reader(saved,count.pos);
            CHECK(arm920t_state_load_io(cpu_ref,&io),"runtime-generated state must reload");
            free(saved);
        }
        printf("conflicting refill instruction=%d preserved=%d failures=%d\n",instruction,preserved,failures);
        teardown_pair();
    }
}
static arm920t_t *mutation_cpu;
static int mutation_side,mutation_once;
static uint32_t mutation_va;
static uint32_t table_read_mutation(void *user,uint32_t address) {
    (void)user;(void)address;
    if(!mutation_once++) {
        if(mutation_side)itlb_store_checked(mutation_cpu,mutation_va,RAM_BASE,0xfffffu,NULL);
        else tlb_store_checked(mutation_cpu,mutation_va,RAM_BASE,0xfffffu,NULL);
    }
    return (RAM_BASE+0x100000u)|2u;
}
static void case_walk_callback(void) {
    for(unsigned side=0;side<2;++side) {
        current_case="table read callback revokes cold miss certificate";
        setup_pair();mutation_cpu=cpu_ref;mutation_side=side;mutation_once=0;mutation_va=0x10000000u;
        cpu_ref->cp15[1]=1;cpu_ref->cp15[2]=RAM_BASE+0x4000u;
        cpu_ref->bus.fastmem=NULL;cpu_ref->bus.read32=table_read_mutation;
        CHECK(translate(cpu_ref,mutation_va,side)==RAM_BASE+0x100000u,"walk returns new section mapping");
        arm920t_translation_state_image_t image;translation_capture(cpu_ref,&image);
        CHECK(translation_valid(&image),"callback-inserted conflict must still be detected");
        CHECK(translate(cpu_ref,mutation_va,side)==RAM_BASE+0x100000u,"later hit uses coherent new mapping");
        printf("callback side=%u reads=%d failures=%d\n",side,mutation_once,failures);
        teardown_pair();
    }
}
static void case_restored_minimum(void) {
    const uint32_t va=0x10000000u,ttb=RAM_BASE+0x4000u,l2=RAM_BASE+0x8000u;
    for(unsigned side=0;side<2;++side) {
        current_case="restored smaller page must prevent false fast rejection";
        setup_pair();cpu_ref->cp15[1]=1;cpu_ref->cp15[2]=ttb;
        set_mem_both(ttb+0x400u,l2|1u);set_mem_both(l2,(RAM_BASE+0x30000u)|2u);
        CHECK(translate(cpu_ref,va,side)==RAM_BASE+0x30000u,"prime small mapping");
        state_io_t count=state_io_counter();CHECK(arm920t_state_save_io(cpu_ref,&count),"count minimum state");
        uint8_t *saved=malloc(count.pos);CHECK(saved!=NULL,"allocate minimum state");
        if(saved) {
            state_io_t io=state_io_writer(saved,count.pos);CHECK(arm920t_state_save_io(cpu_ref,&io),"save minimum state");
            arm920t_reset(cpu_ref,0);io=state_io_reader(saved,count.pos);
            CHECK(arm920t_state_load_io(cpu_ref,&io),"restore small mapping after reset");free(saved);
            set_mem_both(ttb+0x400u,(RAM_BASE+0x100000u)|2u);
            CHECK(translate(cpu_ref,va+0x1000u,side)==RAM_BASE+0x101000u,"refill after restore");
            CHECK(translate(cpu_ref,va,side)==RAM_BASE+0x100000u,"restored small-page conflict removed");
            arm920t_translation_state_image_t image;translation_capture(cpu_ref,&image);
            CHECK(translation_valid(&image),"post-restore banks remain serializable");
        }
        teardown_pair();
    }
    current_case="legacy import rebuilds conservative minimum";setup_pair();
    arm920t_state_image_t *legacy=calloc(1,sizeof(*legacy));CHECK(legacy!=NULL,"allocate legacy minimum fixture");
    if(legacy) {
        legacy->cpsr=0xd3;legacy->cp15[1]=1;legacy->cp15[2]=ttb;
        legacy->tlb_valid[0]=1;legacy->tlb_va_base[0]=va;legacy->tlb_pa_base[0]=RAM_BASE+0x30000u;legacy->tlb_mask[0]=0xfffu;
        arm920t_state_apply(cpu_ref,legacy);free(legacy);
        set_mem_both(ttb+0x400u,(RAM_BASE+0x100000u)|2u);
        CHECK(mmu_translate(cpu_ref,va+0x1000u)==RAM_BASE+0x101000u,"legacy refill");
        CHECK(mmu_translate(cpu_ref,va)==RAM_BASE+0x100000u,"legacy smaller-page conflict removed");
    }
    teardown_pair();
}

static void entry(arm920t_state_image_t *st,unsigned index,uint32_t va,uint32_t pa,uint32_t mask) {
    st->tlb_valid[index]=1;st->tlb_va_base[index]=va;
    st->tlb_pa_base[index]=pa;st->tlb_mask[index]=mask;
}
static unsigned banks(arm920t_t *c) {
    unsigned n=0;for(unsigned i=0;i<64;++i)n+=c->dtlb_bank[i].valid!=0;return n;
}
static void serializable(arm920t_t *c) {
    state_io_t count=state_io_counter();CHECK(arm920t_state_save_io(c,&count),"count migrated state");
    uint8_t *data=malloc(count.pos);CHECK(data!=NULL,"allocate migrated state");
    if(!data)return;
    state_io_t io=state_io_writer(data,count.pos);
    CHECK(arm920t_state_save_io(c,&io),"save migrated state");
    io=state_io_reader(data,count.pos);
    CHECK(arm920t_state_load_io(c,&io) && io.pos==count.pos,"reload migrated state");
    free(data);
}
static void reachable_ranges(void) {
    const uint32_t va=0x10000000u,ttb=RAM_BASE+0x4000u,l2=RAM_BASE+0x8000u;
    const uint32_t masks[]={0xfffffu,0xffffu,0xfffu,0x3ffu};
    for(unsigned shape=0;shape<4;++shape) {
        current_case="legacy import preserves only reachable pages";setup_pair();
        arm920t_state_image_t *st=calloc(1,sizeof(*st));CHECK(st!=NULL,"allocate legacy prefix");
        if(!st){teardown_pair();return;}
        st->cp15[1]=1;st->cp15[2]=ttb;st->cpsr=0xd3;st->r[4]=0x12345678;st->cycles_total=765432;
        set_mem_both(ttb+0x400u,l2|1u);
        for(unsigned i=0;i<256;++i)set_mem_both(l2+4u*i,(RAM_BASE+0x200000u+i*0x1000u)|2u);
        unsigned slot=13;uint32_t mask=masks[shape];
        uint32_t base=(va+slot*0x1000u)&~mask;
        uint32_t phys=RAM_BASE&~mask;
        if(mask==0x3ffu)base+=0x400u;
        entry(st,slot,base,phys,mask);
        arm920t_state_apply(cpu_ref,st);
        CHECK(banks(cpu_ref)==1,"one reachable slot stays one bank");
        CHECK(cpu_ref->r[4]==st->r[4] && cpu_ref->cycles_total==st->cycles_total,"preserve guest registers and cycles");
        serializable(cpu_ref);
        uint32_t hit=base|((slot<<12)&mask)|0x24u;
        CHECK(mmu_translate(cpu_ref,hit)==(phys|(hit&mask)),"saved slot keeps its original physical address");
        uint32_t neighbor=va+(slot+1u)*0x1000u+0x24u;
        CHECK(mmu_translate(cpu_ref,neighbor)==RAM_BASE+0x200000u+(neighbor-va),
              "uncached neighboring page must consult restored page tables");
        serializable(cpu_ref);
        printf("legacy shape=%x failures=%d\n",mask,failures);
        free(st);teardown_pair();
    }
}
static void bounds_and_history(void) {
    for(unsigned variant=0;variant<5;++variant) {
        current_case="legacy slot count and invalid history";setup_pair();
        arm920t_state_image_t *st=calloc(1,sizeof(*st));CHECK(st!=NULL,"allocate old state");
        if(!st){teardown_pair();return;}st->cpsr=0xd3;
        entry(st,0,0x10000000u,RAM_BASE,0xfffffu);
        unsigned expected=0;
        switch(variant) {
        case 0:entry(st,1,0x10000000u,RAM_BASE+0x100000u,0xfffffu);expected=2;break;
        case 1:for(unsigned i=1;i<65;++i)entry(st,i,0x10000000u,RAM_BASE,0xfffffu);break;
        case 2:st->tlb_mask[0]=0x7ffu;break;
        case 3:st->tlb_pa_base[0]|=4u;break;
        default:entry(st,0,0x10000000u,RAM_BASE,0x3ffu);
            entry(st,1,0x10000400u,RAM_BASE+0x400u,0x3ffu);expected=1;break;
        }
        arm920t_state_apply(cpu_ref,st);
        CHECK(banks(cpu_ref)==expected,"capacity and unreachable slot policy");
        serializable(cpu_ref);free(st);teardown_pair();
    }
}
static void same_descriptor_refill(void) {
    current_case="legacy narrow page followed by same large descriptor";
    setup_pair();
    const uint32_t va=0x10000000u,ttb=RAM_BASE+0x4000u;
    arm920t_state_image_t *st=calloc(1,sizeof(*st));CHECK(st!=NULL,"allocate old state");
    if(!st){teardown_pair();return;}
    st->cpsr=0xd3;st->cp15[1]=1;st->cp15[2]=ttb;
    entry(st,13,va,RAM_BASE,0xfffffu);
    set_mem_both(ttb+0x400u,RAM_BASE|2u);
    arm920t_state_apply(cpu_ref,st);
    CHECK(mmu_translate(cpu_ref,va+0xe024u)==RAM_BASE+0xe024u,"adjacent miss refills unchanged section");
    serializable(cpu_ref);
    CHECK(mmu_translate(cpu_ref,va+0xd024u)==RAM_BASE+0xd024u,"overlap still agrees after restore");
    arm920t_translation_state_image_t image;translation_capture(cpu_ref,&image);
    CHECK(translation_valid(&image),"same-mapping overlap accepted");
    image.data[1].pa_base+=0x100000u;
    CHECK(!translation_valid(&image),"conflicting overlap rejected");
    state_io_t count=state_io_counter();CHECK(arm920t_state_save_io(cpu_ref,&count),"count conflict fixture");
    uint8_t *good=malloc(count.pos),*bad=malloc(count.pos),*after=malloc(count.pos);
    CHECK(good && bad && after,"allocate conflict fixture");
    if(good && bad && after) {
        state_io_t io=state_io_writer(good,count.pos);
        CHECK(arm920t_state_save_io(cpu_ref,&io),"capture before malformed restore");
        memcpy(bad,good,count.pos);memcpy(bad+sizeof(*st),&image,sizeof(image));
        io=state_io_reader(bad,count.pos);
        CHECK(!arm920t_state_load_io(cpu_ref,&io),"loader rejects conflicting overlap");
        io=state_io_writer(after,count.pos);
        CHECK(arm920t_state_save_io(cpu_ref,&io) && !memcmp(good,after,count.pos),"rejected overlap leaves CPU unchanged");
    }
    free(good);free(bad);free(after);
    free(st);teardown_pair();
}

static void case_instruction_lookup_invalidation(void) {
    const uint32_t masks[]={0x3ffu,0xfffu,0xffffu,0xfffffu};
    const uint32_t bases[]={0x10000000u,0xfff00000u};
    for(unsigned m=0;m<4u;++m)for(unsigned b=0;b<2u;++b) {
        current_case="instruction invalidation clears its owners and preserves aliases";
        setup_pair();
        const uint32_t va=bases[b],mask=masks[m];
        itlb_store(cpu_ref,va,RAM_BASE,mask);
        for(uint32_t offset=0;offset<=mask;offset+=0x1000u)
            (void)itlb_install_lookup(cpu_ref,va+offset,0u);
        /* Flip the high hash salt and the matching low index bit together. */
        uint32_t collision=va^0x01001000u,outside=va^0x00800000u;
        itlb_store(cpu_ref,collision,RAM_BASE+0x100000u,0xfffu);
        itlb_store(cpu_ref,outside,RAM_BASE+0x200000u,0xfffu);
        CHECK(arm_itlb_index(collision)==arm_itlb_index(va),"fixture creates a real derived collision");
        uint8_t owners[4096];arm_tlb_entry_t entries[4096];
        memcpy(owners,cpu_ref->itlb_owner,sizeof(owners));
        memcpy(entries,cpu_ref->itlb_entry,sizeof(entries));
        cpu_ref->itlb_preserved|=1u;
        cp15_write(cpu_ref,8u,5u,0u,1u,va);
        int exact=1;
        const arm_tlb_entry_t empty={0};
        for(unsigned i=0;i<4096u;++i) {
            unsigned wanted=owners[i]==1u?0u:owners[i];
            const arm_tlb_entry_t *entry=owners[i]==1u?&empty:&entries[i];
            if(cpu_ref->itlb_owner[i]!=wanted || memcmp(&cpu_ref->itlb_entry[i],entry,sizeof(*entry)))exact=0;
        }
        CHECK(exact,"remove every owned lookup without changing any other slot");
        CHECK(!(cpu_ref->itlb_preserved&1u) && !cpu_ref->itlb_bank[0].valid,"selected invalidation removes preserved entry");
        CHECK(itlb_find(cpu_ref,collision)!=NULL && itlb_find(cpu_ref,outside)!=NULL,"other mappings remain resident");
        teardown_pair();
    }
}

static void case_fine_page_tables(void) {
    const uint32_t ttb=RAM_BASE+0x4000u,table=RAM_BASE+0x9000u;
    const uint32_t sizes[]={65536u,4096u,1024u};
    const unsigned types[]={1u,2u,3u};
    const uint32_t addresses[]={0x10034c80u,0xfffffffcu};
    for(unsigned shape=0;shape<3u;++shape)for(unsigned high=0;high<2u;++high)
    for(unsigned side=0;side<2u;++side)for(unsigned slow=0;slow<2u;++slow) {
        current_case="fine page table translates instructions and data";
        setup_pair();
        uint32_t va=addresses[high],size=sizes[shape];
        uint32_t physical=RAM_BASE+0x200000u,offset=va%size;
        cpu_ref->cp15[1]=1;cpu_ref->cp15[2]=ttb;
        /* DDI0151C: fine tables contain 1024 descriptors per MiB.
         * Repeat large/small descriptors over all of their 1KiB slots. */
        set_mem_both(ttb+4u*(va/0x100000u),table|0x1e0u|3u);
        unsigned first=(va%0x100000u)/size*(size/1024u);
        for(unsigned n=0;n<size/1024u;++n)
            set_mem_both(table+4u*(first+n),physical|types[shape]);
        set_mem_both(physical+offset,0xe3a00007u);
        if(slow)cpu_ref->bus.fastmem=NULL;
        CHECK(translate(cpu_ref,va,side)==physical+offset,"fine walk returns mapped address");
        CHECK(translate(cpu_ref,va,side)==physical+offset,"fine mapping survives cache hit");
        if(side)CHECK(fetch32(cpu_ref,va)==0xe3a00007u,"instruction fetch uses fine mapping");
        serializable(cpu_ref);
        CHECK(translate(cpu_ref,va,side)==physical+offset,"fine mapping survives state reload");
        /* An adjacent descriptor must not be absorbed into the cached page. */
        uint32_t neighbor=(va-va%size)^size;
        unsigned neighbor_slot=(neighbor%0x100000u)/1024u;
        set_mem_both(table+4u*neighbor_slot,(RAM_BASE+0x300000u)|3u);
        CHECK(translate(cpu_ref,neighbor,side)==RAM_BASE+0x300000u,"adjacent fine tiny page stays independent");
        teardown_pair();
    }
}

int main(void) {
    case_conflicting_refill(); case_walk_callback(); case_restored_minimum();
    reachable_ranges(); bounds_and_history(); same_descriptor_refill();
    case_instruction_lookup_invalidation();
    case_fine_page_tables();
    printf("translation regression failures=%d\n",failures);
    return failures?1:0;
}
