/*
 * +----------------------------------------------------------------------+
 * | TrueAsync HTTP Server                                                |
 * +----------------------------------------------------------------------+
 *
 * Plain in-thread event — see include/core/async_plain_event.h.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "php.h"
#include "Zend/zend_exceptions.h"   /* zend_clear_exception */
#include "Zend/zend_async_API.h"
#include "core/async_plain_event.h"

static bool plain_add_callback(zend_async_event_t *event, zend_async_event_callback_t *callback)
{
    return zend_async_callbacks_push(event, callback);
}

static bool plain_del_callback(zend_async_event_t *event, zend_async_event_callback_t *callback)
{
    return zend_async_callbacks_remove(event, callback);
}

static bool plain_start(zend_async_event_t *event)
{
    (void)event;
    return true;
}

static bool plain_stop(zend_async_event_t *event)
{
    (void)event;
    return true;
}

static bool plain_dispose(zend_async_event_t *event)
{
    if (event == NULL) {
        return true;
    }

    if (ZEND_ASYNC_EVENT_REFCOUNT(event) > 1) {
        ZEND_ASYNC_EVENT_DEL_REF(event);
        return true;
    }

    zend_async_callbacks_free(event);
    pefree(event, 0);
    return true;
}

void async_coroutine_sleep_ms(zend_coroutine_t *co, const zend_ulong ms)
{
    zend_async_timer_event_t *const t = ZEND_ASYNC_NEW_TIMER_EVENT(ms, false);

    if (UNEXPECTED(t == NULL)) {
        zend_clear_exception();
        return;
    }

    t->base.start(&t->base);
    zend_async_resume_when(co, &t->base, true, zend_async_waker_callback_resolve, NULL);
    ZEND_ASYNC_SUSPEND();
    ZEND_ASYNC_WAKER_DESTROY(co);
}

zend_async_event_t *async_plain_event_new(void)
{
    zend_async_event_t *event = pecalloc(1, sizeof(*event), 0);

    event->ref_count    = 1;
    event->add_callback = plain_add_callback;
    event->del_callback = plain_del_callback;
    event->start        = plain_start;
    event->stop         = plain_stop;
    event->dispose      = plain_dispose;

    return event;
}
