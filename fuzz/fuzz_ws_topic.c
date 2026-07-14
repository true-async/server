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
 * The bytes are read as a little command program over a small pool of sessions:
 * subscribe / unsubscribe / unsubscribe-all / publish / count, each on a
 * session the input selects. That builds, mutates, prunes and walks a real
 * tree, so ASAN/UBSan see the growth, the tombstone compaction, the empty-node
 * prune cascade and the recursive matcher — not just the string parser. The
 * pure validators run on every slice regardless of the command, so malformed
 * topics reach the parser even when the tree stays empty.
 *
 * No hub, no threads: the tree is per-worker and single-threaded by design, and
 * the two hub-interest calls plus the delivery send are stubbed below (the
 * cross-thread fan-out lives in ws_hub.c and is out of this TU's scope).
 */

#include "harness_common.h"
#include "websocket/ws_topic_tree.h"
#include "websocket/ws_session.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* ws_topic_tree.c publishes each subscribe/unsubscribe into the owning worker's
 * cross-worker interest filter; that filter lives in ws_hub.c, which is not in
 * this TU. A no-op keeps the tree self-contained — the prefix it hands us is
 * already exercised directly via ws_topic_interest_prefix below. */
void ws_hub_interest_add(struct ws_hub_s *hub, const char *filter, size_t prefix_len)
{
    (void)hub; (void)filter; (void)prefix_len;
}

void ws_hub_interest_remove(struct ws_hub_s *hub, const char *filter, size_t prefix_len)
{
    (void)hub; (void)filter; (void)prefix_len;
}

/* Delivery target. The matcher calls this for every session a publish reaches;
 * returning true just makes the hit count non-zero. The real send (wslay + the
 * transport) is fuzzed by fuzz_ws_frame — here it must not tear the session
 * down, so the mid-walk tombstone path stays deterministic. */
bool ws_session_try_send(ws_session_t *session, const char *data, size_t len,
                         bool binary)
{
    (void)session; (void)data; (void)len; (void)binary;
    return true;
}

#define WS_TOPIC_FUZZ_SESSIONS 8u
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

    /* Zeroed sessions: the tree only reads topics / ws_id / topic_mark. A
     * distinct non-zero ws_id per session lets exclude-self actually exclude. */
    ws_session_t *sessions[WS_TOPIC_FUZZ_SESSIONS];
    for (uint32_t i = 0; i < WS_TOPIC_FUZZ_SESSIONS; i++) {
        sessions[i] = ecalloc(1, sizeof(*sessions[i]));
        sessions[i]->ws_id = (uint64_t)i + 1;
    }

    size_t pos = 0;

    while (pos < size) {
        const uint8_t op = data[pos++];

        const char *slice;
        const size_t slice_len = read_slice(data, size, &pos, &slice);
        if (slice == NULL) {
            break;
        }

        ws_session_t *const session = sessions[(op >> 2) & (WS_TOPIC_FUZZ_SESSIONS - 1)];

        /* Every slice hits the pure parsers, valid or not — the string paths get
         * covered even when the command below leaves the tree untouched. */
        (void)ws_topic_is_valid_filter(slice, slice_len);
        (void)ws_topic_is_valid_name(slice, slice_len);
        (void)ws_topic_interest_prefix(slice, slice_len);

        ws_topic_prefixes_t prefixes;
        (void)ws_topic_prefixes(slice, slice_len, &prefixes);

        switch (op & 0x3) {
            case 0: {   /* subscribe (filter) */
                zend_string *const filter = zend_string_init(slice, slice_len, 0);
                (void)ws_topic_subscribe(tree, session, filter, WS_TOPIC_FUZZ_MAX_SUBS);
                zend_string_release(filter);
                break;
            }

            case 1: {   /* unsubscribe one filter */
                zend_string *const filter = zend_string_init(slice, slice_len, 0);
                (void)ws_topic_unsubscribe(tree, session, filter);
                zend_string_release(filter);
                break;
            }

            case 2:     /* publish + count (concrete name) */
                (void)ws_topic_publish(tree, slice, slice_len, "x", 1, false,
                                       session->ws_id);
                (void)ws_topic_count(tree, slice, slice_len);
                break;

            case 3:     /* drop every subscription this session holds */
                ws_topic_unsubscribe_all(tree, session);
                break;
        }
    }

    /* Exercise the topic listing, then leave through the destroy path each
     * session takes: unsubscribe-all detaches every node under the tree before
     * the tree itself is freed (a leak or a dangling node shows up here). */
    for (uint32_t i = 0; i < WS_TOPIC_FUZZ_SESSIONS; i++) {
        zval list;
        ZVAL_UNDEF(&list);
        ws_topic_list(sessions[i], &list);
        zval_ptr_dtor(&list);

        ws_topic_unsubscribe_all(tree, sessions[i]);
        efree(sessions[i]);
    }

    ws_topic_tree_free(tree);
    return 0;
}
