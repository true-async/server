/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

#ifndef ROOM_HUB_H
#define ROOM_HUB_H

#include "php.h"
#include "room/room_tree.h"

/*
 * Cross-worker topics (issue #2).
 *
 * A receiver belongs to one thread — a connection may only be written to by the
 * thread that owns it, and a coroutine's ring is thread memory — so a topic is
 * NOT a shared list. Each worker keeps its own topic tree (room_tree.h) over the
 * receivers IT owns, and a publish is handed to every worker through its mailbox
 * (thread_mailbox.h), carrying the topic as a STRING. Each worker then matches
 * that string against its own tree. A receiver pointer never leaves its
 * thread — a use-after-free is impossible by construction rather than by
 * discipline.
 *
 * The consequence worth stating: there is no shared topic registry. The hub
 * carries no name table to lock, no topic object to refcount, and no lifetime to
 * get wrong — a wildcard filter could not be interned as an object anyway,
 * `user/42/#` being a predicate rather than a room. What each worker publishes
 * upward is only a Bloom summary of its interest, never the topics themselves.
 *
 * What the hub does own is the worker slot table (one mailbox per worker), the
 * receiver-id counter, and one interest filter per worker. `admin` guards the
 * slots.
 *
 * Threading:
 *   - create()/release() on the owning server.
 *   - attach()/detach() on each worker (they own that thread's mailbox); detach
 *     drops the worker's reference and may free the hub there.
 *   - every subscribe/unsubscribe on the thread that owns the receiver.
 *   - publish()/count() from any thread.
 */

/* Matches the ceiling setWorkers() enforces. It has to: a worker that finds no
 * slot gets no topic tree, and then every subscribe() on it throws while every
 * publish() quietly does nothing — a half-working server. The table costs ~21KB
 * per hub; the 4KB interest filter is allocated per worker that actually
 * attaches, not per slot. */
#define ROOM_HUB_MAX_WORKERS 1024

typedef struct room_hub_s room_hub_t;

/* Reference counted. A new reference is derived from one the caller already
 * holds and taken where the pointer is copied, not at first use: a later addref
 * races the owner's release. addref/release take NULL.
 *
 * `retry_interval_ms` is the reliable-send drainer's cadence, in milliseconds,
 * for every thread that attaches to this hub. It belongs to the hub because the
 * drainer is one timer per worker: a per-call interval could only ever be
 * honoured by whichever call armed the timer first, and every later sender would
 * inherit that stranger's cadence without being told. 0 means the default (50). */
room_hub_t *room_hub_create(uint32_t retry_interval_ms);
void        room_hub_addref(room_hub_t *hub);
void        room_hub_release(room_hub_t *hub);

/* Claims a slot, publishes this thread's mailbox and takes a reference. Returns
 * the slot, or -1 when every slot is taken or this thread is already attached —
 * start() treats that as fatal. Every exit path must detach, on this thread. */
int  room_hub_attach(room_hub_t *hub);

/* Retires this thread's slot, tree and outbound queue, then drops the reference
 * attach() took; the hub may be freed here. Whoever is parked on a subscription
 * or a send is woken, so a live request learns its room went away. */
void room_hub_detach(room_hub_t *hub);

/* The same teardown for a request that can no longer run PHP — a fatal error, or
 * the end of the request. It wakes nobody, because the coroutines that parked
 * here are gone while their events are still request memory, and it releases by
 * hand what those absent owners would have released. */
void room_hub_detach_request_over(room_hub_t *hub);

/* Detaches whatever this thread still holds, at request shutdown. A thread that
 * attached by subscribing has no start() epilogue to do it, and a slot left
 * taken goes on collecting messages nobody will read. */
void room_hub_thread_sweep(void);

/* One publish's delivery breakdown. The global counters in room_hub_stats_t are
 * still accumulated; this is the same news for one call. */
typedef struct {
    uint32_t served;    /* local subscribers served synchronously on this thread */
    uint32_t workers;   /* worker slots live in the hub, this thread's included */
    uint64_t posted;    /* remote mailboxes that accepted the copy */
    uint64_t dropped;   /* full remote mailboxes that lost it */
} room_hub_publish_result_t;

