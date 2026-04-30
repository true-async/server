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
#include "Zend/zend_async_API.h"
#include "Zend/zend_exceptions.h"

#include "websocket/ws_dispatch.h"
#include "websocket/ws_handshake.h"
#include "websocket/ws_session.h"
#include "websocket/websocket_strategy.h"
#include "websocket/php_websocket.h"

#include "core/http_connection.h"
#include "core/http_protocol_strategy.h"
#include "core/http_protocol_handlers.h"
#include "http1/http_parser.h"

#include <string.h>

extern zval *http_request_create_from_parsed(http_request_t *req);

/* Per-handler-coroutine context. Mirrors http1_request_ctx_t, minus
 * the response zval — WebSocket has no HTTP response after 101. */
typedef struct {
    http_connection_t *conn;
    zval               request_zv;
    zval               websocket_zv;
} ws_handler_ctx_t;

/* Pending-write context attached to conn->io->event as a callback.
 *
 * Why this exists: we are called from on_request_ready, which runs
 * inside the read-callback on the event loop — NOT in a coroutine.
 * The project's suspending I/O helper (async_io_req_await) requires
 * coroutine context; calling it here throws "coroutine cannot be
 * stopped from Scheduler context". The framework convention here is
 * non-suspending writes via ZEND_ASYNC_IO_WRITE plus a completion
 * callback attached to io->event — same pattern http_log uses for
 * its writer (see src/log/http_log.c writer_complete_cb).
 *
 * Lifecycle:
 *   - try_upgrade allocates this struct, fills it in, submits the
 *     write, and attaches itself to io->event. Returns true; never
 *     blocks.
 *   - When the write completes, libuv NOTIFYs every callback on
 *     io->event. Our handler filters by req identity (the same
 *     defensive check writer_complete_cb does).
 *   - On match: detach from io->event, free buffer, dispose req,
 *     and either install the WS strategy + spawn the handler
 *     coroutine (accept path) or just tear down (reject path).
 *   - struct itself is efree'd at the end of the completion handler.
 */
typedef struct {
    zend_async_event_callback_t base;
    http_connection_t          *conn;
    http_request_t             *req;          /* borrowed; owned by parser pool */
    zend_fcall_t               *handler;      /* borrowed; owned by server */
    zend_async_io_req_t        *active_req;   /* identity for filter match */
    char                       *buf;          /* owned heap copy of response bytes */
    bool                        accept;       /* true → post-101 install WS + spawn */
} ws_pending_write_t;

/* {{{ Forward decls */
static void ws_handler_coroutine_entry(void);
static void ws_handler_coroutine_dispose(zend_coroutine_t *coroutine);

static void ws_pending_write_complete_cb(zend_async_event_t *event,
                                         zend_async_event_callback_t *callback,
                                         void *result,
                                         zend_object *exception);
static void ws_pending_write_dispose(zend_async_event_callback_t *callback,
                                     zend_async_event_t *event);

static bool ws_submit_write(http_connection_t *conn,
                            http_request_t    *req,
                            zend_fcall_t      *handler,
                            char              *buf,        /* takes ownership */
                            size_t             len,
                            bool               accept);

static zend_string *build_error_response(int status, const char *extra_header);

static void ws_install_strategy(http_connection_t *conn);
static void ws_spawn_handler_coroutine(http_connection_t *conn,
                                       http_request_t *req,
                                       zend_fcall_t *handler);
/* }}} */

