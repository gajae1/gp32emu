/* The preparation thread owns only unpublished host allocation. It never sees
 * the CPU, guest memory, code cache or slot table. Taking a spare transfers its
 * ownership under the mutex; only the CPU links it into its stable slab list.
 * A missed spare falls back immediately to malloc, without waiting for work. */
typedef struct arm_jit_storage_worker arm_jit_storage_worker_t;

#if ARM_JIT_STORAGE_THREADS
struct arm_jit_storage_worker {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t wake;
    int stop, count;
    arm_jit_op_slab_t *spare[2];
#if ARM_JIT_CHUNK_PREPARE
    size_t chunk_bytes; /* one request, including preparation or ready storage */
    uint8_t *chunk;     /* ready private mapping; ownership passes on take */
    int chunk_failed;
#endif
};

static void *arm_jit_storage_prepare(void *opaque) {
    arm_jit_storage_worker_t *w = opaque;
#if ARM_JIT_CHUNK_PREPARE
    uint8_t *chunk = NULL; /* worker-private until every page is touched */
    size_t touched = 0, bytes = 0;
#endif
    pthread_mutex_lock(&w->mutex);
    while (!w->stop) {
        if (w->count == 2) {
#if ARM_JIT_CHUNK_PREPARE
            if (w->chunk_bytes && !w->chunk && !w->chunk_failed) {
                bytes = w->chunk_bytes;
                pthread_mutex_unlock(&w->mutex);
                if (!chunk) {
                    long page = sysconf(_SC_PAGESIZE);
                    if (page > 0 && bytes % (size_t)page == 0) {
                        /* Match the Linux arena's final permissions before
                         * prefaulting. Changing RW pages to executable on the
                         * owner can trigger another per-page dirty fault and
                         * cache synchronization when code is copied later.
                         * This mapping remains unpublished and worker-owned. */
                        void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE | PROT_EXEC,
                                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                        if (p != MAP_FAILED) chunk = p;
                    }
                }
                if (chunk) {
                    size_t end = bytes - touched > 65536u ? touched + 65536u : bytes;
                    volatile uint8_t *p = chunk;
                    for (size_t at = touched; at < end; at += 4096u) p[at] = 0;
                    p[end - 1u] = 0;
                    touched = end;
                }
                pthread_mutex_lock(&w->mutex);
                if (!chunk) { w->chunk_failed = 1; w->chunk_bytes = 0; }
                else if (touched == bytes && !w->stop) {
                    w->chunk = chunk;
                    chunk = NULL;
                    touched = 0;
                }
                /* Refill decoded spares before the next <=64 KiB quantum. */
                continue;
            }
#endif
            pthread_cond_wait(&w->wake, &w->mutex);
            continue;
        }
        pthread_mutex_unlock(&w->mutex);
        arm_jit_op_slab_t *slab = malloc(sizeof(*slab));
        if (slab) {
            /* Volatile touches cannot be folded into lazy calloc. The bytes
             * are initialized normally before use; no published slab is ever
             * touched here. A 4 KiB stride also covers larger host pages. */
            volatile unsigned char *p = (volatile unsigned char *)slab;
            for (size_t at = 0; at < sizeof(*slab); at += 4096u) p[at] = 0;
            p[sizeof(*slab) - 1u] = 0;
        }
        pthread_mutex_lock(&w->mutex);
        if (w->stop || !slab) {
#if ARM_JIT_CHUNK_PREPARE
            w->chunk_failed = 1; /* do not enqueue work after producer failure */
#endif
            free(slab);
            break;
        }
        w->spare[w->count++] = slab;
    }
    pthread_mutex_unlock(&w->mutex);
#if ARM_JIT_CHUNK_PREPARE
    if (chunk) munmap(chunk, bytes);
#endif
    return NULL;
}

static arm_jit_storage_worker_t *arm_jit_storage_start(void) {
    arm_jit_storage_worker_t *w = calloc(1, sizeof(*w));
    if (!w) return NULL;
    if (pthread_mutex_init(&w->mutex, NULL)) { free(w); return NULL; }
    if (pthread_cond_init(&w->wake, NULL)) {
        pthread_mutex_destroy(&w->mutex);
        free(w);
        return NULL;
    }
    if (pthread_create(&w->thread, NULL, arm_jit_storage_prepare, w)) {
        pthread_cond_destroy(&w->wake);
        pthread_mutex_destroy(&w->mutex);
        free(w);
        return NULL;
    }
    return w;
}

static void arm_jit_storage_stop(arm_jit_storage_worker_t *w) {
    if (!w) return;
    pthread_mutex_lock(&w->mutex);
    w->stop = 1;
    pthread_cond_signal(&w->wake);
    pthread_mutex_unlock(&w->mutex);
    pthread_join(w->thread, NULL);
#if ARM_JIT_CHUNK_PREPARE
    if (w->chunk) munmap(w->chunk, w->chunk_bytes);
#endif
    for (int i = 0; i < w->count; ++i) free(w->spare[i]);
    pthread_cond_destroy(&w->wake);
    pthread_mutex_destroy(&w->mutex);
    free(w);
}

static arm_jit_op_slab_t *arm_jit_storage_take(arm_jit_storage_worker_t *w) {
    arm_jit_op_slab_t *slab = NULL;
    if (w && pthread_mutex_trylock(&w->mutex) == 0) {
        if (w->count) slab = w->spare[--w->count];
        pthread_cond_signal(&w->wake);
        pthread_mutex_unlock(&w->mutex);
    }
    return slab ? slab : malloc(sizeof(*slab));
}

#if ARM_JIT_CHUNK_PREPARE
static void arm_jit_storage_request_chunk(arm_jit_storage_worker_t *w, size_t bytes) {
    if (!w || pthread_mutex_trylock(&w->mutex)) return;
    if (!w->stop && !w->chunk_failed && !w->chunk_bytes) {
        w->chunk_bytes = bytes;
        pthread_cond_signal(&w->wake);
    }
    pthread_mutex_unlock(&w->mutex);
}

static uint8_t *arm_jit_storage_take_chunk(arm_jit_storage_worker_t *w, size_t bytes) {
    uint8_t *p = NULL;
    if (!w || pthread_mutex_trylock(&w->mutex)) return NULL;
    if (w->chunk && w->chunk_bytes == bytes) {
        p = w->chunk;
        w->chunk = NULL;
        w->chunk_bytes = 0;
    }
    pthread_mutex_unlock(&w->mutex);
    return p;
}
#endif
#else
static arm_jit_storage_worker_t *arm_jit_storage_start(void) { return NULL; }
static void arm_jit_storage_stop(arm_jit_storage_worker_t *w) { GP32_UNUSED(w); }
static arm_jit_op_slab_t *arm_jit_storage_take(arm_jit_storage_worker_t *w) {
    GP32_UNUSED(w);
    return malloc(sizeof(arm_jit_op_slab_t));
}
#endif