/* Fans `topic` out to every worker; each matches it against its own tree.
 * Never suspends — a peer whose transport is backed up drops the message
 * (trySend semantics). Delivery to the other workers is asynchronous, so an
 * exact total would be a lie: `served` is this thread's own count.
 *
 * `workers` is what tells a caller that reached nobody WHY: zero means no thread
 * is attached to this hub at all, so the message had nowhere to go and no later
 * attach can rescue it; non-zero with served+posted == 0 means the workers are
 * there and the room is simply empty. */
room_hub_publish_result_t room_hub_publish(room_hub_t *hub,
                        const char *topic, size_t topic_len,
                        const char *data, size_t len, bool binary,
                        uint64_t except_id);

/* Scatter/gather: no global tally exists, so each worker answers with its own
 * match count. SUSPENDS the caller; a worker that misses `timeout_ms` is left
 * out of the sum, so the result is a snapshot, not a live number. Coroutine
 * context only. */
uint32_t room_hub_count(room_hub_t *hub, const char *topic, size_t topic_len,
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
 * `queue_max`/`timeout_ms` come from HttpServerConfig; the drainer's cadence is
 * the hub's (room_hub_create). The local delivery (this worker's own tree) is
 * done synchronously first, exactly as publish() does.
 */

typedef enum {
    ROOM_HUB_SEND_OK = 0,      /* delivered to every target (delivered = count) */
    ROOM_HUB_SEND_QUEUE_FULL,  /* outbound queue at cap; nothing parked */
    ROOM_HUB_SEND_EXPIRED,     /* deadline passed with a target still full */
    ROOM_HUB_SEND_NO_CONTEXT,  /* blocking send() called with no coroutine to park */
    ROOM_HUB_SEND_NO_QUEUE,    /* thread never attached — no outbound queue to retry on */
    ROOM_HUB_SEND_SHUTDOWN,    /* the worker detached while the send was parked */
    ROOM_HUB_SEND_NO_WORKERS,  /* no thread is attached to the hub — nowhere to deliver */
    ROOM_HUB_SEND_NO_TARGETS,  /* workers are running, but the room has no subscriber */
    ROOM_HUB_SEND_CANCELLED,   /* the parked sender was cancelled; EG(exception) is set */
} room_hub_send_status_t;

typedef struct {
    room_hub_send_status_t status;
    /* Targets the message reached: local subscribers served on this thread plus
     * remote mailboxes that took a copy. The two are added because a caller asks
     * "did it arrive anywhere", not "by which road": counting only the remote
     * road would answer 0 for a send whose whole room sits on the calling thread.
     *
     * It is not a subscriber census, and it is not comparable between senders: a
     * remote worker is ONE target however many subscribers sit behind it, so the
     * same room answers 5 from the thread that holds it and 1 from anywhere else.
     * A worker also counts as reached once its mailbox takes the copy, and the
     * interest filter is a Bloom summary that may hit for a worker whose tree then
     * matches nothing (room_tree.h). Only zero is exact — and zero never comes
     * back as OK, it comes back as NO_TARGETS. */
    uint32_t                delivered;
    uint32_t                pending;     /* targets still unfilled at give-up */
    uint32_t                workers;     /* worker slots live in the hub, this thread's included */
} room_hub_send_result_t;

/* Non-blocking: fan out, park full targets, return at once. Any thread; never
 * suspends, so it never returns CANCELLED, EXPIRED or SHUTDOWN — a parked
 * message's fate is in room_hub_get_stats(). OK means delivered outright or
 * parked for retry (`pending` says which).
 *
 * A refusal is not a promise that nothing was delivered: the fan-out runs first,
 * so QUEUE_FULL and NO_QUEUE can both follow targets that already took a copy.
 * `delivered` says how many, which is what makes a re-send a decision. */
room_hub_send_result_t room_hub_try_send(room_hub_t *hub, const char *topic, size_t topic_len,
                        const char *data, size_t len, bool binary, uint64_t except_id,
                        uint32_t timeout_ms, uint32_t queue_max);

