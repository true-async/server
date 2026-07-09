/*
 * +----------------------------------------------------------------------+
 * | TrueAsync HTTP Server                                                |
 * +----------------------------------------------------------------------+
 *
 * Plain in-thread async event — bare zend_async_event_t, no libuv
 * handle. Use for coroutine wakeups (CODING_STANDARDS §1.4).
 */

#ifndef ASYNC_PLAIN_EVENT_H
#define ASYNC_PLAIN_EVENT_H

#include "Zend/zend_async_API.h"

zend_async_event_t *async_plain_event_new(void);

static zend_always_inline void async_plain_event_fire(zend_async_event_t *event)
{
    if (event != NULL && !ZEND_ASYNC_EVENT_IS_CLOSED(event)) {
        zend_async_callbacks_notify(event, NULL, NULL, false);
    }
}

/* Suspend `co` for `ms` on a one-shot timer; the worker loop keeps draining
 * (mailbox, scheduler) meanwhile. Leaves a resume exception in EG for the
 * caller to inspect or clear (a create failure is cleared internally). */
void async_coroutine_sleep_ms(zend_coroutine_t *co, zend_ulong ms);

#endif /* ASYNC_PLAIN_EVENT_H */
