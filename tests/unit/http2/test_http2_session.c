/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/**
 * @file test_http2_session.c
 * @brief Step 2 — http2_session_t lifecycle + SETTINGS exchange.
 *
 * Offline drive: we mint a server-side http2_session_t, feed it the
 * client preface + a client-side SETTINGS frame, then drain what our
 * session wrote back and parse the wire bytes to verify that the
 * initial SETTINGS table and the SETTINGS-ACK both land correctly.
 *
 * Flood defence (§4.3) is exercised by feeding 33 client SETTINGS
 * frames in a row — nghttp2's max_settings=32 cap should close the
 * connection with GOAWAY (ENHANCE_YOUR_CALM). We observe this via
 * http2_session_want_read returning false + feed returning -1.
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <nghttp2/nghttp2.h>

#include "common/php_sapi_test.h"
#include "http1/http_parser.h"
#include "http2/http2_session.h"
#include "http2/http2_stream.h"

/* Linker stubs — see test_http2_strategy.c for rationale. */
http1_parser_t *parser_pool_acquire(void) { return NULL; }
void            parser_pool_return(http1_parser_t *p) { (void)p; }

/* Referenced by http2_session.c's RST_STREAM cancel path (Step 7.2a).
 * The code path short-circuits on NULL, so the offline tests never
 * actually construct an HttpException — they just need the symbol to
 * resolve. */
zend_class_entry *http_exception_ce = NULL;

/* -------------------------------------------------------------------------
 * Wire-byte helpers: build client frames + walk server-emitted frames
 * without pulling in a second nghttp2_session just for decoding.
 * ------------------------------------------------------------------------- */

#define H2_PREFACE      "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"
#define H2_PREFACE_LEN  24

/* Write a frame header into @p dst. RFC 9113 §4.1 — 24-bit length,
 * 8-bit type, 8-bit flags, 31-bit stream id (MSB reserved = 0). */
static void write_frame_header(uint8_t *const dst,
                               const uint32_t length,
                               const uint8_t  type,
                               const uint8_t  flags,
                               const uint32_t stream_id)
{
    dst[0] = (uint8_t)((length >> 16) & 0xff);
    dst[1] = (uint8_t)((length >> 8)  & 0xff);
    dst[2] = (uint8_t)( length        & 0xff);
    dst[3] = type;
    dst[4] = flags;
    dst[5] = (uint8_t)((stream_id >> 24) & 0x7f);  /* R bit cleared */
    dst[6] = (uint8_t)((stream_id >> 16) & 0xff);
    dst[7] = (uint8_t)((stream_id >>  8) & 0xff);
    dst[8] = (uint8_t)( stream_id        & 0xff);
}

/* Emit a client-side SETTINGS frame (without values — empty SETTINGS
 * is legal and exercises the ACK path cleanly). */
static size_t build_empty_settings(uint8_t *const dst, const uint8_t flags)
{
    write_frame_header(dst, 0, NGHTTP2_SETTINGS, flags, 0);
    return 9;
}

/* Iterate over the bytes we drained from the server session, yielding
 * one frame at a time. Uses the same wire format as write_frame_header. */
typedef struct {
    const uint8_t *p;
    size_t         left;
} frame_iter_t;

typedef struct {
    uint32_t length;
    uint8_t  type;
    uint8_t  flags;
    uint32_t stream_id;
    const uint8_t *payload;
} frame_view_t;

static bool frame_iter_next(frame_iter_t *const it, frame_view_t *const out)
{
    if (it->left < 9) { return false; }
    out->length = ((uint32_t)it->p[0] << 16) |
                  ((uint32_t)it->p[1] << 8)  |
                  ((uint32_t)it->p[2]);
    out->type   = it->p[3];
    out->flags  = it->p[4];
    out->stream_id =
        ((uint32_t)(it->p[5] & 0x7f) << 24) |
        ((uint32_t)it->p[6] << 16) |
        ((uint32_t)it->p[7] <<  8) |
        ((uint32_t)it->p[8]);

    if (it->left < 9 + out->length) { return false; }
    out->payload = it->p + 9;
    it->p    += 9 + out->length;
    it->left -= 9 + out->length;
    return true;
}

/* -------------------------------------------------------------------------
 * Tests
 * ------------------------------------------------------------------------- */

/* Session create + destroy: both halves must complete, nothing leaks
 * under ASan. On fresh session, want_write should already be true
 * because we've queued initial SETTINGS. */
static void test_session_lifecycle(void **state)
{
    (void)state;

    http2_session_t *const session = http2_session_new(NULL, NULL, NULL);
    assert_non_null(session);

    assert_true(http2_session_want_write(session));

    http2_session_free(session);
    /* Double-free / NULL-free safety — matches the documented contract. */
    http2_session_free(NULL);
}

/* Feed the client preface + empty SETTINGS, drain what the server
 * emits, verify: (a) our SETTINGS frame appears with the six §4.5
 * values, (b) the SETTINGS-ACK for the client's SETTINGS also
 * appears. Tests the full Step-2 handshake. */
static void test_settings_exchange(void **state)
{
    (void)state;

    http2_session_t *const session = http2_session_new(NULL, NULL, NULL);
    assert_non_null(session);

    uint8_t in[H2_PREFACE_LEN + 9];
    memcpy(in, H2_PREFACE, H2_PREFACE_LEN);
    const size_t client_settings_len = build_empty_settings(
        in + H2_PREFACE_LEN, NGHTTP2_FLAG_NONE);

    size_t consumed = 0;
    assert_int_equal(
        http2_session_feed(session, (const char *)in,
                           H2_PREFACE_LEN + client_settings_len, &consumed),
        0);
    assert_int_equal(consumed, H2_PREFACE_LEN + client_settings_len);

    /* Drain all pending output. 1 KiB is plenty for SETTINGS + ACK. */
    uint8_t out[1024];
    const ssize_t drained = http2_session_drain(
        session, (char *)out, sizeof(out));
    assert_true(drained > 0);

    frame_iter_t it = { .p = out, .left = (size_t)drained };
    frame_view_t f;

    /* Frame 1: our SETTINGS. Must carry the six §4.5 entries, 6 bytes
     * each (16-bit id + 32-bit value), stream id 0, no ACK flag. */
    assert_true(frame_iter_next(&it, &f));
    assert_int_equal(f.type, NGHTTP2_SETTINGS);
    assert_int_equal(f.flags & NGHTTP2_FLAG_ACK, 0);
    assert_int_equal(f.stream_id, 0);
    assert_int_equal(f.length, 6 * 6);

    /* Walk the six entries and prove the critical ones. */
    bool saw_push_disabled = false;
    bool saw_max_streams = false;
    bool saw_initial_window = false;
    bool saw_header_list = false;

    for (size_t i = 0; i < 6; i++) {
        const uint8_t *const e = f.payload + i * 6;
        const uint16_t id = (uint16_t)((e[0] << 8) | e[1]);
        const uint32_t v  = ((uint32_t)e[2] << 24) | ((uint32_t)e[3] << 16) |
                            ((uint32_t)e[4] << 8)  | ((uint32_t)e[5]);
        switch (id) {
            case NGHTTP2_SETTINGS_ENABLE_PUSH:
                saw_push_disabled = (v == 0);
                break;
            case NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS:
                saw_max_streams = (v == HTTP2_SETTINGS_MAX_CONCURRENT);
                break;
            case NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE:
                saw_initial_window = (v == HTTP2_SETTINGS_INITIAL_WINDOW);
                break;
            case NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE:
                saw_header_list = (v == HTTP2_SETTINGS_MAX_HEADER_LIST);
                break;
            default: break;
        }
    }
    assert_true(saw_push_disabled);
    assert_true(saw_max_streams);
    assert_true(saw_initial_window);
    assert_true(saw_header_list);

    /* Frame 2+: WINDOW_UPDATE (for the connection window bump to 1 MiB)
     * and/or SETTINGS-ACK. Order is nghttp2's choice; we just assert
     * the ACK appears somewhere in the remaining stream. */
    bool saw_ack = false;
    while (frame_iter_next(&it, &f)) {
        if (f.type == NGHTTP2_SETTINGS &&
            (f.flags & NGHTTP2_FLAG_ACK) != 0 &&
            f.stream_id == 0 &&
            f.length == 0) {
            saw_ack = true;
        }
    }
    assert_true(saw_ack);

    http2_session_free(session);
}

