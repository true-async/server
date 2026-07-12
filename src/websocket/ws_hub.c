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
#include "core/async_plain_event.h"

#include <TSRM.h>

#define WS_HUB_MAILBOX_CAPACITY 4096
#define WS_HUB_MAILBOX_BATCH      64

/* zend_string's refcount is NOT atomic, so the name is owned outright rather
 * than shared through Zend refcounting: it is only ever addref'd/released under
 * the hub's `admin` mutex, and readers copy it instead of taking a reference. */
struct ws_room_s {
    zend_atomic_int refcount;
    zend_string    *name;
};

struct ws_hub_s {
    zend_atomic_int refcount;

    /* Taken to intern/retire a room, to publish/retire a worker's mailbox, and
     * to post into one. Never held while a session is written to. */
    MUTEX_T    admin;
    HashTable  rooms;          /* name -> ws_room_t*, persistent */

    zend_atomic_int64 next_ws_id;
    zend_atomic_int64 dropped;

    thread_mailbox_t *inbox[WS_HUB_MAX_WORKERS];
    /* Bumped on every attach. A worker that detaches frees its slot for reuse,
     * so a reply in flight must check the generation or it lands on the wrong
     * thread's query. */
    uint32_t          gen[WS_HUB_MAX_WORKERS];
    bool              taken[WS_HUB_MAX_WORKERS];
    int               slots_used;   /* highest slot ever claimed + 1 */
};

typedef struct {
    zend_atomic_int refcount;
    size_t          len;
    bool            binary;
    char            data[1];
} ws_payload_t;

/* Answers travel home through the ASKER's own mailbox, so `pending`, `total`,
 * `abandoned` and `done` are touched on one thread only — no atomics, and a
 * caller that times out can dispose `done` on the spot without racing a worker
 * that is still answering. The refcount is the only cross-thread part: it keeps
 * the struct alive while a command carrying it is in flight. */
typedef struct {
    zend_atomic_int     refcount;
    int                 slot;       /* asker's inbox slot... */
    uint32_t            gen;        /* ...and the generation it was claimed with */
    uint32_t            pending;
    uint32_t            total;
    bool                abandoned;
    zend_async_event_t *done;
} ws_query_t;

typedef enum {
    WS_CMD_BROADCAST,
    WS_CMD_COUNT,
    WS_CMD_COUNT_REPLY,
} ws_cmd_kind_t;

typedef struct {
    ws_cmd_kind_t kind;
    ws_room_t    *room;        /* BROADCAST, COUNT — holds a reference */
    uint64_t      except_id;   /* BROADCAST */
    ws_payload_t *payload;     /* BROADCAST — holds a reference */
    ws_query_t   *query;       /* COUNT, COUNT_REPLY — holds a reference */
    uint32_t      count;       /* COUNT_REPLY */
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
    uint32_t          gen;
    thread_mailbox_t *inbox;
    HashTable         rooms;   /* ws_room_t* -> ws_room_local_t* */
} ws_local_t;

ZEND_TLS ws_local_t *ws_local = NULL;

/* Safe as a key: the pointer is stable while a reference is held. */
#define WS_ROOM_KEY(room) ((zend_ulong)(uintptr_t)(room))

/* zend_atomic has fetch_add for int only. */
static uint64_t ws_atomic_u64_next(zend_atomic_int64 *counter)
{
    int64_t cur = zend_atomic_int64_load(counter);

    while (!zend_atomic_int64_compare_exchange(counter, &cur, cur + 1)) {
        /* cur was refreshed by the failed exchange */
    }

    return (uint64_t) cur;
}

static void ws_hub_note_drop(ws_hub_t *hub)
{
    (void) ws_atomic_u64_next(&hub->dropped);
}

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

ws_hub_t *ws_hub_create(void)
{
    ws_hub_t *const hub = pecalloc(1, sizeof(*hub), 1);

    ZEND_ATOMIC_INT_INIT(&hub->refcount, 1);
    ZEND_ATOMIC_INT64_INIT(&hub->next_ws_id, 1);
    ZEND_ATOMIC_INT64_INIT(&hub->dropped, 0);

    hub->admin = tsrm_mutex_alloc();
    zend_hash_init(&hub->rooms, 8, NULL, NULL, 1);

    return hub;
}

void ws_hub_addref(ws_hub_t *hub)
{
    if (hub != NULL) {
        zend_atomic_int_fetch_add(&hub->refcount, 1);
    }
}

void ws_hub_release(ws_hub_t *hub)
{
    if (hub == NULL || zend_atomic_int_fetch_add(&hub->refcount, -1) != 1) {
        return;
    }

    /* Safety net: a room pinned by a membership that outlived its worker never
     * reached zero on its own. */
    ws_room_t *room;
    ZEND_HASH_FOREACH_PTR(&hub->rooms, room) {
        zend_string_release(room->name);
        pefree(room, 1);
    } ZEND_HASH_FOREACH_END();

    zend_hash_destroy(&hub->rooms);

    tsrm_mutex_free(hub->admin);

    pefree(hub, 1);
}

