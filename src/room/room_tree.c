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
#include "room/room_tree.h"
#include "room/room_hub.h"   /* the interest filter this worker publishes upward */

typedef enum {
    ROOM_NODE_ROOT,
    ROOM_NODE_LITERAL,
    ROOM_NODE_PLUS,
    ROOM_NODE_HASH,
} room_node_kind_t;

typedef struct room_topic_node {
    struct room_topic_node *parent;
    room_node_kind_t        kind;
    zend_string          *level;      /* LITERAL only — the key under parent->children */

    HashTable            *children;   /* level -> node*, allocated on first literal child */
    struct room_topic_node *plus;
    struct room_topic_node *hash;

    /* Receivers AT this node. Dense, not a hash: delivery only walks it, and the
     * walk allocates nothing per receiver. NULL is a tombstone, and `dead` counts
     * them (room_node_detach). */
    room_receiver_t **subs;
    uint32_t              count;
    uint32_t              cap;
    uint32_t              dead;

    bool                  dirty;      /* queued for compaction once the walk ends */
} room_topic_node_t;

struct room_tree {
    room_topic_node_t   root;

    /* Where this worker's interest filter lives — see room_hub.h. */
    struct room_hub_s  *hub;

    /* Bumped once per publish/count. A receiver stamped with the current mark
     * has already been served this pass, so overlapping filters (`a/b` and
     * `a/#`) deliver one copy, not two. */
    uint64_t          mark;

    /* A send can tear its own session down, re-entering unsubscribe mid-walk.
     * While the walk is in flight a removal only tombstones, and the node is
     * queued here — compacting or pruning it under the walk would free the very
     * node the walk is standing on. */
    uint32_t          walking;
    room_topic_node_t **dirty;
    uint32_t          dirty_count;
    uint32_t          dirty_cap;
};

/* ---------------------------------------------------------------- parsing */

typedef struct {
    const char *level[ROOM_TOPIC_MAX_LEVELS];
    size_t      len[ROOM_TOPIC_MAX_LEVELS];
    uint32_t    count;
} room_topic_levels_t;

/* Levels point into `topic` — valid only while it is. An empty level is legal
 * (MQTT allows "a//b"), an empty topic is not. */
static bool room_topic_split(const char *topic, const size_t len, room_topic_levels_t *out)
{
    if (len == 0) {
        return false;
    }

    out->count = 0;

    const char *start = topic;
    const char *const end = topic + len;

    for (const char *p = topic; p <= end; p++) {
        if (p != end && *p != '/') {
            continue;
        }

        if (out->count == ROOM_TOPIC_MAX_LEVELS) {
            return false;
        }

        out->level[out->count] = start;
        out->len[out->count]   = (size_t)(p - start);
        out->count++;

        start = p + 1;
    }

    return true;
}

static bool room_topic_check(const char *topic, const size_t len, const bool wildcards_ok)
{
    room_topic_levels_t levels;

    if (!room_topic_split(topic, len, &levels)) {
        return false;
    }

    for (uint32_t i = 0; i < levels.count; i++) {
        const char *const level     = levels.level[i];
        const size_t      level_len = levels.len[i];

        const bool is_plus = level_len == 1 && level[0] == '+';
        const bool is_hash = level_len == 1 && level[0] == '#';

        if (is_plus || is_hash) {
            if (!wildcards_ok) {
                return false;
            }

            /* '#' stands for the whole remainder, so nothing may follow it. */
            if (is_hash && i != levels.count - 1) {
                return false;
            }

            continue;
        }

        /* A wildcard is a level, never part of one: "sport+" is not a pattern. */
        if (memchr(level, '+', level_len) != NULL || memchr(level, '#', level_len) != NULL) {
            return false;
        }
    }

    return true;
}

bool room_topic_is_valid_filter(const char *topic, const size_t len)
{
    return room_topic_check(topic, len, true);
}

