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
#include "zend_exceptions.h"
#include "websocket/ws_hub.h"
#include "websocket/ws_session.h"
#include "core/thread_mailbox.h"

#include <TSRM.h>

#define WS_HUB_MAILBOX_CAPACITY 4096
#define WS_HUB_MAILBOX_BATCH      64

/* zend_string's refcount is NOT atomic, so the name is owned outright rather
 * than shared through Zend refcounting. */
struct ws_room_s {
    zend_atomic_int refcount;
    zend_string    *name;
};

struct ws_hub_s {
    /* Taken to intern/retire a room and to mint a ws id. Never on a send path. */
    MUTEX_T    admin;
    HashTable  rooms;          /* name -> ws_room_t*, persistent */
    uint64_t   next_ws_id;

    thread_mailbox_t *inbox[WS_HUB_MAX_WORKERS];
    bool              taken[WS_HUB_MAX_WORKERS];
};

typedef struct {
    zend_atomic_int refcount;
    size_t          len;
    bool            binary;
    char            data[1];
} ws_payload_t;

/* Refcounted because a timed-out caller must not free this under a worker that
 * is still answering. */
typedef struct {
    zend_atomic_int             refcount;
    zend_atomic_int             pending;
    zend_atomic_int             total;
    zend_async_trigger_event_t *done;
} ws_query_t;

typedef enum {
    WS_CMD_BROADCAST,
    WS_CMD_COUNT,
} ws_cmd_kind_t;

typedef struct {
    ws_cmd_kind_t kind;
    ws_room_t    *room;        /* holds a reference for the length of the trip */
    uint64_t      except_id;   /* broadcast */
    ws_payload_t *payload;     /* broadcast */
    ws_query_t   *query;       /* count */
} ws_cmd_t;

/* Dense array, not a hash: delivery only walks it, and the walk must not
 * allocate. `dead` counts tombstones (see ws_room_local_remove). */
typedef struct {
    ws_session_t **items;
    uint32_t       count;
    uint32_t       cap;
    uint32_t       dead;
    bool           iterating;
} ws_room_local_t;

typedef struct ws_room_link {
    struct ws_room_link *next;
    ws_room_t           *room;
    uint32_t             idx;
} ws_room_link_t;

typedef struct {
    ws_hub_t         *hub;
    int               slot;
    thread_mailbox_t *inbox;
    HashTable         rooms;   /* ws_room_t* -> ws_room_local_t* */
} ws_local_t;

ZEND_TLS ws_local_t *ws_local = NULL;

/* Safe as a key: the pointer is stable while a reference is held. */
#define WS_ROOM_KEY(room) ((zend_ulong)(uintptr_t)(room))

static ws_payload_t *ws_payload_new(const char *data, const size_t len, const bool binary)
{
    ws_payload_t *const payload = pemalloc(sizeof(*payload) + len, 1);
    ZEND_ATOMIC_INT_INIT(&payload->refcount, 1);
    payload->len    = len;
    payload->binary = binary;
    memcpy(payload->data, data, len);

    return payload;
}

static void ws_payload_release(ws_payload_t *payload)
{
    if (zend_atomic_int_fetch_add(&payload->refcount, -1) == 1) {
        pefree(payload, 1);
    }
}

static void ws_query_release(ws_query_t *query)
{
    if (zend_atomic_int_fetch_add(&query->refcount, -1) == 1) {
        pefree(query, 1);
    }
}

zend_string *ws_room_name(const ws_room_t *room)
{
    return room->name;
}

ws_room_t *ws_hub_room(ws_hub_t *hub, zend_string *name)
{
    if (hub == NULL) {
        return NULL;
    }

    tsrm_mutex_lock(hub->admin);

    ws_room_t *room = zend_hash_find_ptr(&hub->rooms, name);

    if (room != NULL) {
        /* Cannot race a retire that already hit zero: it re-checks under the
         * mutex and backs off when a lookup revived the room. */
        zend_atomic_int_fetch_add(&room->refcount, 1);
    } else {
        room = pemalloc(sizeof(*room), 1);
        ZEND_ATOMIC_INT_INIT(&room->refcount, 1);
        room->name = zend_string_init(ZSTR_VAL(name), ZSTR_LEN(name), 1);

        zend_hash_add_new_ptr(&hub->rooms, room->name, room);
    }

    tsrm_mutex_unlock(hub->admin);

    return room;
}

