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
#include "websocket/ws_topic_tree.h"
#include "websocket/ws_session.h"
#include "websocket/topic_hub.h"   /* the interest filter this worker publishes upward */

typedef enum {
    WS_NODE_ROOT,
    WS_NODE_LITERAL,
    WS_NODE_PLUS,
    WS_NODE_HASH,
} ws_node_kind_t;

typedef struct ws_topic_node {
    struct ws_topic_node *parent;
    ws_node_kind_t        kind;
    zend_string          *level;      /* LITERAL only — the key under parent->children */

    HashTable            *children;   /* level -> node*, allocated on first literal child */
    struct ws_topic_node *plus;
    struct ws_topic_node *hash;

    /* Subscribers AT this node. Dense, not a hash: delivery only walks it, and
     * the walk must not allocate. `dead` counts tombstones (ws_node_detach). */
    ws_session_t        **subs;
    uint32_t              count;
    uint32_t              cap;
    uint32_t              dead;

    bool                  dirty;      /* queued for compaction once the walk ends */
} ws_topic_node_t;

struct ws_topic_tree {
    ws_topic_node_t   root;

    /* Where this worker's interest filter lives — see topic_hub.h. */
    struct topic_hub_s  *hub;

    /* Bumped once per publish/count. A session stamped with the current mark has
     * already been served this pass, so overlapping filters (`a/b` and `a/#`)
     * deliver one copy, not two. */
    uint64_t          mark;

    /* A send can tear its own session down, re-entering unsubscribe mid-walk.
     * While the walk is in flight a removal only tombstones, and the node is
     * queued here — compacting or pruning it under the walk would free the very
     * node the walk is standing on. */
    uint32_t          walking;
    ws_topic_node_t **dirty;
    uint32_t          dirty_count;
    uint32_t          dirty_cap;
};

/* ---------------------------------------------------------------- parsing */

typedef struct {
    const char *level[WS_TOPIC_MAX_LEVELS];
    size_t      len[WS_TOPIC_MAX_LEVELS];
    uint32_t    count;
} ws_topic_levels_t;

/* Levels point into `topic` — valid only while it is. An empty level is legal
 * (MQTT allows "a//b"), an empty topic is not. */
static bool ws_topic_split(const char *topic, const size_t len, ws_topic_levels_t *out)
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

        if (out->count == WS_TOPIC_MAX_LEVELS) {
            return false;
        }

        out->level[out->count] = start;
        out->len[out->count]   = (size_t)(p - start);
        out->count++;

        start = p + 1;
    }

    return true;
}