bool room_topic_is_valid_name(const char *topic, const size_t len)
{
    return room_topic_check(topic, len, false);
}

size_t room_topic_interest_prefix(const char *filter, const size_t len)
{
    room_topic_levels_t levels;

    if (!room_topic_split(filter, len, &levels)) {
        return 0;
    }

    size_t prefix = 0;

    for (uint32_t i = 0; i < levels.count; i++) {
        const char *const level = levels.level[i];

        if (levels.len[i] == 1 && (level[0] == '+' || level[0] == '#')) {
            break;
        }

        prefix = (size_t)(level + levels.len[i] - filter);
    }

    return prefix;
}

bool room_topic_prefixes(const char *topic, const size_t topic_len, room_topic_prefixes_t *out)
{
    room_topic_levels_t levels;

    if (!room_topic_split(topic, topic_len, &levels)) {
        return false;
    }

    out->len[0] = 0;
    out->count  = 1;

    for (uint32_t i = 0; i < levels.count; i++) {
        out->len[out->count++] = (size_t)(levels.level[i] + levels.len[i] - topic);
    }

    return true;
}

/* ------------------------------------------------------------------ nodes */

static room_topic_node_t *room_node_child(room_topic_node_t *parent, const char *level,
                                      const size_t len, const bool create)
{
    if (len == 1 && level[0] == '+') {
        if (parent->plus == NULL && create) {
            parent->plus         = ecalloc(1, sizeof(*parent->plus));
            parent->plus->parent = parent;
            parent->plus->kind   = ROOM_NODE_PLUS;
        }

        return parent->plus;
    }

    if (len == 1 && level[0] == '#') {
        if (parent->hash == NULL && create) {
            parent->hash         = ecalloc(1, sizeof(*parent->hash));
            parent->hash->parent = parent;
            parent->hash->kind   = ROOM_NODE_HASH;
        }

        return parent->hash;
    }

    if (parent->children == NULL) {
        if (!create) {
            return NULL;
        }

        parent->children = emalloc(sizeof(*parent->children));
        zend_hash_init(parent->children, 4, NULL, NULL, 0);
    }

    room_topic_node_t *node = zend_hash_str_find_ptr(parent->children, level, len);

    if (node != NULL || !create) {
        return node;
    }

    node         = ecalloc(1, sizeof(*node));
    node->parent = parent;
    node->kind   = ROOM_NODE_LITERAL;
    node->level  = zend_string_init(level, len, 0);

    zend_hash_add_new_ptr(parent->children, node->level, node);

    return node;
}

static void room_node_free(room_topic_node_t *node);

/* The root is embedded in the tree, so it is emptied but never freed. */
static void room_node_free_contents(room_topic_node_t *node)
{
    if (node->children != NULL) {
        room_topic_node_t *child;
        ZEND_HASH_FOREACH_PTR(node->children, child) {
            room_node_free(child);
        } ZEND_HASH_FOREACH_END();

        zend_hash_destroy(node->children);
        efree(node->children);
    }

    if (node->plus != NULL) {
        room_node_free(node->plus);
    }

    if (node->hash != NULL) {
        room_node_free(node->hash);
    }

    if (node->subs != NULL) {
        efree(node->subs);
    }

    if (node->level != NULL) {
        zend_string_release(node->level);
    }
}

static void room_node_free(room_topic_node_t *node)
{
    room_node_free_contents(node);

    efree(node);
}

static bool room_node_is_empty(const room_topic_node_t *node)
{
    return node->count == 0
        && node->plus == NULL
        && node->hash == NULL
        && (node->children == NULL || zend_hash_num_elements(node->children) == 0);
}

/* A dynamic topic space ("order/{uuid}/status") would grow the tree forever, so
 * a node that lost its last subscriber and has no children goes away, and its
 * parent is then reconsidered. */
