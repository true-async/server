/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "php.h"
#include "Zend/zend_async_API.h"
#include "core/thread_mailbox.h"
#include "core/thread_queue.h"

/* The mailbox pointer lives in the trigger event's persistent extra area so the
 * drain callback can recover it from the event alone (see thread_mailbox_create). */

struct thread_mailbox_s {
    thread_mpsc_t              *queue;   /* shared with producers              */
    zend_async_trigger_event_t *trigger; /* consumer-loop wakeup (uv_async)    */
    thread_mailbox_drain_fn     on_drain;
    void                       *arg;
    void                      **batch_buf; /* consumer-private drain scratch   */
    size_t                      batch;
};

/* Fired on the consumer's reactor thread once per empty->non-empty edge (plus
 * any spurious coalesced wakeups). Drain to empty so nothing is stranded. */
static void mailbox_on_signal(zend_async_event_t *event, zend_async_event_callback_t *callback,
                              void *result, zend_object *exception)
{
    (void) callback;
    (void) result;
    (void) exception;

    thread_mailbox_t *mb = *(thread_mailbox_t **) ((char *) event + event->extra_offset);

    for (;;) {
        const size_t n = thread_mpsc_drain(mb->queue, mb->batch_buf, mb->batch);
        if (n == 0) {
            break;
        }

        mb->on_drain(mb->batch_buf, n, mb->arg);
    }
}

thread_mailbox_t *thread_mailbox_create(const size_t capacity, const size_t batch,
                                        thread_mailbox_drain_fn on_drain, void *arg)
{
    if (capacity == 0 || batch == 0 || on_drain == NULL) {
        return NULL;
    }

    thread_mailbox_t *mb = pecalloc(1, sizeof(*mb), 0);

    mb->on_drain  = on_drain;
    mb->arg       = arg;
    mb->batch     = batch;
    mb->batch_buf = pemalloc(batch * sizeof(void *), 0);
    mb->queue     = thread_mpsc_create(capacity);

    if (mb->queue == NULL) {
        goto fail;
    }

    /* Trigger event is a libuv handle on THIS thread's reactor — only the
     * consumer may create it. Carries the mailbox pointer in its extra area. */
    mb->trigger = ZEND_ASYNC_NEW_TRIGGER_EVENT_EX(sizeof(thread_mailbox_t *));
    if (mb->trigger == NULL) {
        goto fail;
    }

    *(thread_mailbox_t **) ((char *) &mb->trigger->base + mb->trigger->base.extra_offset) = mb;

    zend_async_event_callback_t *cb = ZEND_ASYNC_EVENT_CALLBACK(mailbox_on_signal);
    mb->trigger->base.add_callback(&mb->trigger->base, cb);

    return mb;

fail:
    if (mb->trigger != NULL) {
        ZEND_ASYNC_EVENT_SET_CLOSED(&mb->trigger->base);
        mb->trigger->base.dispose(&mb->trigger->base);
    }

    if (mb->queue != NULL) {
        thread_mpsc_free(mb->queue);
    }

    pefree(mb->batch_buf, 0);
    pefree(mb, 0);
    return NULL;
}

void thread_mailbox_free(thread_mailbox_t *mb)
{
    if (mb == NULL) {
        return;
    }

    /* Mark closed first so a late producer's signal is a no-op (the trigger
     * checks IS_CLOSED before uv_async_send), then dispose the handle. */
    if (mb->trigger != NULL) {
        ZEND_ASYNC_EVENT_SET_CLOSED(&mb->trigger->base);
        mb->trigger->base.dispose(&mb->trigger->base);
    }

    thread_mpsc_free(mb->queue);
    pefree(mb->batch_buf, 0);
    pefree(mb, 0);
}

bool thread_mailbox_post(thread_mailbox_t *mb, void *item)
{
    bool was_empty = false;

    if (!thread_mpsc_enqueue(mb->queue, item, &was_empty)) {
        return false;
    }

    if (was_empty) {
        mb->trigger->trigger(mb->trigger);
    }

    return true;
}

size_t thread_mailbox_count(const thread_mailbox_t *mb)
{
    return thread_mpsc_count(mb->queue);
}