/* Drain semantics under a too-small output buffer: must honour
 * mem_send's "no partial consumption" contract — the second drain
 * picks up exactly where the first one stopped, no data loss. */
static void test_drain_resumes_across_calls(void **state)
{
    (void)state;

    http2_session_t *const session = http2_session_new(NULL, NULL, NULL);
    assert_non_null(session);

    /* Drain 8 bytes at a time — smaller than any real frame header. */
    uint8_t assembled[2048];
    size_t  assembled_len = 0;

    uint8_t chunk[8];
    for (;;) {
        const ssize_t n = http2_session_drain(
            session, (char *)chunk, sizeof(chunk));
        assert_true(n >= 0);
        if (n == 0) { break; }
        assert_true(assembled_len + (size_t)n <= sizeof(assembled));
        memcpy(assembled + assembled_len, chunk, (size_t)n);
        assembled_len += (size_t)n;
    }

    /* Reassembled stream must parse as a valid SETTINGS frame. */
    frame_iter_t it = { .p = assembled, .left = assembled_len };
    frame_view_t f;
    assert_true(frame_iter_next(&it, &f));
    assert_int_equal(f.type, NGHTTP2_SETTINGS);
    assert_int_equal(f.length, 6 * 6);

    http2_session_free(session);
}

/* Plan §4.3 — feeding more SETTINGS than nghttp2's cap must cause a
 * connection-level error (-1 from feed). Exact error code comes out
 * inside the GOAWAY that nghttp2 now has queued; we don't decode it
 * here, we just assert the transport failed as expected. */
static void test_settings_flood_rejected(void **state)
{
    (void)state;

    http2_session_t *const session = http2_session_new(NULL, NULL, NULL);
    assert_non_null(session);

    /* Preface first. */
    size_t consumed = 0;
    assert_int_equal(
        http2_session_feed(session, H2_PREFACE, H2_PREFACE_LEN, &consumed),
        0);
    assert_int_equal(consumed, H2_PREFACE_LEN);

    /* Fire empty SETTINGS frames without draining outbound. Each one
     * makes nghttp2 queue a SETTINGS-ACK; once the outbound-ACK queue
     * exceeds HTTP2_OPT_MAX_OUTBOUND_ACK=64, nghttp2 treats it as
     * flooding and returns a connection-level error. Cap the loop at
     * 4 × the limit (256) as a generous upper bound — hitting the
     * cap is our real success criterion; we only bound the loop to
     * avoid an infinite run on a flood-protection regression. */
    uint8_t settings_frame[9];
    (void)build_empty_settings(settings_frame, NGHTTP2_FLAG_NONE);

    bool saw_error = false;
    const int cap_iters = HTTP2_OPT_MAX_OUTBOUND_ACK * 4;
    for (int i = 0; i < cap_iters; i++) {
        const int rc = http2_session_feed(
            session, (const char *)settings_frame,
            sizeof(settings_frame), &consumed);
        if (rc < 0) {
            saw_error = true;
            break;
        }
    }
    assert_true(saw_error);

    http2_session_free(session);
}

/* -------------------------------------------------------------------------
 * Step 3 — HEADERS decode + stream dispatch
 *
 * We mint a client-side nghttp2_session, submit a request on it, and
 * shuttle the wire bytes into our server-side http2_session_t. The
 * server's on_request_ready hook captures the dispatched request so
 * we can inspect method/uri/headers. No sockets, no handler coroutines
 * — the read path is what we're exercising here.
 * ------------------------------------------------------------------------- */

typedef struct {
    http_request_t *last_request;
    uint32_t        last_stream_id;
    int             dispatch_count;
} dispatch_capture_t;

static void capture_on_request_ready(http_request_t *const request,
                                     const uint32_t stream_id,
                                     void *const user_data)
{
    dispatch_capture_t *const cap = (dispatch_capture_t *)user_data;
    cap->last_request   = request;
    cap->last_stream_id = stream_id;
    cap->dispatch_count++;
}

/* A no-op nghttp2 client session is enough to encode HEADERS frames
 * for us. send_callback pushes into a test-owned ring; recv is fed
 * via mem_recv once we've shuttled the server's output in. */
typedef struct {
    uint8_t buf[16384];
    size_t  len;
} wire_ring_t;

/* Session-wide user_data — wire_ring + optional response capture.
 * send_cb uses ->wire; response-path callbacks use ->cap (may be NULL
 * for Step 3 tests that only exercise the request path). */
typedef struct {
    wire_ring_t        *wire;
    struct response_capture_t *cap;
} client_ctx_t;

static ssize_t client_send_cb(nghttp2_session *session,
                              const uint8_t *data, size_t length,
                              int flags, void *user_data)
{
    (void)session; (void)flags;
    client_ctx_t *const ctx = (client_ctx_t *)user_data;
    wire_ring_t *const out = ctx->wire;
    /* Return WOULDBLOCK instead of asserting when wire is saturated —
     * large-body tests rely on the submit_post_and_feed drain loop to
     * refill wire between session_send passes. */
    if (out->len + length > sizeof(out->buf)) {
        return NGHTTP2_ERR_WOULDBLOCK;
    }
    memcpy(out->buf + out->len, data, length);
    out->len += length;
    return (ssize_t)length;
}

static nghttp2_session *make_client_session(client_ctx_t *ctx)
{
    nghttp2_session_callbacks *cbs = NULL;
    assert_int_equal(nghttp2_session_callbacks_new(&cbs), 0);
    nghttp2_session_callbacks_set_send_callback(cbs, client_send_cb);

    nghttp2_session *client = NULL;
    assert_int_equal(nghttp2_session_client_new(&client, cbs, ctx), 0);
    nghttp2_session_callbacks_del(cbs);

    /* Client must send the connection preface + SETTINGS before any
     * request — nghttp2 does the bookkeeping, we just flush once. */
    static const nghttp2_settings_entry iv[] = {
        { NGHTTP2_SETTINGS_ENABLE_PUSH, 0 },
    };
    assert_int_equal(
        nghttp2_submit_settings(client, NGHTTP2_FLAG_NONE, iv,
                                sizeof(iv) / sizeof(iv[0])),
        0);
    assert_true(nghttp2_session_send(client) == 0);
    return client;
}

/* Forward decl — response capture struct used in Step 4 tests. */
typedef struct response_capture_t response_capture_t;

/* Push all of @p wire.buf into our server-side session. */
static void push_to_server(http2_session_t *const server,
                           wire_ring_t *const wire)
{
    size_t offset = 0;
    while (offset < wire->len) {
        size_t consumed = 0;
        const int rc = http2_session_feed(
            server, (const char *)wire->buf + offset,
            wire->len - offset, &consumed);
        assert_int_equal(rc, 0);
        offset += consumed;
        if (consumed == 0) { break; }
    }
    wire->len = 0;  /* reset ring */
}

/* Canonical GET request: method/path/scheme/authority + one app header.
 * Verifies pseudo-header mapping to method/uri, :authority → Host, and
 * regular header insertion. */
