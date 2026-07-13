/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

#ifndef WS_TOPIC_TREE_H
#define WS_TOPIC_TREE_H

#include "php.h"
#include "websocket/ws_session.h"

/*
 * Per-worker topic tree (issue #2). Thread-local: no locks, no atomics, no
 * shared registry. Each worker indexes only the sessions IT owns, and a publish
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
 * destination. ws_topic_is_valid_name() enforces that.
 *
 * Matching walks the tree, trying the literal, '+' and '#' branch at each level
 * — O(topic length), not O(2^levels) as expanding a topic into every filter
 * that could match it would be.
 *
 * A session subscribed through several filters that all match one topic
 * (`a/b` and `a/#`) must still receive ONE copy: delivery stamps each session
 * with the pass number and skips a session already stamped.
 */

/* Deeper filters are rejected — the walk recurses per level. */
#define WS_TOPIC_MAX_LEVELS 32

/* Every level-prefix of a topic, plus the empty one. */
#define WS_TOPIC_MAX_PREFIXES (WS_TOPIC_MAX_LEVELS + 1)

typedef struct ws_topic_tree ws_topic_tree_t;

ws_topic_tree_t *ws_topic_tree_create(void);
void             ws_topic_tree_free(ws_topic_tree_t *tree);

/* A filter may carry wildcards; a name may not. Both reject an empty string and
 * anything past WS_TOPIC_MAX_LEVELS. */
bool ws_topic_is_valid_filter(const char *topic, size_t len);
bool ws_topic_is_valid_name(const char *topic, size_t len);

/* Idempotent: subscribing twice through the same filter is one subscription. */
bool ws_topic_subscribe(ws_topic_tree_t *tree, ws_session_t *session,
                        zend_string *filter);
bool ws_topic_unsubscribe(ws_topic_tree_t *tree, ws_session_t *session,
                          zend_string *filter);

/* Called from ws_session_destroy — a closing connection leaves every topic. */
void ws_topic_unsubscribe_all(ws_topic_tree_t *tree, ws_session_t *session);

/* Filters this session subscribed through, in no particular order. */
void ws_topic_list(const ws_session_t *session, zval *return_value);

/* Sessions on THIS worker matching `topic`, each served once. Never suspends:
 * a peer whose transport is backed up drops the message (trySend semantics). */
uint32_t ws_topic_publish(ws_topic_tree_t *tree, const char *topic, size_t topic_len,
                          const char *data, size_t len, bool binary,
                          uint64_t except_id);

/* Sessions on THIS worker matching `topic`, counted once each. */
uint32_t ws_topic_count(ws_topic_tree_t *tree, const char *topic, size_t topic_len);

/* ---------------------------------------------------------------- interest
 *
 * These two feed the cross-worker interest filter (ws_hub.h), and only make
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
size_t ws_topic_interest_prefix(const char *filter, size_t len);

/* Byte lengths of `topic`'s level-prefixes, shortest first, starting with the
 * empty one: "a/b" yields 0, 1, 3. False when `topic` will not split. */
typedef struct {
    size_t   len[WS_TOPIC_MAX_PREFIXES];
    uint32_t count;
} ws_topic_prefixes_t;

bool ws_topic_prefixes(const char *topic, size_t topic_len, ws_topic_prefixes_t *out);

#endif /* WS_TOPIC_TREE_H */
