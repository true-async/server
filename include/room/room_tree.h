/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

#ifndef ROOM_TREE_H
#define ROOM_TREE_H

#include "php.h"

struct room_hub_s;

/*
 * Per-worker topic tree (issue #2). Thread-local: no locks, no atomics, no
 * shared registry. Each worker indexes only the receivers IT owns, and a publish
 * is handed to every worker, which matches the topic against its own tree — the
 * same "broadcast the message, let each node filter" model Socket.IO's Redis
 * adapter uses. Nobody has to know who holds what, so nothing has to be kept in
 * sync across threads.
 *
 * Topics are MQTT filters (RFC-grade semantics, so the edge cases are settled
 * rather than invented):
 *
 *   levels separated by '/'      sport/tennis/player1
 *   '+'  exactly ONE level       sport/+/player1  matches sport/tennis/player1
 *                                                 NOT sport/a/b/player1
 *   '#'  zero or MORE levels     sport/#         matches sport/tennis/player1
 *        (last level only)                       and sport itself
 *
 * Wildcards are legal in a SUBSCRIBE filter only. A publish topic must be
 * concrete: a message that fans out to a pattern has no well-defined
 * destination. room_topic_is_valid_name() enforces that.
 *
 * Matching walks the tree, trying the literal, '+' and '#' branch at each level
 * — O(topic length), not O(2^levels) as expanding a topic into every filter
 * that could match it would be.
 *
 * A subscriber holding several filters that all match one topic (`a/b` and
 * `a/#`) still receives ONE copy: delivery stamps each receiver with the pass
 * number and skips a receiver already stamped.
 */

/* Deeper filters are rejected — the walk recurses per level. Same ceiling as
 * EMQX's max_topic_levels, which is the only broker that caps this by default. */
#define ROOM_TOPIC_MAX_LEVELS 128

/* Every level-prefix of a topic, plus the empty one. */
#define ROOM_TOPIC_MAX_PREFIXES (ROOM_TOPIC_MAX_LEVELS + 1)

typedef struct room_tree room_tree_t;

/* ------------------------------------------------------------- subscribers
 *
 * A subscriber is a `room_receiver_t` embedded in whatever receives — a
 * connection, a server-side ring (room_hub.h) — and the tree sees nothing else
 * of it. One kind in a node, one delivery call, no branch.
 *
 * The owner embeds the struct ZERO-INITIALISED, and fills `ops` before the first
 * subscribe; `id` comes from the hub, which assigns it at subscribe. The tree
 * owns the rest, the filter list included. */
typedef struct room_receiver room_receiver_t;

typedef struct {
    /* Hands one message over and answers whether it was taken. `shared` is the
     * walk's one-message scratch, owned by whoever started the walk: the first
     * receiver that needs a persistent body puts it there and the rest take a
     * reference, so one publish costs one body however many receivers matched.
     * A receiver that copies into a transport queue leaves it untouched.
     *
     * Runs inside the walk, which tolerates the receiver tearing ITSELF down —
     * a socket write can close its own connection — and nothing beyond that. */
    bool (*deliver)(room_receiver_t *receiver, const char *data, size_t len,
                    bool binary, void **shared);
} room_receiver_ops_t;

struct room_receiver {
    const room_receiver_ops_t *ops;

    /* Identifies the receiver across threads, so a publish can skip its own
     * sender. NON-ZERO for every receiver the tree holds — that invariant is
     * what lets `except_id` 0 mean "skip nobody" without a second test. The
     * hub's counter (room_hub.h) is the only source. */
    uint64_t id;

    /* The last publish pass that served this receiver. */
    uint64_t mark;

    /* Filters this receiver holds, in no particular order. Owned by the tree,
     * which is why unsubscribe finds a node through the list rather than by
     * walking the topic again. */
    struct room_topic_sub *filters;
};

/* `hub` is where this tree publishes its interest filter (room_hub.h). */
room_tree_t *room_tree_create(struct room_hub_s *hub);
void         room_tree_free(room_tree_t *tree);

/* A filter may carry wildcards; a name may not. Both reject an empty string and
 * anything past ROOM_TOPIC_MAX_LEVELS. */
bool room_topic_is_valid_filter(const char *topic, size_t len);
bool room_topic_is_valid_name(const char *topic, size_t len);

/* Idempotent: subscribing twice through the same filter is one subscription, and
 * it does not spend quota. `max` is the cap on distinct filters this receiver may
 * hold, 0 for none — false means it was already at the cap, which the caller
 * reports on the filter without dropping the subscriber (what EMQX answers
 * with SUBACK 0x97 and NATS with -ERR 'Maximum Subscriptions Exceeded'). A
 * malformed filter is refused the same way, so the caller validates first.
 *
 * The caller owns the receiver and unsubscribes it before freeing it. */
bool room_topic_subscribe(room_tree_t *tree, room_receiver_t *receiver,
                          zend_string *filter, uint32_t max);
bool room_topic_unsubscribe(room_tree_t *tree, room_receiver_t *receiver,
                            const zend_string *filter);

/* Every filter the receiver holds. `tree` may be NULL: a worker that detached
 * took its nodes with it, and only the receiver's own list is left to free. */
void room_topic_unsubscribe_all(room_tree_t *tree, room_receiver_t *receiver);

/* Filters this receiver subscribed through, in no particular order. */
void room_topic_list(const room_receiver_t *receiver, zval *return_value);

/* Receivers on THIS worker matching `topic`, each served once. Never suspends:
 * a peer whose transport is backed up drops the message (trySend semantics). */
uint32_t room_topic_publish(room_tree_t *tree, const char *topic, size_t topic_len,
                            const char *data, size_t len, bool binary,
                            uint64_t except_id, void **shared);

/* Receivers on THIS worker matching `topic`, counted once each. */
uint32_t room_topic_count(room_tree_t *tree, const char *topic, size_t topic_len);

/* ---------------------------------------------------------------- interest
 *
 * These two feed the cross-worker interest filter (room_hub.h), and only make
 * sense together — they are the two halves of one claim:
 *
 *   if a filter matches a concrete topic, the filter's leading literal prefix
 *   is one of that topic's level-prefixes.
 *
 * True because the levels of a filter before its first wildcard match only
 * themselves, so a matching topic has to carry them, in order, from level 0.
 * A publisher can therefore ask "does this worker hold ANY filter whose prefix
 * is one of mine?" and skip the workers that answer no.
 */

/* Byte length of `filter`'s leading literal prefix. Zero when it opens with a
 * wildcard — `#` and `+/x` match topics that share no prefix with them at all. */
size_t room_topic_interest_prefix(const char *filter, size_t len);

/* Byte lengths of `topic`'s level-prefixes, shortest first, starting with the
 * empty one: "a/b" yields 0, 1, 3. False when `topic` will not split. */
typedef struct {
    size_t   len[ROOM_TOPIC_MAX_PREFIXES];
    uint32_t count;
} room_topic_prefixes_t;

bool room_topic_prefixes(const char *topic, size_t topic_len, room_topic_prefixes_t *out);

#endif /* ROOM_TREE_H */