static void test_headers_decode(void **state)
{
    (void)state;

    dispatch_capture_t cap = {0};
    http2_session_t *const server = http2_session_new(
        NULL, capture_on_request_ready, &cap);
    assert_non_null(server);

    wire_ring_t wire = {0};
    client_ctx_t ctx = { .wire = &wire, .cap = NULL };
    nghttp2_session *const client = make_client_session(&ctx);

    const nghttp2_nv req_nv[] = {
        { (uint8_t *)":method",    (uint8_t *)"GET",           7, 3, NGHTTP2_NV_FLAG_NONE },
        { (uint8_t *)":path",      (uint8_t *)"/hello",        5, 6, NGHTTP2_NV_FLAG_NONE },
        { (uint8_t *)":scheme",    (uint8_t *)"https",         7, 5, NGHTTP2_NV_FLAG_NONE },
        { (uint8_t *)":authority", (uint8_t *)"api.example",  10,11, NGHTTP2_NV_FLAG_NONE },
        { (uint8_t *)"accept",     (uint8_t *)"application/json", 6, 16, NGHTTP2_NV_FLAG_NONE },
    };
    const int32_t stream_id = nghttp2_submit_request(
        client, NULL, req_nv, sizeof(req_nv) / sizeof(req_nv[0]), NULL, NULL);
    assert_true(stream_id > 0);
    assert_int_equal(nghttp2_session_send(client), 0);

    push_to_server(server, &wire);

    /* Dispatch must have fired exactly once with pseudo-headers
     * mapped and the regular `accept` header attached. */
    assert_int_equal(cap.dispatch_count, 1);
    assert_int_equal((int)cap.last_stream_id, stream_id);

    http_request_t *const req = cap.last_request;
    assert_non_null(req);
    assert_non_null(req->method);
    assert_int_equal((int)ZSTR_LEN(req->method), 3);
    assert_memory_equal(ZSTR_VAL(req->method), "GET", 3);

    assert_non_null(req->uri);
    assert_int_equal((int)ZSTR_LEN(req->uri), 6);
    assert_memory_equal(ZSTR_VAL(req->uri), "/hello", 6);

    assert_non_null(req->headers);

    /* :authority must have been mapped to Host per RFC 9113 §8.3.1. */
    zval *const host_zv = zend_hash_str_find(req->headers, "host", 4);
    assert_non_null(host_zv);
    assert_int_equal(Z_TYPE_P(host_zv), IS_STRING);
    assert_int_equal((int)Z_STRLEN_P(host_zv), 11);
    assert_memory_equal(Z_STRVAL_P(host_zv), "api.example", 11);

    /* Regular header survived the round-trip. */
    zval *const accept_zv = zend_hash_str_find(req->headers, "accept", 6);
    assert_non_null(accept_zv);
    assert_int_equal(Z_TYPE_P(accept_zv), IS_STRING);
    assert_int_equal((int)Z_STRLEN_P(accept_zv), 16);
    assert_memory_equal(Z_STRVAL_P(accept_zv), "application/json", 16);

    nghttp2_session_del(client);
    http2_session_free(server);
}

/* Two concurrent streams on the same session — verifies the stream
 * table + dispatch callback handle multiplexed IDs correctly. */
static void test_concurrent_streams(void **state)
{
    (void)state;

    dispatch_capture_t cap = {0};
    http2_session_t *const server = http2_session_new(
        NULL, capture_on_request_ready, &cap);
    assert_non_null(server);

    wire_ring_t wire = {0};
    client_ctx_t ctx = { .wire = &wire, .cap = NULL };
    nghttp2_session *const client = make_client_session(&ctx);

    const nghttp2_nv nv1[] = {
        { (uint8_t *)":method",    (uint8_t *)"GET",     7, 3, NGHTTP2_NV_FLAG_NONE },
        { (uint8_t *)":path",      (uint8_t *)"/a",      5, 2, NGHTTP2_NV_FLAG_NONE },
        { (uint8_t *)":scheme",    (uint8_t *)"https",   7, 5, NGHTTP2_NV_FLAG_NONE },
        { (uint8_t *)":authority", (uint8_t *)"h",      10, 1, NGHTTP2_NV_FLAG_NONE },
    };
    const nghttp2_nv nv2[] = {
        { (uint8_t *)":method",    (uint8_t *)"POST",    7, 4, NGHTTP2_NV_FLAG_NONE },
        { (uint8_t *)":path",      (uint8_t *)"/b",      5, 2, NGHTTP2_NV_FLAG_NONE },
        { (uint8_t *)":scheme",    (uint8_t *)"https",   7, 5, NGHTTP2_NV_FLAG_NONE },
        { (uint8_t *)":authority", (uint8_t *)"h",      10, 1, NGHTTP2_NV_FLAG_NONE },
    };
    const int32_t sid1 = nghttp2_submit_request(client, NULL, nv1, 4, NULL, NULL);
    const int32_t sid2 = nghttp2_submit_request(client, NULL, nv2, 4, NULL, NULL);
    assert_true(sid1 > 0 && sid2 > 0 && sid1 != sid2);
    assert_int_equal(nghttp2_session_send(client), 0);

    push_to_server(server, &wire);

    /* Both dispatched, both streams live in the table. */
    assert_int_equal(cap.dispatch_count, 2);

    http2_stream_t *const s1 = http2_session_find_stream(server, (uint32_t)sid1);
    http2_stream_t *const s2 = http2_session_find_stream(server, (uint32_t)sid2);
    assert_non_null(s1);
    assert_non_null(s2);
    assert_ptr_not_equal(s1, s2);
    assert_non_null(s1->request);
    assert_non_null(s2->request);

    /* Each stream's request saw its own :path. */
    assert_memory_equal(ZSTR_VAL(s1->request->uri), "/a", 2);
    assert_memory_equal(ZSTR_VAL(s2->request->uri), "/b", 2);
    assert_memory_equal(ZSTR_VAL(s1->request->method), "GET",  3);
    assert_memory_equal(ZSTR_VAL(s2->request->method), "POST", 4);

    nghttp2_session_del(client);
    http2_session_free(server);
}

/* Plan §4.1 — a header list bigger than HTTP2_SETTINGS_MAX_HEADER_LIST
 * must not crash the connection. nghttp2 enforces the advertised cap
 * and returns a stream-level error; our belt-and-braces accumulator
 * (headers_total_bytes on the stream) catches any overrun nghttp2
 * itself missed. Outcome: no on_request_ready dispatch, other streams
 * unaffected. */
static void test_headers_oversize_rejected(void **state)
{
    (void)state;

    dispatch_capture_t cap = {0};
    http2_session_t *const server = http2_session_new(
        NULL, capture_on_request_ready, &cap);
    assert_non_null(server);

    wire_ring_t wire = {0};
    client_ctx_t ctx = { .wire = &wire, .cap = NULL };
    nghttp2_session *const client = make_client_session(&ctx);

    /* Build a single 72 KiB header value — over the 64 KiB advertised
     * cap. One entry is enough; the aggregate count includes per-entry
     * HPACK overhead (RFC 7541 §4.1 = 32 bytes/entry). */
    static char fat_value[72 * 1024];
    for (size_t i = 0; i < sizeof(fat_value); i++) {
        fat_value[i] = 'x';
    }

    const nghttp2_nv nv[] = {
        { (uint8_t *)":method",    (uint8_t *)"GET",           7, 3, NGHTTP2_NV_FLAG_NONE },
        { (uint8_t *)":path",      (uint8_t *)"/",             5, 1, NGHTTP2_NV_FLAG_NONE },
        { (uint8_t *)":scheme",    (uint8_t *)"https",         7, 5, NGHTTP2_NV_FLAG_NONE },
        { (uint8_t *)":authority", (uint8_t *)"h",            10, 1, NGHTTP2_NV_FLAG_NONE },
        { (uint8_t *)"x-big",      (uint8_t *)fat_value,       5, sizeof(fat_value),
          NGHTTP2_NV_FLAG_NO_INDEX },
    };
    (void)nghttp2_submit_request(client, NULL, nv, 5, NULL, NULL);
    assert_int_equal(nghttp2_session_send(client), 0);

    /* Server may accept bytes (they're a valid HEADERS + CONTINUATION
     * stream) but must not dispatch the oversize request. */
    size_t offset = 0;
    while (offset < wire.len) {
        size_t consumed = 0;
        const int rc = http2_session_feed(
            server, (const char *)wire.buf + offset,
            wire.len - offset, &consumed);
        if (rc < 0) { break; }  /* connection-level error is acceptable */
        offset += consumed;
        if (consumed == 0) { break; }
    }

    assert_int_equal(cap.dispatch_count, 0);

    nghttp2_session_del(client);
    http2_session_free(server);
}

/* -------------------------------------------------------------------------
 * Step 4 — Response submission (HEADERS + DATA + END_STREAM)
 *
 * For write-path tests we graduate to a full dual-session setup:
 * client-side nghttp2_session with on_header / on_data_chunk / on_frame
 * callbacks capturing the response. Server submits via our API, drains
 * into a wire buffer, then we push that wire buffer back into the
 * client. Mirrors what curl / grpc-go would do at the other end.
 * ------------------------------------------------------------------------- */

struct response_capture_t {
    int     status_code;
    char    body[64 * 1024];
    size_t  body_len;
    bool    response_complete;     /* END_STREAM on a response-category frame */