/* Blocking: fan out, park full targets, then SUSPEND the calling coroutine until
 * every target lands or the deadline passes. Coroutine context only — with no
 * coroutine it returns ROOM_HUB_SEND_NO_CONTEXT rather than degrading to
 * best-effort (the caller chose the reliable path). A cancellation across the
 * park comes back as CANCELLED with the exception still pending, so the caller
 * reads one status instead of re-reading EG(exception) behind our back — and it
 * says nothing about the message, which stays on the retry queue until it lands
 * or expires: the sender was cancelled, not the send. */
room_hub_send_result_t room_hub_send(room_hub_t *hub, const char *topic, size_t topic_len,
                        const char *data, size_t len, bool binary, uint64_t except_id,
                        uint32_t timeout_ms, uint32_t queue_max);

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

    /* --- reliable send (room_hub_send / room_hub_try_send) ---------------
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

    /* Message bodies allocated. One publish costs one, whatever the node holds
     * and however many workers it reaches — a body is refcounted and shared by
     * every ring and every mailbox it lands in. So this number growing faster
     * than the publishes means a copy crept back into a delivery path. */
    uint64_t bodies;

    /* ...and released. Bodies are persistent memory whose owner is a mailbox, a
     * subscriber's ring or a retry entry, so a teardown that forgets to empty one
     * of those leaks them — invisibly, because a leak inside a structure valgrind
     * still sees a pointer to is not "definitely lost" (measured: removing the
     * ring drop from the dead-request sweep stays green under every leak-kind
     * filter). The difference `bodies - bodies_freed` is what sees it: at rest,
     * with nothing queued anywhere, the two are equal. */
    uint64_t bodies_freed;

    /* A server subscriber's ring was full, so its oldest message was dropped.
     * Apart from `dropped` deliberately: that one says a WORKER is not draining,
     * this one says one consumer inside a worker is behind. */
    uint64_t sub_overflow;
} room_hub_stats_t;

void room_hub_get_stats(room_hub_t *hub, room_hub_stats_t *out);

/* --------------------------------------------------------- caller receivers
 *
 * A receiver the caller owns and embeds, rather than one the hub mints. It
 * subscribes through the hub, so the tree stays inside it: the tree belongs to
 * this thread's attachment, and the hub is the only place that knows whether the
 * thread still has one.
 *
 * These never cause an attach, which is where they part company with the
 * server-side receiver below. An HTTP worker attaches in start(); a thread
 * without an attachment is refused rather than given a slot the server never
 * accounted for.
 *
 * Called on the thread owning the receiver, which never lets it go. A connection
 * finds its hub through its server (http_server_get_topic_hub), so nothing about
 * topics is carried into a handler. */

typedef enum {
    ROOM_HUB_SUBSCRIBE_OK = 0,
    ROOM_HUB_SUBSCRIBE_DETACHED,   /* this thread is not attached to the hub */
    ROOM_HUB_SUBSCRIBE_AT_CAP,     /* the receiver already holds `max` filters */
} room_hub_subscribe_status_t;

/* `filter` has already passed room_topic_is_valid_filter: a malformed one comes
 * back as AT_CAP, which is the wrong story to tell a caller.
 *
 * Idempotent — subscribing twice through the same filter is one subscription,
 * and the second spends no quota. `max` caps the distinct filters this receiver
 * may hold, 0 for none. The receiver takes its cross-thread id here, on its
 * first subscription, because nothing else about it needs one. */
room_hub_subscribe_status_t room_hub_receiver_subscribe(room_hub_t *hub,
                                                        room_receiver_t *receiver,
                                                        zend_string *filter, uint32_t max);

/* A no-op on a filter the receiver never held. On a thread that has detached it
 * is a no-op too, and the subscription stays in the receiver's own list — which
 * getTopics() reads and the teardown below clears. */
void room_hub_receiver_unsubscribe(room_hub_t *hub, room_receiver_t *receiver,
                                   const zend_string *filter);