bool ws_dispatch_try_upgrade(http_connection_t *conn, http_request_t *req)
{
    /* Cheapest probe first — no Upgrade header → not our concern. */
    const ws_handshake_result_t v = ws_handshake_validate(req);
    if (v == WS_HANDSHAKE_NOT_AN_UPGRADE) {
        return false;
    }

    /* TLS WS (wss://) routes through tls_push_and_maybe_flush instead
     * of raw IO_WRITE; that path lands with the WSS commit. For now,
     * decline to handle WS upgrade attempts on TLS connections — the
     * H1 dispatch will return whatever the user's HTTP handler does
     * (typically 404), which is correct for a server that hasn't yet
     * advertised wss://. */
#ifdef HAVE_OPENSSL
    if (conn->tls != NULL) {
        return false;
    }
#endif

    HashTable *const handlers =
        http_server_get_protocol_handlers(conn->server);
    zend_fcall_t *const handler = handlers != NULL
        ? http_protocol_get_handler(handlers, HTTP_PROTOCOL_WEBSOCKET)
        : NULL;

    /* No WS handler registered → respond 426 so the client knows
     * upgrade is not supported. Also covers the case of a malformed
     * upgrade reaching a server with no WS handler at all. */
    if (handler == NULL) {
        zend_string *resp = build_error_response(426,
            "Sec-WebSocket-Version: 13\r\n");
        if (resp == NULL) {
            conn->keep_alive = false;
            return true;
        }
        char *buf = emalloc(ZSTR_LEN(resp));
        memcpy(buf, ZSTR_VAL(resp), ZSTR_LEN(resp));
        size_t len = ZSTR_LEN(resp);
        zend_string_release(resp);
        ws_submit_write(conn, req, NULL, buf, len, /*accept=*/false);
        return true;
    }

    /* Validation failure → matching error code. */
    if (v < 0) {
        int status = 400;
        const char *extra = NULL;
        switch (v) {
            case WS_HANDSHAKE_FORBIDDEN_METHOD:   status = 405; extra = "Allow: GET\r\n"; break;
            case WS_HANDSHAKE_UPGRADE_REQUIRED:   status = 426; extra = "Sec-WebSocket-Version: 13\r\n"; break;
            case WS_HANDSHAKE_BAD_REQUEST:
            default:                              status = 400; extra = NULL; break;
        }
        zend_string *resp = build_error_response(status, extra);
        if (resp == NULL) {
            conn->keep_alive = false;
            return true;
        }
        char *buf = emalloc(ZSTR_LEN(resp));
        memcpy(buf, ZSTR_VAL(resp), ZSTR_LEN(resp));
        size_t len = ZSTR_LEN(resp);
        zend_string_release(resp);
        ws_submit_write(conn, req, NULL, buf, len, /*accept=*/false);
        return true;
    }

    /* Accept path. ws_handshake_validate already verified the key
     * header is present and well-formed; pull the value. */
    zval *const key_zv = zend_hash_str_find(req->headers,
        "sec-websocket-key", sizeof("sec-websocket-key") - 1);
    if (UNEXPECTED(key_zv == NULL || Z_TYPE_P(key_zv) != IS_STRING)) {
        conn->keep_alive = false;
        return true;
    }

    char accept_value[WS_ACCEPT_LEN];
    if (ws_handshake_compute_accept(Z_STRVAL_P(key_zv), Z_STRLEN_P(key_zv),
                                    accept_value) != 0) {
        conn->keep_alive = false;
        return true;
    }

    /* Subprotocol selection lands with the WebSocketUpgrade-driven
     * commit; we do not echo Sec-WebSocket-Protocol here. */
    zend_string *resp = ws_handshake_build_101_response(accept_value, NULL);
    if (resp == NULL) {
        conn->keep_alive = false;
        return true;
    }
    char *buf = emalloc(ZSTR_LEN(resp));
    memcpy(buf, ZSTR_VAL(resp), ZSTR_LEN(resp));
    size_t len = ZSTR_LEN(resp);
    zend_string_release(resp);

    /* Submit the 101 write. The completion callback is what installs
     * the WS strategy and spawns the handler coroutine — doing it
     * BEFORE the 101 hits the wire would race the strategy swap
     * against any pipelined frames the client may already be
     * sending, and risks the user handler running before the client
     * has confirmation that the upgrade succeeded. */
    if (!ws_submit_write(conn, req, handler, buf, len, /*accept=*/true)) {
        conn->keep_alive = false;
        return true;
    }

    return true;
}

/* {{{ build_error_response — helper for 4xx/5xx; returns owned zend_string */
static zend_string *build_error_response(int status, const char *extra_header)
{
    const char *reason;
    switch (status) {
        case 400: reason = "Bad Request";        break;
        case 405: reason = "Method Not Allowed"; break;
        case 426: reason = "Upgrade Required";   break;
        default:  reason = "Bad Request";        break;
    }

    smart_str buf = {0};
    smart_str_alloc(&buf, 256, 0);

    char status_line[64];
    int n = snprintf(status_line, sizeof(status_line),
                     "HTTP/1.1 %d %s\r\n", status, reason);
    smart_str_appendl(&buf, status_line, (size_t)n);
    smart_str_appends(&buf, "Connection: close\r\n");
    smart_str_appends(&buf, "Content-Type: text/plain; charset=utf-8\r\n");
    if (extra_header != NULL) {
        smart_str_appends(&buf, extra_header);
    }

    char content_length[48];
    int rl = (int)strlen(reason);
    n = snprintf(content_length, sizeof(content_length),
                 "Content-Length: %d\r\n\r\n", rl);
    smart_str_appendl(&buf, content_length, (size_t)n);
    smart_str_appendl(&buf, reason, (size_t)rl);
    smart_str_0(&buf);

    return buf.s;
}
/* }}} */