    /* One common header to verify HPACK encode/decode survives the
     * round trip. We check content-type specifically because that's
     * the canonical REST indicator. */
    char    content_type[128];
    size_t  content_type_len;

    /* gRPC-style trailers (Step 5b): the client distinguishes a
     * trailer HEADERS frame from the initial response HEADERS by
     * category (HCAT_HEADERS vs HCAT_RESPONSE). We only set this on
     * trailer-category frames. */
    char    grpc_status[8];
    size_t  grpc_status_len;
    char    grpc_message[64];
    size_t  grpc_message_len;
    bool    saw_trailers;

    /* Step 7.2b — GOAWAY capture. Set when the server sends GOAWAY. */
    bool     saw_goaway;
    uint32_t goaway_last_stream_id;
    uint32_t goaway_error_code;

    /* Step 7.2c — PING ACK capture. Set when the server bounces back
     * a PING with the ACK flag (i.e. in reply to our client-side PING).
     * We only record the flag here; the RTT lives on the session that
     * originally sent the PING (the server), fetched via
     * http2_session_last_ping_rtt_ns. */
    bool     saw_ping_ack;
};

static int client_on_header(nghttp2_session *session,
                            const nghttp2_frame *frame,
                            const uint8_t *name, size_t namelen,
                            const uint8_t *value, size_t valuelen,
                            uint8_t flags, void *user_data)
{
    (void)session; (void)flags;
    if (frame->hd.type != NGHTTP2_HEADERS) {
        return 0;
    }
    response_capture_t *const cap = ((client_ctx_t *)user_data)->cap;
    if (cap == NULL) { return 0; }

    /* HCAT_HEADERS on a client-side nghttp2 session = trailer frame
     * (arriving AFTER the initial response HEADERS). HCAT_RESPONSE
     * is the initial HEADERS block. Distinguishing the two lets us
     * capture `grpc-status` / `grpc-message` without accidentally
     * overwriting the :status-frame values. */
    const bool is_trailer = (frame->headers.cat == NGHTTP2_HCAT_HEADERS);
    if (is_trailer) {
        cap->saw_trailers = true;
    }

    if (namelen == 7 && memcmp(name, ":status", 7) == 0) {
        /* nghttp2 already validated 3-digit numeric. */
        cap->status_code = (value[0] - '0') * 100 +
                           (value[1] - '0') * 10  +
                           (value[2] - '0');
    } else if (namelen == 12 &&
               memcmp(name, "content-type", 12) == 0) {
        cap->content_type_len = valuelen < sizeof(cap->content_type) - 1
            ? valuelen : sizeof(cap->content_type) - 1;
        memcpy(cap->content_type, value, cap->content_type_len);
        cap->content_type[cap->content_type_len] = '\0';
    } else if (is_trailer && namelen == 11 &&
               memcmp(name, "grpc-status", 11) == 0) {
        cap->grpc_status_len = valuelen < sizeof(cap->grpc_status) - 1
            ? valuelen : sizeof(cap->grpc_status) - 1;
        memcpy(cap->grpc_status, value, cap->grpc_status_len);
        cap->grpc_status[cap->grpc_status_len] = '\0';
    } else if (is_trailer && namelen == 12 &&
               memcmp(name, "grpc-message", 12) == 0) {
        cap->grpc_message_len = valuelen < sizeof(cap->grpc_message) - 1
            ? valuelen : sizeof(cap->grpc_message) - 1;
        memcpy(cap->grpc_message, value, cap->grpc_message_len);
        cap->grpc_message[cap->grpc_message_len] = '\0';
    }
    return 0;
}

static int client_on_data_chunk(nghttp2_session *session,
                                uint8_t flags, int32_t stream_id,
                                const uint8_t *data, size_t len,
                                void *user_data)
{
    (void)session; (void)flags; (void)stream_id;
    response_capture_t *const cap = ((client_ctx_t *)user_data)->cap;
    if (cap == NULL) { return 0; }
    assert_true(cap->body_len + len <= sizeof(cap->body));
    memcpy(cap->body + cap->body_len, data, len);
    cap->body_len += len;
    return 0;
}

static int client_on_frame_recv(nghttp2_session *session,
                                const nghttp2_frame *frame,
                                void *user_data)
{
    (void)session;
    response_capture_t *const cap = ((client_ctx_t *)user_data)->cap;
    if (cap == NULL) { return 0; }
    if ((frame->hd.flags & NGHTTP2_FLAG_END_STREAM) != 0 &&
        (frame->hd.type == NGHTTP2_DATA ||
         frame->hd.type == NGHTTP2_HEADERS)) {
        cap->response_complete = true;
    }
    if (frame->hd.type == NGHTTP2_GOAWAY) {
        cap->saw_goaway            = true;
        cap->goaway_last_stream_id = (uint32_t)frame->goaway.last_stream_id;
        cap->goaway_error_code     = frame->goaway.error_code;
    }
    if (frame->hd.type == NGHTTP2_PING &&
        (frame->hd.flags & NGHTTP2_FLAG_ACK) != 0) {
        cap->saw_ping_ack = true;
    }
    return 0;
}

/* Client-side session with full response capture. Single session-wide
 * user_data (client_ctx_t) routes send-side writes to @p ctx->wire and
 * receive-side frames to @p ctx->cap. */
static nghttp2_session *make_client_session_with_capture(client_ctx_t *ctx)
{
    nghttp2_session_callbacks *cbs = NULL;
    assert_int_equal(nghttp2_session_callbacks_new(&cbs), 0);
    nghttp2_session_callbacks_set_send_callback       (cbs, client_send_cb);
    nghttp2_session_callbacks_set_on_header_callback  (cbs, client_on_header);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cbs, client_on_data_chunk);
    nghttp2_session_callbacks_set_on_frame_recv_callback(cbs, client_on_frame_recv);

    nghttp2_session *client = NULL;
    assert_int_equal(nghttp2_session_client_new(&client, cbs, ctx), 0);
    nghttp2_session_callbacks_del(cbs);

    static const nghttp2_settings_entry iv[] = {
        { NGHTTP2_SETTINGS_ENABLE_PUSH, 0 },
    };
    assert_int_equal(
        nghttp2_submit_settings(client, NGHTTP2_FLAG_NONE, iv,
                                sizeof(iv) / sizeof(iv[0])),
        0);
    assert_true(nghttp2_session_send(client) == 0);
    return client;
}

/* Drain everything the server has queued and feed it back into @p client. */
static void drain_into_client(http2_session_t *const server,
                              nghttp2_session *const client)
{
    uint8_t wire[16384];
    for (;;) {
        const ssize_t n = http2_session_drain(server, (char *)wire, sizeof(wire));
        assert_true(n >= 0);
        if (n == 0) { break; }
        assert_true(nghttp2_session_mem_recv(client, wire, (size_t)n) == (ssize_t)n);
    }
}

/* Helper: create server + client, preface exchange + client submits
 * one GET request, server dispatches, caller handles the response
 * via http2_session_submit_response. Returns the stream id assigned
 * by the client. */
static int32_t bootstrap_request(http2_session_t *server,
                                 nghttp2_session *client,
                                 wire_ring_t *wire)
{
    const nghttp2_nv nv[] = {
        { (uint8_t *)":method",    (uint8_t *)"GET",     7, 3, 0 },
        { (uint8_t *)":path",      (uint8_t *)"/",       5, 1, 0 },
        { (uint8_t *)":scheme",    (uint8_t *)"https",   7, 5, 0 },
        { (uint8_t *)":authority", (uint8_t *)"h",      10, 1, 0 },
    };
    const int32_t sid = nghttp2_submit_request(client, NULL, nv, 4, NULL, NULL);
    assert_true(sid > 0);
    assert_int_equal(nghttp2_session_send(client), 0);

    push_to_server(server, wire);   /* preface + SETTINGS + HEADERS */
    return sid;
}

/* Handler-side trampoline for submit_response plumbing — not doing
 * Strategy integration here; we just exercise the session API. The
 * dispatch callback sets a flag; the test then calls submit_response. */
typedef struct {
    bool     dispatched;
    uint32_t stream_id;
} dispatch_flag_t;