/* Drops every filter the receiver holds, from the tree and from the receiver.
 * Called when a receiver goes away — for a connection, on every close, the
 * bailout path included; on that path this thread's tree is gone already and
 * only the receiver's own list is left to free. */
void room_hub_receiver_unsubscribe_all(room_hub_t *hub, room_receiver_t *receiver);

/* ------------------------------------------------------ server subscribers
 *
 * A receiver the hub itself mints, for server-side code that has no object of
 * its own to embed one in. Subscribing attaches this thread if nobody has;
 * unsubscribing never detaches, because a coroutine parked in send() on the same
 * thread holds no subscription and would be woken with a spurious shutdown. The
 * thread's exit is the only detach.
 *
 * Every call belongs to the thread that subscribed; a subscription is never
 * carried to another thread. */
typedef struct room_server_sub room_server_sub_t;
typedef struct room_payload    room_payload_t;   /* opaque; PHP never sees one */

typedef enum {
    ROOM_HUB_RECV_MESSAGE = 0,
    ROOM_HUB_RECV_TIMEOUT,     /* the deadline passed, or there was nothing and nowhere to park */
    ROOM_HUB_RECV_CLOSED,      /* unsubscribed, here or under a parked recv */
    ROOM_HUB_RECV_BUSY,        /* another coroutine is already parked on this subscription */
    /* An exception is pending across this call and the caller is unwinding — a
     * cancellation of the park, or a waiter this thread could not arm. Whatever
     * had arrived stays in the ring for the next reader. */
    ROOM_HUB_RECV_CANCELLED,
} room_hub_recv_status_t;

/* NULL when the thread could not attach (every slot taken). The caller owns one
 * reference and releases it with room_hub_sub_release. */
room_server_sub_t *room_hub_subscribe(room_hub_t *hub, zend_string *filter);
void               room_hub_unsubscribe(room_server_sub_t *sub);
void               room_hub_sub_release(room_server_sub_t *sub);

/* Messages this subscription's ring dropped because it was full. Monotonic and
 * never reset, so two readers cannot destroy each other's evidence. */
uint64_t room_hub_sub_lost(const room_server_sub_t *sub);

/* Pops one message, or parks the calling coroutine until one arrives, the
 * deadline passes, or the subscription closes. On MESSAGE the caller owns
 * `*out` and releases it with room_hub_payload_release.
 *
 * `timeout_ms`: negative waits with no deadline, 0 takes whatever is already
 * there and returns, positive waits that many milliseconds. */
room_hub_recv_status_t room_hub_recv(room_server_sub_t *sub, int64_t timeout_ms,
                                     room_payload_t **out);

const char *room_hub_payload_data(const room_payload_t *payload, size_t *len, bool *binary);
void        room_hub_payload_release(room_payload_t *payload);

/* ---------------------------------------------------------------- interest
 *
 * Each worker summarises its subscriptions in a counting Bloom filter so a
 * publisher can skip the workers that certainly hold no match, instead of waking
 * every one of them — the "interest" NATS propagates between nodes.
 *
 * Counting, because a Bloom bit cannot be cleared on unsubscribe. The key is the
 * subscription's leading literal prefix, never its full name: room_tree.h
 * argues why that can only cost a wasted wake-up and never lose a message.
 *
 * It degrades honestly: an unbounded topic space ("order/{uuid}/status")
 * saturates the filter, every probe hits, and the hub is back to waking everyone.
 *
 * Called on the thread owning the receiver; a no-op on a thread that never
 * attached. `prefix_len` is a byte count into `filter`.
 */
void room_hub_interest_add(room_hub_t *hub, const char *filter, size_t prefix_len);
void room_hub_interest_remove(room_hub_t *hub, const char *filter, size_t prefix_len);

/* Registers the reliable-room test hook (TrueAsync\__test_force_topic_post_full).
 * A no-op unless the extension was built with --enable-tas-test-hooks. Called from
 * MINIT. */
void room_hub_test_register(int module_type);

#endif /* ROOM_HUB_H */