/* {{{ ws_submit_write
 *
 * Build a ws_pending_write_t, kick off the non-blocking write, and
 * register the completion callback on conn->io->event. Takes
 * ownership of `buf` (always — even on failure, frees it).
 *
 * Returns false only if the request submit / callback attach fails;
 * in that case the caller must mark the connection as keep_alive=false
 * (no completion will fire, so no further bookkeeping happens).
 */
static bool ws_submit_write(http_connection_t *conn,
                            http_request_t    *req,
                            zend_fcall_t      *handler,
                            char              *buf,
                            size_t             len,
                            bool               accept)
{
    ws_pending_write_t *pw = (ws_pending_write_t *)
        ZEND_ASYNC_EVENT_CALLBACK_EX(ws_pending_write_complete_cb,
                                     sizeof(ws_pending_write_t));
    if (pw == NULL) {
        efree(buf);
        return false;
    }
    pw->base.dispose = ws_pending_write_dispose;
    pw->conn         = conn;
    pw->req          = req;
    pw->handler      = handler;
    pw->buf          = buf;
    pw->accept       = accept;
    pw->active_req   = ZEND_ASYNC_IO_WRITE(conn->io, buf, len);
    if (pw->active_req == NULL) {
        efree(pw);
        efree(buf);
        return false;
    }

    if (!conn->io->event.add_callback(&conn->io->event, &pw->base)) {
        /* libuv has the req queued — we cannot cancel it cleanly here.
         * Best we can do is leave the buffer alive until the reactor
         * drops the req on connection close. Mark the conn so the
         * read loop doesn't dispatch into stale state. */
        pw->active_req->dispose(pw->active_req);
        efree(pw);
        efree(buf);
        return false;
    }

    return true;
}
/* }}} */

/* {{{ ws_pending_write_complete_cb
 *
 * io->event NOTIFY broadcasts to every callback (read AND write
 * completions on the same handle), so we filter by req identity —
 * same defensive check writer_complete_cb does.
 */
static void ws_pending_write_complete_cb(
    zend_async_event_t *event,
    zend_async_event_callback_t *callback,
    void *result,
    zend_object *exception)
{
    (void)event;
    ws_pending_write_t *pw = (ws_pending_write_t *)callback;

    /* Not our completion — leave it for whoever it belongs to. */
    if (pw->active_req == NULL || result != pw->active_req) {
        return;
    }

    http_connection_t *conn = pw->conn;
    zend_async_io_req_t *req_io = pw->active_req;
    pw->active_req = NULL;

    /* One-shot — detach from io->event before doing any further work
     * so a re-entrant NOTIFY (e.g. caused by spawning a coroutine
     * that itself touches io) cannot land here twice. */
    (void)conn->io->event.del_callback(&conn->io->event, &pw->base);

    const bool wrote_ok = (exception == NULL && req_io->exception == NULL);
    if (req_io->exception != NULL) {
        OBJ_RELEASE(req_io->exception);
        req_io->exception = NULL;
    }
    req_io->dispose(req_io);

    /* Buffer freed unconditionally — libuv has either flushed it to
     * the kernel or the connection is going away; either way we own
     * it again at this point. */
    efree(pw->buf);
    pw->buf = NULL;

    const bool accept       = pw->accept;
    http_request_t *const req = pw->req;
    zend_fcall_t   *const hnd = pw->handler;

    efree(pw);

    if (!wrote_ok) {
        /* Write failed — connection is effectively dead. The next
         * read attempt will fail and trigger the natural teardown
         * path; we just clear keep_alive to be explicit. */
        conn->keep_alive = false;
        return;
    }

    if (accept) {
        /* 101 is on the wire. Swap strategies — any further bytes
         * arriving on this connection now feed into ws_session
         * via the WS strategy. Then spawn the user handler. */
        ws_install_strategy(conn);
        ws_spawn_handler_coroutine(conn, req, hnd);
    } else {
        /* Reject path — error response delivered, close. */
        conn->keep_alive = false;
    }
}
/* }}} */

static void ws_pending_write_dispose(
    zend_async_event_callback_t *callback, zend_async_event_t *event)
{
    /* Memory is freed inside the completion handler. Nothing to do
     * here. */
    (void)callback;
    (void)event;
}