static void set_dispatch_flag(http_request_t *req, uint32_t sid, void *ud)
{
    (void)req;
    dispatch_flag_t *const f = (dispatch_flag_t *)ud;
    f->dispatched = true;
    f->stream_id  = sid;
}

/* 200 OK with a JSON body + one header. Most common REST path. */
static void test_response_small_body(void **state)
{
    (void)state;

    dispatch_flag_t disp = {0};
    http2_session_t *const server = http2_session_new(NULL, set_dispatch_flag, &disp);
    assert_non_null(server);

    wire_ring_t wire = {0};
    response_capture_t cap = {0};
    client_ctx_t ctx = { .wire = &wire, .cap = &cap };
    nghttp2_session *const client = make_client_session_with_capture(&ctx);

    const int32_t sid = bootstrap_request(server, client, &wire);
    assert_true(disp.dispatched);
    assert_int_equal((int)disp.stream_id, sid);

    const http2_header_view_t resp_hdrs[] = {
        { "content-type", 12, "application/json", 16 },
    };
    static const char body[] = "{\"ok\":true}";
    const size_t body_len = sizeof(body) - 1;

    assert_int_equal(
        http2_session_submit_response(server, (uint32_t)sid, 200,
                                      resp_hdrs, 1, body, body_len),
        0);

    drain_into_client(server, client);

    /* Client must see status 200, content-type, body, END_STREAM. */
    assert_int_equal(cap.status_code, 200);
    assert_string_equal(cap.content_type, "application/json");
    assert_int_equal((int)cap.body_len, (int)body_len);
    assert_memory_equal(cap.body, body, body_len);
    assert_true(cap.response_complete);

    nghttp2_session_del(client);
    http2_session_free(server);
}

/* 204 No Content: HEADERS with END_STREAM, no DATA frame at all. */
static void test_response_no_body(void **state)
{
    (void)state;

    dispatch_flag_t disp = {0};
    http2_session_t *const server = http2_session_new(NULL, set_dispatch_flag, &disp);
    wire_ring_t wire = {0};
    response_capture_t cap = {0};
    client_ctx_t ctx = { .wire = &wire, .cap = &cap };
    nghttp2_session *const client = make_client_session_with_capture(&ctx);

    const int32_t sid = bootstrap_request(server, client, &wire);
    assert_int_equal(
        http2_session_submit_response(server, (uint32_t)sid, 204,
                                      NULL, 0, NULL, 0),
        0);
    drain_into_client(server, client);

    assert_int_equal(cap.status_code, 204);
    assert_int_equal((int)cap.body_len, 0);
    assert_true(cap.response_complete);

    nghttp2_session_del(client);
    http2_session_free(server);
}

/* 40 KiB body — exercises multi-DATA-frame splitting (MAX_FRAME_SIZE
 * is 16 KiB) and the data_provider's iterative read calls. Byte-
 * exact round-trip is the critical invariant. */
static void test_response_multi_frame_body(void **state)
{
    (void)state;

    dispatch_flag_t disp = {0};
    http2_session_t *const server = http2_session_new(NULL, set_dispatch_flag, &disp);
    wire_ring_t wire = {0};
    response_capture_t cap = {0};
    client_ctx_t ctx = { .wire = &wire, .cap = &cap };
    nghttp2_session *const client = make_client_session_with_capture(&ctx);

    const int32_t sid = bootstrap_request(server, client, &wire);

    /* 40 KiB deterministic filler — catches boundary-truncation bugs
     * in the data_provider far better than zeros would. */
    static char body[40 * 1024];
    for (size_t i = 0; i < sizeof(body); i++) {
        body[i] = (char)('a' + (i % 26));
    }

    assert_int_equal(
        http2_session_submit_response(server, (uint32_t)sid, 200,
                                      NULL, 0, body, sizeof(body)),
        0);

    /* Run the full drain loop several times — DATA frames are
     * flow-controlled and nghttp2 won't emit more than the peer's
     * initial window size (64 KiB default for clients that haven't
     * bumped their own setting). We've sized body under that, so
     * one pass suffices. */
    drain_into_client(server, client);

    assert_int_equal(cap.status_code, 200);
    assert_int_equal((int)cap.body_len, (int)sizeof(body));
    assert_memory_equal(cap.body, body, sizeof(body));
    assert_true(cap.response_complete);

    nghttp2_session_del(client);
    http2_session_free(server);
}

/* -------------------------------------------------------------------------
 * Step 5b — Response trailers (gRPC status delivery)
 *
 * After a normal HEADERS + DATA response, submit a trailer frame
 * carrying grpc-status + grpc-message. Verify the client sees the
 * trailer HEADERS frame (HCAT_HEADERS), the initial response still
 * parses, and END_STREAM lands on the trailer rather than on DATA.
 * ------------------------------------------------------------------------- */

static void test_response_trailers(void **state)
{
    (void)state;

    dispatch_flag_t disp = {0};
    http2_session_t *const server = http2_session_new(NULL, set_dispatch_flag, &disp);
    wire_ring_t wire = {0};
    response_capture_t cap = {0};
    client_ctx_t ctx = { .wire = &wire, .cap = &cap };
    nghttp2_session *const client = make_client_session_with_capture(&ctx);

    const int32_t sid = bootstrap_request(server, client, &wire);

    const http2_header_view_t resp_hdrs[] = {
        { "content-type", 12, "application/grpc", 16 },
    };
    static const char body[] = "\x00\x00\x00\x00\x02OK";   /* gRPC length-prefixed message */
    assert_int_equal(
        http2_session_submit_response(server, (uint32_t)sid, 200,
                                      resp_hdrs, 1,
                                      body, sizeof(body) - 1),
        0);

    /* Terminal trailer frame — grpc-status: 0 (OK) + grpc-message. */
    const http2_header_view_t trailers[] = {
        { "grpc-status",  11, "0",       1 },
        { "grpc-message", 12, "success", 7 },
    };
    assert_int_equal(
        http2_session_submit_trailer(server, (uint32_t)sid, trailers, 2),
        0);

    drain_into_client(server, client);

    /* Initial :status + content-type still captured. */
    assert_int_equal(cap.status_code, 200);
    assert_string_equal(cap.content_type, "application/grpc");

    /* Trailer frame delivered, grpc-status + grpc-message captured,
     * END_STREAM landed on the trailer. */
    assert_true(cap.saw_trailers);
    assert_string_equal(cap.grpc_status, "0");
    assert_string_equal(cap.grpc_message, "success");
    assert_true(cap.response_complete);

    nghttp2_session_del(client);
    http2_session_free(server);
}

/* Submitting a trailer without a matching response must fail
 * cleanly — no crash, -1 returned, stream untouched. */
static void test_response_trailers_without_response(void **state)
{
    (void)state;

    dispatch_flag_t disp = {0};
    http2_session_t *const server = http2_session_new(NULL, set_dispatch_flag, &disp);

    const http2_header_view_t trailers[] = {
        { "grpc-status", 11, "0", 1 },
    };
    /* stream_id=1 doesn't exist on this session. */
    assert_int_equal(
        http2_session_submit_trailer(server, 1, trailers, 1),
        -1);

    http2_session_free(server);
}

/* -------------------------------------------------------------------------
 * Step 5a — Request body streaming IN
 *
 * Offline tests for on_data_chunk_recv_cb + body finalization. The
 * client-side nghttp2_session submits a full POST (HEADERS + DATA
 * frames), we feed the whole wire stream into our server, then
 * inspect stream->request->body to verify accumulation + completion
 * + size-cap semantics.
 * ------------------------------------------------------------------------- */

/* Client-side body view for test_body_read_cb. */
typedef struct {
    const char *buf;
    size_t      len;
    size_t      off;
} body_view_t;

/* nghttp2 DATA provider for the client side — reads from a
 * body_view_t and sets EOF on the final slice so the client emits
 * END_STREAM naturally. */
static ssize_t test_body_read_cb(nghttp2_session *session, int32_t stream_id,
                                 uint8_t *buf, size_t length,
                                 uint32_t *flags,
                                 nghttp2_data_source *source,
                                 void *user_data)
{
    (void)session; (void)stream_id; (void)user_data;
    body_view_t *const v = (body_view_t *)source->ptr;
    const size_t left    = v->len - v->off;
    const size_t to_copy = left < length ? left : length;
    if (to_copy > 0) {
        memcpy(buf, v->buf + v->off, to_copy);
        v->off += to_copy;
    }
    if (v->off >= v->len) {
        *flags |= NGHTTP2_DATA_FLAG_EOF;
    }
    return (ssize_t)to_copy;
}

