/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

#ifndef WS_HUB_H
#define WS_HUB_H

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
 *   - create()/release() on the owning server.
 *   - attach()/detach() on each worker (they own that thread's mailbox).
 *   - subscribe/unsubscribe/unsubscribe_all() on the thread owning the session.
 *   - publish()/count() from any thread.
 */

/* Matches the ceiling setWorkers() enforces. It has to: a worker that finds no
 * slot gets no topic tree, and then every subscribe() on it throws while every
 * publish() quietly does nothing — a half-working server. The table costs ~21KB
 * per hub; the 4KB interest filter is allocated per worker that actually
 * attaches, not per slot. */
#define WS_HUB_MAX_WORKERS 1024

typedef struct ws_hub_s ws_hub_t;

/* One owner — the server that created it. Clones borrow the pointer and never
 * free it, so there is nothing to refcount. */
ws_hub_t *ws_hub_create(void);
void      ws_hub_release(ws_hub_t *hub);

/* Claims a slot and publishes this thread's mailbox. Returns the slot, or -1 —
 * every slot taken, or this thread is already attached to this hub. A caller
 * that ignores -1 gets a worker whose connections cannot subscribe at all, so
 * start() treats it as a startup failure rather than degrading quietly. */
int  ws_hub_attach(ws_hub_t *hub);
void ws_hub_detach(ws_hub_t *hub);

/* This thread's topic tree FOR THAT HUB, NULL when it never attached. Keyed by
 * hub because a tree is per-SERVER state, which CODING_STANDARDS §1.2 keeps out
 * of thread-globals. A connection finds its hub through its server
 * (http_server_get_ws_hub), so no topic handle is carried into a handler. */
struct ws_topic_tree *ws_hub_tree(const ws_hub_t *hub);

/* Assigned on first subscribe; identifies a session across threads so a publish
 * can skip its own sender. */
uint64_t ws_hub_next_id(ws_hub_t *hub);

/* Fans `topic` out to every worker; each matches it against its own tree.
 * Never suspends — a peer whose transport is backed up drops the message
 * (trySend semantics). Returns the subscribers served on THIS worker; delivery
 * to the others is asynchronous, so an exact total would be a lie.
 *
 * A worker whose mailbox is full also drops the message, and that one is NOT in
 * the return value: it is counted in ws_hub_get_stats().dropped instead. */
uint32_t ws_hub_publish(ws_hub_t *hub, const char *topic, size_t topic_len,
                        const char *data, size_t len, bool binary,
                        uint64_t except_id);

/* Scatter/gather: no global tally exists, so each worker answers with its own
 * match count. SUSPENDS the caller; a worker that misses `timeout_ms` is left
 * out of the sum, so the result is a snapshot, not a live number. Coroutine
 * context only. */
uint32_t ws_hub_count(ws_hub_t *hub, const char *topic, size_t topic_len,
                      uint32_t timeout_ms);

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
} ws_hub_stats_t;

void ws_hub_get_stats(ws_hub_t *hub, ws_hub_stats_t *out);

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
void ws_hub_interest_add(ws_hub_t *hub, const char *filter, size_t prefix_len);
void ws_hub_interest_remove(ws_hub_t *hub, const char *filter, size_t prefix_len);

#endif /* WS_HUB_H */