/* {{{ ws_install_strategy
 *
 * Tear down the H1 strategy and install the WS one. After this call
 * conn->strategy->feed routes incoming bytes into ws_session_feed.
 */
static void ws_install_strategy(http_connection_t *conn)
{
    if (conn->strategy != NULL) {
        if (conn->strategy->cleanup) {
            conn->strategy->cleanup(conn);
        }
        http_protocol_strategy_destroy(conn->strategy);
        conn->strategy = NULL;
    }

    conn->strategy      = http_protocol_strategy_websocket_create();
    conn->protocol_type = HTTP_PROTOCOL_WEBSOCKET;
}
/* }}} */

/* {{{ ws_spawn_handler_coroutine */
static void ws_spawn_handler_coroutine(http_connection_t *conn,
                                       http_request_t *req,
                                       zend_fcall_t *handler)
{
    ws_handler_ctx_t *ctx = ecalloc(1, sizeof(*ctx));
    ctx->conn = conn;

    zval *req_obj = http_request_create_from_parsed(req);
    ZVAL_COPY_VALUE(&ctx->request_zv, req_obj);
    efree(req_obj);

    /* Session pointer is filled lazily by ws_strategy on first feed();
     * the WebSocket factory accepts NULL and the recv/send method
     * bodies (still throw-stubs at this scaffold stage) will resolve
     * the live session through the strategy when they land. */
    zend_object *ws_obj = websocket_object_create_for_session(NULL);
    ZVAL_OBJ(&ctx->websocket_zv, ws_obj);

    zend_coroutine_t *coroutine = ZEND_ASYNC_NEW_COROUTINE(conn->scope);
    if (coroutine == NULL) {
        zval_ptr_dtor(&ctx->request_zv);
        zval_ptr_dtor(&ctx->websocket_zv);
        efree(ctx);
        conn->keep_alive = false;
        return;
    }

    coroutine->internal_entry   = ws_handler_coroutine_entry;
    coroutine->extended_data    = ctx;
    coroutine->extended_dispose = ws_handler_coroutine_dispose;

    conn->handler = handler;

    /* Pin conn for the handler — same discipline as the H1/H2 paths. */
    conn->handler_refcount++;

    ZEND_ASYNC_ENQUEUE_COROUTINE(coroutine);
}
/* }}} */

/* {{{ ws_handler_coroutine_entry
 *
 * Coroutine body: invoke the user PHP handler with (WebSocket,
 * HttpRequest). Three-arg WebSocketUpgrade injection lands in a
 * follow-up commit — for now we always pass two arguments.
 */
static void ws_handler_coroutine_entry(void)
{
    const zend_coroutine_t *coroutine = ZEND_ASYNC_CURRENT_COROUTINE;
    ws_handler_ctx_t *ctx = (ws_handler_ctx_t *)coroutine->extended_data;
    http_connection_t *conn = ctx->conn;

    zval params[2], retval;
    ZVAL_COPY_VALUE(&params[0], &ctx->websocket_zv);
    ZVAL_COPY_VALUE(&params[1], &ctx->request_zv);
    ZVAL_UNDEF(&retval);

    call_user_function(NULL, NULL, &conn->handler->fci.function_name,
                       &retval, 2, params);

    zval_ptr_dtor(&retval);
}
/* }}} */

/* {{{ ws_handler_coroutine_dispose
 *
 * Mark the WebSocket closed, release PHP objects, decrement
 * handler_refcount. CLOSE-frame send + FIN land with the
 * close-handshake commit — for now we just flag keep_alive=false
 * and let the read-side natural teardown handle the rest.
 */
static void ws_handler_coroutine_dispose(zend_coroutine_t *coroutine)
{
    ws_handler_ctx_t *ctx = (ws_handler_ctx_t *)coroutine->extended_data;
    if (ctx == NULL) {
        return;
    }
    http_connection_t *conn = ctx->conn;
    coroutine->extended_data = NULL;

    if (Z_TYPE(ctx->websocket_zv) == IS_OBJECT) {
        websocket_object *w = websocket_from_obj(Z_OBJ(ctx->websocket_zv));
        w->closed  = true;
        w->session = NULL;
    }

    zval_ptr_dtor(&ctx->request_zv);
    zval_ptr_dtor(&ctx->websocket_zv);
    efree(ctx);

    conn->keep_alive = false;
    conn->current_request = NULL;

    ZEND_ASSERT(conn->handler_refcount > 0);
    conn->handler_refcount--;
}
/* }}} */