/* Submit a POST with @p body of @p body_len bytes, drive the client
 * → server wire exchange in a drain loop so arbitrarily large bodies
 * don't blow wire_ring_t's fixed 16 KiB buffer. Caller owns @p src;
 * the provider references it via pointer.
 *
 * Returns the assigned stream id, or 0 if the client bailed (e.g.
 * flow-control window exhausted before body finished — acceptable
 * for the oversize-cap test which expects early termination). */
static int32_t submit_post_and_feed(http2_session_t *server,
                                    nghttp2_session *client,
                                    wire_ring_t *wire,
                                    body_view_t *src)
{
    const nghttp2_nv nv[] = {
        { (uint8_t *)":method",    (uint8_t *)"POST",    7, 4, 0 },
        { (uint8_t *)":path",      (uint8_t *)"/",       5, 1, 0 },
        { (uint8_t *)":scheme",    (uint8_t *)"https",   7, 5, 0 },
        { (uint8_t *)":authority", (uint8_t *)"h",      10, 1, 0 },
    };

    nghttp2_data_provider prv;
    prv.source.ptr    = src;
    prv.read_callback = test_body_read_cb;

    const int32_t sid = nghttp2_submit_request(client, NULL, nv, 4, &prv, NULL);
    assert_true(sid > 0);

    /* Shuttle bi-directionally until the exchange settles. Client→server
     * carries HEADERS + DATA + END_STREAM; server→client carries its
     * own SETTINGS + SETTINGS-ACK + WINDOW_UPDATE. Without the reverse
     * leg the client stays at the default 64 KiB outbound window and
     * stalls after one DATA frame. Safety bound caps iterations. */
    /* Bi-directional shuttle until the exchange quiesces. "Work
     * happened" = we actually moved bytes this iteration; merely
     * checking want_write doesn't count. Loop exits when client is
     * fully drained and server has no more outbound state. */
    uint8_t srv_out[16384];
    int iter = 0;
    while (iter++ < 8192) {
        const size_t wire_before = wire->len;
        if (nghttp2_session_want_write(client)) {
            (void)nghttp2_session_send(client);
        }
        const bool client_wrote = (wire->len != wire_before) || wire->len > 0;

        if (wire->len > 0) {
            push_to_server(server, wire);
        }

        ssize_t n = 0;
        if (http2_session_want_write(server)) {
            n = http2_session_drain(server, (char *)srv_out, sizeof(srv_out));
            if (n > 0) {
                assert_true(nghttp2_session_mem_recv(
                    client, srv_out, (size_t)n) == (ssize_t)n);
            }
        }

        if (!client_wrote && n <= 0) { break; }
    }

    return sid;
}

/* Single-frame POST body round-trip. Verifies that
 * on_data_chunk_recv_cb appends bytes onto the per-stream
 * request_body_buf accumulator and finalize_request_body produces a
 * byte-exact request->body on END_STREAM.
 *
 * Multi-DATA-frame splits (bodies exceeding MAX_FRAME_SIZE=16 KiB) are
 * exercised by tests/phpt/server/071-h2c-post.phpt against a real
 * curl client, which drives nghttp2's flow-control state machine
 * end-to-end. A purely offline multi-frame test is possible but
 * requires implementing the client-side deferred-data resume dance
 * that the real network event loop provides for free — overkill for
 * what's already covered. */
static void test_request_body(void **state)
{
    (void)state;

    dispatch_capture_t disp = {0};
    http2_session_t *const server = http2_session_new(
        NULL, capture_on_request_ready, &disp);
    wire_ring_t wire = {0};
    client_ctx_t ctx = { .wire = &wire, .cap = NULL };
    nghttp2_session *const client = make_client_session(&ctx);

    static char body[10 * 1024];    /* fits in one MAX_FRAME_SIZE DATA frame */
    for (size_t i = 0; i < sizeof(body); i++) {
        body[i] = (char)('0' + (i % 10));
    }
    body_view_t src = { body, sizeof(body), 0 };

    const int32_t sid = submit_post_and_feed(server, client, &wire, &src);
    assert_int_equal(disp.dispatch_count, 1);

    http2_stream_t *const stream = http2_session_find_stream(server, (uint32_t)sid);
    assert_non_null(stream);
    assert_non_null(stream->request);
    assert_true(stream->request->complete);
    assert_non_null(stream->request->body);
    assert_int_equal((int)ZSTR_LEN(stream->request->body), (int)sizeof(body));
    assert_memory_equal(ZSTR_VAL(stream->request->body), body, sizeof(body));

    nghttp2_session_del(client);
    http2_session_free(server);
}

/* Bodiless POST: HEADERS with END_STREAM, no DATA frames at all.
 * Verifies the HEADERS-END_STREAM branch of finalize_request_body
 * (request completes even though on_data_chunk_recv_cb never fired). */
static void test_request_bodyless(void **state)
{
    (void)state;

    dispatch_capture_t disp = {0};
    http2_session_t *const server = http2_session_new(
        NULL, capture_on_request_ready, &disp);
    wire_ring_t wire = {0};
    client_ctx_t ctx = { .wire = &wire, .cap = NULL };
    nghttp2_session *const client = make_client_session(&ctx);

    /* bootstrap_request submits a GET with no body — HEADERS
     * carries END_STREAM. */
    const int32_t sid = bootstrap_request(server, client, &wire);

    assert_int_equal(disp.dispatch_count, 1);
    http2_stream_t *const stream = http2_session_find_stream(server, (uint32_t)sid);
    assert_non_null(stream);
    assert_non_null(stream->request);
    assert_true(stream->request->complete);
    /* No DATA ever arrived → body stays NULL (getBody() returns ""). */
    assert_null(stream->request->body);

    nghttp2_session_del(client);
    http2_session_free(server);
}

/* Body above HTTP2_MAX_BODY_SIZE must be rejected stream-level.
 * on_data_chunk_recv_cb returns NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE
 * once the cap trips, and nghttp2 emits RST_STREAM; the session-level
 * feed should NOT see a connection-level error. The stream's request
 * never reaches complete=true. */
static void test_request_body_oversize(void **state)
{
    (void)state;

    /* Bump the client's INITIAL_WINDOW_SIZE so it can actually ship
     * HTTP2_MAX_BODY_SIZE + slack before our server RST_STREAMs it.
     * The default server-advertised window is 1 MiB (plan §4.5), and
     * we need to push a little past HTTP2_MAX_BODY_SIZE (10 MiB) to
     * trip the cap. nghttp2's client defers when the window drains,
     * but for an offline test we just feed all the prepared bytes in
     * one go, so a window expansion on submission isn't required —
     * push_to_server loops until consumed == 0. */
    dispatch_capture_t disp = {0};
    http2_session_t *const server = http2_session_new(
        NULL, capture_on_request_ready, &disp);
    wire_ring_t wire = {0};
    client_ctx_t ctx = { .wire = &wire, .cap = NULL };
    nghttp2_session *const client = make_client_session(&ctx);

    /* Allocate HTTP2_MAX_BODY_SIZE + 1 KiB — enough to definitively
     * cross the cap after the first DATA frame batch. */
    const size_t oversize_len = HTTP2_MAX_BODY_SIZE + 1024;
    char *const body = emalloc(oversize_len);
    memset(body, 'X', oversize_len);
    body_view_t src = { body, oversize_len, 0 };

    /* Feed until the server trips cap OR the flow-control window is
     * saturated. Either way: server must NOT mark the request complete. */
    (void)submit_post_and_feed(server, client, &wire, &src);

    http2_stream_t *const stream = http2_session_find_stream(server, 1);
    /* Stream may have been closed already (cb_on_stream_close removed
     * it from the table) — either way the request must not be
     * "complete" with a full body. */
    if (stream != NULL) {
        assert_false(stream->request->complete);
    }

    efree(body);
    nghttp2_session_del(client);
    http2_session_free(server);
}

/* POST with an empty body (Content-Length: 0 via HEADERS+END_STREAM,
 * no DATA frame). Exercises the HEADERS-END_STREAM branch of
 * finalize_request_body for a method that normally carries a body,
 * asserting no crash and the expected "complete, body NULL" state. */
