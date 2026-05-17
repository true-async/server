/*
 * +----------------------------------------------------------------------+
 * | TrueAsync HTTP Server                                                |
 * +----------------------------------------------------------------------+
 *
 * Plain in-thread event — see include/core/async_plain_event.h.
 *
 * Pattern lifted from ext/async/task_group.c::task_group_event_init —
 * the existing canonical in-thread event in PHP-async. We replicate it
 * here as a server-local helper to keep ABI bumps out of scope.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "php.h"
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
    /* No reactor handle to ref — start/stop are bookkeeping only. */
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