static void room_node_prune(room_topic_node_t *node)
{
    while (node->kind != ROOM_NODE_ROOT && room_node_is_empty(node)) {
        room_topic_node_t *const parent = node->parent;

        switch (node->kind) {
            case ROOM_NODE_PLUS:
                parent->plus = NULL;
                break;

            case ROOM_NODE_HASH:
                parent->hash = NULL;
                break;

            default:
                zend_hash_del(parent->children, node->level);
                break;
        }

        room_node_free(node);

        node = parent;
    }
}

static void room_node_compact(room_topic_node_t *node)
{
    if (node->dead == 0) {
        return;
    }

    uint32_t kept = 0;

    for (uint32_t i = 0; i < node->count; i++) {
        if (node->subs[i] != NULL) {
            node->subs[kept++] = node->subs[i];
        }
    }

    node->count = kept;
    node->dead  = 0;
}

/* ------------------------------------------------------------- receivers */

typedef struct room_topic_sub {
    struct room_topic_sub *next;
    room_topic_node_t     *node;
    zend_string         *filter;
} room_topic_sub_t;

static room_topic_sub_t *room_sub_find(const room_receiver_t *receiver,
                                   const zend_string *filter)
{
    for (room_topic_sub_t *sub = receiver->filters; sub != NULL; sub = sub->next) {
        if (zend_string_equals(sub->filter, filter)) {
            return sub;
        }
    }

    return NULL;
}

static void room_tree_mark_dirty(room_tree_t *tree, room_topic_node_t *node)
{
    if (node->dirty) {
        return;
    }

    node->dirty = true;

    if (tree->dirty_count == tree->dirty_cap) {
        tree->dirty_cap = tree->dirty_cap != 0 ? tree->dirty_cap * 2 : 8;
        tree->dirty     = erealloc(tree->dirty, tree->dirty_cap * sizeof(*tree->dirty));
    }

    tree->dirty[tree->dirty_count++] = node;
}

static void room_tree_settle(room_tree_t *tree)
{
    for (uint32_t i = 0; i < tree->dirty_count; i++) {
        room_topic_node_t *const node = tree->dirty[i];

        node->dirty = false;

        room_node_compact(node);
        room_node_prune(node);
    }

    tree->dirty_count = 0;
}

static void room_node_detach(room_tree_t *tree, room_topic_node_t *node,
                           const room_receiver_t *receiver)
{
    uint32_t idx = node->count;

    for (uint32_t i = 0; i < node->count; i++) {
        if (node->subs[i] == receiver) {
            idx = i;
            break;
        }
    }

    if (idx == node->count) {
        return;
    }

    /* Mid-walk the array must not shift under the iterator — tombstone instead,
     * and let room_tree_settle compact once the walk unwinds.
     *
     * `count` deliberately still counts the tombstone, and that is what keeps
     * room_tree_settle safe: a node it has not reached yet never looks empty, so
     * pruning a dirty CHILD cannot cascade up and free a dirty parent still
     * sitting in the list. Decrement `count` here and settle becomes a UAF. */
    if (tree->walking > 0) {
        node->subs[idx] = NULL;
        node->dead++;
        room_tree_mark_dirty(tree, node);
        return;
    }

    node->subs[idx] = node->subs[node->count - 1];
    node->count--;

    room_node_prune(node);
}

/* ------------------------------------------------------------------- tree */

room_tree_t *room_tree_create(struct room_hub_s *hub)
{
    room_tree_t *const tree = ecalloc(1, sizeof(*tree));
    tree->root.kind = ROOM_NODE_ROOT;
    tree->hub       = hub;

    return tree;
}

void room_tree_free(room_tree_t *tree)
{
    if (tree == NULL) {
        return;
    }

    room_node_free_contents(&tree->root);

    if (tree->dirty != NULL) {
        efree(tree->dirty);
    }

    efree(tree);
}

