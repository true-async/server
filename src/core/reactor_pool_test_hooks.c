/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+

  Reactor pool test hook (#80). Entirely gated behind HTTP_SERVER_TEST_HOOKS
  (--enable-http-server-test-hooks); never present in a release build.
  See include/core/reactor_pool_test.h.
*/

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "core/reactor_pool_test.h"

#ifdef HTTP_SERVER_TEST_HOOKS

#include "php.h"
#include "Zend/zend_API.h"
#include "Zend/zend_atomic.h"
#include "zend_exceptions.h"
#include "core/reactor_pool.h"
#include "core/response_wire.h"
#include "core/worker_dispatch.h"
#include "core/worker_inbox.h"
#include "core/worker_registry.h"
#include "core/stats_registry.h"
#include "core/async_plain_event.h"
#include "php_http_server.h"
#include "http1/http_parser.h"
#include "log/http_log.h"

#include <stdint.h>
#include <string.h>
#ifndef PHP_WIN32
# include <unistd.h>   /* STDOUT_FILENO */
#endif
#ifndef STDOUT_FILENO
# define STDOUT_FILENO 1
#endif

/* Defined in src/http_request.c; wraps an http_request_t in an HttpRequest zval. */
extern zval *http_request_create_from_parsed(http_request_t *req);

/* Defined in src/http_server_class.c; snapshots the per-worker stats slab. */
extern int http_server_stats_slab_snapshot(uint64_t *out, int max);

#ifdef PHP_WIN32
# include <windows.h>
#else
# include <time.h>
# include <pthread.h>
#endif

/* Reactor-side H3 listener spike (#80, B3p3-a). POSIX-only: the raw-fd recv
 * path and the C datagram send below use BSD sockets directly. */
#if defined(HAVE_HTTP_SERVER_HTTP3) && !defined(PHP_WIN32)
# include "http3/http3_listener.h"
# include <sys/socket.h>
# include <netinet/in.h>
# include <arpa/inet.h>
# include <unistd.h>
#endif

/* Upper bound on how long the self-test waits for reactors to drain. */
#define REACTOR_SELFTEST_WAIT_MS 5000

static void selftest_msleep(void)
{
#ifdef PHP_WIN32
    Sleep(1);
#else
    const struct timespec ts = { 0, 1000000 }; /* 1 ms */
    nanosleep(&ts, NULL);
#endif
}

/* Opaque OS thread identity, for asserting a callback ran off the parent. */
static uintptr_t selftest_thread_id(void)
{
#ifdef PHP_WIN32
    return (uintptr_t)GetCurrentThreadId();
#else
    return (uintptr_t)pthread_self();
#endif
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_MASK_EX(arginfo_reactor_pool_selftest, 0, 2,
                                        MAY_BE_ARRAY | MAY_BE_FALSE)
    ZEND_ARG_TYPE_INFO(0, reactors, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, items_per_reactor, IS_LONG, 0)
ZEND_END_ARG_INFO()

/* Spin up `reactors` transport reactors, post `items_per_reactor` opaque tokens
 * into each reactor's #81 inbound, wait for them to drain, tear down, and return
 * the per-reactor drained counts (or false on spawn failure). Exercises spawn,
 * channel drain, per-reactor isolation, and clean shutdown. */
PHP_FUNCTION(_http_server_reactor_pool_selftest)
{
    zend_long reactors = 0;
    zend_long items = 0;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_LONG(reactors)
        Z_PARAM_LONG(items)
    ZEND_PARSE_PARAMETERS_END();

    if (reactors <= 0 || items < 0) {
        RETURN_FALSE;
    }

    reactor_pool_t *const rp = reactor_pool_create((int)reactors, 0);

    if (rp == NULL) {
        RETURN_FALSE;
    }

    const int count = reactor_pool_count(rp);

    for (int r = 0; r < count; r++) {
        for (zend_long k = 1; k <= items; k++) {
            void *const token = (void *)(uintptr_t)k; /* opaque, never deref'd */

            while (!reactor_pool_post(rp, r, token)) {
                selftest_msleep(); /* mailbox full: let the reactor drain */
            }
        }
    }

    /* Reactors drain on their own threads; bounded wait for completion. */
    for (int waited = 0; waited < REACTOR_SELFTEST_WAIT_MS; waited++) {
        bool all_done = true;

        for (int r = 0; r < count; r++) {
            if (reactor_pool_processed(rp, r) < (uint64_t)items) {
                all_done = false;
                break;
            }
        }

        if (all_done) {
            break;
        }

        selftest_msleep();
    }

    array_init(return_value);

    for (int r = 0; r < count; r++) {
        add_next_index_long(return_value, (zend_long)reactor_pool_processed(rp, r));
    }

    reactor_pool_destroy(rp);
}

/* Build a synthetic http_request_t the way the #80 reactor will: persistent
 * (malloc) domain method/uri/headers + routing triple, ZMM body (worker-domain
 * — deliberately mixed-domain).
 * Returns a refcount=1 request the caller owns (hand to dispatch/inbox, or
 * release via http_request_destroy). NULL only on persistent-alloc failure.
 * `headers`/`body` may be NULL. */
static http_request_t *selftest_build_request(uint32_t reactor_id, int64_t stream_id,
                                              const char *method, size_t method_len,
                                              const char *path, size_t path_len,
                                              HashTable *headers,
                                              const char *body, size_t body_len)
{
    http_request_t *const req = pecalloc(1, sizeof(*req), 1);

    if (req == NULL) {
        return NULL;
    }

    req->refcount          = 1;
    req->persistent        = true;
    req->reactor_id        = reactor_id;
    req->reactor_stream_id = stream_id;
    req->reactor_conn      = NULL;

    req->method = zend_string_init(method, method_len, 1);
    req->uri    = zend_string_init(path, path_len, 1);

    if (headers != NULL) {
        http_request_init_headers(req);

        zend_string *name;
        zval        *value;
        ZEND_HASH_FOREACH_STR_KEY_VAL(headers, name, value) {
            if (name == NULL || Z_TYPE_P(value) != IS_STRING) {
                continue;
            }

            zend_string *const key = zend_string_init(ZSTR_VAL(name), ZSTR_LEN(name), 1);
            zval val;
            ZVAL_STR(&val, zend_string_init(Z_STRVAL_P(value), Z_STRLEN_P(value), 1));
            zend_hash_update(req->headers, key, &val);
            zend_string_release(key);
        } ZEND_HASH_FOREACH_END();
    }

    if (body != NULL && body_len > 0) {
        req->body           = zend_string_init(body, body_len, 0);  /* worker-domain */
        req->content_length = body_len;
    }

    return req;
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_MASK_EX(arginfo_persistent_request_selftest, 0, 4,
                                        MAY_BE_OBJECT | MAY_BE_FALSE)
    ZEND_ARG_TYPE_INFO(0, method, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, path, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, headers, IS_ARRAY, 0)
    ZEND_ARG_TYPE_INFO(0, body, IS_STRING, 0)
ZEND_END_ARG_INFO()

/* Build an http_request_t in the PERSISTENT (malloc) domain the way the #80
 * reactor will (selftest_build_request: persistent method/uri/headers, ZMM
 * body) and wrap it in an HttpRequest. The phpt then exercises the flag-aware
 * accessors (deep-copy persistent strings, rebuild the persistent headers table
 * into ZMM) and frees the object, which runs http_request_destroy on the
 * persistent domain. ASan proves the whole reactor-side request lifecycle
 * (create + read + free) is heap-clean. */
PHP_FUNCTION(_http_server_persistent_request_selftest)
{
    zend_string *method;
    zend_string *path;
    HashTable   *headers;
    zend_string *body;

    ZEND_PARSE_PARAMETERS_START(4, 4)
        Z_PARAM_STR(method)
        Z_PARAM_STR(path)
        Z_PARAM_ARRAY_HT(headers)
        Z_PARAM_STR(body)
    ZEND_PARSE_PARAMETERS_END();

    http_request_t *const req = selftest_build_request(
        0, 0, ZSTR_VAL(method), ZSTR_LEN(method), ZSTR_VAL(path), ZSTR_LEN(path),
        headers, ZSTR_VAL(body), ZSTR_LEN(body));

    if (req == NULL) {
        RETURN_FALSE;
    }

    zval *const obj = http_request_create_from_parsed(req);
    ZVAL_COPY_VALUE(return_value, obj);
    efree(obj);
}

/* Filled by exec_probe_fn ON the reactor thread; read by the parent only after
 * reactor_pool_exec returns (its acquire-load of `done` orders these writes), so
 * plain fields suffice — no atomics needed in the probe itself. */
typedef struct {
    uintptr_t tid;   /* thread the callback ran on */
    int       ran;   /* 1 once the callback executed */
} exec_probe_t;

static void exec_probe_fn(void *arg)
{
    exec_probe_t *const p = (exec_probe_t *)arg;
    p->tid = selftest_thread_id();
    p->ran = 1;
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_MASK_EX(arginfo_reactor_pool_exec_selftest, 0, 1,
                                        MAY_BE_ARRAY | MAY_BE_FALSE)
    ZEND_ARG_TYPE_INFO(0, reactors, IS_LONG, 0)
ZEND_END_ARG_INFO()

/* Spin up `reactors` reactors and run a probe callback on each via
 * reactor_pool_exec, proving the function executes ON the reactor's own thread
 * (off the parent, one distinct thread per reactor). Returns a summary array
 * { reactors, ran, off_parent, distinct_threads } or false on spawn failure. */
PHP_FUNCTION(_http_server_reactor_pool_exec_selftest)
{
    zend_long reactors = 0;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(reactors)
    ZEND_PARSE_PARAMETERS_END();

    if (reactors <= 0) {
        RETURN_FALSE;
    }

    reactor_pool_t *const rp = reactor_pool_create((int)reactors, 0);

    if (rp == NULL) {
        RETURN_FALSE;
    }

    const int       count      = reactor_pool_count(rp);
    const uintptr_t parent_tid = selftest_thread_id();

    exec_probe_t *const probes = ecalloc((size_t)count, sizeof(*probes));

    for (int r = 0; r < count; r++) {
        reactor_pool_exec(rp, r, exec_probe_fn, &probes[r]);
    }

    int  ran        = 0;
    int  off_parent = 0;
    bool distinct   = true;

    for (int r = 0; r < count; r++) {
        if (probes[r].ran == 1) {
            ran++;

            if (probes[r].tid != parent_tid) {
                off_parent++;
            }
        }

        for (int s = r + 1; s < count; s++) {
            if (probes[r].tid == probes[s].tid) {
                distinct = false;
            }
        }
    }

    array_init(return_value);
    add_assoc_long(return_value, "reactors", count);
    add_assoc_long(return_value, "ran", ran);
    add_assoc_long(return_value, "off_parent", off_parent);
    add_assoc_bool(return_value, "distinct_threads", distinct);

    efree(probes);
    reactor_pool_destroy(rp);
}

/* Each reactor's own probe — written only by that one reactor thread (single
 * writer, so load+store on `ran` is safe), polled by the parent via an atomic
 * load. Mirrors how reactor_pool counts `processed`. */
typedef struct {
    zend_atomic_int ran;   /* callbacks that executed on this reactor */
    uintptr_t       tid;   /* thread they ran on */
} post_exec_probe_t;

static void post_exec_probe_fn(void *arg)
{
    post_exec_probe_t *const p = (post_exec_probe_t *)arg;
    p->tid = selftest_thread_id();
    zend_atomic_int_store_ex(&p->ran, zend_atomic_int_load_ex(&p->ran) + 1);
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_MASK_EX(arginfo_reactor_post_exec_selftest, 0, 2,
                                        MAY_BE_ARRAY | MAY_BE_FALSE)
    ZEND_ARG_TYPE_INFO(0, reactors, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, count, IS_LONG, 0)
ZEND_END_ARG_INFO()

/* Drive the fire-and-forget reverse-path primitive: post `count` non-blocking
 * callbacks into each reactor and confirm they all ran on the reactor's own
 * thread without the caller ever blocking. Unlike reactor_pool_exec (one blocking
 * round-trip), reactor_pool_post_exec returns immediately, so the parent posts
 * everything first and only then polls for completion. Returns a summary
 * { reactors, expected, ran, off_parent } or false on spawn failure. */
PHP_FUNCTION(_http_server_reactor_post_exec_selftest)
{
    zend_long reactors = 0, count = 0;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_LONG(reactors)
        Z_PARAM_LONG(count)
    ZEND_PARSE_PARAMETERS_END();

    if (reactors <= 0 || count <= 0) {
        RETURN_FALSE;
    }

    reactor_pool_t *const rp = reactor_pool_create((int)reactors, 0);

    if (rp == NULL) {
        RETURN_FALSE;
    }

    const int       cnt        = reactor_pool_count(rp);
    const uintptr_t parent_tid = selftest_thread_id();

    post_exec_probe_t *const probes = ecalloc((size_t)cnt, sizeof(*probes));

    for (int r = 0; r < cnt; r++) {
        ZEND_ATOMIC_INT_INIT(&probes[r].ran, 0);
    }

    /* Fire everything without blocking; backpressure → retry. */
    for (int r = 0; r < cnt; r++) {
        for (zend_long k = 0; k < count; k++) {
            while (!reactor_pool_post_exec(rp, r, post_exec_probe_fn, &probes[r])) {
                selftest_msleep();
            }
        }
    }

    /* Now poll until every reactor has run all its callbacks (bounded). */
    for (int waited = 0; waited < REACTOR_SELFTEST_WAIT_MS; waited++) {
        bool all_done = true;

        for (int r = 0; r < cnt; r++) {
            if (zend_atomic_int_load_ex(&probes[r].ran) < (int)count) {
                all_done = false;
                break;
            }
        }

        if (all_done) {
            break;
        }

        selftest_msleep();
    }

    int ran = 0, off_parent = 0;

    for (int r = 0; r < cnt; r++) {
        ran += zend_atomic_int_load_ex(&probes[r].ran);

        if (probes[r].tid != 0 && probes[r].tid != parent_tid) {
            off_parent++;
        }
    }

    array_init(return_value);
    add_assoc_long(return_value, "reactors", cnt);
    add_assoc_long(return_value, "expected", (zend_long)(cnt * (int)count));
    add_assoc_long(return_value, "ran", ran);
    add_assoc_long(return_value, "off_parent", off_parent);

    efree(probes);
    reactor_pool_destroy(rp);
}

/* Sink + suspend state for the worker-dispatch self-test. */
typedef struct {
    response_wire_t    *captured;  /* response handed back by dispatch (owned) */
    zend_async_event_t *done;      /* fired by the sink to wake the test */
} dispatch_probe_t;

static bool dispatch_probe_sink(response_wire_t *rw, void *arg)
{
    dispatch_probe_t *const p = (dispatch_probe_t *)arg;

    /* Self-test handlers are buffered-only: keep the FULL wire, free any
     * earlier capture defensively so a streaming handler cannot leak. */
    if (p->captured != NULL) {
        response_wire_free(p->captured);
    }

    p->captured = rw;             /* take ownership */
    async_plain_event_fire(p->done);
    return true;
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_MASK_EX(arginfo_dispatch_from_wire_selftest, 0, 5,
                                        MAY_BE_ARRAY | MAY_BE_FALSE)
    ZEND_ARG_TYPE_INFO(0, server, IS_OBJECT, 0)
    ZEND_ARG_TYPE_INFO(0, method, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, path, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, headers, IS_ARRAY, 0)
    ZEND_ARG_TYPE_INFO(0, body, IS_STRING, 0)
ZEND_END_ARG_INFO()

/* Drive the worker-side request path end to end on this thread: build a
 * persistent http_request_t from the args, hand its pointer to
 * worker_dispatch_request (which spawns the user handler coroutine in the
 * current scope), suspend until the handler's dispose renders the response_wire
 * and fires the sink, then return the rendered { status, headers, body } (or
 * false on failure / timeout). Must be called from inside a coroutine
 * (Async\spawn) on a server with a registered handler. */
PHP_FUNCTION(_http_server_dispatch_from_wire_selftest)
{
    zval        *server_zv;
    zend_string *method, *path, *body;
    HashTable   *headers;

    ZEND_PARSE_PARAMETERS_START(5, 5)
        Z_PARAM_OBJECT_OF_CLASS(server_zv, http_server_ce)
        Z_PARAM_STR(method)
        Z_PARAM_STR(path)
        Z_PARAM_ARRAY_HT(headers)
        Z_PARAM_STR(body)
    ZEND_PARSE_PARAMETERS_END();

    http_server_object *const server = http_server_object_from_zend(Z_OBJ_P(server_zv));

    if (server == NULL) {
        RETURN_FALSE;
    }

    http_request_t *const req = selftest_build_request(
        0, 1, ZSTR_VAL(method), ZSTR_LEN(method), ZSTR_VAL(path), ZSTR_LEN(path),
        headers, ZSTR_VAL(body), ZSTR_LEN(body));

    if (req == NULL) {
        RETURN_FALSE;
    }

    dispatch_probe_t probe = { .captured = NULL, .done = async_plain_event_new() };

    if (probe.done == NULL) {
        http_request_destroy(req);
        RETURN_FALSE;
    }

    /* worker_dispatch_request consumes req unconditionally (owns it on success,
     * destroys it on failure) — nothing to free here. */
    const bool ok = worker_dispatch_request(server, ZEND_ASYNC_CURRENT_SCOPE,
                                            req, /*own_scope=*/true,
                                            dispatch_probe_sink, &probe);

    if (!ok) {
        probe.done->dispose(probe.done);
        RETURN_FALSE;
    }

    /* Suspend until the handler coroutine's dispose fires the sink; a timeout
     * timer keeps a misbehaving handler from hanging the test. Both events are
     * trans_event=true, so the waker owns and disposes them on resume. */
    zend_coroutine_t *const co = ZEND_ASYNC_CURRENT_COROUTINE;

    if (ZEND_ASYNC_WAKER_NEW(co) == NULL) {
        probe.done->dispose(probe.done);
        RETURN_FALSE;
    }

    zend_async_resume_when(co,
        &ZEND_ASYNC_NEW_TIMER_EVENT((zend_ulong)5000, false)->base, true,
        zend_async_waker_callback_timeout, NULL);
    zend_async_resume_when(co, probe.done, true,
        zend_async_waker_callback_resolve, NULL);

    ZEND_ASYNC_SUSPEND();
    zend_async_waker_clean(co);

    if (EG(exception)) {
        zend_clear_exception();
    }

    if (probe.captured == NULL) {
        RETURN_FALSE; /* timed out / no response rendered */
    }

    array_init(return_value);
    add_assoc_long(return_value, "status", response_wire_status(probe.captured));

    zval hdrs;
    array_init(&hdrs);
    const size_t hcount = response_wire_header_count(probe.captured);

    for (size_t i = 0; i < hcount; i++) {
        const char *np, *vp;
        size_t      nl, vl;

        if (response_wire_header_at(probe.captured, i, &np, &nl, &vp, &vl)) {
            add_assoc_stringl_ex(&hdrs, np, nl, (char *)vp, vl);
        }
    }

    add_assoc_zval(return_value, "headers", &hdrs);

    size_t      blen;
    const char *b = response_wire_body(probe.captured, &blen);
    add_assoc_stringl(return_value, "body", b != NULL ? (char *)b : "", blen);

    response_wire_free(probe.captured);
}

/* Accumulating sink for the worker-inbox self-test: validate each rendered
 * response (200 + "ok-" body), count it, and fire `done` once all expected
 * responses have arrived. */
typedef struct {
    int                 expected;
    int                 received;
    int                 ok;
    zend_async_event_t *done;
} inbox_probe_t;

static bool inbox_probe_sink(response_wire_t *rw, void *arg)
{
    inbox_probe_t *const p = (inbox_probe_t *)arg;

    if (response_wire_status(rw) == 200) {
        size_t      blen;
        const char *b = response_wire_body(rw, &blen);

        if (b != NULL && blen >= 3 && strncmp(b, "ok-", 3) == 0) {
            p->ok++;
        }
    }

    p->received++;
    response_wire_free(rw);

    if (p->received >= p->expected && p->done != NULL) {
        async_plain_event_fire(p->done);
    }

    return true;
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_MASK_EX(arginfo_worker_inbox_selftest, 0, 2,
                                        MAY_BE_ARRAY | MAY_BE_FALSE)
    ZEND_ARG_TYPE_INFO(0, server, IS_OBJECT, 0)
    ZEND_ARG_TYPE_INFO(0, count, IS_LONG, 0)
ZEND_END_ARG_INFO()

/* Drive the worker-inbox path: stand up a worker_inbox on this thread, post
 * `count` synthetic http_request_t pointers into it (as a reactor would), wait
 * for the drain to dispatch them all and the handlers to render their responses,
 * then return { expected, received, ok }. Proves the #81 mailbox -> dispatch ->
 * response path carries N independent requests. Call inside a coroutine on a
 * server with a registered handler. */
PHP_FUNCTION(_http_server_worker_inbox_selftest)
{
    zval     *server_zv;
    zend_long count;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_OBJECT_OF_CLASS(server_zv, http_server_ce)
        Z_PARAM_LONG(count)
    ZEND_PARSE_PARAMETERS_END();

    if (count <= 0) {
        RETURN_FALSE;
    }

    http_server_object *const server = http_server_object_from_zend(Z_OBJ_P(server_zv));

    if (server == NULL) {
        RETURN_FALSE;
    }

    inbox_probe_t probe = {
        .expected = (int)count,
        .received = 0,
        .ok       = 0,
        .done     = async_plain_event_new(),
    };

    if (probe.done == NULL) {
        RETURN_FALSE;
    }

    worker_inbox_t *const inbox = worker_inbox_create(server, ZEND_ASYNC_CURRENT_SCOPE,
                                                      /*own_scope=*/true,
                                                      inbox_probe_sink, &probe);

    if (inbox == NULL) {
        probe.done->dispose(probe.done);
        RETURN_FALSE;
    }

    for (zend_long i = 0; i < count; i++) {
        char      path[32];
        const int plen = snprintf(path, sizeof(path), "/item-%lld", (long long)i);
        http_request_t *const req = selftest_build_request(
            0, i, "GET", 3, path, plen > 0 ? (size_t)plen : 0, NULL, NULL, 0);

        if (req == NULL) {
            probe.expected--;
            continue;
        }

        if (!worker_inbox_post(inbox, req)) {
            http_request_destroy(req);  /* full: backpressure, we keep ownership */
            probe.expected--;
        }
    }

    /* Suspend until every dispatched handler has rendered its response; a
     * timeout timer keeps the loop alive and bounds a hang. */
    zend_coroutine_t *const co = ZEND_ASYNC_CURRENT_COROUTINE;

    if (ZEND_ASYNC_WAKER_NEW(co) == NULL) {
        worker_inbox_free(inbox);
        probe.done->dispose(probe.done);
        RETURN_FALSE;
    }

    zend_async_resume_when(co,
        &ZEND_ASYNC_NEW_TIMER_EVENT((zend_ulong)5000, false)->base, true,
        zend_async_waker_callback_timeout, NULL);
    zend_async_resume_when(co, probe.done, true,
        zend_async_waker_callback_resolve, NULL);

    ZEND_ASYNC_SUSPEND();
    zend_async_waker_clean(co);

    if (EG(exception)) {
        zend_clear_exception();
    }

    /* Every handler has completed (or we timed out) — no in-flight dispatch
     * references the inbox, so it is safe to tear down. */
    worker_inbox_free(inbox);

    array_init(return_value);
    add_assoc_long(return_value, "expected", probe.expected);
    add_assoc_long(return_value, "received", probe.received);
    add_assoc_long(return_value, "ok", probe.ok);
}

/* Registry self-test: one shared tally across all inboxes, plus a per-inbox
 * counter so the test can see the round-robin spread. */
typedef struct {
    int                 expected;
    int                 received;
    int                 ok;
    zend_async_event_t *done;
} reg_shared_t;

typedef struct {
    reg_shared_t *shared;
    int           per_inbox;  /* responses this inbox handled */
} reg_slot_probe_t;

static bool reg_probe_sink(response_wire_t *rw, void *arg)
{
    reg_slot_probe_t *const p = (reg_slot_probe_t *)arg;

    if (response_wire_status(rw) == 200) {
        size_t      blen;
        const char *b = response_wire_body(rw, &blen);

        if (b != NULL && blen >= 3 && strncmp(b, "ok-", 3) == 0) {
            p->shared->ok++;
        }
    }

    p->per_inbox++;
    p->shared->received++;
    response_wire_free(rw);

    if (p->shared->received >= p->shared->expected && p->shared->done != NULL) {
        async_plain_event_fire(p->shared->done);
    }

    return true;
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_MASK_EX(arginfo_worker_registry_selftest, 0, 3,
                                        MAY_BE_ARRAY | MAY_BE_FALSE)
    ZEND_ARG_TYPE_INFO(0, server, IS_OBJECT, 0)
    ZEND_ARG_TYPE_INFO(0, workers, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, count, IS_LONG, 0)
ZEND_END_ARG_INFO()

/* Stand up `workers` inboxes published into a worker_registry, post `count`
 * synthetic request pointers through worker_registry_pick (round-robin), wait for
 * all to dispatch + render, then return { expected, received, ok, distribution }
 * where distribution[i] is how many requests slot i handled — proving the
 * registry spreads load across worker inboxes. */
PHP_FUNCTION(_http_server_worker_registry_selftest)
{
    zval     *server_zv;
    zend_long workers, count;

    ZEND_PARSE_PARAMETERS_START(3, 3)
        Z_PARAM_OBJECT_OF_CLASS(server_zv, http_server_ce)
        Z_PARAM_LONG(workers)
        Z_PARAM_LONG(count)
    ZEND_PARSE_PARAMETERS_END();

    if (workers <= 0 || count <= 0) {
        RETURN_FALSE;
    }

    http_server_object *const server = http_server_object_from_zend(Z_OBJ_P(server_zv));

    if (server == NULL) {
        RETURN_FALSE;
    }

    reg_shared_t shared = {
        .expected = (int)count,
        .received = 0,
        .ok       = 0,
        .done     = async_plain_event_new(),
    };

    if (shared.done == NULL) {
        RETURN_FALSE;
    }

    worker_registry_t *const reg = worker_registry_create((int)workers);
    worker_inbox_t   **const inboxes = ecalloc((size_t)workers, sizeof(*inboxes));
    reg_slot_probe_t  *const probes  = ecalloc((size_t)workers, sizeof(*probes));

    for (zend_long w = 0; w < workers; w++) {
        probes[w].shared    = &shared;
        probes[w].per_inbox = 0;
        inboxes[w] = worker_inbox_create(server, ZEND_ASYNC_CURRENT_SCOPE,
                                         /*own_scope=*/true, reg_probe_sink, &probes[w]);
        worker_registry_publish(reg, (int)w, inboxes[w]);
    }

    for (zend_long i = 0; i < count; i++) {
        worker_inbox_t *const target = worker_registry_pick(reg);
        char      path[32];
        const int plen = snprintf(path, sizeof(path), "/item-%lld", (long long)i);
        http_request_t *const req = selftest_build_request(
            0, i, "GET", 3, path, plen > 0 ? (size_t)plen : 0, NULL, NULL, 0);

        if (target == NULL || req == NULL) {
            if (req != NULL) {
                http_request_destroy(req);
            }

            shared.expected--;
            continue;
        }

        if (!worker_inbox_post(target, req)) {
            http_request_destroy(req);
            shared.expected--;
        }
    }

    zend_coroutine_t *const co = ZEND_ASYNC_CURRENT_COROUTINE;

    if (ZEND_ASYNC_WAKER_NEW(co) != NULL) {
        zend_async_resume_when(co,
            &ZEND_ASYNC_NEW_TIMER_EVENT((zend_ulong)5000, false)->base, true,
            zend_async_waker_callback_timeout, NULL);
        zend_async_resume_when(co, shared.done, true,
            zend_async_waker_callback_resolve, NULL);

        ZEND_ASYNC_SUSPEND();
        zend_async_waker_clean(co);

        if (EG(exception)) {
            zend_clear_exception();
        }
    } else {
        shared.done->dispose(shared.done);
    }

    array_init(return_value);
    add_assoc_long(return_value, "expected", shared.expected);
    add_assoc_long(return_value, "received", shared.received);
    add_assoc_long(return_value, "ok", shared.ok);

    zval dist;
    array_init(&dist);

    for (zend_long w = 0; w < workers; w++) {
        add_next_index_long(&dist, probes[w].per_inbox);
        worker_inbox_free(inboxes[w]);
    }

    add_assoc_zval(return_value, "distribution", &dist);

    worker_registry_free(reg);
    efree(inboxes);
    efree(probes);
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_MASK_EX(arginfo_worker_registry_route_selftest, 0, 6,
                                        MAY_BE_ARRAY | MAY_BE_FALSE)
    ZEND_ARG_TYPE_INFO(0, server, IS_OBJECT, 0)
    ZEND_ARG_TYPE_INFO(0, workers, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, published, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, n_reactors, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, reactor_id, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, iterations, IS_LONG, 0)
ZEND_END_ARG_INFO()

/* Drive worker_registry_least_busy (D5) deterministically: publish `published` of
 * `workers` inboxes (all idle, depth 0), call route `iterations` times for
 * (reactor_id, n_reactors), and return { none, distribution[workers] } — the slot
 * each call picked. Proves strided ownership, idle tie-rotation spread, skipping of
 * unpublished slots, and owned-empty -> NULL (the global-fallback trigger). No
 * dispatch/drain: inboxes stay empty, so there is nothing to free per call. */
PHP_FUNCTION(_http_server_worker_registry_route_selftest)
{
    zval     *server_zv;
    zend_long workers, published, n_reactors, reactor_id, iterations;

    ZEND_PARSE_PARAMETERS_START(6, 6)
        Z_PARAM_OBJECT_OF_CLASS(server_zv, http_server_ce)
        Z_PARAM_LONG(workers)
        Z_PARAM_LONG(published)
        Z_PARAM_LONG(n_reactors)
        Z_PARAM_LONG(reactor_id)
        Z_PARAM_LONG(iterations)
    ZEND_PARSE_PARAMETERS_END();

    if (workers <= 0 || published < 0 || published > workers || iterations <= 0) {
        RETURN_FALSE;
    }

    http_server_object *const server = http_server_object_from_zend(Z_OBJ_P(server_zv));

    if (server == NULL) {
        RETURN_FALSE;
    }

    worker_registry_t *const reg = worker_registry_create((int)workers);
    worker_inbox_t   **const inboxes = ecalloc((size_t)workers, sizeof(*inboxes));

    for (zend_long w = 0; w < published; w++) {
        inboxes[w] = worker_inbox_create(server, ZEND_ASYNC_CURRENT_SCOPE,
                                         /*own_scope=*/false, NULL, NULL);
        worker_registry_publish(reg, (int)w, inboxes[w]);
    }

    zend_long  none = 0;
    zend_long *const dist = ecalloc((size_t)workers, sizeof(*dist));

    for (zend_long i = 0; i < iterations; i++) {
        int slot = -1;
        worker_inbox_t *const got =
            worker_registry_least_busy(reg, (int)reactor_id, (int)n_reactors, &slot);

        if (got == NULL || slot < 0) {
            none++;
        } else {
            dist[slot]++;
        }
    }

    array_init(return_value);
    add_assoc_long(return_value, "none", none);

    zval distz;
    array_init(&distz);

    for (zend_long w = 0; w < workers; w++) {
        add_next_index_long(&distz, dist[w]);

        if (inboxes[w] != NULL) {
            worker_inbox_free(inboxes[w]);
        }
    }

    add_assoc_zval(return_value, "distribution", &distz);

    worker_registry_free(reg);
    efree(inboxes);
    efree(dist);
}

/* === Reactor-side H3 listener spike (#80, B3p3-a) ===================
 *
 * Proves the single biggest unknown of the reactor split: that the H3 UDP
 * listener — its uv-bound socket, poll handle and recv path — can live on a
 * transport reactor thread (not a PHP worker) and that the reactor's own loop
 * actually services inbound datagrams. The listener is spawned with
 * server_obj == NULL and ssl_ctx == NULL: no PHP dispatch (http3_stream_dispatch
 * guards on server == NULL) and no crypto — a recv-only spike. */
#if defined(HAVE_HTTP_SERVER_HTTP3) && !defined(PHP_WIN32)

/* Spawn the listener ON the reactor thread (its uv handles must be created on
 * the loop that owns them). Out: the listener + its kernel-assigned port. */
typedef struct {
    const char       *host;
    int               port;
    http3_listener_t *listener;     /* out: NULL on spawn failure */
    int               local_port;   /* out: actual bound port */
} h3l_spawn_ctx_t;

static void h3l_spawn_fn(void *arg)
{
    h3l_spawn_ctx_t *const c = (h3l_spawn_ctx_t *)arg;

    c->listener = http3_listener_spawn(c->host, c->port, NULL, NULL, NULL);

    if (c->listener != NULL) {
        c->local_port = http3_listener_local_port(c->listener);
    } else if (EG(exception)) {
        /* spawn throws on the reactor thread's EG — clear it here so it does
         * not dangle on the reactor; the caller sees listener == NULL. */
        zend_clear_exception();
    }
}

/* Read the recv counter ON the reactor thread (the listener struct is reactor
 * owned). Each call also forces a reactor tick via reactor_pool_exec. */
typedef struct {
    http3_listener_t *listener;
    uint64_t          received;     /* out */
} h3l_stats_ctx_t;

static void h3l_stats_fn(void *arg)
{
    h3l_stats_ctx_t *const c = (h3l_stats_ctx_t *)arg;
    http3_listener_stats_t st;

    http3_listener_get_stats(c->listener, &st);
    c->received = st.datagrams_received;
}

static void h3l_destroy_fn(void *arg)
{
    http3_listener_destroy((http3_listener_t *)arg);
}

#endif /* HAVE_HTTP_SERVER_HTTP3 && !PHP_WIN32 */

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_MASK_EX(arginfo_reactor_h3_listener_selftest, 0, 0,
                                        MAY_BE_BOOL)
ZEND_END_ARG_INFO()

/* Stand up one reactor, spawn an H3 listener on it, fire a few datagrams at the
 * reactor-owned socket from this thread, and confirm the reactor's loop counted
 * them (datagrams_received >= 1). Tears the listener down on its own thread.
 * Returns true iff recv was serviced on the reactor; false when built without
 * HTTP/3, on Windows, or on spawn failure. */
PHP_FUNCTION(_http_server_reactor_h3_listener_selftest)
{
    ZEND_PARSE_PARAMETERS_NONE();

#if defined(HAVE_HTTP_SERVER_HTTP3) && !defined(PHP_WIN32)
    reactor_pool_t *const rp = reactor_pool_create(1, 0);

    if (rp == NULL) {
        RETURN_FALSE;
    }

    h3l_spawn_ctx_t sc = { .host = "127.0.0.1", .port = 0,
                           .listener = NULL, .local_port = 0 };
    reactor_pool_exec(rp, 0, h3l_spawn_fn, &sc);

    if (sc.listener == NULL || sc.local_port <= 0) {
        reactor_pool_destroy(rp);
        RETURN_FALSE;
    }

    /* Fire a handful of short-header garbage datagrams at the listener. They
     * bump datagrams_received before dispatch; dispatch then drops them
     * (unknown short header → stateless reset, no crypto, no server deref). */
    const int s = socket(AF_INET, SOCK_DGRAM, 0);
    bool sent = false;

    if (s >= 0) {
        struct sockaddr_in to;
        memset(&to, 0, sizeof(to));
        to.sin_family      = AF_INET;
        to.sin_port        = htons((uint16_t)sc.local_port);
        to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        static const char probe[] = "quicspike";

        for (int i = 0; i < 8; i++) {
            (void)sendto(s, probe, sizeof(probe) - 1, 0,
                         (struct sockaddr *)&to, (socklen_t)sizeof(to));
        }

        sent = true;
        close(s);
    }

    /* Poll the recv counter until the reactor has serviced our datagrams or we
     * time out. Each iteration round-trips through the reactor (a forced tick). */
    uint64_t received = 0;

    if (sent) {
        for (int waited = 0; waited < REACTOR_SELFTEST_WAIT_MS; waited++) {
            h3l_stats_ctx_t stc = { .listener = sc.listener, .received = 0 };
            reactor_pool_exec(rp, 0, h3l_stats_fn, &stc);
            received = stc.received;

            if (received >= 1) {
                break;
            }

            selftest_msleep();
        }
    }

    /* Tear the listener down on its own thread (libuv handles + reactor ZMM). */
    reactor_pool_exec(rp, 0, h3l_destroy_fn, sc.listener);
    reactor_pool_destroy(rp);

    RETURN_BOOL(received >= 1);
#else
    RETURN_FALSE;
#endif
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_MASK_EX(arginfo_stats_registry_selftest, 0, 1,
                                        MAY_BE_ARRAY | MAY_BE_FALSE)
    ZEND_ARG_TYPE_INFO(0, capacity, IS_LONG, 0)
ZEND_END_ARG_INFO()

/* Exercise the per-worker stats slab (issue #5, A1) with no server or coroutine:
 * claim every slot (indices distinct and in range), confirm a full slab refuses a
 * further claim, write+read a counter through a slot, then retire a slot and
 * confirm it drops from the active count, reads inactive, and is recycled — with
 * its counters zeroed — by the next claim. Returns a summary the phpt asserts, or
 * false on a bad capacity. */
PHP_FUNCTION(_http_server_stats_registry_selftest)
{
    zend_long capacity = 0;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(capacity)
    ZEND_PARSE_PARAMETERS_END();

    if (capacity <= 0) {
        RETURN_FALSE;
    }

    http_stats_registry_t *const reg = http_stats_registry_create((int)capacity);

    if (reg == NULL) {
        RETURN_FALSE;
    }

    const int cap = http_stats_registry_capacity(reg);

    int  *const idxs = ecalloc((size_t)cap, sizeof(*idxs));
    bool *const seen = ecalloc((size_t)cap, sizeof(*seen));
    int  claimed  = 0;
    bool distinct = true;

    for (int i = 0; i < cap; i++) {
        const int idx = http_stats_registry_claim(reg);

        if (idx < 0 || idx >= cap || seen[idx]) {
            distinct = false;
            break;
        }

        seen[idx]       = true;
        idxs[claimed++] = idx;
    }

    const bool overflow_refused = (http_stats_registry_claim(reg) == -1);
    const int  count_full       = http_stats_registry_count(reg);

    /* Write a counter through a slot, read it back through a fresh at(). */
    http_stats_registry_at(reg, idxs[0])->counters.total_requests = 0xABCDEF;
    const bool write_read_ok =
        (http_stats_registry_at(reg, idxs[0])->counters.total_requests == 0xABCDEF);
    const bool active0 =
        http_stats_slot_active(http_stats_registry_at(reg, idxs[0]));

    /* Retire slot 0: it drops from the active count and reads inactive. */
    const bool retire_ok     = http_stats_registry_retire(reg, idxs[0]);
    const int  count_retired = http_stats_registry_count(reg);
    const bool inactive0     =
        !http_stats_slot_active(http_stats_registry_at(reg, idxs[0]));

    /* Reclaim: the next claim reuses the freed slot with its counters zeroed. */
    const int  reidx           = http_stats_registry_claim(reg);
    const bool recycle_idx_ok  = (reidx == idxs[0]);
    const bool recycled_zeroed =
        (http_stats_registry_at(reg, reidx)->counters.total_requests == 0);

    array_init(return_value);
    add_assoc_long(return_value, "capacity", cap);
    add_assoc_long(return_value, "claimed", claimed);
    add_assoc_bool(return_value, "distinct", distinct);
    add_assoc_bool(return_value, "overflow_refused", overflow_refused);
    add_assoc_long(return_value, "count_full", count_full);
    add_assoc_bool(return_value, "write_read_ok", write_read_ok);
    add_assoc_bool(return_value, "active0", active0);
    add_assoc_bool(return_value, "retire_ok", retire_ok);
    add_assoc_long(return_value, "count_retired", count_retired);
    add_assoc_bool(return_value, "inactive0", inactive0);
    add_assoc_bool(return_value, "recycle_idx_ok", recycle_idx_ok);
    add_assoc_bool(return_value, "recycled_zeroed", recycled_zeroed);

    efree(idxs);
    efree(seen);
    http_stats_registry_free(reg);
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_stats_slab_snapshot, 0, 0, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

/* Snapshot the process-wide stats slab (issue #5, A2): one entry per active
 * worker slot carrying that slot's total_requests. Called by the phpt from the
 * parent coroutine while a pool serves — proving each worker bumps its own slab
 * slot (not an embedded counter). Empty array when no slab exists. */
PHP_FUNCTION(_http_server_stats_slab_snapshot)
{
    ZEND_PARSE_PARAMETERS_NONE();

    uint64_t  totals[256];
    const int n = http_server_stats_slab_snapshot(totals, 256);

    array_init(return_value);

    for (int i = 0; i < n; i++) {
        add_next_index_long(return_value, (zend_long)totals[i]);
    }
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_MASK_EX(arginfo_log_format_selftest, 0, 1,
                                        MAY_BE_STRING | MAY_BE_FALSE)
    ZEND_ARG_TYPE_INFO(0, style, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, color, _IS_BOOL, 0, "false")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, template, IS_STRING, 0, "\"\"")
ZEND_END_ARG_INFO()

/* Format a fixed canonical record (issue #5, B2/B3) with the named formatter
 * and return the bytes, so the phpt goldens plain/logfmt/json/pretty output —
 * including space/quote/newline escaping, the json-only trace fields, and the
 * pretty colour on/off paths — with no server or coroutine. `color` feeds the
 * pretty formatter's ud; `template` the template formatter's. false on an
 * unknown style or an uncompilable template. */
PHP_FUNCTION(_http_log_format_selftest)
{
    char       *style;
    size_t      style_len;
    zend_bool   color = 0;
    char       *tmpl_arg = NULL;
    size_t      tmpl_len = 0;

    ZEND_PARSE_PARAMETERS_START(1, 3)
        Z_PARAM_STRING(style, style_len)
        Z_PARAM_OPTIONAL
        Z_PARAM_BOOL(color)
        Z_PARAM_STRING(tmpl_arg, tmpl_len)
    ZEND_PARSE_PARAMETERS_END();

    /* Resolve through the formatter registry — the same lookup setLogSinks
     * uses — so the goldens also cover the plugin seam. ud stays hook-built
     * (make_ud wants a sink spec): pretty = colour flag, syslog = facility,
     * template = compiled template (freed below). */
    const http_log_formatter_def_t *fdef =
        http_log_formatter_by_name(style, style_len);

    if (fdef == NULL) {
        RETURN_FALSE;
    }

    http_log_formatter_fn fmt      = fdef->fn;
    void                 *fmt_ud   = NULL;
    bool                  ud_owned = false;

    if (style_len == 6 && memcmp(style, "pretty", 6) == 0) {
        fmt_ud = color ? (void *)1 : NULL;
    } else if (style_len == 6 && memcmp(style, "syslog", 6) == 0) {
        fmt_ud = (void *)(intptr_t)1;   /* facility = user */
    } else if (style_len == 8 && memcmp(style, "template", 8) == 0) {
        fmt_ud = http_log_template_parse(tmpl_arg, tmpl_len);
        if (fmt_ud == NULL) {
            RETURN_FALSE;
        }
        ud_owned = true;
    }

    const http_log_attr_t attrs[] = {
        { .key = "path", .type = HTTP_LOG_ATTR_STR,  .v.s   = "/a b" },
        { .key = "tag",  .type = HTTP_LOG_ATTR_STR,  .v.s   = "v\"1" },
        { .key = "line", .type = HTTP_LOG_ATTR_STR,  .v.s   = "a\nb" },
        { .key = "n",    .type = HTTP_LOG_ATTR_I64,  .v.i64 = -7 },
        { .key = "sz",   .type = HTTP_LOG_ATTR_U64,  .v.u64 = 4294967296ULL },
        { .key = "ok",   .type = HTTP_LOG_ATTR_BOOL, .v.b   = true },
        { .key = "r",    .type = HTTP_LOG_ATTR_F64,  .v.f64 = 1.5 },
    };

    http_log_record_t rec = {
        .state        = NULL,
        .timestamp_ns = 1704067200123000000ULL,   /* 2024-01-01T00:00:00.123Z */
        .severity     = HTTP_LOG_INFO,
        .tmpl         = "user login",
        .body         = "user login",
        .body_len     = sizeof("user login") - 1,
        .attrs        = attrs,
        .attrs_count  = sizeof attrs / sizeof attrs[0],
        .has_trace    = true,
    };

    for (uint8_t i = 0; i < sizeof rec.trace_id; i++) {
        rec.trace_id[i] = i;
    }
    for (uint8_t i = 0; i < sizeof rec.span_id; i++) {
        rec.span_id[i] = i;
    }

    char   buf[2048];
    size_t n = fmt(&rec, buf, sizeof buf, fmt_ud);

    if (ud_owned) {
        efree(fmt_ud);
    }

    RETURN_STRINGL(buf, n);
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_log_color_decide, 0, 0, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

/* Resolve the pretty-sink colour decision (issue #5, B3) against the current
 * environment on a non-TTY fd, so the phpt can assert NO_COLOR / CLICOLOR_FORCE
 * are honoured without needing a real terminal. */
PHP_FUNCTION(_http_log_color_decide)
{
    ZEND_PARSE_PARAMETERS_NONE();

    RETURN_BOOL(http_log_color_for_fd(STDOUT_FILENO));
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_log_registry_names, 0, 0, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

/* Snapshot the sink-type / formatter registry (issue #5, B5d) as
 * {'types' => [...], 'formatters' => [...]} so a phpt can assert the
 * built-ins registered and the pipe-joined error lists match. */
PHP_FUNCTION(_http_log_registry_names)
{
    ZEND_PARSE_PARAMETERS_NONE();

    char types[256];
    char formatters[256];
    http_log_sink_type_names(types, sizeof types);
    http_log_formatter_names(formatters, sizeof formatters);

    array_init(return_value);
    add_assoc_string(return_value, "types", types);
    add_assoc_string(return_value, "formatters", formatters);
}

static const zend_function_entry reactor_pool_test_functions[] = {
    ZEND_FE(_http_server_reactor_pool_selftest, arginfo_reactor_pool_selftest)
    ZEND_FE(_http_server_persistent_request_selftest, arginfo_persistent_request_selftest)
    ZEND_FE(_http_server_reactor_pool_exec_selftest, arginfo_reactor_pool_exec_selftest)
    ZEND_FE(_http_server_reactor_post_exec_selftest, arginfo_reactor_post_exec_selftest)
    ZEND_FE(_http_server_dispatch_from_wire_selftest, arginfo_dispatch_from_wire_selftest)
    ZEND_FE(_http_server_worker_inbox_selftest, arginfo_worker_inbox_selftest)
    ZEND_FE(_http_server_worker_registry_selftest, arginfo_worker_registry_selftest)
    ZEND_FE(_http_server_worker_registry_route_selftest, arginfo_worker_registry_route_selftest)
    ZEND_FE(_http_server_reactor_h3_listener_selftest, arginfo_reactor_h3_listener_selftest)
    ZEND_FE(_http_server_stats_registry_selftest, arginfo_stats_registry_selftest)
    ZEND_FE(_http_server_stats_slab_snapshot, arginfo_stats_slab_snapshot)
    ZEND_FE(_http_log_format_selftest, arginfo_log_format_selftest)
    ZEND_FE(_http_log_color_decide, arginfo_log_color_decide)
    ZEND_FE(_http_log_registry_names, arginfo_log_registry_names)
    PHP_FE_END
};

void reactor_pool_test_register(const int module_type)
{
    zend_register_functions(NULL, reactor_pool_test_functions, NULL, module_type);
}

#else /* !HTTP_SERVER_TEST_HOOKS */

void reactor_pool_test_register(const int module_type)
{
    (void)module_type;
}

#endif /* HTTP_SERVER_TEST_HOOKS */
