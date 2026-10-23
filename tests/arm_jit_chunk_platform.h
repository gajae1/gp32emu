/* Windows-only test adapter: real threads/locks and mapped storage. The fixed
 * remap is modeled by a copy, so this does NOT validate Linux kernel remapping. */
#include <windows.h>
#include <process.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef HANDLE pthread_t;
typedef SRWLOCK pthread_mutex_t;
typedef CONDITION_VARIABLE pthread_cond_t;
typedef struct { void *(*fn)(void *); void *arg; } chunk_thread_start_t;
static unsigned __stdcall chunk_thread_start(void *arg) {
    chunk_thread_start_t s = *(chunk_thread_start_t *)arg;
    free(arg); s.fn(s.arg); return 0;
}
static int pthread_create(pthread_t *t, const void *attr, void *(*fn)(void *), void *arg) {
    (void)attr;
    chunk_thread_start_t *s = malloc(sizeof(*s));
    if (!s) return 1;
    s->fn = fn; s->arg = arg;
    *t = (HANDLE)_beginthreadex(NULL, 0, chunk_thread_start, s, 0, NULL);
    if (!*t) { free(s); return 1; }
    return 0;
}
static int pthread_join(pthread_t t, void **result) {
    (void)result;
    DWORD r = WaitForSingleObject(t, INFINITE); CloseHandle(t);
    return r == WAIT_OBJECT_0 ? 0 : 1;
}
static int pthread_mutex_init(pthread_mutex_t *m, const void *a) {
    (void)a; InitializeSRWLock(m); return 0;
}
static int pthread_mutex_destroy(pthread_mutex_t *m) { (void)m; return 0; }
static int pthread_mutex_lock(pthread_mutex_t *m) { AcquireSRWLockExclusive(m); return 0; }
static int pthread_mutex_trylock(pthread_mutex_t *m) { return !TryAcquireSRWLockExclusive(m); }
static int pthread_mutex_unlock(pthread_mutex_t *m) { ReleaseSRWLockExclusive(m); return 0; }
static int pthread_cond_init(pthread_cond_t *c, const void *a) {
    (void)a; InitializeConditionVariable(c); return 0;
}
static int pthread_cond_destroy(pthread_cond_t *c) { (void)c; return 0; }
static int pthread_cond_signal(pthread_cond_t *c) { WakeConditionVariable(c); return 0; }
static int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) {
    return !SleepConditionVariableSRW(c, m, INFINITE, 0);
}
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define MAP_PRIVATE 2
#define MAP_ANONYMOUS 32
#define MAP_FIXED 16
#define MAP_FAILED ((void *)(intptr_t)-1)
#define MREMAP_MAYMOVE 1
#define MREMAP_FIXED 2
#define _SC_PAGESIZE 30
static long sysconf(int key) { (void)key; SYSTEM_INFO s; GetSystemInfo(&s); return s.dwPageSize; }
static void *mmap(void *at, size_t bytes, int prot, int flags, int fd, long off) {
    (void)fd; (void)off;
    DWORD p = (prot & PROT_EXEC) ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
    if (flags & MAP_FIXED) {
        DWORD old;
        return VirtualProtect(at, bytes, p, &old) ? at : MAP_FAILED;
    }
    void *v = VirtualAlloc(NULL, bytes, MEM_RESERVE | MEM_COMMIT, p);
    return v ? v : MAP_FAILED;
}
static int munmap(void *p, size_t bytes) { (void)bytes; return !VirtualFree(p, 0, MEM_RELEASE); }
static int mprotect(void *p, size_t bytes, int prot) {
    DWORD old;
    return !VirtualProtect(p, bytes, (prot & PROT_EXEC) ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE, &old);
}
static void *mremap(void *p, size_t old, size_t bytes, int flags, void *at) {
    (void)flags;
    if (old != bytes) return MAP_FAILED;
    memcpy(at, p, bytes); VirtualFree(p, 0, MEM_RELEASE); return at;
}
static void chunk_pause(void) { Sleep(1); }
static void chunk_make_hole(void *p, size_t bytes) { DWORD old; VirtualProtect(p, bytes, PAGE_NOACCESS, &old); }