static void room_interest_publish(struct room_hub_s *hub, const zend_string *filter,
                                const bool joining)
{
    const size_t prefix =
        room_topic_interest_prefix(ZSTR_VAL(filter), ZSTR_LEN(filter));

    if (joining) {
        room_hub_interest_add(hub, ZSTR_VAL(filter), prefix);
    } else {
        room_hub_interest_remove(hub, ZSTR_VAL(filter), prefix);
    }
}

static uint32_t room_sub_count(const room_receiver_t *receiver)
{
    uint32_t count = 0;

    for (const room_topic_sub_t *sub = receiver->filters; sub != NULL; sub = sub->next) {
        count++;
    }

    return count;
}

/* Creates the missing levels; returns the leaf so the caller can record it. */
static room_topic_node_t *room_node_add(room_tree_t *tree, const room_topic_levels_t *levels,
                                    room_receiver_t *receiver)
{
    room_topic_node_t *node = &tree->root;

    for (uint32_t i = 0; i < levels->count; i++) {
        node = room_node_child(node, levels->level[i], levels->len[i], true);
    }

    if (node->count == node->cap) {
        node->cap  = node->cap != 0 ? node->cap * 2 : 4;
        node->subs = erealloc(node->subs, node->cap * sizeof(*node->subs));
    }

    node->subs[node->count] = receiver;
    node->count++;

    return node;
}

bool room_topic_subscribe(room_tree_t *tree, room_receiver_t *receiver,
                        zend_string *filter, const uint32_t max)
{
    /* An id of 0 collides with "exclude nobody" and the receiver would then be
     * skipped by every publish, silently. Free in release, loud under the
     * fuzzers and the debug build. */
    ZEND_ASSERT(receiver->id != 0);

    room_topic_levels_t levels;

    if (!room_topic_split(ZSTR_VAL(filter), ZSTR_LEN(filter), &levels)) {
        return false;
    }

    if (room_sub_find(receiver, filter) != NULL) {
        return true;   /* idempotent */
    }

    if (max != 0 && room_sub_count(receiver) >= max) {
        return false;
    }

    room_topic_node_t *const node = room_node_add(tree, &levels, receiver);

    room_topic_sub_t *const sub = emalloc(sizeof(*sub));
    sub->node         = node;
    sub->filter       = zend_string_copy(filter);
    sub->next         = receiver->filters;
    receiver->filters = sub;

    room_interest_publish(tree->hub, filter, true);

    return true;
}

static void room_sub_drop(room_tree_t *tree, room_receiver_t *receiver,
                        room_topic_sub_t *sub, room_topic_sub_t *prev)
{
    room_node_detach(tree, sub->node, receiver);

    if (prev != NULL) {
        prev->next = sub->next;
    } else {
        receiver->filters = sub->next;
    }

    /* After the tree, never before: while a subscription is live the interest
     * filter must not understate it, or a publish would skip this worker. */
    room_interest_publish(tree->hub, sub->filter, false);

    zend_string_release(sub->filter);

    efree(sub);
}

bool room_topic_unsubscribe(room_tree_t *tree, room_receiver_t *receiver,
                          const zend_string *filter)
{
    if (tree == NULL) {
        return false;   /* the worker detached; see room_topic_unsubscribe_all */
    }

    room_topic_sub_t *prev = NULL;

    for (room_topic_sub_t *sub = receiver->filters; sub != NULL; prev = sub, sub = sub->next) {
        if (zend_string_equals(sub->filter, filter)) {
            room_sub_drop(tree, receiver, sub, prev);
            return true;
        }
    }

    return false;
}

/* `tree` is NULL when the worker already detached — which happens only on the
 * bailout path, where start() cannot drain the receivers before letting go of
 * the tree. Their nodes are freed memory by then, so drop the list without
 * touching a single one of them. */
void room_topic_unsubscribe_all(room_tree_t *tree, room_receiver_t *receiver)
{
    while (receiver->filters != NULL) {
        if (tree != NULL) {
            room_sub_drop(tree, receiver, receiver->filters, NULL);
            continue;
        }

        room_topic_sub_t *const sub = receiver->filters;

        receiver->filters = sub->next;

        zend_string_release(sub->filter);

        efree(sub);
    }
}

