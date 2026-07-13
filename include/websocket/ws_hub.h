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
 * The consequence worth stating: there is no shared topic registry at all. The
 * hub does not know which worker holds subscribers of what, and does not need
 * to — it publishes to everyone and the workers filter, the same model
 * Socket.IO's Redis adapter uses. So there is no name table to lock, no topic
 * object to refcount, and no lifetime to get wrong. A wildcard filter could not
 * be interned as an object anyway: `user/42/#` is a predicate, not a room.
 *
 * What the hub does own is the worker slot table (one mailbox per worker), the
 * ws_id counter, and one interest filter per worker. `admin` guards the slots.
 *
 * Threading:
 *   - create()/addref()/release() from any thread.
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

ws_hub_t *ws_hub_create(void);
void      ws_hub_addref(ws_hub_t *hub);
void      ws_hub_release(ws_hub_t *hub);

/* Claims a slot and publishes this thread's mailbox. Returns the slot, or -1 —
 * every slot taken, or this thread is already attached to this hub. A caller
 * that ignores -1 gets a worker whose connections cannot subscribe at all, so
 * start() treats it as a startup failure rather than degrading quietly. */
int  ws_hub_attach(ws_hub_t *hub);
void ws_hub_detach(ws_hub_t *hub);

/* This thread's topic tree FOR THAT HUB, NULL when it never attached. Keyed by
 * hub, not thread: two HttpServers can share a thread, and a connection must
 * reach its own server's tree. A connection finds its hub through its server
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
     * is data loss: a worker is not draining fast enough, or a client is
     * flooding publishes (issue #120 — there is no rate limit yet). */
    uint64_t dropped;
} ws_hub_stats_t;

void ws_hub_get_stats(ws_hub_t *hub, ws_hub_stats_t *out);

/* ---------------------------------------------------------------- interest
 *
 * Without this, a publish wakes EVERY worker — copies the payload, posts a
 * command and fires an eventfd — and most of them then find no subscriber and
 * throw it away. So each worker summarises what it subscribes to in a counting
 * Bloom filter, and a publisher skips the workers that certainly do not match.
 * This is what NATS propagates between nodes as "interest", and it is why the
 * cost of a topic nobody in the process listens to is now zero wake-ups instead
 * of one per worker.
 *
 * Bloom, so a wildcard cannot be keyed by name: the filter holds each
 * subscription's leading literal prefix and a publisher probes every
 * level-prefix of its topic — see ws_topic_tree.h, which argues why that can
 * only ever produce a false POSITIVE (a wasted wake-up), never a false negative
 * (a lost message). Counting, because a Bloom bit cannot be cleared on
 * unsubscribe. A worker with a `#` subscription contributes the empty prefix and
 * is therefore woken by everything, correctly.
 *
 * The filter degrades honestly: a topic space with unboundedly many distinct
 * prefixes ("order/{uuid}/status") saturates it, every probe hits, and the hub
 * is back to waking everyone — never to losing a message.
 *
 * Called on the thread owning the session, from ws_topic_tree.c; a no-op on a
 * thread that never attached. `prefix_len` is a byte count into `filter`.
 */
void ws_hub_interest_add(ws_hub_t *hub, const char *filter, size_t prefix_len);
void ws_hub_interest_remove(ws_hub_t *hub, const char *filter, size_t prefix_len);

#endif /* WS_HUB_H */
