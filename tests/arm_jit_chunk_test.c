/* Real storage worker + CPU reset/state APIs. Only the OS mapping boundary is
 * replaced on Windows; Linux uses mmap/mprotect/mremap except injected faults. */
#define _GNU_SOURCE 1
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include "arm_jit_chunk_platform.h"
#else
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#include <time.h>
static void chunk_pause(void) { struct timespec t = {0, 1000000}; nanosleep(&t, NULL); }
static void chunk_make_hole(void *p, size_t bytes) { munmap(p, bytes); }
#endif

static atomic_int hold_map, entered_map, fail_map, fail_protect, fail_remap, fail_restore;
static atomic_int fail_malloc, fail_calloc, fail_thread, live_maps, remap_calls;
static _Atomic(void *) private_map;
static void *chunk_malloc(size_t n) { return atomic_load(&fail_malloc) ? NULL : malloc(n); }
static void *chunk_calloc(size_t n, size_t s) { return atomic_load(&fail_calloc) ? NULL : calloc(n,s); }
static int chunk_create(pthread_t *t, const void *a, void *(*fn)(void *), void *arg) {
    return atomic_load(&fail_thread) ? 1 : pthread_create(t,a,fn,arg);
}
static void *chunk_mmap(void *p, size_t n, int prot, int flags, int fd, long off) {
    int donor = !(prot & PROT_EXEC) && !(flags & MAP_FIXED);
    if (donor) {
        atomic_store(&entered_map, 1);
        while (atomic_load(&hold_map)) chunk_pause();
        if (atomic_load(&fail_map)) return MAP_FAILED;
    }
    if ((flags & MAP_FIXED) && atomic_load(&fail_restore)) return MAP_FAILED;
    void *v = mmap(p,n,prot,flags,fd,off);
    if (donor && v != MAP_FAILED) { atomic_store(&private_map,v); atomic_fetch_add(&live_maps,1); }
    return v;
}
static int chunk_munmap(void *p, size_t n) {
    if (p == atomic_load(&private_map)) { atomic_store(&private_map,NULL); atomic_fetch_sub(&live_maps,1); }
    return munmap(p,n);
}
static int chunk_mprotect(void *p, size_t n, int prot) {
    return atomic_load(&fail_protect) ? -1 : mprotect(p,n,prot);
}
static void *chunk_mremap(void *p, size_t old, size_t n, int flags, void *at) {
    atomic_fetch_add(&remap_calls,1);
    if (atomic_load(&fail_remap)) { chunk_make_hole(at,n); return MAP_FAILED; }
    void *r = mremap(p,old,n,flags,at);
    if (r != MAP_FAILED) { atomic_store(&private_map,NULL); atomic_fetch_sub(&live_maps,1); }
    return r;
}

/* Internal feature gates may be overridden only in this test translation unit. */
#define ARM_JIT_STORAGE_THREADS 1
#define ARM_JIT_CHUNK_PREPARE 1
#define GP32EMU_ENABLE_THREADS 1
#define ARM_JIT_CODE_SIZE (4u * 512u * 1024u)
#define malloc chunk_malloc
#define calloc chunk_calloc
#define pthread_create chunk_create
#define mmap chunk_mmap
#define munmap chunk_munmap
#define mprotect chunk_mprotect
#define mremap chunk_mremap
#include "../src/arm920t.c"
#undef malloc
#undef calloc
#undef pthread_create
#undef mmap
#undef munmap
#undef mprotect
#undef mremap

