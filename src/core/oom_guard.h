/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

#ifndef HTTP_OOM_GUARD_H
#define HTTP_OOM_GUARD_H

#include "zend.h"
#include "zend_alloc.h"
#include "zend_globals.h"
#include "zend_globals_macros.h"
#include "log/http_log.h"

/* A request body is buffered with fallible allocations (femalloc,
 * zend_string_falloc, smart_str_fappendl_ex) so that a body past
 * memory_limit refuses that request alone. Since php-src 3584a98bda0 an
 * allocation past memory_limit does not bail out: a non-fallible one
 * succeeds and queues a delayed error, and one past the hard limit (twice
 * memory_limit by default) ends the process with _exit(137), taking every
 * connection of the worker with it. A fallible one returns NULL instead, but
 * still queues the delayed error in EG(delayed_oom), which the engine raises
 * as a fatal UnwindExit in whichever coroutine runs PHP code next, and which
 * php-async answers by shutting the scheduler down.
 *
 * The scope drops that error when an allocation inside it queued it: the
 * refused allocation did not happen, so no code is owed the fatal. A small
 * non-fallible allocation in the scope (a strdup, the bookkeeping node of a
 * huge block) may have passed memory_limit by its own size; the next
 * allocation past the limit queues the error again. An error already pending
 * when the scope began belongs to the code that caused it and is left in
 * place. */
typedef struct {
    bool pending_before;
} http_oom_scope_t;

/* Taken immediately before the fallible allocations it covers. */
static zend_always_inline void http_oom_scope_begin(http_oom_scope_t *scope)
{
    scope->pending_before = ZEND_HAS_OOM();
}

/* True when an allocation since http_oom_scope_begin() hit memory_limit:
 * logs the engine's message as a warning to `log` (NULL: nowhere) and drops
 * the delayed error. The caller refuses the request. A huge allocation can
 * return memory and still queue the error (its bookkeeping node is not
 * fallible), so the caller checks this even after a non-NULL result. With an
 * error already pending at begin this answers false: the caller still has
 * the NULL it got. */
static zend_always_inline bool http_oom_scope_end(const http_oom_scope_t *scope,
                                                  http_log_state_t *log)
{
    if (EXPECTED(scope->pending_before || !ZEND_HAS_OOM())) {
        return false;
    }

    zend_oom_info *const oom = &EG(delayed_oom);

    http_logf_warn(log, "request body refused: %s",
                   oom->message != NULL ? ZSTR_VAL(oom->message) : "memory_limit exhausted");

    /* zend_mm_oom_info_dtor() does the same, but it is not exported. */
    if (oom->message != NULL) {
        zend_string_release(oom->message);
        oom->message = NULL;
    }

    if (oom->filename != NULL) {
        zend_string_release(oom->filename);
        oom->filename = NULL;
    }

    oom->lineno = 0;
    oom->state  = ZEND_OOM_STATE_NONE;
    return true;
}

#endif /* HTTP_OOM_GUARD_H */
