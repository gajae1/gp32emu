#define _GNU_SOURCE
#include "switch.h"
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include <assert.h>

int switch_test_fail_create, switch_test_fail_write, switch_test_fail_publish;
unsigned switch_test_live_buffers;
Result jitCreate(Jit *j, size_t size) {
    if (switch_test_fail_create) return 1;
    size = (size + 4095u) & ~(size_t)4095u;
    int fd = memfd_create("gp32-switch-jit-test", 0);
    if (fd < 0) return 1;
    if (ftruncate(fd, (off_t)size)) { close(fd); return 1; }
    j->type = getenv("SWITCH_JIT_PERMISSION_MODE") ? JitType_SetProcessMemoryPermission : JitType_CodeMemory;
    j->size = size;
    j->rw_addr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    j->rx_addr = mmap(NULL, size, j->type == JitType_CodeMemory ? PROT_READ | PROT_EXEC : PROT_NONE, MAP_SHARED, fd, 0);
    close(fd);
    assert(j->rw_addr != MAP_FAILED && j->rx_addr != MAP_FAILED && j->rw_addr != j->rx_addr);
    j->is_executable = false;
    ++switch_test_live_buffers;
    return 0;
}
Result jitTransitionToWritable(Jit *j) {
    if (switch_test_fail_write) return 1;
    if (j->type == JitType_SetProcessMemoryPermission) {
        assert(!mprotect(j->rx_addr, j->size, PROT_NONE));
        assert(!mprotect(j->rw_addr, j->size, PROT_READ | PROT_WRITE));
    }
    j->is_executable = false;
    return 0;
}
Result jitTransitionToExecutable(Jit *j) {
    if (switch_test_fail_publish) return 1;
    armDCacheFlush(j->rw_addr, j->size);
    if (j->type == JitType_SetProcessMemoryPermission) {
        assert(!mprotect(j->rw_addr, j->size, PROT_NONE));
        assert(!mprotect(j->rx_addr, j->size, PROT_READ | PROT_EXEC));
    }
    armICacheInvalidate(j->rx_addr, j->size);
    j->is_executable = true;
    return 0;
}
Result jitClose(Jit *j) {
    assert(!munmap(j->rw_addr, j->size));
    assert(!munmap(j->rx_addr, j->size));
    --switch_test_live_buffers;
    return 0;
}
void armDCacheFlush(void *p, size_t size) { __builtin___clear_cache(p, (char *)p + size); }
void armICacheInvalidate(void *p, size_t size) { __builtin___clear_cache(p, (char *)p + size); }
