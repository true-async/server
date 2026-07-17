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
};

/* One copy shared by refcount across the whole fan-out, rather than one per
 * worker. The topic rides inline in each command instead — it is short. */
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

/* This thread's attachment to ONE hub. */
typedef struct ws_local_s {
    struct ws_local_s *next;
    topic_hub_t          *hub;
    int                slot;
    uint32_t           gen;
    thread_mailbox_t  *inbox;
    ws_topic_tree_t   *tree;
} ws_local_t;

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

    hub->admin = tsrm_mutex_alloc();

    return hub;
}

void topic_hub_release(topic_hub_t *hub)
{
    if (hub == NULL) {
        return;
    }

    tsrm_mutex_free(hub->admin);

    pefree(hub, 1);
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

/* The mailbox is freed by its own worker on detach, and thread_mailbox's
 * contract is "free after producers have quiesced" — so claiming a slot,
 * retiring it and posting into it all happen under `admin`. */
static bool topic_hub_post_locked(topic_hub_t *hub, const int slot, ws_cmd_t *cmd)
{
    thread_mailbox_t *const inbox = hub->inbox[slot];

    return inbox != NULL && thread_mailbox_post(inbox, cmd);
}

static void topic_hub_drain(void **items, const size_t count, void *arg);

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

void topic_hub_detach(topic_hub_t *hub)
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

    /* The slot is retired, so no producer can post any more. Whatever is still
     * queued holds payload/query references and thread_mailbox_free throws the
     * queue away without touching them — drain it first or every rotation of the
     * pool leaks. The attachment stays on the list across the drain: it is what
     * the drain resolves the tree through. */
    thread_mailbox_drain_pending(local->inbox);
    thread_mailbox_free(local->inbox);

    if (prev != NULL) {
        prev->next = local->next;
    } else {
        ws_locals = local->next;
    }

    ws_topic_tree_free(local->tree);

    efree(local);
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
                ws_query_settle(cmd->query, cmd->count);
                break;
        }

        pefree(cmd, 1);
    }
}

/* --------------------------------------------------------------- publish */

uint32_t topic_hub_publish(topic_hub_t *hub, const char *topic, const size_t topic_len,
                        const char *data, const size_t len, const bool binary,
                        const uint64_t except_id)
{
    if (hub == NULL) {
        return 0;
    }

    const ws_local_t *const local = ws_local_of(hub);

    const uint32_t sent = local != NULL
        ? ws_topic_publish(local->tree, topic, topic_len, data, len, binary, except_id)
        : 0;

    ws_interest_t interest;
    ws_interest_build(&interest, topic, topic_len);

    tsrm_mutex_lock(hub->admin);

    ws_payload_t *payload  = NULL;
    uint64_t      posted   = 0;
    uint64_t      skipped  = 0;

    for (int slot = 0; slot < hub->slots_used; slot++) {
        if (hub->inbox[slot] == NULL || (local != NULL && slot == local->slot)) {
            continue;
        }

        if (!ws_interest_matches(hub, slot, &interest)) {
            skipped++;
            continue;
        }

        /* One copy for the whole fan-out, refcounted — and none at all when every
         * other worker was skipped. */
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
            topic_hub_note_drop(hub);
        }
    }

    tsrm_mutex_unlock(hub->admin);

    (void) ws_atomic_u64_add(&hub->posted,  (int64_t) posted);
    (void) ws_atomic_u64_add(&hub->skipped, (int64_t) skipped);

    if (payload != NULL) {
        ws_payload_release(payload);
    }

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
        zend_async_resume_when(coroutine, done, false, zend_async_waker_callback_resolve, NULL);
        ZEND_ASYNC_SUSPEND();
        zend_async_waker_clean(coroutine);
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
