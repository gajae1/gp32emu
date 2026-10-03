/* Minimal model of RetroArch 69a4f0e's playback FIFO wait protocol.
 * No ALSA calls, device access, sleeps, or changes to RetroArch.
 * A one-slot FIFO starts full (write_avail == 0).
 * Barriers force the check/signal/wait order; timeout bounds observation.
 * POSIX permits spurious wakeups: those are reported as inconclusive.
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct model {
    pthread_mutex_t fifo_lock;
    pthread_mutex_t cond_lock;
    pthread_cond_t cond;
    pthread_barrier_t checked;
    pthread_barrier_t attempted;
    pthread_barrier_t worker_done;
    int fixed;
    int shutdown;
    int thread_dead;       /* Only accessed under fifo_lock. */
    int write_avail;       /* Only accessed under fifo_lock. */
    int guard_busy;        /* Worker-owned, observed after join. */
    int wait_result;       /* Writer-owned, observed after join. */
    int written;          /* Writer-owned, observed after join. */
} model;

static void check(int rc, const char *operation)
{
    if (rc != 0) {
        fprintf(stderr, "%s: %s (%d)\n", operation, strerror(rc), rc);
        exit(2);
    }
}

#define CHECK(operation) check((operation), #operation)

static void meet(pthread_barrier_t *barrier)
{
    int rc = pthread_barrier_wait(barrier);
    if (rc != PTHREAD_BARRIER_SERIAL_THREAD)
        check(rc, "pthread_barrier_wait");
}

static struct timespec deadline(void)
{
    struct timespec until;
    if (clock_gettime(CLOCK_REALTIME, &until) != 0) {
        perror("clock_gettime");
        exit(2);
    }
    until.tv_nsec += 100000000L; /* Watchdog only, not schedule control. */
    if (until.tv_nsec >= 1000000000L) {
        ++until.tv_sec;
        until.tv_nsec -= 1000000000L;
    }
    return until;
}

static void *worker(void *data)
{
    model *m = data;
    int rc;
    meet(&m->checked);
    /* Prove whether the writer already holds the notification mutex. */
    rc = pthread_mutex_trylock(&m->fifo_lock);
    if (rc == 0) {
        m->guard_busy = 0;
        CHECK(pthread_mutex_unlock(&m->fifo_lock));
    } else if (rc == EBUSY) {
        m->guard_busy = 1;
    } else {
        check(rc, "pthread_mutex_trylock");
    }
    meet(&m->attempted);

    CHECK(pthread_mutex_lock(&m->fifo_lock));
    if (m->shutdown)
        m->thread_dead = 1; /* Same mutex as the fixed wait predicate. */
    else
        m->write_avail = 1; /* Worker consumes the queued sample. */
    CHECK(pthread_cond_signal(&m->cond));
    CHECK(pthread_mutex_unlock(&m->fifo_lock));
    meet(&m->worker_done);
    return NULL;
}

static void *writer(void *data)
{
    model *m = data;
    int first = 1;
    while (m->written == 0) {
        CHECK(pthread_mutex_lock(&m->fifo_lock));
        if (m->thread_dead) {
            CHECK(pthread_mutex_unlock(&m->fifo_lock));
            break;
        }
        if (m->write_avail == 0) {
            struct timespec until;
            pthread_mutex_t *wait_lock = m->fixed
                    ? &m->fifo_lock : &m->cond_lock;
            if (!m->fixed)
                CHECK(pthread_mutex_unlock(&m->fifo_lock));
            if (first) {
                meet(&m->checked);
                meet(&m->attempted);
                /* Original's legal gap: worker signals before wait. */
                if (!m->fixed)
                    meet(&m->worker_done);
            }
            if (!m->fixed)
                CHECK(pthread_mutex_lock(&m->cond_lock));
            until = deadline();
            m->wait_result = pthread_cond_timedwait(
                    &m->cond, wait_lock, &until);
            CHECK(pthread_mutex_unlock(wait_lock));
            if (first && m->fixed)
                meet(&m->worker_done);
            first = 0;
            if (m->wait_result == ETIMEDOUT)
                break;
            check(m->wait_result, "pthread_cond_timedwait");
            /* As in RA, recheck the FIFO after every ordinary wake. */
        } else {
            m->write_avail = 0;
            m->written = 1;
            CHECK(pthread_mutex_unlock(&m->fifo_lock));
        }
    }
    return NULL;
}

int main(int argc, char **argv)
{
    model m;
    pthread_t producer, consumer;
    memset(&m, 0, sizeof(m));
    if (argc != 2 || (strcmp(argv[1], "original") != 0
            && strcmp(argv[1], "fixed") != 0
            && strcmp(argv[1], "fixed-shutdown") != 0)) {
        fputs("Usage: lost_wakeup original|fixed|fixed-shutdown\n", stderr);
        return 2;
    }
    m.fixed = strcmp(argv[1], "original") != 0;
    m.shutdown = strcmp(argv[1], "fixed-shutdown") == 0;
    CHECK(pthread_mutex_init(&m.fifo_lock, NULL));
    CHECK(pthread_mutex_init(&m.cond_lock, NULL));
    CHECK(pthread_cond_init(&m.cond, NULL));
    CHECK(pthread_barrier_init(&m.checked, NULL, 2));
    CHECK(pthread_barrier_init(&m.attempted, NULL, 2));
    CHECK(pthread_barrier_init(&m.worker_done, NULL, 2));
    CHECK(pthread_create(&producer, NULL, writer, &m));
    CHECK(pthread_create(&consumer, NULL, worker, &m));
    CHECK(pthread_join(producer, NULL));
    CHECK(pthread_join(consumer, NULL));
    printf("%s: guard_busy=%d wait=%s written=%d write_avail=%d dead=%d\n",
            argv[1], m.guard_busy,
            m.wait_result == ETIMEDOUT ? "ETIMEDOUT" : "wake",
            m.written, m.write_avail, m.thread_dead);
    CHECK(pthread_barrier_destroy(&m.worker_done));
    CHECK(pthread_barrier_destroy(&m.attempted));
    CHECK(pthread_barrier_destroy(&m.checked));
    CHECK(pthread_cond_destroy(&m.cond));
    CHECK(pthread_mutex_destroy(&m.cond_lock));
    CHECK(pthread_mutex_destroy(&m.fifo_lock));
    if (m.guard_busy != m.fixed) {
        fputs("Unexpected schedule\n", stderr);
        return 2;
    }
    if (m.fixed) {
        if (m.wait_result != 0 || (!m.shutdown && m.written != 1)
                || (m.shutdown && (!m.thread_dead || m.written))) {
            fputs("FAIL: corrected protocol did not make expected progress.\n", stderr);
            return 2;
        }
        puts("PASS: corrected FIFO mutex handoff preserved the notification.");
        return 0;
    }
    if (m.wait_result == ETIMEDOUT && m.written == 0 && m.write_avail == 1) {
        puts("REPRODUCED: writer waited despite available FIFO space; progress failed.");
        return 1; /* Progress requirement deliberately fails on original. */
    }
    fputs("INCONCLUSIVE: a permitted spurious wake rescued this execution.\n", stderr);
    return 77;
}