uint64_t ws_hub_dropped(ws_hub_t *hub)
{
    return hub != NULL ? (uint64_t) zend_atomic_int64_load(&hub->dropped) : 0;
}

ws_room_t *ws_hub_room(ws_hub_t *hub, zend_string *name)
{
    if (hub == NULL) {
        return NULL;
    }

    tsrm_mutex_lock(hub->admin);

    ws_room_t *room = zend_hash_find_ptr(&hub->rooms, name);

    if (room != NULL) {
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

    /* refcount_dec_and_lock. The drop that can retire the room takes `admin`
     * BEFORE decrementing. Decrementing first and re-checking under the mutex
     * looks equivalent but is not: between the two, a lookup can revive the room
     * from zero, release it again and free it — and the first dropper then reads
     * freed memory. Every other drop stays lock-free. */
    int cur = zend_atomic_int_load(&room->refcount);

    while (cur > 1) {
        if (zend_atomic_int_compare_exchange(&room->refcount, &cur, cur - 1)) {
            return;
        }
    }

    tsrm_mutex_lock(hub->admin);

    if (zend_atomic_int_fetch_add(&room->refcount, -1) == 1) {
        zend_hash_del(&hub->rooms, room->name);
        zend_string_release(room->name);
        pefree(room, 1);
    }

    tsrm_mutex_unlock(hub->admin);
}

/* The mailbox is freed by its own worker on detach, and thread_mailbox's
 * contract is "free after producers have quiesced" — so claiming a slot,
 * retiring it and posting into it all happen under `admin`. */
static bool ws_hub_post_locked(ws_hub_t *hub, const int slot, ws_cmd_t *cmd)
{
    thread_mailbox_t *const inbox = hub->inbox[slot];

    return inbox != NULL && thread_mailbox_post(inbox, cmd);
}

static ws_cmd_t *ws_cmd_new(const ws_cmd_kind_t kind, ws_room_t *room)
{
    ws_cmd_t *const cmd = pecalloc(1, sizeof(*cmd), 1);
    cmd->kind = kind;
    cmd->room = room;

    if (room != NULL) {
        zend_atomic_int_fetch_add(&room->refcount, 1);
    }

    return cmd;
}

/* Drop a command that never made it into a mailbox, with `admin` held. The
 * caller holds a room reference of its own, so this drop can never be the last
 * one — ws_room_release would deadlock here, and never needs to run. */
static void ws_cmd_discard_locked(ws_cmd_t *cmd)
{
    if (cmd->room != NULL) {
        zend_atomic_int_fetch_add(&cmd->room->refcount, -1);
    }

    pefree(cmd, 1);
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

    int      slot = -1;
    uint32_t gen  = 0;

    tsrm_mutex_lock(hub->admin);
    for (int i = 0; i < WS_HUB_MAX_WORKERS; i++) {
        if (!hub->taken[i]) {
            hub->taken[i] = true;
            hub->inbox[i] = inbox;
            gen           = ++hub->gen[i];
            slot          = i;

            if (i >= hub->slots_used) {
                hub->slots_used = i + 1;
            }

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
    ws_local->gen   = gen;
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

    /* The slot is retired, so no producer can post any more. Whatever is still
     * queued holds room/payload/query references and thread_mailbox_free throws
     * the queue away without touching them — drain it first or every rotation of
     * the pool leaks. */
    thread_mailbox_drain_pending(ws_local->inbox);
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
        session->ws_id = ws_atomic_u64_next(&ws_local->hub->next_ws_id);
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

static uint32_t ws_local_deliver(ws_room_t *room, const char *data, const size_t len,
                                 const bool binary, const uint64_t except_id)
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

        if (ws_session_try_send(session, data, len, binary)) {
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

/* Runs on the worker that was asked. Answer with our own member count and send
 * it home, so the asker settles its query on its own thread. */
static void ws_hub_answer_count(ws_hub_t *hub, ws_cmd_t *cmd)
{
    ws_query_t *const query = cmd->query;

    cmd->query = NULL;   /* the reference travels on with the reply */

    ws_cmd_t *const reply = ws_cmd_new(WS_CMD_COUNT_REPLY, NULL);
    reply->query = query;
    reply->count = ws_local != NULL ? ws_local_count(cmd->room) : 0;

    tsrm_mutex_lock(hub->admin);

    /* A detached asker frees its slot for reuse; without the generation check
     * the reply would settle a stranger's query on the wrong thread. */
    const bool posted = query->gen == hub->gen[query->slot]
        && ws_hub_post_locked(hub, query->slot, reply);

    if (!posted) {
        ws_cmd_discard_locked(reply);
    }

    tsrm_mutex_unlock(hub->admin);

    if (!posted) {
        ws_hub_note_drop(hub);
        ws_query_release(query);
    }
}

/* Runs on the thread that started the query — see ws_query_t. */
static void ws_query_settle(ws_query_t *query, const uint32_t answered)
{
    if (!query->abandoned) {
        query->total += answered;

        if (--query->pending == 0) {
            async_plain_event_fire(query->done);
        }
    }

    ws_query_release(query);
}

static void ws_hub_drain(void **items, const size_t count, void *arg)
{
    ws_hub_t *const hub = arg;

    for (size_t i = 0; i < count; i++) {
        ws_cmd_t *const cmd = items[i];

        switch (cmd->kind) {
            case WS_CMD_BROADCAST:
                if (ws_local != NULL) {
                    (void) ws_local_deliver(cmd->room, cmd->payload->data,
                                            cmd->payload->len, cmd->payload->binary,
                                            cmd->except_id);
                }

                ws_payload_release(cmd->payload);
                break;

            case WS_CMD_COUNT:
                ws_hub_answer_count(hub, cmd);
                break;

            case WS_CMD_COUNT_REPLY:
                ws_query_settle(cmd->query, cmd->count);
                break;
        }

        ws_room_release(hub, cmd->room);

        pefree(cmd, 1);
    }
}

uint32_t ws_hub_broadcast(ws_hub_t *hub, ws_room_t *room,
                          const char *data, const size_t len, const bool binary,
                          const uint64_t except_id)
{
    if (hub == NULL || room == NULL) {
        return 0;
    }

    const uint32_t sent = ws_local != NULL
        ? ws_local_deliver(room, data, len, binary, except_id) : 0;

    tsrm_mutex_lock(hub->admin);

    ws_payload_t *payload = NULL;

    for (int slot = 0; slot < hub->slots_used; slot++) {
        if (hub->inbox[slot] == NULL || (ws_local != NULL && slot == ws_local->slot)) {
            continue;
        }

        /* Copied once and shared by refcount across the fan-out — and not copied
         * at all when this is the only worker. */
        if (payload == NULL) {
            payload = ws_payload_new(data, len, binary);
        }

        ws_cmd_t *const cmd = ws_cmd_new(WS_CMD_BROADCAST, room);
        cmd->except_id = except_id;
        cmd->payload   = payload;

        zend_atomic_int_fetch_add(&payload->refcount, 1);

        if (!ws_hub_post_locked(hub, slot, cmd)) {
            ws_payload_release(payload);
            ws_cmd_discard_locked(cmd);
            ws_hub_note_drop(hub);
        }
    }

    tsrm_mutex_unlock(hub->admin);

    if (payload != NULL) {
        ws_payload_release(payload);
    }

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

    /* In-thread event, not a cross-thread trigger: the replies come back through
     * our own mailbox, so nothing but this thread ever touches it. (An event's
     * ref_count is not atomic — a trigger shared with the answering workers could
     * not be disposed safely on a timeout.) */
    zend_async_event_t *const done = async_plain_event_new();

    if (done == NULL) {
        return local;
    }

    ws_query_t *const query = pecalloc(1, sizeof(*query), 1);
    ZEND_ATOMIC_INT_INIT(&query->refcount, 1);
    query->slot  = ws_local->slot;
    query->gen   = ws_local->gen;
    query->total = local;
    query->done  = done;

    tsrm_mutex_lock(hub->admin);

    for (int slot = 0; slot < hub->slots_used; slot++) {
        if (slot == ws_local->slot || hub->inbox[slot] == NULL) {
            continue;
        }

        ws_cmd_t *const cmd = ws_cmd_new(WS_CMD_COUNT, room);
        cmd->query = query;

        zend_atomic_int_fetch_add(&query->refcount, 1);

        if (ws_hub_post_locked(hub, slot, cmd)) {
            query->pending++;
        } else {
            zend_atomic_int_fetch_add(&query->refcount, -1);
            ws_cmd_discard_locked(cmd);
            ws_hub_note_drop(hub);
        }
    }

    tsrm_mutex_unlock(hub->admin);

    /* The waker owns its timeout timer. A worker that misses the deadline is
     * simply left out of the tally; only a genuine cancellation sets an
     * exception, which we let the caller see. */
    if (query->pending > 0
        && zend_async_waker_new_with_timeout(me, timeout_ms, NULL) != NULL) {
        zend_async_resume_when(me, done, false, zend_async_waker_callback_resolve, NULL);
        ZEND_ASYNC_SUSPEND();
        zend_async_waker_clean(me);
    }

    const uint32_t total = query->total;

    /* Anything still in flight settles into an abandoned query — on this same
     * thread, so disposing the event here cannot race an answer. */
    query->abandoned = true;
    query->done      = NULL;

    done->dispose(done);

    ws_query_release(query);

    return total;
}