void ws_room_release(ws_hub_t *hub, ws_room_t *room)
{
    if (hub == NULL || room == NULL) {
        return;
    }

    /* Hot path stays lock-free: only the drop that reaches zero takes the mutex. */
    if (zend_atomic_int_fetch_add(&room->refcount, -1) != 1) {
        return;
    }

    tsrm_mutex_lock(hub->admin);

    if (zend_atomic_int_load(&room->refcount) == 0) {
        zend_hash_del(&hub->rooms, room->name);
        zend_string_release(room->name);
        pefree(room, 1);
    }

    tsrm_mutex_unlock(hub->admin);
}

static void ws_room_local_free(zval *zv)
{
    ws_room_local_t *const local = Z_PTR_P(zv);

    efree(local->items);

    efree(local);
}

static ws_room_local_t *ws_room_local_get(const ws_room_t *room, const bool create)
{
    ws_room_local_t *local = zend_hash_index_find_ptr(&ws_local->rooms, WS_ROOM_KEY(room));

    if (local != NULL || !create) {
        return local;
    }

    local        = ecalloc(1, sizeof(*local));
    local->cap   = 8;
    local->items = ecalloc(local->cap, sizeof(*local->items));
    zend_hash_index_add_new_ptr(&ws_local->rooms, WS_ROOM_KEY(room), local);

    return local;
}

static ws_room_link_t *ws_session_link(const ws_session_t *session, const ws_room_t *room)
{
    for (ws_room_link_t *link = session->rooms; link != NULL; link = link->next) {
        if (link->room == room) {
            return link;
        }
    }

    return NULL;
}

/* Close the hole by moving the last member into it (O(1)), then fix the slot it
 * remembers. Mid-delivery the array must not shift, so leave a tombstone. */
static void ws_room_local_remove(ws_room_local_t *local, const ws_room_t *room,
                                 const uint32_t idx)
{
    if (local->iterating) {
        local->items[idx] = NULL;
        local->dead++;
        return;
    }

    const uint32_t last = local->count - 1;

    if (idx != last) {
        ws_session_t *const moved = local->items[last];
        local->items[idx] = moved;

        ws_room_link_t *const link = ws_session_link(moved, room);

        if (link != NULL) {
            link->idx = idx;
        }
    }

    local->count = last;
}

static void ws_room_local_compact(ws_room_local_t *local, const ws_room_t *room)
{
    if (local->dead == 0) {
        return;
    }

    uint32_t out = 0;

    for (uint32_t i = 0; i < local->count; i++) {
        ws_session_t *const session = local->items[i];

        if (session == NULL) {
            continue;
        }

        local->items[out] = session;

        ws_room_link_t *const link = ws_session_link(session, room);

        if (link != NULL) {
            link->idx = out;
        }

        out++;
    }

    local->count = out;
    local->dead  = 0;
}

ws_hub_t *ws_hub_create(void)
{
    ws_hub_t *const hub = pecalloc(1, sizeof(*hub), 1);
    hub->admin      = tsrm_mutex_alloc();
    hub->next_ws_id = 1;
    zend_hash_init(&hub->rooms, 8, NULL, NULL, 1);

    return hub;
}

void ws_hub_free(ws_hub_t *hub)
{
    if (hub == NULL) {
        return;
    }

    ws_room_t *room;
    ZEND_HASH_FOREACH_PTR(&hub->rooms, room) {
        zend_string_release(room->name);
        pefree(room, 1);
    } ZEND_HASH_FOREACH_END();

    zend_hash_destroy(&hub->rooms);

    tsrm_mutex_free(hub->admin);

    pefree(hub, 1);
}

static void ws_hub_drain(void **items, size_t count, void *arg);

int ws_hub_attach(ws_hub_t *hub)
{
    if (hub == NULL || ws_local != NULL) {
        return -1;
    }

    thread_mailbox_t *const inbox = thread_mailbox_create(
        WS_HUB_MAILBOX_CAPACITY, WS_HUB_MAILBOX_BATCH, ws_hub_drain, hub);

    if (inbox == NULL) {
        return -1;
    }

    int slot = -1;

    tsrm_mutex_lock(hub->admin);
    for (int i = 0; i < WS_HUB_MAX_WORKERS; i++) {
        if (!hub->taken[i]) {
            hub->taken[i] = true;
            hub->inbox[i] = inbox;
            slot          = i;
            break;
        }
    }
    tsrm_mutex_unlock(hub->admin);

    if (slot < 0) {
        thread_mailbox_free(inbox);
        return -1;
    }

    ws_local        = ecalloc(1, sizeof(*ws_local));
    ws_local->hub   = hub;
    ws_local->slot  = slot;
    ws_local->inbox = inbox;
    zend_hash_init(&ws_local->rooms, 8, NULL, ws_room_local_free, 0);

    return slot;
}