void room_topic_list(const room_receiver_t *receiver, zval *return_value)
{
    array_init(return_value);

    for (const room_topic_sub_t *sub = receiver->filters; sub != NULL; sub = sub->next) {
        add_next_index_str(return_value, zend_string_copy(sub->filter));
    }
}

/* ---------------------------------------------------------------- matching */

typedef struct {
    room_tree_t *tree;

    /* publish */
    const char *data;
    size_t      len;
    bool        binary;
    uint64_t    except_id;

    /* The caller's one-message scratch (see the header) — and the walk's mode:
     * a counting walk has none, so "deliver" and "have somewhere to put the
     * body" cannot drift apart into a pair of flags that disagree. */
    void      **shared;

    uint32_t    hits;
} room_topic_visit_t;

static void room_topic_visit(room_topic_visit_t *visit, room_topic_node_t *node)
{
    for (uint32_t i = 0; i < node->count; i++) {
        room_receiver_t *const receiver = node->subs[i];

        if (receiver == NULL) {
            continue;
        }

        /* Every receiver in the tree has a non-zero id, so except_id 0 skips
         * nobody and a publisher that never subscribed excludes nothing. */
        if (receiver->id == visit->except_id) {
            continue;
        }

        /* Two filters of one receiver can match the same topic — serve it once. */
        if (receiver->mark == visit->tree->mark) {
            continue;
        }

        receiver->mark = visit->tree->mark;

        if (visit->shared == NULL) {
            visit->hits++;
            continue;
        }

        if (receiver->ops->deliver(receiver, visit->data, visit->len, visit->binary,
                                   visit->shared)) {
            visit->hits++;
        }
    }
}

static void room_topic_walk(room_topic_visit_t *visit, room_topic_node_t *node,
                          const room_topic_levels_t *levels, const uint32_t i)
{
    /* '#' takes the whole remainder — including none of it, which is why
     * "sport/#" matches "sport" itself. */
    if (node->hash != NULL) {
        room_topic_visit(visit, node->hash);
    }

    if (i == levels->count) {
        room_topic_visit(visit, node);
        return;
    }

    if (node->children != NULL) {
        room_topic_node_t *const literal = zend_hash_str_find_ptr(
            node->children, levels->level[i], levels->len[i]);

        if (literal != NULL) {
            room_topic_walk(visit, literal, levels, i + 1);
        }
    }

    if (node->plus != NULL) {
        room_topic_walk(visit, node->plus, levels, i + 1);
    }
}

static uint32_t room_topic_match(room_tree_t *tree, const char *topic,
                               const size_t topic_len, room_topic_visit_t *visit)
{
    room_topic_levels_t levels;

    if (!room_topic_split(topic, topic_len, &levels)) {
        return 0;
    }

    tree->mark++;
    visit->tree = tree;

    tree->walking++;
    room_topic_walk(visit, &tree->root, &levels, 0);
    tree->walking--;

    if (tree->walking == 0) {
        room_tree_settle(tree);
    }

    return visit->hits;
}

uint32_t room_topic_publish(room_tree_t *tree, const char *topic, const size_t topic_len,
                          const char *data, const size_t len, const bool binary,
                          const uint64_t except_id, void **shared)
{
    room_topic_visit_t visit = {
        .data      = data,
        .len       = len,
        .binary    = binary,
        .except_id = except_id,
        .shared    = shared,
    };

    return room_topic_match(tree, topic, topic_len, &visit);
}

uint32_t room_topic_count(room_tree_t *tree, const char *topic, const size_t topic_len)
{
    room_topic_visit_t visit = { .shared = NULL };   /* counting walk: nothing to deliver into */

    return room_topic_match(tree, topic, topic_len, &visit);
}