#ifndef ARM_JIT_CHUNKS_INCLUDED
int main(void) { fputs("FAIL: private RW chunk preparation is not implemented\n",stderr); return 1; }
#elif !ARM920T_NATIVE_BACKEND
int main(void) { puts("SKIP: native executable-memory backend unavailable"); return 77; }
#else
#define CHUNK (512u * 1024u)
#define REQUIRE(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); exit(1); } } while (0)
static uint8_t rd8(void *p,uint32_t a) { (void)p; (void)a; return 0; }
static uint16_t rd16(void *p,uint32_t a) { (void)p; (void)a; return 0; }
static uint32_t rd32(void *p,uint32_t a) { (void)p; (void)a; return 0xe1a0f00eu; }
static void wr8(void *p,uint32_t a,uint8_t v) { (void)p; (void)a; (void)v; }
static void wr16(void *p,uint32_t a,uint16_t v) { (void)p; (void)a; (void)v; }
static void wr32(void *p,uint32_t a,uint32_t v) { (void)p; (void)a; (void)v; }
static arm920t_t *new_cpu(void) {
    arm_bus_t bus = {.read8=rd8,.read16=rd16,.read32=rd32,.write8=wr8,.write16=wr16,.write32=wr32};
    arm920t_t *c=arm920t_create(&bus);
    if (!c) abort();
    c->jit_code_size=4u*CHUNK;
    c->jit_code=arm_jit_alloc_exec(c,c->jit_code_size);
    c->jit_storage_worker=arm_jit_storage_start(); c->jit_storage_started=1;
    if (!c->jit_code || !c->jit_storage_worker) abort();
    memset(c->jit_code,0x5a,c->jit_code_size);
    return c;
}
static int wait_phase(arm_jit_storage_worker_t *w, int ready) {
    for(unsigned n=0;n<5000u;++n) {
        pthread_mutex_lock(&w->mutex);
        int hit=ready ? w->chunk != NULL : w->chunk_failed;
        pthread_mutex_unlock(&w->mutex);
        if(hit) return 1;
        chunk_pause();
    }
    return 0;
}
static int wait_entered(void) {
    for(unsigned n=0;n<5000u;++n) { if(atomic_load(&entered_map))return 1; chunk_pause(); }
    return 0;
}
static void request_next(arm920t_t *c) {
    c->jit_code_used=CHUNK/2u;
    arm_jit_chunk_request(c,CHUNK,0,CHUNK);
}
static void apply_lifecycle(arm920t_t *c, unsigned event) {
    if(event==0) arm920t_reset(c,0);
    else if(event==1) {
        uint8_t image[sizeof(arm920t_state_image_t)+sizeof(arm920t_translation_state_image_t)];
        c->r[0]=0x12345678u;
        state_io_t io=state_io_writer(image,sizeof(image));
        REQUIRE(arm920t_state_save_io(c,&io));
        c->r[0]=0;
        io=state_io_reader(image,sizeof(image));
        REQUIRE(arm920t_state_load_io(c,&io));
        REQUIRE(c->r[0]==0x12345678u);
    } else {
        c->jit_generation=UINT32_MAX;
        arm920t_jit_invalidate_all(c,ARM_JIT_INV_API_FLUSH);
        REQUIRE(c->jit_generation==1u);
    }
}
static void test_lifecycle(void) {
    for(unsigned event=0;event<3u;++event) for(unsigned ready=0;ready<2u;++ready) {
        arm920t_t *c=new_cpu();
        REQUIRE(arm_jit_chunk_open(c,CHUNK,0,CHUNK));
        atomic_store(&entered_map,0); atomic_store(&hold_map,!ready);
        request_next(c);
        REQUIRE(ready ? wait_phase(c->jit_storage_worker,1) : wait_entered());
        apply_lifecycle(c,event);
        REQUIRE(c->jit_code_opened==CHUNK);
        REQUIRE(c->jit_chunk_requested==CHUNK);
        REQUIRE(arm_jit_chunk_open(c,CHUNK,0,CHUNK));
        REQUIRE(c->jit_code[0]==0x5a && c->jit_code[CHUNK-1u]==0x5a);
        if(!ready) {
            REQUIRE(arm_jit_chunk_open(c,CHUNK,CHUNK,2u*CHUNK)); /* late -> synchronous */
            REQUIRE(c->jit_code[CHUNK]==0x5a);
            atomic_store(&hold_map,0);
            REQUIRE(wait_phase(c->jit_storage_worker,1));
            apply_lifecycle(c,event);
            REQUIRE(arm_jit_chunk_open(c,CHUNK,CHUNK,2u*CHUNK)); /* cannot replace reused code */
            REQUIRE(c->jit_code[CHUNK]==0x5a);
        }
        size_t at=ready ? CHUNK : 2u*CHUNK;
        REQUIRE(arm_jit_chunk_open(c,CHUNK,at,at+CHUNK));
        REQUIRE(c->jit_code[at]==0 && c->jit_code[at+CHUNK-1u]==0);
        REQUIRE(c->jit_code[at-1u]==0x5a && c->jit_code[at+CHUNK]==0x5a);
        REQUIRE(atomic_load(&live_maps)==0);
        arm920t_destroy(c);
    }
}
static void test_failures(void) {
    atomic_store(&fail_calloc,1); REQUIRE(!arm_jit_storage_start()); atomic_store(&fail_calloc,0);
    atomic_store(&fail_thread,1); REQUIRE(!arm_jit_storage_start()); atomic_store(&fail_thread,0);
    for(unsigned failure=0;failure<4u;++failure) {
        arm920t_t *c=new_cpu();
        REQUIRE(arm_jit_chunk_open(c,CHUNK,0,CHUNK));
        atomic_store(&fail_map,failure==0);
        request_next(c);
        REQUIRE(wait_phase(c->jit_storage_worker,failure!=0));
        atomic_store(&fail_protect,failure==1);
        atomic_store(&fail_remap,failure>=2);
        atomic_store(&fail_restore,failure==3);
        REQUIRE(arm_jit_chunk_open(c,CHUNK,CHUNK,2u*CHUNK)==(failure!=3));
        REQUIRE(c->jit_code[0]==0x5a && c->jit_code[2u*CHUNK]==0x5a);
        REQUIRE(atomic_load(&live_maps)==0);
        if(failure==3) {
            apply_lifecycle(c,0); apply_lifecycle(c,1); apply_lifecycle(c,2);
            REQUIRE(!arm_jit_chunk_open(c,CHUNK,CHUNK,2u*CHUNK));
            REQUIRE(c->jit_code_unusable);
#if defined(ARM_JIT_NATIVE_A64)
            arm_jit_block_t b={.count=1};
            arm_jit_compile_host(c,&b); /* must return before reading any ops */
            REQUIRE(!b.native);
#endif
        } else {
            REQUIRE(!c->jit_code_unusable);
            if(failure<2u) REQUIRE(c->jit_code[CHUNK]==0x5a);
            /* A successful fallback must really restore a writable mapping,
             * not merely return success after the injected destructive remap. */
            volatile uint8_t *destination=c->jit_code+CHUNK;
            *destination=0x33;
            REQUIRE(*destination==0x33);
        }
        atomic_store(&fail_map,0); atomic_store(&fail_protect,0);
        atomic_store(&fail_remap,0); atomic_store(&fail_restore,0);
        arm920t_destroy(c);
    }
    atomic_store(&fail_malloc,1);
    arm_jit_storage_worker_t *w=arm_jit_storage_start(); REQUIRE(w);
    REQUIRE(arm_jit_storage_take(w)==NULL);
    arm_jit_storage_stop(w); atomic_store(&fail_malloc,0);
}
static void *destroy_cpu(void *p) { arm920t_destroy(p); return NULL; }
static void test_shutdown_and_contention(void) {
    for(unsigned ready=0;ready<2u;++ready) {
        arm920t_t *c=new_cpu();
        REQUIRE(arm_jit_chunk_open(c,CHUNK,0,CHUNK));
        atomic_store(&entered_map,0); atomic_store(&hold_map,!ready);
        request_next(c);
        REQUIRE(ready ? wait_phase(c->jit_storage_worker,1) : wait_entered());
        if(ready) {
            pthread_mutex_lock(&c->jit_storage_worker->mutex);
            REQUIRE(arm_jit_chunk_open(c,CHUNK,CHUNK,2u*CHUNK));
            pthread_mutex_unlock(&c->jit_storage_worker->mutex);
            REQUIRE(c->jit_code[CHUNK]==0x5a);
            arm920t_destroy(c);
        } else {
            pthread_t t; REQUIRE(!pthread_create(&t,NULL,destroy_cpu,c));
            int stopping=0;
            for(unsigned n=0;n<5000u && !stopping;++n) {
                pthread_mutex_lock(&c->jit_storage_worker->mutex);
                stopping=c->jit_storage_worker->stop;
                pthread_mutex_unlock(&c->jit_storage_worker->mutex);
                if(!stopping)chunk_pause();
            }
            REQUIRE(stopping);
            atomic_store(&hold_map,0); REQUIRE(!pthread_join(t,NULL));
        }
        REQUIRE(atomic_load(&live_maps)==0);
    }
    for(unsigned n=0;n<16u;++n) { arm_jit_storage_worker_t *w=arm_jit_storage_start(); REQUIRE(w); arm_jit_storage_stop(w); }
}
static unsigned native_value(uint8_t *p, unsigned value) {
#if defined(ARM_JIT_NATIVE_A64)
    uint32_t code[2]={0x52800000u | (value<<5),0xd65f03c0u};
#else
    uint8_t code[6]={0xb8,(uint8_t)value,0,0,0,0xc3};
#endif
    memcpy(p,code,sizeof(code)); arm_jit_flush_exec(p,sizeof(code));
    union { void *p; unsigned (*fn)(void); } f; f.p=p;
    return f.fn();
}
static void test_publication_and_bound(void) {
    arm920t_t *c=new_cpu();
    REQUIRE(arm_jit_chunk_open(c,CHUNK,0,CHUNK));
    REQUIRE(native_value(c->jit_code,11)==11);
    c->jit_code_used=CHUNK/2u-1u;
    arm_jit_chunk_request(c,CHUNK,0,CHUNK);
    pthread_mutex_lock(&c->jit_storage_worker->mutex);
    REQUIRE(c->jit_storage_worker->chunk_bytes==0);
    pthread_mutex_unlock(&c->jit_storage_worker->mutex);
    request_next(c); REQUIRE(wait_phase(c->jit_storage_worker,1));
    void *prepared=atomic_load(&private_map);
    arm_jit_storage_request_chunk(c->jit_storage_worker,CHUNK);
    REQUIRE(atomic_load(&private_map)==prepared && atomic_load(&live_maps)==1);
    REQUIRE(arm_jit_chunk_open(c,CHUNK,CHUNK,2u*CHUNK));
    REQUIRE(native_value(c->jit_code+CHUNK,29)==29);
    union { void *p; unsigned (*fn)(void); } first,second;
    first.p=c->jit_code; second.p=c->jit_code+CHUNK;
    REQUIRE(first.fn()==11);
    for(unsigned event=0;event<3u;++event) {
        apply_lifecycle(c,event);
        REQUIRE(arm_jit_chunk_open(c,CHUNK,CHUNK,2u*CHUNK));
        REQUIRE(first.fn()==11 && second.fn()==29);
    }
    REQUIRE(atomic_load(&live_maps)==0);
    arm920t_destroy(c);
}
int main(void) {
    test_lifecycle(); test_failures(); test_shutdown_and_contention(); test_publication_and_bound();
    printf("PASS: reset/state-load/wrap in BUILDING+READY; allocation/protection/remap failures; contention/shutdown (remaps=%d)\n",atomic_load(&remap_calls));
#ifdef _WIN32
    puts("Windows: real worker threads; Linux mapping syscalls modeled, not kernel-validated.");
#else
    puts("Linux: real pthreads and successful mmap/mprotect/mremap; failures injected.");
#endif
    return 0;
}
#endif
