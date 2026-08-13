/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

#ifndef TOPIC_HUB_H
#define TOPIC_HUB_H

#include "php.h"
#include "websocket/ws_session.h"

/*
 * Cross-worker WebSocket topics (issue #2).
 *
 * Only the owning thread may write to a socket, so a topic is NOT a shared list
 * of connections. Each worker keeps its own topic tree (ws_topic_tree.h) over
 * the sessions IT owns, and a publish is handed to every worker through its
 * mailbox (thread_mailbox.h), carrying the topic as a STRING. Each worker then
 * matches that string against its own tree. A ws_session_t pointer never leaves
 * its thread — a use-after-free is impossible by construction rather than by
 * discipline.
 *
 * The consequence worth stating: there is no shared topic registry. The hub
 * carries no name table to lock, no topic object to refcount, and no lifetime to
 * get wrong — a wildcard filter could not be interned as an object anyway,
 * `user/42/#` being a predicate rather than a room. What each worker publishes
 * upward is only a Bloom summary of its interest, never the topics themselves.
 *
 * What the hub does own is the worker slot table (one mailbox per worker), the
 * ws_id counter, and one interest filter per worker. `admin` guards the slots.
 *
 * Threading:
 *   - create() on the owning server; release() drops that server's reference.
 *   - attach()/detach() on each worker (they own that thread's mailbox); detach
 *     drops the worker's reference, and the last holder frees the hub.
 *   - subscribe/unsubscribe/unsubscribe_all() on the thread owning the session.
 *   - publish()/count() from any thread.
 */

/* Matches the ceiling setWorkers() enforces. It has to: a worker that finds no
 * slot gets no topic tree, and then every subscribe() on it throws while every
 * publish() quietly does nothing — a half-working server. The table costs ~21KB
 * per hub; the 4KB interest filter is allocated per worker that actually
 * attaches, not per slot. */
#define TOPIC_HUB_MAX_WORKERS 1024

typedef struct topic_hub_s topic_hub_t;

/* Reference counted. create() returns the first reference; addref takes another
 * and release drops one, and the hub lives until the last holder lets go — so
 * the free can run on a worker thread, and does whenever a worker outlives the
 * server object that created the hub.
 *
 * A new reference must be derived from one the caller already holds. Every
 * pointer copy that outlives its source therefore addrefs at the copy — the
 * worker shell when the hub is fanned out, the clone when the shell is loaded —
 * rather than at first use: a copy that waits until it needs the hub is racing
 * the owner's release, which is the bug this counting exists to prevent.
 * Both take NULL, so a build without WebSocket support needs no guards. */
topic_hub_t *topic_hub_create(void);
void      topic_hub_addref(topic_hub_t *hub);
void      topic_hub_release(topic_hub_t *hub);

/* Claims a slot, publishes this thread's mailbox and takes a reference. Returns
 * the slot, or -1 — every slot taken, or this thread is already attached to this
 * hub. A caller that ignores -1 gets a worker whose connections cannot subscribe
 * at all, so start() treats it as a startup failure rather than degrading
 * quietly. A successful attach must be paired with a detach on the same thread,
 * on every exit path, or the hub is never freed. */
int  topic_hub_attach(topic_hub_t *hub);

/* Retires this thread's slot, tree and outbound queue, then drops the reference
 * attach() took — which may be the last, in which case the hub is freed here. */
void topic_hub_detach(topic_hub_t *hub);

/* This thread's topic tree FOR THAT HUB, NULL when it never attached. Keyed by
 * hub because a tree is per-SERVER state, which CODING_STANDARDS §1.2 keeps out
 * of thread-globals. A connection finds its hub through its server
 * (http_server_get_topic_hub), so no topic handle is carried into a handler. */
struct ws_topic_tree *topic_hub_tree(const topic_hub_t *hub);

/* Assigned on first subscribe; identifies a session across threads so a publish
 * can skip its own sender. */
uint64_t topic_hub_next_id(topic_hub_t *hub);

/* Fans `topic` out to every worker; each matches it against its own tree.
 * Never suspends — a peer whose transport is backed up drops the message
 * (trySend semantics). Returns the subscribers served on THIS worker; delivery
 * to the others is asynchronous, so an exact total would be a lie.
 *
 * A worker whose mailbox is full also drops the message, and that one is NOT in
 * the return value: it is counted in topic_hub_get_stats().dropped instead.
 *
 * `posted_out`/`dropped_out` (either may be NULL) report THIS call's remote
 * breakdown — mailboxes that accepted the copy vs full ones that dropped it — so
 * a caller can surface a per-publish delivery result without reading the
 * process-wide stats. The global counters are still accumulated regardless. */
uint32_t topic_hub_publish(topic_hub_t *hub, const char *topic, size_t topic_len,
                        const char *data, size_t len, bool binary,
                        uint64_t except_id,
                        uint64_t *posted_out, uint64_t *dropped_out);

/* Scatter/gather: no global tally exists, so each worker answers with its own
 * match count. SUSPENDS the caller; a worker that misses `timeout_ms` is left
 * out of the sum, so the result is a snapshot, not a live number. Coroutine
 * context only. */
uint32_t topic_hub_count(topic_hub_t *hub, const char *topic, size_t topic_len,
                      uint32_t timeout_ms);

