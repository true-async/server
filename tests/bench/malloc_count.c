/*
 * LD_PRELOAD malloc-counting shim for allocation-per-request audit
 * (PLAN_HTTP2 §Step 10 hot-path audit).
 *
 * Counts malloc / calloc / realloc / free invocations and total bytes
 * allocated. A driver snapshots counters by sending a real-time
 * signal; the delta between two snapshots is the steady-state cost.
 * On program exit the final snapshot is also dumped.
 *
 * Why a real-time signal + dedicated thread instead of SIGUSR1?
 * PHP (and ext/async) register their own handlers for SIGUSR1/SIGUSR2
 * during startup, which races with our LD_PRELOAD constructor and
 * wins — the next SIGUSR1 then kills the process. A dedicated thread
 * that sigwait()s on SIGRTMAX is immune to that: all other threads
 * (including every PHP thread created later) inherit the blocked
 * mask, so the signal is only ever delivered to us.
 *
 * Build: cc -O2 -fPIC -shared -o malloc_count.so malloc_count.c \
 *           -ldl -lpthread
 * Run:   USE_ZEND_ALLOC=0 LD_PRELOAD=./malloc_count.so php ...
 * Snap:  kill -s SIGRTMAX $pid   # or: kill -34 $pid on Linux
 *
 * USE_ZEND_ALLOC=0 is required — otherwise Zend MM amortises every
 * emalloc into a single giant mmap chunk at startup and per-request
 * allocator traffic becomes invisible from the libc layer.
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void *(*real_malloc)(size_t)           = NULL;
static void *(*real_calloc)(size_t, size_t)   = NULL;
static void *(*real_realloc)(void *, size_t)  = NULL;
static void  (*real_free)(void *)             = NULL;

static atomic_ullong n_malloc  = 0;
static atomic_ullong n_calloc  = 0;
static atomic_ullong n_realloc = 0;
static atomic_ullong n_free    = 0;
static atomic_ullong bytes_alloc = 0;

static void resolve_syms(void)
{
    if (real_malloc == NULL) {
        real_malloc  = dlsym(RTLD_NEXT, "malloc");
        real_calloc  = dlsym(RTLD_NEXT, "calloc");
        real_realloc = dlsym(RTLD_NEXT, "realloc");
        real_free    = dlsym(RTLD_NEXT, "free");
    }
}

static void dump_stats(const char *label)
{
    fprintf(stderr, "[malloc_count] %s: malloc=%llu calloc=%llu "
            "realloc=%llu free=%llu bytes=%llu\n",
            label,
            (unsigned long long)atomic_load(&n_malloc),
            (unsigned long long)atomic_load(&n_calloc),
            (unsigned long long)atomic_load(&n_realloc),
            (unsigned long long)atomic_load(&n_free),
            (unsigned long long)atomic_load(&bytes_alloc));
}

static void dump_stats_exit(void) { dump_stats("EXIT"); }

/* Dedicated thread that owns SIGRTMAX. Created once from the library
 * constructor, inherits the blocked mask from us so the signal is
 * guaranteed to be delivered here and not to any PHP worker. */
static void *sig_thread(void *arg)
{
    (void)arg;
    sigset_t wait_set;
    sigemptyset(&wait_set);
    sigaddset(&wait_set, SIGRTMAX);
    for (;;) {
        int sig = 0;
        if (sigwait(&wait_set, &sig) == 0 && sig == SIGRTMAX) {
            dump_stats("SNAPSHOT");
            /* No reset — caller computes deltas across snapshots. */
        }
    }
    return NULL;
}

__attribute__((constructor))
static void init(void)
{
    resolve_syms();

    /* Block SIGRTMAX process-wide before spawning any thread. Every
     * thread created later (including PHP's own) inherits this mask,
     * so our sigwait thread is the exclusive recipient. */
    sigset_t block_set;
    sigemptyset(&block_set);
    sigaddset(&block_set, SIGRTMAX);
    pthread_sigmask(SIG_BLOCK, &block_set, NULL);

    pthread_t t;
    if (pthread_create(&t, NULL, sig_thread, NULL) == 0) {
        pthread_detach(t);
    } else {
        fprintf(stderr, "[malloc_count] warning: pthread_create failed, "
                        "snapshots disabled\n");
    }

    atexit(dump_stats_exit);
}

void *malloc(size_t sz)
{
    resolve_syms();
    atomic_fetch_add(&n_malloc, 1);
    atomic_fetch_add(&bytes_alloc, sz);
    return real_malloc(sz);
}

void *calloc(size_t n, size_t sz)
{
    resolve_syms();
    atomic_fetch_add(&n_calloc, 1);
    atomic_fetch_add(&bytes_alloc, n * sz);
    return real_calloc(n, sz);
}

void *realloc(void *p, size_t sz)
{
    resolve_syms();
    atomic_fetch_add(&n_realloc, 1);
    atomic_fetch_add(&bytes_alloc, sz);
    return real_realloc(p, sz);
}

void free(void *p)
{
    resolve_syms();
    if (p != NULL) atomic_fetch_add(&n_free, 1);
    real_free(p);
}