void ws_hub_detach(void)
{
    if (ws_local == NULL) {
        return;
    }

    ws_hub_t *const hub = ws_local->hub;

    tsrm_mutex_lock(hub->admin);
    hub->inbox[ws_local->slot] = NULL;
    hub->taken[ws_local->slot] = false;
    tsrm_mutex_unlock(hub->admin);

    thread_mailbox_free(ws_local->inbox);

    zend_hash_destroy(&ws_local->rooms);

    efree(ws_local);
    ws_local = NULL;
}

bool ws_hub_join(ws_room_t *room, ws_session_t *session)
{
    if (ws_local == NULL || room == NULL) {
        return false;
    }

    if (ws_session_link(session, room) != NULL) {
        return true;
    }

    if (session->ws_id == 0) {
        tsrm_mutex_lock(ws_local->hub->admin);
        session->ws_id = ws_local->hub->next_ws_id++;
        tsrm_mutex_unlock(ws_local->hub->admin);
    }

    ws_room_local_t *const local = ws_room_local_get(room, true);

    if (local->count == local->cap) {
        local->cap  *= 2;
        local->items = erealloc(local->items, local->cap * sizeof(*local->items));
    }

    local->items[local->count] = session;

    /* The membership holds a reference — the room must outlive its members even
     * once every PHP WebSocketRoom object is gone. */
    zend_atomic_int_fetch_add(&room->refcount, 1);

    ws_room_link_t *const link = emalloc(sizeof(*link));
    link->room     = room;
    link->idx      = local->count;
    link->next     = session->rooms;
    session->rooms = link;

    local->count++;

    return true;
}

static void ws_hub_unlink(ws_session_t *session, ws_room_link_t *link,
                          ws_room_link_t *prev)
{
    ws_room_local_t *const local = ws_room_local_get(link->room, false);

    if (local != NULL) {
        ws_room_local_remove(local, link->room, link->idx);
    }

    if (prev != NULL) {
        prev->next = link->next;
    } else {
        session->rooms = link->next;
    }

    ws_room_release(ws_local->hub, link->room);

    efree(link);
}

bool ws_hub_leave(ws_room_t *room, ws_session_t *session)
{
    if (ws_local == NULL || room == NULL) {
        return false;
    }

    ws_room_link_t *prev = NULL;
    ws_room_link_t *link = session->rooms;

    while (link != NULL && link->room != room) {
        prev = link;
        link = link->next;
    }

    if (link == NULL) {
        return false;
    }

    ws_hub_unlink(session, link, prev);

    return true;
}

void ws_hub_leave_all(ws_session_t *session)
{
    if (ws_local == NULL) {
        return;
    }

    while (session->rooms != NULL) {
        ws_hub_unlink(session, session->rooms, NULL);
    }
}

static uint32_t ws_local_deliver(ws_room_t *room, const ws_payload_t *payload,
                                 const uint64_t except_id)
{
    ws_room_local_t *const local = ws_room_local_get(room, false);

    if (local == NULL || local->count == 0) {
        return 0;
    }

    /* A send can tear its own session down, re-entering leave_all mid-walk. */
    const bool nested = local->iterating;
    local->iterating  = true;

    uint32_t sent = 0;

    for (uint32_t i = 0; i < local->count; i++) {
        ws_session_t *const session = local->items[i];

        if (session == NULL || session->ws_id == except_id) {
            continue;
        }

        if (ws_session_try_send(session, payload->data, payload->len, payload->binary)) {
            sent++;
        }
    }

    if (!nested) {
        local->iterating = false;
        ws_room_local_compact(local, room);
    }

    return sent;
}

static uint32_t ws_local_count(const ws_room_t *room)
{
    const ws_room_local_t *const local = ws_room_local_get(room, false);

    return local != NULL ? local->count - local->dead : 0;
}

static void ws_hub_drain(void **items, const size_t count, void *arg)
{
    ws_hub_t *const hub = arg;

    for (size_t i = 0; i < count; i++) {
        ws_cmd_t *const cmd = items[i];

        if (cmd->kind == WS_CMD_BROADCAST) {
            if (ws_local != NULL) {
                (void)ws_local_deliver(cmd->room, cmd->payload, cmd->except_id);
            }

            ws_payload_release(cmd->payload);
        } else {
            ws_query_t *const query = cmd->query;

            if (ws_local != NULL) {
                zend_atomic_int_fetch_add(&query->total, (int)ws_local_count(cmd->room));
            }

            if (zend_atomic_int_fetch_add(&query->pending, -1) == 1) {
                query->done->trigger(query->done);
            }

            ws_query_release(query);
        }

        ws_room_release(hub, cmd->room);

        pefree(cmd, 1);
    }
}

