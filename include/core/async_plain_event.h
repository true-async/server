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

#endif /* ASYNC_PLAIN_EVENT_H */
