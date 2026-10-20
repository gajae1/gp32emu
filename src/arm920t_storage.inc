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
};

static void *arm_jit_storage_prepare(void *opaque) {
    arm_jit_storage_worker_t *w = opaque;
    pthread_mutex_lock(&w->mutex);
    while (!w->stop) {
        if (w->count == 2) {
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
            free(slab);
            break;
        }
        w->spare[w->count++] = slab;
    }
    pthread_mutex_unlock(&w->mutex);
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
#else
static arm_jit_storage_worker_t *arm_jit_storage_start(void) { return NULL; }
static void arm_jit_storage_stop(arm_jit_storage_worker_t *w) { GP32_UNUSED(w); }
static arm_jit_op_slab_t *arm_jit_storage_take(arm_jit_storage_worker_t *w) {
    GP32_UNUSED(w);
    return malloc(sizeof(arm_jit_op_slab_t));
}
#endif
