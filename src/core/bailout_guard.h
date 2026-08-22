/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

#ifndef HTTP_BAILOUT_GUARD_H
#define HTTP_BAILOUT_GUARD_H

#include "zend.h"
#include "zend_gc.h"
#include "zend_globals.h"
#include "zend_globals_macros.h"

/* Engine state that _zend_bailout() changes on its way out and that
 * zend_end_try() does not give back — it restores EG(bailout) and nothing
 * else. Every firewall in this server catches a bailout and then keeps
 * running, because one failed stream must not take the connection and one
 * failed request must not take the worker, so each of them owes this
 * restore.
 *
 * EG(current_execute_data) is cleared before the longjmp. The callback that
 * bailed out runs on the C stack of whichever coroutine is driving the
 * reactor tick, and a coroutine woken inside that same tick returns from its
 * suspend without a context switch — nothing else puts the frame back. An
 * internal method declared `static` resolves that type through
 * EG(current_execute_data), so a null frame turns the return into
 * E_CORE_ERROR and abandons the request mid-response.
 *
 * gc_protect is armed by the same function. Left armed, the cycle collector
 * never runs again in this process. */
typedef struct {
    zend_execute_data *frame;
    bool               gc_protected;
} http_bailout_state_t;

/* Snapshot taken immediately before zend_try. Assigned before the SETJMP and
 * never touched inside the block, so it needs no volatile. */
static zend_always_inline void http_bailout_state_save(http_bailout_state_t *state)
{
    state->frame        = EG(current_execute_data);
    state->gc_protected = gc_protected();
}

/* First statement of zend_catch, before anything that reads engine state.
 * Allocates nothing, which is what makes it safe where an emalloc is not. */
static zend_always_inline void http_bailout_state_restore(const http_bailout_state_t *state)
{
    EG(current_execute_data) = state->frame;
    gc_protect(state->gc_protected);
}

#endif /* HTTP_BAILOUT_GUARD_H */
