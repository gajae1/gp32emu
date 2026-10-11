/* Linux-only libnx contract shim: genuinely distinct RW/RX mappings. */
#pragma once
#include <stddef.h>
#include <stdbool.h>
typedef unsigned Result;
#define R_FAILED(rc) ((rc) != 0)
#define R_SUCCEEDED(rc) ((rc) == 0)
typedef enum { JitType_SetProcessMemoryPermission, JitType_CodeMemory } JitType;
typedef struct {
    JitType type;
    size_t size;
    void *rw_addr, *rx_addr;
    bool is_executable;
} Jit;
Result jitCreate(Jit *j, size_t size);
Result jitTransitionToWritable(Jit *j);
Result jitTransitionToExecutable(Jit *j);
Result jitClose(Jit *j);
static inline void *jitGetRwAddr(Jit *j) { return j->rw_addr; }
static inline void *jitGetRxAddr(Jit *j) { return j->rx_addr; }
void armDCacheFlush(void *p, size_t size);
void armICacheInvalidate(void *p, size_t size);
extern int switch_test_fail_create, switch_test_fail_write, switch_test_fail_publish;
extern unsigned switch_test_live_buffers;