static ws_cmd_t *ws_cmd_new(const ws_cmd_kind_t kind, ws_room_t *room)
{
    ws_cmd_t *const cmd = pecalloc(1, sizeof(*cmd), 1);
    cmd->kind = kind;
    cmd->room = room;

    zend_atomic_int_fetch_add(&room->refcount, 1);

    return cmd;
}

static void ws_cmd_free(ws_hub_t *hub, ws_cmd_t *cmd)
{
    ws_room_release(hub, cmd->room);

    pefree(cmd, 1);
}

uint32_t ws_hub_broadcast(ws_hub_t *hub, ws_room_t *room,
                          const char *data, const size_t len, const bool binary,
                          const uint64_t except_id)
{
    if (hub == NULL || room == NULL) {
        return 0;
    }

    ws_payload_t *const payload = ws_payload_new(data, len, binary);

    uint32_t sent = 0;

    if (ws_local != NULL) {
        sent = ws_local_deliver(room, payload, except_id);
    }

    for (int slot = 0; slot < WS_HUB_MAX_WORKERS; slot++) {
        if (ws_local != NULL && slot == ws_local->slot) {
            continue;
        }

        thread_mailbox_t *const inbox = hub->inbox[slot];

        if (inbox == NULL) {
            continue;
        }

        ws_cmd_t *const cmd = ws_cmd_new(WS_CMD_BROADCAST, room);
        cmd->except_id = except_id;
        cmd->payload   = payload;

        zend_atomic_int_fetch_add(&payload->refcount, 1);

        if (!thread_mailbox_post(inbox, cmd)) {
            ws_payload_release(payload);
            ws_cmd_free(hub, cmd);
        }
    }

    ws_payload_release(payload);

    return sent;
}

uint32_t ws_hub_count(ws_hub_t *hub, ws_room_t *room, const uint32_t timeout_ms)
{
    if (hub == NULL || room == NULL || ws_local == NULL) {
        return 0;
    }

    const uint32_t local = ws_local_count(room);

    zend_coroutine_t *const me = ZEND_ASYNC_CURRENT_COROUTINE;

    if (me == NULL || ZEND_ASYNC_IS_SCHEDULER_CONTEXT) {
        return local;   /* no coroutine to park — this worker's answer is all there is */
    }

    zend_async_trigger_event_t *const done = ZEND_ASYNC_NEW_TRIGGER_EVENT();

    if (done == NULL) {
        return local;
    }

    ws_query_t *const query = pemalloc(sizeof(*query), 1);
    ZEND_ATOMIC_INT_INIT(&query->refcount, 1);
    ZEND_ATOMIC_INT_INIT(&query->pending, 0);
    ZEND_ATOMIC_INT_INIT(&query->total, (int)local);
    query->done = done;

    /* Subscribe BEFORE posting: a worker can answer, and fire `done`, before we
     * reach the suspend — that wakeup must not be lost. */
    const bool armed = zend_async_waker_new_with_timeout(me, timeout_ms, NULL) != NULL;

    if (armed) {
        zend_async_resume_when(me, &done->base, false,
                               zend_async_waker_callback_resolve, NULL);
    }

    int asked = 0;

    for (int slot = 0; slot < WS_HUB_MAX_WORKERS; slot++) {
        if (slot == ws_local->slot || hub->inbox[slot] == NULL) {
            continue;
        }

        ws_cmd_t *const cmd = ws_cmd_new(WS_CMD_COUNT, room);
        cmd->query = query;

        zend_atomic_int_fetch_add(&query->refcount, 1);
        zend_atomic_int_fetch_add(&query->pending, 1);

        if (thread_mailbox_post(hub->inbox[slot], cmd)) {
            asked++;
        } else {
            zend_atomic_int_fetch_add(&query->pending, -1);
            ws_query_release(query);
            ws_cmd_free(hub, cmd);
        }
    }

    if (armed) {
        if (asked > 0) {
            ZEND_ASYNC_SUSPEND();
        }

        zend_async_waker_clean(me);

        if (EG(exception) != NULL) {
            zend_clear_exception();
        }
    }

    const uint32_t total = (uint32_t)zend_atomic_int_load(&query->total);

    done->base.dispose(&done->base);

    ws_query_release(query);

    return total;
}
