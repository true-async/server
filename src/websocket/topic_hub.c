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
#include "websocket/topic_hub.h"
#include "websocket/ws_session.h"
#include "websocket/ws_topic_tree.h"
#include "core/thread_mailbox.h"
#include "core/async_plain_event.h"

#include <TSRM.h>

#define TOPIC_HUB_MAILBOX_CAPACITY 4096
#define TOPIC_HUB_MAILBOX_BATCH      64

/* Interest filter (topic_hub.h). Power of two — the probe sequence masks. 1024
 * counters is 4KB per worker, and holds a few hundred distinct topic prefixes
 * before the false-positive rate is worth caring about. */
#define WS_INTEREST_BUCKETS 1024u
#define WS_INTEREST_PROBES     3u

struct topic_hub_s {
    /* Guards the slot table below — claiming a slot, retiring it, and posting
     * into one. There is no topic registry to guard: topics live in each
     * worker's own tree. */
    MUTEX_T admin;

    zend_atomic_int64 next_ws_id;
    zend_atomic_int64 posted;
    zend_atomic_int64 skipped;
    zend_atomic_int64 dropped;

    /* Reliable-send (topic_hub_send / topic_hub_try_send) counters. Kept apart
     * from `dropped`, which is best-effort loss only: a full mailbox on the
     * reliable path is parked here, never conflated with a drop. */
    zend_atomic_int64 retry_queued;      /* targets parked for retry */
    zend_atomic_int64 retry_delivered;   /* parked targets a retry landed */
    zend_atomic_int64 retry_expired;     /* parked targets dropped at deadline */
    zend_atomic_int64 retry_rejected;    /* sends refused — outbound queue at cap */
    zend_atomic_int64 retry_gone;        /* parked targets whose worker had detached */
    zend_atomic_int64 retry_shutdown;    /* parked targets abandoned by THIS worker's detach */
    zend_atomic_int64 sub_overflow;      /* server subscriber rings that dropped their oldest */

    thread_mailbox_t *inbox[TOPIC_HUB_MAX_WORKERS];
    /* Bumped on every attach. A worker that detaches frees its slot for reuse,
     * so a reply in flight must check the generation or it lands on the wrong
     * thread's query. */
    uint32_t          gen[TOPIC_HUB_MAX_WORKERS];
    bool              taken[TOPIC_HUB_MAX_WORKERS];
    int               slots_used;   /* highest slot ever claimed + 1 */

    /* One counting Bloom per worker, written only by the worker that owns the
     * slot and read by every publisher. Per-bucket atomics rather than a lock:
     * a half-applied update can cost a wasted wake-up but cannot hide a live
     * subscription, which is the only error that would matter. */
    zend_atomic_int  *interest[TOPIC_HUB_MAX_WORKERS];

    /* One for the owner, one per attached worker. A worker can still be in the
     * epilogue of its start() when the owning HttpServer object is freed, so
     * whichever of them leaves last frees the hub. Without this the worker locks
     * hub->admin in a block that has already been returned to the allocator. */
    zend_atomic_int   refcount;
};

/* One copy shared by refcount across the whole fan-out, rather than one per
 * worker. The topic rides inline in each command instead — it is short. */
typedef struct ws_payload {
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
    WS_CMD_PUBLISH,
    WS_CMD_COUNT,
    WS_CMD_COUNT_REPLY,
} ws_cmd_kind_t;

typedef struct {
    ws_cmd_kind_t kind;
    ws_payload_t *payload;     /* PUBLISH — holds a reference */
    ws_query_t   *query;       /* COUNT, COUNT_REPLY — holds a reference */
    uint32_t      count;       /* COUNT_REPLY */
    uint64_t      except_id;   /* PUBLISH */
    size_t        topic_len;   /* PUBLISH, COUNT */
    char          topic[1];
} ws_cmd_t;

/* One still-unfilled destination in a parked reliable send: the target worker's
 * slot plus the generation it was claimed with. The generation detects slot
 * REUSE — a fresh attach on the same slot bumps it, so a retry on a stale
 * generation would mis-deliver and is dropped (retry_gone). A plain detach nulls
 * the inbox WITHOUT bumping gen, so the drain checks both (see topic_hub_retry_drain). */
typedef struct {
    int      slot;
    uint32_t gen;
} retry_target_t;

/* One parked reliable-send message on a worker's outbound queue. Ownership is
 * cloned from ws_query_t (same early-resume / cancel hazards): a blocking send()
 * holds `refcount` across ZEND_ASYNC_SUSPEND and reads delivered/npending from
 * this struct AFTER resume, so the drainer must not free it out from under the
 * sleeper. The queue holds the other reference; the last release frees.
 *
 * The topic rides inline because each retry rebuilds a fresh ws_cmd_t (the topic
 * is matched against the target worker's own tree, so it must travel with every
 * post). `payload` is the shared, refcounted body — the entry owns exactly ONE
 * payload reference, released once at the terminal step. */
typedef struct retry_entry_s {
    struct retry_entry_s *next;

    zend_atomic_int       refcount;    /* caller (send) holds 1 across the park; queue holds 1 */
    bool                  abandoned;   /* set by whoever resumes/cancels first; the other side skips the fire */
    bool                  shutdown;    /* set by detach teardown so a woken send() reports SHUTDOWN, not a false timeout */

    ws_payload_t         *payload;     /* shared body — the entry owns one ref */
    retry_target_t       *pending;     /* npending still-unfilled targets, compacted in place */
    uint32_t              npending;

    uint32_t              delivered;   /* targets a retry has landed on */
    uint32_t              gone;         /* targets resolved as detached (mis-delivery avoided) */

    uint64_t              except_id;   /* the publish's origin, excluded on the far tree */
    uint64_t              deadline_ms; /* ZEND_ASYNC_NOW() ms; the DRAINER owns this deadline */
    zend_async_event_t   *waiter;      /* non-NULL only for a blocking send(); the caller disposes it */

    size_t                topic_len;
    char                  topic[1];    /* inline — rebuilt into each retry's ws_cmd_t */
} retry_entry_t;

/* This thread's attachment to ONE hub. */
typedef struct ws_local_s {
    struct ws_local_s *next;
    topic_hub_t          *hub;
    int                slot;
    uint32_t           gen;
    thread_mailbox_t  *inbox;
    ws_topic_tree_t   *tree;

    /* Server subscriptions held on this thread; detach closes whatever is left. */
    struct ws_server_sub *subs;

    /* Reliable-send outbound queue — thread-local, so NO lock: enqueued by this
     * worker's coroutines, drained by this worker's timer, torn down by this
     * worker's detach, all one thread. The bound (`retry_count` vs queue_max)
     * counts ENTRIES: one entry is one message, parked atomically. */
    retry_entry_t     *retry_head;
    retry_entry_t     *retry_tail;
    uint32_t           retry_count;
    uint32_t           retry_interval_ms;   /* drainer cadence, fixed at the first arm */

    /* ONE reused MULTISHOT timer (the deadline_tick model), created lazily on the
     * first enqueue and kept for the worker's life. A one-shot would have to be
     * re-created every tick, and the engine does NOT free a fired one-shot's event
     * (on_timer_event only stops it) — re-arming would leak a timer handle per
     * tick. So we START this one when the queue goes non-empty, STOP it (keeping the
     * handle) when the queue drains, and dispose it exactly once at detach. `armed`
     * tracks whether it is currently running. */
    zend_async_event_t          *retry_timer;
    zend_async_event_callback_t *retry_timer_cb;
    bool                         retry_timer_armed;
} ws_local_t;

/* The drainer timer's callback carries the hub so the tick can re-resolve this
 * thread's ws_local_t (via ws_local_of) — never a raw ws_local_t*, which detach
 * may have freed. */
typedef struct {
    zend_async_event_callback_t base;
    topic_hub_t                *hub;
} retry_cb_t;

/* Keyed by hub, not one-per-thread: a topic tree is per-SERVER state, and
 * CODING_STANDARDS §1.2 keeps per-server state out of thread-globals. One
 * element in every supported setup (§1.1 — one running server per thread), so
 * the walk is free. */
ZEND_TLS ws_local_t *ws_locals = NULL;

static ws_local_t *ws_local_of(const topic_hub_t *hub)
{
    for (ws_local_t *local = ws_locals; local != NULL; local = local->next) {
        if (local->hub == hub) {
            return local;
        }
    }

    return NULL;
}

/* zend_atomic has fetch_add for int only. Returns the value before the add. */
static uint64_t ws_atomic_u64_add(zend_atomic_int64 *counter, const int64_t delta)
{
    int64_t cur = zend_atomic_int64_load(counter);

    while (!zend_atomic_int64_compare_exchange(counter, &cur, cur + delta)) {
        /* cur was refreshed by the failed exchange */
    }

    return (uint64_t) cur;
}