/* -------------------------------------------------------------- reliable send
 *
 * `publish()` above is best-effort: a full target mailbox drops the copy. The
 * two calls below never drop silently — a full target is PARKED on a bounded
 * per-worker outbound queue and retried by a reactor timer until it lands or
 * `timeout_ms` passes. Flow control is between the sender and this queue (its
 * cap), never between the sender and the slowest consumer (the NATS model). See
 * docs/PLAN_RELIABLE_ROOM_PUBLISH.md.
 *
 * `queue_max`/`interval_ms`/`timeout_ms` come from HttpServerConfig; the hub
 * holds no config of its own. The local delivery (this worker's own tree) is
 * done synchronously first, exactly as publish() does.
 */

typedef enum {
    TOPIC_HUB_SEND_OK = 0,      /* delivered to every target (delivered = count) */
    TOPIC_HUB_SEND_QUEUE_FULL,  /* outbound queue at cap; nothing parked */
    TOPIC_HUB_SEND_EXPIRED,     /* deadline passed with a target still full */
    TOPIC_HUB_SEND_NO_CONTEXT,  /* blocking send() called with no coroutine to park */
    TOPIC_HUB_SEND_NO_QUEUE,    /* thread never attached — no outbound queue to retry on */
    TOPIC_HUB_SEND_SHUTDOWN,    /* the worker detached while the send was parked */
} topic_hub_send_status_t;

typedef struct {
    topic_hub_send_status_t status;
    uint32_t                delivered;   /* targets the message reached */
    uint32_t                pending;     /* targets still unfilled at give-up */
} topic_hub_send_result_t;

/* Non-blocking: fan out, park full targets, return at once. `true` = delivered
 * or parked; `false` = nothing was parked — the queue is at `queue_max` or the
 * drainer could not arm (counted retry_rejected), or the thread has no worker
 * attachment (not counted). Any thread; never suspends. */
bool topic_hub_try_send(topic_hub_t *hub, const char *topic, size_t topic_len,
                        const char *data, size_t len, bool binary, uint64_t except_id,
                        uint32_t timeout_ms, uint32_t interval_ms, uint32_t queue_max);

/* Blocking: fan out, park full targets, then SUSPEND the calling coroutine until
 * every target lands or the deadline passes. Coroutine context only — with no
 * coroutine it returns TOPIC_HUB_SEND_NO_CONTEXT rather than degrading to
 * best-effort (the caller chose the reliable path). */
topic_hub_send_result_t topic_hub_send(topic_hub_t *hub, const char *topic, size_t topic_len,
                        const char *data, size_t len, bool binary, uint64_t except_id,
                        uint32_t timeout_ms, uint32_t interval_ms, uint32_t queue_max);

/* Process-wide since start, for HttpServer::getRuntimeStats(). */
typedef struct {
    /* Commands handed to another worker's mailbox. */
    uint64_t posted;

    /* Workers the interest filter proved had no subscriber, so they were never
     * woken. Large next to `posted` means the filter is earning its keep; stuck
     * at zero under a many-worker fan-out means every worker really is
     * interested — or the topic space has saturated the filter. */
    uint64_t skipped;

    /* Commands a worker's mailbox would not take because it was full. This one
     * is data loss: a worker is not draining fast enough, or a publisher is
     * running without setWsPublishRateLimit(). */
    uint64_t dropped;

    /* --- reliable send (topic_hub_send / topic_hub_try_send) ---------------
     * The best-effort `dropped` above is never conflated with these: a full
     * mailbox on the reliable path is PARKED, not lost, and only these count. */

    /* Full targets parked on the sender-side outbound queue for retry. */
    uint64_t retry_queued;

    /* Parked targets a later retry got into the mailbox. */
    uint64_t retry_delivered;

    /* Parked targets dropped because their deadline passed with the mailbox
     * still full — the only reliable-path loss, and it is bounded and reported. */
    uint64_t retry_expired;

    /* Reliable sends refused at enqueue because the outbound queue was at its
     * cap (setWsPublishRetryQueueMax). trySend() returned false / send() threw;
     * nothing was parked. */
    uint64_t retry_rejected;

    /* Parked targets dropped because the target worker had detached (its inbox
     * gone or its slot reused) — delivering would have been a mis-delivery. */
    uint64_t retry_gone;

    /* Parked targets abandoned because THIS worker detached while they were still
     * in flight — a clean-shutdown loss, distinct from a deadline `retry_expired`
     * so a shutdown is never mislabelled a timeout. */
    uint64_t retry_shutdown;
} topic_hub_stats_t;

void topic_hub_get_stats(topic_hub_t *hub, topic_hub_stats_t *out);

/* ---------------------------------------------------------------- interest
 *
 * Each worker summarises its subscriptions in a counting Bloom filter so a
 * publisher can skip the workers that certainly hold no match, instead of waking
 * every one of them — the "interest" NATS propagates between nodes.
 *
 * Counting, because a Bloom bit cannot be cleared on unsubscribe. The key is the
 * subscription's leading literal prefix, never its full name: ws_topic_tree.h
 * argues why that can only cost a wasted wake-up and never lose a message.
 *
 * It degrades honestly: an unbounded topic space ("order/{uuid}/status")
 * saturates the filter, every probe hits, and the hub is back to waking everyone.
 *
 * Called on the thread owning the session; a no-op on a thread that never
 * attached. `prefix_len` is a byte count into `filter`.
 */
void topic_hub_interest_add(topic_hub_t *hub, const char *filter, size_t prefix_len);
void topic_hub_interest_remove(topic_hub_t *hub, const char *filter, size_t prefix_len);

/* Registers the reliable-room test hook (TrueAsync\__test_force_topic_post_full).
 * A no-op unless the extension was built with --enable-tas-test-hooks. Called from
 * MINIT. */
void topic_hub_test_register(int module_type);

#endif /* TOPIC_HUB_H */