static void test_request_body_empty_post(void **state)
{
    (void)state;

    dispatch_capture_t disp = {0};
    http2_session_t *const server = http2_session_new(
        NULL, capture_on_request_ready, &disp);
    wire_ring_t wire = {0};
    client_ctx_t ctx = { .wire = &wire, .cap = NULL };
    nghttp2_session *const client = make_client_session(&ctx);

    /* Submit POST with END_STREAM on HEADERS (no body). */
    const nghttp2_nv nv[] = {
        { (uint8_t *)":method",    (uint8_t *)"POST",    7, 4, 0 },
        { (uint8_t *)":path",      (uint8_t *)"/empty",  5, 6, 0 },
        { (uint8_t *)":scheme",    (uint8_t *)"https",   7, 5, 0 },
        { (uint8_t *)":authority", (uint8_t *)"h",      10, 1, 0 },
    };
    const int32_t sid = nghttp2_submit_request(client, NULL, nv, 4, NULL, NULL);
    assert_true(sid > 0);
    assert_int_equal(nghttp2_session_send(client), 0);

    push_to_server(server, &wire);

    http2_stream_t *const stream = http2_session_find_stream(server, (uint32_t)sid);
    assert_non_null(stream);
    assert_non_null(stream->request);
    assert_true(stream->request->complete);
    assert_null(stream->request->body);

    /* Dispatch still fired — handler would see an empty-body POST. */
    assert_int_equal(disp.dispatch_count, 1);

    nghttp2_session_del(client);
    http2_session_free(server);
}

/* Peer sends RST_STREAM mid-body: our on_stream_close_cb removes the
 * stream from the session table and the stream_free dtor drops the
 * partial request_body_buf without leaking. ASan run on this case is
 * the real lint; the assertion is just "no crash, table cleaned". */
static void test_request_peer_reset_mid_body(void **state)
{
    (void)state;

    dispatch_capture_t disp = {0};
    http2_session_t *const server = http2_session_new(
        NULL, capture_on_request_ready, &disp);
    wire_ring_t wire = {0};
    client_ctx_t ctx = { .wire = &wire, .cap = NULL };
    nghttp2_session *const client = make_client_session(&ctx);

    /* Submit POST with a data provider but no END_STREAM yet (the
     * body will be partial because the client will RST_STREAM below). */
    const nghttp2_nv nv[] = {
        { (uint8_t *)":method",    (uint8_t *)"POST",    7, 4, 0 },
        { (uint8_t *)":path",      (uint8_t *)"/x",      5, 2, 0 },
        { (uint8_t *)":scheme",    (uint8_t *)"https",   7, 5, 0 },
        { (uint8_t *)":authority", (uint8_t *)"h",      10, 1, 0 },
    };
    static char body[2048];
    memset(body, 'A', sizeof(body));
    body_view_t src = { body, sizeof(body), 0 };
    nghttp2_data_provider prv = { .source.ptr = &src,
                                  .read_callback = test_body_read_cb };

    const int32_t sid = nghttp2_submit_request(client, NULL, nv, 4, &prv, NULL);
    assert_true(sid > 0);

    /* Flush what fits in wire, push to server. */
    assert_int_equal(nghttp2_session_send(client), 0);
    push_to_server(server, &wire);

    /* Peer decides to abort. RST_STREAM removes the stream on the
     * server side via on_stream_close_cb. */
    assert_int_equal(nghttp2_submit_rst_stream(client, NGHTTP2_FLAG_NONE,
                                               sid, NGHTTP2_CANCEL), 0);
    assert_int_equal(nghttp2_session_send(client), 0);
    push_to_server(server, &wire);

    /* Stream must be gone from the table; no crash accessing session. */
    assert_null(http2_session_find_stream(server, (uint32_t)sid));
    /* Request was dispatched (HEADERS done) but body never finalised. */
    assert_int_equal(disp.dispatch_count, 1);

    nghttp2_session_del(client);
    http2_session_free(server);
}

/* -------------------------------------------------------------------------
 * Step 7.2 — graceful shutdown (GOAWAY) and PING RTT
 * ------------------------------------------------------------------------- */

/* Server terminates mid-session: one stream in flight, server calls
 * http2_session_terminate, drain emits GOAWAY carrying the in-flight
 * stream's id as last-stream-id. Peer uses that id to decide which
 * requests are safe to retry. */
static void test_h2_goaway_graceful(void **state)
{
    (void)state;

    dispatch_flag_t disp = {0};
    http2_session_t *const server = http2_session_new(
        NULL, set_dispatch_flag, &disp);
    assert_non_null(server);

    response_capture_t cap = {0};
    wire_ring_t wire = {0};
    client_ctx_t ctx = { .wire = &wire, .cap = &cap };
    nghttp2_session *const client = make_client_session_with_capture(&ctx);

    const int32_t sid = bootstrap_request(server, client, &wire);
    assert_int_equal((int)disp.stream_id, sid);

    /* Drain preface/SETTINGS/ACK back to the client so its state machine
     * matches reality (otherwise client_on_frame_recv skips GOAWAY). */
    drain_into_client(server, client);

    /* Server decides to graceful-shutdown. */
    assert_int_equal(http2_session_terminate(server, NGHTTP2_NO_ERROR), 0);
    drain_into_client(server, client);

    assert_true(cap.saw_goaway);
    assert_int_equal((int)cap.goaway_error_code, NGHTTP2_NO_ERROR);
    /* last-stream-id must include our in-flight stream — peer is told
     * it will be processed. */
    assert_true((int32_t)cap.goaway_last_stream_id >= sid);

    nghttp2_session_del(client);
    http2_session_free(server);
}

/* Server sends a PING with hrtime() payload; client auto-ACKs it;
 * server records RTT > 0. Validates the full embed/decode path used
 * by gRPC-style keepalive telemetry. */
static void test_h2_ping_rtt(void **state)
{
    (void)state;

    http2_session_t *const server = http2_session_new(NULL, NULL, NULL);
    assert_non_null(server);

    response_capture_t cap = {0};
    wire_ring_t wire = {0};
    client_ctx_t ctx = { .wire = &wire, .cap = &cap };
    nghttp2_session *const client = make_client_session_with_capture(&ctx);

    /* Exchange preface + SETTINGS so the PING is processed at
     * MUST-APPLY state, not queued behind handshake. */
    push_to_server(server, &wire);
    drain_into_client(server, client);

    /* Server-initiated PING. nghttp2 on the client side auto-ACKs it
     * during session_mem_recv and queues the ACK via send_cb. */
    assert_int_equal(http2_session_submit_ping(server), 0);
    drain_into_client(server, client);

    /* Flush client's pending ACK back to the server. */
    assert_int_equal(nghttp2_session_send(client), 0);
    push_to_server(server, &wire);

    /* Server's on_frame_recv decoded the ACK payload and stored RTT.
     * A non-zero value is proof the embedded hrtime() round-tripped
     * through the peer intact — a broken memcpy on either side would
     * yield a nonsense delta (underflow → 0 via our own guard). */
    const uint64_t rtt = http2_session_last_ping_rtt_ns(server);
    assert_true(rtt > 0);

    nghttp2_session_del(client);
    http2_session_free(server);
    (void)cap;
}

/* Peer-initiated PING must NOT update our RTT counter — we measure
 * only round-trips of *our own* pings. Regression guard for anyone
 * who drops the NGHTTP2_FLAG_ACK check in cb_on_frame_recv. */
static void test_h2_peer_ping_does_not_update_rtt(void **state)
{
    (void)state;

    http2_session_t *const server = http2_session_new(NULL, NULL, NULL);
    assert_non_null(server);

    wire_ring_t wire = {0};
    client_ctx_t ctx = { .wire = &wire, .cap = NULL };
    nghttp2_session *const client = make_client_session(&ctx);

    push_to_server(server, &wire);     /* preface + SETTINGS */
    drain_into_client(server, client);

    /* Client-initiated PING (no ACK flag). nghttp2 on the server
     * side auto-ACKs these via its callback path; our on_frame_recv
     * branch for PING+ACK must ignore them because we never stored a
     * matching send-timestamp. */
    uint8_t payload[8] = { 0xde, 0xad, 0xbe, 0xef, 0x01, 0x02, 0x03, 0x04 };
    assert_int_equal(nghttp2_submit_ping(client, NGHTTP2_FLAG_NONE, payload), 0);
    assert_int_equal(nghttp2_session_send(client), 0);
    push_to_server(server, &wire);

    assert_int_equal((int)http2_session_last_ping_rtt_ns(server), 0);

    nghttp2_session_del(client);
    http2_session_free(server);
}

