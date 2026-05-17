/*
 * +----------------------------------------------------------------------+
 * | TrueAsync HTTP Server                                                |
 * +----------------------------------------------------------------------+
 *
 * Plain in-thread async event.
 *
 * A minimal zend_async_event_t with no underlying libuv handle. Callers
 * pile waker callbacks onto it via zend_async_resume_when and wake them
 * with async_plain_event_fire(), which is a direct call to
 * zend_async_callbacks_notify — no uv_async_send (= no eventfd_write
 * syscall), no extra loop iteration, no cross-thread serialization.
 *
 * Use this whenever the consumer of the event is a COROUTINE — see
 * docs/CODING_STANDARDS.md §1.4 for the full rule. The waker callback
 * only enqueues the coroutine into ASYNC_G(coroutine_queue), so no
 * recursion is possible even if the producer fires from deep inside
 * an I/O stack frame.
 *
 * Do NOT use this if any callback registered on the event itself
 * performs I/O (e.g. calls http2_session_emit) — those wakeups must
 * be deferred via a microtask (ZEND_ASYNC_ADD_MICROTASK; see
 * src/core/http_connection.c::conn_destroy_microtask_handler for the
 * reference implementation) to avoid reentering non-reentrant code
 * such as nghttp2_session_send.
 *
 * Trigger event (ZEND_ASYNC_NEW_TRIGGER_EVENT) is reserved for
 * CROSS-THREAD wakeups — do not use it as an in-thread defer.
 */

#ifndef ASYNC_PLAIN_EVENT_H
#define ASYNC_PLAIN_EVENT_H

#include "Zend/zend_async_API.h"

/* Allocate + initialise a plain event with refcount=1. Caller takes
 * ownership; dispose via event->dispose(event) (matches the trigger-
 * event lifecycle so swap-in is mechanical). */
zend_async_event_t *async_plain_event_new(void);

/* Fire all attached wakers. Direct callbacks_notify — schedules the
 * woken coroutines into ASYNC_G(coroutine_queue); the actual fiber
 * switch happens at the next scheduler tick, NOT inline. Safe to call
 * from reactor callbacks. */
static zend_always_inline void async_plain_event_fire(zend_async_event_t *event)
{
    if (event != NULL && !ZEND_ASYNC_EVENT_IS_CLOSED(event)) {
        zend_async_callbacks_notify(event, NULL, NULL, false);
    }
}

#endif /* ASYNC_PLAIN_EVENT_H */
