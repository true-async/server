/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/*
 * libFuzzer harness for the WebSocket pub/sub topic engine (src/websocket/
 * ws_topic_tree.c). Topic names and subscribe filters arrive verbatim from
 * the network — a client picks the bytes — so the MQTT-style parser
 * (ws_topic_split / ws_topic_is_valid_*), the level-prefix builder that feeds
 * the cross-worker interest filter (ws_topic_prefixes / _interest_prefix), and
 * the wildcard matcher + node lifecycle (subscribe / unsubscribe / publish with
 * tombstone-and-compact) are all reachable with attacker-chosen input. The
 * frame fuzzer (fuzz_ws_frame) stubs this whole TU out; this harness drives it.
 *
 * The bytes are read as a little command program over a small pool of receivers:
 * subscribe / unsubscribe / unsubscribe-all / publish / count, each on a
 * receiver the input selects. That builds, mutates, prunes and walks a real
 * tree, so ASAN/UBSan see the growth, the tombstone compaction, the empty-node
 * prune cascade and the recursive matcher — not just the string parser. The
 * pure validators run on every slice regardless of the command, so malformed
 * topics reach the parser even when the tree stays empty.
 *
 * The pool is mixed on purpose: some receivers take the message, some refuse it
 * as a backed-up transport does, and one unsubscribes itself from inside the
 * delivery callback — which is how a socket write that closes its own connection
 * reaches the tombstone-and-compact path.
 *
 * No hub, no threads: the tree is per-worker and single-threaded by design, and
 * the two hub-interest calls are stubbed below (the cross-thread fan-out lives
 * in topic_hub.c and is out of this TU's scope).
 */

#include "harness_common.h"
#include "websocket/ws_topic_tree.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* ws_topic_tree.c publishes each subscribe/unsubscribe into the owning worker's
 * cross-worker interest filter; that filter lives in topic_hub.c, which is not in
 * this TU. A no-op keeps the tree self-contained — the prefix it hands us is
 * already exercised directly via ws_topic_interest_prefix below. */
void topic_hub_interest_add(struct topic_hub_s *hub, const char *filter, size_t prefix_len)
{
    (void)hub; (void)filter; (void)prefix_len;
}

void topic_hub_interest_remove(struct topic_hub_s *hub, const char *filter, size_t prefix_len)
{
    (void)hub; (void)filter; (void)prefix_len;
}

/* Delivery targets. The matcher calls one of these for every receiver a publish
 * reaches; the real sends (wslay and the transport, the server ring) belong to
 * fuzz_ws_frame and topic_hub.c. None of them writes `shared`, so no persistent
 * body is made in this TU. */
static bool ws_fuzz_deliver_ok(ws_topic_receiver_t *receiver, const char *data,
                               size_t len, bool binary, void **shared)
{
    (void)receiver; (void)data; (void)len; (void)binary; (void)shared;
    return true;
}

static bool ws_fuzz_deliver_refused(ws_topic_receiver_t *receiver, const char *data,
                                    size_t len, bool binary, void **shared)
{
    (void)receiver; (void)data; (void)len; (void)binary; (void)shared;
    return false;
}

/* A socket write can close its own connection, so delivery re-enters unsubscribe
 * in the middle of the walk — the case the tombstone-and-compact path exists for.
 * One receiver of the pool does it on every message it is handed. */
typedef struct {
    ws_topic_receiver_t base;      /* first: the ops callback casts straight back */
    ws_topic_tree_t    *tree;
} ws_fuzz_receiver_t;

static bool ws_fuzz_deliver_self_closing(ws_topic_receiver_t *receiver, const char *data,
                                         size_t len, bool binary, void **shared)
{
    (void)data; (void)len; (void)binary; (void)shared;

    ws_topic_unsubscribe_all(((ws_fuzz_receiver_t *)receiver)->tree, receiver);

    return false;
}

static const ws_topic_receiver_ops_t ws_fuzz_ops_ok      = { .deliver = ws_fuzz_deliver_ok };
static const ws_topic_receiver_ops_t ws_fuzz_ops_refused = { .deliver = ws_fuzz_deliver_refused };
static const ws_topic_receiver_ops_t ws_fuzz_ops_closing = { .deliver = ws_fuzz_deliver_self_closing };

#define WS_TOPIC_FUZZ_RECEIVERS 8u
#define WS_TOPIC_FUZZ_MAX_SUBS 32u   /* exercise the at-cap SUBACK-refused path */