/* GOAWAY does NOT abort in-flight streams — the whole point of
 * graceful shutdown. Sequence: dispatch a request, terminate(NO_ERROR),
 * THEN submit the response → client still receives full HEADERS+DATA
 * plus the GOAWAY frame. This is the invariant that makes CoDel →
 * GOAWAY (Step 8) safe: drain-don't-drop. */
static void test_h2_goaway_drains_inflight(void **state)
{
    (void)state;

    dispatch_flag_t disp = {0};
    http2_session_t *const server = http2_session_new(
        NULL, set_dispatch_flag, &disp);
    assert_non_null(server);

    response_capture_t cap = {0};
    wire_ring_t wire = {0};
    client_ctx_t ctx = { .wire = &wire, .cap = &cap };
    nghttp2_session *const client = make_client_session_with_capture(&ctx);

    const int32_t sid = bootstrap_request(server, client, &wire);
    assert_true(disp.dispatched);

    /* Submit the response FIRST, then terminate. Matches real handler
     * ordering: the handler finishes its work (commit_stream_response
     * queues HEADERS+DATA), and only afterwards does the server decide
     * it is shutting down and appends GOAWAY. Inverting the order is
     * still correct under nghttp2 semantics but exercises a less
     * common path; production timing is this one. */
    static const char body[] = "{\"ok\":true}";
    const http2_header_view_t resp_nv[] = {
        { "content-type", 12, "application/json", 16 },
    };
    assert_int_equal(
        http2_session_submit_response(server, (uint32_t)sid, 200,
                                      resp_nv, 1, body, sizeof(body) - 1),
        0);

    assert_int_equal(http2_session_terminate(server, NGHTTP2_NO_ERROR), 0);

    drain_into_client(server, client);

    /* Client must have seen BOTH: full response + GOAWAY. */
    assert_int_equal(cap.status_code, 200);
    assert_true(cap.response_complete);
    assert_int_equal((int)cap.body_len, (int)(sizeof(body) - 1));
    assert_memory_equal(cap.body, body, sizeof(body) - 1);
    assert_true(cap.saw_goaway);
    assert_true((int32_t)cap.goaway_last_stream_id >= sid);

    nghttp2_session_del(client);
    http2_session_free(server);
}

/* After GOAWAY, peer-initiated streams whose id > last-stream-id are
 * refused by nghttp2. Client submits a new request after terminate;
 * the server must not dispatch it. */
static void test_h2_goaway_rejects_new_streams(void **state)
{
    (void)state;

    dispatch_flag_t disp = {0};
    http2_session_t *const server = http2_session_new(
        NULL, set_dispatch_flag, &disp);
    assert_non_null(server);

    response_capture_t cap = {0};
    wire_ring_t wire = {0};
    client_ctx_t ctx = { .wire = &wire, .cap = &cap };
    nghttp2_session *const client = make_client_session_with_capture(&ctx);

    /* Preface + SETTINGS exchange — no request yet. */
    push_to_server(server, &wire);
    drain_into_client(server, client);

    /* Terminate BEFORE any stream exists — last-stream-id will be 0. */
    assert_int_equal(http2_session_terminate(server, NGHTTP2_NO_ERROR), 0);
    drain_into_client(server, client);
    assert_true(cap.saw_goaway);
    assert_int_equal((int)cap.goaway_last_stream_id, 0);

    /* Client now tries a new request. nghttp2 on the client side lets
     * the submit succeed locally but the server must not dispatch — we
     * assert zero dispatch calls happened. */
    const nghttp2_nv nv[] = {
        { (uint8_t *)":method",    (uint8_t *)"GET",     7, 3, 0 },
        { (uint8_t *)":path",      (uint8_t *)"/after",  5, 6, 0 },
        { (uint8_t *)":scheme",    (uint8_t *)"https",   7, 5, 0 },
        { (uint8_t *)":authority", (uint8_t *)"h",      10, 1, 0 },
    };
    (void)nghttp2_submit_request(client, NULL, nv, 4, NULL, NULL);
    (void)nghttp2_session_send(client);
    /* push_to_server may return -1 once server notices the new stream
     * after GOAWAY — accept either outcome, what matters is that the
     * dispatch callback was NOT invoked. */
    size_t offset = 0;
    while (offset < wire.len) {
        size_t consumed = 0;
        if (http2_session_feed(server, (const char *)wire.buf + offset,
                               wire.len - offset, &consumed) < 0) {
            break;
        }
        offset += consumed;
        if (consumed == 0) { break; }
    }

    assert_false(disp.dispatched);

    nghttp2_session_del(client);
    http2_session_free(server);
}

/* error_code on terminate round-trips verbatim to the peer. Sets the
 * stage for Step 8 (CoDel → GOAWAY(ENHANCE_YOUR_CALM)). */
static void test_h2_goaway_error_code_propagates(void **state)
{
    (void)state;

    http2_session_t *const server = http2_session_new(NULL, NULL, NULL);
    assert_non_null(server);

    response_capture_t cap = {0};
    wire_ring_t wire = {0};
    client_ctx_t ctx = { .wire = &wire, .cap = &cap };
    nghttp2_session *const client = make_client_session_with_capture(&ctx);

    push_to_server(server, &wire);
    drain_into_client(server, client);

    assert_int_equal(
        http2_session_terminate(server, NGHTTP2_ENHANCE_YOUR_CALM), 0);
    drain_into_client(server, client);

    assert_true(cap.saw_goaway);
    assert_int_equal((int)cap.goaway_error_code,
                     (int)NGHTTP2_ENHANCE_YOUR_CALM);

    nghttp2_session_del(client);
    http2_session_free(server);
}

/* NULL / no-session safety — matches the pattern used across the suite. */
static void test_h2_terminate_null_safe(void **state)
{
    (void)state;
    assert_int_equal(http2_session_terminate(NULL, 0), -1);
    assert_int_equal(http2_session_submit_ping(NULL), -1);
    assert_int_equal((int)http2_session_last_ping_rtt_ns(NULL), 0);
}

/* -------------------------------------------------------------------------
 * Suite harness
 * ------------------------------------------------------------------------- */

static int group_setup(void **state)
{
    (void)state;
    return php_test_runtime_init();
}

static int group_teardown(void **state)
{
    (void)state;
    php_test_runtime_shutdown();
    return 0;
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_session_lifecycle),
        cmocka_unit_test(test_settings_exchange),
        cmocka_unit_test(test_drain_resumes_across_calls),
        cmocka_unit_test(test_settings_flood_rejected),
        cmocka_unit_test(test_headers_decode),
        cmocka_unit_test(test_concurrent_streams),
        cmocka_unit_test(test_headers_oversize_rejected),
        cmocka_unit_test(test_response_small_body),
        cmocka_unit_test(test_response_no_body),
        cmocka_unit_test(test_response_multi_frame_body),
        cmocka_unit_test(test_request_body),
        cmocka_unit_test(test_request_bodyless),
        cmocka_unit_test(test_request_body_oversize),
        cmocka_unit_test(test_request_body_empty_post),
        cmocka_unit_test(test_request_peer_reset_mid_body),
        cmocka_unit_test(test_response_trailers),
        cmocka_unit_test(test_response_trailers_without_response),
        cmocka_unit_test(test_h2_goaway_graceful),
        cmocka_unit_test(test_h2_ping_rtt),
        cmocka_unit_test(test_h2_peer_ping_does_not_update_rtt),
        cmocka_unit_test(test_h2_goaway_drains_inflight),
        cmocka_unit_test(test_h2_goaway_rejects_new_streams),
        cmocka_unit_test(test_h2_goaway_error_code_propagates),
        cmocka_unit_test(test_h2_terminate_null_safe),
    };
    return cmocka_run_group_tests(tests, group_setup, group_teardown);
}