static void topic_hub_note_drop(topic_hub_t *hub)
{
    (void) ws_atomic_u64_add(&hub->dropped, 1);
}

void topic_hub_get_stats(topic_hub_t *hub, topic_hub_stats_t *out)
{
    if (hub == NULL) {
        memset(out, 0, sizeof(*out));
        return;
    }

    out->posted  = (uint64_t) zend_atomic_int64_load(&hub->posted);
    out->skipped = (uint64_t) zend_atomic_int64_load(&hub->skipped);
    out->dropped = (uint64_t) zend_atomic_int64_load(&hub->dropped);

    out->retry_queued    = (uint64_t) zend_atomic_int64_load(&hub->retry_queued);
    out->retry_delivered = (uint64_t) zend_atomic_int64_load(&hub->retry_delivered);
    out->retry_expired   = (uint64_t) zend_atomic_int64_load(&hub->retry_expired);
    out->retry_rejected  = (uint64_t) zend_atomic_int64_load(&hub->retry_rejected);
    out->retry_gone      = (uint64_t) zend_atomic_int64_load(&hub->retry_gone);
    out->retry_shutdown  = (uint64_t) zend_atomic_int64_load(&hub->retry_shutdown);
    out->sub_overflow    = (uint64_t) zend_atomic_int64_load(&hub->sub_overflow);
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

static ws_cmd_t *ws_cmd_new(const ws_cmd_kind_t kind, const char *topic,
                            const size_t topic_len)
{
    ws_cmd_t *const cmd = pecalloc(1, sizeof(*cmd) + topic_len, 1);
    cmd->kind      = kind;
    cmd->topic_len = topic_len;

    if (topic_len != 0) {
        memcpy(cmd->topic, topic, topic_len);
    }

    return cmd;
}

/* ------------------------------------------------------------------- hub */

topic_hub_t *topic_hub_create(void)
{
    topic_hub_t *const hub = pecalloc(1, sizeof(*hub), 1);

    ZEND_ATOMIC_INT64_INIT(&hub->next_ws_id, 1);
    ZEND_ATOMIC_INT64_INIT(&hub->posted, 0);
    ZEND_ATOMIC_INT64_INIT(&hub->skipped, 0);
    ZEND_ATOMIC_INT64_INIT(&hub->dropped, 0);
    ZEND_ATOMIC_INT64_INIT(&hub->retry_queued, 0);
    ZEND_ATOMIC_INT64_INIT(&hub->retry_delivered, 0);
    ZEND_ATOMIC_INT64_INIT(&hub->retry_expired, 0);
    ZEND_ATOMIC_INT64_INIT(&hub->retry_rejected, 0);
    ZEND_ATOMIC_INT64_INIT(&hub->retry_gone, 0);
    ZEND_ATOMIC_INT64_INIT(&hub->retry_shutdown, 0);
    ZEND_ATOMIC_INT64_INIT(&hub->sub_overflow, 0);

    hub->admin = tsrm_mutex_alloc();
    ZEND_ATOMIC_INT_INIT(&hub->refcount, 1);

    return hub;
}

void topic_hub_addref(topic_hub_t *hub)
{
    if (hub == NULL) {
        return;
    }

    zend_atomic_int_fetch_add(&hub->refcount, 1);
}

void topic_hub_release(topic_hub_t *hub)
{
    if (hub == NULL) {
        return;
    }

    /* fetch_add returns the value BEFORE the add, so 1 is the last reference. */
    if (zend_atomic_int_fetch_add(&hub->refcount, -1) == 1) {
        tsrm_mutex_free(hub->admin);
        pefree(hub, 1);
    }
}

uint64_t topic_hub_next_id(topic_hub_t *hub)
{
    return ws_atomic_u64_add(&hub->next_ws_id, 1);
}

ws_topic_tree_t *topic_hub_tree(const topic_hub_t *hub)
{
    const ws_local_t *const local = hub != NULL ? ws_local_of(hub) : NULL;

    return local != NULL ? local->tree : NULL;
}

/* -------------------------------------------------------------- interest */

/* Two independent hashes, then Kirsch-Mitzenmacher: the i-th probe is
 * h1 + i*h2. h2 is forced odd so the sequence cannot get stuck on one bucket. */
static void ws_interest_probe(const char *key, const size_t len,
                              uint32_t bucket[WS_INTEREST_PROBES])
{
    const uint32_t h1 = (uint32_t) zend_inline_hash_func(key, len);

    uint32_t h2 = 2166136261u;   /* FNV-1a */

    for (size_t i = 0; i < len; i++) {
        h2 = (h2 ^ (uint32_t)(unsigned char) key[i]) * 16777619u;
    }

    h2 |= 1u;

    for (uint32_t i = 0; i < WS_INTEREST_PROBES; i++) {
        bucket[i] = (h1 + i * h2) & (WS_INTEREST_BUCKETS - 1u);
    }
}

/* A no-op once the slot is retired: detach nulls hub->interest[slot] before
 * draining, and that drain can tear a session down and unsubscribe it. */
static void ws_interest_bump(topic_hub_t *hub, const char *filter,
                             const size_t prefix_len, const int delta)
{
    const ws_local_t *const local = ws_local_of(hub);

    if (local == NULL || hub->interest[local->slot] == NULL) {
        return;
    }

    zend_atomic_int *const counters = hub->interest[local->slot];

    uint32_t bucket[WS_INTEREST_PROBES];
    ws_interest_probe(filter, prefix_len, bucket);

    for (uint32_t i = 0; i < WS_INTEREST_PROBES; i++) {
        zend_atomic_int_fetch_add(&counters[bucket[i]], delta);
    }
}

void topic_hub_interest_add(topic_hub_t *hub, const char *filter, const size_t prefix_len)
{
    ws_interest_bump(hub, filter, prefix_len, 1);
}

void topic_hub_interest_remove(topic_hub_t *hub, const char *filter, const size_t prefix_len)
{
    ws_interest_bump(hub, filter, prefix_len, -1);
}

/* Every level-prefix of the topic being published, hashed once for the whole
 * fan-out rather than once per worker. */
typedef struct {
    uint32_t bucket[WS_TOPIC_MAX_PREFIXES][WS_INTEREST_PROBES];
    uint32_t count;    /* 0 = no probe; treat every worker as interested */
} ws_interest_t;

static void ws_interest_build(ws_interest_t *interest, const char *topic,
                              const size_t topic_len)
{
    ws_topic_prefixes_t prefixes;

    if (!ws_topic_prefixes(topic, topic_len, &prefixes)) {
        interest->count = 0;
        return;
    }

    for (uint32_t i = 0; i < prefixes.count; i++) {
        ws_interest_probe(topic, prefixes.len[i], interest->bucket[i]);
    }

    interest->count = prefixes.count;
}

/* Called under `admin`, which is also what keeps the filter from being freed
 * under us by a concurrent detach. */
static bool ws_interest_matches(const topic_hub_t *hub, const int slot,
                                const ws_interest_t *interest)
{
    zend_atomic_int *const counters = hub->interest[slot];

    /* A worker with no filter yet, or a topic we could not split, is sent the
     * message: a false positive wastes a wake-up, a false negative loses it. */
    if (counters == NULL || interest->count == 0) {
        return true;
    }

    for (uint32_t p = 0; p < interest->count; p++) {
        bool hit = true;

        for (uint32_t i = 0; i < WS_INTEREST_PROBES && hit; i++) {
            hit = zend_atomic_int_load(&counters[interest->bucket[p][i]]) != 0;
        }

        if (hit) {
            return true;
        }
    }

    return false;
}

#ifdef TAS_TEST_HOOKS
/* Retry-path fault injection: when positive, the next N cross-worker posts fail as
 * if the mailbox were full, forcing the park->retry path a test cannot provoke
 * without a real backed-up consumer. Read and decremented under hub->admin (every
 * post runs through topic_hub_post_locked holding the lock); the setter below writes
 * it from the test thread before any send is triggered, and the admin mutex around
 * the fan-out publishes that write — so reader and writer never overlap. */
static int tas_test_force_full_posts = 0;
#endif

/* The mailbox is freed by its own worker on detach, and thread_mailbox's
 * contract is "free after producers have quiesced" — so claiming a slot,
 * retiring it and posting into it all happen under `admin`. */
static bool topic_hub_post_locked(topic_hub_t *hub, const int slot, ws_cmd_t *cmd)
{
#ifdef TAS_TEST_HOOKS
    if (UNEXPECTED(tas_test_force_full_posts > 0)) {
        tas_test_force_full_posts--;
        return false;   /* simulate a full target mailbox for this post */
    }
#endif

    thread_mailbox_t *const inbox = hub->inbox[slot];

    return inbox != NULL && thread_mailbox_post(inbox, cmd);
}

static void topic_hub_drain(void **items, const size_t count, void *arg);

/* ------------------------------------------------------ server subscribers
 *
 * A subscriber that queues the payload the tree hands it and wakes the coroutine
 * parked in recv(). Thread-local like the rest of ws_local_t: created, drained
 * and closed on one thread.
 *
 * The ring is bounded and overflow drops the OLDEST entry, because for a control
 * room the newest command is the authoritative one. */

#define WS_SERVER_SUB_RING 64

struct ws_server_sub {
    struct ws_server_sub *next;
    ws_local_t           *local;
    zend_string          *filter;    /* owned */
    uint64_t              mark;
    bool                  parked;    /* a recv holds this subscription; a second one is refused */

    ws_payload_t         *ring[WS_SERVER_SUB_RING];
    uint32_t              head;
    uint32_t              size;
    uint64_t              lost;      /* monotonic, never reset */

    zend_async_event_t   *waiter;    /* non-NULL only while a recv is parked */
    bool                  closed;
    uint32_t              refcount;  /* the tree's side and the caller's side */
};

uint64_t ws_server_sub_mark(const ws_server_sub_t *sub)
{
    return sub->mark;
}

void ws_server_sub_set_mark(ws_server_sub_t *sub, const uint64_t mark)
{
    sub->mark = mark;
}

bool ws_server_sub_try_deliver(ws_server_sub_t *sub, const char *data,
                               const size_t len, const bool binary)
{
    if (sub->closed) {
        return false;
    }

    ws_payload_t *const payload = ws_payload_new(data, len, binary);

    if (payload == NULL) {
        return false;
    }

    if (sub->size == WS_SERVER_SUB_RING) {
        ws_payload_release(sub->ring[sub->head]);
        sub->head = (sub->head + 1) % WS_SERVER_SUB_RING;
        sub->size--;
        sub->lost++;

        if (sub->local != NULL) {
            (void) ws_atomic_u64_add(&sub->local->hub->sub_overflow, 1);
        }
    }

    sub->ring[(sub->head + sub->size) % WS_SERVER_SUB_RING] = payload;
    sub->size++;

    /* Runs no PHP: the woken coroutine resumes after the tree walk unwinds. */
    if (sub->waiter != NULL) {
        async_plain_event_fire(sub->waiter);
    }

    return true;
}

static void ws_server_sub_addref(ws_server_sub_t *sub)
{
    sub->refcount++;
}

/* The ring holds persistent payload references; the subscription itself is
 * request memory. A reference nobody will ever drop therefore costs a payload
 * leak rather than a struct leak, which is why the end-of-request path empties
 * the ring by hand instead of waiting for the last release. */
static void ws_server_sub_drop_ring(ws_server_sub_t *sub)
{
    while (sub->size > 0) {
        ws_payload_release(sub->ring[sub->head]);
        sub->head = (sub->head + 1) % WS_SERVER_SUB_RING;
        sub->size--;
    }
}

static void ws_server_sub_release(ws_server_sub_t *sub)
{
    if (--sub->refcount > 0) {
        return;
    }

    ws_server_sub_drop_ring(sub);

    if (sub->filter != NULL) {
        zend_string_release(sub->filter);
    }

    efree(sub);
}

/* Reliable-send teardown, defined with the rest of the outbound-queue machinery
 * at the foot of the file; detach (above them) needs it. */
static void topic_hub_retry_teardown(topic_hub_t *hub, ws_local_t *local, bool request_over);

int topic_hub_attach(topic_hub_t *hub)
{
    if (hub == NULL || ws_local_of(hub) != NULL) {
        return -1;
    }

    thread_mailbox_t *const inbox = thread_mailbox_create(
        TOPIC_HUB_MAILBOX_CAPACITY, TOPIC_HUB_MAILBOX_BATCH, topic_hub_drain, hub);

    if (inbox == NULL) {
        return -1;
    }

    zend_atomic_int *const counters =
        pecalloc(WS_INTEREST_BUCKETS, sizeof(*counters), 1);

    for (uint32_t i = 0; i < WS_INTEREST_BUCKETS; i++) {
        ZEND_ATOMIC_INT_INIT(&counters[i], 0);
    }

    int      slot = -1;
    uint32_t gen  = 0;

    /* The filter is published with the mailbox, so a publisher that can see the
     * slot can already see (an empty) interest for it. */
    tsrm_mutex_lock(hub->admin);
    for (int i = 0; i < TOPIC_HUB_MAX_WORKERS; i++) {
        if (!hub->taken[i]) {
            hub->taken[i]    = true;
            hub->inbox[i]    = inbox;
            hub->interest[i] = counters;
            gen              = ++hub->gen[i];
            slot             = i;

            if (i >= hub->slots_used) {
                hub->slots_used = i + 1;
            }

            break;
        }
    }
    tsrm_mutex_unlock(hub->admin);

    if (slot < 0) {
        thread_mailbox_free(inbox);
        pefree(counters, 1);
        return -1;
    }

    /* Paired with the drop in topic_hub_detach: the slot outlives this thread's
     * server object on the failure paths out of start(), so it holds its own. */
    zend_atomic_int_fetch_add(&hub->refcount, 1);

    ws_local_t *const local = ecalloc(1, sizeof(*local));
    local->hub   = hub;
    local->slot  = slot;
    local->gen   = gen;
    local->inbox = inbox;
    local->tree  = ws_topic_tree_create(hub);
    local->next  = ws_locals;
    ws_locals    = local;

    return slot;
}

static void topic_hub_detach_ex(topic_hub_t *hub, bool request_over);

void topic_hub_thread_sweep(void)
{
    while (ws_locals != NULL) {
        topic_hub_detach_ex(ws_locals->hub, /*request_over*/ true);
    }
}

/* Attaches once per thread; the shutdown paths are the only detach. */
static ws_local_t *topic_hub_attach_ensure(topic_hub_t *hub)
{
    if (hub == NULL) {
        return NULL;
    }

    ws_local_t *local = ws_local_of(hub);

    if (local != NULL) {
        return local;
    }

    return topic_hub_attach(hub) >= 0 ? ws_local_of(hub) : NULL;
}

ws_server_sub_t *topic_hub_subscribe(topic_hub_t *hub, zend_string *filter)
{
    ws_local_t *const local = topic_hub_attach_ensure(hub);

    if (local == NULL) {
        return NULL;
    }

    ws_server_sub_t *const sub = ecalloc(1, sizeof(*sub));
    sub->local    = local;
    sub->filter   = zend_string_copy(filter);
    sub->refcount = 1;   /* the tree's side; the caller takes its own below */

    if (!ws_topic_subscribe_server(local->tree, sub, filter)) {
        ws_server_sub_release(sub);
        return NULL;
    }

    ws_server_sub_addref(sub);   /* the caller's side */

    sub->next   = local->subs;
    local->subs = sub;

    return sub;
}

/* The thread's attachment stays: a coroutine parked in send() holds no
 * subscription, and detaching under it would wake it with a spurious shutdown. */
void topic_hub_unsubscribe(ws_server_sub_t *sub)
{
    if (sub == NULL || sub->closed) {
        return;
    }

    ws_local_t *const local = sub->local;

    sub->closed = true;

    if (local != NULL) {
        ws_topic_unsubscribe_server(local->tree, sub, sub->filter);

        for (ws_server_sub_t **it = &local->subs; *it != NULL; it = &(*it)->next) {
            if (*it == sub) {
                *it = sub->next;
                break;
            }
        }
    }

    if (sub->waiter != NULL) {
        async_plain_event_fire(sub->waiter);   /* a parked recv wakes to a closed sub */
    }

    ws_server_sub_release(sub);
}

void topic_hub_sub_release(ws_server_sub_t *sub)
{
    if (sub != NULL) {
        ws_server_sub_release(sub);
    }
}

uint64_t topic_hub_sub_lost(const ws_server_sub_t *sub)
{
    return sub != NULL ? sub->lost : 0;
}

static ws_payload_t *ws_server_sub_pop(ws_server_sub_t *sub)
{
    if (sub->size == 0) {
        return NULL;
    }

    ws_payload_t *const payload = sub->ring[sub->head];
    sub->head = (sub->head + 1) % WS_SERVER_SUB_RING;
    sub->size--;

    return payload;
}

/* Keep this thread's reactor alive while a recv() is parked on it.
 *
 * The wake source is the inbox trigger, and a mailbox does not hold the loop by
 * default — it wakes a loop other handles keep running. A thread whose only
 * coroutine waits on a room therefore has no live handle at all: the reactor
 * reports no work, and the scheduler cancels every waiter as a deadlock. Timed
 * receivers escape only because their timeout waker arms a timer.
 *
 * Nesting belongs to the reactor: start()/stop() count the event's loop_ref_count
 * and only the 0<->1 edge touches uv_ref, so two parked receivers hold the loop
 * until the second one leaves. The ref lasts exactly as long as a park, and an
 * idle thread stays mortal.
 *
 * NULL on either side means the attachment is being torn down — topic_hub_detach
 * frees the mailbox and clears both fields before it wakes the subscriptions it
 * still holds, and a receiver resuming into that has no loop left to keep. */
static void ws_local_recv_keepalive(ws_local_t *local, const bool parked)
{
    if (local != NULL && local->inbox != NULL) {
        thread_mailbox_keepalive(local->inbox, parked);
    }
}

topic_hub_recv_status_t topic_hub_recv(ws_server_sub_t *sub, const int64_t timeout_ms,
                                       ws_payload_t **out)
{
    *out = NULL;

    if (sub == NULL || sub->closed) {
        return TOPIC_HUB_RECV_CLOSED;
    }

    /* Whoever is parked owns this subscription, even with a message sitting in
     * the ring: an empty-ring test would hand that message to a second caller. */
    if (sub->parked) {
        return TOPIC_HUB_RECV_BUSY;
    }

    *out = ws_server_sub_pop(sub);

    if (*out != NULL) {
        return TOPIC_HUB_RECV_MESSAGE;
    }

    zend_coroutine_t *const coroutine = ZEND_ASYNC_CURRENT_COROUTINE;

    /* 0 is "take what is there", which an expired computed deadline also spells;
     * outside a coroutine there is nothing to park on either way. */
    if (timeout_ms == 0 || coroutine == NULL || ZEND_ASYNC_IS_SCHEDULER_CONTEXT) {
        return TOPIC_HUB_RECV_TIMEOUT;
    }

    /* In-thread event, like count()'s: only this thread's tree walk fires it. */
    zend_async_event_t *const waiter = async_plain_event_new();

    if (waiter == NULL) {
        return TOPIC_HUB_RECV_TIMEOUT;
    }

    /* Held across the suspend: unsubscribe() and the caller may both drop their
     * references while this coroutine is parked. */
    ws_server_sub_addref(sub);

    sub->waiter = waiter;
    sub->parked = true;

    /* The engine spells "no timer" as 0, which is what a negative timeout means
     * here: wait until a message or a close. */
    const zend_ulong waker_timeout = timeout_ms < 0 ? 0 : (zend_ulong) timeout_ms;

    if (zend_async_waker_new_with_timeout(coroutine, waker_timeout, NULL) != NULL) {
        ws_local_recv_keepalive(sub->local, true);
        zend_async_resume_when(coroutine, waiter, false, zend_async_waker_callback_resolve, NULL);

        /* zend_try, because a fatal error longjmps straight past the release: the
         * trigger would stay ref'd, the thread would reach shutdown with a live
         * handle, and the scheduler asserts on that ("The event loop must be
         * stopped"). Same trap the deadline timer hit in HttpServer::start(). */
        volatile bool bailout = false;

        zend_try {
            ZEND_ASYNC_SUSPEND();
        } zend_catch {
            bailout = true;
        } zend_end_try();

        /* Released here and not after the branches below, so a cancellation —
         * which resumes us with an exception pending — drops the ref too. The
         * attachment is re-read because a sweep can have cleared it under us. */
        ws_local_recv_keepalive(sub->local, false);

        if (UNEXPECTED(bailout)) {
            zend_bailout();
        }

        zend_async_waker_clean(coroutine);
    }

    sub->waiter = NULL;
    sub->parked = false;
    waiter->dispose(waiter);

    topic_hub_recv_status_t status;

    if (sub->closed) {
        status = TOPIC_HUB_RECV_CLOSED;
    } else if (EG(exception) != NULL) {
        /* Cancelled across the park: the message stays in the ring. This
         * coroutine is unwinding and cannot use it; the next reader can. */
        status = TOPIC_HUB_RECV_TIMEOUT;
    } else {
        *out   = ws_server_sub_pop(sub);
        status = *out != NULL ? TOPIC_HUB_RECV_MESSAGE : TOPIC_HUB_RECV_TIMEOUT;
    }

    /* Last, because releasing the final reference frees the ring `*out` came
     * from — the payload itself carries its own reference. */
    ws_server_sub_release(sub);

    return status;
}

const char *topic_hub_payload_data(const ws_payload_t *payload, size_t *len, bool *binary)
{
    *len    = payload->len;
    *binary = payload->binary;

    return payload->data;
}

void topic_hub_payload_release(ws_payload_t *payload)
{
    ws_payload_release(payload);
}

static void ws_local_unlink(ws_local_t *prev, const ws_local_t *local)
{
    if (prev != NULL) {
        prev->next = local->next;
    } else {
        ws_locals = local->next;
    }
}

/* `request_over` says the request that owns this attachment can no longer run
 * PHP — the end-of-request sweep, and after a fatal error the coroutines that
 * were parked here are dead while their waiter events are still request memory.
 * Waking one then calls back into the dead, so this teardown wakes nobody and
 * drops by hand what the absent owners would have released. A detach from a
 * live request is the opposite: stop() must wake a parked send() and a parked
 * recv(), or they hang on a queue and a tree that are about to vanish. */
static void topic_hub_detach_ex(topic_hub_t *hub, const bool request_over)
{
    ws_local_t *local = ws_locals;
    ws_local_t *prev  = NULL;

    while (local != NULL && local->hub != hub) {
        prev  = local;
        local = local->next;
    }

    if (local == NULL) {
        return;
    }

    tsrm_mutex_lock(hub->admin);
    zend_atomic_int *const counters = hub->interest[local->slot];
    hub->inbox[local->slot]         = NULL;
    hub->interest[local->slot]      = NULL;
    hub->taken[local->slot]         = false;
    tsrm_mutex_unlock(hub->admin);

    /* Retired under the lock, so no publisher is still reading it. The drain
     * below can tear a session down and unsubscribe it; hub->interest[slot] is
     * NULL now, so those decrements land nowhere — which is what we want. */
    pefree(counters, 1);

    /* A dead request services nothing: unlink first, so the drain below discards
     * what is queued instead of publishing it into a tree whose sessions the
     * request teardown may already have taken apart. */
    if (request_over) {
        ws_local_unlink(prev, local);
    }

    /* Tear the outbound retry queue down BEFORE the tree/attachment go: it stops
     * the drainer timer (so no tick starts mid-teardown) and wakes every parked
     * send() with a shutdown outcome, so a blocking caller throws rather than
     * hanging on a queue that is about to vanish. Runs on this same thread, so a
     * woken coroutine resumes only after we return — and its entry refcount keeps
     * the struct alive until it does. */
    topic_hub_retry_teardown(hub, local, request_over);

    /* The slot is retired, so no producer can post any more. Whatever is still
     * queued holds payload/query references and thread_mailbox_free throws the
     * queue away without touching them — drain it first or every rotation of the
     * pool leaks. While the request lives the attachment stays on the list across
     * the drain: it is what the drain resolves the tree through. */
    thread_mailbox_drain_pending(local->inbox);
    thread_mailbox_free(local->inbox);
    local->inbox = NULL;   /* a receiver woken below must not keep a freed loop handle */

    if (!request_over) {
        ws_local_unlink(prev, local);
    }

    /* Before the tree goes: a subscription outliving its nodes would unsubscribe
     * into freed memory. */
    while (local->subs != NULL) {
        ws_server_sub_t *const sub = local->subs;

        local->subs = sub->next;
        sub->local  = NULL;   /* the tree is going; nothing left to detach from */

        if (request_over) {
            /* No wake-up, and the ring emptied here: the receiver parked on this
             * subscription is gone holding a reference nobody will drop, and the
             * payloads it queued are persistent. */
            sub->closed = true;
            sub->waiter = NULL;
            ws_server_sub_drop_ring(sub);
            ws_server_sub_release(sub);   /* the tree's side */
        } else {
            topic_hub_unsubscribe(sub);
        }
    }

    ws_topic_tree_free(local->tree);

    efree(local);

    /* Last touch of the hub — after this the owner may already be gone and the
     * block may be freed here. */
    topic_hub_release(hub);
}

void topic_hub_detach(topic_hub_t *hub)
{
    topic_hub_detach_ex(hub, /*request_over*/ false);
}

void topic_hub_detach_request_over(topic_hub_t *hub)
{
    topic_hub_detach_ex(hub, /*request_over*/ true);
}

/* ----------------------------------------------------------------- query */

/* Runs on the asked worker. The answer goes home rather than being applied here,
 * so the asker settles its query on its own thread. */
static void topic_hub_answer_count(topic_hub_t *hub, ws_cmd_t *cmd)
{
    ws_query_t *const query = cmd->query;

    cmd->query = NULL;   /* the reference travels on with the reply */

    const ws_local_t *const local = ws_local_of(hub);

    ws_cmd_t *const reply = ws_cmd_new(WS_CMD_COUNT_REPLY, NULL, 0);
    reply->query = query;
    reply->count = local != NULL
        ? ws_topic_count(local->tree, cmd->topic, cmd->topic_len) : 0;

    tsrm_mutex_lock(hub->admin);

    const bool posted = query->gen == hub->gen[query->slot]
        && topic_hub_post_locked(hub, query->slot, reply);

    if (!posted) {
        pefree(reply, 1);
    }

    tsrm_mutex_unlock(hub->admin);

    if (!posted) {
        topic_hub_note_drop(hub);
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

/* A reply that comes home to a request which is over. The asker cannot be woken —
 * it is gone, and its event is request memory — and the reference it holds across
 * its park will never be dropped by it, so this drops it once. `abandoned` is the
 * handshake that says it already has been, whether by the asker settling normally
 * or by an earlier reply arriving here. */
static void ws_query_orphan(ws_query_t *query)
{
    if (!query->abandoned) {
        query->abandoned = true;
        query->done      = NULL;

        ws_query_release(query);   /* the asker's side */
    }

    ws_query_release(query);       /* this reply's side */
}

static void topic_hub_drain(void **items, const size_t count, void *arg)
{
    topic_hub_t *const hub = arg;

    const ws_local_t *const local = ws_local_of(hub);

    for (size_t i = 0; i < count; i++) {
        ws_cmd_t *const cmd = items[i];

        switch (cmd->kind) {
            case WS_CMD_PUBLISH:
                if (local != NULL) {
                    (void) ws_topic_publish(local->tree, cmd->topic, cmd->topic_len,
                                            cmd->payload->data, cmd->payload->len,
                                            cmd->payload->binary, cmd->except_id);
                }

                ws_payload_release(cmd->payload);
                break;

            case WS_CMD_COUNT:
                topic_hub_answer_count(hub, cmd);
                break;

            case WS_CMD_COUNT_REPLY:
                /* No attachment means this drain runs for a request that is over:
                 * the end-of-request teardown unlinks before draining, exactly so
                 * that what is queued is disposed of rather than delivered. */
                if (local != NULL) {
                    ws_query_settle(cmd->query, cmd->count);
                } else {
                    ws_query_orphan(cmd->query);
                }

                break;
        }

        pefree(cmd, 1);
    }
}

/* --------------------------------------------------------------- publish */

/* The shared locked slot walk behind publish() AND the reliable sends. There is
 * exactly one copy of the fan-out; the only thing that varies is what a FULL
 * mailbox means:
 *
 *   full != NULL (reliable send): the full target is appended to full[] as a
 *     {slot,gen} pair to park for retry — nothing is dropped;
 *   full == NULL (best-effort publish): the full target drops the copy and is
 *     tallied in *dropped_out.
 *
 * The payload is built lazily on the first real post (through *payload_io), so a
 * fan-out that interest-skips every other worker allocates nothing. Every
 * successful post takes a fresh +1 on the payload because the consumer's drain
 * unconditionally releases it (topic_hub_drain, WS_CMD_PUBLISH) — the
 * +1-before-post / -1-on-fail discipline. The generation is read HERE, under
 * admin, so a parked target can later tell a live worker from a reused slot.
 *
 * Caller MUST hold hub->admin. Returns mailboxes that accepted the copy. */
static uint32_t topic_hub_fanout_locked(topic_hub_t *hub, const ws_local_t *local,
        const char *topic, const size_t topic_len,
        const char *data, const size_t len, const bool binary, const uint64_t except_id,
        const ws_interest_t *interest, ws_payload_t **payload_io,
        retry_target_t *full, uint32_t *nfull,
        uint64_t *skipped_out, uint64_t *dropped_out)
{
    ws_payload_t *payload = *payload_io;
    uint32_t      posted  = 0;
    uint64_t      skipped = 0;
    uint64_t      dropped = 0;

    for (int slot = 0; slot < hub->slots_used; slot++) {
        if (hub->inbox[slot] == NULL || (local != NULL && slot == local->slot)) {
            continue;
        }

        if (!ws_interest_matches(hub, slot, interest)) {
            skipped++;
            continue;
        }

        if (payload == NULL) {
            payload = ws_payload_new(data, len, binary);
        }

        ws_cmd_t *const cmd = ws_cmd_new(WS_CMD_PUBLISH, topic, topic_len);
        cmd->payload   = payload;
        cmd->except_id = except_id;

        zend_atomic_int_fetch_add(&payload->refcount, 1);

        if (topic_hub_post_locked(hub, slot, cmd)) {
            posted++;
        } else {
            ws_payload_release(payload);
            pefree(cmd, 1);

            if (full != NULL) {
                full[*nfull].slot = slot;
                full[*nfull].gen  = hub->gen[slot];
                (*nfull)++;
            } else {
                dropped++;
            }
        }
    }

    *payload_io  = payload;
    *skipped_out = skipped;
    *dropped_out = dropped;
    return posted;
}

uint32_t topic_hub_publish(topic_hub_t *hub, const char *topic, const size_t topic_len,
                        const char *data, const size_t len, const bool binary,
                        const uint64_t except_id,
                        uint64_t *posted_out, uint64_t *dropped_out)
{
    if (hub == NULL) {
        if (posted_out  != NULL) *posted_out  = 0;
        if (dropped_out != NULL) *dropped_out = 0;
        return 0;
    }

    const ws_local_t *const local = ws_local_of(hub);

    const uint32_t sent = local != NULL
        ? ws_topic_publish(local->tree, topic, topic_len, data, len, binary, except_id)
        : 0;

    ws_interest_t interest;
    ws_interest_build(&interest, topic, topic_len);

    ws_payload_t *payload = NULL;
    uint64_t      skipped = 0;
    uint64_t      dropped = 0;

    tsrm_mutex_lock(hub->admin);
    const uint32_t posted = topic_hub_fanout_locked(hub, local, topic, topic_len,
        data, len, binary, except_id, &interest, &payload,
        /*full*/ NULL, /*nfull*/ NULL, &skipped, &dropped);
    tsrm_mutex_unlock(hub->admin);

    (void) ws_atomic_u64_add(&hub->posted,  (int64_t) posted);
    (void) ws_atomic_u64_add(&hub->skipped, (int64_t) skipped);
    (void) ws_atomic_u64_add(&hub->dropped, (int64_t) dropped);

    if (payload != NULL) {
        ws_payload_release(payload);
    }

    if (posted_out  != NULL) *posted_out  = posted;
    if (dropped_out != NULL) *dropped_out = dropped;

    return sent;
}

/* Posts the query to every worker that might hold a match, and counts what went
 * out in query->pending. A worker with no interest would answer 0, so skipping it
 * is not a shortcut — it is the same answer, sooner. */
static void topic_hub_ask_others(topic_hub_t *hub, const int own_slot, ws_query_t *query,
                              const char *topic, const size_t topic_len)
{
    ws_interest_t interest;
    ws_interest_build(&interest, topic, topic_len);

    tsrm_mutex_lock(hub->admin);

    for (int slot = 0; slot < hub->slots_used; slot++) {
        if (slot == own_slot || hub->inbox[slot] == NULL
            || !ws_interest_matches(hub, slot, &interest)) {
            continue;
        }

        ws_cmd_t *const cmd = ws_cmd_new(WS_CMD_COUNT, topic, topic_len);
        cmd->query = query;

        zend_atomic_int_fetch_add(&query->refcount, 1);

        if (topic_hub_post_locked(hub, slot, cmd)) {
            query->pending++;
        } else {
            zend_atomic_int_fetch_add(&query->refcount, -1);
            pefree(cmd, 1);
            topic_hub_note_drop(hub);
        }
    }

    tsrm_mutex_unlock(hub->admin);
}

uint32_t topic_hub_count(topic_hub_t *hub, const char *topic, const size_t topic_len,
                      const uint32_t timeout_ms)
{
    const ws_local_t *const local = hub != NULL ? ws_local_of(hub) : NULL;

    if (local == NULL) {
        return 0;
    }

    const uint32_t local_matches = ws_topic_count(local->tree, topic, topic_len);

    zend_coroutine_t *const coroutine = ZEND_ASYNC_CURRENT_COROUTINE;

    if (coroutine == NULL || ZEND_ASYNC_IS_SCHEDULER_CONTEXT) {
        return local_matches;   /* no coroutine to park — our answer is all there is */
    }

    /* In-thread event, not a cross-thread trigger: the replies come back through
     * our own mailbox, so nothing but this thread ever touches it. (An event's
     * ref_count is not atomic — a trigger shared with the answering workers could
     * not be disposed safely on a timeout.) */
    zend_async_event_t *const done = async_plain_event_new();

    if (done == NULL) {
        return local_matches;
    }

    ws_query_t *const query = pecalloc(1, sizeof(*query), 1);
    ZEND_ATOMIC_INT_INIT(&query->refcount, 1);
    query->slot  = local->slot;
    query->gen   = local->gen;
    query->total = local_matches;
    query->done  = done;

    topic_hub_ask_others(hub, local->slot, query, topic, topic_len);

    /* A timeout resumes cleanly — an exception here is a cancellation, and we
     * deliberately do not swallow it. */
    if (query->pending > 0
        && zend_async_waker_new_with_timeout(coroutine, timeout_ms, NULL) != NULL) {
        /* zend_try, because a fatal error longjmps past the settle below. The
         * query is persistent and a reply is still on its way home: an abandoned
         * query is the handshake that lets the reply's drain release it instead
         * of firing an event whose owner is gone. */
        volatile bool bailout = false;

        zend_try {
            zend_async_resume_when(coroutine, done, false, zend_async_waker_callback_resolve, NULL);
            ZEND_ASYNC_SUSPEND();
            zend_async_waker_clean(coroutine);
        } zend_catch {
            bailout = true;
        } zend_end_try();

        if (UNEXPECTED(bailout)) {
            query->abandoned = true;
            query->done      = NULL;
            done->dispose(done);
            ws_query_release(query);
            zend_bailout();
        }
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

/* ----------------------------------------------------------- reliable send
 *
 * The contract — publish() vs these, the queue, the config knobs — lives in
 * topic_hub.h. Implementation: a full target is parked as a {slot,gen} on this
 * worker's thread-local outbound queue and retried by the shared periodic timer
 * until it lands, the deadline passes, or the target worker detaches.
 */

static bool topic_hub_retry_arm(topic_hub_t *hub, ws_local_t *local);
static void topic_hub_retry_drain(topic_hub_t *hub, ws_local_t *local);

static retry_entry_t *retry_entry_new(const char *topic, const size_t topic_len,
        ws_payload_t *payload, const uint64_t except_id, const uint64_t deadline_ms,
        const retry_target_t *targets, const uint32_t ntargets, zend_async_event_t *waiter)
{
    retry_entry_t *const e = pemalloc(sizeof(*e) + topic_len, 1);

    e->next        = NULL;
    /* A blocking send() holds a ref across the suspend; the queue always holds
     * one. trySend has no waiter, so only the queue's ref. */
    ZEND_ATOMIC_INT_INIT(&e->refcount, waiter != NULL ? 2 : 1);
    e->abandoned   = false;
    e->shutdown    = false;
    e->payload     = payload;       /* the entry takes over the caller's base ref */
    e->npending    = ntargets;
    e->pending     = pemalloc((size_t) ntargets * sizeof(retry_target_t), 1);
    memcpy(e->pending, targets, (size_t) ntargets * sizeof(retry_target_t));
    e->delivered   = 0;
    e->gone        = 0;
    e->except_id   = except_id;
    e->deadline_ms = deadline_ms;
    e->waiter      = waiter;
    e->topic_len   = topic_len;

    if (topic_len != 0) {
        memcpy(e->topic, topic, topic_len);
    }

    return e;
}

/* The last reference frees the entry (never the payload — that is released once at
 * the terminal step, independent of the refcount). */
static void retry_entry_release(retry_entry_t *e)
{
    if (zend_atomic_int_fetch_add(&e->refcount, -1) == 1) {
        pefree(e->pending, 1);
        pefree(e, 1);
    }
}

/* Free an entry that never entered service — its enqueue could not arm the
 * drainer. Still sole-owned (nothing shared it across a suspend or a post), so
 * releasing its one payload ref and freeing it is the whole teardown. The caller
 * disposes any blocking-send event; it owns that. */
static void retry_entry_discard(retry_entry_t *e)
{
    ws_payload_release(e->payload);
    pefree(e->pending, 1);
    pefree(e, 1);
}

/* The terminal step for a resolved entry (all targets delivered/gone, deadline
 * passed, or the worker detaching). Wake a blocking send() exactly once — clearing
 * `waiter` before the fire, as count() clears `done` before dispose, so the
 * resuming caller never sees a stale event — but only when the caller has not
 * already abandoned it (cancellation). Then drop the one payload reference the
 * entry owned and the queue's refcount. MUST run outside hub->admin: the fire can
 * schedule a resume and we hold no lock across it (count()'s discipline). */
static void retry_entry_finish(retry_entry_t *e)
{
    if (e->waiter != NULL && !e->abandoned) {
        zend_async_event_t *const w = e->waiter;
        e->waiter = NULL;
        async_plain_event_fire(w);
    }

    ws_payload_release(e->payload);
    retry_entry_release(e);
}

/* -------------------------------------------------------------- the drainer */

static void topic_hub_retry_cb_dispose(zend_async_event_callback_t *cb,
                                       zend_async_event_t *event)
{
    (void) event;
    efree(cb);
}

static void topic_hub_retry_tick_fn(zend_async_event_t *event,
        zend_async_event_callback_t *callback, void *result, zend_object *exception)
{
    (void) event; (void) result;

    retry_cb_t *const  cb    = (retry_cb_t *) callback;
    topic_hub_t *const hub   = cb->hub;
    ws_local_t *const  local = ws_local_of(hub);

    if (exception != NULL) {
        return;   /* loop teardown — the reactor is going away */
    }

    if (local == NULL) {
        return;
    }

    topic_hub_retry_drain(hub, local);

    /* Queue drained — PAUSE the timer (keep the handle for reuse) so an idle worker
     * pays no periodic wakeup. Safe from inside the callback: stop() only clears the
     * repeat via uv_timer_stop; the event is not disposed and is re-started by the
     * next enqueue. */
    if (local->retry_head == NULL && local->retry_timer != NULL && local->retry_timer_armed) {
        local->retry_timer->stop(local->retry_timer);
        local->retry_timer_armed = false;
    }
}

/* Make sure the drainer is running. Returns false only on a genuine failure to
 * create/start the timer — so a caller that MUST have a tick coming (a blocking
 * send about to suspend with no timeout waker of its own) can refuse instead of
 * hanging. The MULTISHOT handle is created once and reused; here we (re)start it.
 * Mirrors pending_post_arm_timer's bool contract and http_server_deadline_tick's
 * MULTISHOT discipline. */
static bool topic_hub_retry_arm(topic_hub_t *hub, ws_local_t *local)
{
    if (local->retry_timer_armed) {
        return true;
    }

    if (local->retry_timer == NULL) {
        const zend_ulong ms = local->retry_interval_ms != 0 ? local->retry_interval_ms : 50;

        /* Periodic MULTISHOT: the reactor does NOT close it on fire, so one handle
         * serves every tick and we own its single dispose() at detach. */
        zend_async_timer_event_t *const t =
            ZEND_ASYNC_NEW_TIMER_EVENT(ms, /*periodic*/ true);

        if (UNEXPECTED(t == NULL)) {
            zend_clear_exception();
            return false;
        }

        ZEND_ASYNC_TIMER_SET_MULTISHOT(t);

        retry_cb_t *const cb = (retry_cb_t *)
            ZEND_ASYNC_EVENT_CALLBACK_EX(topic_hub_retry_tick_fn, sizeof(*cb));

        if (UNEXPECTED(cb == NULL)) {
            t->base.dispose(&t->base);
            return false;
        }

        cb->base.dispose = topic_hub_retry_cb_dispose;
        cb->hub          = hub;

        if (UNEXPECTED(!t->base.add_callback(&t->base, &cb->base))) {
            efree(cb);
            t->base.dispose(&t->base);
            return false;
        }

        local->retry_timer    = &t->base;
        local->retry_timer_cb = &cb->base;
    }

    if (UNEXPECTED(!local->retry_timer->start(local->retry_timer))) {
        return false;
    }

    local->retry_timer_armed = true;
    return true;
}

/* Detach-time teardown of the single reused timer. Stop it (if running) then
 * dispose once — dispose() frees the callbacks and schedules uv_close on a later
 * loop turn (libuv forbids a synchronous free of a live handle). */
static void topic_hub_retry_timer_dispose(ws_local_t *local)
{
    if (local->retry_timer == NULL) {
        return;
    }

    if (local->retry_timer_armed) {
        local->retry_timer->stop(local->retry_timer);
        local->retry_timer_armed = false;
    }

    local->retry_timer->dispose(local->retry_timer);
    local->retry_timer    = NULL;
    local->retry_timer_cb = NULL;
}

/* One tick: take hub->admin ONCE for the whole walk (the walk never suspends, so
 * one hold per tick rather than entries×targets acquisitions), resolve every
 * pending target, and unlink the entries that finished. Finishing (which fires a
 * waiter) is deferred to AFTER the unlock so no resume is scheduled under the
 * lock. */
static void topic_hub_retry_drain(topic_hub_t *hub, ws_local_t *local)
{
    const uint64_t now = ZEND_ASYNC_NOW();

    uint64_t delivered = 0;
    uint64_t gone      = 0;
    uint64_t expired   = 0;

    retry_entry_t *resolved = NULL;   /* finished this tick; ->next reused as its link */

    tsrm_mutex_lock(hub->admin);

    retry_entry_t *prev = NULL;
    retry_entry_t *e    = local->retry_head;

    while (e != NULL) {
        retry_entry_t *const next = e->next;

        /* Compact the pending set in place: landed or gone targets are removed,
         * a still-full one is kept at the front for the next tick. */
        uint32_t keep = 0;

        for (uint32_t r = 0; r < e->npending; r++) {
            const int      slot = e->pending[r].slot;
            const uint32_t gen  = e->pending[r].gen;

            /* detach nulls the inbox but does NOT bump gen, so a matching gen is
             * not proof of life — the null inbox is. Either way the worker we
             * parked for is gone; posting into a reused slot would mis-deliver. */
            if (hub->inbox[slot] == NULL || hub->gen[slot] != gen) {
                e->gone++;
                gone++;
                continue;
            }

            ws_cmd_t *const cmd = ws_cmd_new(WS_CMD_PUBLISH, e->topic, e->topic_len);
            cmd->payload   = e->payload;
            cmd->except_id = e->except_id;

            /* +1 before the post; the consumer's drain unconditionally releases
             * it (topic_hub_drain), so a failed post must -1. */
            zend_atomic_int_fetch_add(&e->payload->refcount, 1);

            if (topic_hub_post_locked(hub, slot, cmd)) {
                e->delivered++;
                delivered++;
            } else {
                ws_payload_release(e->payload);
                pefree(cmd, 1);
                e->pending[keep++] = e->pending[r];   /* still full — keep */
            }
        }

        e->npending = keep;

        const bool done = (e->npending == 0);
        const bool exp  = (!done && now >= e->deadline_ms);

        if (done || exp) {
            if (exp) {
                expired += e->npending;   /* the still-full remainder is the reported loss */
            }

            if (prev != NULL) {
                prev->next = next;
            } else {
                local->retry_head = next;
            }

            if (local->retry_tail == e) {
                local->retry_tail = prev;
            }

            local->retry_count--;

            e->next  = resolved;   /* park for the post-unlock finish */
            resolved = e;
            /* prev unchanged — e is gone from the live list */
        } else {
            prev = e;
        }

        e = next;
    }

    tsrm_mutex_unlock(hub->admin);

    /* Fire waiters and release refs with the lock dropped. */
    while (resolved != NULL) {
        retry_entry_t *const n = resolved->next;
        retry_entry_finish(resolved);
        resolved = n;
    }

    if (delivered != 0) (void) ws_atomic_u64_add(&hub->retry_delivered, (int64_t) delivered);
    if (gone      != 0) (void) ws_atomic_u64_add(&hub->retry_gone,      (int64_t) gone);
    if (expired   != 0) (void) ws_atomic_u64_add(&hub->retry_expired,   (int64_t) expired);

    /* The tick's caller pauses the timer when this leaves the queue empty. */
}

/* Detach teardown (called from topic_hub_detach, off hub->admin). Stop the timer
 * first so no tick starts, then finish every parked entry: a blocking send() is
 * woken (→ throws on shutdown rather than hanging), the payload and queue refs are
 * dropped. A send() that has not resumed yet is protected by its own entry
 * refcount, so its post-resume read of the struct is still valid. */
static void topic_hub_retry_teardown(topic_hub_t *hub, ws_local_t *local, const bool request_over)
{
    topic_hub_retry_timer_dispose(local);

    retry_entry_t *e    = local->retry_head;
    uint64_t       lost = 0;

    while (e != NULL) {
        retry_entry_t *const next = e->next;

        /* An entry with a waiter carries a second reference, taken by the sender
         * parked on it. When the request is over that sender is gone and will
         * never drop it, and the entry is persistent memory. */
        const bool orphaned = request_over && e->waiter != NULL && !e->abandoned;

        lost += e->npending;
        e->shutdown = true;      /* a woken send() reports SHUTDOWN, not a false timeout */

        if (request_over) {
            /* Its waiter is request memory owned by a coroutine that cannot
             * resume: firing it calls back into the dead. */
            e->abandoned = true;
        }

        retry_entry_finish(e);   /* fires the waiter iff still live and not abandoned */

        if (orphaned) {
            retry_entry_release(e);
        }

        e = next;
    }

    local->retry_head        = NULL;
    local->retry_tail        = NULL;
    local->retry_count       = 0;
    local->retry_interval_ms = 0;   /* queue is gone — let a future attach re-clock */

    if (lost != 0) {
        /* Clean-shutdown loss — charged to its own counter, never to retry_expired
         * (a deadline miss), so a shutdown is never read as a timeout. */
        (void) ws_atomic_u64_add(&hub->retry_shutdown, (int64_t) lost);
    }
}

/* Append one atomic entry to the outbound queue and make sure the drainer is
 * armed. Thread-local, so no lock. Returns false if the drainer could not be
 * armed — the entry is then unlinked and the queue restored exactly as it was, so
 * the caller can refuse the send rather than park behind a timer that will never
 * tick (pending_post_defer's discipline). */
static bool topic_hub_retry_enqueue(topic_hub_t *hub, ws_local_t *local,
                                    retry_entry_t *e, const uint32_t interval_ms)
{
    retry_entry_t *const prev_tail     = local->retry_tail;
    const uint32_t       prev_interval = local->retry_interval_ms;

    e->next = NULL;

    if (prev_tail != NULL) {
        prev_tail->next = e;
    } else {
        local->retry_head = e;
    }

    local->retry_tail = e;
    local->retry_count++;

    /* The very first enqueue fixes the drainer cadence for the worker's life: the
     * MULTISHOT timer is created once at that interval and only paused/resumed
     * thereafter, never re-clocked (a per-call interval on a later send is ignored —
     * first-wins). */
    if (local->retry_interval_ms == 0) {
        local->retry_interval_ms = interval_ms != 0 ? interval_ms : 50;
    }

    if (UNEXPECTED(!topic_hub_retry_arm(hub, local))) {
        /* Undo the append — restore head/tail/count/cadence to the prior state. */
        if (prev_tail != NULL) {
            prev_tail->next = NULL;
        } else {
            local->retry_head = NULL;
        }

        local->retry_tail        = prev_tail;
        local->retry_count--;
        local->retry_interval_ms = prev_interval;
        return false;
    }

    return true;
}

/* The fan-out common to both reliable calls: local tree first (like publish),
 * then the locked slot walk collecting full targets into `full`. Returns posted
 * count; fills *nfull and *payload (base ref transferred to the caller). */
static uint32_t topic_hub_send_fanout(topic_hub_t *hub, const ws_local_t *local,
        const char *topic, const size_t topic_len,
        const char *data, const size_t len, const bool binary, const uint64_t except_id,
        retry_target_t *full, uint32_t *nfull, ws_payload_t **payload)
{
    if (local != NULL) {
        (void) ws_topic_publish(local->tree, topic, topic_len, data, len, binary, except_id);
    }

    ws_interest_t interest;
    ws_interest_build(&interest, topic, topic_len);

    uint64_t skipped = 0;
    uint64_t dropped = 0;   /* always 0 on the reliable path — full targets park */

    *payload = NULL;
    *nfull   = 0;

    tsrm_mutex_lock(hub->admin);
    const uint32_t posted = topic_hub_fanout_locked(hub, local, topic, topic_len,
        data, len, binary, except_id, &interest, payload,
        full, nfull, &skipped, &dropped);
    tsrm_mutex_unlock(hub->admin);

    (void) ws_atomic_u64_add(&hub->posted,  (int64_t) posted);
    (void) ws_atomic_u64_add(&hub->skipped, (int64_t) skipped);

    return posted;
}

bool topic_hub_try_send(topic_hub_t *hub, const char *topic, const size_t topic_len,
        const char *data, const size_t len, const bool binary, const uint64_t except_id,
        const uint32_t timeout_ms, const uint32_t interval_ms, const uint32_t queue_max)
{
    if (hub == NULL) {
        return false;
    }

    ws_local_t *const local = ws_local_of(hub);

    retry_target_t full[TOPIC_HUB_MAX_WORKERS];
    uint32_t       nfull;
    ws_payload_t  *payload;

    (void) topic_hub_send_fanout(hub, local, topic, topic_len, data, len, binary,
                                 except_id, full, &nfull, &payload);

    if (nfull == 0) {
        /* Delivered outright — no target needed parking. */
        if (payload != NULL) {
            ws_payload_release(payload);
        }

        return true;
    }

    /* No attachment ⇒ no outbound queue to retry on. Not a queue-full refusal:
     * there is no queue, so it is not charged to retry_rejected. */
    if (local == NULL) {
        if (payload != NULL) {
            ws_payload_release(payload);
        }

        return false;
    }

    /* The cap is checked before we commit the (whole) entry, so the queue is never
     * half-parked. */
    if (local->retry_count >= queue_max) {
        if (payload != NULL) {
            ws_payload_release(payload);
        }

        (void) ws_atomic_u64_add(&hub->retry_rejected, 1);
        return false;
    }

    retry_entry_t *const e = retry_entry_new(topic, topic_len, payload, except_id,
        ZEND_ASYNC_NOW() + timeout_ms, full, nfull, /*waiter*/ NULL);

    if (UNEXPECTED(!topic_hub_retry_enqueue(hub, local, e, interval_ms))) {
        /* Drainer would not arm — nothing is parked, so this is a refusal. */
        retry_entry_discard(e);
        (void) ws_atomic_u64_add(&hub->retry_rejected, 1);
        return false;
    }

    (void) ws_atomic_u64_add(&hub->retry_queued, (int64_t) nfull);

    return true;
}

topic_hub_send_result_t topic_hub_send(topic_hub_t *hub, const char *topic, const size_t topic_len,
        const char *data, const size_t len, const bool binary, const uint64_t except_id,
        const uint32_t timeout_ms, const uint32_t interval_ms, const uint32_t queue_max)
{
    topic_hub_send_result_t result = { TOPIC_HUB_SEND_OK, 0, 0 };

    if (hub == NULL) {
        return result;
    }

    zend_coroutine_t *const coroutine = ZEND_ASYNC_CURRENT_COROUTINE;

    /* The caller chose the reliable path; with no coroutine to park we refuse
     * rather than degrade to best-effort publish(). Checked before any delivery,
     * so a NO_CONTEXT send is all-or-nothing. */
    if (coroutine == NULL || ZEND_ASYNC_IS_SCHEDULER_CONTEXT) {
        result.status = TOPIC_HUB_SEND_NO_CONTEXT;
        return result;
    }

    ws_local_t *const local = ws_local_of(hub);

    retry_target_t full[TOPIC_HUB_MAX_WORKERS];
    uint32_t       nfull;
    ws_payload_t  *payload;

    const uint32_t posted = topic_hub_send_fanout(hub, local, topic, topic_len,
        data, len, binary, except_id, full, &nfull, &payload);

    if (nfull == 0) {
        if (payload != NULL) {
            ws_payload_release(payload);
        }

        result.delivered = posted;
        return result;   /* OK — delivered outright */
    }

    /* No attachment ⇒ no outbound queue. Distinct from a queue-full refusal, and
     * not charged to retry_rejected — there is no queue to be full. */
    if (local == NULL) {
        if (payload != NULL) {
            ws_payload_release(payload);
        }

        result.status    = TOPIC_HUB_SEND_NO_QUEUE;
        result.delivered = posted;
        result.pending   = nfull;
        return result;
    }

    if (local->retry_count >= queue_max) {
        if (payload != NULL) {
            ws_payload_release(payload);
        }

        (void) ws_atomic_u64_add(&hub->retry_rejected, 1);
        result.status    = TOPIC_HUB_SEND_QUEUE_FULL;
        result.delivered = posted;
        result.pending   = nfull;
        return result;
    }

    /* In-thread event (count()'s reasoning): the drainer that fires it runs on THIS
     * thread, so its non-atomic ref_count is safe. */
    zend_async_event_t *const done = async_plain_event_new();

    if (UNEXPECTED(done == NULL)) {
        /* Cannot park a blocking waiter — report an at-capacity refusal rather than
         * silently losing the message. */
        if (payload != NULL) {
            ws_payload_release(payload);
        }

        (void) ws_atomic_u64_add(&hub->retry_rejected, 1);
        result.status    = TOPIC_HUB_SEND_QUEUE_FULL;
        result.delivered = posted;
        result.pending   = nfull;
        return result;
    }

    /* refcount 2: this caller holds one across the suspend, the queue the other. */
    retry_entry_t *const e = retry_entry_new(topic, topic_len, payload, except_id,
        ZEND_ASYNC_NOW() + timeout_ms, full, nfull, /*waiter*/ done);

    if (UNEXPECTED(!topic_hub_retry_enqueue(hub, local, e, interval_ms))) {
        /* The drainer would not arm; parking here would suspend on an event nothing
         * ever fires (we run no timeout waker of our own). Refuse before suspending:
         * dispose our event, discard the never-serviced entry, report queue-full. */
        done->dispose(done);
        retry_entry_discard(e);
        (void) ws_atomic_u64_add(&hub->retry_rejected, 1);
        result.status    = TOPIC_HUB_SEND_QUEUE_FULL;
        result.delivered = posted;
        result.pending   = nfull;
        return result;
    }

    (void) ws_atomic_u64_add(&hub->retry_queued, (int64_t) nfull);

    /* Park on the entry's event. NO timeout waker — the DRAINER owns the deadline
     * and fires this on success OR expiry (arm-failure above guarantees a tick is
     * coming). Cooperative scheduling guarantees the first tick cannot fire before
     * we suspend (count()'s structural guarantee). An exception on resume is a
     * cancellation and we do not swallow it. */
    zend_async_resume_when(coroutine, done, false, zend_async_waker_callback_resolve, NULL);
    ZEND_ASYNC_SUSPEND();
    zend_async_waker_clean(coroutine);

    /* Resumed. We still hold a reference, so these reads are valid even though the
     * drainer may already have unlinked and payload-released the entry. */
    const uint32_t entry_delivered = e->delivered;
    const uint32_t entry_pending   = e->npending;
    const bool     entry_shutdown  = e->shutdown;

    /* Settle our side: abandon and clear the waiter before disposing the event we
     * own, so any terminal step still to run skips the fire (the handshake — there
     * is no cross-thread racer in-thread, but this is the contract). */
    e->abandoned = true;
    e->waiter    = NULL;
    done->dispose(done);
    retry_entry_release(e);

    result.delivered = posted + entry_delivered;

    if (entry_shutdown) {
        result.status  = TOPIC_HUB_SEND_SHUTDOWN;   /* our worker detached — not a timeout */
        result.pending = entry_pending;
    } else if (entry_pending == 0) {
        result.status  = TOPIC_HUB_SEND_OK;
        result.pending = 0;
    } else {
        result.status  = TOPIC_HUB_SEND_EXPIRED;
        result.pending = entry_pending;
    }

    return result;
}

/* ------------------------------------------------------------ test hooks */

#ifdef TAS_TEST_HOOKS
/* proto void TrueAsync\__test_force_topic_post_full(int $n)
 * Make the next $n cross-worker posts (fan-out or retry) fail as if the target
 * mailbox were full, so a test can force the park -> retry path deterministically
 * without a real backed-up consumer. Runs on the thread that calls it; the counter
 * it sets is read under hub->admin by topic_hub_post_locked. */
PHP_FUNCTION(topic_hub_test_force_post_full)
{
    zend_long n;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(n)
    ZEND_PARSE_PARAMETERS_END();

    tas_test_force_full_posts = n > 0 ? (int) n : 0;
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_topic_hub_test_force_post_full, 0, 1, IS_VOID, 0)
    ZEND_ARG_TYPE_INFO(0, n, IS_LONG, 0)
ZEND_END_ARG_INFO()

static const zend_function_entry topic_hub_test_functions[] = {
    ZEND_RAW_FENTRY(ZEND_NS_NAME("TrueAsync", "__test_force_topic_post_full"),
                    zif_topic_hub_test_force_post_full,
                    arginfo_topic_hub_test_force_post_full, 0, NULL, NULL)
    ZEND_FE_END
};

void topic_hub_test_register(const int module_type)
{
    zend_register_functions(NULL, topic_hub_test_functions, NULL, module_type);
}
#else
void topic_hub_test_register(const int module_type)
{
    (void) module_type;
}
#endif /* TAS_TEST_HOOKS */