/* One byte of length, so a single slice is 0..255 bytes — long enough to reach
 * WS_TOPIC_MAX_LEVELS (128) with single-char levels, and to overrun a segment
 * buffer if one existed. */
static size_t read_slice(const uint8_t *data, size_t size, size_t *pos,
                         const char **out)
{
    if (*pos >= size) {
        *out = NULL;
        return 0;
    }

    size_t len = data[(*pos)++];

    const size_t avail = size - *pos;
    if (len > avail) {
        len = avail;
    }

    *out = (const char *)(data + *pos);
    *pos += len;
    return len;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    ws_topic_tree_t *const tree = ws_topic_tree_create(NULL);
    if (tree == NULL) {
        return 0;
    }

    /* The hub hands out ids in production; here the index does, non-zero as the
     * tree requires, so exclude-self can actually exclude. The last one drops its
     * own subscriptions from inside the walk. */
    ws_fuzz_receiver_t receivers[WS_TOPIC_FUZZ_RECEIVERS] = { 0 };
    for (uint32_t i = 0; i < WS_TOPIC_FUZZ_RECEIVERS; i++) {
        receivers[i].tree    = tree;
        receivers[i].base.id = (uint64_t)i + 1;
        receivers[i].base.ops = i == WS_TOPIC_FUZZ_RECEIVERS - 1
            ? &ws_fuzz_ops_closing
            : ((i % 2) == 0 ? &ws_fuzz_ops_ok : &ws_fuzz_ops_refused);
    }

    size_t pos = 0;

    while (pos < size) {
        const uint8_t op = data[pos++];

        const char *slice;
        const size_t slice_len = read_slice(data, size, &pos, &slice);
        if (slice == NULL) {
            break;
        }

        ws_topic_receiver_t *const receiver =
            &receivers[(op >> 3) & (WS_TOPIC_FUZZ_RECEIVERS - 1)].base;

        /* Every slice hits the pure parsers, valid or not — the string paths get
         * covered even when the command below leaves the tree untouched. */
        (void)ws_topic_is_valid_filter(slice, slice_len);
        (void)ws_topic_is_valid_name(slice, slice_len);
        (void)ws_topic_interest_prefix(slice, slice_len);

        ws_topic_prefixes_t prefixes;
        (void)ws_topic_prefixes(slice, slice_len, &prefixes);

        switch (op & 0x7) {
            case 0: {   /* subscribe (filter) */
                zend_string *const filter = zend_string_init(slice, slice_len, 0);
                (void)ws_topic_subscribe(tree, receiver, filter, WS_TOPIC_FUZZ_MAX_SUBS);
                zend_string_release(filter);
                break;
            }

            case 1: {   /* unsubscribe one filter */
                zend_string *const filter = zend_string_init(slice, slice_len, 0);
                (void)ws_topic_unsubscribe(tree, receiver, filter);
                zend_string_release(filter);
                break;
            }

            case 2: {   /* publish + count (concrete name) */
                void *shared = NULL;   /* the walk's one-message scratch */

                (void)ws_topic_publish(tree, slice, slice_len, "x", 1, false,
                                       receiver->id, &shared);
                (void)ws_topic_count(tree, slice, slice_len);
                break;
            }

            case 3:     /* drop every subscription this receiver holds */
                ws_topic_unsubscribe_all(tree, receiver);
                break;

            case 4: {   /* subscribe with no quota, as a server receiver does */
                zend_string *const filter = zend_string_init(slice, slice_len, 0);
                (void)ws_topic_subscribe(tree, receiver, filter, 0);
                zend_string_release(filter);
                break;
            }

            case 5: {   /* publish that excludes nobody */
                void *shared = NULL;

                (void)ws_topic_publish(tree, slice, slice_len, "x", 1, true, 0, &shared);
                break;
            }

            default:    /* the parsers above still ran; leave the tree alone */
                break;
        }
    }

    /* Exercise the topic listing, then leave through the destroy path a closing
     * receiver takes: unsubscribe-all detaches every node under the tree before
     * the tree itself is freed (a leak or a dangling node shows up here). */
    for (uint32_t i = 0; i < WS_TOPIC_FUZZ_RECEIVERS; i++) {
        zval list;
        ZVAL_UNDEF(&list);
        ws_topic_list(&receivers[i].base, &list);
        zval_ptr_dtor(&list);

        ws_topic_unsubscribe_all(tree, &receivers[i].base);
    }

    ws_topic_tree_free(tree);

    return 0;
}