static bool ws_topic_check(const char *topic, const size_t len, const bool wildcards_ok)
{
    ws_topic_levels_t levels;

    if (!ws_topic_split(topic, len, &levels)) {
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

bool ws_topic_is_valid_filter(const char *topic, const size_t len)
{
    return ws_topic_check(topic, len, true);
}

bool ws_topic_is_valid_name(const char *topic, const size_t len)
{
    return ws_topic_check(topic, len, false);
}

size_t ws_topic_interest_prefix(const char *filter, const size_t len)
{
    ws_topic_levels_t levels;

    if (!ws_topic_split(filter, len, &levels)) {
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

bool ws_topic_prefixes(const char *topic, const size_t topic_len, ws_topic_prefixes_t *out)
{
    ws_topic_levels_t levels;

    if (!ws_topic_split(topic, topic_len, &levels)) {
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

static ws_topic_node_t *ws_node_child(ws_topic_node_t *parent, const char *level,
                                      const size_t len, const bool create)
{
    if (len == 1 && level[0] == '+') {
        if (parent->plus == NULL && create) {
            parent->plus         = ecalloc(1, sizeof(*parent->plus));
            parent->plus->parent = parent;
            parent->plus->kind   = WS_NODE_PLUS;
        }

        return parent->plus;
    }

    if (len == 1 && level[0] == '#') {
        if (parent->hash == NULL && create) {
            parent->hash         = ecalloc(1, sizeof(*parent->hash));
            parent->hash->parent = parent;
            parent->hash->kind   = WS_NODE_HASH;
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

    ws_topic_node_t *node = zend_hash_str_find_ptr(parent->children, level, len);

    if (node != NULL || !create) {
        return node;
    }

    node         = ecalloc(1, sizeof(*node));
    node->parent = parent;
    node->kind   = WS_NODE_LITERAL;
    node->level  = zend_string_init(level, len, 0);

    zend_hash_add_new_ptr(parent->children, node->level, node);

    return node;
}

static void ws_node_free(ws_topic_node_t *node);

/* The root is embedded in the tree, so it is emptied but never freed. */
static void ws_node_free_contents(ws_topic_node_t *node)
{
    if (node->children != NULL) {
        ws_topic_node_t *child;
        ZEND_HASH_FOREACH_PTR(node->children, child) {
            ws_node_free(child);
        } ZEND_HASH_FOREACH_END();

        zend_hash_destroy(node->children);
        efree(node->children);
    }

    if (node->plus != NULL) {
        ws_node_free(node->plus);
    }

    if (node->hash != NULL) {
        ws_node_free(node->hash);
    }

    if (node->subs != NULL) {
        efree(node->subs);
    }

    if (node->level != NULL) {
        zend_string_release(node->level);
    }
}

static void ws_node_free(ws_topic_node_t *node)
{
    ws_node_free_contents(node);

    efree(node);
}

static bool ws_node_is_empty(const ws_topic_node_t *node)
{
    return node->count == 0
        && node->plus == NULL
        && node->hash == NULL
        && (node->children == NULL || zend_hash_num_elements(node->children) == 0);
}

/* A dynamic topic space ("order/{uuid}/status") would grow the tree forever, so
 * a node that lost its last subscriber and has no children goes away, and its
 * parent is then reconsidered. */
static void ws_node_prune(ws_topic_node_t *node)
{
    while (node->kind != WS_NODE_ROOT && ws_node_is_empty(node)) {
        ws_topic_node_t *const parent = node->parent;

        switch (node->kind) {
            case WS_NODE_PLUS:
                parent->plus = NULL;
                break;

            case WS_NODE_HASH:
                parent->hash = NULL;
                break;

            default:
                zend_hash_del(parent->children, node->level);
                break;
        }

        ws_node_free(node);

        node = parent;
    }
}

static void ws_node_compact(ws_topic_node_t *node)
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

/* --------------------------------------------------------------- sessions */

typedef struct ws_topic_sub {
    struct ws_topic_sub *next;
    ws_topic_node_t     *node;
    zend_string         *filter;
} ws_topic_sub_t;

static ws_topic_sub_t *ws_sub_find(const ws_session_t *session, const zend_string *filter)
{
    for (ws_topic_sub_t *sub = session->topics; sub != NULL; sub = sub->next) {
        if (zend_string_equals(sub->filter, filter)) {
            return sub;
        }
    }

    return NULL;
}

static void ws_tree_mark_dirty(ws_topic_tree_t *tree, ws_topic_node_t *node)
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

static void ws_tree_settle(ws_topic_tree_t *tree)
{
    for (uint32_t i = 0; i < tree->dirty_count; i++) {
        ws_topic_node_t *const node = tree->dirty[i];

        node->dirty = false;

        ws_node_compact(node);
        ws_node_prune(node);
    }

    tree->dirty_count = 0;
}

static void ws_node_detach(ws_topic_tree_t *tree, ws_topic_node_t *node,
                           const ws_session_t *session)
{
    uint32_t idx = node->count;

    for (uint32_t i = 0; i < node->count; i++) {
        if (node->subs[i] == session) {
            idx = i;
            break;
        }
    }

    if (idx == node->count) {
        return;
    }

    /* Mid-walk the array must not shift under the iterator — tombstone instead,
     * and let ws_tree_settle compact once the walk unwinds.
     *
     * `count` deliberately still counts the tombstone, and that is what keeps
     * ws_tree_settle safe: a node it has not reached yet never looks empty, so
     * pruning a dirty CHILD cannot cascade up and free a dirty parent still
     * sitting in the list. Decrement `count` here and settle becomes a UAF. */
    if (tree->walking > 0) {
        node->subs[idx] = NULL;
        node->dead++;
        ws_tree_mark_dirty(tree, node);
        return;
    }

    node->subs[idx] = node->subs[node->count - 1];
    node->count--;

    ws_node_prune(node);
}

/* ------------------------------------------------------------------- tree */

ws_topic_tree_t *ws_topic_tree_create(struct topic_hub_s *hub)
{
    ws_topic_tree_t *const tree = ecalloc(1, sizeof(*tree));
    tree->root.kind = WS_NODE_ROOT;
    tree->hub       = hub;

    return tree;
}

void ws_topic_tree_free(ws_topic_tree_t *tree)
{
    if (tree == NULL) {
        return;
    }

    ws_node_free_contents(&tree->root);

    if (tree->dirty != NULL) {
        efree(tree->dirty);
    }

    efree(tree);
}

static void ws_interest_publish(struct topic_hub_s *hub, const zend_string *filter,
                                const bool joining)
{
    const size_t prefix =
        ws_topic_interest_prefix(ZSTR_VAL(filter), ZSTR_LEN(filter));

    if (joining) {
        topic_hub_interest_add(hub, ZSTR_VAL(filter), prefix);
    } else {
        topic_hub_interest_remove(hub, ZSTR_VAL(filter), prefix);
    }
}

static uint32_t ws_sub_count(const ws_session_t *session)
{
    uint32_t count = 0;

    for (const ws_topic_sub_t *sub = session->topics; sub != NULL; sub = sub->next) {
        count++;
    }

    return count;
}

bool ws_topic_subscribe(ws_topic_tree_t *tree, ws_session_t *session,
                        zend_string *filter, const uint32_t max)
{
    ws_topic_levels_t levels;

    if (!ws_topic_split(ZSTR_VAL(filter), ZSTR_LEN(filter), &levels)) {
        return false;
    }

    if (ws_sub_find(session, filter) != NULL) {
        return true;   /* idempotent */
    }

    if (max != 0 && ws_sub_count(session) >= max) {
        return false;
    }

    ws_topic_node_t *node = &tree->root;

    for (uint32_t i = 0; i < levels.count; i++) {
        node = ws_node_child(node, levels.level[i], levels.len[i], true);
    }

    if (node->count == node->cap) {
        node->cap  = node->cap != 0 ? node->cap * 2 : 4;
        node->subs = erealloc(node->subs, node->cap * sizeof(*node->subs));
    }

    node->subs[node->count++] = session;

    ws_topic_sub_t *const sub = emalloc(sizeof(*sub));
    sub->node      = node;
    sub->filter    = zend_string_copy(filter);
    sub->next      = session->topics;
    session->topics = sub;

    ws_interest_publish(tree->hub, filter, true);

    return true;
}

static void ws_sub_drop(ws_topic_tree_t *tree, ws_session_t *session,
                        ws_topic_sub_t *sub, ws_topic_sub_t *prev)
{
    ws_node_detach(tree, sub->node, session);

    if (prev != NULL) {
        prev->next = sub->next;
    } else {
        session->topics = sub->next;
    }

    /* After the tree, never before: while a subscription is live the interest
     * filter must not understate it, or a publish would skip this worker. */
    ws_interest_publish(tree->hub, sub->filter, false);

    zend_string_release(sub->filter);

    efree(sub);
}

bool ws_topic_unsubscribe(ws_topic_tree_t *tree, ws_session_t *session,
                          const zend_string *filter)
{
    if (tree == NULL) {
        return false;   /* the worker detached; see ws_topic_unsubscribe_all */
    }

    ws_topic_sub_t *prev = NULL;

    for (ws_topic_sub_t *sub = session->topics; sub != NULL; prev = sub, sub = sub->next) {
        if (zend_string_equals(sub->filter, filter)) {
            ws_sub_drop(tree, session, sub, prev);
            return true;
        }
    }

    return false;
}

/* `tree` is NULL when the worker already detached — which happens only on the
 * bailout path, where start() cannot drain the sessions before letting go of the
 * tree. Their nodes are freed memory by then, so drop the list without touching
 * a single one of them. */
void ws_topic_unsubscribe_all(ws_topic_tree_t *tree, ws_session_t *session)
{
    while (session->topics != NULL) {
        if (tree != NULL) {
            ws_sub_drop(tree, session, session->topics, NULL);
            continue;
        }

        ws_topic_sub_t *const sub = session->topics;

        session->topics = sub->next;

        zend_string_release(sub->filter);

        efree(sub);
    }
}

void ws_topic_list(const ws_session_t *session, zval *return_value)
{
    array_init(return_value);

    for (const ws_topic_sub_t *sub = session->topics; sub != NULL; sub = sub->next) {
        add_next_index_str(return_value, zend_string_copy(sub->filter));
    }
}

/* ---------------------------------------------------------------- matching */

typedef struct {
    ws_topic_tree_t *tree;

    /* publish */
    const char *data;
    size_t      len;
    bool        binary;
    uint64_t    except_id;
    bool        should_deliver;

    uint32_t    hits;
} ws_topic_visit_t;

static void ws_topic_visit(ws_topic_visit_t *visit, ws_topic_node_t *node)
{
    for (uint32_t i = 0; i < node->count; i++) {
        ws_session_t *const session = node->subs[i];

        if (session == NULL || session->ws_id == visit->except_id) {
            continue;
        }

        /* Two filters of one session can match the same topic — serve it once. */
        if (session->topic_mark == visit->tree->mark) {
            continue;
        }

        session->topic_mark = visit->tree->mark;

        if (!visit->should_deliver) {
            visit->hits++;
            continue;
        }

        if (ws_session_try_send(session, visit->data, visit->len, visit->binary)) {
            visit->hits++;
        }
    }
}

static void ws_topic_walk(ws_topic_visit_t *visit, ws_topic_node_t *node,
                          const ws_topic_levels_t *levels, const uint32_t i)
{
    /* '#' takes the whole remainder — including none of it, which is why
     * "sport/#" matches "sport" itself. */
    if (node->hash != NULL) {
        ws_topic_visit(visit, node->hash);
    }

    if (i == levels->count) {
        ws_topic_visit(visit, node);
        return;
    }

    if (node->children != NULL) {
        ws_topic_node_t *const literal = zend_hash_str_find_ptr(
            node->children, levels->level[i], levels->len[i]);

        if (literal != NULL) {
            ws_topic_walk(visit, literal, levels, i + 1);
        }
    }

    if (node->plus != NULL) {
        ws_topic_walk(visit, node->plus, levels, i + 1);
    }
}

static uint32_t ws_topic_match(ws_topic_tree_t *tree, const char *topic,
                               const size_t topic_len, ws_topic_visit_t *visit)
{
    ws_topic_levels_t levels;

    if (!ws_topic_split(topic, topic_len, &levels)) {
        return 0;
    }

    tree->mark++;
    visit->tree = tree;

    tree->walking++;
    ws_topic_walk(visit, &tree->root, &levels, 0);
    tree->walking--;

    if (tree->walking == 0) {
        ws_tree_settle(tree);
    }

    return visit->hits;
}

uint32_t ws_topic_publish(ws_topic_tree_t *tree, const char *topic, const size_t topic_len,
                          const char *data, const size_t len, const bool binary,
                          const uint64_t except_id)
{
    ws_topic_visit_t visit = {
        .data      = data,
        .len       = len,
        .binary    = binary,
        .except_id = except_id,
        .should_deliver = true,
    };

    return ws_topic_match(tree, topic, topic_len, &visit);
}

uint32_t ws_topic_count(ws_topic_tree_t *tree, const char *topic, const size_t topic_len)
{
    ws_topic_visit_t visit = { .should_deliver = false };

    return ws_topic_match(tree, topic, topic_len, &visit);
}
